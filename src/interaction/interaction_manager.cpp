// #define DEBUG_INTERACTION_MANAGER
// #define DEBUG_TRANSMISSION
//
// =============================================================================
// InteractionManager: file roadmap
// =============================================================================
// The class is split across four translation units. All methods are members
// of InteractionManager; the split is by *intent*, not by call order, so the
// per-tick narrative bounces between files. Use this map to navigate.
//
//   interaction_manager.cpp                : this file
//       Constructor, per-day driver, parent-aggregate machinery, low-level
//       cross-file primitives (binning math, person/visitor lookup,
//       infectiousness gather, symptom-id resolve).
//       Entry: processTransmissions
//                ├─ filterAndSortActiveLocations
//                ├─ buildParentAggregates              (parent-mixing only)
//                └─ per venue group: processOneVenueGroup
//                                      └─ processVenueTransmissions  [→ venue]
//
//   interaction_manager_venue.cpp          : standard FOI path
//       processVenueTransmissions orchestrator + the per-susceptible
//       Bernoulli-draw / infector-sample / infection-apply pipeline.
//       Calls into venue_bins.cpp for setup, then runs the susc-bin loop here.
//
//   interaction_manager_venue_bins.cpp     : pre-Bernoulli setup
//       Everything processVenueTransmissions needs *before* the susc-bin
//       loop: matrix/venue-type resolve, fomite/comp-uptake mode collection,
//       bins-buffer prep, member binning, deterministic sorting, per-mode
//       cumulative weights, fomite-deposition lambda.
//
//   interaction_manager_partial_presence.cpp : partial-presence FOI path
//       The alternate FOI pipeline for venues with partial presence
//       (commute lines etc.). dispatchPartialPresenceIfApplicable
//       (called from processVenueTransmissions) routes here when the
//       venue type is in SimulationConfig::partial_presence; otherwise
//       the standard path above is used.
//
// Per-tick call flow (standard path):
//   processTransmissions  → processOneVenueGroup
//     → processVenueTransmissions          [venue.cpp]
//         → dispatchPartialPresenceIfApplicable  [partial_presence.cpp]
//             └─ if matched: processPartialPresenceVenue → return
//         → resolveVenueTypeAndMatrix       [venue_bins.cpp]
//         → collectFomiteAndCompUptakeModes [venue_bins.cpp]
//         → prepareBinsBuffer               [venue_bins.cpp]
//         → binMembersAndPrepareBuffers     [venue_bins.cpp]
//         → venueHasNoTransmissionPossible  [venue_bins.cpp]
//         → susc-bin loop: processOneSuscBin → applyVenueInfection
//                                             [both venue.cpp]
// =============================================================================
#include "epidemiology/interaction_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <stdexcept>

#include "activity/presence_window.h"
#include "activity/runtime_group_allocator.h"
#include "simulation/compartmental_model_manager.h"
#include "utils/deterministic_rng.h"
#include "utils/event_logging/event_types.h"
#include "utils/profiler.h"
#include "utils/random.h"

