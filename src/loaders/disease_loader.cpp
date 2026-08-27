#include "loaders/disease_loader.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

#include "utils/filtered_csv.h"

namespace june {

namespace {

void logCurveRescale(const std::string& context_label, const char* type_name,
                     double max_inf, double factor) {
  std::cout << "[DEBUG] Curve (" << context_label << "): type=" << type_name
            << "  old_peak=" << max_inf * factor
            << "  max_infectiousness=" << max_inf
            << "  rescale_factor=" << factor
            << "  (set max_infectiousness: " << max_inf * factor
            << " in YAML to match previous behaviour)\n";
}

std::shared_ptr<ConstantCurve> makeConstantCurve(const YAML::Node& node) {
  double value = node["value"] ? node["value"].as<double>() : 1.0;
  return std::make_shared<ConstantCurve>(value);
}

std::shared_ptr<ExponentialDecayCurve> makeExponentialDecayCurve(
    const YAML::Node& node) {
  double initial =
      node["initial_value"] ? node["initial_value"].as<double>() : 1.0;
  double decay = node["decay_rate"] ? node["decay_rate"].as<double>() : 0.5;
  double delay = node["delay"] ? node["delay"].as<double>() : 0.0;
  return std::make_shared<ExponentialDecayCurve>(initial, decay, delay);
}

std::shared_ptr<LinearRampCurve> makeLinearRampCurve(const YAML::Node& node) {
  double start = node["start_value"] ? node["start_value"].as<double>() : 0.0;
  double end = node["end_value"] ? node["end_value"].as<double>() : 1.0;
  double duration =
      node["ramp_duration"] ? node["ramp_duration"].as<double>() : 1.0;
  return std::make_shared<LinearRampCurve>(start, end, duration);
}

std::shared_ptr<GammaCurve> makeGammaCurve(const YAML::Node& node,
                                           const std::string& context_label,
                                           bool verbose) {
  double max_inf = node["max_infectiousness"]
                       ? node["max_infectiousness"].as<double>()
                       : 1.0;
  double shape = node["shape"] ? node["shape"].as<double>() : 1.56;
  double rate = node["rate"] ? node["rate"].as<double>() : 0.53;
  double shift = node["shift"] ? node["shift"].as<double>() : 0.0;
  auto curve = std::make_shared<GammaCurve>(max_inf, shape, rate, shift);
  if (verbose) {
    logCurveRescale(context_label, "gamma", max_inf,
                    curve->peakScalingFactor());
  }
  return curve;
}

std::shared_ptr<LognormalCurve> makeLognormalCurve(
    const YAML::Node& node, const std::string& context_label, bool verbose) {
  double max_inf = node["max_infectiousness"]
                       ? node["max_infectiousness"].as<double>()
                       : 1.0;
  double mu = node["mu"] ? node["mu"].as<double>() : 0.0;
  double sigma = node["sigma"] ? node["sigma"].as<double>() : 1.0;
  auto curve = std::make_shared<LognormalCurve>(max_inf, mu, sigma);
  if (verbose) {
    logCurveRescale(context_label, "lognormal", max_inf,
                    curve->peakScalingFactor());
  }
  return curve;
}

std::shared_ptr<BetaCurve> makeBetaCurve(const YAML::Node& node,
                                         const std::string& context_label,
                                         bool verbose) {
  double max_inf = node["max_infectiousness"]
                       ? node["max_infectiousness"].as<double>()
                       : 1.0;
  double alpha = node["alpha"] ? node["alpha"].as<double>() : 2.0;
  double beta = node["beta"] ? node["beta"].as<double>() : 2.0;
  double duration = node["duration"] ? node["duration"].as<double>() : 1.0;
  auto curve = std::make_shared<BetaCurve>(max_inf, alpha, beta, duration);
  if (verbose) {
    logCurveRescale(context_label, "beta", max_inf, curve->peakScalingFactor());
  }
  return curve;
}

}  // namespace

// =============================================================================
// Parse Distribution Parameters from YAML
// =============================================================================

DistributionParams DiseaseLoader::parseDistribution(
    const YAML::Node& dist_node) {
  DistributionParams dist;

  if (!dist_node["type"]) {
    throw std::runtime_error("Distribution must have a 'type' field");
  }

  dist.type = dist_node["type"].as<std::string>();

  // Parse all parameters into the params map
  for (auto it = dist_node.begin(); it != dist_node.end(); ++it) {
    std::string key = it->first.as<std::string>();
    if (key != "type") {  // Skip the type field
      try {
        double value = it->second.as<double>();
        dist.params[key] = value;
      } catch (const YAML::Exception& e) {
        std::cerr << "Warning: Could not parse parameter '" << key
                  << "' as double: " << e.what() << std::endl;
      }
    }
  }

  return dist;
}

// =============================================================================
// Parse Trajectory Definitions from YAML
// =============================================================================

std::optional<TrajectoryStage> DiseaseLoader::parseTrajectoryStage(
    const YAML::Node& stage_node) {
  if (!stage_node["symptom_tag"]) {
    std::cerr << "Warning: Stage missing 'symptom_tag' field" << std::endl;
    return std::nullopt;
  }
  TrajectoryStage stage;
  stage.symptom_tag = stage_node["symptom_tag"].as<std::string>();
  if (stage_node["completion_time"]) {
    stage.completion_time = parseDistribution(stage_node["completion_time"]);
  }
  return stage;
}

std::optional<TrajectoryDefinition> DiseaseLoader::parseOneTrajectory(
    const YAML::Node& traj_node) {
  TrajectoryDefinition traj;

  if (traj_node["description"]) {
    traj.description = traj_node["description"].as<std::string>();
  }

  if (traj_node["selection_key"]) {
    traj.selection_key = traj_node["selection_key"].as<std::string>();
  }

  if (traj_node["probability"]) {
    traj.probability = traj_node["probability"].as<double>();
  }

  traj.severity =
      traj_node["severity"] ? traj_node["severity"].as<double>() : 0.0;

  traj.infectiousness_factor =
      traj_node["infectiousness_factor"]
          ? traj_node["infectiousness_factor"].as<double>()
          : 1.0;

  if (traj_node["start_stage"]) {
    traj.start_stage = traj_node["start_stage"].as<std::string>();
  }

  if (!traj_node["stages"]) {
    std::cerr << "Warning: Trajectory missing 'stages' field" << std::endl;
    return std::nullopt;
  }

  for (const auto& stage_node : traj_node["stages"]) {
    if (auto stage = parseTrajectoryStage(stage_node)) {
      traj.stages.push_back(std::move(*stage));
    }
  }

  return traj;
}

std::vector<TrajectoryDefinition> DiseaseLoader::parseTrajectories(
    const YAML::Node& trajectories_node) {
  std::vector<TrajectoryDefinition> trajectories;

  if (!trajectories_node || !trajectories_node.IsSequence()) {
    std::cerr << "Warning: No trajectories defined or invalid format"
              << std::endl;
    return trajectories;
  }

  for (const auto& traj_node : trajectories_node) {
    if (auto traj = parseOneTrajectory(traj_node)) {
      trajectories.push_back(std::move(*traj));
    }
  }

  return trajectories;
}

// =============================================================================
// Load Outcome Rates from filter-column CSV
// =============================================================================

OutcomeRates DiseaseLoader::loadOutcomeRatesFromCSV(
    const std::string& csv_path) {
  OutcomeRates rates;
  csv::FilteredTable table = csv::loadFilteredCSV(csv_path);

  int row_num = 0;
  for (const auto& r : table.rows) {
    ++row_num;
    OutcomeRow row;
    row.criteria = r.criteria;
    for (const auto& name : table.value_columns) {
      auto it = r.values.find(name);
      if (it == r.values.end() || it->second.empty()) continue;
      try {
        row.probabilities[name] = std::stod(it->second);
      } catch (const std::exception&) {
        throw std::runtime_error("Outcome rates CSV '" + csv_path + "' row " +
                                 std::to_string(row_num) + " column '" + name +
                                 "' has non-numeric value '" + it->second +
                                 "'");
      }
    }
    rates.rows.push_back(std::move(row));
  }

  return rates;
}

// =============================================================================
// Section helpers for loadFromYAML
// =============================================================================

void DiseaseLoader::loadModeFomite(const YAML::Node& mode_node,
                                   TransmissionMode& tmode, int mode_idx,
                                   const std::vector<SymptomTag>& symptom_tags,
                                   bool verbose) {
  tmode.type = TransmissionModeType::Fomite;
  FomiteConfig fcfg;
  fcfg.mode_index = mode_idx;
  fcfg.max_age =
      mode_node["max_age"] ? mode_node["max_age"].as<double>() : 14.0;
  fcfg.sub_bin_time =
      mode_node["sub_bin_time"] ? mode_node["sub_bin_time"].as<double>() : 0.0;
  if (mode_node["fomite_curve"]) {
    fcfg.infectiousness_curve =
        parseCurve(mode_node["fomite_curve"],
                   "fomite / " + tmode.name + " / fomite_curve", verbose);
  } else {
    fcfg.infectiousness_curve = std::make_shared<ConstantCurve>(0.0);
  }
  parseDepositionStages(mode_node, fcfg.deposition_by_symptom, "fomite",
                        tmode.name, symptom_tags, verbose);
  tmode.config = std::move(fcfg);
  std::cout << "[DiseaseLoader] Fomite mode '" << tmode.name
            << "' registered at index " << mode_idx << std::endl;
}

void DiseaseLoader::loadTransmission(
    const YAML::Node& config, TransmissionParams& transmission,
    const std::vector<SymptomTag>& symptom_tags, bool verbose) {
  if (!config["transmission"]) return;
  auto trans_node = config["transmission"];

  // Detect mode (default to Trajectory-Driven)
  std::string mode_str = trans_node["mode"]
                             ? trans_node["mode"].as<std::string>()
                             : "Trajectory-Driven";
  transmission.mode = (mode_str == "Stage-Driven")
                          ? InfectiousnessMode::STAGE_DRIVEN
                          : InfectiousnessMode::TRAJECTORY_DRIVEN;

  if (transmission.mode == InfectiousnessMode::TRAJECTORY_DRIVEN) {
    loadTransmissionTrajectoryDriven(trans_node, transmission);
  } else {
    loadTransmissionStageDriven(trans_node, transmission, symptom_tags,
                                verbose);
  }
}

void DiseaseLoader::loadTransmissionStageDriven(
    const YAML::Node& trans_node, TransmissionParams& transmission,
    const std::vector<SymptomTag>& symptom_tags, bool verbose) {
  if (trans_node["modes"]) {
    // Multi-mode: parse modes list with per-mode stage_curves
    parseStageDrivenModes(trans_node["modes"], transmission, symptom_tags,
                          verbose);
    if (trans_node["stage_curves"]) {
      attachStageCurvesToModes(trans_node["stage_curves"], transmission,
                               symptom_tags, verbose);
    }
    finalizeStageDrivenMultiMode(transmission, symptom_tags);
  } else {
    loadTransmissionStageDrivenFlat(trans_node, transmission, symptom_tags,
                                    verbose);
  }
}

void DiseaseLoader::loadTransmissionStageDrivenFlat(
    const YAML::Node& trans_node, TransmissionParams& transmission,
    const std::vector<SymptomTag>& symptom_tags, bool verbose) {
  // Flat stage_curves at top level: single Standard mode.
  if (trans_node["stage_curves"]) {
    for (auto it = trans_node["stage_curves"].begin();
         it != trans_node["stage_curves"].end(); ++it) {
      std::string stage_name = it->first.as<std::string>();
      transmission.stage_curves[stage_name] =
          parseCurve(it->second, "stage_curves / " + stage_name, verbose);
    }
  }

  // Populate hot-path vector for ID-based lookups.
  transmission.symptom_id_curves.resize(symptom_tags.size(), nullptr);
  for (const auto& tag : symptom_tags) {
    auto it = transmission.stage_curves.find(tag.name);
    if (it != transmission.stage_curves.end()) {
      transmission.symptom_id_curves[tag.id] = it->second;
    }
  }

  // Single Standard mode.
  TransmissionMode tmode;
  tmode.name = "default";
  tmode.symptom_curves = transmission.symptom_id_curves;
  transmission.modes.push_back(std::move(tmode));
}

void DiseaseLoader::finalizeStageDrivenMultiMode(
    TransmissionParams& transmission,
    const std::vector<SymptomTag>& symptom_tags) {
  // Fill zero symptom_curves for modes without explicit stage_curves
  // (fomite, compartmental modes have no person-to-person infectiousness).
  std::vector<std::shared_ptr<InfectiousnessCurve>> zero_curves(
      symptom_tags.size(), std::make_shared<ConstantCurve>(0.0));
  for (auto& tmode : transmission.modes) {
    if (tmode.symptom_curves.empty()) {
      tmode.symptom_curves = zero_curves;
    }
  }

  // Populate flat symptom_id_curves from first Standard mode.
  transmission.symptom_id_curves.resize(symptom_tags.size(), nullptr);
  for (const auto& tmode : transmission.modes) {
    if (tmode.type == TransmissionModeType::Standard) {
      transmission.symptom_id_curves = tmode.symptom_curves;
      break;
    }
  }
}

void DiseaseLoader::attachStageCurvesToModes(
    const YAML::Node& stage_curves_node, TransmissionParams& transmission,
    const std::vector<SymptomTag>& symptom_tags, bool verbose) {
  for (const auto& mode_kv : stage_curves_node) {
    std::string mname = mode_kv.first.as<std::string>();
    std::vector<std::shared_ptr<InfectiousnessCurve>> mode_curves(
        symptom_tags.size(), nullptr);
    for (auto it = mode_kv.second.begin(); it != mode_kv.second.end(); ++it) {
      std::string stage_name = it->first.as<std::string>();
      auto curve = parseCurve(it->second, mname + " / " + stage_name, verbose);
      transmission.stage_curves[stage_name] = curve;
      for (const auto& tag : symptom_tags) {
        if (tag.name == stage_name) {
          mode_curves[tag.id] = curve;
          break;
        }
      }
    }
    // Find mode index by name and store symptom_curves on the mode.
    for (auto& tmode : transmission.modes) {
      if (tmode.name == mname) {
        for (const auto& tag : symptom_tags) {
          if (tag.id < mode_curves.size() && !mode_curves[tag.id]) {
            if (tag.value > 0) {
              std::cerr << "[DiseaseLoader] Warning: mode '" << mname
                        << "' has no stage_curve for symptom '" << tag.name
                        << "'; using zero infectiousness." << std::endl;
            }
            mode_curves[tag.id] = std::make_shared<ConstantCurve>(0.0);
          }
        }
        tmode.symptom_curves = mode_curves;
        break;
      }
    }
  }
}

void DiseaseLoader::parseStageDrivenModes(
    const YAML::Node& modes_node, TransmissionParams& transmission,
    const std::vector<SymptomTag>& symptom_tags, bool verbose) {
  for (const auto& mode_node : modes_node) {
    TransmissionMode tmode;
    tmode.name =
        mode_node["name"] ? mode_node["name"].as<std::string>() : "default";
    tmode.mode_transmissibility_multiplier =
        mode_node["mode_transmissibility_multiplier"]
            ? mode_node["mode_transmissibility_multiplier"].as<double>()
            : 1.0;

    std::string mode_type =
        mode_node["type"] ? mode_node["type"].as<std::string>() : "";
    int mode_idx = static_cast<int>(transmission.modes.size());

    if (mode_type == "fomite") {
      loadModeFomite(mode_node, tmode, mode_idx, symptom_tags, verbose);
    } else if (mode_type == "compartmental_uptake") {
      loadModeCompartmentalUptake(tmode, mode_idx);
    } else if (mode_type == "compartmental_deposition") {
      loadModeCompartmentalDeposition(mode_node, tmode, mode_idx, symptom_tags,
                                      verbose);
    }

    transmission.modes.push_back(std::move(tmode));
  }
}

void DiseaseLoader::loadModeCompartmentalUptake(TransmissionMode& tmode,
                                                int mode_idx) {
  tmode.type = TransmissionModeType::CompartmentalUptake;
  CompartmentalUptakeConfig ucfg;
  ucfg.mode_index = mode_idx;
  tmode.config = ucfg;
  std::cout << "[DiseaseLoader] Compartmental uptake mode '" << tmode.name
            << "' registered at index " << mode_idx << std::endl;
}

void DiseaseLoader::loadModeCompartmentalDeposition(
    const YAML::Node& mode_node, TransmissionMode& tmode, int mode_idx,
    const std::vector<SymptomTag>& symptom_tags, bool verbose) {
  tmode.type = TransmissionModeType::CompartmentalDeposition;
  CompartmentalDepositionConfig dcfg;
  dcfg.mode_index = mode_idx;
  parseDepositionStages(mode_node, dcfg.deposition_by_symptom,
                        "compartmental_deposition", tmode.name, symptom_tags,
                        verbose);
  tmode.config = std::move(dcfg);
  std::cout << "[DiseaseLoader] Compartmental deposition mode '" << tmode.name
            << "' registered at index " << mode_idx << std::endl;
}

void DiseaseLoader::loadTransmissionTrajectoryDriven(
    const YAML::Node& trans_node, TransmissionParams& transmission) {
  if (trans_node["type"]) {
    transmission.type = trans_node["type"].as<std::string>();
  } else {
    transmission.type = "gamma";
  }

  // Load infectiousness distribution parameters
  if (trans_node["max_infectiousness"]) {
    transmission.max_infectiousness =
        parseDistribution(trans_node["max_infectiousness"]);
  }
  if (trans_node["shape"]) {
    transmission.shape = parseDistribution(trans_node["shape"]);
  }
  if (trans_node["rate"]) {
    transmission.rate = parseDistribution(trans_node["rate"]);
  }
  if (trans_node["shift"]) {
    transmission.shift = parseDistribution(trans_node["shift"]);
  }

  // Note: symptom-specific infectiousness scaling is now handled via
  // infectiousness_factor on each TrajectoryDefinition, applied once at
  // infection creation (matching original JUNE behaviour).

  // Multi-mode support for TRAJECTORY_DRIVEN: parse optional modes list
  // for mode names and susceptibility multipliers. No per-mode curves
  // are loaded; getInfectiousness(m, t) returns the same scalar for all
  // modes; the multipliers scale susceptibility per-mode at the FOI step.
  if (trans_node["modes"]) {
    for (const auto& mode_node : trans_node["modes"]) {
      TransmissionMode tmode;
      tmode.name =
          mode_node["name"] ? mode_node["name"].as<std::string>() : "default";
      tmode.mode_transmissibility_multiplier =
          mode_node["mode_transmissibility_multiplier"]
              ? mode_node["mode_transmissibility_multiplier"].as<double>()
              : 1.0;
      transmission.modes.push_back(std::move(tmode));
    }
  } else {
    TransmissionMode tmode;
    tmode.name = "default";
    transmission.modes.push_back(std::move(tmode));
  }
}

void DiseaseLoader::parseDepositionStages(
    const YAML::Node& mode_node,
    std::vector<std::shared_ptr<InfectiousnessCurve>>& deposition_by_symptom,
    const std::string& mode_type_prefix, const std::string& mode_name,
    const std::vector<SymptomTag>& symptom_tags, bool verbose) {
  deposition_by_symptom.resize(symptom_tags.size(), nullptr);
  if (!mode_node["deposition_stages"]) return;
  for (auto it = mode_node["deposition_stages"].begin();
       it != mode_node["deposition_stages"].end(); ++it) {
    std::string stage_name = it->first.as<std::string>();
    auto curve = parseCurve(
        it->second, mode_type_prefix + " / " + mode_name + " / " + stage_name,
        verbose);
    for (const auto& tag : symptom_tags) {
      if (tag.name == stage_name) {
        deposition_by_symptom[tag.id] = curve;
        break;
      }
    }
  }
}

void DiseaseLoader::loadNaturalImmunity(const YAML::Node& config,
                                        TransmissionParams& transmission) {
  if (!config["immunity"]) return;
  auto immunity_node = config["immunity"];
  transmission.natural_immunity.level =
      immunity_node["level"] ? immunity_node["level"].as<double>() : 0.95;
  transmission.natural_immunity.waning_rate =
      immunity_node["waning_rate"] ? immunity_node["waning_rate"].as<double>()
                                   : 0.001;
}

void DiseaseLoader::validateOutcomeRowSums(const OutcomeRates& outcome_rates) {
  for (size_t row_i = 0; row_i < outcome_rates.rows.size(); ++row_i) {
    double row_sum = 0.0;
    for (const auto& [key, prob] : outcome_rates.rows[row_i].probabilities) {
      row_sum += prob;
    }
    if (!outcome_rates.rows[row_i].probabilities.empty() &&
        std::abs(row_sum - 1.0) > 0.01) {
      // Throws rather than warns: this fired on 5 of 40 rows of the old
      // covid19 table (worst sum 1.326) and the warning was printed and
      // ignored for the life of that file. A row that does not sum to 1 is
      // not a table the trajectory walk can sample from.
      throw std::runtime_error(
          "Outcome rates row " + std::to_string(row_i) + " sums to " +
          std::to_string(row_sum) +
          ", expected 1.0. Outcome columns are a "
          "probability distribution over trajectories and must sum to 1.");
    }
    for (const auto& [key, prob] : outcome_rates.rows[row_i].probabilities) {
      // Second net, not the first one: a negative rate can hide inside a row
      // that still sums to 1.
      if (prob < 0.0) {
        throw std::runtime_error("Outcome rates row " + std::to_string(row_i) +
                                 " has negative '" + key +
                                 "' = " + std::to_string(prob) + ".");
      }
    }
  }
}

OutcomeRates DiseaseLoader::loadOutcomeRatesFromConfig(
    const YAML::Node& config, const std::string& yaml_path) {
  OutcomeRates outcome_rates;
  if (!config["outcome_rates_csv"]) return outcome_rates;
  const auto& csv_node = config["outcome_rates_csv"];

  // Accept either a scalar path ("outcome_rates_csv: path/to/file.csv")
  // or a mapping with a "file:" key (legacy format).
  std::string csv_rel_path;
  if (csv_node.IsScalar()) {
    csv_rel_path = csv_node.as<std::string>();
  } else if (csv_node["file"]) {
    csv_rel_path = csv_node["file"].as<std::string>();
  } else {
    throw std::runtime_error(
        "outcome_rates_csv must be a file path or a mapping with a 'file:' "
        "key");
  }

  std::string yaml_dir = yaml_path.substr(0, yaml_path.find_last_of("/\\") + 1);
  return loadOutcomeRatesFromCSV(yaml_dir + csv_rel_path);
}

void DiseaseLoader::validateTrajectoryStageRefs(
    const std::vector<TrajectoryDefinition>& trajectories,
    const std::vector<SymptomTag>& symptom_tags) {
  std::unordered_set<std::string> valid_tags;
  for (const auto& tag : symptom_tags) {
    valid_tags.insert(tag.name);
  }
  for (size_t ti = 0; ti < trajectories.size(); ++ti) {
    const auto& traj = trajectories[ti];
    const std::string traj_label =
        traj.description.empty() ? ("trajectory[" + std::to_string(ti) + "]")
                                 : ("trajectory \"" + traj.description + "\"");
    // BUG-S10: start_stage must name a defined symptom tag
    if (traj.start_stage.has_value() &&
        valid_tags.find(*traj.start_stage) == valid_tags.end()) {
      throw std::runtime_error("Disease config error: " + traj_label +
                               " has start_stage \"" + *traj.start_stage +
                               "\" which is not a defined symptom tag.");
    }
    // BUG-S11: every stage symptom_tag must name a defined symptom tag
    for (size_t si = 0; si < traj.stages.size(); ++si) {
      const auto& stage = traj.stages[si];
      if (valid_tags.find(stage.symptom_tag) == valid_tags.end()) {
        throw std::runtime_error("Disease config error: " + traj_label +
                                 " stage[" + std::to_string(si) +
                                 "] has symptom_tag \"" + stage.symptom_tag +
                                 "\" which is not a defined symptom tag.");
      }
    }
  }
}

DiseaseStageSettings DiseaseLoader::loadStageSettings(
    const YAML::Node& config) {
  DiseaseStageSettings stage_settings;
  if (!config["settings"]) return stage_settings;
  auto settings = config["settings"];

  if (settings["default_lowest_stage"]) {
    stage_settings.default_lowest_stage =
        settings["default_lowest_stage"].as<std::string>();
  }
  if (settings["max_mild_symptom_tag"]) {
    stage_settings.max_mild_symptom_tag =
        settings["max_mild_symptom_tag"].as<std::string>();
  }

  auto loadStageList = [](const YAML::Node& node,
                          std::vector<std::string>& out) {
    if (!node) return;
    for (const auto& item : node) {
      if (item["name"]) {
        out.push_back(item["name"].as<std::string>());
      }
    }
  };

  loadStageList(settings["stay_at_home_stage"],
                stage_settings.stay_at_home_stages);
  loadStageList(settings["severe_symptoms_stay_at_home_stage"],
                stage_settings.severe_symptoms_stay_at_home_stages);
  loadStageList(settings["hospitalised_stage"],
                stage_settings.hospitalised_stages);
  loadStageList(settings["intensive_care_stage"],
                stage_settings.intensive_care_stages);
  loadStageList(settings["fatality_stage"], stage_settings.fatality_stages);
  loadStageList(settings["recovered_stage"], stage_settings.recovered_stages);
  return stage_settings;
}

std::vector<SymptomTag> DiseaseLoader::loadSymptomTags(
    const YAML::Node& config) {
  std::vector<SymptomTag> symptom_tags;
  if (!config["symptom_tags"]) return symptom_tags;
  // Symptom ids are narrowed to uint8_t for event logging and cross-rank
  // transmission (PendingInfection, InfectionEvent); kNoSymptomId reserves
  // 255 as the "not applicable" sentinel, so a real id can't reach it.
  if (config["symptom_tags"].size() >= kNoSymptomId) {
    throw std::runtime_error(
        "DiseaseLoader::loadSymptomTags: too many symptom_tags (" +
        std::to_string(config["symptom_tags"].size()) +
        "); ids are stored as uint8_t and must stay below " +
        std::to_string(static_cast<int>(kNoSymptomId)));
  }
  uint16_t current_id = 0;
  for (const auto& tag_node : config["symptom_tags"]) {
    SymptomTag tag;
    tag.name = tag_node["name"].as<std::string>();
    tag.value = tag_node["value"].as<int>();
    tag.id = current_id++;
    symptom_tags.push_back(tag);
  }
  return symptom_tags;
}

// =============================================================================
// Main Loading Function
// =============================================================================

Disease DiseaseLoader::loadFromYAML(const std::string& yaml_path,
                                    bool verbose) {
  try {
    YAML::Node root = YAML::LoadFile(yaml_path);

    // Get disease node
    if (!root["disease"]) {
      throw std::runtime_error("YAML must contain 'disease' section");
    }

    YAML::Node config = root["disease"];

    // Get disease name
    std::string disease_name = config["name"].as<std::string>("covid19");

    std::vector<SymptomTag> symptom_tags = loadSymptomTags(config);

    DiseaseStageSettings stage_settings = loadStageSettings(config);

    // === Parse Trajectories ===
    std::vector<TrajectoryDefinition> trajectories;
    if (config["trajectories"]) {
      trajectories = parseTrajectories(config["trajectories"]);
    }

    validateTrajectoryStageRefs(trajectories, symptom_tags);

    OutcomeRates outcome_rates = loadOutcomeRatesFromConfig(config, yaml_path);

    TransmissionParams transmission;
    loadTransmission(config, transmission, symptom_tags, verbose);

    loadNaturalImmunity(config, transmission);
    validateOutcomeRowSums(outcome_rates);

    if (static_cast<int>(transmission.modes.size()) > VisitorInfo::MAX_MODES) {
      throw std::runtime_error(
          "Disease '" + disease_name + "' has " +
          std::to_string(transmission.modes.size()) +
          " transmission modes but VisitorInfo::MAX_MODES=" +
          std::to_string(VisitorInfo::MAX_MODES) +
          ". Increase MAX_MODES in include/core/types.h.");
    }

    // Create and return Disease object
    return Disease(disease_name, symptom_tags, stage_settings, trajectories,
                   outcome_rates, transmission);

  } catch (const YAML::Exception& e) {
    throw std::runtime_error("Failed to load disease config from " + yaml_path +
                             ": " + e.what());
  } catch (const std::exception& e) {
    throw std::runtime_error("Error loading disease config: " +
                             std::string(e.what()));
  }
}

// =============================================================================
// Parse an infectiousness curve from YAML
// =============================================================================

std::shared_ptr<InfectiousnessCurve> DiseaseLoader::parseCurve(
    const YAML::Node& curve_node, const std::string& context_label,
    bool verbose) {
  if (!curve_node || !curve_node["type"]) {
    return std::make_shared<ConstantCurve>(0.0);
  }

  std::string type = curve_node["type"].as<std::string>();

  static constexpr double kTableMaxDays = 90.0;
  static constexpr int kTableNPoints = 2700;

  std::shared_ptr<InfectiousnessCurve> curve;
  if (type == "constant") {
    curve = makeConstantCurve(curve_node);
  } else if (type == "gamma") {
    curve = makeGammaCurve(curve_node, context_label, verbose);
  } else if (type == "exponential_decay") {
    curve = makeExponentialDecayCurve(curve_node);
  } else if (type == "linear_ramp") {
    curve = makeLinearRampCurve(curve_node);
  } else if (type == "lognormal") {
    curve = makeLognormalCurve(curve_node, context_label, verbose);
  } else if (type == "beta") {
    curve = makeBetaCurve(curve_node, context_label, verbose);
  } else {
    throw std::runtime_error("Unknown infectiousness curve type: " + type);
  }

  curve->buildIntegralTable(kTableMaxDays, kTableNPoints);
  return curve;
}

}  // namespace june
