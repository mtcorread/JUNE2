// Standard FOI path. Contains everything processVenueTransmissions needs
// *before* the per-susceptible Bernoulli loop: matrix/venue-type resolution,
// fomite/comp-uptake mode collection, bins-buffer prep, member binning,
// deterministic sorting, per-mode cumulative weights, fomite-deposition
// lambda accumulation.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>

#include "epidemiology/interaction_manager.h"
#include "simulation/compartmental_model_manager.h"
#include "utils/random.h"

namespace june {

void InteractionManager::clearUsedBins(int num_modes) {
  for (int b : used_bins_) bins_buffer_[b].clearAfterUse(num_modes);
  used_bins_.clear();
}

std::vector<double> InteractionManager::binMembersAndPrepareBuffers(
    const std::vector<InteractionMember>& members, Venue* venue,
    const ContactMatrix& bin_structure, int num_bins_needed, int num_modes,
    int num_fomite_modes, const std::vector<FomiteModeRef>& fomite_modes,
    const std::vector<int>& n_sub_per_mode, double current_time,
    double delta_hours, uint8_t encounter_type_id,
    const std::string& venue_type, uint8_t venue_type_id,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data) {
  // Pass 1: bin each member by contact-matrix row.
  for (const auto& member : members) {
    binOneMember(member, venue, bin_structure, num_bins_needed, num_modes,
                 num_fomite_modes, fomite_modes, n_sub_per_mode, current_time,
                 delta_hours, encounter_type_id, venue_type, venue_type_id,
                 visitor_data);
  }

  // Sort infectious lists by person_id (MPI determinism: identical iteration
  // order regardless of how members were partitioned across ranks).
  sortInfectiousByPersonId(num_bins_needed, num_modes);

  // Sort susceptibles by person_id (deterministic per-susc-bin loop order).
  sortSusceptiblesByPersonId(num_bins_needed);

  // Per-mode cumulative weight arrays for infectious bins (later sampled via
  // sampleFromCumulative in the Bernoulli loop, replacing per-call
  // std::discrete_distribution construction).
  buildCumulativeWeightsPerBin(num_bins_needed, num_modes);

  // Fomite deposition + per-mode lambda accumulation.
  return recordFomiteDepositionAndLambda(
      venue, num_bins_needed, num_fomite_modes, fomite_modes, n_sub_per_mode,
      current_time, delta_hours);
}

bool InteractionManager::venueHasNoTransmissionPossible(
    int num_bins_needed, const std::vector<int>& comp_uptake_modes,
    const std::vector<double>& lambda_fomite_by_mode, VenueId actual_venue_id,
    const CompartmentalModelManager* comp_model) const {
  bool has_infectious = false;
  bool has_susceptible = false;
  for (int b = 0; b < num_bins_needed; ++b) {
    if (!bins_buffer_[b].infectious_ids.empty()) has_infectious = true;
    if (!bins_buffer_[b].susceptible.empty()) has_susceptible = true;
  }
  double total_lambda_fomite = 0.0;
  for (double lf : lambda_fomite_by_mode) total_lambda_fomite += lf;

  bool has_comp_uptake_potential =
      !comp_uptake_modes.empty() && comp_model != nullptr &&
      comp_model->venueToLocalNodeIndex(static_cast<int>(actual_venue_id)) >= 0;

  return !has_susceptible || (!has_infectious && total_lambda_fomite <= 0.0 &&
                              !has_comp_uptake_potential);
}

std::pair<const ParentAggregate*, const ContactMatrix*>
InteractionManager::getParentAggregateForVenue(
    const Venue* venue, bool is_virtual_encounter) const {
  if (!venue || venue->parent_id < 0 || is_virtual_encounter)
    return {nullptr, nullptr};
  auto pit = parent_aggregates_.find(venue->parent_id);
  if (pit == parent_aggregates_.end()) return {nullptr, nullptr};
  const ContactMatrix* parent_flat_matrix =
      &contact_matrices_.getBinStructure(pit->second.parent_venue_type_id);
  // V1 supports single-bin parent matrices only. The currently shipped
  // YAML (school/company/university) all use `bins: [all]`. A multi-
  // bin parent matrix would require per-susceptible parent_bin lookup
  // in the hot path; defer to V2. Refuse loudly per the no-silent-
  // fallbacks rule rather than silently mis-applying contacts[0][0].
  if (parent_flat_matrix->bins.size() > 1) {
    throw std::runtime_error(
        "parent_mixing: multi-bin parent contact matrices are not "
        "supported yet; parent venue type id=" +
        std::to_string(pit->second.parent_venue_type_id) + " has " +
        std::to_string(parent_flat_matrix->bins.size()) + " bins");
  }
  return {&pit->second, parent_flat_matrix};
}

std::vector<double> InteractionManager::recordFomiteDepositionAndLambda(
    Venue* venue, int num_bins_needed, int num_fomite_modes,
    const std::vector<FomiteModeRef>& fomite_modes,
    const std::vector<int>& n_sub_per_mode, double current_time,
    double delta_hours) {
  std::vector<double> lambda_fomite_by_mode(num_fomite_modes, 0.0);
  if (num_fomite_modes == 0 || !venue) return lambda_fomite_by_mode;

  for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
    const FomiteConfig& fcfg = *fomite_modes[local_fm].cfg;
    auto& history = venue->fomite_history;
    if (local_fm >= (int)history.size()) continue;

    // Sum deposition per sub-bin and append one history entry per sub-bin
    int n_sub = n_sub_per_mode[local_fm];
    double dt_sub = delta_hours / n_sub / 24.0;
    for (int k = 0; k < n_sub; ++k) {
      double amount_k = 0.0;
      for (int b = 0; b < num_bins_needed; ++b)
        amount_k += bins_buffer_[b].total_fomite_deposition_sub[local_fm][k];
      if (amount_k > 0.0) {
        double tau_k = current_time + (k + 0.5) * dt_sub;
        history[local_fm].push_back({tau_k, amount_k});
      }
    }

    // lambda = sum_k amount_k * ∫_{age_k}^{age_k+Δ} Q(a) da
    if (fcfg.infectiousness_curve) {
      const double delta_days = delta_hours / 24.0;
      for (const auto& event : history[local_fm]) {
        double age = current_time - event.time;
        lambda_fomite_by_mode[local_fm] +=
            event.amount *
            fcfg.infectiousness_curve->integrate(age, age + delta_days) / 24.0;
      }
    }
  }
  return lambda_fomite_by_mode;
}

