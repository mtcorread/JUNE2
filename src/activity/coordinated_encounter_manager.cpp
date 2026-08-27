#include "activity/coordinated_encounter_manager.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "loaders/config_loader.h"
#include "utils/filtering.h"

#ifdef USE_MPI
#include <mpi.h>
#endif

namespace june {

namespace {

// Draws an integer count from a Distribution. For BINOMIAL the
// number-of-trials is taken from `n_for_binomial` (the per-call upper bound).
// Returns `default_val` when the distribution type is unset/unrecognised.
template <typename Dist>
int sampleCountFromDistribution(const Dist& dist, int n_for_binomial,
                                int default_val, SplitMix64& gen) {
  if (dist.type == DistributionType::POISSON) {
    std::poisson_distribution<int> pd(dist.mean);
    return pd(gen);
  }
  if (dist.type == DistributionType::BINOMIAL) {
    std::binomial_distribution<int> bd(n_for_binomial, dist.p);
    return bd(gen);
  }
  if (dist.type == DistributionType::FIXED) {
    return dist.count;
  }
  return default_val;
}

using ProposalKey = CoordinatedEncounterManager::ProposalKey;
using ProposalSet = CoordinatedEncounterManager::ProposalSet;

// Sorts proposals by (invitee, slot, encounter_id, host) so that the same
// invitee processes proposals for the same slot in the same order regardless
// of which rank generated them. Required for MPI-determinism of the
// committed_slots_ decisions.
std::vector<EncounterProposal> buildSortedProposals(
    const std::vector<EncounterProposal>& in_proposals) {
  std::vector<EncounterProposal> sorted(in_proposals.begin(),
                                        in_proposals.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const EncounterProposal& a, const EncounterProposal& b) {
              if (a.invitee_id != b.invitee_id)
                return a.invitee_id < b.invitee_id;
              if (a.slot != b.slot) return a.slot < b.slot;
              if (a.encounter_id != b.encounter_id)
                return a.encounter_id < b.encounter_id;
              return a.host_id < b.host_id;
            });
  return sorted;
}

// Builds the (host, invitee, slot, encounter_type) lookup set from BOTH
// incoming and local-host proposals, so the mutual-proposal check works
// across MPI ranks.
ProposalSet buildProposalKeySet(
    const std::vector<EncounterProposal>& incoming,
    const std::vector<EncounterProposal>& host_proposals) {
  ProposalSet s;
  s.reserve(incoming.size() + host_proposals.size());
  for (const auto& p : incoming) {
    s.insert({p.host_id, p.invitee_id, p.slot, p.encounter_type_id});
  }
  for (const auto& p : host_proposals) {
    s.insert({p.host_id, p.invitee_id, p.slot, p.encounter_type_id});
  }
  return s;
}

bool hasMutualProposal(const ProposalSet& s, const EncounterProposal& prop) {
  return s.count({prop.invitee_id, prop.host_id, prop.slot,
                  prop.encounter_type_id}) > 0;
}

// Returns true iff the invitee's slot is committed AND the mutual-proposal
// bypass does not apply (i.e. this proposal should be
// REJECTED_ALREADY_COMMITTED). Mutual proposals on 1:1 virtual encounters
// bypass commitment to avoid the deadlock where both partners commit the
// slot as hosts and then reject each other as invitees.
bool isInviteeSlotBlocked(
    const std::unordered_map<PersonId, std::set<int>>& committed_slots,
    PersonId invitee_id, int slot, const CoordinatedEncounterDef& matched_def,
    const EncounterProposal& prop, const ProposalSet& proposal_set) {
  auto it = committed_slots.find(invitee_id);
  if (it == committed_slots.end() || it->second.count(slot) == 0) return false;
  return !(matched_def.is_virtual && hasMutualProposal(proposal_set, prop));
}

// Populates the proposal-derived fields of a reply (all but status, which
// is set by the per-proposal decision logic).
EncounterReply makeReplySkeleton(const EncounterProposal& prop) {
  EncounterReply reply;
  reply.encounter_id = prop.encounter_id;
  reply.host_id = prop.host_id;
  reply.invitee_id = prop.invitee_id;
  reply.venue_id = prop.venue_id;
  reply.venue_type_id = prop.venue_type_id;
  reply.slot = prop.slot;
  reply.encounter_type_id = prop.encounter_type_id;
  return reply;
}

}  // namespace

