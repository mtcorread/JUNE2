// Simulator core: run() loop, runOneDay() orchestration, infection seeding,
// fomite init, checkpoint trigger, console summaries. Other Simulator methods
// live in sibling simulator_*.cpp files (declared in simulation/simulator.h).
#include "simulation/simulator.h"

#include "activity/runtime_group_allocator.h"
#ifdef USE_MPI
#include "parallel/domain_manager.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "epidemiology/seeding/seed_shortfall.h"
#include "loaders/calendar_event_loader.h"
#include "loaders/catchment_rule_loader.h"
#include "utils/event_logging/event_writer.h"

namespace june {

namespace {

void printSeedAudit(const InfectionSeedConfig& seed_config) {
  std::cout << "\n=== [AUDIT] Infection-seed config (rank 0) ===" << std::endl;
  std::cout << "  base_cases_per_capita="
            << seed_config.global_params.base_cases_per_capita
            << "  default_strength="
            << seed_config.global_params.default_seed_strength << std::endl;
  std::cout << "  seeds (" << seed_config.seeds.size() << "):" << std::endl;
  // Helper: pretty-print one SelectionCriterion. Reused for both
  // global attribute_filters and per-target-group criteria, because
  // bulk-CSV exact/clustered seeds store their filters in
  // structured_config.target_groups[*].criteria, not attribute_filters.
  auto printCriterion = [](const SelectionCriterion& f,
                           const std::string& prefix) {
    std::cout << prefix << f.property_path << " " << f.operator_type << " ";
    if (std::holds_alternative<std::string>(f.value)) {
      std::cout << '"' << std::get<std::string>(f.value) << '"';
    } else if (std::holds_alternative<int>(f.value)) {
      std::cout << std::get<int>(f.value);
    } else if (std::holds_alternative<double>(f.value)) {
      std::cout << std::get<double>(f.value);
    } else if (std::holds_alternative<bool>(f.value)) {
      std::cout << (std::get<bool>(f.value) ? "true" : "false");
    } else {
      std::cout << "(complex)";
    }
    std::cout << std::endl;
  };

  for (const auto& s : seed_config.seeds) {
    size_t tg_crit_total = 0;
    for (const auto& g : s.structured_config.target_groups) {
      tg_crit_total += g.criteria.size();
    }
    std::cout << "    - " << s.name << "  type="
              << (s.type == InfectionSeedType::UNIFORM ? "uniform"
                                                       : "structured")
              << "  date=" << s.date_time
              << "  filters=" << s.attribute_filters.size()
              << "  target_group_criteria=" << tg_crit_total << std::endl;
    if (s.type == InfectionSeedType::UNIFORM) {
      std::cout << "      cases_per_capita="
                << s.uniform_config.cases_per_capita
                << "  seed_strength=" << s.seed_strength << std::endl;
    } else {
      std::cout << "      geo_level=" << s.structured_config.geo_level
                << "  units=" << s.structured_config.unit_cases.size()
                << "  target_groups="
                << s.structured_config.target_groups.size() << std::endl;
      for (const auto& uc : s.structured_config.unit_cases) {
        int total = 0;
        for (const auto& budget : uc.budgets) total += budget.cases;
        std::cout << "        unit=" << uc.unit_id << "  cases=" << total
                  << std::endl;
      }
    }
    for (const auto& f : s.attribute_filters) {
      printCriterion(f, "      filter: ");
    }
    for (size_t gi = 0; gi < s.structured_config.target_groups.size(); ++gi) {
      for (const auto& c : s.structured_config.target_groups[gi].criteria) {
        std::cout << "      target_group[" << gi << "]: ";
        printCriterion(c, "");
      }
    }
  }
  std::cout << "==========================================\n" << std::endl;
}

void printDiseaseAudit(const Disease& disease,
                       const std::string& disease_yaml_path) {
  auto fmtDist = [](const DistributionParams& d) {
    std::ostringstream os;
    os << d.type << "(";
    bool first = true;
    for (const auto& [k, v] : d.params) {
      if (!first) os << ", ";
      os << k << "=" << v;
      first = false;
    }
    os << ")";
    return os.str();
  };

  const auto& tp = disease.getTransmissionParams();
  std::cout << "\n=== [AUDIT] Disease config (rank 0) ===" << std::endl;
  std::cout << "  Disease name: " << disease.getName() << std::endl;
  std::cout << "  Disease YAML: " << disease_yaml_path << std::endl;
  const auto& tags = disease.getSymptomTags();
  std::cout << "  symptom_tags (" << tags.size() << "):";
  for (const auto& t : tags) std::cout << " " << t.name;
  std::cout << std::endl;

  const auto& trajs = disease.getTrajectories();
  std::cout << "  trajectories (" << trajs.size() << "):" << std::endl;
  for (const auto& t : trajs) {
    std::cout << "    - " << std::left << std::setw(14) << t.selection_key
              << " sev=" << t.severity
              << " inf_factor=" << t.infectiousness_factor
              << " stages=" << t.stages.size() << "  path:";
    for (size_t si = 0; si < t.stages.size(); ++si) {
      std::cout << (si ? "→" : " ") << t.stages[si].symptom_tag;
    }
    if (!t.stages.empty()) {
      std::cout << "  exposed_dist=" << fmtDist(t.stages[0].completion_time);
    }
    std::cout << std::endl;
  }

  std::cout << "  transmission:" << std::endl;
  std::cout << "    mode="
            << (tp.mode == InfectiousnessMode::TRAJECTORY_DRIVEN
                    ? "Trajectory-Driven"
                    : "Stage-Driven")
            << "   type=" << tp.type << std::endl;
  if (tp.mode == InfectiousnessMode::TRAJECTORY_DRIVEN) {
    std::cout << "    max_infectiousness=" << fmtDist(tp.max_infectiousness)
              << std::endl;
    std::cout << "    shape=" << fmtDist(tp.shape)
              << "  rate=" << fmtDist(tp.rate)
              << "  shift=" << fmtDist(tp.shift) << std::endl;
    // Predicted gamma peak for diagnostic only:
    // mode = (shape-1)/rate + shift, when shape > 1
    auto getLoc = [](const DistributionParams& d) -> double {
      auto it = d.params.find("loc");
      return it == d.params.end() ? 0.0 : it->second;
    };
    double sh = getLoc(tp.shape);
    double rt = getLoc(tp.rate);
    double sft = getLoc(tp.shift);
    if (sh > 1.0 && rt > 0.0) {
      std::cout << "    -> predicted peak day post-infection: "
                << ((sh - 1.0) / rt + sft) << std::endl;
    }
  }
  std::cout << "    modes (" << tp.modes.size() << "):";
  for (const auto& tmode : tp.modes) {
    std::cout << "  " << tmode.name << "="
              << tmode.mode_transmissibility_multiplier;
  }
  std::cout << std::endl;

  std::cout << "  immunity: level=" << tp.natural_immunity.level
            << "  waning_rate=" << tp.natural_immunity.waning_rate << std::endl;

  const auto& orates = disease.getOutcomeRates();
  std::cout << "  outcome_rates: " << orates.rows.size() << " rows loaded"
            << std::endl;
  auto dumpRow = [&](size_t i) {
    if (i >= orates.rows.size()) return;
    const auto& row = orates.rows[i];
    double total = 0.0;
    std::cout << "    row[" << i << "]:";
    for (const auto& [k, v] : row.probabilities) {
      std::cout << " " << k << "=" << v;
      total += v;
    }
    std::cout << "  sum=" << total << std::endl;
  };
  dumpRow(0);
  dumpRow(orates.rows.size() / 2);
  if (!orates.rows.empty()) dumpRow(orates.rows.size() - 1);
}

// Per-slot transmission + epidemiology summary print: MPI_Reduce the
// per-rank totals onto rank 0 and print one line of transmissions + one
// line of epi transitions/recoveries/deaths if any are non-zero.
// Env-gated per-day state hash. JUNE_DEBUG_DAY_HASH=1 turns it on; each rank
// XOR-hashes its local population state, ranks reduce via MPI_BXOR so the
// final hash is partition-independent, and rank 0 prints. Useful for
// confirming deterministic divergence between -np 1 and -np N runs.
void dumpDayHashIfEnabled(int day, int rank, const WorldState& world,
                          DomainManager* domain_mgr) {
  static const bool debug_hash_enabled = []() {
    const char* e = std::getenv("JUNE_DEBUG_DAY_HASH");
    return e && std::string(e) != "0";
  }();
  if (!debug_hash_enabled) return;

  auto mix64 = [](uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
  };
  auto dbits = [](double d) {
    uint64_t u = 0;
    std::memcpy(&u, &d, sizeof(u));
    return u;
  };

  uint64_t h_dead = 0;
  uint64_t h_inf = 0;
  uint64_t h_imm = 0;
  int n_alive = 0;
  int n_infected = 0;
  int n_immune = 0;
  int n_dead = 0;

  for (size_t i = 0; i < world.people.size(); ++i) {
    const auto& p = world.people[i];
    uint64_t base = mix64(static_cast<uint64_t>(p.id) + 0x9e3779b97f4a7c15ULL);

    if (p.is_dead) {
      n_dead++;
      h_dead ^= mix64(base ^ 0xDEAD5A10ULL ^ dbits(p.death_time));
    } else {
      n_alive++;
    }

    if (p.infection) {
      n_infected++;
      h_inf ^=
          mix64(base ^ 0x11FEC7EDULL ^ dbits(p.infection->getInfectionTime()));
    }

    if (p.immunity.natural_acquisition_time >= 0.0) {
      n_immune++;
      h_imm ^= mix64(base ^ 0x1EEEEDEDULL ^
                     dbits(p.immunity.natural_acquisition_time));
    }
  }

  uint64_t local_h[3] = {h_dead, h_inf, h_imm};
  uint64_t global_h[3] = {h_dead, h_inf, h_imm};
  int local_n[4] = {n_alive, n_dead, n_infected, n_immune};
  int global_n[4] = {n_alive, n_dead, n_infected, n_immune};
#ifdef USE_MPI
  if (domain_mgr) {
    MPI_Allreduce(local_h, global_h, 3, MPI_UINT64_T, MPI_BXOR, MPI_COMM_WORLD);
    MPI_Allreduce(local_n, global_n, 4, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  }
#else
  (void)domain_mgr;
#endif
  if (rank == 0) {
    std::ios_base::fmtflags f = std::cout.flags();
    std::cout << "[DayHash] day=" << day << " alive=" << global_n[0]
              << " dead=" << global_n[1] << " inf=" << global_n[2]
              << " imm=" << global_n[3] << std::hex << std::setfill('0')
              << " H_dead=" << std::setw(16) << global_h[0]
              << " H_inf=" << std::setw(16) << global_h[1]
              << " H_imm=" << std::setw(16) << global_h[2] << std::endl;
    std::cout.flags(f);
  }
}

// Rank-0 announcement of the active checkpoint cadence and any out-of-window
// date entries. Only fires when checkpointing is enabled; callers should
// gate on rank 0 themselves.
void announceCheckpointMode(const SimulationConfig::CheckpointConfig& cp,
                            const std::string& start_date,
                            const std::string& end_date) {
  if (!cp.enabled) return;
  if (cp.usesDates()) {
    if (cp.every_n_days.has_value()) {
      std::cout << "[checkpoint] on_dates is set; every_n_days ("
                << *cp.every_n_days << ") is IGNORED (dates take precedence)"
                << std::endl;
    }
    for (const auto& d : *cp.on_dates) {
      if (d < start_date || d > end_date) {
        std::cout << "[checkpoint] WARNING: on_dates entry '" << d
                  << "' is outside the simulation window [" << start_date
                  << ", " << end_date << "] and will never fire" << std::endl;
      }
    }
    std::cout << "[checkpoint] enabled: date-based cadence, "
              << cp.on_dates->size() << " date(s), output_dir='"
              << cp.output_dir << "', keep_last=" << cp.keep_last << std::endl;
  } else if (cp.every_n_days.has_value() && *cp.every_n_days > 0) {
    std::cout << "[checkpoint] enabled: every " << *cp.every_n_days
              << " day(s), output_dir='" << cp.output_dir
              << "', keep_last=" << cp.keep_last << std::endl;
  } else {
    std::cout << "[checkpoint] enabled but no cadence configured "
                 "(every_n_days and on_dates both absent) — no "
                 "checkpoints will be written"
              << std::endl;
  }
}

// One-shot rank-0 dump of the loaded disease + infection-seed configuration.
// Output is purely diagnostic (no MPI participation, no global state mutated)
// so it is safe to run after construction but before the simulator starts.
void printStartupAudit(const Disease& disease,
                       const std::string& disease_yaml_path,
                       const InfectionSeedConfig& seed_config) {
  auto saved_flags = std::cout.flags();
  auto saved_prec = std::cout.precision();
  std::cout.unsetf(std::ios::floatfield);
  std::cout << std::setprecision(6);

  printDiseaseAudit(disease, disease_yaml_path);
  printSeedAudit(seed_config);

  std::cout.flags(saved_flags);
  std::cout.precision(saved_prec);
}

}  // namespace

// =============================================================================
// Implementation
// =============================================================================

Simulator::Simulator(WorldState& world, Config& config,
                     DomainManager* domain_mgr,
                     const std::string& infection_seeds_file,
                     const std::string& output_filename)
    : world_(world),
      config_(config),
      domain_mgr_(domain_mgr),
      activity_manager_(world, config),
      runtime_group_allocator_(
          std::make_unique<RuntimeGroupAllocator>(world, config)),
      current_day_num_(0),
      current_simulation_time_(0.0) {
  // GlobalRNG is seeded in main.cpp before any components are created
  // This ensures Domain, ActivityManager, etc. can use RNG during construction

  // Checkpointing is incompatible with the compartmental-model plugin: the
  // external ODE plugin's internal state is opaque to the engine, so a
  // resume would silently diverge. Fail fast on all ranks.
  if (config_.simulation.checkpoint.enabled &&
      !config_.simulation.compartmental_model_sidecar.empty()) {
    throw std::runtime_error(
        "Checkpointing is not supported for compartmental-model scenarios "
        "(compartmental_model_sidecar is set). Disable 'checkpoint.enabled' "
        "or remove the sidecar.");
  }

  // Parse start date
  if (!config_.simulation.start_date.empty()) {
    try {
      current_date_ = parseDate(config_.simulation.start_date);
    } catch (...) {
      current_date_ = {0, 0, 0, 1, 0, 120};  // 2020-01-01
    }
  } else {
    current_date_ = {0, 0, 0, 1, 0, 120};  // 2020-01-01
  }

  // Calculate total simulation days
  if (!config_.simulation.end_date.empty()) {
    try {
      std::tm end_date = parseDate(config_.simulation.end_date);
      total_days_ = daysBetween(current_date_, end_date);
    } catch (...) {
      total_days_ = 365;
    }
  } else {
    total_days_ = 365;
  }

  // Load disease configuration
  try {
    disease_ = std::make_unique<Disease>(DiseaseLoader::loadFromYAML(
        config_.simulation.disease_file, config_.simulation.verbose));
  } catch (const std::exception& e) {
    std::cerr << "FATAL: Failed to load disease: " << e.what() << std::endl;
    throw;
  }

  world_.symptom_names = disease_->getSymptomNames();

  // Rebuild the default per-mode contact matrix lookup against the disease's
  // own mode list, so a missing default is caught here (loud, fatal) rather
  // than surfacing later as a runtime lookup failure. Independent of whether
  // contact_matrices.mode_names ended up empty.
  {
    std::vector<std::string> disease_mode_names;
    for (const auto& mode : disease_->getTransmissionParams().modes) {
      disease_mode_names.push_back(mode.name);
    }
    config_.contact_matrices.finalizeDefaultModeMatrices(world_,
                                                         disease_mode_names);
    // Reconcile ContactMatrixConfig's own mode order (derived from
    // contact_matrices.yaml) against the disease's mode order, so per-venue
    // matrix lookups are matched by name rather than by list position.
    config_.contact_matrices.finalizeDiseaseModeAlignment(disease_mode_names);
    // Resolve every (type, mode) the world can present, so transmission reads
    // a complete table instead of walking a fallback chain per lookup, and a
    // scenario leaning on the scenario-wide default says so here.
    config_.contact_matrices.finalizeResolvedMatrices(world_,
                                                      disease_mode_names);
  }

  // Initialize fomite state on venues before epidemiology
  initFomiteState();

  // Now that disease_ is loaded, initialize Epidemiology
  epidemiology_ =
      std::make_unique<Epidemiology>(world_, disease_.get(), &event_logger_);

  // Initialize infection seeder
  InfectionSeedConfig seed_config;
  try {
    seed_config = InfectionSeedConfigLoader::loadFromFile(infection_seeds_file);
  } catch (const std::exception& e) {
    std::cerr << "Warning: Could not load infection seeds: " << e.what()
              << std::endl;
    std::cerr << "Using empty infection seed configuration." << std::endl;
  }
  infection_seeder_ = std::make_unique<InfectionSeeder>(
      world_, disease_.get(), seed_config, &event_logger_,
      config_.simulation.random_seed);
#ifdef USE_MPI
  // A structured seed's count is global, so its candidates are pooled across
  // ranks before the winners are chosen.
  if (domain_mgr_) {
    infection_seeder_->setOfferExchange(&seed_offer_exchange_);
  }
#endif

  if (getRank() == 0) {
    printStartupAudit(*disease_, config_.simulation.disease_file, seed_config);
  }

  // Initialize interaction and transmission management
  interaction_manager_ = std::make_unique<InteractionManager>(
      world_, config_.contact_matrices, config_.simulation, config_.parallel,
      disease_.get(), &event_logger_);
  // Wire the runtime group allocator so processPartialPresenceVenue can
  // consult runtime group assignments. Allocator already constructed above.
  interaction_manager_->setRuntimeGroupAllocator(
      runtime_group_allocator_.get());

  // Initialize coordinated encounter manager
  const int rank = getRank();
  coordinated_encounter_manager_ =
      std::make_unique<CoordinatedEncounterManager>(world_, config_, rank);

  // Resolve infection seed selection criteria against the loaded world.
  infection_seeder_->resolveConfig(world_);

  // Initialize vaccination manager
  vaccination_manager_ =
      std::make_unique<VaccinationManager>(world_, config_, &event_logger_);

  // Initialize policy manager
  policy_manager_ = std::make_unique<PolicyManager>(world_);
  policy_manager_->setBaseSeed(config_.simulation.random_seed);
  try {
    PolicyLoader::loadPolicies(*policy_manager_,
                               config_.simulation.policies_file,
                               config_.simulation.start_date);
  } catch (const std::exception& e) {
    std::cerr << "Warning: Could not load policies: " << e.what() << std::endl;
    std::cerr << "Continuing without policies." << std::endl;
  }

  // Set disease in domain manager (if in parallel mode)
#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->setDisease(disease_.get());
  }
#endif

