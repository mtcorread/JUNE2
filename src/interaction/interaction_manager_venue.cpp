// Standard FOI path: processVenueTransmissions orchestrator plus the
// per-susceptible Bernoulli pipeline (per-source builders, susc-bin
// orchestrator, infector sampling, infection apply).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>

#include "epidemiology/interaction_manager.h"
#include "simulation/compartmental_model_manager.h"
#include "utils/deterministic_rng.h"
#include "utils/event_logging/event_types.h"
#include "utils/random.h"

namespace june {

// =============================================================================
// processVenueTransmissions: entry point for the standard FOI path.
//
// Called from InteractionManager::processOneVenueGroup. Routes
// partial-presence venues out to processPartialPresenceVenue; otherwise
// runs the standard pre-Bernoulli setup (in venue_bins.cpp) followed by
// the per-susceptible Bernoulli draws below.
// =============================================================================

int InteractionManager::processVenueTransmissions(
    const std::vector<InteractionMember>& members, Venue* venue,
    VenueId actual_venue_id, double current_time, double delta_hours,
    std::unordered_set<PersonId>* active_infections,
    const std::unordered_set<PersonId>* visitor_ids,
    std::vector<PendingInfection>* pending_infections,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    uint8_t encounter_type_id, const CompartmentalModelManager* comp_model) {
  if (auto pp = dispatchPartialPresenceIfApplicable(
          members, venue, actual_venue_id, current_time, delta_hours,
          active_infections, visitor_ids, pending_infections, visitor_data,
          encounter_type_id, comp_model)) {
    return *pp;
  }

  std::string venue_type;
  uint8_t venue_type_id = kUnknownVenueTypeId;
  const ContactMatrix* bin_structure = nullptr;
  resolveVenueTypeAndMatrix(venue, actual_venue_id, encounter_type_id,
                            venue_type, venue_type_id, bin_structure);
  const bool is_virtual_encounter = isVirtualVenue(actual_venue_id);

  // Null means the group was neither a physical venue nor a recognised
  // virtual encounter, so there is nothing to attribute contacts to. Taking
  // one bin here would quietly score everyone against contacts[0][0].
  if (!bin_structure)
    throw std::runtime_error(
        "processVenueTransmissions: no venue and no known encounter type for "
        "venue_id=" +
        std::to_string(actual_venue_id) +
        ", encounter_type_id=" + std::to_string((int)encounter_type_id));

  int num_bins_needed = static_cast<int>(bin_structure->bins.size());
  if (num_bins_needed == 0) num_bins_needed = 1;
  int num_modes = std::max(1, disease_->numModes());
  const auto& trans_params = disease_->getTransmissionParams();

  std::vector<FomiteModeRef> fomite_modes;
  std::vector<int> comp_uptake_modes;
  std::vector<int> n_sub_per_mode;
  collectFomiteAndCompUptakeModes(delta_hours, fomite_modes, comp_uptake_modes,
                                  n_sub_per_mode);
  int num_fomite_modes = static_cast<int>(fomite_modes.size());

  prepareBinsBuffer(num_bins_needed, num_modes, num_fomite_modes,
                    n_sub_per_mode);

  std::vector<double> lambda_fomite_by_mode = binMembersAndPrepareBuffers(
      members, venue, *bin_structure, num_bins_needed, num_modes,
      num_fomite_modes, fomite_modes, n_sub_per_mode, current_time, delta_hours,
      encounter_type_id, venue_type, venue_type_id, visitor_data);

  if (venueHasNoTransmissionPossible(num_bins_needed, comp_uptake_modes,
                                     lambda_fomite_by_mode, actual_venue_id,
                                     comp_model)) {
    clearUsedBins(num_modes);
    return 0;
  }

  // Sibling-mixing setup: per-mode "sibling" source representing infectious
  // people in OTHER children of the same parent.
  const ParentAggregate* parent_agg = nullptr;
  const ContactMatrix* parent_flat_matrix = nullptr;
  std::tie(parent_agg, parent_flat_matrix) =
      getParentAggregateForVenue(venue, is_virtual_encounter);

  // Per-susceptible Bernoulli draws (mixing-model FOI infection), mode-aware.
  int new_infections = 0;
  for (int susc_bin = 0; susc_bin < num_bins_needed; ++susc_bin) {
    new_infections += processOneSuscBin(
        susc_bin, num_bins_needed, num_modes, num_fomite_modes,
        is_virtual_encounter, encounter_type_id, venue_type_id, venue,
        actual_venue_id, fomite_modes, comp_uptake_modes, lambda_fomite_by_mode,
        trans_params, parent_agg, parent_flat_matrix, comp_model, current_time,
        delta_hours, visitor_data, active_infections, pending_infections);
  }

  // Clear person-proportional fields while bins are still cache-hot.
  clearUsedBins(num_modes);
  return new_infections;
}

// =============================================================================
// Helpers below: per-susc-bin orchestrator, per-susceptible Bernoulli draw,
// infector sampling, FOI-source builders, infection apply.
// =============================================================================

int InteractionManager::processOneSuscBin(
    int susc_bin, int num_bins_needed, int num_modes, int num_fomite_modes,
    bool is_virtual_encounter, uint8_t encounter_type_id, uint8_t venue_type_id,
    Venue* venue, VenueId actual_venue_id,
    const std::vector<FomiteModeRef>& fomite_modes,
    const std::vector<int>& comp_uptake_modes,
    const std::vector<double>& lambda_fomite_by_mode,
    const TransmissionParams& trans_params, const ParentAggregate* parent_agg,
    const ContactMatrix* parent_flat_matrix,
    const CompartmentalModelManager* comp_model, double current_time,
    double delta_hours,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    std::unordered_set<PersonId>* active_infections,
    std::vector<PendingInfection>* pending_infections) {
  const auto& susc_group = bins_buffer_[susc_bin];
  if (susc_group.susceptible.empty()) return 0;

  // 3a. Pre-calculate total Force of Infection across all modes and bins.
  sources_buffer_.clear();
  source_weights_buffer_.clear();
  double total_lambda_eff = 0.0;

  appendDirectContactSources(susc_bin, num_bins_needed, num_modes,
                             is_virtual_encounter, encounter_type_id,
                             venue_type_id, trans_params, total_lambda_eff);
  appendSiblingMixingSources(susc_bin, num_modes, actual_venue_id, venue,
                             parent_agg, parent_flat_matrix, trans_params,
                             total_lambda_eff);
  appendFomiteSources(num_fomite_modes, fomite_modes, lambda_fomite_by_mode,
                      trans_params, total_lambda_eff);
  appendCompUptakeSources(actual_venue_id, venue_type_id, comp_uptake_modes,
                          comp_model, trans_params, total_lambda_eff);

  double total_risk = total_lambda_eff;
  if (simulation_config_.regional_risk.enabled && venue) {
    total_risk *= venue->transmission_factor;
  }
  if (total_risk <= 0.0) return 0;

  // Build cumulative source weights once per susc_bin; each susceptible
  // samples from it via sampleFromCumulative below. Avoids the per-bin
  // std::discrete_distribution construction that dominated the 60M run.
  bool have_source_dist =
      source_weights_buffer_.size() > 1 &&
      buildCumulative(source_weights_buffer_, source_cumulative_buffer_) > 0.0;

  uint64_t time_bits = static_cast<uint64_t>(current_time * 1000);
  int new_infections = 0;
  for (const auto& susc_mem : susc_group.susceptible) {
    if (processOneVenueSusceptible(
            susc_mem, total_risk, susc_bin, have_source_dist, time_bits,
            current_time, actual_venue_id, venue, venue_type_id, parent_agg,
            visitor_data, active_infections, pending_infections)) {
      new_infections++;
    }
  }
  (void)delta_hours;  // present in signature for future DEBUG_TRANSMISSION use
  return new_infections;
}

bool InteractionManager::processOneVenueSusceptible(
    const SusceptibleMember& susc_mem, double total_risk, int susc_bin,
    bool have_source_dist, uint64_t time_bits, double current_time,
    VenueId actual_venue_id, Venue* venue, uint8_t venue_type_id,
    const ParentAggregate* parent_agg,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    std::unordered_set<PersonId>* active_infections,
    std::vector<PendingInfection>* pending_infections) {
  PersonId susceptible_id = susc_mem.id;
  double prob = 1.0 - std::exp(-total_risk * susc_mem.susceptibility);
  if (!(prob > 1e-12)) return false;

  // Per-susceptible deterministic RNG for MPI reproducibility. For a virtual
  // venue, key on the host's person_id instead, so the seed is the same
  // regardless of which rank hosts the encounter.
  uint64_t venue_key = static_cast<uint64_t>(actual_venue_id);
  if (isVirtualVenue(actual_venue_id)) {
    venue_key = static_cast<uint64_t>(virtualVenueHost(actual_venue_id));
  }
  SplitMix64 susc_rng(
      mix_seed(base_seed_, susceptible_id, venue_key, time_bits));

  double rng_roll = uniform_dist_(susc_rng);
  if (!(rng_roll < prob)) return false;

  int src_idx = have_source_dist
                    ? sampleFromCumulative(source_cumulative_buffer_, susc_rng)
                    : 0;
  if (src_idx < 0) src_idx = 0;

  InfectionSource infection_source = InfectionSource::Person;
  uint8_t transmission_mode_index = 0;
  PersonId infector_id = sampleVenueInfector(
      src_idx, susc_bin, actual_venue_id, venue, susceptible_id, parent_agg,
      susc_rng, infection_source, transmission_mode_index);
  uint16_t infector_symptom_id =
      resolveInfectorSymptomId(infector_id, current_time, visitor_data);

  applyVenueInfection(susc_mem, infector_id, infection_source,
                      transmission_mode_index, infector_symptom_id,
                      current_time, venue_type_id, actual_venue_id, venue_key,
                      visitor_data, active_infections, pending_infections);
  return true;
}

PersonId InteractionManager::sampleSiblingInfector(
    int sampled_mode, int pbin, VenueId actual_venue_id,
    const ParentAggregate& parent_agg, SplitMix64& susc_rng) {
  if (pbin < 0 || pbin >= (int)parent_agg.infectors_by_bin.size()) return -1;
  const auto& pool = parent_agg.infectors_by_bin[pbin];
  sibling_cum_buffer_.clear();
  sibling_pool_indices_buffer_.clear();
  double acc = 0.0;
  for (size_t pe_idx = 0; pe_idx < pool.size(); ++pe_idx) {
    const auto& pe = pool[pe_idx];
    if (pe.child_venue_id == actual_venue_id) continue;
    double w = (sampled_mode < (int)pe.inf_by_mode.size())
                   ? pe.inf_by_mode[sampled_mode]
                   : 0.0;
    if (w <= 0.0) continue;
    acc += w;
    sibling_cum_buffer_.push_back(acc);
    sibling_pool_indices_buffer_.push_back(pe_idx);
  }
  if (!(acc > 0.0) || sibling_cum_buffer_.empty()) return -1;
  int idx = sampleFromCumulative(sibling_cum_buffer_, susc_rng);
  if (idx < 0 || idx >= (int)sibling_pool_indices_buffer_.size()) return -1;
  return pool[sibling_pool_indices_buffer_[idx]].person_id;
}

PersonId InteractionManager::sampleVenueInfector(
    int src_idx, int susc_bin, VenueId actual_venue_id, const Venue* venue,
    PersonId susceptible_id, const ParentAggregate* parent_agg,
    SplitMix64& susc_rng, InfectionSource& infection_source_out,
    uint8_t& transmission_mode_index_out) {
  int sampled_mode = sources_buffer_[src_idx].mode;
  int sampled_inf_bin = sources_buffer_[src_idx].inf_bin;
  transmission_mode_index_out = static_cast<uint8_t>(sampled_mode);

  if (sampled_inf_bin == -2) {
    infection_source_out = InfectionSource::Compartmental;
    return -1;
  }
  if (sampled_inf_bin == -1) {
    infection_source_out = InfectionSource::Fomite;
    return -1;
  }
  if (sampled_inf_bin == SIBLING_INF_BIN_SENTINEL) {
    // Sibling-venue source: still a Person infector, but in a DIFFERENT
    // child venue under the same parent. Two-stage sample: build cumulative
    // over the parent's infector pool for this mode (excluding the
    // susceptible's own venue), then sample with susc_rng, the same RNG
    // already used for source-selection.
    infection_source_out = InfectionSource::Person;
    PersonId infector_id = -1;
    if (parent_agg) {
      int pbin = sources_buffer_[src_idx].sibling_parent_inf_bin;
      infector_id = sampleSiblingInfector(sampled_mode, pbin, actual_venue_id,
                                          *parent_agg, susc_rng);
    }
    if (debug_parent_mixing_ && dbg_sample_infection_prints_ < 20) {
      std::cerr << "[PMIX] sibling_infection susc=" << susceptible_id
                << " venue=" << actual_venue_id
                << " parent=" << (venue ? venue->parent_id : -1)
                << " mode=" << sampled_mode << " infector=" << infector_id
                << " infector_pool_size=" << sibling_pool_indices_buffer_.size()
                << " susc_bin=" << susc_bin << std::endl;
      dbg_sample_infection_prints_++;
    }
    dbg_sibling_infections_++;
    return infector_id;
  }

  // sampled_inf_bin >= 0: in-bin Person source
  infection_source_out = InfectionSource::Person;
  PersonId infector_id = -1;
  auto& inf_group = bins_buffer_[sampled_inf_bin];
  const auto& cum = inf_group.cumulative_by_mode[sampled_mode];
  if (!cum.empty() && !inf_group.infectious_ids.empty()) {
    int person_idx = sampleFromCumulative(cum, susc_rng);
    if (person_idx >= 0 && person_idx < (int)inf_group.infectious_ids.size()) {
      infector_id = inf_group.infectious_ids[person_idx];
    }
  } else if (!inf_group.infectious_ids.empty()) {
    infector_id = inf_group.infectious_ids[0];
  }
  return infector_id;
}

void InteractionManager::applyVenueInfection(
    const SusceptibleMember& susc_mem, PersonId infector_id,
    InfectionSource infection_source, uint8_t transmission_mode_index,
    uint16_t infector_symptom_id, double current_time, uint8_t venue_type_id,
    VenueId actual_venue_id, uint64_t venue_key,
    const std::unordered_map<PersonId, VisitorInfo>* /*visitor_data*/,
    std::unordered_set<PersonId>* active_infections,
    std::vector<PendingInfection>* pending_infections) {
  PersonId susceptible_id = susc_mem.id;
  const VisitorInfo* visitor = susc_mem.visitor;
  const bool is_visitor = (visitor != nullptr);

  if (is_visitor && pending_infections != nullptr) {
    // Visitor infection - queue for home rank. The home rank logs the
    // InfectionEvent after applying the pending infection, so /lookups/people
    // (built from world.people on the home rank, filtered by
    // getInfectedPersonIds()) includes the infectee.
    PendingInfection pending;
    pending.person_id = susceptible_id;
    pending.infector_id = infector_id;
    pending.infection_time = current_time;
    pending.venue_type_id = venue_type_id;
    pending.encounter_type_id = susc_mem.encounter_type_id;
    pending.venue_id = actual_venue_id;
    pending.infector_symptom_id = infector_symptom_id;
    pending.transmission_mode_index = transmission_mode_index;
    if (visitor) pending.home_array_index = visitor->home_array_index;
    pending_infections->push_back(pending);
    return;
  }

  // Local person infection - create immediately
  Person* susc_person = world_.getPerson(susceptible_id);
  if (!susc_person || susc_person->infection || disease_ == nullptr) return;

  float severity_factor = 1.0f;
  auto* gu = world_.getGeoUnit(susc_person->geo_unit_id);
  if (gu) severity_factor = gu->severity_factor;

  std::string venue_type_name;
  if (venue_type_id < world_.venue_type_names.size()) {
    venue_type_name = world_.venue_type_names[venue_type_id];
  }

  uint64_t infection_seed =
      mix_seed(base_seed_, susceptible_id,
               static_cast<uint64_t>(current_time * 1000), venue_key);
  susc_person->infection = std::make_unique<Infection>(
      disease_, current_time, susc_person,
      static_cast<unsigned int>(infection_seed), &world_, venue_type_name,
      actual_venue_id, severity_factor, infector_symptom_id, "", "",
      transmission_mode_index);

  if (event_logger_ != nullptr) {
    event_logger_->logInfection(susceptible_id, infector_id, actual_venue_id,
                                current_time, susc_mem.encounter_type_id,
                                static_cast<uint8_t>(infector_symptom_id),
                                transmission_mode_index, infection_source);
  }

  if (active_infections != nullptr) {
    active_infections->insert(susceptible_id);
  }
}

void InteractionManager::appendDirectContactSources(
    int susc_bin, int num_bins_needed, int num_modes, bool is_virtual_encounter,
    uint8_t encounter_type_id, uint8_t venue_type_id,
    const TransmissionParams& trans_params, double& total_lambda_eff) {
  for (int m = 0; m < num_modes; ++m) {
    // Get mode-specific contact matrix. Virtual encounters are keyed by
    // encounter_type_id; physical venues by venue_type_id. The split
    // matters: these are disjoint integer spaces and aliasing them pulls
    // the wrong matrix.
    const ContactMatrix& mode_matrix =
        is_virtual_encounter
            ? contact_matrices_.getVirtualMatrix(encounter_type_id, m)
            : contact_matrices_.getMatrix(venue_type_id, m);
    double mode_susc_mult =
        (m < (int)trans_params.modes.size())
            ? trans_params.modes[m].mode_transmissibility_multiplier
            : 1.0;

    for (int inf_bin = 0; inf_bin < num_bins_needed; ++inf_bin) {
      const auto& inf_group = bins_buffer_[inf_bin];
      if (inf_group.total_infectiousness_by_mode[m] <= 0.0) continue;

      double contacts =
          lookupContactsForBinPair(mode_matrix, susc_bin, inf_bin);
      if (contacts <= 0.0) continue;

      int bin_size = bins_buffer_[inf_bin].total_size;
      if (susc_bin == inf_bin) bin_size = std::max(1, bin_size - 1);

      // Force of infection: omega = C / N_bin
      // (delta_hours is already absorbed into the integrated infectiousness
      // values stored in total_infectiousness_by_mode)
      double omega = contacts / bin_size;
      double contrib = omega * inf_group.total_infectiousness_by_mode[m];
      double weighted = contrib * mode_susc_mult;

      if (weighted > 0.0) {
        total_lambda_eff += weighted;
        sources_buffer_.push_back({m, inf_bin});
        source_weights_buffer_.push_back(weighted);
      }
    }
  }
}

void InteractionManager::logSiblingFOIDump(
    int m, int susc_bin, VenueId actual_venue_id, const Venue* venue,
    const ParentAggregate& parent_agg, double contacts, double parent_inf,
    double own_inf, double sibling_inf, int parent_size, int own_size,
    int sibling_size, double omega, double weighted) {
  if (!debug_parent_mixing_ || dbg_sample_susc_prints_ >= 20) return;
  std::cerr << "[PMIX] sibling_FOI venue=" << actual_venue_id
            << " parent=" << venue->parent_id
            << " parent_type=" << (int)parent_agg.parent_venue_type_id
            << " susc_bin=" << susc_bin << " mode=" << m
            << " contacts=" << contacts << " parent_inf=" << parent_inf
            << " own_inf=" << own_inf << " sibling_inf=" << sibling_inf
            << " parent_size=" << parent_size << " own_size=" << own_size
            << " sibling_size=" << sibling_size << " omega=" << omega
            << " weighted=" << weighted << std::endl;
  dbg_sample_susc_prints_++;
}

void InteractionManager::appendSiblingMixingSources(
    int susc_bin, int num_modes, VenueId actual_venue_id, const Venue* venue,
    const ParentAggregate* parent_agg, const ContactMatrix* parent_flat_matrix,
    const TransmissionParams& trans_params, double& total_lambda_eff) {
  if (!parent_agg) return;

  const int pbin = 0;  // single-bin parent assumption (enforced earlier)
  auto cs_it = parent_agg->child_size_by_bin.find(actual_venue_id);
  auto ci_it = parent_agg->child_inf_by_bin_mode.find(actual_venue_id);
  int parent_size = (pbin < (int)parent_agg->size_by_bin.size())
                        ? parent_agg->size_by_bin[pbin]
                        : 0;
  int own_size = (cs_it != parent_agg->child_size_by_bin.end() &&
                  pbin < (int)cs_it->second.size())
                     ? cs_it->second[pbin]
                     : 0;
  int sibling_size = std::max(1, parent_size - own_size);

  for (int m = 0; m < num_modes; ++m) {
    const ContactMatrix& pmm =
        contact_matrices_.getMatrix(parent_agg->parent_venue_type_id, m);
    if (pmm.contacts.empty() || pmm.contacts[0].empty()) continue;
    double contacts = pmm.contacts[0][0];
    if (contacts <= 0.0) continue;

    double parent_inf = (pbin < (int)parent_agg->total_inf_by_bin_mode.size())
                            ? parent_agg->total_inf_by_bin_mode[pbin][m]
                            : 0.0;
    double own_inf = (ci_it != parent_agg->child_inf_by_bin_mode.end() &&
                      pbin < (int)ci_it->second.size())
                         ? ci_it->second[pbin][m]
                         : 0.0;
    double sibling_inf = parent_inf - own_inf;
    if (sibling_inf <= 0.0) continue;

    double mode_susc_mult =
        (m < (int)trans_params.modes.size())
            ? trans_params.modes[m].mode_transmissibility_multiplier
            : 1.0;
    double omega = contacts / sibling_size;
    double weighted = omega * sibling_inf * mode_susc_mult;
    if (weighted <= 0.0) continue;

    total_lambda_eff += weighted;
    SourceEntry se;
    se.mode = m;
    se.inf_bin = SIBLING_INF_BIN_SENTINEL;
    se.sibling_parent_inf_bin = pbin;
    sources_buffer_.push_back(se);
    source_weights_buffer_.push_back(weighted);

    logSiblingFOIDump(m, susc_bin, actual_venue_id, venue, *parent_agg,
                      contacts, parent_inf, own_inf, sibling_inf, parent_size,
                      own_size, sibling_size, omega, weighted);
  }
}

void InteractionManager::appendFomiteSources(
    int num_fomite_modes, const std::vector<FomiteModeRef>& fomite_modes,
    const std::vector<double>& lambda_fomite_by_mode,
    const TransmissionParams& trans_params, double& total_lambda_eff) {
  for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
    if (lambda_fomite_by_mode[local_fm] <= 0.0) continue;
    int fomite_mode_idx = fomite_modes[local_fm].mode_index;
    double mode_susc_mult = (fomite_mode_idx < (int)trans_params.modes.size())
                                ? trans_params.modes[fomite_mode_idx]
                                      .mode_transmissibility_multiplier
                                : 1.0;
    double weighted = lambda_fomite_by_mode[local_fm] * mode_susc_mult;
    total_lambda_eff += weighted;
    sources_buffer_.push_back(SourceEntry{fomite_mode_idx, -1});
    source_weights_buffer_.push_back(weighted);
  }
}

void InteractionManager::appendCompUptakeSources(
    VenueId actual_venue_id, uint8_t venue_type_id,
    const std::vector<int>& comp_uptake_modes,
    const CompartmentalModelManager* comp_model,
    const TransmissionParams& trans_params, double& total_lambda_eff) {
  if (!comp_model || comp_uptake_modes.empty()) return;
  const float* buf = comp_model->readCouplingOutputs();
  int node_idx =
      comp_model->venueToLocalNodeIndex(static_cast<int>(actual_venue_id));
  if (!buf || node_idx < 0) return;
  float foi_scale =
      comp_model->getOutputFOIScale(static_cast<int>(venue_type_id), 0);
  float node_output = buf[node_idx] * foi_scale;
  for (int mode_idx : comp_uptake_modes) {
    double mode_susc_mult =
        (mode_idx < (int)trans_params.modes.size())
            ? trans_params.modes[mode_idx].mode_transmissibility_multiplier
            : 1.0;
    double weighted = node_output * mode_susc_mult;
    if (weighted <= 0.0) continue;
    total_lambda_eff += weighted;
    sources_buffer_.push_back(SourceEntry{mode_idx, -2});
    source_weights_buffer_.push_back(weighted);
  }
}

}  // namespace june