size_t CoordinatedEncounterManager::ProposalKeyHash::operator()(
    const ProposalKey& k) const {
  size_t h = std::hash<int>{}(k.host);
  h ^= std::hash<int>{}(k.invitee) + 0x9e3779b9 + (h << 6) + (h >> 2);
  h ^= std::hash<int>{}(k.slot) + 0x9e3779b9 + (h << 6) + (h >> 2);
  h ^= std::hash<uint8_t>{}(k.encounter_type_id) + 0x9e3779b9 + (h << 6) +
       (h >> 2);
  return h;
}

CoordinatedEncounterManager::CoordinatedEncounterManager(
    const WorldState& world, const Config& config, int mpi_rank)
    : world_(world), config_(config), mpi_rank_(mpi_rank) {
  resetDaily();
}

// Evaluate a person against a frequency group's rows (first-match wins).
// Returns 0.0 if no row matches (person not eligible for this group).
static double lookupFrequencyDailyP(const Person& person,
                                    const WorldState& world,
                                    const FrequencyGroup& fg) {
  for (const auto& row : fg.rows) {
    if (filtering::matchesCriteria(person, &world, row.criteria)) {
      return row.daily_probability;
    }
  }
  return 0.0;
}

// Hash a frequency-group name into a stable 64-bit key suitable for mixing
// into the per-person RNG. FNV-1a 64.
static uint64_t hashGroupName(const std::string& name) {
  uint64_t h = 1469598103934665603ULL;
  for (char c : name) {
    h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
    h *= 1099511628211ULL;
  }
  return h;
}

int CoordinatedEncounterManager::getVirtualVenueTypeId(
    const std::string& matrix_name) const {
  auto it = config_.contact_matrices.matrix_name_to_id.find(matrix_name);
  if (it != config_.contact_matrices.matrix_name_to_id.end()) {
    return it->second;
  }
  return 255;  // Default catch-all
}

// =============================================================================
// generateProposals helpers
// =============================================================================

bool CoordinatedEncounterManager::tryEmitForOneSlot(
    const Person& person, size_t person_idx,
    const CoordinatedEncounterDef& enc_def, int slot_idx, int virtual_v_type,
    const std::string* fg_name_ptr, SplitMix64& gen,
    std::uniform_real_distribution<double>& dist,
    std::unordered_map<size_t, std::vector<int>>& remaining_slots,
    std::vector<EncounterProposal>& out_proposals) {
  // Rate gate: frequency_group bypass (budget-hit verified once per day) or
  // the legacy scalar proposal_probability roll.
  if (!fg_name_ptr) {
    if (dist(gen) > enc_def.proposal_probability) return false;
  }

  VenueSelection venue = selectVenue(person, enc_def, virtual_v_type, gen);
  if (enc_def.is_virtual) {
    // The reserved virtual range encodes the host, so collisions are
    // impossible by construction: a person hosts at most one encounter per
    // slot per type.
    venue.id = makeVirtualVenueId(person.id);
  } else if (!venue.valid) {
    return false;
  }

  std::vector<PersonId> partners = gatherEligiblePartners(person, enc_def);
  if (partners.empty()) return false;

  emitProposals(person, enc_def, slot_idx, venue, partners, gen, out_proposals);

  committed_slots_[person.id].insert(slot_idx);
  auto& rem = remaining_slots[person_idx];
  rem.erase(std::remove(rem.begin(), rem.end(), slot_idx), rem.end());

  if (fg_name_ptr) {
    freq_group_committed_[person.id][*fg_name_ptr] = true;
    freq_group_stats_[*fg_name_ptr].encounters_emitted++;
  }
  return true;
}

