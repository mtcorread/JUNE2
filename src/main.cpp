#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "loaders/config_loader.h"
#include "loaders/hdf5_loader.h"
#include "simulation/compartmental_model_manager.h"
#include "simulation/simulator.h"
#include "utils/event_logging/event_logger.h"
#include "utils/memory_utils.h"
#include "utils/profiler.h"
#include "utils/random.h"
#include "utils/run_dir.h"

#ifdef USE_MPI
#include <mpi.h>

#include "parallel/domain_manager.h"
#endif

// gperftools CPU profiler - optional
#ifdef USE_GPERFTOOLS
#include <gperftools/profiler.h>
#endif

using namespace june;

inline std::ostream& operator<<(std::ostream& os, Sex sex) {
  switch (sex) {
    case Sex::MALE:
      return os << "male";
    case Sex::FEMALE:
      return os << "female";
    default:
      return os << "unknown";
  }
}

void printExamplePeople(const WorldState& world, size_t count = 5) {
  std::cout << "\n=== Example People ===" << std::endl;

  for (size_t i = 0; i < std::min(count, world.people.size()); ++i) {
    const Person& p = world.people[i];
    std::cout << "Person " << p.id << ": age=" << p.age << ", sex=" << p.sex
              << ", geo_unit=" << p.geo_unit_id;

    // Show properties
    if (p.properties_count > 0) {
      std::cout << ", properties={";
      for (size_t k = 0; k < p.properties_count; ++k) {
        if (k > 0) std::cout << ", ";
        const std::string& key = world.person_property_names[k];
        std::cout << key << "=";
        auto prop_opt = world.getPersonProperty(p, key);
        if (prop_opt.has_value()) {
          std::visit(
              [](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                  std::cout << "null";
                } else if constexpr (std::is_same_v<T, std::vector<int32_t>> ||
                                     std::is_same_v<T,
                                                    std::vector<std::string>>) {
                  std::cout << "[" << arg.size() << " items]";
                } else {
                  std::cout << arg;
                }
              },
              *prop_opt);
        } else {
          std::cout << "null";
        }
      }
      std::cout << "}";
    }

    // Show activities
    auto metas = world.getActivityMetas(p);
    if (!metas.empty()) {
      std::cout << ", activities={";
      bool first = true;
      for (const auto& meta : metas) {
        int16_t act_idx = meta.activity_index;
        auto venues = world.getActivityVenues(meta);

        if (act_idx < 0 || act_idx >= (int16_t)world.activity_names.size())
          continue;

        if (!first) std::cout << ", ";
        first = false;
        std::cout << world.activity_names[act_idx] << ":" << venues.size();
      }
      std::cout << "}";
    }

    std::cout << std::endl;
  }
}

void printExampleVenues(const WorldState& world, size_t count = 5) {
  std::cout << "\n=== Example Venues ===" << std::endl;

  for (size_t i = 0; i < std::min(count, world.venues.size()); ++i) {
    const Venue& v = world.venues[i];
    std::string v_type = (v.type_id < world.venue_type_names.size())
                             ? world.venue_type_names[v.type_id]
                             : "unknown";
    std::cout << "Venue " << v.id << " (type=" << v_type << ")"
              << ", geo_unit=" << v.geo_unit_id
              << ", residence=" << (v.is_residence ? "yes" : "no")
              << ", subsets=" << v.subset_count;

    if (v.properties_count > 0) {
      std::cout << ", properties=" << v.properties_count;
    }

    std::cout << std::endl;
  }
}

void printVenueTypeSummary(const WorldState& world) {
  std::cout << "\n=== Venues by Type ===" << std::endl;

  // Count by type
  std::map<std::string, size_t> typeCounts;
  for (const auto& v : world.venues) {
    std::string v_type = (v.type_id < world.venue_type_names.size())
                             ? world.venue_type_names[v.type_id]
                             : "unknown";
    typeCounts[v_type]++;
  }

  for (const auto& [type, count] : typeCounts) {
    std::cout << "  " << std::setw(20) << std::left << type << ": " << count
              << std::endl;
  }
}