void InteractionManager::sortInfectiousByPersonId(int num_bins_needed,
                                                  int num_modes) {
  for (int b = 0; b < num_bins_needed; ++b) {
    auto& group = bins_buffer_[b];
    size_t n = group.infectious_ids.size();
    if (n <= 1) continue;

    // Build index permutation sorted by person_id
    std::vector<size_t> perm(n);
    std::iota(perm.begin(), perm.end(), 0);
    std::sort(perm.begin(), perm.end(), [&](size_t a, size_t b_idx) {
      return group.infectious_ids[a] < group.infectious_ids[b_idx];
    });

    // Apply permutation to infectious_ids and each mode's infectiousness
    std::vector<PersonId> sorted_ids(n);
    for (size_t i = 0; i < n; ++i)
      sorted_ids[i] = group.infectious_ids[perm[i]];
    group.infectious_ids = std::move(sorted_ids);

    for (int m = 0; m < num_modes; ++m) {
      auto& inf_m = group.infectiousness_by_mode[m];
      if (inf_m.size() != n) continue;
      std::vector<double> sorted_inf(n);
      for (size_t i = 0; i < n; ++i) sorted_inf[i] = inf_m[perm[i]];
      inf_m = std::move(sorted_inf);
    }
  }
}

void InteractionManager::sortSusceptiblesByPersonId(int num_bins_needed) {
  for (int b = 0; b < num_bins_needed; ++b) {
    auto& susc = bins_buffer_[b].susceptible;
    if (susc.size() > 1) {
      std::sort(susc.begin(), susc.end(),
                [](const SusceptibleMember& a, const SusceptibleMember& b) {
                  return a.id < b.id;
                });
    }
  }
}

void InteractionManager::buildCumulativeWeightsPerBin(int num_bins_needed,
                                                      int num_modes) {
  for (int b = 0; b < num_bins_needed; ++b) {
    auto& group = bins_buffer_[b];
    if (group.infectious_ids.empty()) continue;

    for (int m = 0; m < num_modes; ++m) {
      const auto& w = group.infectiousness_by_mode[m];
      double total = buildCumulative(w, group.cumulative_by_mode[m]);
      if (!(total > 0.0)) {
        // No positive weight; clear so callers know to skip sampling.
        group.cumulative_by_mode[m].clear();
      }
    }
  }
}