namespace june {

InteractionManager::InteractionManager(
    WorldState& world, const ContactMatrixConfig& contact_matrices,
    const SimulationConfig& simulation_config,
    const ParallelConfig& parallel_config, Disease* disease,
    EventLogger* event_logger)
    : world_(world),
      contact_matrices_(contact_matrices),
      simulation_config_(simulation_config),
      parallel_config_(parallel_config),
      disease_(disease),
      event_logger_(event_logger),
      base_seed_(simulation_config.random_seed) {
  // Refuse here rather than at the first lookup. An unresolved lookup throws
  // mid-slot, and under MPI a rank that throws inside a collective leaves the
  // others waiting on it forever, so the run hangs instead of failing. Every
  // rank reaches this constructor, so every rank refuses together.
  if (!contact_matrices_.isResolved()) {
    throw std::runtime_error(
        "InteractionManager: contact matrices have not been resolved. Call "
        "finalizeResolvedMatrices (Simulator does this at startup; tests can "
        "use the finalizeContactMatrices helper) before computing "
        "transmission.");
  }
  if (const char* dbg = std::getenv("JUNE_DEBUG_PARENT_MIXING")) {
    debug_parent_mixing_ = (dbg[0] != '\0' && dbg[0] != '0');
  }
}

int InteractionManager::computeBinIndexForMatrix(const Person* person,
                                                 const Venue* venue,
                                                 SubsetIndex subset_index,
                                                 const ContactMatrix* matrix,
                                                 int num_bins) const {
  if (!matrix || num_bins <= 1) return 0;

  int bin_index = -1;

  // Stage 2: subset-based binning. The parent matrix may have NO entries
  // for the child's subset_type_names (e.g. school matrix has bins=[all]
  // while classroom subsets are student/worker). In that case
  // bin_by_subset_type[...] is -1 and we fall through to stage 3.
  if (bin_index < 0 && venue && subset_index >= 0) {
    auto subsets = world_.getSubsets(*venue);
    if (subset_index < (int)subsets.size()) {
      const auto& subset = subsets[subset_index];
      if (subset.subset_type_id < matrix->bin_by_subset_type.size()) {
        bin_index = matrix->bin_by_subset_type[subset.subset_type_id];
      }
    }
  }

  // Stage 3: property-based (sex first, then age).
  if (bin_index < 0 && person) {
    int sex_bin = (person->sex == Sex::MALE)     ? matrix->male_bin
                  : (person->sex == Sex::FEMALE) ? matrix->female_bin
                                                 : -1;
    if (sex_bin >= 0) {
      bin_index = sex_bin;
    } else {
      int age_int = std::min(static_cast<int>(person->age), 99);
      if (age_int >= 0) bin_index = matrix->age_to_bin[age_int];
    }
  }

  if (bin_index < 0 || bin_index >= num_bins) bin_index = 0;
  return bin_index;
}

void InteractionManager::buildParentAggregates(
    double current_time, double delta_hours,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data) {
  parent_aggregates_.clear();

  if (active_locations_buffer_.empty()) return;

  int num_modes = disease_->numModes();
  if (num_modes == 0) num_modes = 1;

  // Walk venue groups in the SAME order the main loop uses, so debug
  // output and any FP accumulations line up.
  size_t i = 0;
  while (i < active_locations_buffer_.size()) {
    size_t group_start = i;
    const auto& first = active_locations_buffer_[group_start];
    while (
        i < active_locations_buffer_.size() &&
        active_locations_buffer_[i].venue_id == first.venue_id &&
        (first.venue_id >= 0 || active_locations_buffer_[i].encounter_type_id ==
                                    first.encounter_type_id)) {
      i++;
    }

    aggregateOneVenueGroupForParent(group_start, i, current_time, delta_hours,
                                    num_modes, visitor_data);
  }

  dumpParentAggregatesDebug(current_time, delta_hours);
}

bool InteractionManager::resolvePersonAndVisitor(
    PersonId pid, size_t array_index,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    Person*& person_out, const VisitorInfo*& visitor_out) const {
  person_out = nullptr;
  visitor_out = nullptr;
  if (array_index < world_.people.size()) {
    person_out = &world_.people[array_index];
    if (person_out->id != pid) person_out = world_.getPerson(pid);
  } else {
    person_out = world_.getPerson(pid);
  }
  if (!person_out && visitor_data) {
    auto it = visitor_data->find(pid);
    if (it != visitor_data->end()) visitor_out = &it->second;
  }
  return person_out != nullptr || visitor_out != nullptr;
}

std::vector<PersonLocation> InteractionManager::buildPersonIdSortedMembers(
    size_t group_start, size_t group_end) const {
  std::vector<PersonLocation> mem_sorted;
  mem_sorted.reserve(group_end - group_start);
  for (size_t k = group_start; k < group_end; ++k) {
    mem_sorted.push_back(active_locations_buffer_[k]);
  }
  std::sort(mem_sorted.begin(), mem_sorted.end(),
            [](const PersonLocation& a, const PersonLocation& b) {
              return a.person_id < b.person_id;
            });
  return mem_sorted;
}

bool InteractionManager::gatherMemberInfectiousnessByMode(
    const Person* person, const VisitorInfo* visitor, double current_time,
    double delta_hours, int num_modes, std::vector<double>& inf_by_mode) const {
  inf_by_mode.assign(num_modes, 0.0);
  double total = 0.0;
  if (visitor) {
    if (!visitor->is_infectious) return false;
    for (int m = 0; m < num_modes; ++m) {
      inf_by_mode[m] = (m < VisitorInfo::MAX_MODES)
                           ? visitor->integrated_infectiousness[m]
                           : 0.0;
      total += inf_by_mode[m];
    }
  } else if (person && person->infection &&
             person->infection->isInfectious(current_time)) {
    const double t1 = current_time + delta_hours / 24.0;
    for (int m = 0; m < num_modes; ++m) {
      inf_by_mode[m] =
          person->infection->getIntegratedInfectiousness(m, current_time, t1);
      total += inf_by_mode[m];
    }
  } else {
    return false;
  }
  return total > 0.0;
}

ParentAggregate& InteractionManager::ensureParentAggregateInitialised(
    VenueId parent_id, VenueId child_venue_id, uint8_t parent_type_id,
    int parent_num_bins, int num_modes) {
  auto& agg = parent_aggregates_[parent_id];
  if (agg.total_inf_by_bin_mode.empty()) {
    agg.total_inf_by_bin_mode.assign(parent_num_bins,
                                     std::vector<double>(num_modes, 0.0));
    agg.size_by_bin.assign(parent_num_bins, 0);
    agg.infectors_by_bin.assign(parent_num_bins, {});
    agg.parent_venue_type_id = parent_type_id;
  }

  auto& csize = agg.child_size_by_bin[child_venue_id];
  if (csize.empty()) csize.assign(parent_num_bins, 0);
  auto& cinf = agg.child_inf_by_bin_mode[child_venue_id];
  if (cinf.empty()) {
    cinf.assign(parent_num_bins, std::vector<double>(num_modes, 0.0));
  }
  return agg;
}

void InteractionManager::aggregateOneVenueGroupForParent(
    size_t group_start, size_t group_end, double current_time,
    double delta_hours, int num_modes,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data) {
  const auto& first = active_locations_buffer_[group_start];

  // A group at no Venue has no parent Venue either.
  if (occupiesNoVenue(first.venue_id)) return;

  Venue* venue = world_.getVenue(first.venue_id);
  if (!venue) return;
  if (venue->parent_id < 0) return;

  Venue* parent_venue = world_.getVenue(venue->parent_id);
  if (!parent_venue) return;
  uint8_t parent_type_id = parent_venue->type_id;
  const ContactMatrix& parent_bin_structure =
      contact_matrices_.getBinStructure(parent_type_id);
  int parent_num_bins =
      std::max(1, static_cast<int>(parent_bin_structure.bins.size()));

  ParentAggregate& agg = ensureParentAggregateInitialised(
      venue->parent_id, first.venue_id, parent_type_id, parent_num_bins,
      num_modes);
  auto& csize = agg.child_size_by_bin[first.venue_id];
  auto& cinf = agg.child_inf_by_bin_mode[first.venue_id];

  // Walk members of this venue group in person_id order, the same ordering
  // binMembersAndPrepareBuffers uses, so the sibling FP sum order is
  // identical across rank counts. (The active_locations_buffer_ is sorted
  // by venue_id but not by person_id within a venue.)
  std::vector<PersonLocation> mem_sorted =
      buildPersonIdSortedMembers(group_start, group_end);

  std::vector<double> inf_by_mode;
  for (const auto& loc : mem_sorted) {
    accumulateOneMemberIntoParent(loc, venue, &parent_bin_structure,
                                  parent_num_bins, agg, csize, cinf,
                                  first.venue_id, current_time, delta_hours,
                                  num_modes, visitor_data, inf_by_mode);
  }
}

void InteractionManager::accumulateOneMemberIntoParent(
    const PersonLocation& loc, Venue* venue, const ContactMatrix* parent_matrix,
    int parent_num_bins, ParentAggregate& agg, std::vector<int>& csize,
    std::vector<std::vector<double>>& cinf, VenueId child_venue_id,
    double current_time, double delta_hours, int num_modes,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    std::vector<double>& inf_by_mode_scratch) const {
  PersonId pid = loc.person_id;
  Person* person = nullptr;
  const VisitorInfo* visitor = nullptr;
  if (!resolvePersonAndVisitor(pid, loc.person_array_index, visitor_data,
                               person, visitor)) {
    return;
  }
  if (person && person->is_dead) return;

  int parent_bin = computeBinIndexForMatrix(person, venue, loc.subset_index,
                                            parent_matrix, parent_num_bins);

  // Headcount (matches BinGroup::total_size convention)
  agg.size_by_bin[parent_bin]++;
  csize[parent_bin]++;

  if (gatherMemberInfectiousnessByMode(person, visitor, current_time,
                                       delta_hours, num_modes,
                                       inf_by_mode_scratch)) {
    for (int m = 0; m < num_modes; ++m) {
      agg.total_inf_by_bin_mode[parent_bin][m] += inf_by_mode_scratch[m];
      cinf[parent_bin][m] += inf_by_mode_scratch[m];
    }
    ParentInfectorEntry entry;
    entry.person_id = pid;
    entry.child_venue_id = child_venue_id;
    entry.inf_by_mode = std::move(inf_by_mode_scratch);
    agg.infectors_by_bin[parent_bin].push_back(std::move(entry));
  }
}

void InteractionManager::dumpParentAggregatesDebug(double current_time,
                                                   double delta_hours) const {
  if (!debug_parent_mixing_) return;

  // Summary: per-parent totals. Sort keys for deterministic print order.
  std::vector<VenueId> keys;
  keys.reserve(parent_aggregates_.size());
  for (const auto& kv : parent_aggregates_) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());