void printActivitySummary(const WorldState& world) {
  std::cout << "\n=== Activities ===" << std::endl;

  for (const auto& name : world.activity_names) {
    // Count how many people have this activity
    size_t count = 0;
    for (const auto& p : world.people) {
      if (!world.getActivityVenues(p, name).empty()) {
        count++;
      }
    }
    std::cout << "  " << std::setw(20) << std::left << name << ": " << count
              << " people" << std::endl;
  }
}

void printConfig(const Config& config) {
  std::cout << "\n=== Configuration ===" << std::endl;
  std::cout << "Simulation:" << std::endl;
  std::cout << "  Date range: " << config.simulation.start_date << " to "
            << config.simulation.end_date << std::endl;
  std::cout << "  Time steps: defined by schedule (event-driven)" << std::endl;
  std::cout << "  Stats interval: every "
            << config.simulation.stats_interval_days << " day(s)" << std::endl;

  std::cout << "\nSchedule:" << std::endl;
  if (!config.schedule.schedule_types.empty()) {
    std::cout << "  Schedule types: " << config.schedule.schedule_types.size()
              << std::endl;
    std::cout << "  Default: " << config.schedule.default_schedule_type
              << std::endl;
    for (const auto& stype : config.schedule.schedule_types) {
      std::cout << "    - " << stype.name << " (priority " << stype.priority;
      for (const auto& [dt_name, dt_slots] : stype.slots_by_day_type) {
        std::cout << ", " << dt_name << " slots: " << dt_slots.size();
      }
      std::cout << ")" << std::endl;
    }
  } else {
    std::cerr << "  ERROR: No schedule types defined!" << std::endl;
  }

  std::cout << "\nContact Matrices:" << std::endl;
  std::cout << "  Venue types configured: "
            << config.contact_matrices.matrices.size() << std::endl;
  std::cout << "  Betas: " << config.contact_matrices.betas.size() << std::endl;
  std::cout << "  Default beta: " << config.contact_matrices.default_beta
            << std::endl;

  // Show a few contact matrices
  int shown = 0;
  for (const auto& [venue_type, cm] : config.contact_matrices.matrices) {
    if (shown++ >= 3) break;
    std::cout << "    " << venue_type << ": ";
    if (cm.bins.empty()) {
      std::cout << "default" << std::endl;
    } else {
      std::cout << cm.bins.size() << " bins [";
      for (size_t i = 0; i < std::min(size_t(2), cm.bins.size()); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << cm.bins[i];
      }
      if (cm.bins.size() > 2) std::cout << ", ...";
      std::cout << "]" << std::endl;
    }
  }
}

