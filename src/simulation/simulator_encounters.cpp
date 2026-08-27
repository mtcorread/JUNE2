// Simulator coordinated-encounter helpers: daily negotiation + slot injection
// + local-rank logging. Split from simulator.cpp (declared in
// simulation/simulator.h).
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "simulation/encounters/eligibility.h"
#include "simulation/follow_bindings.h"
#include "simulation/simulator.h"
#include "utils/deterministic_rng.h"
#ifdef USE_MPI
#include "parallel/mpi_utils.h"
#endif

namespace june {

using encounters::buildEncounterLookups;
using encounters::computeLocalEligibility;
using encounters::EncounterEligibility;
using encounters::EncounterLookups;

namespace {

// When both A→B and B→A proposals are accepted in the same slot, two
// encounters exist for the same pair. Collapse them by keeping the lowest
// encounter_id; then sort by encounter_id so injection order is
// deterministic across ranks (later encounters for the same person skip
// already-assigned slots).
std::vector<CoordinatedEncounter> dedupAndSortDailyEncounters(
    std::vector<CoordinatedEncounter> daily_encounters) {
  std::map<std::pair<std::set<PersonId>, int>, size_t> best;
  for (size_t i = 0; i < daily_encounters.size(); ++i) {
    auto key = std::make_pair(daily_encounters[i].participants,
                              daily_encounters[i].slot);
    auto it = best.find(key);
    if (it == best.end()) {
      best[key] = i;
    } else if (daily_encounters[i].encounter_id <
               daily_encounters[it->second].encounter_id) {
      it->second = i;
    }
  }
  std::vector<CoordinatedEncounter> deduped;
  deduped.reserve(best.size());
  for (auto& [key, idx] : best) {
    deduped.push_back(std::move(daily_encounters[idx]));
  }
  std::sort(deduped.begin(), deduped.end(),
            [](const CoordinatedEncounter& a, const CoordinatedEncounter& b) {
              return a.encounter_id < b.encounter_id;
            });
  return deduped;
}

// MPI eligibility exchange. Each rank only knows its local participants'
// eligibility; for encounters with any remote participants we Allgatherv
// (encounter_id, local_eligible) pairs and sum across ranks. The result
// maps encounter_id -> global eligible count, with entries only for
// encounters that had any remote participants. Serial or single-rank
// builds return an empty map.
std::unordered_map<int, int> exchangeGlobalEligibility(
    const std::vector<EncounterEligibility>& slot_encounters,
    const std::vector<CoordinatedEncounter>& daily_encounters,
    const WorldState& world, DomainManager* domain_mgr) {
  std::unordered_map<int, int> global_eligible_map;
#ifdef USE_MPI
  if (!domain_mgr) return global_eligible_map;
  // Collect (encounter_id, local_eligible) for encounters with any remote
  // participants; these are the only ones that need exchange.
  std::vector<int> local_pairs;  // flat array: [eid, count, eid, count, ...]
  for (const auto& ee : slot_encounters) {
    const auto& enc = daily_encounters[ee.encounter_idx];
    bool has_remote = false;
    for (PersonId pid : enc.participants) {
      if (world.person_index.find(pid) == world.person_index.end()) {
        has_remote = true;
        break;
      }
    }
    if (has_remote) {
      local_pairs.push_back(enc.encounter_id);
      local_pairs.push_back(ee.local_eligible);
    }
  }

  int local_count = static_cast<int>(local_pairs.size());
  int num_ranks = domain_mgr->getNumRanks();
  std::vector<int> all_counts(num_ranks);
  MPI_Allgather(&local_count, 1, MPI_INT, all_counts.data(), 1, MPI_INT,
                MPI_COMM_WORLD);

  std::vector<int> displs(num_ranks, 0);
  int total = 0;
  for (int r = 0; r < num_ranks; ++r) {
    displs[r] = total;
    total += all_counts[r];
  }

  std::vector<int> all_pairs(total);
  MPI_Allgatherv(local_pairs.data(), local_count, MPI_INT, all_pairs.data(),
                 all_counts.data(), displs.data(), MPI_INT, MPI_COMM_WORLD);

  for (int i = 0; i < total; i += 2) {
    global_eligible_map[all_pairs[i]] += all_pairs[i + 1];
  }
#else
  (void)slot_encounters;
  (void)daily_encounters;
  (void)world;
  (void)domain_mgr;
#endif
  return global_eligible_map;
}

// Pass 2: stamp the per-encounter venue_id / encounter_type_id onto every
// local eligible participant of any encounter that meets its min_required
// threshold. Then for virtual venues (negative IDs) register the host-rank
// ownership so the visitor exchange routes cross-rank participants
// correctly.
void applyEncounterInjection(
    const std::vector<EncounterEligibility>& slot_encounters,
    const std::vector<CoordinatedEncounter>& daily_encounters,
    const std::unordered_map<int, int>& global_eligible_map,
    std::vector<PersonLocation>& locations, DomainManager* domain_mgr) {
  for (size_t i = 0; i < slot_encounters.size(); ++i) {
    const auto& ee = slot_encounters[i];
    const auto& enc = daily_encounters[ee.encounter_idx];

    // Use global eligible count if available (MPI mode with remote
    // participants); otherwise use local count (serial or all-local).
    int total_eligible;
    auto ge_it = global_eligible_map.find(enc.encounter_id);
    if (ge_it != global_eligible_map.end()) {
      total_eligible = ge_it->second;
    } else {
      total_eligible = ee.local_eligible;
    }

    if (total_eligible < ee.min_required || ee.local_eligible <= 0) continue;

    for (size_t array_idx : ee.eligible_indices) {
      locations[array_idx].venue_id = enc.venue_id;
      locations[array_idx].encounter_type_id = enc.encounter_type_id;
      // Bug #13: adopt the host's subset so every participant bins as the
      // host's subgroup, not by the stale subset_index of the venue they were
      // scheduled to. -1 on virtual venues (no subsets), so left untouched.
      if (enc.host_subset_index >= 0)
        locations[array_idx].subset_index = enc.host_subset_index;
    }

#ifdef USE_MPI
    // Register virtual venue ownership so the visitor exchange can route
    // cross-rank encounter participants to the host's rank. Physical
    // venues already have ownership via the venue ownership map; virtual
    // venues need explicit registration. isVirtualVenue, to match the test in
    // Domain::ownsVenue that reads this registry back.
    if (domain_mgr && isVirtualVenue(enc.venue_id)) {
      int host_rank = domain_mgr->getPersonRank(enc.host_id);
      domain_mgr->setVenueRank(enc.venue_id, host_rank);
      if (host_rank == domain_mgr->getRank()) {
        domain_mgr->getDomain().registerVirtualVenue(enc.venue_id);
      }
    }
#else
    (void)domain_mgr;
#endif
  }
}

}  // namespace

void Simulator::negotiateAndLogDailyEncounters(int day, int rank) {
  if (!coordinated_encounter_manager_) return;
  coordinated_encounter_manager_->resetDaily();

  int encounter_day_type_idx = config_.schedule.getDayTypeIndex(day);

  // Phase 1: Generate proposals locally
  std::vector<EncounterProposal> local_proposals;
  coordinated_encounter_manager_->generateProposals(day, local_proposals,
                                                    encounter_day_type_idx);

  // Accumulate proposal stats for debug summary
  coordinated_encounter_manager_->accumulateProposalStats(local_proposals);

  // Phase 2: Exchange proposals across ranks, then process.
  // Keep local_proposals alive for mutual proposal detection.
  std::vector<EncounterProposal> all_proposals;
#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->exchangeEncounterProposals(local_proposals, all_proposals);
  } else {
    all_proposals = local_proposals;  // copy, not move
  }
#else
  all_proposals = local_proposals;  // copy, not move
#endif