  // Initialize locations (everyone starts at residence)
  activity_manager_.initializeLocations(locations_);

  // Initialize incremental lookup tracking
  // No pre-allocation needed for sets

  // Assign schedule types to people based on selection criteria
  activity_manager_.assignScheduleTypes();

#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->exchangeScheduleTypes();
  }
#endif

  // Set policy manager in activity manager
  activity_manager_.setPolicyManager(policy_manager_.get());

  // Recovery and death end any policy freeze the person is under
  epidemiology_->setPolicyManager(policy_manager_.get());

  // Precompute which policies apply to each person (based on selection
  // criteria) This must be done AFTER schedules are assigned (person properties
  // are set)
  if (policy_manager_->getSymptomPolicyCount() > 0 ||
      policy_manager_->getTemporalPolicyCount() > 0) {
    policy_manager_->resolveAll(*disease_);
    policy_manager_->precomputePolicyApplicability(world_.people);
  }

  // Set policy manager in coordinated encounter manager

  // Pre-compute schedules
  activity_manager_.precomputeSchedules();

  // Load calendar events (optional — no-op if paths are empty)
  if (!config_.simulation.calendar_event_catchment_rules_file.empty()) {
    catchment_rules_ = CatchmentRuleLoader::load(
        config_.simulation.calendar_event_catchment_rules_file);
  }
  if (!config_.simulation.calendar_events_file.empty()) {
    auto events = CalendarEventLoader::load(
        config_.simulation.calendar_events_file, world_,
        config_.simulation.start_date, total_days_);
    calendar_event_manager_ = CalendarEventManager(std::move(events));
    activity_manager_.setCalendarEventManager(&calendar_event_manager_);
  }
  if (!config_.simulation.on_the_fly_venues_file.empty()) {
    on_the_fly_allocator_.emplace(config_.simulation.on_the_fly_venues_file);
    on_the_fly_allocator_->checkConsistency(world_);
    activity_manager_.setOnTheFlyVenueAllocator(&on_the_fly_allocator_.value());
    // Warm every pool the allocator can serve while the maps still exist.
    on_the_fly_allocator_->precomputeAllPools(
        world_, calendar_event_manager_.hostingGeoUnits());
  }
  // global_venue_geo_unit_map / global_venues_by_type_name exist only to build
  // OTF pools (now precomputed), so free them. The all-venue type_map used for
  // cross-rank type lookups stays.
  world_.dropGlobalVenueMaps();