void InteractionManager::binMemberClassification(
    const InteractionMember& member, Person* person, const VisitorInfo* visitor,
    int bin_index, int num_modes, int num_fomite_modes,
    const std::vector<FomiteModeRef>& fomite_modes,
    const std::vector<int>& n_sub_per_mode, double current_time,
    double delta_hours) {
  PersonId pid = member.id;
  if (visitor) {
    if (visitor->is_infectious) {
      accumulateVisitorInfectiousnessAndFomite(
          visitor, pid, bin_index, num_modes, num_fomite_modes, fomite_modes,
          n_sub_per_mode, delta_hours);
    } else if (!visitor->is_infected && visitor->immunity_level < 1.0) {
      double susceptibility = 1.0 - visitor->immunity_level;
      bins_buffer_[bin_index].susceptible.push_back(
          {pid, susceptibility, visitor, member.encounter_type_id});
    }
    return;
  }
  if (!person || person->is_dead) return;

  if (person->infection && person->infection->isInfectious(current_time)) {
    accumulateLocalInfectiousness(person, pid, bin_index, num_modes,
                                  current_time, delta_hours);
  } else if (!person->infection) {
    double susceptibility =
        person->getSusceptibility(current_time, disease_->getName());
    if (susceptibility > 0.0) {
      bins_buffer_[bin_index].susceptible.push_back(
          {pid, susceptibility, /*visitor=*/nullptr, member.encounter_type_id});
    }
  }
  if (person->infection && num_fomite_modes > 0) {
    accumulateLocalFomiteDeposition(person, bin_index, num_fomite_modes,
                                    fomite_modes, n_sub_per_mode, current_time,
                                    delta_hours);
  }
}

int InteractionManager::resolveMemberBinIndex(
    const InteractionMember& member, const Person* person, Venue* venue,
    const ContactMatrix& bin_structure, int num_bins_needed,
    uint8_t encounter_type_id, [[maybe_unused]] const std::string& venue_type,
    [[maybe_unused]] uint8_t venue_type_id) {
  stats_.bin_lookups++;
  int bin_index = computeBinIndexForMatrix(person, venue, member.subset_index,
                                           &bin_structure, num_bins_needed);
  if (bin_index >= 0 && bin_index < num_bins_needed) return bin_index;

#ifdef DEBUG_TRANSMISSION
  static int bin_fallback_count = 0;
  if (bin_fallback_count < 20) {
    std::cerr << "[DEBUG_TRANSMISSION] bin_index fallback to 0: "
              << "person=" << member.id
              << " age=" << (person ? (int)person->age : -1)
              << " sex=" << (person ? (int)person->sex : -1)
              << " venue_type=" << venue_type
              << " venue_type_id=" << (int)venue_type_id
              << " num_bins=" << num_bins_needed
              << " original_bin=" << bin_index
              << " encounter_type=" << (int)encounter_type_id << std::endl;
    bin_fallback_count++;
    if (bin_fallback_count == 20)
      std::cerr << "[DEBUG_TRANSMISSION] (suppressing further bin_index "
                   "fallback warnings)"
                << std::endl;
  }
#endif
  return 0;
}

void InteractionManager::binOneMember(
    const InteractionMember& member, Venue* venue,
    const ContactMatrix& bin_structure, int num_bins_needed, int num_modes,
    int num_fomite_modes, const std::vector<FomiteModeRef>& fomite_modes,
    const std::vector<int>& n_sub_per_mode, double current_time,
    double delta_hours, uint8_t encounter_type_id,
    const std::string& venue_type, uint8_t venue_type_id,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data) {
  PersonId pid = member.id;
  Person* person = nullptr;
  if (member.array_index < world_.people.size()) {
    person = &world_.people[member.array_index];
    if (person->id != pid) person = world_.getPerson(pid);
  } else {
    person = world_.getPerson(pid);
  }

  int bin_index = resolveMemberBinIndex(member, person, venue, bin_structure,
                                        num_bins_needed, encounter_type_id,
                                        venue_type, venue_type_id);

  // Track which bins are used (for selective clearing next call)
  if (bins_buffer_[bin_index].total_size == 0) {
    used_bins_.push_back(bin_index);
  }
  // Track total bin size (for frequency-dependent transmission denominator)
  if (!person || !person->is_dead) {
    bins_buffer_[bin_index].total_size++;
  }

  const VisitorInfo* visitor = nullptr;
  if (!person && visitor_data != nullptr) {
    auto it = visitor_data->find(pid);
    if (it != visitor_data->end()) visitor = &it->second;
  }
  binMemberClassification(member, person, visitor, bin_index, num_modes,
                          num_fomite_modes, fomite_modes, n_sub_per_mode,
                          current_time, delta_hours);
}