bool CoordinatedEncounterManager::isFrequencyGroupBudgetAvailable(
    const Person& person, const std::string& fg_name, int current_day) {
  auto& per_person = freq_group_hit_[person.id];
  if (per_person.find(fg_name) == per_person.end()) {
    auto fg_it = config_.coordinated_encounters.frequency_groups.find(fg_name);
    double daily_p = 0.0;
    if (fg_it != config_.coordinated_encounters.frequency_groups.end()) {
      daily_p = lookupFrequencyDailyP(person, world_, fg_it->second);
    }
    uint64_t group_key = hashGroupName(fg_name);
    SplitMix64 fg_gen(mix_seed(config_.simulation.random_seed, person.id,
                               current_day, group_key));
    std::uniform_real_distribution<double> fg_dist(0.0, 1.0);
    bool hit = (daily_p > 0.0) && (fg_dist(fg_gen) < daily_p);
    per_person[fg_name] = hit;

    // Daily-summary accounting (per frequency group). Counted once
    // per (person, group, day), not per encounter type.
    auto& fgs = freq_group_stats_[fg_name];
    fgs.persons_evaluated++;
    fgs.sum_daily_p += daily_p;
    if (hit) fgs.budget_hits++;
  }
  if (!per_person[fg_name]) return false;
  auto& committed = freq_group_committed_[person.id];
  if (committed[fg_name]) return false;
  return true;
}

void CoordinatedEncounterManager::populateInitialRemainingSlotsIfAbsent(
    const Person& person, size_t person_idx, int day_type_idx,
    std::unordered_map<size_t, std::vector<int>>& remaining_slots) const {
  if (remaining_slots.find(person_idx) != remaining_slots.end()) return;
  std::vector<int> all_valid;
  if (person.cached_schedule_type_ != nullptr) {
    const auto* sched = person.cached_schedule_type_;
    if (day_type_idx >= 0 &&
        day_type_idx < static_cast<int>(sched->slots_by_day_type_idx.size()) &&
        sched->slots_by_day_type_idx[day_type_idx] != nullptr) {
      const auto& slots = *sched->slots_by_day_type_idx[day_type_idx];
      for (size_t s = 0; s < slots.size(); ++s) {
        all_valid.push_back(static_cast<int>(s));
      }
    }
  }
  remaining_slots[person_idx] = all_valid;
}

void CoordinatedEncounterManager::logEncounterConfig(
    const CoordinatedEncounterDef& enc_def) const {
  // One-line config dump (rank 0, day 0). Shows the rate source and the
  // resolved acceptance value, so silent defaults (acceptance_probability
  // omitted -> 1.0) are visible in the log rather than invisible.
  std::cout << "[CoordEnc] '" << enc_def.name << "' rate=";
  if (enc_def.frequency_group.has_value()) {
    std::cout << "frequency_group('" << *enc_def.frequency_group << "')";
  } else {
    std::cout << "proposal_probability=" << enc_def.proposal_probability;
  }
  std::cout << "  accept=" << enc_def.acceptance_probability
            << "  priority=" << enc_def.priority << "  network='"
            << enc_def.network << "'";
  if (!enc_def.network_partner_filter.empty()) {
    std::cout << "  filter='" << enc_def.network_partner_filter << "'";
  }
  std::cout << std::endl;
}

std::vector<int> CoordinatedEncounterManager::getValidSlotsForType(
    const Person& person, const CoordinatedEncounterDef& enc_def,
    const std::vector<int>& remaining, int day_type_idx) const {
  // Use pre-cached trigger_mask from enc_def (populated at config resolve time)
  ActivityMask trigger_mask = enc_def.trigger_mask;
  std::vector<int> valid;

  if (person.cached_schedule_type_ == nullptr) return valid;
  const auto* sched = person.cached_schedule_type_;
  if (day_type_idx < 0 ||
      day_type_idx >= static_cast<int>(sched->slots_by_day_type_idx.size()) ||
      sched->slots_by_day_type_idx[day_type_idx] == nullptr)
    return valid;
  const auto& slots = *sched->slots_by_day_type_idx[day_type_idx];
  for (int slot_idx : remaining) {
    if (slot_idx < 0 || slot_idx >= static_cast<int>(slots.size())) continue;

    // Use pre-cached allowed_activity_mask + coordinated_only_activity_mask
    ActivityMask slot_mask = slots[slot_idx].allowed_activity_mask |
                             slots[slot_idx].coordinated_only_activity_mask;
    if (slot_mask & trigger_mask) {
      valid.push_back(slot_idx);
    }
  }
  return valid;
}