  // Initialize events filename based on rank
#ifdef USE_MPI
  if (domain_mgr_) {
    std::filesystem::path p(output_filename);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    std::string parent = p.parent_path().string();
    if (!parent.empty()) parent += "/";
    events_filename_ =
        parent + stem + "_rank" + std::to_string(domain_mgr_->getRank()) + ext;
  } else {
    events_filename_ = output_filename;
  }
#else
  events_filename_ = output_filename;
#endif

  // Ensure we start with a fresh file for this new simulation run
  if (std::filesystem::exists(events_filename_)) {
    std::filesystem::remove(events_filename_);
  }

  compartmental_model_manager_ = std::make_unique<CompartmentalModelManager>(
      config_.simulation.compartmental_model_sidecar, domain_mgr_);

  MemoryUtils::logGlobalMemoryStats("Simulator Initialized");
}

void Simulator::run() {
  const int rank = getRank();

  if (rank == 0) {
    std::cout << "\n=== Starting Simulation ===" << std::endl;
    std::cout << std::string(50, '=') << std::endl;

    // Enable wall-clock profiling for high-level phases
    Profiler::instance().enable();

    // Checkpoint cadence: validate + announce the active mode once.
    announceCheckpointMode(config_.simulation.checkpoint,
                           config_.simulation.start_date,
                           config_.simulation.end_date);
  }

  if (resume_from_day_ > 0 && rank == 0) {
    std::cout << "[checkpoint] resuming simulation at day " << resume_from_day_
              << " of " << total_days_ << std::endl;
  }
  for (int day = resume_from_day_; day < total_days_; ++day) {
    runOneDay(day, rank);
  }

  writeFinalEventsAndLookups(rank);

  if (rank == 0) printRunSummary();
}