  std::vector<EncounterReply> local_replies;
  coordinated_encounter_manager_->processProposals(
      all_proposals, local_proposals, local_replies, encounter_day_type_idx);

  // Phase 3: Exchange replies back to host ranks, then finalize
  std::vector<EncounterReply> all_replies;
#ifdef USE_MPI
  if (domain_mgr_) {
    domain_mgr_->exchangeEncounterReplies(local_replies, all_replies);
  } else {
    all_replies = std::move(local_replies);
  }
#else
  all_replies = std::move(local_replies);
#endif

  // Accumulate reply stats for debug summary
  coordinated_encounter_manager_->accumulateReplyStats(all_replies);

  std::vector<CoordinatedEncounter> finalized;
  coordinated_encounter_manager_->finalizeEncounters(all_replies, finalized);

  // Accumulate finalize stats for debug summary
  coordinated_encounter_manager_->accumulateFinalizeStats(finalized);

  // Log locally-finalized encounters to HDF5 BEFORE merging remote ones, so
  // cross-rank encounters never get double-logged.
  logFinalizedEncountersLocally(finalized, rank);

  // Phase 4: Exchange finalized encounters so remote participants know
#ifdef USE_MPI
  if (domain_mgr_) {
    std::vector<CoordinatedEncounter> remote_finalized;
    domain_mgr_->exchangeFinalizedEncounters(finalized, remote_finalized);
    // Add remote encounters that involve our local people
    for (auto& enc : remote_finalized) {
      coordinated_encounter_manager_->addDailyEncounter(enc);
    }
  }
#endif
}

void Simulator::logFinalizedEncountersLocally(
    const std::vector<CoordinatedEncounter>& finalized, int rank) {
  // Skip before anything enters the EventLogger buffer, so disabling this
  // avoids the RAM cost too, not just the HDF5 write. Encounters still
  // happen and still drive transmission (see
  // injectCoordinatedEncountersIntoSlot); this only gates whether they're
  // recorded to simulation_events.h5.
  if (!config_.simulation.save_coordinated_encounters) return;

  // group_id fans one unique uint64 per real encounter across every pair-row
  // belonging to it. Rank is packed into the high 16 bits so counters stay
  // unique across MPI ranks without coordination; the low 48 bits are a
  // per-rank monotonic counter (2^48 events / rank is effectively unbounded
  // for realistic simulations).
  const uint64_t rank_prefix = static_cast<uint64_t>(rank) << 48;
  for (const auto& enc : finalized) {
    const uint64_t group_id =
        rank_prefix | (next_encounter_group_id_++ & 0x0000FFFFFFFFFFFFULL);
    for (PersonId pid : enc.participants) {
      if (pid != enc.host_id) {
        event_logger_.logCoordinatedEncounter(
            enc.host_id, pid, current_simulation_time_, enc.encounter_type_id,
            enc.slot, group_id);
      }
    }
  }
}