int CoordinatedEncounterManager::sampleTypeBudget(
    const CoordinatedEncounterDef& enc_def, int num_valid_slots,
    SplitMix64& gen) const {
  int budget = sampleCountFromDistribution(enc_def.daily_max_distribution,
                                           num_valid_slots, 1, gen);
  return std::max(0, std::min(budget, num_valid_slots));
}

CoordinatedEncounterManager::VenueSelection
CoordinatedEncounterManager::selectVenue(const Person& person,
                                         const CoordinatedEncounterDef& enc_def,
                                         int virtual_v_type,
                                         SplitMix64& gen) const {
  if (enc_def.is_virtual) {
    // Virtual venues use a negative ID from the rank-partitioned space.
    // The actual decrement happens in the caller after confirming validity.
    return {0, virtual_v_type, true};  // id filled in by caller
  }

  // Physical: host must have a linked venue of one of the allowed types.
  // Use getVenueTypeId() (global map) instead of getVenue() (local only)
  // to resolve venue types for cross-rank venues in MPI mode.
  // No fallback: if a person has no venue of the required type, they
  // can't host this encounter.
  std::vector<std::pair<VenueId, int>> possible_venues;
  for (const auto& venue_type_name : enc_def.allowed_venues) {
    auto vt_it = std::find(world_.venue_type_names.begin(),
                           world_.venue_type_names.end(), venue_type_name);
    if (vt_it == world_.venue_type_names.end()) continue;
    int vt_idx = std::distance(world_.venue_type_names.begin(), vt_it);

    for (int i = 0; i < person.activity_meta_count; ++i) {
      const auto& meta = world_.activity_meta[person.activity_meta_start + i];
      if (meta.venue_count > 0) {
        auto venues = world_.getActivityVenues(meta);
        for (const auto& v : venues) {
          if (world_.getVenueTypeId(v.first) == vt_idx) {
            possible_venues.push_back({v.first, vt_idx});
          }
        }
      }
    }
  }

  if (possible_venues.empty()) {
    return {-1, 255, false};
  }

  std::uniform_int_distribution<int> v_dist(0, possible_venues.size() - 1);
  auto selection = possible_venues[v_dist(gen)];
  return {selection.first, selection.second, true};
}

std::vector<PersonId> CoordinatedEncounterManager::gatherEligiblePartners(
    const Person& person, const CoordinatedEncounterDef& enc_def) const {
  std::vector<PersonId> partners;
  auto nw_partners =
      world_.getNetworkPartners(person, enc_def.cached_network_idx);
  partners.reserve(nw_partners.size());
  for (PersonId p : nw_partners) partners.push_back(p);
  return partners;
}