  int nonzero = 0;
  for (VenueId pid : keys) {
    const auto& a = parent_aggregates_.at(pid);
    double total_inf = 0.0;
    for (const auto& by_mode : a.total_inf_by_bin_mode)
      for (double v : by_mode) total_inf += v;
    if (total_inf > 0.0) nonzero++;
  }
  std::cerr << "[PMIX] t=" << current_time << " dt=" << delta_hours
            << " n_parents=" << parent_aggregates_.size()
            << " n_parents_with_infectious=" << nonzero << std::endl;

  // Print details of up to 3 parents with non-zero infectiousness
  int printed = 0;
  for (VenueId pid : keys) {
    if (printed >= 3) break;
    const auto& a = parent_aggregates_.at(pid);
    double total_inf = 0.0;
    for (const auto& v : a.total_inf_by_bin_mode)
      for (double x : v) total_inf += x;
    if (total_inf <= 0.0) continue;
    std::cerr << "[PMIX]   parent_venue=" << pid
              << " parent_type=" << (int)a.parent_venue_type_id
              << " n_children=" << a.child_size_by_bin.size();
    for (int b = 0; b < (int)a.total_inf_by_bin_mode.size(); ++b) {
      std::cerr << " bin[" << b << "](size=" << a.size_by_bin[b]
                << ",inf=" << a.infectors_by_bin[b].size() << ",I=[";
      for (int m = 0; m < (int)a.total_inf_by_bin_mode[b].size(); ++m) {
        if (m) std::cerr << ",";
        std::cerr << a.total_inf_by_bin_mode[b][m];
      }
      std::cerr << "])";
    }
    std::cerr << std::endl;
    printed++;
  }
}