void Simulator::injectCoordinatedEncountersIntoSlot(int time_slot_index) {
  if (!coordinated_encounter_manager_) return;

  // Per-slot lookup tables (trigger activities + min attendees) keyed by
  // encounter_type_id. Cheap to rebuild per slot.
  EncounterLookups lookups =
      buildEncounterLookups(world_, config_.coordinated_encounters.encounters);

  auto daily_encounters = dedupAndSortDailyEncounters(
      coordinated_encounter_manager_->getDailyEncounters());

  // Pass 1: compute each encounter's local eligible-participant set under
  // the alive + policy-block checks.
  std::vector<EncounterEligibility> slot_encounters = computeLocalEligibility(
      daily_encounters, time_slot_index, current_simulation_time_, lookups,
      world_, locations_, policy_manager_.get());

  // Pass 1.5: in MPI mode, sum local_eligible across ranks for encounters
  // that span ranks so every rank sees the true global count.
  std::unordered_map<int, int> global_eligible_map = exchangeGlobalEligibility(
      slot_encounters, daily_encounters, world_, domain_mgr_);

  // Pass 2: stamp venue_id / encounter_type_id onto eligible participants
  // for encounters that meet their min_required threshold.
  applyEncounterInjection(slot_encounters, daily_encounters,
                          global_eligible_map, locations_, domain_mgr_);
}

namespace {

// Where a host is this slot, as seen by its followers. activity is the host's
// resolved activity, used to decide the follower-side activity exception.
// venue_type travels with the venue because a follower's rank may not hold the
// host's venue in its own halo and so cannot type it locally.
struct HostSlot {
  VenueId venue = -1;
  SubsetIndex subset = -1;
  int16_t activity = -1;
  uint8_t venue_type = kUnknownVenueTypeId;
};

}  // namespace