void CoordinatedEncounterManager::emitProposals(
    const Person& person, const CoordinatedEncounterDef& enc_def, int slot_idx,
    const VenueSelection& venue, std::vector<PersonId>& eligible_partners,
    SplitMix64& gen, std::vector<EncounterProposal>& out_proposals) {
  // The proposal is the single manufacture point for the venue type every
  // downstream participant reads: the reply copies it, and so does the
  // finalised encounter. An unresolvable type here would survive as far as
  // def matching and be dropped as a no-matching-def warning, so refuse it
  // where the defect is. A venue type sitting at registry index 255 aliases
  // kUnknownVenueTypeId, so this is reachable, not defensive. Checked once
  // per encounter — the venue does not vary across the invitees.
  if (!occupiesNoVenue(venue.id) && venue.type_id == kUnknownVenueTypeId)
    throw std::runtime_error("coordinated encounters: physical venue " +
                             std::to_string(venue.id) + " for encounter '" +
                             enc_def.name + "' has an unresolvable venue type");

  int num_partners = static_cast<int>(eligible_partners.size());

  // Sample invite count from distribution, clamped to [1, num_partners]
  int to_invite = sampleCountFromDistribution(enc_def.invite_distribution,
                                              num_partners, 1, gen);
  to_invite = std::max(1, std::min(to_invite, num_partners));

  // Canonicalize partner order before shuffling. Network partner lists can
  // arrive in rank-dependent order; sorting by partner_id makes the shuffle
  // MPI-independent.
  std::sort(eligible_partners.begin(), eligible_partners.end());

  // Shuffle and pick
  std::shuffle(eligible_partners.begin(), eligible_partners.end(), gen);
  int invited = 0;

  for (PersonId partner_id : eligible_partners) {
    if (invited >= to_invite) break;

    EncounterProposal prop;
    prop.encounter_id =
        static_cast<int>(mix_seed(config_.simulation.random_seed, person.id,
                                  static_cast<uint64_t>(slot_idx),
                                  static_cast<uint64_t>(partner_id)) &
                         0x7FFFFFFF);
    prop.host_id = person.id;
    prop.host_rank = mpi_rank_;
    prop.invitee_id = partner_id;
    prop.venue_id = venue.id;
    prop.venue_owner_rank = mpi_rank_;
    prop.venue_type_id = venue.type_id;
    prop.slot = slot_idx;
    prop.encounter_type_id = enc_def.cached_encounter_type_id;

    out_proposals.push_back(prop);
    invited++;
  }
}

// =============================================================================
// processProposals helpers
// =============================================================================

const CoordinatedEncounterDef*
CoordinatedEncounterManager::findMatchingEncounterDef(
    const EncounterProposal& prop) const {
  for (const auto& enc_def : config_.coordinated_encounters.encounters) {
    if (!enc_def.enabled) continue;

    if (enc_def.is_virtual) {
      if (prop.venue_type_id == enc_def.cached_virtual_venue_type_id) {
        return &enc_def;
      }
    } else {
      // Use bitmask: check if bit at prop.venue_type_id is set in
      // allowed_venue_mask
      if (prop.venue_type_id < 32 &&
          ((enc_def.allowed_venue_mask >> prop.venue_type_id) & 1)) {
        return &enc_def;
      }
    }
  }
  return nullptr;
}

bool CoordinatedEncounterManager::isScheduleCompatible(
    const Person& invitee, int slot, const CoordinatedEncounterDef& def,
    int day_type_idx) const {
  if (invitee.cached_schedule_type_ == nullptr) {
    return true;  // No schedule cached → assume free as fallback
  }

  const auto* sched = invitee.cached_schedule_type_;
  if (day_type_idx < 0 ||
      day_type_idx >= static_cast<int>(sched->slots_by_day_type_idx.size()) ||
      sched->slots_by_day_type_idx[day_type_idx] == nullptr) {
    return false;
  }
  const auto& schedule_slots = *sched->slots_by_day_type_idx[day_type_idx];

  if (slot < 0 || slot >= static_cast<int>(schedule_slots.size())) {
    return false;
  }

  // Use pre-cached masks (no string comparisons)
  // OR in coordinated_only_activity_mask so encounters can trigger on
  // activities that are only available via coordinated encounters.
  ActivityMask slot_mask = schedule_slots[slot].allowed_activity_mask |
                           schedule_slots[slot].coordinated_only_activity_mask;
  ActivityMask trigger_mask = def.trigger_mask;
  return (slot_mask & trigger_mask) != 0;
}

// =============================================================================
// Main public methods (now thin orchestrators)
// =============================================================================