void InteractionManager::accumulateVisitorInfectiousnessAndFomite(
    const VisitorInfo* visitor, PersonId pid, int bin_index, int num_modes,
    int num_fomite_modes, const std::vector<FomiteModeRef>& fomite_modes,
    const std::vector<int>& n_sub_per_mode, double delta_hours) {
  // Use pre-computed integrated infectiousness from the sending rank.
  // These values were computed using the identical code path as local
  // people (Infection::getIntegratedInfectiousness), guaranteeing
  // bit-identical FP results regardless of local vs visitor status.
  im_scratch_buffer_.assign(num_modes, 0.0);
  double total_im_visitor = 0.0;
  for (int m = 0; m < num_modes; ++m) {
    im_scratch_buffer_[m] = (m < VisitorInfo::MAX_MODES)
                                ? visitor->integrated_infectiousness[m]
                                : 0.0;
    total_im_visitor += im_scratch_buffer_[m];
  }
  if (total_im_visitor > 0.0) {
    bins_buffer_[bin_index].infectious_ids.push_back(pid);
    for (int m = 0; m < num_modes; ++m) {
      bins_buffer_[bin_index].infectiousness_by_mode[m].push_back(
          im_scratch_buffer_[m]);
      bins_buffer_[bin_index].total_infectiousness_by_mode[m] +=
          im_scratch_buffer_[m];
    }
  }
  // Fomite deposition for visitors (per temporal sub-bin)
  for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
    int n_sub = n_sub_per_mode[local_fm];
    double dt_sub_stage = delta_hours / n_sub / 24.0;
    for (int k = 0; k < n_sub; ++k) {
      double t_stage_k_s = visitor->time_in_stage + k * dt_sub_stage;
      double t_stage_k_e = visitor->time_in_stage + (k + 1) * dt_sub_stage;
      double dep_k = disease_->integrateFomiteDeposition(
          fomite_modes[local_fm].mode_index, visitor->symptom_id, t_stage_k_s,
          t_stage_k_e);
      if (dep_k > 0.0)
        bins_buffer_[bin_index].total_fomite_deposition_sub[local_fm][k] +=
            dep_k;
    }
  }
}

void InteractionManager::accumulateLocalInfectiousness(
    const Person* person, PersonId pid, int bin_index, int num_modes,
    double current_time, double delta_hours) {
  const double t1 = current_time + delta_hours / 24.0;
  // Compute per-mode integrated infectiousness (hour-units: 24*∫I dt)
  im_scratch_buffer_.resize(num_modes);
  double infectiousness_total = 0.0;
  for (int m = 0; m < num_modes; ++m) {
    im_scratch_buffer_[m] =
        person->infection->getIntegratedInfectiousness(m, current_time, t1);
    infectiousness_total += im_scratch_buffer_[m];
  }
  if (infectiousness_total > 0.0) {
    bins_buffer_[bin_index].infectious_ids.push_back(pid);
    for (int m = 0; m < num_modes; ++m) {
      bins_buffer_[bin_index].infectiousness_by_mode[m].push_back(
          im_scratch_buffer_[m]);
      bins_buffer_[bin_index].total_infectiousness_by_mode[m] +=
          im_scratch_buffer_[m];
    }
  }
}

void InteractionManager::accumulateLocalFomiteDeposition(
    const Person* person, int bin_index, int num_fomite_modes,
    const std::vector<FomiteModeRef>& fomite_modes,
    const std::vector<int>& n_sub_per_mode, double current_time,
    double delta_hours) {
  const double t1 = current_time + delta_hours / 24.0;
  for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
    int n_sub = n_sub_per_mode[local_fm];
    double dt_sub = (t1 - current_time) / n_sub;
    for (int k = 0; k < n_sub; ++k) {
      double t_sub_s = current_time + k * dt_sub;
      double t_sub_e = current_time + (k + 1) * dt_sub;
      double dep_k = person->infection->getIntegratedFomiteDeposition(
          fomite_modes[local_fm].mode_index, t_sub_s, t_sub_e);
      if (dep_k > 0.0)
        bins_buffer_[bin_index].total_fomite_deposition_sub[local_fm][k] +=
            dep_k;
    }
  }
}