int main(int argc, char* argv[]) {
#ifdef USE_MPI
  // Initialize MPI
  MPI_Init(&argc, &argv);

  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
#else
  int rank = 0;
  int size = 1;
#endif

  std::string filename;
  std::string sim_config_file;
  std::string infection_seeds_file =
      "";  // empty = use value from simulation.yaml
  bool infection_seeds_cli_override = false;
  std::string runs_dir = "runs";
  std::string run_id_override = "";
  std::string restart_from = "";  // checkpoint dir to resume from (P4)
  int days_override = -1;
  // Wide enough to hold the full unsigned 32-bit seed range while keeping -1
  // as the "not provided" sentinel. Auto-generated seeds routinely exceed
  // INT_MAX, and must be feedable back via --seed for reproducible restart.
  long long seed_override = -1;
  double beta_override = -1.0;

  std::vector<std::string> cli_args(argv + 1, argv + argc);

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--infection_seeds" && i + 1 < argc) {
      infection_seeds_file = argv[++i];
      infection_seeds_cli_override = true;
    } else if ((arg == "--sim_config" || arg == "--config") && i + 1 < argc) {
      sim_config_file = argv[++i];
    } else if (arg == "--beta" && i + 1 < argc) {
      try {
        beta_override = std::stod(argv[++i]);
      } catch (...) {
        if (rank == 0)
          std::cerr << "Warning: Invalid value for --beta: " << argv[i]
                    << std::endl;
      }
    } else if (arg == "--seed" && i + 1 < argc) {
      try {
        long long v = std::stoll(argv[++i]);
        if (v < 0 || v > 0xFFFFFFFFLL) {
          throw std::out_of_range("seed must be in [0, 4294967295]");
        }
        seed_override = v;
      } catch (...) {
        if (rank == 0)
          std::cerr << "Warning: Invalid value for --seed: " << argv[i]
                    << std::endl;
      }
    } else if (arg == "--runs-dir" && i + 1 < argc) {
      runs_dir = argv[++i];
    } else if (arg == "--run-id" && i + 1 < argc) {
      run_id_override = argv[++i];
    } else if (arg == "--restart-from" && i + 1 < argc) {
      restart_from = argv[++i];
    } else if (arg == "--days" && i + 1 < argc) {
      try {
        days_override = std::stoi(argv[++i]);
      } catch (...) {
        if (rank == 0)
          std::cerr << "Warning: Invalid value for --days: " << argv[i]
                    << std::endl;
      }
    } else if (arg == "--world" && i + 1 < argc) {
      filename = argv[++i];
    } else if (arg[0] != '-') {
      filename = arg;
    }
  }

  if (sim_config_file.empty() || filename.empty()) {
    if (rank == 0) {
      if (sim_config_file.empty())
        std::cerr << "Error: --config <path/to/simulation.yaml> is required."
                  << std::endl;
      if (filename.empty())
        std::cerr << "Error: --world <path/to/world.h5> is required."
                  << std::endl;
    }
#ifdef USE_MPI
    MPI_Finalize();
#endif
    return 1;
  }

  // Hoisted out of the try so the top-level H5::Exception handler can tell
  // whether a compartmental plugin (the only thing that dlopen's a second
  // libhdf5_cpp) was in play — see the catch below.
  std::string compartmental_sidecar_path;

  try {
    // Load configuration (all ranks load config)
    Config config = ConfigLoader::loadAll(sim_config_file);
    compartmental_sidecar_path = config.simulation.compartmental_model_sidecar;

    // Resolve infection_seeds_file: CLI arg takes priority, then
    // simulation.yaml, then hardcoded default
    if (!infection_seeds_cli_override) {
      infection_seeds_file = config.simulation.infection_seeds_file;
    }

    // Resolve run id (UTC timestamp by default; --run-id overrides). Rank 0
    // generates and broadcasts so every rank lands on the same path.
    std::string run_id = run_id_override;
    if (rank == 0 && run_id.empty()) {
      run_id = run_dir::generateRunIdUtc();
    }
    run_dir::broadcastRunId(run_id);
    std::filesystem::path run_path = std::filesystem::path(runs_dir) / run_id;
    std::string output_path = (run_path / "simulation_events.h5").string();

    // Checkpoint resume: the checkpoint's recorded effective seed is
    // authoritative. Adopt it (unless an explicit --seed was given, in which
    // case a mismatch is rejected later in restoreFromCheckpoint; no silent
    // override). Path is normalised here ('latest' symlink resolved).
    if (!restart_from.empty()) {
      std::filesystem::path cpdir = std::filesystem::canonical(restart_from);
      YAML::Node cman = YAML::LoadFile((cpdir / "manifest.yaml").string());
      unsigned int cseed = cman["effective_random_seed"].as<unsigned int>();
      if (seed_override < 0) {
        seed_override = static_cast<long long>(cseed);
        if (rank == 0)
          std::cout << "Resuming from checkpoint " << cpdir << " (seed "
                    << cseed << ")" << std::endl;
      }
      restart_from = cpdir.string();
    }

    // Resolve the effective RNG seed exactly once, before snapshotting, so it
    // is recorded for reproducible restart. Precedence: CLI --seed overrides
    // config; a zero/absent seed is auto-generated on rank 0 and broadcast so
    // every rank (and any future checkpoint resume) uses the identical stream.
    if (seed_override >= 0) {
      config.simulation.random_seed = static_cast<unsigned int>(seed_override);
      if (rank == 0)
        std::cout << "Overriding random seed: " << seed_override << std::endl;
    }
    unsigned int effective_seed = config.simulation.random_seed;
    const bool seed_autogenerated = (effective_seed == 0);
    if (seed_autogenerated && rank == 0) {
      effective_seed = std::random_device{}();
      if (effective_seed == 0) effective_seed = 1;  // 0 is the "unset" sentinel
    }
    run_dir::broadcastSeed(effective_seed);
    config.simulation.random_seed = effective_seed;

    // Snapshot every loaded YAML / referenced data file into the run dir
    // and write manifest.yaml (incl. lineage.effective_random_seed). Rank 0.
    if (rank == 0) {
      run_dir::snapshotRun(run_path, sim_config_file, config, size, cli_args,
                           effective_seed, filename);
    }
#ifdef USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    if (rank == 0) {
      std::cout << "Loading configuration..." << std::endl;
      std::cout << "  Config:  " << sim_config_file << std::endl;
      std::cout << "  Seeds:   " << infection_seeds_file << std::endl;
      std::cout << "  Run dir: " << run_path.string() << std::endl;
    }

    // Apply overrides
    if (beta_override >= 0.0) {
      config.contact_matrices.global_beta.enabled = true;
      config.contact_matrices.global_beta.value = beta_override;
      if (rank == 0)
        std::cout << "Overriding global beta: " << beta_override << std::endl;
    }
    // (random seed already resolved + recorded above, before snapshotRun)
    // Apply days override if provided
    if (days_override > 0) {
      std::tm start = parseDate(config.simulation.start_date);
      std::tm end = addDays(start, days_override);
      config.simulation.end_date = formatDate(end);
      if (rank == 0) {
        std::cout << "Overriding simulation duration: " << days_override
                  << " days (End date: " << config.simulation.end_date << ")"
                  << std::endl;
      }
    }

    if (rank == 0) {
      std::cout << "\nSimulation Parameters:" << std::endl;
      std::cout << "  Start: " << config.simulation.start_date
                << "  End: " << config.simulation.end_date
                << "  Seed: " << config.simulation.random_seed << std::endl;
      if (config.contact_matrices.global_beta.enabled) {
        std::cout << "  Global Beta: "
                  << config.contact_matrices.global_beta.value << std::endl;
      }
    }

    // Seed global RNG before creating any components
    // Domain, DomainManager, and other components may use RNG during
    // construction
    june::GlobalRNG::seed(effective_seed);
    if (seed_autogenerated && rank == 0) {
      std::cout << "No random seed configured; generated effective seed "
                << effective_seed
                << " (recorded in manifest.yaml lineage for reproducible "
                   "restart)"
                << std::endl;
    }

