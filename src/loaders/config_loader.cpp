#include "loaders/config_loader.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "loaders/config_loader_detail.h"
#include "loaders/selection_criterion_value.h"
#include "utils/filtered_csv.h"
#include "utils/filtering.h"

namespace june {

namespace {

using ::june::config_detail::parseSelectionCriteria;

// Parse a YAML sequence of `{name, start, end, allowed_activities, ...}`
// entries into a vector of TimeSlot. Shared by the per-schedule-type loop
// in loadSchedule (both flat_slots and per-day-type slot lists).
std::vector<TimeSlot> parseSlotList(const YAML::Node& slot_list_node) {
  std::vector<TimeSlot> slots;
  for (const auto& slot : slot_list_node) {
    TimeSlot ts;
    ts.name = slot["name"].as<std::string>();
    ts.start = slot["start"].as<std::string>();
    ts.end = slot["end"].as<std::string>();
    ts.allowed_activities =
        slot["allowed_activities"].as<std::vector<std::string>>();

    if (slot["coordinated_only_activities"])
      ts.coordinated_only_activities =
          slot["coordinated_only_activities"].as<std::vector<std::string>>();

    if (slot["specified_activity"]) {
      SpecifiedActivity spec_act;
      spec_act.type = slot["specified_activity"]["type"].as<std::string>();
      spec_act.index = slot["specified_activity"]["index"].as<int>();
      if (slot["specified_activity"]["venue_type"]) {
        spec_act.venue_type =
            slot["specified_activity"]["venue_type"].as<std::string>();
      }
      ts.specified_activity = spec_act;
    }

    if (slot["hop_on_activity"]) {
      for (const auto& hop_kv : slot["hop_on_activity"]) {
        std::string act_name = hop_kv.first.as<std::string>();
        if (hop_kv.second.IsScalar()) {
          // Direct hop: activity_name → schedule_name
          ts.hop_on_activity[act_name] = hop_kv.second.as<std::string>();
        } else if (hop_kv.second.IsMap()) {
          // Property-dispatched hop: schedule name resolved at runtime
          TimeSlot::PropertyDispatchHop dispatch;
          dispatch.property_name = hop_kv.second["property"].as<std::string>();
          dispatch.schedule_name_template =
              hop_kv.second["schedule_template"].as<std::string>();
          ts.property_hop_dispatch[act_name] = std::move(dispatch);
        }
      }
    }

    slots.push_back(ts);
  }
  return slots;
}

// Read the `config_paths:` block (or legacy top-level keys when no
// `config_paths` section is present) onto the SimulationConfig file fields.
// Also throws on the removed `group_encounters_file` key.
void parseSimulationConfigPaths(const YAML::Node& paths,
                                SimulationConfig& config) {
  if (paths["disease_file"])
    config.disease_file = paths["disease_file"].as<std::string>();
  if (paths["contact_matrices_file"])
    config.contact_matrices_file =
        paths["contact_matrices_file"].as<std::string>();
  if (paths["schedules_file"])
    config.schedules_file = paths["schedules_file"].as<std::string>();
  if (paths["vaccines_file"])
    config.vaccines_file = paths["vaccines_file"].as<std::string>();
  if (paths["activity_preferences_file"])
    config.activity_preferences_file =
        paths["activity_preferences_file"].as<std::string>();
  if (paths["coordinated_encounters_file"])
    config.coordinated_encounters_file =
        paths["coordinated_encounters_file"].as<std::string>();
  if (paths["group_encounters_file"]) {
    throw std::runtime_error(
        "simulation.yaml: 'group_encounters_file' is no longer a top-level "
        "config_paths entry. Move it into coordinated_encounters.yaml as a "
        "`parameters_file` field on the encounter entry whose network is "
        "\"group_sex_roster\".");
  }
  if (paths["performance_file"])
    config.performance_file = paths["performance_file"].as<std::string>();
  if (paths["parallel_file"])
    config.parallel_file = paths["parallel_file"].as<std::string>();
  if (paths["policies_file"])
    config.policies_file = paths["policies_file"].as<std::string>();
  if (paths["infection_seeds_file"])
    config.infection_seeds_file =
        paths["infection_seeds_file"].as<std::string>();
  if (paths["compartmental_model_sidecar"])
    config.compartmental_model_sidecar =
        paths["compartmental_model_sidecar"].as<std::string>();
  if (paths["calendar_events_file"])
    config.calendar_events_file =
        paths["calendar_events_file"].as<std::string>();
  if (paths["calendar_event_catchment_rules_file"])
    config.calendar_event_catchment_rules_file =
        paths["calendar_event_catchment_rules_file"].as<std::string>();
  if (paths["on_the_fly_venues_file"])
    config.on_the_fly_venues_file =
        paths["on_the_fly_venues_file"].as<std::string>();
}

// Read the `output:` block onto the SimulationConfig output fields.
// `stats_interval_days` is required when the block is present; everything
// else is optional.
void parseSimulationOutput(const YAML::Node& output, SimulationConfig& config) {
  config.stats_interval_days = output["stats_interval_days"].as<int>();

  if (output["save_full_person_details"]) {
    config.save_full_person_details =
        output["save_full_person_details"].as<std::string>();
  }
  if (output["save_person_activities"]) {
    config.save_person_activities =
        output["save_person_activities"].as<std::string>();
  }
  if (output["compression_level"]) {
    config.compression_level = output["compression_level"].as<int>();
  }
  if (output["flush_interval_days"]) {
    config.flush_interval_days = output["flush_interval_days"].as<int>();
  }
  if (output["max_event_buffer_size"]) {
    config.max_event_buffer_size = output["max_event_buffer_size"].as<int>();
  }
  if (output["save_population_summary"]) {
    config.save_population_summary =
        output["save_population_summary"].as<bool>();
  }
  if (output["save_coordinated_encounters"]) {
    config.save_coordinated_encounters =
        output["save_coordinated_encounters"].as<bool>();
  }
  if (output["summary_properties"]) {
    config.summary_properties =
        output["summary_properties"].as<std::vector<std::string>>();
  }
}

// Read the `partial_presence.enabled_venue_types:` map onto the per-venue
// target_group_size table.
void parseSimulationPartialPresence(const YAML::Node& pp,
                                    SimulationConfig& config) {
  if (pp["enabled_venue_types"] && pp["enabled_venue_types"].IsMap()) {
    for (auto it = pp["enabled_venue_types"].begin();
         it != pp["enabled_venue_types"].end(); ++it) {
      std::string name = it->first.as<std::string>();
      int tgs = it->second["target_group_size"]
                    ? it->second["target_group_size"].as<int>()
                    : 0;
      config.partial_presence.target_group_size_by_name[name] = tgs;
    }
  }
}

// Parse a single contact-matrix YAML node into a ContactMatrix. Multiplies
// the `contacts` rows by the optional `beta` scaling factor at parse time
// (matching the historical behaviour of the inline lambda this replaced).
ContactMatrix parseContactMatrix(const YAML::Node& matrix_node) {
  ContactMatrix cm;
  if (matrix_node["bins"]) {
    cm.bins = matrix_node["bins"].as<std::vector<std::string>>();
  }
  if (matrix_node["characteristic_time"]) {
    cm.characteristic_time = matrix_node["characteristic_time"].as<double>();
  }
  double beta = 1.0;
  if (matrix_node["beta"]) {
    beta = matrix_node["beta"].as<double>();
  }
  if (matrix_node["contacts"]) {
    for (const auto& row : matrix_node["contacts"]) {
      std::vector<double> row_vec;
      for (const auto& val : row) row_vec.push_back(val.as<double>() * beta);
      cm.contacts.push_back(row_vec);
    }
  }
  if (matrix_node["proportion_physical"]) {
    for (const auto& row : matrix_node["proportion_physical"]) {
      std::vector<double> row_vec;
      for (const auto& val : row) row_vec.push_back(val.as<double>());
      cm.proportion_physical.push_back(row_vec);
    }
  }
  return cm;
}

// Read the top-level betas / global_beta / default_* scalars onto the
// ContactMatrixConfig. The per-matrix `contact_matrices:` block is handled
// separately by parseContactMatricesList.
void parseContactMatrixScalars(const YAML::Node& root,
                               ContactMatrixConfig& config) {
  if (root["betas"]) {
    for (const auto& beta_kv : root["betas"]) {
      std::string venue_type = beta_kv.first.as<std::string>();
      double beta = beta_kv.second.as<double>();
      config.betas[venue_type] = beta;
    }
  }

  if (root["global_beta"]) {
    if (root["global_beta"]["value"]) {
      config.global_beta.value = root["global_beta"]["value"].as<double>();
    }
    if (root["global_beta"]["enabled"]) {
      config.global_beta.enabled = root["global_beta"]["enabled"].as<bool>();
    }
  }

  if (root["default_beta"]) {
    config.default_beta = root["default_beta"].as<double>();
  }
  if (root["default_proportion_physical"]) {
    config.default_proportion_physical =
        root["default_proportion_physical"].as<double>();
  }
  if (root["alpha_physical"]) {
    config.alpha_physical = root["alpha_physical"].as<double>();
  }
  if (root["default_characteristic_time"]) {
    config.default_characteristic_time =
        root["default_characteristic_time"].as<double>();
  }
}

// Read the `contact_matrices:` block: for each venue type, dispatch on
// whether the entry declares `modes:` (multi-mode) or is a single contact
// matrix. Records mode names in YAML insertion order on config.mode_names.
void parseContactMatricesList(const YAML::Node& cm_node,
                              ContactMatrixConfig& config) {
  std::vector<std::string> seen_modes_ordered;
  auto addMode = [&seen_modes_ordered](const std::string& m) {
    for (const auto& s : seen_modes_ordered)
      if (s == m) return;
    seen_modes_ordered.push_back(m);
  };

  for (const auto& matrix_kv : cm_node) {
    std::string venue_type = matrix_kv.first.as<std::string>();
    const auto& matrix_node = matrix_kv.second;

    if (matrix_node["modes"]) {
      // Multi-mode: parse each mode entry. Nothing is copied into the flat
      // `matrices` map — a type that declares modes is described by those
      // modes, and mirroring an arbitrary one of them into a mode-free slot
      // is how a mode-specific matrix used to end up answering mode-free
      // questions.
      for (const auto& mode_kv : matrix_node["modes"]) {
        std::string mode_name = mode_kv.first.as<std::string>();
        addMode(mode_name);
        config.mode_matrices[venue_type][mode_name] =
            parseContactMatrix(mode_kv.second);
      }
    } else {
      // Single-mode fallback: store as "default" mode and in flat map
      addMode("default");
      ContactMatrix cm = parseContactMatrix(matrix_node);
      config.mode_matrices[venue_type]["default"] = cm;
      config.matrices[venue_type] = cm;
    }
  }

  if (!seen_modes_ordered.empty()) {
    config.mode_names = seen_modes_ordered;
  }
}

// Read the required `parallel.partitioning:` block. Throws if any of the
// three mandatory keys (level / centroids_file / adjacency_file) is missing,
// embedding the source `filename` in the error so the user knows which
// parallel.yaml triggered it. The optional metis.imbalance_tolerance is
// applied if present.
void parseParallelPartitioning(const YAML::Node& part,
                               const std::string& filename,
                               ParallelConfig& config) {
  if (!part["level"]) {
    throw std::runtime_error(
        "Parallel config '" + filename +
        "' is missing required key 'parallel.partitioning.level' "
        "(e.g. 'SGU', 'MGU', 'LGU').");
  }
  config.partition_level = part["level"].as<std::string>();

  if (!part["centroids_file"]) {
    throw std::runtime_error(
        "Parallel config '" + filename +
        "' is missing required key 'parallel.partitioning.centroids_file'.");
  }
  config.centroids_file = part["centroids_file"].as<std::string>();

  if (!part["adjacency_file"]) {
    throw std::runtime_error(
        "Parallel config '" + filename +
        "' is missing required key 'parallel.partitioning.adjacency_file'.");
  }
  config.adjacency_file = part["adjacency_file"].as<std::string>();

  if (part["metis"] && part["metis"]["imbalance_tolerance"]) {
    config.metis_imbalance_tolerance =
        part["metis"]["imbalance_tolerance"].as<double>();
  }
}

// Read the optional `parallel.output:` block (partition / load-balance /
// communication reporting flags).
void parseParallelOutput(const YAML::Node& output, ParallelConfig& config) {
  if (output["save_partition"]) {
    config.save_partition = output["save_partition"].as<bool>();
  }
  if (output["partition_file"]) {
    config.partition_file = output["partition_file"].as<std::string>();
  }
  if (output["report_load_balance"]) {
    config.report_load_balance = output["report_load_balance"].as<bool>();
  }
  if (output["report_communication"]) {
    config.report_communication = output["report_communication"].as<bool>();
  }
  if (output["report_interval_days"]) {
    config.report_interval_days = output["report_interval_days"].as<int>();
  }
}

// Read the `checkpoint:` block. Cadence is mutually exclusive: on_dates
// (non-null, non-empty) takes precedence over every_n_days. A null YAML
// value leaves the corresponding optional empty.
void parseSimulationCheckpoint(const YAML::Node& cp, SimulationConfig& config) {
  if (cp["enabled"]) {
    config.checkpoint.enabled = cp["enabled"].as<bool>();
  }
  if (cp["output_dir"]) {
    config.checkpoint.output_dir = cp["output_dir"].as<std::string>();
  }
  if (cp["every_n_days"] && !cp["every_n_days"].IsNull()) {
    config.checkpoint.every_n_days = cp["every_n_days"].as<int>();
  }
  if (cp["on_dates"] && !cp["on_dates"].IsNull()) {
    config.checkpoint.on_dates = cp["on_dates"].as<std::vector<std::string>>();
  }
  if (cp["keep_last"]) {
    config.checkpoint.keep_last = cp["keep_last"].as<int>();
  }
}

// Parse one entry from `schedule_types:` (a named schedule). Reads the
// flag/scalar fields, the optional `selection` block (via
// parseSelectionCriteria), the flat_slots / per-day-type slot lists (via
// parseSlotList), the per-schedule activity overrides, and the
// participation-rate table. `day_type_names` lists the day-type keys to
// look up on the YAML node.
ScheduleType parseScheduleType(const std::string& name,
                               const YAML::Node& type_node,
                               const std::vector<std::string>& day_type_names) {
  ScheduleType sched_type;
  sched_type.name = name;

  if (type_node["priority"]) {
    sched_type.priority = type_node["priority"].as<int>();
  }

  if (type_node["is_temporary"]) {
    sched_type.is_temporary = type_node["is_temporary"].as<bool>();
  }
  if (type_node["return_schedule"]) {
    sched_type.return_schedule = type_node["return_schedule"].as<std::string>();
  }
  if (type_node["flat_slots"]) {
    sched_type.flat_slots = parseSlotList(type_node["flat_slots"]);
  }

  if (type_node["selection"]) {
    parseSelectionCriteria(type_node["selection"],
                           sched_type.selection_criteria);
  }

  for (const auto& dt_name : day_type_names) {
    if (type_node[dt_name]) {
      sched_type.slots_by_day_type[dt_name] = parseSlotList(type_node[dt_name]);
    }
  }

  if (type_node["force_hybrid_activities"]) {
    sched_type.force_hybrid_activities =
        type_node["force_hybrid_activities"].as<std::vector<std::string>>();
  }

  if (type_node["linked_activities"]) {
    sched_type.linked_activities =
        type_node["linked_activities"].as<std::vector<std::string>>();
  }

  if (type_node["participation"]) {
    for (const auto& kv : type_node["participation"]) {
      std::string activity = kv.first.as<std::string>();
      for (const auto& dt_name : day_type_names) {
        if (kv.second[dt_name]) {
          sched_type.participation_by_day_type[dt_name][activity] =
              kv.second[dt_name].as<double>();
        }
      }
    }
  }

  return sched_type;
}

}  // namespace

namespace config_detail {

// Parse a YAML sequence of `{property, operator, value}` entries into a vector
// of SelectionCriterion. The scalar `value` is dispatched int -> double ->
// string; a sequence `value` goes through parseCriterionSequenceValue.
// Declared in loaders/config_loader_detail.h so the vaccination-loader TU can
// reuse it.
void parseSelectionCriteria(const YAML::Node& selection_node,
                            std::vector<SelectionCriterion>& out) {
  for (const auto& criterion_node : selection_node) {
    SelectionCriterion criterion;
    criterion.property_path = criterion_node["property"].as<std::string>();
    criterion.operator_type = criterion_node["operator"].as<std::string>();

    const auto& value_node = criterion_node["value"];
    if (value_node.IsSequence()) {
      criterion.value = config_detail::parseCriterionSequenceValue(
          value_node, criterion.property_path);
    } else if (value_node.IsScalar()) {
      try {
        criterion.value = value_node.as<int>();
      } catch (...) {
        try {
          criterion.value = value_node.as<double>();
        } catch (...) {
          criterion.value = value_node.as<std::string>();
        }
      }
    }
    out.push_back(std::move(criterion));
  }
}

}  // namespace config_detail

Config ConfigLoader::loadAll(const std::string& simulation_file) {
  Config config;

  // 1. Load the master simulation config first
  config.simulation = loadSimulation(simulation_file);

  // 2. Load all other configs using paths specified in the simulation config
  config.schedule = loadSchedule(config.simulation.schedules_file);
  config.contact_matrices =
      loadContactMatrices(config.simulation.contact_matrices_file);
  config.performance = loadPerformance(config.simulation.performance_file);
  config.parallel = loadParallel(config.simulation.parallel_file);
  config.activity_preferences =
      loadActivityPreferences(config.simulation.activity_preferences_file);
  config.vaccination = loadVaccination(config.simulation.vaccines_file);
  config.coordinated_encounters =
      loadCoordinatedEncounters(config.simulation.coordinated_encounters_file);

  return config;
}

SimulationConfig ConfigLoader::loadSimulation(const std::string& filename) {
  YAML::Node root = YAML::LoadFile(filename);
  SimulationConfig config;

  if (root["time"]) {
    auto time = root["time"];
    config.start_date = time["start_date"].as<std::string>();
    config.end_date = time["end_date"].as<std::string>();
  }

  // File links: support both nested `config_paths:` block and legacy
  // top-level keys.
  parseSimulationConfigPaths(root["config_paths"] ? root["config_paths"] : root,
                             config);

  if (root["random_seed"]) {
    config.random_seed = root["random_seed"].as<unsigned int>();
  }
  if (root["verbose"]) {
    config.verbose = root["verbose"].as<bool>();
  }

  if (root["output"]) {
    parseSimulationOutput(root["output"], config);
  }

  if (root["regional_risk"]) {
    auto regional_risk = root["regional_risk"];
    if (regional_risk["enabled"]) {
      config.regional_risk.enabled = regional_risk["enabled"].as<bool>();
    }
    if (regional_risk["regional_risk_file"]) {
      config.regional_risk.regional_risk_file =
          regional_risk["regional_risk_file"].as<std::string>();
    }
  }

  if (root["partial_presence"]) {
    parseSimulationPartialPresence(root["partial_presence"], config);
  }

  if (root["checkpoint"]) {
    parseSimulationCheckpoint(root["checkpoint"], config);
  }

  return config;
}

ScheduleConfig ConfigLoader::loadSchedule(const std::string& filename) {
  YAML::Node root = YAML::LoadFile(filename);
  ScheduleConfig config;

  // Day type cycle
  if (root["day_type_cycle"]) {
    config.day_type_cycle =
        root["day_type_cycle"].as<std::vector<std::string>>();
  }

  // Derive ordered unique day type names from cycle
  for (const auto& dt : config.day_type_cycle) {
    if (std::find(config.day_type_names.begin(), config.day_type_names.end(),
                  dt) == config.day_type_names.end()) {
      config.day_type_names.push_back(dt);
    }
  }

  // Schedule types
  if (root["schedule_types"]) {
    // Optional CSV for probabilistic schedule assignment
    if (root["schedule_csv"]) {
      config.csv_path = root["schedule_csv"].as<std::string>();
    }

    // Load default schedule type
    if (root["default_schedule_type"]) {
      config.default_schedule_type =
          root["default_schedule_type"].as<std::string>();
    }

    // Parse each schedule type
    for (const auto& type_kv : root["schedule_types"]) {
      config.schedule_types.push_back(
          parseScheduleType(type_kv.first.as<std::string>(), type_kv.second,
                            config.day_type_names));
    }

    // Sort schedule types by priority (highest first)
    std::sort(config.schedule_types.begin(), config.schedule_types.end(),
              [](const ScheduleType& a, const ScheduleType& b) {
                return a.priority > b.priority;
              });
  }

  return config;
}

ContactMatrixConfig ConfigLoader::loadContactMatrices(
    const std::string& filename) {
  YAML::Node root = YAML::LoadFile(filename);
  ContactMatrixConfig config;

  parseContactMatrixScalars(root, config);

  if (root["contact_matrices"]) {
    parseContactMatricesList(root["contact_matrices"], config);
  }

  if (root["allow_default_matrix"]) {
    config.allow_default_matrix = root["allow_default_matrix"].as<bool>();
  }

  if (!root["default_contacts_matrix"]) {
    throw std::runtime_error(
        "Contact matrix config '" + filename +
        "' is missing required key 'default_contacts_matrix'.");
  }
  const auto& default_node = root["default_contacts_matrix"];

  if (default_node["modes"]) {
    std::unordered_map<std::string, ContactMatrix> mode_matrices;
    for (const auto& mode_kv : default_node["modes"]) {
      mode_matrices[mode_kv.first.as<std::string>()] =
          parseContactMatrix(mode_kv.second);
    }

    std::vector<std::string> missing_modes;
    for (const auto& mode_name : config.mode_names) {
      if (!mode_matrices.count(mode_name)) missing_modes.push_back(mode_name);
    }
    if (!missing_modes.empty()) {
      std::string missing_list;
      for (size_t i = 0; i < missing_modes.size(); ++i) {
        if (i) missing_list += ", ";
        missing_list += missing_modes[i];
      }
      throw std::runtime_error(
          "Contact matrix config '" + filename +
          "': 'default_contacts_matrix' is missing mode(s) [" + missing_list +
          "] present in 'contact_matrices'.");
    }

    config.default_mode_matrices = std::move(mode_matrices);
  } else {
    config.default_matrix = parseContactMatrix(default_node);
  }

  return config;
}

PerformanceConfig ConfigLoader::loadPerformance(const std::string& filename) {
  PerformanceConfig config;

  // Try to load file, use defaults if it doesn't exist
  try {
    YAML::Node root = YAML::LoadFile(filename);

    if (root["performance"]) {
      const auto& perf = root["performance"];

      if (perf["precompute_schedules"]) {
        config.precompute_schedules = perf["precompute_schedules"].as<bool>();
      }

      if (perf["deterministic_activities"]) {
        config.deterministic_activities =
            perf["deterministic_activities"].as<std::vector<std::string>>();
      }

      if (perf["hybrid_activities"]) {
        config.hybrid_activities =
            perf["hybrid_activities"].as<std::vector<std::string>>();
      }

      if (perf["stochastic_activities"]) {
        config.stochastic_activities =
            perf["stochastic_activities"].as<std::vector<std::string>>();
      }

      if (perf["track_active_infections_only"]) {
        config.track_active_infections_only =
            perf["track_active_infections_only"].as<bool>();
      }
    }
  } catch (const std::exception& e) {
    // File doesn't exist or parse error - use defaults
    std::cerr << "Warning: Could not load " << filename << ": " << e.what()
              << std::endl;
    std::cerr << "Using default performance settings (maximum performance mode)"
              << std::endl;

    // Defaults: maximum performance (all activities deterministic)
    config.precompute_schedules = true;
    config.deterministic_activities.clear();  // Empty = all deterministic
    config.stochastic_activities.clear();
    config.track_active_infections_only = true;
  }

  return config;
}

ParallelConfig ConfigLoader::loadParallel(const std::string& filename) {
  ParallelConfig config;
  YAML::Node root = YAML::LoadFile(filename);

  if (!root["parallel"]) {
    throw std::runtime_error("Parallel config file '" + filename +
                             "' is missing the top-level 'parallel:' block.");
  }
  YAML::Node parallel = root["parallel"];

  if (parallel["enabled"]) {
    config.enabled = parallel["enabled"].as<bool>();
  }

  if (!parallel["partitioning"]) {
    throw std::runtime_error(
        "Parallel config '" + filename +
        "' is missing the required 'parallel.partitioning' block "
        "(must specify level, centroids_file, adjacency_file).");
  }
  parseParallelPartitioning(parallel["partitioning"], filename, config);

  if (parallel["chunked_loading"]) {
    YAML::Node chunked = parallel["chunked_loading"];
    if (chunked["person_metadata_chunk_size"]) {
      config.person_metadata_chunk_size =
          chunked["person_metadata_chunk_size"].as<size_t>();
    }
    if (chunked["geo_unit_chunk_size"]) {
      config.geo_unit_chunk_size = chunked["geo_unit_chunk_size"].as<size_t>();
    }
  }

  if (parallel["communication"]) {
    YAML::Node comm = parallel["communication"];
    if (comm["buffer_size_mb"]) {
      config.buffer_size_mb = comm["buffer_size_mb"].as<int>();
    }
  }

  if (parallel["output"]) {
    parseParallelOutput(parallel["output"], config);
  }

  return config;
}

ActivityPreferenceConfig ConfigLoader::loadActivityPreferences(
    const std::string& filename) {
  ActivityPreferenceConfig config;

  try {
    YAML::Node root = YAML::LoadFile(filename);
    if (root["profiles"]) {
      for (const auto& profile_node : root["profiles"]) {
        PreferenceProfile profile;
        profile.name = profile_node["name"]
                           ? profile_node["name"].as<std::string>()
                           : "unnamed";
        profile.activity = profile_node["activity"]
                               ? profile_node["activity"].as<std::string>()
                               : "";
        profile.priority =
            profile_node["priority"] ? profile_node["priority"].as<int>() : 0;

        // Load selection criteria
        if (profile_node["selection"]) {
          parseSelectionCriteria(profile_node["selection"],
                                 profile.selection_criteria);
        }

        // Load preference weights
        if (profile_node["weights"]) {
          for (const auto& weight_kv : profile_node["weights"]) {
            profile.preference_weights[weight_kv.first.as<std::string>()] =
                weight_kv.second.as<double>();
          }
        }
        config.profiles.push_back(profile);
      }
    }

    // Sort by priority
    std::sort(config.profiles.begin(), config.profiles.end(),
              [](const PreferenceProfile& a, const PreferenceProfile& b) {
                return a.priority > b.priority;
              });

  } catch (const std::exception& e) {
    std::cerr << "Warning: Could not load " << filename << ": " << e.what()
              << std::endl;
    std::cerr << "Using default activity preferences (all weights = 1.0)"
              << std::endl;
  }

  return config;
}

// loadVaccination is defined in config_loader_vaccination.cpp.
// loadCoordinatedEncounters is defined in config_loader_encounters.cpp.

}  // namespace june