void InteractionManager::prepareBinsBuffer(
    int num_bins_needed, int num_modes, int num_fomite_modes,
    const std::vector<int>& n_sub_per_mode) {
  if (static_cast<int>(bins_buffer_.size()) < num_bins_needed) {
    bins_buffer_.resize(num_bins_needed);
  }
  for (int b = 0; b < num_bins_needed; ++b) {
    auto& bin = bins_buffer_[b];
    if (static_cast<int>(bin.infectiousness_by_mode.size()) != num_modes) {
      bin.clearAfterUse(num_modes);
    }
    bin.initFomiteSubBins(num_fomite_modes, n_sub_per_mode);
  }
}

void InteractionManager::collectFomiteAndCompUptakeModes(
    double delta_hours, std::vector<FomiteModeRef>& fomite_modes_out,
    std::vector<int>& comp_uptake_modes_out,
    std::vector<int>& n_sub_per_mode_out) const {
  fomite_modes_out.clear();
  comp_uptake_modes_out.clear();
  const auto& trans_params = disease_->getTransmissionParams();
  for (int midx = 0; midx < (int)trans_params.modes.size(); ++midx) {
    const auto& tmode = trans_params.modes[midx];
    if (tmode.type == TransmissionModeType::Fomite) {
      fomite_modes_out.push_back(
          FomiteModeRef{midx, &std::get<FomiteConfig>(tmode.config)});
    } else if (tmode.type == TransmissionModeType::CompartmentalUptake) {
      comp_uptake_modes_out.push_back(midx);
    }
  }
  const int num_fomite_modes = static_cast<int>(fomite_modes_out.size());
  n_sub_per_mode_out.assign(num_fomite_modes, 1);
  for (int local_fm = 0; local_fm < num_fomite_modes; ++local_fm) {
    double sbt = fomite_modes_out[local_fm].cfg->sub_bin_time;
    n_sub_per_mode_out[local_fm] =
        (sbt > 0.0) ? std::max(1, (int)(delta_hours / sbt)) : 1;
  }
}

void InteractionManager::resolveVenueTypeAndMatrix(
    Venue* venue, VenueId actual_venue_id, uint8_t encounter_type_id,
    std::string& venue_type_out, uint8_t& venue_type_id_out,
    const ContactMatrix*& matrix_out) {
  venue_type_out = "unknown";
  venue_type_id_out = kUnknownVenueTypeId;
  matrix_out = nullptr;

  // For VIRTUAL encounters (venue_id < 0), use the encounter type's contact
  // matrix; these are purpose-built venues with no physical type.
  // Virtual-encounter matrices are keyed by encounter_type_id via
  // virtual_matrices_by_encounter_id, NOT by venue_type_id; aliasing the
  // latter produced a silent bug where every sexual-channel transmission
  // pulled contact rates from whatever unrelated venue happened to sit at
  // that integer slot.
  //
  // For PHYSICAL venues (venue_id >= 0), always use the venue's own type
  // for contact matrix selection, even if some members are encounter
  // participants. This is critical for MPI reproducibility: the encounter
  // group at a physical venue is mixed with regular patrons, and the
  // encounter_type_id of the "first" person in the group is arbitrary
  // (depends on person order, which varies with rank count).
  const bool is_virtual_encounter = isVirtualVenue(actual_venue_id);
  if (is_virtual_encounter &&
      encounter_type_id < world_.encounter_type_names.size()) {
    venue_type_id_out = encounter_type_id;
    auto vm_it = contact_matrices_.virtual_matrix_names.find(encounter_type_id);
    if (vm_it != contact_matrices_.virtual_matrix_names.end()) {
      venue_type_out = vm_it->second;
    } else {
      venue_type_out = world_.encounter_type_names[encounter_type_id];
    }
    matrix_out = &contact_matrices_.getVirtualBinStructure(encounter_type_id);
  } else if (venue) {
    venue_type_id_out = venue->type_id;
    if (venue_type_id_out < world_.venue_type_names.size()) {
      venue_type_out = world_.venue_type_names[venue_type_id_out];
    }
    stats_.matrix_lookups++;
    matrix_out = &contact_matrices_.getBinStructure(venue_type_id_out);
  }
}

}  // namespace june
