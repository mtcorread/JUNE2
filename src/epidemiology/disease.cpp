#include "epidemiology/disease.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_set>

#include "utils/filtering.h"
#ifdef USE_MPI
#include <mpi.h>
#endif
#include <sstream>

#include "utils/random.h"

namespace june {

// =============================================================================
// OutcomeRates Implementation
// =============================================================================

double OutcomeRates::getRate(const Person& person, const WorldState* world,
                             const std::string& outcome,
                             const InfectionContext& ctx) const {
  for (const auto& row : rows) {
    if (filtering::matchesCriteria(person, world, row.criteria, ctx)) {
      auto it = row.probabilities.find(outcome);
      return (it != row.probabilities.end()) ? it->second : 0.0;
    }
  }
  return 0.0;
}

void OutcomeRates::resolve(const WorldState& world) {
  for (auto& row : rows) {
    for (auto& c : row.criteria) {
      c.resolveOrThrow(world, "disease outcome rates row");
    }
  }
}

// =============================================================================
// Disease Implementation
// =============================================================================

Disease::Disease(const std::string& name,
                 const std::vector<SymptomTag>& symptom_tags,
                 const DiseaseStageSettings& stage_settings,
                 const std::vector<TrajectoryDefinition>& trajectories,
                 const OutcomeRates& outcome_rates,
                 const TransmissionParams& transmission_params)
    : name_(name),
      symptom_tags_(symptom_tags),
      stage_settings_(stage_settings),
      trajectories_(trajectories),
      outcome_rates_(outcome_rates),
      transmission_params_(transmission_params) {
  // Build lookup maps and ID name mapping
  id_to_name_.resize(symptom_tags_.size());
  for (const auto& tag : symptom_tags_) {
    symptom_tag_map_[tag.name] = tag;
    if (tag.id < id_to_name_.size()) {
      id_to_name_[tag.id] = tag.name;
    }
  }

  // Ensure at least one mode is always defined.
  if (transmission_params_.modes.empty()) {
    TransmissionMode default_mode;
    default_mode.name = "default";
    default_mode.symptom_curves = transmission_params_.symptom_id_curves;
    transmission_params_.modes.push_back(std::move(default_mode));
  }
}

const std::string& Disease::getModeName(uint8_t index) const {
  const auto& modes = transmission_params_.modes;
  if (index < modes.size()) return modes[index].name;
  static const std::string empty;
  return empty;
}

int Disease::numModes() const {
  return static_cast<int>(transmission_params_.modes.size());
}

uint16_t Disease::getSymptomId(const std::string& name) const {
  auto it = symptom_tag_map_.find(name);
  if (it == symptom_tag_map_.end()) {
    static std::unordered_set<std::string> warned_names;
    if (warned_names.find(name) == warned_names.end()) {
      warned_names.insert(name);
      std::cerr << "[WARNING] getSymptomId: unrecognized symptom name '" << name
                << "' -> returning 0 (recovered). "
                << "Check trajectory symptom_tag names match symptom_tags "
                   "definitions."
                << std::endl;
    }
    return 0;
  }
  return it->second.id;
}

const std::string& Disease::getSymptomName(uint16_t id) const {
  if (id < id_to_name_.size()) {
    return id_to_name_[id];
  }
  static const std::string unknown = "unknown";
  return unknown;
}

const SymptomTag* Disease::findSymptomTag(const std::string& name) const {
  auto it = symptom_tag_map_.find(name);
  if (it != symptom_tag_map_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool Disease::isInCategory(const std::string& symptom_name,
                           const std::vector<std::string>& category) const {
  return std::find(category.begin(), category.end(), symptom_name) !=
         category.end();
}

bool Disease::isFatalStage(const std::string& symptom_name) const {
  return isInCategory(symptom_name, stage_settings_.fatality_stages);
}

bool Disease::isRecoveredStage(const std::string& symptom_name) const {
  return isInCategory(symptom_name, stage_settings_.recovered_stages);
}

bool Disease::isHospitalisedStage(const std::string& symptom_name) const {
  return isInCategory(symptom_name, stage_settings_.hospitalised_stages);
}

bool Disease::isICUStage(const std::string& symptom_name) const {
  return isInCategory(symptom_name, stage_settings_.intensive_care_stages);
}

bool Disease::isInfectiousStage(const std::string& symptom_name) const {
  // Infectious if not healthy, recovered, or dead
  if (symptom_name == "healthy") return false;
  if (isRecoveredStage(symptom_name)) return false;
  if (isFatalStage(symptom_name)) return false;
  return true;
}

double Disease::evaluateStageDrivenInfectiousness(int mode_index,
                                                  uint16_t symptom_id,
                                                  float time_in_stage) const {
  if (transmission_params_.mode != InfectiousnessMode::STAGE_DRIVEN) {
    static bool warned = false;
    if (!warned) {
      std::cerr << "[WARNING] evaluateStageDrivenInfectiousness called on a "
                   "TRAJECTORY_DRIVEN disease. "
                << "Cross-rank visitor infectiousness will be zero. Use "
                   "STAGE_DRIVEN for multi-rank multi-mode simulations.\n";
      warned = true;
    }
    return 0.0;
  }

  const auto& modes = transmission_params_.modes;
  int safe_mode = mode_index;
  if (mode_index < 0 || mode_index >= (int)modes.size()) {
    safe_mode = 0;
    static int mode_clamp_count = 0;
    if (mode_clamp_count < 10) {
      std::cerr << "[WARNING] evaluateStageDrivenInfectiousness: mode_index="
                << mode_index << " out of range (modes.size()=" << modes.size()
                << ") -> clamped to 0" << std::endl;
      mode_clamp_count++;
    }
  }
  const auto& curves = modes.empty() ? transmission_params_.symptom_id_curves
                                     : modes[safe_mode].symptom_curves;

  if (symptom_id >= curves.size() || !curves[symptom_id]) {
    static int missing_curve_count = 0;
    if (missing_curve_count < 20) {
      std::cerr << "[WARNING] evaluateStageDrivenInfectiousness: no curve for"
                << " mode=" << safe_mode << " ("
                << (safe_mode < (int)modes.size() ? modes[safe_mode].name : "?")
                << ")"
                << " symptom_id=" << symptom_id << " ("
                << getSymptomName(symptom_id) << ")"
                << " -> returning 0.0" << std::endl;
      missing_curve_count++;
      if (missing_curve_count == 20)
        std::cerr << "[WARNING] (suppressing further missing curve warnings in "
                     "evaluateStageDrivenInfectiousness)"
                  << std::endl;
    }
    return 0.0;
  }
  return curves[symptom_id]->evaluate(static_cast<double>(time_in_stage));
}

double Disease::integrateStageDrivenInfectiousness(
    int mode_index, uint16_t symptom_id, double t_in_stage_start,
    double t_in_stage_end) const {
  if (transmission_params_.mode != InfectiousnessMode::STAGE_DRIVEN) return 0.0;

  const auto& modes = transmission_params_.modes;
  int safe_mode =
      (mode_index >= 0 && mode_index < (int)modes.size()) ? mode_index : 0;
  const auto& curves = modes.empty() ? transmission_params_.symptom_id_curves
                                     : modes[safe_mode].symptom_curves;

  if (symptom_id >= curves.size() || !curves[symptom_id]) return 0.0;
  return curves[symptom_id]->integrate(t_in_stage_start, t_in_stage_end) * 24.0;
}

double Disease::evaluateFomiteDeposition(int fomite_mode_index,
                                         uint16_t symptom_id,
                                         double time_in_stage) const {
  const auto& modes = transmission_params_.modes;
  if (fomite_mode_index < 0 || fomite_mode_index >= (int)modes.size())
    return 0.0;
  if (modes[fomite_mode_index].type != TransmissionModeType::Fomite) return 0.0;
  const auto& cfg = std::get<FomiteConfig>(modes[fomite_mode_index].config);
  if (symptom_id >= cfg.deposition_by_symptom.size()) return 0.0;
  const auto& curve = cfg.deposition_by_symptom[symptom_id];
  if (!curve) return 0.0;
  return curve->evaluate(time_in_stage);
}

double Disease::integrateFomiteDeposition(int fomite_mode_index,
                                          uint16_t symptom_id,
                                          double t_in_stage_start,
                                          double t_in_stage_end) const {
  const auto& modes = transmission_params_.modes;
  if (fomite_mode_index < 0 || fomite_mode_index >= (int)modes.size())
    return 0.0;
  if (modes[fomite_mode_index].type != TransmissionModeType::Fomite) return 0.0;
  const auto& cfg = std::get<FomiteConfig>(modes[fomite_mode_index].config);
  if (symptom_id >= cfg.deposition_by_symptom.size()) return 0.0;
  const auto& curve = cfg.deposition_by_symptom[symptom_id];
  if (!curve) return 0.0;
  return curve->integrate(t_in_stage_start, t_in_stage_end) * 24.0;
}

// =============================================================================
// Infection Implementation
// =============================================================================

Infection::Infection(const Disease* disease, double infection_time,
                     const Person* person, unsigned int random_seed,
                     const WorldState* world, const std::string& venue_type,
                     int venue_id, float severity_factor,
                     uint16_t infector_symptom_id,
                     const std::string& trajectory_key_override,
                     const std::string& start_symptom_override,
                     uint8_t transmission_mode_index)
    : disease_(disease), infection_time_(infection_time) {
  SplitMix64 rng(random_seed);

  // Sample transmission parameters only for TRAJECTORY_DRIVEN mode
  if (disease_->getTransmissionParams().mode ==
      InfectiousnessMode::TRAJECTORY_DRIVEN) {
    sampleTransmissionParameters(rng);
  }

  // Generate trajectory
  trajectory_ = generateTrajectoryFromRates(
      rng, person, world, venue_type, venue_id, severity_factor,
      infector_symptom_id, trajectory_key_override, start_symptom_override,
      transmission_mode_index);

  if (disease_->getTransmissionParams().mode ==
          InfectiousnessMode::TRAJECTORY_DRIVEN &&
      trajectory_.transitions.size() >= 2) {
    uint16_t exposed_id = disease_->getSymptomId("exposed");
    if (trajectory_.transitions[0].second == exposed_id) {
      double time_to_symptoms_onset =
          trajectory_.transitions[1].first - infection_time_;
      transmission_shift_ += time_to_symptoms_onset;
    }

    // Scale max_infectiousness by the trajectory's infectiousness_factor
    // (original JUNE: self.norm *= _modify_infectiousness_for_symptoms).
    // Applied once at creation, not per-timestep.
    max_infectiousness_ *= trajectory_.infectiousness_factor;
  }
}

std::unique_ptr<Infection> Infection::fromCheckpoint(
    const Disease* disease, double infection_time,
    const InfectionTrajectory& trajectory, double max_infectiousness,
    double transmission_shape, double transmission_rate,
    double transmission_shift, double last_checked_time,
    uint16_t cached_symptom_id, double cached_symptom_start_time) {
  // Bypass the sampling constructor entirely; restore every
  // behaviour-defining field verbatim so the resumed run is bit-identical.
  std::unique_ptr<Infection> inf(new Infection(disease, RestoreTag{}));
  inf->infection_time_ = infection_time;
  inf->trajectory_ = trajectory;
  inf->max_infectiousness_ = max_infectiousness;
  inf->transmission_shape_ = transmission_shape;
  inf->transmission_rate_ = transmission_rate;
  inf->transmission_shift_ = transmission_shift;
  inf->last_checked_time_ = last_checked_time;
  inf->cached_symptom_id_ = cached_symptom_id;
  inf->cached_symptom_start_time_ = cached_symptom_start_time;
  return inf;
}

std::string Infection::getCurrentSymptom(double current_time) const {
  uint16_t id = trajectory_.getCurrentSymptomId(current_time);
  return disease_->getSymptomName(id);
}

bool Infection::isInfectious(double current_time) const {
  uint16_t id = trajectory_.getCurrentSymptomId(current_time);
  return disease_->isInfectiousStage(disease_->getSymptomName(id));
}

bool Infection::isSymptomatic(double current_time) const {
  uint16_t id = trajectory_.getCurrentSymptomId(current_time);
  const std::string& current_symptom = disease_->getSymptomName(id);

  if (current_symptom == "healthy" || current_symptom == "exposed" ||
      current_symptom == "asymptomatic") {
    return false;
  }
  if (disease_->isRecoveredStage(current_symptom)) return false;
  if (disease_->isFatalStage(current_symptom)) return false;

  return true;
}

bool Infection::isRecovered(double current_time) const {
  uint16_t id = trajectory_.getCurrentSymptomId(current_time);
  return disease_->isRecoveredStage(disease_->getSymptomName(id));
}

bool Infection::isDead(double current_time) const {
  uint16_t id = trajectory_.getCurrentSymptomId(current_time);
  return disease_->isFatalStage(disease_->getSymptomName(id));
}

std::optional<double> Infection::getNextTransitionTime(
    double current_time) const {
  return trajectory_.getNextTransitionTime(current_time);
}

double Infection::sampleFromDistribution(const DistributionParams& dist,
                                         SplitMix64& rng) {
  if (dist.type == "constant") {
    return dist.params.at("value");
  } else if (dist.type == "normal") {
    double loc = dist.params.count("loc") ? dist.params.at("loc") : 0.0;
    double scale = dist.params.count("scale") ? dist.params.at("scale") : 1.0;
    std::normal_distribution<double> d(loc, scale);
    return std::max(0.0, d(rng));
  } else if (dist.type == "lognormal") {
    double s = dist.params.at("s");
    double loc = dist.params.at("loc");
    double scale = dist.params.at("scale");
    std::lognormal_distribution<double> d(std::log(scale), s);
    return loc + d(rng);
  } else if (dist.type == "beta") {
    double a = dist.params.at("a");
    double b = dist.params.at("b");
    double loc = dist.params.at("loc");
    double scale = dist.params.at("scale");

    // Beta distribution using gamma distributions
    std::gamma_distribution<double> gamma_a(a, 1.0);
    std::gamma_distribution<double> gamma_b(b, 1.0);
    double x = gamma_a(rng);
    double y = gamma_b(rng);
    double beta_sample = x / (x + y);
    return loc + scale * beta_sample;
  } else if (dist.type == "exponweib") {
    double a = dist.params.at("a");
    double c = dist.params.at("c");
    double loc = dist.params.at("loc");
    double scale = dist.params.at("scale");

    // Exponentiated Weibull distribution
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    double u = uniform(rng);
    double weibull_sample = scale * std::pow(-std::log(1.0 - u), 1.0 / c);
    double exp_weibull = std::pow(weibull_sample, a);
    return loc + exp_weibull;
  } else if (dist.type == "gamma") {
    double shape = dist.params.at("shape");
    double scale = dist.params.at("scale");
    double loc = dist.params.count("loc") ? dist.params.at("loc") : 0.0;
    std::gamma_distribution<double> d(shape, scale);
    return loc + d(rng);
  }

  // Default: return 0
  std::cerr << "Unknown distribution type: " << dist.type << std::endl;
  return 0.0;
}

void Infection::buildTransitionsFromStages(InfectionTrajectory& traj,
                                           const TrajectoryDefinition& traj_def,
                                           const std::string& start_target,
                                           SplitMix64& rng) {
  int start_stage_idx = 0;
  if (!start_target.empty()) {
    for (int s = 0; s < (int)traj_def.stages.size(); ++s) {
      if (traj_def.stages[s].symptom_tag == start_target) {
        start_stage_idx = s;
        break;
      }
    }
  }
  double current_time = infection_time_;
  for (int s = start_stage_idx; s < (int)traj_def.stages.size(); ++s) {
    const auto& stage = traj_def.stages[s];
    traj.transitions.push_back(
        {current_time, disease_->getSymptomId(stage.symptom_tag)});
    current_time += sampleFromDistribution(stage.completion_time, rng);
  }
}

std::pair<std::vector<double>, double> Infection::gatherTrajectoryRates(
    const Person& person, const WorldState* world,
    const InfectionContext& ctx) const {
  const auto& trajectories = disease_->getTrajectories();
  const auto& outcome_rates = disease_->getOutcomeRates();
  std::vector<double> rates;
  rates.reserve(trajectories.size());
  double total = 0.0;
  for (const auto& traj_def : trajectories) {
    double rate = 0.0;
    if (traj_def.probability.has_value()) {
      rate = traj_def.probability.value();
    } else if (!traj_def.selection_key.empty()) {
      rate = outcome_rates.getRate(person, world, traj_def.selection_key, ctx);
    }
    rates.push_back(rate);
    total += rate;
  }
  if (total > 1.0001) {
    for (double& r : rates) r /= total;
    total = 1.0;
  }
  return {std::move(rates), total};
}

int Infection::sampleTrajectoryIndex(const std::vector<double>& rates,
                                     double total_rate, SplitMix64& rng) {
  if (total_rate <= 0.0) return 0;
  std::uniform_real_distribution<double> prob_dist(0.0, 1.0);
  double rand_val = prob_dist(rng);
  double cumulative = 0.0;
  for (size_t i = 0; i < rates.size(); ++i) {
    cumulative += rates[i];
    if (rand_val <= cumulative) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void Infection::applyVaccineEfficacyShift(std::vector<double>& rates,
                                          const Person& person,
                                          double infection_time,
                                          double total_rate) const {
  if (!person.vaccine_trajectory || total_rate <= 0.0) return;
  double ve_symptoms = person.vaccine_trajectory->getEfficacy(
      infection_time, disease_->getName(), person.age, true);
  if (ve_symptoms <= 0.0) return;

  const auto& trajectories = disease_->getTrajectories();
  size_t safe_idx = 0;
  double min_severity = trajectories[0].severity;
  for (size_t i = 1; i < trajectories.size(); ++i) {
    if (trajectories[i].severity < min_severity) {
      min_severity = trajectories[i].severity;
      safe_idx = i;
    }
  }

  double total_shifted = 0.0;
  for (size_t i = 0; i < trajectories.size(); ++i) {
    if (i == safe_idx) continue;
    if (trajectories[i].severity > min_severity) {
      double shift = rates[i] * ve_symptoms;
      rates[i] -= shift;
      total_shifted += shift;
    }
  }
  rates[safe_idx] += total_shifted;
}

std::optional<InfectionTrajectory> Infection::tryBuildForcedTrajectory(
    const std::string& key, SplitMix64& rng) {
  const auto& trajectories = disease_->getTrajectories();
  for (const auto& traj_def : trajectories) {
    if (traj_def.selection_key == key) {
      InfectionTrajectory traj;
      traj.infection_time = infection_time_;
      buildTransitionsFromStages(traj, traj_def,
                                 traj_def.start_stage.value_or(""), rng);
      traj.infectiousness_factor = traj_def.infectiousness_factor;
      return traj;
    }
  }
  return std::nullopt;
}

InfectionTrajectory Infection::generateTrajectoryFromRates(
    SplitMix64& rng, const Person* person, const WorldState* world,
    const std::string& venue_type, int venue_id, float severity_factor,
    uint16_t infector_symptom_id, const std::string& trajectory_key_override,
    const std::string& start_symptom_override,
    uint8_t transmission_mode_index) {
  if (!person) {
    std::cerr
        << "WARNING: person pointer is null in generateTrajectoryFromRates"
        << std::endl;
    return InfectionTrajectory();
  }

  InfectionTrajectory traj;
  traj.infection_time = infection_time_;

  const auto& trajectories = disease_->getTrajectories();
  if (trajectories.empty()) {
    std::cerr << "WARNING: No trajectories defined for disease: "
              << disease_->getName() << std::endl;
    return traj;
  }

  if (!trajectory_key_override.empty()) {
    if (auto forced = tryBuildForcedTrajectory(trajectory_key_override, rng)) {
      return std::move(*forced);
    }
    std::cerr << "WARNING: trajectory_key_override '" << trajectory_key_override
              << "' not found; falling back to rate-based selection."
              << std::endl;
  }

  InfectionContext infection_ctx{
      disease_->getSymptomName(infector_symptom_id),
      disease_->getModeName(transmission_mode_index)};
  auto [trajectory_rates, total_rate] =
      gatherTrajectoryRates(*person, world, infection_ctx);
  applyVaccineEfficacyShift(trajectory_rates, *person, traj.infection_time,
                            total_rate);
  int selected_idx = sampleTrajectoryIndex(trajectory_rates, total_rate, rng);

  const auto& final_def = trajectories[selected_idx];
  traj.infectiousness_factor = final_def.infectiousness_factor;
  // start_symptom_override (from seeding) takes priority over the trajectory's
  // start_stage field.
  const std::string start_target = !start_symptom_override.empty()
                                       ? start_symptom_override
                                       : final_def.start_stage.value_or("");
  buildTransitionsFromStages(traj, final_def, start_target, rng);
  return traj;
}

void Infection::sampleTransmissionParameters(SplitMix64& rng) {
  const auto& trans_params = disease_->getTransmissionParams();

  // Sample max_infectiousness
  max_infectiousness_ =
      sampleFromDistribution(trans_params.max_infectiousness, rng);

  // Sample shape
  transmission_shape_ = sampleFromDistribution(trans_params.shape, rng);

  // Sample rate
  transmission_rate_ = sampleFromDistribution(trans_params.rate, rng);

  // Sample shift
  transmission_shift_ = sampleFromDistribution(trans_params.shift, rng);
}

double Infection::evaluateTransmissionProfile(double x) const {
  const auto& trans_params = disease_->getTransmissionParams();

  if (trans_params.type == "gamma") {
    // Gamma PDF: f(x; k, θ) = (1 / (Γ(k) * θ^k)) * x^(k-1) * e^(-x/θ)
    // Where k = shape, θ = 1/rate (scale parameter)
    double shape = transmission_shape_;
    double rate = transmission_rate_;

    if (x <= 0.0) return 0.0;
    double scale = 1.0 / rate;

    // Use log-space to avoid numerical overflow
    double log_pdf = (shape - 1.0) * std::log(x) - x / scale -
                     shape * std::log(scale) - std::lgamma(shape);
    return std::exp(log_pdf);
  }

  // Default or unknown type fallback
  return 1.0;
}

void Infection::cacheCurrentSymptom(double lookup_time, uint16_t& symptom_id,
                                    double& stage_start_time) const {
  if (lookup_time == last_checked_time_) {
    symptom_id = cached_symptom_id_;
    stage_start_time = cached_symptom_start_time_;
    return;
  }
  symptom_id = 0;
  stage_start_time = infection_time_;
  for (const auto& trans : trajectory_.transitions) {
    if (lookup_time >= trans.first) {
      stage_start_time = trans.first;
      symptom_id = trans.second;
    } else {
      break;
    }
  }
  last_checked_time_ = lookup_time;
  cached_symptom_id_ = symptom_id;
  cached_symptom_start_time_ = stage_start_time;
}

double Infection::getInfectiousness(double current_time) const {
  const auto& trans_params = disease_->getTransmissionParams();

  // MODE 1: TRAJECTORY-DRIVEN
  if (trans_params.mode == InfectiousnessMode::TRAJECTORY_DRIVEN) {
    // Time since infection
    double time_since_infection = current_time - infection_time_;

    // Apply shift
    double shifted_time = time_since_infection - transmission_shift_;

    // If we're before the shift, not yet infectious
    if (shifted_time <= 0.0) {
      return 0.0;
    }

    // Evaluate distribution profile at shifted time (e.g. Gamma PDF)
    double profile_value = evaluateTransmissionProfile(shifted_time);

    // Scale by max infectiousness (already includes infectiousness_factor
    // from the selected trajectory, applied once at infection creation)
    return max_infectiousness_ * profile_value;
  }

  // MODE 2: STAGE-DRIVEN
  uint16_t current_symptom_id = 0;
  double stage_start_time = infection_time_;
  cacheCurrentSymptom(current_time, current_symptom_id, stage_start_time);

  // Evaluate curve for the specific symptom stage
  if (current_symptom_id < trans_params.symptom_id_curves.size()) {
    const auto& curve = trans_params.symptom_id_curves[current_symptom_id];
    if (curve) {
      double time_in_stage = current_time - stage_start_time;
      return curve->evaluate(time_in_stage);
    }
  }

  return 0.0;
}

double Infection::getInfectiousness(int mode_index, double current_time) const {
  const auto& trans_params = disease_->getTransmissionParams();

  // TRAJECTORY-DRIVEN: per-mode not supported; return scalar infectiousness for
  // all modes.
  if (trans_params.mode == InfectiousnessMode::TRAJECTORY_DRIVEN) {
    return getInfectiousness(current_time);
  }

  // STAGE-DRIVEN: use mode-specific curve.
  const auto& modes = trans_params.modes;
  int safe_mode = mode_index;
  if (mode_index < 0 || mode_index >= (int)modes.size()) {
    safe_mode = 0;
    static int mode_clamp_count2 = 0;
    if (mode_clamp_count2 < 10) {
      std::cerr << "[WARNING] getInfectiousness(mode): mode_index="
                << mode_index << " out of range (modes.size()=" << modes.size()
                << ") -> clamped to 0" << std::endl;
      mode_clamp_count2++;
    }
  }
  const auto& curves = modes.empty() ? trans_params.symptom_id_curves
                                     : modes[safe_mode].symptom_curves;

  uint16_t current_symptom_id = 0;
  double stage_start_time = infection_time_;
  cacheCurrentSymptom(current_time, current_symptom_id, stage_start_time);

  if (current_symptom_id >= curves.size() || !curves[current_symptom_id]) {
    static int missing_curve_count2 = 0;
    if (missing_curve_count2 < 20) {
      std::cerr << "[WARNING] getInfectiousness(mode): no curve for"
                << " mode=" << safe_mode << " ("
                << (safe_mode < (int)modes.size() ? modes[safe_mode].name : "?")
                << ")"
                << " symptom_id=" << current_symptom_id << " ("
                << disease_->getSymptomName(current_symptom_id) << ")"
                << " -> returning 0.0" << std::endl;
      missing_curve_count2++;
      if (missing_curve_count2 == 20)
        std::cerr << "[WARNING] (suppressing further missing curve warnings in "
                     "getInfectiousness)"
                  << std::endl;
    }
    return 0.0;
  }
  double t_in_stage = current_time - stage_start_time;
  return curves[current_symptom_id]->evaluate(t_in_stage);
}

double Infection::getIntegratedInfectiousness(int mode_index, double t0,
                                              double t1) const {
  const auto& trans_params = disease_->getTransmissionParams();

  // TRAJECTORY-DRIVEN: no per-stage curve; use point-eval midpoint fallback.
  if (trans_params.mode == InfectiousnessMode::TRAJECTORY_DRIVEN) {
    return getInfectiousness(mode_index, 0.5 * (t0 + t1)) * (t1 - t0) * 24.0;
  }

  // STAGE-DRIVEN: use mode-specific curve and integrate.
  const auto& modes = trans_params.modes;
  int safe_mode =
      (mode_index >= 0 && mode_index < (int)modes.size()) ? mode_index : 0;
  const auto& curves = modes.empty() ? trans_params.symptom_id_curves
                                     : modes[safe_mode].symptom_curves;

  uint16_t current_symptom_id = 0;
  double stage_start_time = infection_time_;
  cacheCurrentSymptom(t0, current_symptom_id, stage_start_time);

  if (current_symptom_id >= curves.size() || !curves[current_symptom_id]) {
    return 0.0;
  }
  const double t_in_stage_0 = t0 - stage_start_time;
  const double t_in_stage_1 = t1 - stage_start_time;
  return curves[current_symptom_id]->integrate(t_in_stage_0, t_in_stage_1) *
         24.0;
}

double Infection::getFomiteDepositRate(int fomite_mode_index,
                                       double current_time) const {
  const auto& trans_params = disease_->getTransmissionParams();
  const auto& modes = trans_params.modes;
  if (fomite_mode_index < 0 || fomite_mode_index >= (int)modes.size())
    return 0.0;
  if (modes[fomite_mode_index].type != TransmissionModeType::Fomite) return 0.0;
  const auto& cfg = std::get<FomiteConfig>(modes[fomite_mode_index].config);

  uint16_t current_symptom_id = 0;
  double stage_start_time = infection_time_;
  cacheCurrentSymptom(current_time, current_symptom_id, stage_start_time);

  if (current_symptom_id >= cfg.deposition_by_symptom.size()) return 0.0;
  const auto& curve = cfg.deposition_by_symptom[current_symptom_id];
  if (!curve) return 0.0;
  return curve->evaluate(current_time - stage_start_time);
}

double Infection::getIntegratedFomiteDeposition(int fomite_mode_index,
                                                double t0, double t1) const {
  const auto& trans_params = disease_->getTransmissionParams();
  const auto& modes = trans_params.modes;
  if (fomite_mode_index < 0 || fomite_mode_index >= (int)modes.size())
    return 0.0;
  if (modes[fomite_mode_index].type != TransmissionModeType::Fomite) return 0.0;
  const auto& cfg = std::get<FomiteConfig>(modes[fomite_mode_index].config);

  uint16_t current_symptom_id = 0;
  double stage_start_time = infection_time_;
  cacheCurrentSymptom(t0, current_symptom_id, stage_start_time);

  if (current_symptom_id >= cfg.deposition_by_symptom.size()) return 0.0;
  const auto& curve = cfg.deposition_by_symptom[current_symptom_id];
  if (!curve) return 0.0;
  return curve->integrate(t0 - stage_start_time, t1 - stage_start_time) * 24.0;
}

}  // namespace june