#ifdef USE_MPI
    // Check if parallel mode is enabled
    if (config.parallel.enabled && size > 1) {
      // PARALLEL MODE: Use domain decomposition with distributed memory
      if (rank == 0) {
        std::cout << "\nRunning in PARALLEL mode with " << size << " MPI ranks"
                  << std::endl;
      }

      // All ranks need empty world initially - will be populated during
      // initialize()
      WorldState world;

      // Rank 0 loads geography only (lightweight - just geo units, no
      // people/venues) DomainManager::initialize() will load person metadata
      // separately
      if (rank == 0) {
        world = HDF5Loader::loadGeographyOnly(filename);
      }

      MPI_Barrier(MPI_COMM_WORLD);

      // Create domain manager and partition world
      DomainManager domain_mgr(world, config);
      domain_mgr.setWorldStateFile(filename);
      domain_mgr.initialize();  // This will load domain-specific data
      domain_mgr.synchronizeRegistries();  // MPI determinism: align property
                                           // value indices
      config.resolve(world);

      // Get the local domain's world state
      Domain& domain = domain_mgr.getDomain();

      // Regional Risk: Load factors
      if (config.simulation.regional_risk.enabled) {
        if (rank == 0)
          std::cout << "Loading regional risk factors for parallel domains..."
                    << std::endl;
        domain.world->loadRegionalRiskFactors(
            config.simulation.regional_risk.regional_risk_file);
      }

      MPI_Barrier(MPI_COMM_WORLD);

      Simulator simulator(*domain.world, config, &domain_mgr,
                          infection_seeds_file, output_path);

      if (!restart_from.empty()) simulator.restoreFromCheckpoint(restart_from);

      // Start CPU profiling (like cProfile)
      // Start CPU profiling
#ifdef USE_GPERFTOOLS
      const char* profile_env = std::getenv("CPUPROFILE");
      if (profile_env) {
        if (rank == 0)
          std::cout << "\n[CPU profiling active (via CPUPROFILE env var): "
                    << profile_env << "]" << std::endl;
      } else {
        // By default, only profile Rank 0 to avoid massive IO and file
        // contention
        if (rank == 0) {
          std::string prof_name = "cpu_profile.prof";
#ifdef USE_MPI
          if (size > 1) prof_name = "cpu_profile_rank0.prof";
#endif
          std::string prof_file = (run_path / prof_name).string();
          std::cout << "\n[Starting CPU profiling for Rank 0 to " << prof_file
                    << "...]" << std::endl;
          ProfilerStart(prof_file.c_str());
        }
      }
#endif

      // Enable built-in profiler on rank 0
      if (rank == 0) {
        Profiler::instance().enable();
      }

      // Run simulation on this domain with cross-domain visitor exchange
      simulator.run();

      // Stop profiling and save results
      if (rank == 0) {
        Profiler::instance().disable();
        Profiler::instance().printByTotalTime(std::cout, 20);
#ifdef USE_GPERFTOOLS
        ProfilerStop();
        std::string prof_name = "cpu_profile.prof";
#ifdef USE_MPI
        if (size > 1) prof_name = "cpu_profile_rank0.prof";
#endif
        std::string prof_file = (run_path / prof_name).string();
        std::cout << "[Profiling stopped. Saved to " << prof_file << "]"
                  << std::endl;
#endif
      }

      MPI_Barrier(MPI_COMM_WORLD);

      // Merge event files from all ranks into a single file
      if (rank == 0) {
        std::cout << "\n[Parallel simulation complete!]" << std::endl;
        std::cout << "[Cross-domain visitor exchange enabled]" << std::endl;

        // Collect all rank event files
        std::vector<std::string> rank_files;
        const std::string& final_output = output_path;
        std::filesystem::path p(final_output);
        std::string stem = p.stem().string();
        std::string ext = p.extension().string();
        std::string parent = p.parent_path().string();
        if (!parent.empty()) parent += "/";

        for (int r = 0; r < size; ++r) {
          rank_files.push_back(parent + stem + "_rank" + std::to_string(r) +
                               ext);
        }

        // Merge into a single file
        EventLogger::mergeEventFiles(rank_files, final_output);
      }
    } else