// The binding helpers live in a named namespace (rather than the anonymous one)
// so the follow unit tests can drive them directly on a synthetic world. They
// are otherwise internal to this translation unit.
namespace follow_detail {

// Find the host's venue of the configured pool type, or -1 if it has none.
VenueId findPoolVenue(const WorldState& world, const Person& host,
                      int pool_venue_type_id) {
  for (const auto& meta : world.getActivityMetas(host)) {
    for (const auto& vs : world.getActivityVenues(meta)) {
      const Venue* v = world.getVenue(vs.first);
      if (v && v->type_id == pool_venue_type_id) return vs.first;
    }
  }
  return -1;
}

// Does an exception keep this follower on its own schedule for the slot?
//
// Two of the three ask where the host is going. An excepted host activity peels
// an infant off to its own nursery while the parent is at work, and drops a
// host who is a patient being treated (medical_facility) while still following
// one merely sent home sick (residence). An excepted host venue type makes the
// cut activity cannot: leisure reaches a cinema, a gym and a grocery alike.
//
// The third asks what the follower would otherwise be doing, and it is the only
// one that can outrank a host who is somewhere perfectly followable. A
// school-age child trails a parent all day, but when the child's own schedule
// says school, school wins, whether the parent is at work, at home, or out. The
// host-side exceptions cannot express that: a parent with no primary activity
// is never at an excepted activity, so without this the parent's venue would
// overwrite the child's school every time.
bool venueExcepted(const FollowConfig& fc, uint8_t venue_type) {
  return std::find(fc.venue_exception_type_ids.begin(),
                   fc.venue_exception_type_ids.end(),
                   venue_type) != fc.venue_exception_type_ids.end();
}

bool mirrorSuppressed(const FollowConfig& fc, int16_t host_activity,
                      uint8_t host_venue_type, int16_t follower_activity) {
  // Every rank types every Venue, and the host is known to have one, so an
  // unresolvable type here can only be an id naming no Venue at all — a defect
  // under every configuration, gated rule or not. Unconditional deliberately:
  // arming it on venue_exceptions would warn a gated rule its world is corrupt
  // while silently mirroring an ungated follower into an unnameable venue.
  if (host_venue_type == kUnknownVenueTypeId)
    throw std::runtime_error(
        "follow: host venue type is unresolvable at the mirror gate");
  auto listed = [](const auto& ids, auto value) {
    return std::find(ids.begin(), ids.end(), value) != ids.end();
  };
  if (listed(fc.activity_exception_ids, host_activity)) return true;
  if (venueExcepted(fc, host_venue_type)) return true;
  // -1 means the follower has nowhere of its own to be this slot, so there is
  // nothing for an exception to protect and it follows.
  if (follower_activity >= 0 &&
      listed(fc.follower_activity_exception_ids, follower_activity))
    return true;
  return false;
}

bool policySuppressesMirror(PolicyManager* policy_manager,
                            const Person& follower,
                            int16_t follower_activity_index,
                            uint8_t host_venue_type, double current_time) {
  if (!policy_manager) return false;
  // known(), not fromVenue(): the host's type travelled with its venue, so no
  // local lookup is needed and none would succeed for a cross-rank host. A host
  // only enters host_loc with a venue, so the type is never absent here.
  return policy_manager->suppressesParticipation(
      follower, follower_activity_index, SlotVenueType::known(host_venue_type),
      current_time);
}

// The host's candidate pool: co-members of its venue of the configured type, or
// its partners in the configured network. Both return global PersonIds; callers
// enrol only those local to this rank (remote network partners are routed
// separately).
std::vector<PersonId> gatherPool(const WorldState& world,
                                 const FollowConfig& fc, const Person& host) {
  std::vector<PersonId> pool;
  if (fc.usesNetwork()) {
    for (PersonId m : world.getNetworkPartners(host, fc.network_idx))
      pool.push_back(m);
    return pool;
  }
  VenueId pool_venue = findPoolVenue(world, host, fc.pool_venue_type_id);
  if (pool_venue < 0) return pool;
  for (const auto& sub : world.getSubsets(*world.getVenue(pool_venue)))
    for (PersonId m : world.getSubsetMembers(sub)) pool.push_back(m);
  return pool;
}

// Rebuild the criteria bindings from scratch. Every eligible local person
// becomes a follower of the lowest-id host among its pool. There is no
// randomness, so the result is the same at any rank count and can be thrown
// away and recomputed each day rather than saved.
//
// For a venue pool the members are co-resident, so the host is the lowest-id
// co-member that is alive and host-eligible, all decided locally with no
// messaging. For a network pool a partner can live on another rank, whose age
// and liveness are not knowable here, so the host is simply the lowest partner
// id (host eligibility is rejected for network pools at config time). If that
// host is remote it is collected into remote_hosts for the caller to route to
// its owning rank; if it is dead its rank just leaves it out of the per-slot
// location broadcast, and the follower keeps its own schedule.
// follower_excl / host_excl carry the people already claimed by earlier rules:
// a candidate follower in either set is skipped (it cannot follow twice nor
// host), a candidate host in host_excl is skipped (a follower cannot host). For
// a network pool host_excl is the global follower set, so a remote partner that
// follows under an earlier rule is excluded at any rank count.
void rebuildCriteriaBindings(
    WorldState& world, const FollowConfig& fc,
    std::unordered_map<PersonId, PersonId>& follower_host,
    std::unordered_set<PersonId>& active_hosts,
    std::vector<std::pair<PersonId, PersonId>>* remote_hosts,
    std::unordered_map<PersonId, PersonId>* new_follows,
    const std::unordered_set<PersonId>& follower_excl,
    const std::unordered_set<PersonId>& host_excl) {
  follower_host.clear();
  active_hosts.clear();
  const bool net = fc.usesNetwork();
  for (const Person& f : world.people) {
    if (f.is_dead || !filtering::matchesCriteria(f, &world, fc.follower))
      continue;
    if (follower_excl.count(f.id)) continue;  // claimed by an earlier rule
    PersonId host = -1;
    for (PersonId m : gatherPool(world, fc, f)) {
      if (m == f.id || (host >= 0 && m >= host)) continue;
      if (host_excl.count(m)) continue;  // a follower elsewhere cannot host
      if (net) {
        host = m;  // lowest partner id; eligibility/liveness handled elsewhere
      } else {
        auto mi = world.person_index.find(m);
        if (mi == world.person_index.end()) continue;
        const Person& mp = world.people[mi->second];
        if (mp.is_dead || !filtering::matchesCriteria(mp, &world, fc.host))
          continue;
        host = m;
      }
    }
    if (host < 0) continue;
    follower_host[f.id] = host;
    if (new_follows) (*new_follows)[f.id] = host;
    // Activate the host so its location is available for mirroring. A local
    // host is added here; a remote one is routed to its owning rank.
    if (world.person_index.count(host))
      active_hosts.insert(host);
    else if (remote_hosts)
      remote_hosts->push_back({f.id, host});
  }
}

// A host passes its once-per-trip probability roll. Deterministic per
// (seed, host, hop-start day) so the same host commits the same way at any rank
// count and stays committed for every day of a multi-day trip.
bool hostRollsFollow(const FollowConfig& fc, uint64_t seed, PersonId host,
                     int hop_start_day) {
  if (fc.probability >= 1.0) return true;
  SplitMix64 rng(mix_seed(seed, static_cast<uint64_t>(host),
                          static_cast<uint64_t>(hop_start_day)));
  double u =
      static_cast<double>(rng()) / static_cast<double>(SplitMix64::max());
  return u < fc.probability;
}

// A host rolls once to gather followers from its pool. For a hop span the host
// must be on a trip and the roll is keyed on the trip's start day, so a trip
// resumed from a checkpoint mid-way re-rolls the same way. For a standing span
// there is no trip: any alive host rolls once, keyed on (seed, host) with a
// fixed day of 0, so re-considering it each slot is idempotent and the bond
// simply persists once formed. A host already tried, someone already following
// (or away on their own trip, or dead) is never enrolled. Only pool members
// local to this rank are enrolled here; remote network partners are collected
// into remote_invites for the caller to route.
// Returns {hosts that gained followers, total local followers enrolled}.
// follower_excl / host_excl carry the people already claimed by earlier rules
// (see rebuildCriteriaBindings). A host in host_excl is skipped (a follower
// elsewhere cannot host); a candidate follower in follower_excl is skipped. A
// remote follower is checked again on its own rank in applyFollowInvites.
std::pair<int, int> enrolFollowHosts(
    WorldState& world, const FollowConfig& fc, const ScheduleConfig& sched,
    uint64_t seed, bool standing, std::unordered_set<PersonId>& active_hosts,
    std::unordered_map<PersonId, PersonId>& follower_host, int current_day,
    std::vector<std::pair<PersonId, PersonId>>* remote_invites,
    std::unordered_map<PersonId, PersonId>* new_follows,
    const std::unordered_set<PersonId>& follower_excl,
    const std::unordered_set<PersonId>& host_excl) {
  int hosts = 0, followers = 0;
  for (Person& host : world.people) {
    if (host.is_dead) continue;
    if (!standing && !host.schedule_hop.isActive()) continue;
    if (active_hosts.count(host.id) || follower_host.count(host.id)) continue;
    if (host_excl.count(host.id)) continue;  // a follower elsewhere cannot host
    active_hosts.insert(host.id);

    int roll_day = 0;
    if (!standing) {
      int hopped = host.schedule_hop.hopped_schedule_id;
      int n =
          (hopped >= 0 &&
           hopped < static_cast<int>(sched.schedule_types.size()))
              ? static_cast<int>(sched.schedule_types[hopped].flat_slots.size())
              : 1;
      if (n < 1) n = 1;
      roll_day = ScheduleHop::hopStartDay(current_day, static_cast<int16_t>(n),
                                          host.schedule_hop.temp_slot_progress);
    }
    if (!hostRollsFollow(fc, seed, host.id, roll_day)) continue;

    int enrolled = 0;
    for (PersonId m : gatherPool(world, fc, host)) {
      if (m == host.id) continue;
      if (follower_excl.count(m)) continue;  // claimed by an earlier rule
      // A committed host never also follows someone. On a hop span that falls
      // out of the on-hop check below (a host is on its trip). A standing host
      // has no such signal, so test its roll directly: it is a pure function of
      // the person, so every rank agrees on who is a host and the host/follower
      // split does not depend on the order people are visited.
      if (standing && hostRollsFollow(fc, seed, m, 0)) continue;
      // A follower eligible for several hosts follows the smallest host_id.
      // This tiebreak is order-independent, so the choice is the same however
      // the world is partitioned across ranks.
      auto existing = follower_host.find(m);
      if (existing != follower_host.end() && existing->second <= host.id)
        continue;
      auto mi = world.person_index.find(m);
      if (mi == world.person_index.end()) {
        // Not local. For a network pool this partner lives in another domain;
        // route an invite to their rank. Venue pools are co-resident, so a
        // non-local member cannot occur there.
        if (remote_invites) remote_invites->push_back({m, host.id});
        continue;
      }
      const Person& mp = world.people[mi->second];
      if (mp.is_dead || mp.schedule_hop.isActive()) continue;
      follower_host[m] = host.id;
      if (new_follows) (*new_follows)[m] = host.id;
      ++enrolled;
    }
    if (enrolled > 0) {
      ++hosts;
      followers += enrolled;
    }
  }
  return {hosts, followers};
}

}  // namespace follow_detail