void Simulator::runOneDay(int day, int rank) {
  current_day_num_ = day;
  current_date_ = addDays(parseDate(config_.simulation.start_date), day);

  if (rank == 0) {
    std::cout << "\nDay " << day << " (" << formatDate(current_date_) << ")"
              << std::endl;
  }
  MemoryUtils::logGlobalMemoryStats("Start of Day " + std::to_string(day));

  // 0. Update simulation time for start of day
  current_simulation_time_ = static_cast<double>(day);

  // 1. Run vaccination manager updates at START of day
  // This ensures efficacy applies to today's interactions
  if (vaccination_manager_) {
    vaccination_manager_->update(current_simulation_time_);
  }

  // 2. Sync death flags across ranks so relationship dissolution can
  //    detect partners who died on a remote rank during the previous day.
#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->exchangeDeathFlags();
  }
#endif

  // 2.5. Trigger calendar events (schedule hops for fairs, etc.)
  calendar_event_manager_.triggerEventsForDay(day, world_, world_.people,
                                              config_.simulation.random_seed,
                                              catchment_rules_);

  // 3. Negotiate Coordinated Encounters
  negotiateAndLogDailyEncounters(day, rank);

  // Print daily encounter debug summary
  if (coordinated_encounter_manager_) {
    coordinated_encounter_manager_->printDailyEncounterSummary(day);
  }

  simulateDay(day);

  dumpDayHashIfEnabled(day, rank, world_, domain_mgr_);

  // Output statistics periodically
  if ((day + 1) % config_.simulation.stats_interval_days == 0) {
    outputStatistics();
  }

  // End-of-day flush check (triggers flush_interval_days)
  checkAndFlushEvents(true);

  maybeWriteCheckpoint(day, rank);
}