#endif
    {
      // SERIAL MODE: Load full world
      if (rank == 0) {
        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "Loading world from: " << filename << std::endl;
        std::cout << std::string(50, '=') << std::endl;
      }

      WorldState world = HDF5Loader::load(filename, config);
      config.resolve(world);

      // Regional Risk: Load factors
      if (config.simulation.regional_risk.enabled) {
        std::cout << "Loading regional risk factors..." << std::endl;
        world.loadRegionalRiskFactors(
            config.simulation.regional_risk.regional_risk_file);
      }

      if (rank == 0) {
        std::cout << std::string(50, '=') << std::endl;
        world.printSummary();
        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "Running in SERIAL mode" << std::endl;
        std::cout << std::string(50, '=') << std::endl;

        std::cout << "Loading infection seeds from: " << infection_seeds_file
                  << std::endl;
        Simulator simulator(world, config, nullptr, infection_seeds_file,
                            output_path);

        if (!restart_from.empty()) {
          // Resume: state (incl. applied_seeds_) comes from the checkpoint.
          // Do NOT re-apply the start-of-sim seeds.
          simulator.restoreFromCheckpoint(restart_from);
        } else {
          // Apply configured infection seeds
          std::string start_dt = config.simulation.start_date + " 00:00";
          simulator.applyInfectionSeeds(start_dt);
        }

        // Start CPU profiling
#ifdef USE_GPERFTOOLS
        const char* profile_env = std::getenv("CPUPROFILE");
        if (profile_env) {
          std::cout << "\n[CPU profiling active (via CPUPROFILE env var): "
                    << profile_env << "]" << std::endl;
        } else {
          std::string prof_file = (run_path / "cpu_profile.prof").string();
          std::cout << "\n[Starting CPU profiling to " << prof_file << "...]"
                    << std::endl;
          ProfilerStart(prof_file.c_str());
        }
#endif

        simulator.run();
        MemoryUtils::logMemory("Post-Simulation-Serial");

        // Stop profiling
#ifdef USE_GPERFTOOLS
        ProfilerStop();
        std::cout << "\n[Profiling stopped. Saved to "
                  << (run_path / "cpu_profile.prof").string() << "]"
                  << std::endl;
#endif
      }
    }

  } catch (H5::Exception& e) {
    // Special-case the duplicate-libhdf5_cpp ABI clash: when a compartmental
    // plugin is dlopen'd against a different libhdf5_cpp than this binary, HDF5
    // initialises its global DataSpace::ALL constant twice and throws from
    // inside ld.so's call_init. That throw cannot be caught at the dlopen site
    // (a handler there is bypassed or escalates to std::terminate), but it does
    // unwind cleanly to here — so this is where we convert it into actionable
    // guidance. Gated on a configured sidecar so it fires iff a plugin (the
    // only second libhdf5_cpp) was actually loaded.
    if (!compartmental_sidecar_path.empty() &&
        june::isHdf5DuplicateConstantError(e.getFuncName(), e.getDetailMsg())) {
      if (rank == 0) {
        std::string plugin_path;
        try {
          plugin_path = june::CompartmentalModelManager::realLoadSidecar(
                            compartmental_sidecar_path)
                            .plugin_so_path;
        } catch (...) {
          // best-effort: fall back to the sidecar path in the message
          plugin_path = compartmental_sidecar_path;
        }
        std::cerr << june::formatHdf5AbiMismatchMessage(plugin_path,
                                                        e.getDetailMsg());
      }
    } else {
      std::cerr << "[Rank " << rank
                << "] FATAL: HDF5 error: " << e.getCDetailMsg() << std::endl;
    }
#ifdef USE_MPI
    MPI_Abort(MPI_COMM_WORLD, 1);
#endif
    return 1;
  } catch (std::exception& e) {
    // FATAL, not Error: this handler is unconditionally followed by MPI_Abort,
    // so everything it prints ends the run for every rank.
    std::cerr << "[Rank " << rank << "] FATAL: " << e.what() << std::endl;
#ifdef USE_MPI
    MPI_Abort(MPI_COMM_WORLD, 1);
#endif
    return 1;
  }

#ifdef USE_MPI
  MPI_Finalize();
#endif

  return 0;
}