void CoordinatedEncounterManager::generateProposals(
    int current_day, std::vector<EncounterProposal>& out_proposals,
    int day_type_idx) {
  if (!config_.coordinated_encounters.enabled) return;
  // Per-person remaining slot tracking for this day
  std::unordered_map<size_t, std::vector<int>> remaining_slots;

  // Encounter defs are already sorted by priority (done in config_loader)
  int enc_type_counter = 0;
  for (const auto& enc_def : config_.coordinated_encounters.encounters) {
    if (!enc_def.enabled) continue;
    if (current_day == 0 && mpi_rank_ == 0) logEncounterConfig(enc_def);
    // Unresolved (negative) network_idx means the encounter is effectively
    // disabled.
    if (enc_def.cached_network_idx < 0) continue;

    int virtual_v_type = enc_def.cached_virtual_venue_type_id;
    for (size_t person_idx = 0; person_idx < world_.people.size();
         ++person_idx) {
      proposeForOnePersonOneEncounter(world_.people[person_idx], person_idx,
                                      enc_def, current_day, day_type_idx,
                                      enc_type_counter, virtual_v_type,
                                      remaining_slots, out_proposals);
    }
    enc_type_counter++;
  }
}

void CoordinatedEncounterManager::proposeForOnePersonOneEncounter(
    const Person& person, size_t person_idx,
    const CoordinatedEncounterDef& enc_def, int current_day, int day_type_idx,
    int enc_type_counter, int virtual_v_type,
    std::unordered_map<size_t, std::vector<int>>& remaining_slots,
    std::vector<EncounterProposal>& out_proposals) {
  if (person.is_dead) return;

  // Per-person deterministic RNG for MPI reproducibility
  SplitMix64 gen(mix_seed(config_.simulation.random_seed, person.id,
                          current_day, enc_type_counter));
  std::uniform_real_distribution<double> dist(0.0, 1.0);

  // If this encounter is part of a frequency_group, resolve today's
  // per-person budget-hit once per (person, group). The roll uses a
  // group-specific RNG so it is independent of enc_type_counter (which
  // varies across the types sharing the group, and across runs where
  // encounter ordering may differ).
  const std::string* fg_name_ptr =
      enc_def.frequency_group.has_value() ? &*enc_def.frequency_group : nullptr;
  if (fg_name_ptr &&
      !isFrequencyGroupBudgetAvailable(person, *fg_name_ptr, current_day)) {
    return;
  }

  populateInitialRemainingSlotsIfAbsent(person, person_idx, day_type_idx,
                                        remaining_slots);

  std::vector<int> valid_slots = getValidSlotsForType(
      person, enc_def, remaining_slots[person_idx], day_type_idx);
  if (valid_slots.empty()) return;

  int type_budget =
      sampleTypeBudget(enc_def, static_cast<int>(valid_slots.size()), gen);
  if (type_budget == 0) return;

  std::shuffle(valid_slots.begin(), valid_slots.end(), gen);

  int proposals_made = 0;
  for (int slot_idx : valid_slots) {
    if (proposals_made >= type_budget) break;
    if (tryEmitForOneSlot(person, person_idx, enc_def, slot_idx, virtual_v_type,
                          fg_name_ptr, gen, dist, remaining_slots,
                          out_proposals)) {
      proposals_made++;
    }
  }
}

void CoordinatedEncounterManager::processProposals(
    const std::vector<EncounterProposal>& in_proposals,
    const std::vector<EncounterProposal>& host_proposals,
    std::vector<EncounterReply>& out_replies, int day_type_idx) {
  if (!config_.coordinated_encounters.enabled) return;

  std::vector<EncounterProposal> sorted_proposals =
      buildSortedProposals(in_proposals);
  ProposalSet proposal_set =
      buildProposalKeySet(sorted_proposals, host_proposals);

  for (const auto& prop : sorted_proposals) {
    out_replies.push_back(
        replyForOneProposal(prop, day_type_idx, proposal_set));
  }
}