void InteractionManager::filterAndSortActiveLocations(
    const std::vector<PersonLocation>& locations) {
  active_locations_buffer_.clear();
  active_locations_buffer_.reserve(locations.size());
  for (const auto& loc : locations) {
    // A line takes its members from the allocator's rider table, not from
    // here, because one location cannot express a journey across four of
    // them. Leaving these in would count every rider twice.
    if (runtime_group_allocator_ &&
        runtime_group_allocator_->isPartialPresenceVenue(loc.venue_id))
      continue;
    if (loc.venue_id != -1 ||
        loc.encounter_type_id != kDefaultEncounterTypeId) {
      active_locations_buffer_.push_back(loc);
    }
  }

  if (active_locations_buffer_.empty()) return;

  stats_.grouping_ops++;
  std::sort(active_locations_buffer_.begin(), active_locations_buffer_.end(),
            [](const PersonLocation& a, const PersonLocation& b) {
              if (a.venue_id != b.venue_id) return a.venue_id < b.venue_id;
              // Sub-sort by encounter_type_id only where there is no Venue.
              // occupiesNoVenue, not isVirtualVenue: this comparator is
              // reproducibility-critical, so it keeps its exact current
              // semantics over the whole negative range.
              if (occupiesNoVenue(a.venue_id))
                return a.encounter_type_id < b.encounter_type_id;
              return false;
            });
}