void Simulator::maybeWriteCheckpoint(int day, int rank) {
  // Checkpoint trigger placed after the end-of-day flush so disease
  // progression is done, events are flushed, and cross-rank buffers are
  // empty.
  if (!config_.simulation.checkpoint.triggersOnDay(day,
                                                   formatDate(current_date_))) {
    return;
  }
  if (rank == 0) {
    const auto& cp = config_.simulation.checkpoint;
    std::cout << "[checkpoint] TRIGGER at end of day " << day << " ("
              << formatDate(current_date_)
              << "), sim_time=" << current_simulation_time_
              << ", mode=" << (cp.usesDates() ? "on_dates" : "every_n_days")
              << std::endl;
  }
  ScopedTimer timer("06_Checkpoint");
  writeCheckpoint(day, formatDate(current_date_));
}

void Simulator::writeFinalEventsAndLookups(int rank) {
  if (rank == 0) {
    std::cout << "\n=== Simulation Complete ===" << std::endl;
    std::cout << "Saving final epidemic events with lookup tables to "
              << events_filename_ << "..." << std::endl;
  }

  // Final save: write lookups for anyone still not written (e.g. if flushes
  // were missed or people infected in last slots)
  std::unordered_set<PersonId> remaining_ids;
  if (config_.simulation.save_full_person_details == "infected_only") {
    for (PersonId pid : event_logger_.getInfectedPersonIds()) {
      if (lookups_written_.find(pid) == lookups_written_.end()) {
        remaining_ids.insert(pid);
        lookups_written_.insert(pid);
      }
    }
  }

  // saveToHDF5WithLookups handles appending if file exists (from previous
  // flushes)
  ScopedTimer timer("05_FinalHDF5Save");
  event_logger_.saveToHDF5WithLookups(
      events_filename_, world_, config_,
      remaining_ids.empty() &&
              config_.simulation.save_full_person_details == "infected_only"
          ? nullptr
          : &remaining_ids);
}