EncounterReply CoordinatedEncounterManager::replyForOneProposal(
    const EncounterProposal& prop, int day_type_idx,
    const ProposalSet& proposal_set) {
  EncounterReply reply = makeReplySkeleton(prop);

  auto it = world_.person_index.find(prop.invitee_id);
  if (it == world_.person_index.end()) {
    reply.status = ReplyStatus::REJECTED_NOT_FOUND;
    return reply;
  }
  const Person& invitee = world_.people[it->second];
  if (invitee.is_dead) {
    reply.status = ReplyStatus::REJECTED_DEAD;
    return reply;
  }

  // Match def first; needed for the mutual-proposal bypass decision below.
  const CoordinatedEncounterDef* matched_def = findMatchingEncounterDef(prop);
  if (!matched_def) {
    reply.status = ReplyStatus::REJECTED_NO_MATCHING_DEF;
    std::cerr << "[Rank " << mpi_rank_ << "] WARNING: Encounter "
              << prop.encounter_id << " has venue_type_id "
              << prop.venue_type_id
              << " which does not match any encounter definition. Rejecting."
              << std::endl;
    return reply;
  }

  if (isInviteeSlotBlocked(committed_slots_, invitee.id, prop.slot,
                           *matched_def, prop, proposal_set)) {
    reply.status = ReplyStatus::REJECTED_ALREADY_COMMITTED;
    return reply;
  }

  if (!isScheduleCompatible(invitee, prop.slot, *matched_def, day_type_idx)) {
    reply.status = ReplyStatus::REJECTED_SCHEDULE_CONFLICT;
    return reply;
  }

  // Per-invitee deterministic RNG (no mpi_rank). Mix invitee_id so each
  // invitee gets an independent draw even if encounter_id collisions occur
  // (31-bit truncation).
  SplitMix64 gen(mix_seed(config_.simulation.random_seed, prop.encounter_id,
                          prop.invitee_id, 0xACCE97ULL));
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  if (dist(gen) > matched_def->acceptance_probability) {
    reply.status = ReplyStatus::REJECTED_DECLINED;
  } else {
    reply.status = ReplyStatus::ACCEPTED;
    committed_slots_[invitee.id].insert(prop.slot);
  }
  return reply;
}

void CoordinatedEncounterManager::finalizeEncounters(
    const std::vector<EncounterReply>& replies,
    std::vector<CoordinatedEncounter>& out_finalized) {
  // Progress message removed (sub-second operation)
  std::map<std::pair<int, PersonId>, std::vector<EncounterReply>> grouper;
  for (const auto& r : replies) {
    grouper[{r.encounter_id, r.host_id}].push_back(r);
  }

  for (const auto& kv : grouper) {
    int eid = kv.first.first;
    const auto& event_replies = kv.second;

    std::set<PersonId> attendees;
    attendees.insert(event_replies[0].host_id);

    for (const auto& r : event_replies) {
      if (r.status == ReplyStatus::ACCEPTED) attendees.insert(r.invitee_id);
    }

    if (attendees.size() > 1) {
      CoordinatedEncounter final_ev;
      final_ev.encounter_id = eid;
      final_ev.host_id = event_replies[0].host_id;
      final_ev.venue_id = event_replies[0].venue_id;
      final_ev.venue_type_id = event_replies[0].venue_type_id;
      final_ev.slot = event_replies[0].slot;
      final_ev.encounter_type_id = event_replies[0].encounter_type_id;
      final_ev.participants = attendees;

      // Resolve the host's subset at the venue while the host is local (this
      // rank hosts the encounter). Every participant adopts it at injection.
      // Virtual venues (negative id) have no subsets, so leave it at -1.
      if (final_ev.venue_id >= 0) {
        if (const Person* h = world_.getPerson(final_ev.host_id)) {
          for (const auto& meta : world_.getActivityMetas(*h)) {
            for (const auto& vs : world_.getActivityVenues(meta)) {
              if (vs.first == final_ev.venue_id) {
                final_ev.host_subset_index = vs.second;
                break;
              }
            }
            if (final_ev.host_subset_index >= 0) break;
          }
        }
      }

      out_finalized.push_back(final_ev);
      daily_encounters_.push_back(final_ev);
    }
  }
}

void CoordinatedEncounterManager::resetDaily() {
  daily_encounters_.clear();
  committed_slots_.clear();
  daily_stats_ = DailyEncounterStats{};
  freq_group_hit_.clear();
  freq_group_committed_.clear();
  freq_group_stats_.clear();
}

}  // namespace june