namespace {

#ifdef USE_MPI
// Allgatherv a flat int array; every rank receives all ranks' contributions
// concatenated in rank order.
std::vector<int> allgathervInts(const std::vector<int>& local) {
  int nr;
  MPI_Comm_size(MPI_COMM_WORLD, &nr);
  int local_n = static_cast<int>(local.size());
  std::vector<int> sizes(nr);
  MPI_Allgather(&local_n, 1, MPI_INT, sizes.data(), 1, MPI_INT, MPI_COMM_WORLD);
  std::vector<int> displs;
  int total = 0;
  mpi_utils::computeDisplacements(sizes, displs, total);
  std::vector<int> all(total);
  MPI_Allgatherv(const_cast<int*>(local.data()), local_n, MPI_INT, all.data(),
                 sizes.data(), displs.data(), MPI_INT, MPI_COMM_WORLD);
  return all;
}

// Gather the union of a per-rank id set so every rank sees the same global set.
std::unordered_set<PersonId> allgathervPersonSet(
    const std::unordered_set<PersonId>& local_set) {
  std::vector<int> local(local_set.begin(), local_set.end());
  std::vector<int> all = allgathervInts(local);
  return std::unordered_set<PersonId>(all.begin(), all.end());
}

// Route network follow invites across ranks: apply each (follower, host) pair
// whose follower is local, keeping the smallest host_id (order-independent, so
// identical at any rank count). A follower already claimed by an earlier rule
// on its own rank is dropped here, so exclusivity holds at any rank count.
void applyFollowInvites(
    const std::vector<std::pair<PersonId, PersonId>>& invites,
    WorldState& world, std::unordered_map<PersonId, PersonId>& follower_host,
    std::unordered_map<PersonId, PersonId>* new_follows,
    const std::unordered_set<PersonId>& follower_excl) {
  std::vector<int> local;
  local.reserve(invites.size() * 2);
  for (const auto& [f, h] : invites) {
    local.push_back(static_cast<int>(f));
    local.push_back(static_cast<int>(h));
  }
  std::vector<int> all = allgathervInts(local);
  for (size_t i = 0; i + 1 < all.size(); i += 2) {
    PersonId f = all[i], h = all[i + 1];
    if (follower_excl.count(f)) continue;  // claimed by an earlier rule
    auto existing = follower_host.find(f);
    if (existing != follower_host.end() && existing->second <= h) continue;
    auto mi = world.person_index.find(f);
    if (mi == world.person_index.end()) continue;  // not local to this rank
    const Person& fp = world.people[mi->second];
    if (fp.is_dead || fp.schedule_hop.isActive()) continue;
    follower_host[f] = h;
    if (new_follows) (*new_follows)[f] = h;
  }
}

// A network criteria follower picks its host locally, but that host may live on
// another rank. This gathers every (follower, host) pick and each rank adds to
// its active set the hosts it owns, so their locations get broadcast for
// mirroring. The follower side of the pick was already recorded locally.
void activateRemoteCriteriaHosts(
    const std::vector<std::pair<PersonId, PersonId>>& picks, WorldState& world,
    std::unordered_set<PersonId>& active_hosts) {
  std::vector<int> local;
  local.reserve(picks.size() * 2);
  for (const auto& [f, h] : picks) {
    local.push_back(static_cast<int>(f));
    local.push_back(static_cast<int>(h));
  }
  std::vector<int> all = allgathervInts(local);
  for (size_t i = 0; i + 1 < all.size(); i += 2) {
    PersonId h = all[i + 1];
    if (world.person_index.count(h)) active_hosts.insert(h);
  }
}

// Broadcast the hosts that are active this slot so remote followers can mirror.
// The caller passes active_now already holding this rank's active hosts; each
// is sent with its location and the received remote hosts are added back into
// active_now (and, where they have a venue, host_loc). The host's activity
// travels too so the activity exception works cross-rank. Sending only the
// slot-active hosts (not every enrolled host) keeps an off-hop host from being
// mirrored on a hop span.
//
// Ints per host on the wire. Shared by the packer, the bounds guard and the
// unpack stride so the two halves cannot drift.
constexpr size_t kHostLocationInts = 5;

void broadcastHostLocations(WorldState& world,
                            const std::vector<PersonLocation>& locations,
                            std::unordered_map<PersonId, HostSlot>& host_loc,
                            std::unordered_set<PersonId>& active_now) {
  std::vector<int> local;
  for (PersonId h : active_now) {
    auto hi = world.person_index.find(h);
    if (hi == world.person_index.end()) continue;
    const PersonLocation& hl = locations[hi->second];
    local.push_back(static_cast<int>(h));
    local.push_back(static_cast<int>(hl.venue_id));
    local.push_back(static_cast<int>(hl.subset_index));
    local.push_back(static_cast<int>(hl.activity_index));
    local.push_back(static_cast<int>(world.getVenueTypeId(hl.venue_id)));
  }
  std::vector<int> all = allgathervInts(local);
  for (size_t i = 0; i + kHostLocationInts <= all.size();
       i += kHostLocationInts) {
    PersonId h = all[i];
    VenueId v = all[i + 1];
    SubsetIndex s = all[i + 2];
    int16_t act = static_cast<int16_t>(all[i + 3]);
    uint8_t venue_type = static_cast<uint8_t>(all[i + 4]);
    active_now.insert(h);
    if (v < 0) continue;
    // The sender typed this venue against its own all-venue type map, and the
    // v < 0 check above has already excluded venue-less hosts, so an unknown
    // type arriving here means the wire and the sender's venue registry have
    // diverged.
    if (venue_type == kUnknownVenueTypeId)
      throw std::runtime_error(
          "follow: host " + std::to_string(h) +
          " was broadcast with an unresolvable venue type for venue " +
          std::to_string(v));
    host_loc[h] = {v, s, act, venue_type};
  }
}
#endif  // USE_MPI

}  // namespace