void Simulator::printRunSummary() {
  // Print wall-clock summary AFTER everything is done
  Profiler::instance().printDetailedResults();

  // Print optimization stats
  activity_manager_.getStats().print();
  if (interaction_manager_) {
    interaction_manager_->getStats().print();
  }

  // Print encounter stats
  event_logger_.printEncounterStats(config_.schedule.day_type_names,
                                    day_type_counts_);
}

void Simulator::simulateDay(int day_num) {
  int day_type_idx = config_.schedule.getDayTypeIndex(day_num);
  activity_manager_.setCurrentDay(day_num);

  // Track per-day-type occurrence counts
  if (day_type_idx >= static_cast<int>(day_type_counts_.size()))
    day_type_counts_.resize(day_type_idx + 1, 0);
  day_type_counts_[day_type_idx]++;

  // Set simulation time to start of this day (redundant if already set in
  // run(), but safe)
  current_simulation_time_ = static_cast<double>(day_num);

  // Get slot list for this day type from the first schedule type (all schedule
  // types share aligned boundaries)
  if (config_.schedule.schedule_types.empty()) {
    std::cerr << "ERROR: No schedule types defined!" << std::endl;
    return;
  }
  const auto* slots_ptr =
      config_.schedule.schedule_types[0].slots_by_day_type_idx.empty()
          ? nullptr
          : config_.schedule.schedule_types[0]
                .slots_by_day_type_idx[day_type_idx];
  if (!slots_ptr) {
    std::cerr << "ERROR: No slots defined for day type index " << day_type_idx
              << std::endl;
    return;
  }
  const auto& schedule = *slots_ptr;

  // Simulate each time slot
  for (size_t slot_idx = 0; slot_idx < schedule.size(); ++slot_idx) {
    const auto& slot = schedule[slot_idx];
    double delta_hours = calculateDuration(slot.start, slot.end);
    simulateTimeSlot(slot, slot_idx, day_type_idx, delta_hours);
    current_simulation_time_ += delta_hours / 24.0;

    // Granular memory check after each time slot
    std::string mem_label = "Day " + std::to_string(current_day_num_) +
                            " Slot " + std::to_string(slot_idx);
    MemoryUtils::logGlobalMemoryStats(mem_label);

    // Granular flush check (triggers max_event_buffer_size mid-day)
    checkAndFlushEvents(false);
  }
}