void InteractionManager::logCoordinatedEncounterParticipants(
    size_t group_start, size_t group_end,
    const std::unordered_set<PersonId>* visitor_ids) {
  if (!event_logger_) return;
  int encounter_participants = 0;
  for (size_t idx = group_start; idx < group_end; ++idx) {
    PersonId pid = active_locations_buffer_[idx].person_id;
    bool is_visitor = (visitor_ids && visitor_ids->count(pid) > 0);
    if (!is_visitor && active_locations_buffer_[idx].encounter_type_id <
                           world_.encounter_type_names.size()) {
      encounter_participants++;
    }
  }
  if (encounter_participants > 0) {
    event_logger_->logEncounterStats(current_day_type_idx_, true,
                                     encounter_participants);
  }
}

bool InteractionManager::venueGroupHasTransmissionSource(
    const Venue* venue, VenueId venue_id,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    const CompartmentalModelManager* comp_model) const {
  bool has_fomite =
      venue && !venue->fomite_history.empty() &&
      std::any_of(venue->fomite_history.begin(), venue->fomite_history.end(),
                  [](const auto& v) { return !v.empty(); });
  bool venue_has_comp_uptake =
      comp_model != nullptr &&
      comp_model->venueToLocalNodeIndex(static_cast<int>(venue_id)) >= 0;
  if (has_fomite || venue_has_comp_uptake) return true;

  for (const auto& m : group_members_buffer_) {
    if (m.array_index < world_.people.size() &&
        world_.people[m.array_index].id == m.id &&
        world_.people[m.array_index].infection != nullptr) {
      return true;
    }
    if (visitor_data) {
      auto vit = visitor_data->find(m.id);
      if (vit != visitor_data->end() && vit->second.is_infectious) {
        return true;
      }
    }
  }
  return false;
}

size_t InteractionManager::collectAndSortGroupMembers(size_t group_start) {
  const auto& first = active_locations_buffer_[group_start];
  group_members_buffer_.clear();
  size_t i = group_start;
  while (
      i < active_locations_buffer_.size() &&
      active_locations_buffer_[i].venue_id == first.venue_id &&
      (first.venue_id >= 0 || active_locations_buffer_[i].encounter_type_id ==
                                  first.encounter_type_id)) {
    group_members_buffer_.push_back(
        {active_locations_buffer_[i].person_id,
         active_locations_buffer_[i].person_array_index,
         active_locations_buffer_[i].subset_index,
         active_locations_buffer_[i].encounter_type_id});
    i++;
  }

  // Sort members by person_id BEFORE binning to ensure deterministic
  // floating-point accumulation order for total_infectiousness_by_mode.
  // In MPI mode, locals and visitors arrive in different order than
  // single-rank mode. Without this sort, the sum of infectiousness
  // values can differ in the last bits due to FP non-associativity.
  std::sort(group_members_buffer_.begin(), group_members_buffer_.end(),
            [](const InteractionMember& a, const InteractionMember& b) {
              return a.id < b.id;
            });
  return i;
}

void InteractionManager::printTickParentMixingSummary(
    int total_new_infections, double current_time) const {
  if (!debug_parent_mixing_ || total_new_infections <= 0) return;
  int non_sibling = total_new_infections - dbg_sibling_infections_;
  std::cerr << "[PMIX] t=" << current_time
            << " tick_summary: total=" << total_new_infections
            << " non_sibling=" << non_sibling
            << " sibling=" << dbg_sibling_infections_
            << " (sibling counts person-sourced cross-child only;"
            << " non_sibling includes own-venue, fomite, and"
            << " compartmental sources)" << std::endl;
}