void Simulator::injectFollowsIntoSlot(int time_slot_index) {
  const auto& rules = config_.coordinated_encounters.follows;
  if (rules.empty()) return;
  if (follow_state_.size() != rules.size()) follow_state_.resize(rules.size());

  const int day = static_cast<int>(current_simulation_time_);

  // The rules run in config order, carrying two sets forward: everyone an
  // earlier rule has bound as a host, and everyone it has bound as a follower.
  // A later rule yields to them, so a follower belongs to the first rule that
  // binds it, a host may recur across rules, and no one is ever both. Config
  // order is fixed on every rank, so the whole resolution is the same at any
  // rank count.
  std::unordered_set<PersonId> committed_hosts, committed_followers;
  std::vector<std::pair<PersonId, PersonId>> cotravel;
  for (size_t ri = 0; ri < rules.size(); ++ri) {
    const FollowConfig& fc = rules[ri];
    if (!fc.enabled) continue;
    processFollowRule(time_slot_index, day, static_cast<uint8_t>(ri), fc,
                      follow_state_[ri], committed_hosts, committed_followers,
                      cotravel);
  }

  // Followers bound for a line take over their host's rider entries, so they
  // board the same runtime groups at the same time. Done once for all rules,
  // since it costs one exchange between ranks.
  if (runtime_group_allocator_)
    runtime_group_allocator_->attachFollowers(cotravel);
}