void Simulator::printSimulationState(const std::string& time_slot_name,
                                     double delta_hours) {
  if (getRank() == 0) {
    std::cout << "    [" << time_slot_name << "] duration = " << std::fixed
              << std::setprecision(2) << delta_hours << " hours" << std::endl;
  }
}

void Simulator::applyInfectionSeeds(const std::string& current_datetime) {
  std::vector<PersonId> newly_infected = infection_seeder_->seedInfections(
      current_datetime, current_simulation_time_);
  int local_count = static_cast<int>(newly_infected.size());
  int global_count = local_count;
#ifdef USE_MPI
  if (domain_mgr_) {
    MPI_Allreduce(&local_count, &global_count, 1, MPI_INT, MPI_SUM,
                  MPI_COMM_WORLD);
  }
#endif

  // Every rank derives the same shortfall records from the same pooled offers,
  // so rank 0 emits the block alone: one report per seeding step, not one per
  // rank, and no collective to reach it.
  if (getRank() == 0) {
    std::string shortfall_report =
        formatSeedShortfallReport(infection_seeder_->getSeedShortfalls());
    if (!shortfall_report.empty()) std::cout << shortfall_report << std::flush;
  }

  if (global_count > 0) {
    if (getRank() == 0) {
      std::cout << "    [INFECTION SEED] Seeded " << global_count
                << " infections" << std::endl;
    }

    // Add seeded infections to active tracking set locally
    for (PersonId pid : newly_infected) {
      epidemiology_->trackInfection(pid);
    }
  }
}