int InteractionManager::processOneVenueGroup(
    size_t group_start, size_t group_end, double current_time,
    double delta_hours, std::unordered_set<PersonId>* active_infections,
    const std::unordered_set<PersonId>* visitor_ids,
    std::vector<PendingInfection>* pending_infections,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    const CompartmentalModelManager* comp_model) {
  if (group_members_buffer_.empty()) return 0;

  const auto& first = active_locations_buffer_[group_start];
  Venue* venue = world_.getVenue(first.venue_id);

  // Skip if person has no venue AND no encounter type
  if (!venue && first.encounter_type_id == kDefaultEncounterTypeId) return 0;

  logCoordinatedEncounterParticipants(group_start, group_end, visitor_ids);

  if (!venueGroupHasTransmissionSource(venue, first.venue_id, visitor_data,
                                       comp_model)) {
    return 0;
  }

  try {
    return processVenueTransmissions(
        group_members_buffer_, venue, first.venue_id, current_time, delta_hours,
        active_infections, visitor_ids, pending_infections, visitor_data,
        first.encounter_type_id, comp_model);
  } catch (const std::exception& e) {
    std::cerr << "[Rank 0] Fatal error in venue=" << first.venue_id
              << " encounter_type=" << (int)first.encounter_type_id << ": "
              << e.what() << std::endl;
    throw;  // Rethrow to abort
  }
}

int InteractionManager::processTransmissions(
    const std::vector<PersonLocation>& locations, double current_time,
    double delta_hours, std::unordered_set<PersonId>* active_infections,
    const std::unordered_set<PersonId>* visitor_ids,
    std::vector<PendingInfection>* pending_infections,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data,
    const CompartmentalModelManager* comp_model) {
  int total_new_infections = 0;

  filterAndSortActiveLocations(locations);
  if (active_locations_buffer_.empty()) return 0;

  // 2b. Build parent-venue aggregates (sibling-mixing). Walks the same
  // venue groups but under each PARENT's contact matrix. All child venues
  // of a parent share its MGU, so this is rank-local. See
  // project_venue_hierarchy_mgu memory.
  dbg_sibling_infections_ = 0;
  dbg_sample_susc_prints_ = 0;
  dbg_sample_infection_prints_ = 0;
  buildParentAggregates(current_time, delta_hours, visitor_data);

  // 3. Process groups linearly
  size_t i = 0;
  while (i < active_locations_buffer_.size()) {
    size_t group_start = i;
    i = collectAndSortGroupMembers(group_start);
    total_new_infections += processOneVenueGroup(
        group_start, i, current_time, delta_hours, active_infections,
        visitor_ids, pending_infections, visitor_data, comp_model);
  }

  // Per-slot transmission count removed; captured in DAY_SUMMARY

  printTickParentMixingSummary(total_new_infections, current_time);

  return total_new_infections;
}

double InteractionManager::lookupContactsForBinPair(const ContactMatrix& matrix,
                                                    int susc_bin,
                                                    int inf_bin) const {
  if (susc_bin < static_cast<int>(matrix.contacts.size()) &&
      inf_bin < static_cast<int>(matrix.contacts[susc_bin].size())) {
    return matrix.contacts[susc_bin][inf_bin];
  }
  // Bin counts come from the same venue type's bin structure, and every mode
  // of a type is checked at load to declare the same bins, so an out-of-range
  // pair here means the caller sized its loops off something else entirely.
  throw std::runtime_error(
      "lookupContactsForBinPair: no contact matrix entry for (susc_bin=" +
      std::to_string(susc_bin) + ", inf_bin=" + std::to_string(inf_bin) +
      "); matrix has " + std::to_string(matrix.contacts.size()) + " bin rows.");
}

uint16_t InteractionManager::resolveInfectorSymptomId(
    PersonId infector_id, double current_time,
    const std::unordered_map<PersonId, VisitorInfo>* visitor_data) const {
  if (infector_id < 0) return kNoSymptomId;
  Person* infp = world_.getPerson(infector_id);
  if (infp && infp->infection) {
    return infp->infection->getTrajectory().getCurrentSymptomId(current_time);
  }
  if (!infp && visitor_data) {
    auto vit = visitor_data->find(infector_id);
    if (vit != visitor_data->end()) return vit->second.symptom_id;
  }
  return kNoSymptomId;
}

}  // namespace june