void Simulator::processFollowRule(
    int time_slot_index, int day, uint8_t rule_id, const FollowConfig& fc,
    FollowRuntime& st, std::unordered_set<PersonId>& committed_hosts,
    std::unordered_set<PersonId>& committed_followers,
    std::vector<std::pair<PersonId, PersonId>>& cotravel) {
  // Network pools may enrol partners in other domains; venue pools are always
  // co-resident, so they need no cross-rank routing or per-slot broadcast.
  bool cross_rank = false;
#ifdef USE_MPI
  cross_rank = domain_mgr_ != nullptr && fc.usesNetwork();
#endif

  // A candidate follower is barred if it already hosts or follows elsewhere; a
  // candidate host is barred only if it already follows elsewhere (a follower
  // cannot host — that is the chain we forbid — but a host may gain a second
  // party). For a network pool the host may be a partner on another rank, so
  // the host bar uses the global follower set, so the same partners are
  // excluded at any rank count.
  std::unordered_set<PersonId> follower_excl = committed_followers;
  follower_excl.insert(committed_hosts.begin(), committed_hosts.end());
  const std::unordered_set<PersonId>* host_excl = &committed_followers;
  std::unordered_set<PersonId> global_followers;
#ifdef USE_MPI
  if (cross_rank) {
    global_followers = allgathervPersonSet(committed_followers);
    host_excl = &global_followers;
  }
#endif

  // A binding this rule already holds may have just been claimed by an earlier
  // rule; drop it so exclusivity holds. Anyone now following elsewhere goes,
  // and any host that has become a follower elsewhere goes too, releasing its
  // own followers with it. A criteria rebuild would redo this, but it only runs
  // on a new day, so this also covers a criteria rule between rebuilds.
  for (auto it = st.follower_host.begin(); it != st.follower_host.end();) {
    if (follower_excl.count(it->first))
      it = st.follower_host.erase(it);
    else
      ++it;
  }
  for (auto it = st.active_hosts.begin(); it != st.active_hosts.end();) {
    if (host_excl->count(*it)) {
      PersonId h = *it;
      for (auto f = st.follower_host.begin(); f != st.follower_host.end();) {
        if (f->second == h)
          f = st.follower_host.erase(f);
        else
          ++f;
      }
      it = st.active_hosts.erase(it);
    } else {
      ++it;
    }
  }

  // 1. Form the bindings. Criteria has no randomness, so its whole set of
  //    bindings is rebuilt once a day; being derived rather than rolled, a
  //    fresh run and one resumed from a checkpoint arrive at the same bindings
  //    with nothing saved. Stochastic instead enrols each host once (as a trip
  //    starts for a hop span, or once up front for a standing span) and keeps
  //    it, serialising the result. In both cases a network pool may reach
  //    across ranks: criteria routes each remote host to its owner so its
  //    location is broadcast, stochastic routes each remote follower to its
  //    owner.
  bool span_standing = fc.span == FollowConfig::Span::Standing;
  std::vector<std::pair<PersonId, PersonId>> remote_pairs;
  std::unordered_map<PersonId, PersonId> new_follows;
  if (fc.usesCriteria()) {
    if (day != st.follow_day) {
      st.follow_day = day;
      follow_detail::rebuildCriteriaBindings(
          world_, fc, st.follower_host, st.active_hosts,
          cross_rank ? &remote_pairs : nullptr, fc.log ? &new_follows : nullptr,
          follower_excl, *host_excl);
#ifdef USE_MPI
      if (cross_rank)
        activateRemoteCriteriaHosts(remote_pairs, world_, st.active_hosts);
#endif
    }
  } else {
    follow_detail::enrolFollowHosts(
        world_, fc, config_.schedule, config_.simulation.random_seed,
        span_standing, st.active_hosts, st.follower_host, day,
        cross_rank ? &remote_pairs : nullptr, fc.log ? &new_follows : nullptr,
        follower_excl, *host_excl);
#ifdef USE_MPI
    if (cross_rank)
      applyFollowInvites(remote_pairs, world_, st.follower_host,
                         fc.log ? &new_follows : nullptr, follower_excl);
#endif
  }

  // Log each newly-established follow on the follower's rank (single log; event
  // shards merge at run end). These go to /events/follows, not the encounter
  // dataset: a follow is a binding, not a negotiated encounter, and a host's
  // travel party is recovered by grouping on (rule_id, host).
  if (fc.log) {
    for (const auto& [follower, host] : new_follows)
      event_logger_.logFollow(host, follower, current_simulation_time_, rule_id,
                              time_slot_index);
  }

  // 2. Which hosts are placed somewhere this slot, and where? Local hosts come
  //    from locations_; remote (network) hosts from a broadcast. A hop-span
  //    host counts while its trip runs; a standing host counts while it is
  //    alive. For a stochastic binding a host that has dropped out is removed
  //    here so it stops being broadcast and its followers get released below. A
  //    criteria binding is owned by the daily rebuild, so its set is left
  //    untouched: an off-hop or just-dead host is simply skipped this slot (its
  //    followers keep their own schedule) and the next rebuild revises the set.
  bool criteria = fc.usesCriteria();
  std::unordered_map<PersonId, HostSlot> host_loc;
  std::unordered_set<PersonId> active_now;
  for (auto it = st.active_hosts.begin(); it != st.active_hosts.end();) {
    auto hi = world_.person_index.find(*it);
    bool live =
        hi != world_.person_index.end() && !world_.people[hi->second].is_dead &&
        (span_standing || world_.people[hi->second].schedule_hop.isActive());
    if (!live) {
      if (criteria) {
        ++it;
        continue;
      }
      it = st.active_hosts.erase(it);
      continue;
    }
    active_now.insert(*it);
    const PersonLocation& hl = locations_[hi->second];
    if (hl.venue_id >= 0)
      host_loc[*it] = {hl.venue_id, hl.subset_index, hl.activity_index,
                       world_.getVenueTypeId(hl.venue_id)};
    ++it;
  }
#ifdef USE_MPI
  if (cross_rank)
    broadcastHostLocations(world_, locations_, host_loc, active_now);
#endif

  // 3. Release followers whose host has dropped out. Only stochastic bindings
  //    are consumed this way; a criteria binding persists until the daily
  //    rebuild, so nothing is erased here for it.
  if (!criteria) {
    for (auto f = st.follower_host.begin(); f != st.follower_host.end();) {
      if (!active_now.count(f->second))
        f = st.follower_host.erase(f);
      else
        ++f;
    }
  }

  // 4. Mirror each follower onto its host's location for this slot.
  for (const auto& [follower, host] : st.follower_host) {
    auto fi = world_.person_index.find(follower);
    if (fi == world_.person_index.end() || world_.people[fi->second].is_dead)
      continue;
    auto hl = host_loc.find(host);
    if (hl == host_loc.end()) continue;  // host active but no venue this slot

    PersonLocation& floc = locations_[fi->second];
    // Off the wire, not a local lookup: a cross-rank host's venue is outside
    // this rank's halo and would type as unresolvable here.
    const uint8_t host_venue_type = hl->second.venue_type;
    if (follow_detail::mirrorSuppressed(fc, hl->second.activity,
                                        host_venue_type, floc.activity_index))
      continue;
    // The follower's own policy wins. If a policy would move them (sick and
    // sent home, say), decline the mirror and leave them on their own
    // schedule — ActivityManager has already placed them, and declining to
    // mirror moves nobody anywhere. The question is asked about the host's
    // venue type, since that is where the mirror is about to put them; a
    // venue-gated policy asked about their own venue would miss. Only the
    // named leg is gated — a partial-presence host's other legs are handled by
    // the venue exceptions below.
    if (follow_detail::policySuppressesMirror(
            policy_manager_.get(), world_.people[fi->second],
            floc.activity_index, host_venue_type, current_simulation_time_))
      continue;

    // Travelling with the host means riding the host's whole journey, not the
    // single leg their location happens to name. A journey is all or nothing:
    // if any leg of it is a venue type this rule excepts, the follower stays on
    // its own schedule, because a child cannot be dropped at a bus stop and
    // rejoin two legs later.
    if (runtime_group_allocator_ &&
        runtime_group_allocator_->isPartialPresenceVenue(hl->second.venue)) {
      const auto& legs = runtime_group_allocator_->legsOf(host);
      bool excepted = false;
      for (VenueId leg : legs) {
        // The host's line may belong to another rank, whose venue types this
        // rank does not hold, so take the type the rider broadcast carried.
        if (follow_detail::venueExcepted(
                fc, runtime_group_allocator_->venueTypeOf(leg))) {
          excepted = true;
          break;
        }
      }
      if (excepted) continue;
      cotravel.emplace_back(follower, host);
    }

    // Copy the host's (venue, subset). The host resolved the subset against
    // this venue, so it is valid there and identical at any rank count.
    floc.venue_id = hl->second.venue;
    floc.encounter_type_id = fc.encounter_type_id;
    floc.subset_index = hl->second.subset;
  }

  // This rule's hosts and followers are now committed, so later rules skip
  // them.
  committed_hosts.insert(st.active_hosts.begin(), st.active_hosts.end());
  for (const auto& [follower, host] : st.follower_host)
    committed_followers.insert(follower);
}

}  // namespace june