void Simulator::checkAndFlushEvents(bool is_day_end) {
  bool should_flush = false;

  // 1. Time-based trigger (days)
  if (is_day_end && config_.simulation.flush_interval_days > 0) {
    if ((current_day_num_ + 1) % config_.simulation.flush_interval_days == 0) {
      should_flush = true;
    }
  }

  // 2. Buffer-size based trigger (can happen anytime/mid-day)
  if (event_logger_.getTotalRecordCount() >=
      (size_t)config_.simulation.max_event_buffer_size) {
    should_flush = true;
  }

  if (should_flush && event_logger_.getTotalRecordCount() > 0) {
    // Identify people who need their lookup records written (not already
    // written)
    std::unordered_set<PersonId> newly_infected;
    if (config_.simulation.save_full_person_details == "infected_only") {
      for (PersonId pid : event_logger_.getInfectedPersonIds()) {
        if (lookups_written_.find(pid) == lookups_written_.end()) {
          newly_infected.insert(pid);
          lookups_written_.insert(pid);
        }
      }
    }

    {
      ScopedTimer timer("05_HDF5_Flushing");
      event_logger_.flush(
          events_filename_, config_, world_,
          newly_infected.empty() &&
                  config_.simulation.save_full_person_details == "infected_only"
              ? nullptr
              : &newly_infected);
    }
  }
}
void Simulator::initFomiteState() {
  if (!disease_) return;
  const auto& modes = disease_->getTransmissionParams().modes;
  int num_fomite_modes = 0;
  for (const auto& tmode : modes) {
    if (tmode.type == TransmissionModeType::Fomite) ++num_fomite_modes;
  }
  if (num_fomite_modes == 0) return;
  for (auto& venue : world_.venues) {
    venue.fomite_history.assign(num_fomite_modes, {});
  }
}

}  // namespace june
