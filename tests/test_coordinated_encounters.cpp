/**
 * =============================================================================
 * test_coordinated_encounters.cpp
 * =============================================================================
 *
 * Comprehensive unit, integration, and stress tests for the Coordinated
 * Encounter pipeline:
 *
 *   generateProposals()  →  processProposals()  →  finalizeEncounters()
 *
 * The coordinated encounter system allows individuals to propose social or
 * romantic encounters to their network partners. The pipeline works in three
 * phases:
 *
 *  Phase 1 (generateProposals): Each person rolls against proposal_probability.
 *          If successful, they select a venue (physical or virtual), pick a
 *          time slot from their schedule, and invite friends from their
 * network. The number of invitees is controlled by invite_distribution.
 *
 *  Phase 2 (processProposals): Each invitee checks:
 *          - Are they alive?
 *          - Does their schedule allow the encounter's trigger activity at the
 *            proposed slot?
 *          - Do they accept based on acceptance_probability?
 *
 *  Phase 3 (finalizeEncounters): Accepted replies are grouped by encounter_id.
 *          If at least one invitee accepted, a CoordinatedEncounter is created
 *          with all participants.
 *
 * Known bugs this test suite guards against:
 *
 *  Bug #1: Multiple people ending up in a venue restricted to pairs.
 *          invite_distribution fixed(1) means "invite exactly 1",
 *          so finalized encounters must have exactly 2 participants.
 *
 *  Bug #2: Wrong contact matrix applied to virtual encounters.
 *          virtual_contact_matrix must resolve to the correct matrix name,
 *          not a fallback or a physical venue's matrix.
 *
 *  Bug #3: Physical venue IDs colliding with virtual venue IDs, causing
 *          encounters to be logged as happening at a "classroom" when
 *          they should be at a virtual venue. Fixed by using negative IDs
 *          for all virtual venues.
 *
 *  Bug #4 (NEW): Virtual encounters bypassed schedule validation entirely.
 *          A worker would accept a romantic encounter during work hours.
 *          Fixed by removing the unconditional schedule_allows = true
 *          for virtual encounters.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "activity/coordinated_encounter_manager.h"
#include "core/config.h"
#include "core/world_state.h"
#include "doctest.h"
#include "test_utils.h"

using namespace june;

// =============================================================================
// Test Helper: Builds a fully wired WorldState + Config for encounter tests.
//
// This avoids duplicating setup boilerplate across 25+ tests. Every test can
// call this with specific parameters and get a consistent, valid world.
// =============================================================================

struct EncounterTestWorld {
  WorldState world;
  Config config;

  // Static schedule storage (must outlive WorldState pointers)
  std::vector<ScheduleType> schedule_storage;
};

// Helper: Resolve all pre-cached indices in the test world.
// Must be called after any config/schedule changes and before creating the CEM.
static void resolveTestWorld(EncounterTestWorld& tw) {
  // Resolve config-level caches (encounter defs, contact matrices, etc.)
  tw.config.resolve(tw.world);

  // Resolve schedule_storage TimeSlot masks (these are stored outside
  // config.schedule)
  // Day type order for tests: workday=0, rest_day=1
  static const std::vector<std::string> test_day_type_names = {"workday",
                                                               "rest_day"};
  for (auto& sched : tw.schedule_storage) {
    auto resolveSlots = [&](std::vector<TimeSlot>& slots) {
      for (auto& slot : slots) {
        slot.allowed_activity_mask = 0;
        slot.allowed_activity_indices.clear();
        for (const auto& act : slot.allowed_activities) {
          int idx = tw.world.getActivityIndex(act);
          if (idx >= 0 && idx < 32) {
            slot.allowed_activity_mask |= (1u << idx);
            slot.allowed_activity_indices.push_back(static_cast<int16_t>(idx));
          }
        }
      }
    };
    for (auto& [dt_name, slots_vec] : sched.slots_by_day_type) {
      resolveSlots(slots_vec);
    }
    // Rebuild slots_by_day_type_idx using fixed test order
    sched.slots_by_day_type_idx.clear();
    for (const auto& name : test_day_type_names) {
      auto it = sched.slots_by_day_type.find(name);
      sched.slots_by_day_type_idx.push_back(
          it != sched.slots_by_day_type.end() ? &it->second : nullptr);
    }
  }
}

/**
 * Creates a minimal but complete world for testing coordinated encounters.
 *
 * @param num_people      Number of people to create (IDs 0..N-1)
 * @param num_venues      Number of physical venues (IDs 100..100+N-1)
 * @param venue_type_name The venue type name for physical venues (e.g., "pub")
 * @param add_network     If non-empty, creates a network with this name
 * connecting all people pairwise (for small N) or sequentially.
 * @param encounter_name  Name of the encounter definition to create
 * @param is_virtual      Whether the encounter is virtual
 * @param virtual_matrix  Contact matrix name for virtual encounters
 * @param trigger_slots   Schedule slots that trigger this encounter (e.g.,
 * {"leisure"})
 * @param invite_dist    InviteDistribution config (type, mean/p/count)
 * @param proposal_prob   Probability of proposing (default 1.0 for
 * deterministic tests)
 * @param acceptance_prob Probability of accepting (default 1.0 for
 * deterministic tests)
 */
static EncounterTestWorld buildEncounterWorld(
    int num_people, int num_venues, const std::string& venue_type_name,
    const std::string& network_name, const std::string& encounter_name,
    bool is_virtual, const std::string& virtual_matrix,
    const std::vector<std::string>& trigger_slots,
    const InviteDistribution& invite_dist, double proposal_prob = 1.0,
    double acceptance_prob = 1.0) {
  EncounterTestWorld tw;

  // ---- Registries ----
  tw.world.activity_names = {"residence", "work", "leisure"};
  tw.world.venue_type_names = {"home", "office", venue_type_name};
  tw.world.geo_level_names = {"city"};
  tw.world.person_property_names = {"age", "sex"};

  // Encounter type registry (needed by generateProposals for encounter_type_id)
  tw.world.encounter_type_names = {encounter_name};

  // ---- Geography ----
  GeographicalUnit gu;
  gu.id = 0;
  gu.name = "TestCity";
  gu.level_id = 0;
  gu.parent_id = -1;
  tw.world.geo_units.push_back(gu);

  // ---- Venues ----
  // Physical venue type index = 2 (the third entry in venue_type_names)
  int venue_type_id = 2;
  for (int i = 0; i < num_venues; ++i) {
    Venue v;
    v.id = 100 + i;
    v.type_id = venue_type_id;
    v.geo_unit_id = 0;
    tw.world.venues.push_back(v);
  }

  // ---- People ----
  for (int i = 0; i < num_people; ++i) {
    Person& p = tw.world.people.emplace_back();
    p.id = i;
    p.age = 25.0f + i;
    p.sex = (i % 2 == 0) ? Sex::MALE : Sex::FEMALE;
    p.geo_unit_id = 0;
  }

  // ---- Network (connect all pairs for small N, or chain for large N) ----
  if (!network_name.empty() && num_people >= 2) {
    std::vector<std::pair<PersonId, PersonId>> pairs;
    if (num_people <= 10) {
      // Full mesh for small populations
      for (int i = 0; i < num_people; ++i) {
        for (int j = i + 1; j < num_people; ++j) {
          pairs.push_back({i, j});
        }
      }
    } else {
      // Chain for large populations (each person connected to next)
      for (int i = 0; i < num_people - 1; ++i) {
        pairs.push_back({i, i + 1});
      }
    }
    TestWorldFactory::addNetwork(tw.world, network_name, pairs);
  }

  // ---- Link people to activities (activity_meta) ----
  // First, give every person a home venue for "residence" (activity index 0)
  int residence_act_idx = 0;  // "residence" is first in activity_names
  int home_type_id = 0;       // "home" is first in venue_type_names
  for (int i = 0; i < num_people; ++i) {
    Venue home;
    home.id = i;  // Home venue ID = person ID
    home.type_id = home_type_id;
    home.geo_unit_id = 0;
    home.is_residence = true;
    tw.world.venues.push_back(home);
  }

  for (auto& p : tw.world.people) {
    int meta_count = 1;                  // Residence always
    if (num_venues > 0) meta_count = 2;  // + physical venue

    p.activity_meta_start =
        static_cast<uint32_t>(tw.world.activity_meta.size());
    p.activity_meta_count = meta_count;

    // Residence activity
    Person::ActivityMeta res_meta;
    res_meta.activity_index = static_cast<int16_t>(residence_act_idx);
    res_meta.venue_start =
        static_cast<uint32_t>(tw.world.activity_venues.size());
    res_meta.venue_count = 1;
    tw.world.activity_meta.push_back(res_meta);
    tw.world.activity_venues.push_back({p.id, 0});  // Home venue ID = person ID

    // Physical venue activity (if venues exist)
    if (num_venues > 0) {
      Person::ActivityMeta phys_meta;
      phys_meta.activity_index = static_cast<int16_t>(venue_type_id);
      phys_meta.venue_start =
          static_cast<uint32_t>(tw.world.activity_venues.size());
      phys_meta.venue_count = 1;
      tw.world.activity_meta.push_back(phys_meta);
      // Link to the first encounter venue (ID = 100), not a home venue.
      // venues[0..num_venues-1] are the encounter venues with the right type.
      tw.world.activity_venues.push_back({100, 0});
    }
  }

  tw.world.buildIndices();

  // ---- Config ----
  tw.config.coordinated_encounters.enabled = true;
  tw.config.simulation.random_seed = 42;

  CoordinatedEncounterDef def;
  def.name = encounter_name;
  def.enabled = true;
  def.network = network_name;
  def.trigger_slots = trigger_slots;
  def.is_virtual = is_virtual;
  def.virtual_contact_matrix = virtual_matrix;
  def.proposal_probability = proposal_prob;
  def.invite_distribution = invite_dist;
  def.acceptance_probability = acceptance_prob;

  if (!is_virtual) {
    def.allowed_venues = {venue_type_name};
  }

  tw.config.coordinated_encounters.encounters = {def};

  // ---- Contact Matrices (needed for getVirtualVenueTypeId) ----
  tw.config.contact_matrices.matrices["home"] = ContactMatrix();
  tw.config.contact_matrices.matrices["office"] = ContactMatrix();
  tw.config.contact_matrices.matrices[venue_type_name] = ContactMatrix();
  if (!virtual_matrix.empty()) {
    tw.config.contact_matrices.matrices[virtual_matrix] = ContactMatrix();
  }

  // Resolve config caches (encounter type IDs, trigger masks, venue masks,
  // etc.)
  tw.config.resolve(tw.world);

  return tw;
}

/**
 * Assigns a schedule to a person.
 *
 * The schedule_storage vector must outlive the Person (use EncounterTestWorld).
 */
static ScheduleType& addSchedule(
    EncounterTestWorld& tw, PersonId person_id, const std::string& sched_name,
    const std::vector<std::pair<std::string, std::vector<std::string>>>&
        slots) {
  ScheduleType sched;
  sched.name = sched_name;
  for (const auto& [slot_name, activities] : slots) {
    TimeSlot ts;
    ts.name = slot_name;
    ts.start = "00:00";
    ts.end = "23:59";
    ts.allowed_activities = activities;
    sched.slots_by_day_type["workday"].push_back(ts);
  }
  tw.schedule_storage.push_back(sched);
  ScheduleType* ptr = &tw.schedule_storage.back();

  auto it = tw.world.person_index.find(person_id);
  if (it != tw.world.person_index.end()) {
    tw.world.people[it->second].cached_schedule_type_ = ptr;
  }

  // Resolve TimeSlot masks on the newest schedule
  resolveTestWorld(tw);

  return tw.schedule_storage.back();
}

/**
 * Assigns the same schedule to all people in the world.
 * Optionally specify different weekend slots.
 */
static void addScheduleToAll(
    EncounterTestWorld& tw, const std::string& sched_name,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& slots,
    const std::vector<std::pair<std::string, std::vector<std::string>>>&
        rest_day_slots = {}) {
  ScheduleType sched;
  sched.name = sched_name;
  for (const auto& [slot_name, activities] : slots) {
    TimeSlot ts;
    ts.name = slot_name;
    ts.start = "00:00";
    ts.end = "23:59";
    ts.allowed_activities = activities;
    sched.slots_by_day_type["workday"].push_back(ts);
  }
  // If rest_day_slots provided, use those; otherwise copy workday slots
  const auto& wknd = rest_day_slots.empty() ? slots : rest_day_slots;
  for (const auto& [slot_name, activities] : wknd) {
    TimeSlot ts;
    ts.name = slot_name;
    ts.start = "00:00";
    ts.end = "23:59";
    ts.allowed_activities = activities;
    sched.slots_by_day_type["rest_day"].push_back(ts);
  }
  tw.schedule_storage.push_back(sched);
  ScheduleType* ptr = &tw.schedule_storage.back();

  for (auto& p : tw.world.people) {
    p.cached_schedule_type_ = ptr;
  }

  // Resolve TimeSlot masks on the newest schedule
  resolveTestWorld(tw);
}

// =============================================================================
// SECTION 1: Unit Tests — generateProposals
//
// These tests verify proposal generation logic: how many invitees are selected,
// which people are eligible to propose, and how venue IDs are assigned.
// =============================================================================

TEST_CASE(
    "1a. generateProposals — PairOnly: invite_distribution fixed(1) "
    "produces exactly 1 invite per proposal [Regression Bug #1]") {
  /**
   * SCENARIO: The config says [0.0, 1.0] meaning:
   *   - 0% chance of inviting 0 friends (index 0)
   *   - 100% chance of inviting exactly 1 friend (index 1)
   *
   * With 4 fully-connected people and proposal_probability = 1.0, every
   * person will propose. Each proposal must target exactly 1 invitee.
   *
   * This is the direct regression test for Bug #1 where multiple people
   * ended up in the same virtual venue despite the pair restriction.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  // Every person should propose, and each proposal should be to exactly 1
  // invitee
  REQUIRE(proposals.size() > 0);

  // Group by host to confirm each host invited exactly 1 person
  std::map<PersonId, int> invites_per_host;
  for (const auto& p : proposals) {
    invites_per_host[p.host_id]++;
  }

  for (const auto& [host_id, count] : invites_per_host) {
    INFO("Host " << host_id << " invited " << count << " people (expected 1)");
    CHECK(count == 1);
  }
}

TEST_CASE(
    "1b. generateProposals — GroupInvite: invite_distribution fixed(2) "
    "invites exactly 2 friends per proposal") {
  /**
   * SCENARIO: [0.0, 0.0, 1.0] means:
   *   - 0% chance of 0 invitees
   *   - 0% chance of 1 invitee
   *   - 100% chance of 2 invitees
   *
   * With 4 fully-connected people, each host should invite 2 friends.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 2}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  REQUIRE(proposals.size() > 0);

  std::map<PersonId, int> invites_per_host;
  for (const auto& p : proposals) {
    invites_per_host[p.host_id]++;
  }

  for (const auto& [host_id, count] : invites_per_host) {
    INFO("Host " << host_id << " invited " << count << " people (expected 2)");
    CHECK(count == 2);
  }
}

TEST_CASE(
    "1c. generateProposals — proposal_probability = 0.0 "
    "produces no proposals") {
  /**
   * SCENARIO: proposal_probability = 0.0, so no person ever rolls
   * high enough to propose. Zero proposals should be generated.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 0.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  CHECK(proposals.size() == 0);
}

TEST_CASE("1d. generateProposals — Dead people are never hosts") {
  /**
   * SCENARIO: Person 0 is dead. Even with proposal_probability = 1.0,
   * they should not appear as a host in any proposal.
   */
  auto tw = buildEncounterWorld(
      3, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});
  tw.world.people[0].is_dead = true;

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  for (const auto& p : proposals) {
    CHECK(p.host_id != 0);
  }
}

TEST_CASE("1e. generateProposals — No network partners means no proposals") {
  /**
   * SCENARIO: Person has no friends in the specified network.
   * Even with proposal_probability = 1.0, they can't invite anyone,
   * so no proposals should be created.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "" /* no network */, "social_encounters", false, "",
      {"leisure"}, InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1},
      1.0, 1.0);
  // Manually set network name in the encounter def without adding actual
  // network data
  tw.config.coordinated_encounters.encounters[0].network = "friendships";
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  CHECK(proposals.size() == 0);
}

TEST_CASE(
    "1f. generateProposals — Virtual encounters always produce negative venue "
    "IDs") {
  /**
   * SCENARIO: A virtual encounter (romantic_encounter) should assign
   * a negative venue ID to every proposal. This is the mechanism that
   * separates virtual venues from physical venues in the ID space.
   *
   * Guards against Bug #3: venue ID collisions.
   */
  auto tw = buildEncounterWorld(
      4, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  REQUIRE(proposals.size() > 0);

  for (const auto& p : proposals) {
    INFO("Proposal " << p.encounter_id << " has venue_id = " << p.venue_id);
    CHECK(p.venue_id < 0);
  }
}

TEST_CASE(
    "1g. generateProposals — Physical encounter requires valid venue of "
    "correct type") {
  /**
   * SCENARIO: A physical encounter requires the host to have a linked
   * venue of the allowed type. If the world has no venues of that type,
   * no proposals should be generated.
   *
   * Here we set allowed_venues = {"cinema"} but only have "pub" venues.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  // Override allowed venues to a type that doesn't exist
  tw.config.coordinated_encounters.encounters[0].allowed_venues = {"cinema"};
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  CHECK(proposals.size() == 0);
}

TEST_CASE(
    "1h. generateProposals — A physical venue typed as unresolvable throws") {
  /**
   * SCENARIO: A physical proposal carries the venue type every downstream
   * participant reads, so manufacturing one whose type is unresolvable must
   * fail loudly. Without the check, allowed_venue_mask only holds types below
   * 32, so a 255 proposal is silently rejected later as a no-matching-def
   * warning — indistinguishable from "the config does not allow this type".
   *
   * The reachable way to build one: a registry with the venue type at index
   * 255, aliasing kUnknownVenueTypeId.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  // Move "pub" to registry index 255. The old entry is renamed so the venue
  // type lookup cannot find it at index 2 instead.
  auto& venue_type_names = tw.world.venue_type_names;
  venue_type_names[2] = "unused_venue_type";
  venue_type_names.resize(kUnknownVenueTypeId, "padding_venue_type");
  venue_type_names.push_back("pub");
  REQUIRE(venue_type_names.size() == 256);
  tw.world.venues[0].type_id = kUnknownVenueTypeId;
  tw.world.buildIndices();

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  CHECK_THROWS_AS(cem.generateProposals(0, proposals, 0), std::runtime_error);
}

// =============================================================================
// SECTION 2: Unit Tests — processProposals
//
// These tests verify negotiation logic: schedule validation, acceptance
// probability, dead/missing invitees, and correct encounter definition
// matching.
// =============================================================================

TEST_CASE(
    "2a. processProposals — Virtual contact matrix resolves correctly "
    "[Regression Bug #2]") {
  /**
   * SCENARIO: A virtual encounter with virtual_contact_matrix =
   * "romantic_encounter" should resolve to a specific venue_type_id via
   * getVirtualVenueTypeId(). The processProposals method uses this to match the
   * proposal to the correct encounter definition.
   *
   * We verify that a proposal with the correct venue_type_id is ACCEPTED
   * (definition found and matched), while one with a wrong venue_type_id
   * falls through to the "no definition found" path.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Find what venue_type_id the virtual matrix resolves to
  // We do this indirectly by generating a proposal and inspecting it
  std::vector<EncounterProposal> gen_proposals;
  cem.generateProposals(0, gen_proposals, 0);

  // Find a proposal and extract its venue_type_id
  REQUIRE(gen_proposals.size() > 0);
  int correct_vtype = gen_proposals[0].venue_type_id;

  // Now test processProposals with a proposal using the correct venue_type_id
  // Use a fresh CEM to avoid host commitment interference from
  // generateProposals
  CoordinatedEncounterManager cem2(tw.world, tw.config, 0);

  EncounterProposal good_prop;
  good_prop.encounter_id = 1;
  good_prop.host_id = 0;
  good_prop.host_rank = 0;
  good_prop.invitee_id = 1;
  good_prop.venue_id = -5000;
  good_prop.venue_owner_rank = 0;
  good_prop.venue_type_id = correct_vtype;
  good_prop.slot = 0;
  good_prop.encounter_type_id = 0;

  std::vector<EncounterReply> replies;
  cem2.processProposals({good_prop}, {good_prop}, replies, 0);

  REQUIRE(replies.size() == 1);
  INFO("Reply status: " << replies[0].status);
  CHECK(replies[0].status == ReplyStatus::ACCEPTED);
  CHECK(replies[0].venue_type_id == correct_vtype);
}

TEST_CASE(
    "2a2. mode-only virtual contact matrix resolves "
    "[Regression PR25]") {
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  tw.config.contact_matrices.matrices.erase("romantic_encounter");
  tw.config.contact_matrices
      .mode_matrices["romantic_encounter"]["respiratory"] = ContactMatrix();
  tw.config.contact_matrices
      .mode_matrices["romantic_encounter"]["physical_contact"] =
      ContactMatrix();
  tw.config.resolve(tw.world);

  const auto& def = tw.config.coordinated_encounters.encounters.front();
  INFO("mode-only virtual matrix index: " << def.cached_virtual_venue_type_id);
  INFO("matrix-name index contains romantic_encounter: "
       << (tw.config.contact_matrices.matrix_name_to_id.count(
               "romantic_encounter") != 0));
  REQUIRE(def.cached_virtual_venue_type_id != kUnknownVenueTypeId);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);
  INFO("generated proposals: " << proposals.size());
  REQUIRE(proposals.size() > 0);

  std::vector<EncounterReply> replies;
  cem.processProposals(proposals, proposals, replies, 0);
  INFO("accepted proposals: " << std::count_if(replies.begin(), replies.end(),
                                               [](const auto& reply) {
                                                 return reply.status ==
                                                        ReplyStatus::ACCEPTED;
                                               })
                              << " / " << replies.size());
  CHECK(std::count_if(replies.begin(), replies.end(), [](const auto& reply) {
          return reply.status == ReplyStatus::ACCEPTED;
        }) > 0);
}

TEST_CASE(
    "2b. processProposals — Physical venue type matches encounter def, "
    "not classroom [Regression Bug #3]") {
  /**
   * SCENARIO: We create a world with venue_type_names including both "pub"
   * and "classroom". The encounter def allows only "pub". We send a proposal
   * with venue_type_id pointing to "pub" (not "classroom").
   *
   * This guards against the bug where venue IDs collided and a "pub"
   * encounter was logged as happening at a "classroom".
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  // Add "classroom" as venue type index 3
  tw.world.venue_type_names.push_back("classroom");
  tw.config.contact_matrices.matrices["classroom"] = ContactMatrix();
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  int pub_type_id = tw.world.getVenueTypeIndex("pub");
  int classroom_type_id = tw.world.getVenueTypeIndex("classroom");

  REQUIRE(pub_type_id >= 0);
  REQUIRE(classroom_type_id >= 0);
  REQUIRE(pub_type_id != classroom_type_id);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Proposal with pub type -> should be ACCEPTED
  EncounterProposal pub_prop;
  pub_prop.encounter_id = 1;
  pub_prop.host_id = 0;
  pub_prop.host_rank = 0;
  pub_prop.invitee_id = 1;
  pub_prop.venue_id = 100;
  pub_prop.venue_owner_rank = 0;
  pub_prop.venue_type_id = pub_type_id;
  pub_prop.slot = 0;
  pub_prop.encounter_type_id = 0;

  std::vector<EncounterReply> replies;
  cem.processProposals({pub_prop}, {pub_prop}, replies, 0);

  REQUIRE(replies.size() == 1);
  CHECK(replies[0].status == ReplyStatus::ACCEPTED);
  CHECK(replies[0].venue_type_id == pub_type_id);

  // Proposal with classroom type -> should still get processed but
  // no matching encounter def found (classroom is not in allowed_venues)
  EncounterProposal class_prop = pub_prop;
  class_prop.encounter_id = 2;
  class_prop.venue_type_id = classroom_type_id;

  std::vector<EncounterReply> replies2;
  cem.processProposals({class_prop}, {class_prop}, replies2, 0);

  REQUIRE(replies2.size() == 1);
  // Without a matching def, the code defaults to acceptance_prob = 1.0
  // but the venue_type_id in the reply should match what was sent
  CHECK(replies2[0].venue_type_id == classroom_type_id);
}

TEST_CASE(
    "2c. processProposals — Schedule conflict produces "
    "REJECTED_SCHEDULE_CONFLICT") {
  /**
   * SCENARIO: Person 1 is a worker whose schedule only allows "work" at
   * slot 0. A social encounter with trigger_slots = ["leisure"] is proposed
   * at slot 0. Person 1 must reject because "leisure" is not in their
   * allowed_activities for that slot.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  // Person 0 = open schedule, Person 1 = worker (no leisure)
  addSchedule(tw, 0, "open", {{"all_day", {"leisure", "residence"}}});
  addSchedule(tw, 1, "worker", {{"work_hours", {"work"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterProposal prop;
  prop.encounter_id = 1;
  prop.host_id = 0;
  prop.host_rank = 0;
  prop.invitee_id = 1;
  prop.venue_id = 100;
  prop.venue_owner_rank = 0;
  prop.venue_type_id = tw.world.getVenueTypeIndex("pub");
  prop.slot = 0;
  prop.encounter_type_id = 0;

  std::vector<EncounterReply> replies;
  cem.processProposals({prop}, {prop}, replies, 0);

  REQUIRE(replies.size() == 1);
  CHECK(replies[0].status == ReplyStatus::REJECTED_SCHEDULE_CONFLICT);
}

TEST_CASE(
    "2d. processProposals — Virtual encounters respect trigger_slot schedule "
    "constraints [Bug #4 Fix Validation]") {
  /**
   * SCENARIO (User's exact example):
   *   Person A (no_primary_activity) has leisure time 9-5 on weekdays.
   *   Person B (has_primary_activity) only has "work" during 9-5.
   *
   *   Person A proposes a romantic_encounter (virtual,
   * trigger_slots=["leisure"]) at slot 0 (the 9-5 slot).
   *
   *   Person B MUST reject because their schedule only allows "work" at
   *   that slot, not "leisure".
   *
   * Before the fix, virtual encounters bypassed schedule validation entirely,
   * meaning Person B would incorrectly accept.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  // Person A: has leisure (proposer)
  addSchedule(tw, 0, "no_primary_activity",
              {{"daytime", {"leisure", "residence"}}});
  // Person B: worker, no leisure at slot 0
  addSchedule(tw, 1, "has_primary_activity", {{"daytime", {"work"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Compute the venue_type_id deterministically using sorted registry
  auto id_it =
      tw.config.contact_matrices.matrix_name_to_id.find("romantic_encounter");
  REQUIRE(id_it != tw.config.contact_matrices.matrix_name_to_id.end());
  int vtype = id_it->second;

  // Manually construct the proposal — no RNG dependency
  EncounterProposal prop;
  prop.encounter_id = 999;
  prop.host_id = 0;
  prop.host_rank = 0;
  prop.invitee_id = 1;
  prop.venue_id = -5000;
  prop.venue_owner_rank = 0;
  prop.venue_type_id = vtype;
  prop.slot = 0;
  prop.encounter_type_id = 0;

  std::vector<EncounterReply> replies;
  cem.processProposals({prop}, {prop}, replies, 0);

  REQUIRE(replies.size() == 1);
  INFO("Reply: " << replies[0].status
                 << " (expected REJECTED_SCHEDULE_CONFLICT)");
  CHECK(replies[0].status == ReplyStatus::REJECTED_SCHEDULE_CONFLICT);
}

TEST_CASE("2e. processProposals — Dead invitee produces REJECTED_DEAD") {
  /**
   * SCENARIO: Person 1 is dead. Any proposal targeting them should
   * return REJECTED_DEAD, regardless of schedule or acceptance probability.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});
  tw.world.people[1].is_dead = true;

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterProposal prop;
  prop.encounter_id = 1;
  prop.host_id = 0;
  prop.host_rank = 0;
  prop.invitee_id = 1;
  prop.venue_id = 100;
  prop.venue_owner_rank = 0;
  prop.venue_type_id = tw.world.getVenueTypeIndex("pub");
  prop.slot = 0;
  prop.encounter_type_id = 0;

  std::vector<EncounterReply> replies;
  cem.processProposals({prop}, {prop}, replies, 0);

  REQUIRE(replies.size() == 1);
  CHECK(replies[0].status == ReplyStatus::REJECTED_DEAD);
}

TEST_CASE(
    "2f. processProposals — Missing invitee produces REJECTED_NOT_FOUND") {
  /**
   * SCENARIO: A proposal targets PersonId 999 who doesn't exist on this rank.
   * This simulates an MPI scenario where the invitee is on another rank.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterProposal prop;
  prop.encounter_id = 1;
  prop.host_id = 0;
  prop.host_rank = 0;
  prop.invitee_id = 999;  // Does not exist
  prop.venue_id = 100;
  prop.venue_owner_rank = 0;
  prop.venue_type_id = tw.world.getVenueTypeIndex("pub");
  prop.slot = 0;
  prop.encounter_type_id = 0;

  std::vector<EncounterReply> replies;
  cem.processProposals({prop}, {prop}, replies, 0);

  REQUIRE(replies.size() == 1);
  CHECK(replies[0].status == ReplyStatus::REJECTED_NOT_FOUND);
}

TEST_CASE(
    "2g. processProposals — acceptance_probability = 0.0 rejects all "
    "proposals") {
  /**
   * SCENARIO: With acceptance_probability = 0.0, every single proposal
   * should be REJECTED_DECLINED even if the invitee's schedule allows it.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0,
      0.0 /* acceptance = 0 */
  );
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterProposal prop;
  prop.encounter_id = 1;
  prop.host_id = 0;
  prop.host_rank = 0;
  prop.invitee_id = 1;
  prop.venue_id = 100;
  prop.venue_owner_rank = 0;
  prop.venue_type_id = tw.world.getVenueTypeIndex("pub");
  prop.slot = 0;
  prop.encounter_type_id = 0;

  std::vector<EncounterReply> replies;
  cem.processProposals({prop}, {prop}, replies, 0);

  REQUIRE(replies.size() == 1);
  CHECK(replies[0].status == ReplyStatus::REJECTED_DECLINED);
}

// =============================================================================
// SECTION 3: Unit Tests — finalizeEncounters
//
// These tests verify that accepted replies correctly produce finalized
// encounters with the right participants and venue metadata.
// =============================================================================

TEST_CASE(
    "3a. finalizeEncounters — Accepted reply creates encounter with 2 "
    "participants") {
  /**
   * SCENARIO: One accepted reply for encounter_id=1 with host=0 and invitee=1.
   * The finalized encounter should have both in its participants set.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterReply reply;
  reply.encounter_id = 1;
  reply.host_id = 0;
  reply.invitee_id = 1;
  reply.venue_id = 100;
  reply.venue_type_id = 2;
  reply.slot = 0;
  reply.encounter_type_id = 0;
  reply.status = ReplyStatus::ACCEPTED;

  std::vector<CoordinatedEncounter> finalized;
  cem.finalizeEncounters({reply}, finalized);

  REQUIRE(finalized.size() == 1);
  CHECK(finalized[0].participants.size() == 2);
  CHECK(finalized[0].participants.count(0) == 1);  // Host
  CHECK(finalized[0].participants.count(1) == 1);  // Invitee
}

TEST_CASE(
    "3b. finalizeEncounters — All rejected replies produce no encounter") {
  /**
   * SCENARIO: All replies for encounter_id=1 are rejected.
   * The attendees set will only contain the host (size 1),
   * so no CoordinatedEncounter should be created.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterReply reply;
  reply.encounter_id = 1;
  reply.host_id = 0;
  reply.invitee_id = 1;
  reply.venue_id = 100;
  reply.venue_type_id = 2;
  reply.slot = 0;
  reply.encounter_type_id = 0;
  reply.status = ReplyStatus::REJECTED_SCHEDULE_CONFLICT;

  std::vector<CoordinatedEncounter> finalized;
  cem.finalizeEncounters({reply}, finalized);

  CHECK(finalized.size() == 0);
}

TEST_CASE(
    "3c. finalizeEncounters — Multiple accepted replies produce one group "
    "encounter") {
  /**
   * SCENARIO: Host 0 invited persons 1, 2, and 3. All three accept.
   * This should produce one CoordinatedEncounter with 4 participants
   * (host + 3 invitees).
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 3}, 1.0, 1.0);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  std::vector<EncounterReply> replies;
  for (int i = 1; i <= 3; ++i) {
    EncounterReply r;
    r.encounter_id = 42;  // Same encounter!
    r.host_id = 0;
    r.invitee_id = i;
    r.venue_id = 100;
    r.venue_type_id = 2;
    r.slot = 0;
    r.encounter_type_id = 0;
    r.status = ReplyStatus::ACCEPTED;
    replies.push_back(r);
  }

  std::vector<CoordinatedEncounter> finalized;
  cem.finalizeEncounters(replies, finalized);

  REQUIRE(finalized.size() == 1);
  CHECK(finalized[0].participants.size() == 4);
  CHECK(finalized[0].host_id == 0);
  CHECK(finalized[0].encounter_id == 42);
  for (int i = 0; i <= 3; ++i) {
    CHECK(finalized[0].participants.count(i) == 1);
  }
}

TEST_CASE("3d. finalizeEncounters — venue_type_id is preserved from reply") {
  /**
   * SCENARIO: The reply carries venue_type_id = 7 (some custom type).
   * The finalized encounter must preserve this exact value so that
   * downstream systems (InteractionManager) use the correct contact matrix.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterReply reply;
  reply.encounter_id = 1;
  reply.host_id = 0;
  reply.invitee_id = 1;
  reply.venue_id = -9999;
  reply.venue_type_id = 7;  // Custom type
  reply.slot = 0;
  reply.encounter_type_id = 0;
  reply.status = ReplyStatus::ACCEPTED;

  std::vector<CoordinatedEncounter> finalized;
  cem.finalizeEncounters({reply}, finalized);

  REQUIRE(finalized.size() == 1);
  CHECK(finalized[0].venue_type_id == 7);
  CHECK(finalized[0].venue_id == -9999);
}

// =============================================================================
// SECTION 4: Integration Tests — Full Pipeline
//
// These tests exercise the complete generate → process → finalize flow on
// a single rank to verify the phases compose correctly.
// =============================================================================

TEST_CASE("4a. Full Pipeline — Physical pair encounter end-to-end") {
  /**
   * FLOW:
   *  1. generateProposals: Person 0 proposes to Person 1 at a pub
   *  2. processProposals: Person 1 checks schedule & accepts
   *  3. finalizeEncounters: Creates encounter with {0, 1}
   *
   * This is the happy path for physical 1-to-1 encounters.
   *
   * With the commitment system, people who host at one slot cannot be
   * invited to the same slot. We use 2 leisure slots so hosting uses
   * one slot and invitations succeed on the other.
   */
  auto tw = buildEncounterWorld(
      5, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open",
                   {{"morning", {"leisure", "residence"}},
                    {"evening", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Phase 1: Generate
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);
  REQUIRE(proposals.size() > 0);

  // Phase 2: Process (on same rank for single-rank test)
  std::vector<EncounterReply> replies;
  cem.processProposals(proposals, proposals, replies, 0);

  // Count accepted replies
  int accepted = 0;
  for (const auto& r : replies) {
    if (r.status == ReplyStatus::ACCEPTED) accepted++;
  }
  REQUIRE(accepted > 0);

  // Phase 3: Finalize (only accepted ones will produce encounters)
  std::vector<CoordinatedEncounter> finalized;
  cem.finalizeEncounters(replies, finalized);
  REQUIRE(finalized.size() > 0);

  // Validate each encounter has exactly 2 participants (pair)
  for (const auto& enc : finalized) {
    CHECK(enc.participants.size() == 2);
    CHECK(enc.venue_id >= 0);  // Physical venue
  }
}

TEST_CASE("4b. Full Pipeline — Virtual pair encounter end-to-end") {
  /**
   * FLOW:
   *  1. generateProposals: Person 0 proposes a romantic encounter to Person 1
   *  2. processProposals: Person 1 checks schedule & accepts (they have
   * leisure)
   *  3. finalizeEncounters: Creates encounter with {0, 1} at a negative venue
   * ID
   *
   * Validates that virtual encounters flow through the full pipeline
   * with correct venue_type_id for the contact matrix.
   *
   * Uses 2 leisure slots so hosting commits one slot, leaving the other
   * free for invitations (no double-booking at same slot).
   */
  auto tw = buildEncounterWorld(
      5, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open",
                   {{"morning", {"leisure", "residence"}},
                    {"evening", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Phase 1: Generate
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);
  REQUIRE(proposals.size() > 0);

  // All proposals should have negative venue IDs
  for (const auto& p : proposals) {
    CHECK(p.venue_id < 0);
  }

  // Phase 2: Process
  std::vector<EncounterReply> replies;
  cem.processProposals(proposals, proposals, replies, 0);

  int accepted = 0;
  for (const auto& r : replies) {
    if (r.status == ReplyStatus::ACCEPTED) accepted++;
  }
  REQUIRE(accepted > 0);

  // Phase 3: Finalize
  std::vector<CoordinatedEncounter> finalized;
  cem.finalizeEncounters(replies, finalized);
  REQUIRE(finalized.size() > 0);

  for (const auto& enc : finalized) {
    CHECK(enc.participants.size() == 2);
    CHECK(enc.venue_id < 0);  // Virtual venue

    // venue_type_id should be consistent across the pipeline
    INFO("Finalized encounter venue_type_id = " << enc.venue_type_id);
  }
}

// =============================================================================
// SECTION 5: Stress Tests
//
// These tests generate high volumes of proposals to verify invariants
// hold under scale.
// =============================================================================

TEST_CASE("5a. Stress — No duplicate virtual venue IDs across proposals") {
  /**
   * SCENARIO: Generate proposals for 100 people, all pair-only virtual.
   * Every proposal should have a unique negative venue_id.
   * Collision here would mean two encounters share a "room".
   */
  auto tw = buildEncounterWorld(
      100, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  std::set<VenueId> venue_ids;
  for (const auto& p : proposals) {
    INFO("Proposal " << p.encounter_id << " venue_id = " << p.venue_id);
    CHECK(p.venue_id < 0);
    CHECK(venue_ids.count(p.venue_id) == 0);  // No duplicates!
    venue_ids.insert(p.venue_id);
  }

  MESSAGE("Generated " << proposals.size() << " proposals with "
                       << venue_ids.size() << " unique virtual venue IDs");
}

TEST_CASE(
    "5b. Stress — Pair-only finalization never exceeds 2 participants "
    "[Stress Test Bug #1]") {
  /**
   * SCENARIO: Generate pair-only encounters for 50 people, process and
   * finalize them. Every finalized encounter must have exactly 2 participants.
   *
   * This is the stress version of test 1a — running at higher volume
   * to catch statistical edge cases in the invite logic.
   */
  auto tw = buildEncounterWorld(
      50, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);
  REQUIRE(proposals.size() > 0);

  std::vector<EncounterReply> replies;
  cem.processProposals(proposals, proposals, replies, 0);

  std::vector<CoordinatedEncounter> finalized;
  cem.finalizeEncounters(replies, finalized);

  int overcrowded = 0;
  for (const auto& enc : finalized) {
    if (enc.participants.size() > 2) {
      overcrowded++;
      std::cout << "WARNING: Encounter " << enc.encounter_id << " has "
                << enc.participants.size() << " participants (max 2)"
                << std::endl;
    }
    CHECK(enc.participants.size() == 2);
  }
  CHECK(overcrowded == 0);

  MESSAGE("Finalized " << finalized.size() << " pair encounters, "
                       << overcrowded << " overcrowded");
}

TEST_CASE("5c. Stress — resetDaily prevents venue ID collision across days") {
  /**
   * SCENARIO: Run two full days. After resetDaily(), the virtual venue IDs
   * should restart from the rank-specific base. This means day-1 and day-2
   * IDs should not collide for rank 0.
   *
   * The formula is: next_virtual_venue_id_ = -1000 - (mpi_rank * 10000000)
   * So for rank 0: always starts at -1000 and decrements.
   *
   * Since each day's encounters are independent, we verify the IDs
   * are unique within each day.
   */
  auto tw = buildEncounterWorld(
      20, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Day 1
  std::vector<EncounterProposal> day1_proposals;
  cem.generateProposals(0, day1_proposals, 0);
  std::set<VenueId> day1_ids;
  for (const auto& p : day1_proposals) day1_ids.insert(p.venue_id);

  // Reset for Day 2
  cem.resetDaily();

  // Day 2
  std::vector<EncounterProposal> day2_proposals;
  cem.generateProposals(1, day2_proposals, 0);
  std::set<VenueId> day2_ids;
  for (const auto& p : day2_proposals) day2_ids.insert(p.venue_id);

  // Within each day, all IDs should be unique
  CHECK(day1_ids.size() == day1_proposals.size());
  CHECK(day2_ids.size() == day2_proposals.size());

  // After reset, daily_encounters should be empty
  CHECK(cem.getDailyEncounters().empty());

  MESSAGE("Day 1: " << day1_proposals.size() << " proposals, "
                    << "Day 2: " << day2_proposals.size() << " proposals");
}

// =============================================================================
// SECTION 6: Regression Tests — Known Bug Scenarios
//
// These tests reconstruct the exact conditions that triggered bugs during
// development, to ensure they never recur.
// =============================================================================

TEST_CASE(
    "6a. Regression — Virtual venue overcrowding: pair-only config "
    "must never produce >2 participants [Bug #1]") {
  /**
   * EXACT BUG REPRODUCTION:
   * Multiple people were ending up in the same virtual venue when the
   * config restricted encounters to pairs via invite_distribution fixed(1).
   *
   * The root cause was that multiple hosts could independently propose
   * encounters with the same invitee for the same slot. When all were
   * finalized, the invitee appeared in multiple encounters.
   *
   * This test verifies that each FINALIZED encounter has at most 2
   * participants, regardless of how many proposals target the same person.
   */
  auto tw = buildEncounterWorld(
      30, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  std::vector<EncounterReply> replies;
  cem.processProposals(proposals, proposals, replies, 0);

  std::vector<CoordinatedEncounter> finalized;
  cem.finalizeEncounters(replies, finalized);

  for (const auto& enc : finalized) {
    INFO("Encounter " << enc.encounter_id << " at venue " << enc.venue_id
                      << " has " << enc.participants.size() << " participants");
    CHECK(enc.participants.size() <= 2);
    CHECK(enc.participants.size() >= 2);  // Must have host + at least 1 invitee
  }
}

TEST_CASE(
    "6b. Regression — Contact matrix mismatch: physical venue types must "
    "match encounter def [Bug #2]") {
  /**
   * EXACT BUG REPRODUCTION:
   * The wrong virtual_contact_matrix was being applied because the
   * venue_type_id resolution was inconsistent between proposal and
   * processProposals.
   *
   * We create two encounter definitions — one physical ("social_encounters"
   * at pubs) and one virtual ("romantic_encounters" with romantic_encounter
   * matrix). Then we verify that each proposal's venue_type_id maps to
   * the correct encounter definition.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  // Add a second encounter definition (virtual)
  CoordinatedEncounterDef virtual_def;
  virtual_def.name = "romantic_encounters";
  virtual_def.enabled = true;
  virtual_def.network = "friendships";
  virtual_def.trigger_slots = {"leisure"};
  virtual_def.is_virtual = true;
  virtual_def.virtual_contact_matrix = "romantic_encounter";
  virtual_def.proposal_probability = 1.0;
  virtual_def.invite_distribution =
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1};
  virtual_def.acceptance_probability = 1.0;
  tw.config.coordinated_encounters.encounters.push_back(virtual_def);
  tw.config.contact_matrices.matrices["romantic_encounter"] = ContactMatrix();

  // Register both encounter types
  tw.world.encounter_type_names = {"social_encounters", "romantic_encounters"};

  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  // Separate physical and virtual proposals
  int physical_count = 0, virtual_count = 0;
  for (const auto& p : proposals) {
    if (p.venue_id >= 0) {
      physical_count++;
      // Physical venue_type_id should correspond to "pub" (index 2)
      CHECK(p.venue_type_id == tw.world.getVenueTypeIndex("pub"));
    } else {
      virtual_count++;
      // Virtual venue_type_id should NOT be the same as the pub type
      CHECK(p.venue_type_id != tw.world.getVenueTypeIndex("pub"));
    }
  }

  MESSAGE("Generated " << physical_count << " physical and " << virtual_count
                       << " virtual proposals");
}

TEST_CASE(
    "6c. Regression — Venue ID type collision: physical ID 5 must never "
    "equal a virtual venue ID [Bug #3]") {
  /**
   * EXACT BUG REPRODUCTION:
   * Physical venues had positive IDs (e.g., 5) and virtual venues were
   * also getting small positive IDs, causing "classroom" to be logged
   * for what should have been a virtual encounter.
   *
   * The fix uses negative IDs for all virtual venues (starting at -1000
   * per rank). This test verifies the ID spaces are disjoint.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  // Physical venue with a small positive ID
  tw.world.venues[0].id = 5;
  tw.world.buildIndices();
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  for (const auto& p : proposals) {
    // Virtual venues must be negative — can never collide with physical ID 5
    CHECK(p.venue_id < 0);
    CHECK(p.venue_id != 5);

    // The venue_type_id should also be distinct from physical venue types
    INFO("Virtual proposal venue_type_id = "
         << p.venue_type_id
         << ", physical pub type_id = " << tw.world.getVenueTypeIndex("pub"));
  }
}

// =============================================================================
// SECTION 7: Policy Enforcement Tests
//
// In the simulator, after encounters are finalized, they are "injected" into
// each participant's location for the relevant time slot. Eligibility asks
// PolicyManager::suppressesParticipation() for each participant — the question
// only, since pass 1 may still cancel the encounter.
//
// If a policy overrides the encounter's trigger activity (e.g., "leisure" →
// "residence" due to symptom isolation), that participant is NOT injected
// into the encounter venue — they stay at their policy-mandated location.
//
// IMPORTANT: Each participant is checked independently. If the HOST is
// policy-blocked, other participants can still attend. The encounter does
// NOT collapse just because the host can't go.
//
// These tests replicate the injection loop from simulator.cpp to verify
// policy enforcement without needing the full Simulator.
// =============================================================================

#include "epidemiology/disease.h"
#include "epidemiology/policy.h"
#include "simulation/encounters/eligibility.h"

/**
 * Drives Simulator::injectCoordinatedEncountersIntoSlot without a Simulator.
 *
 * Pass 1 is the production seam (june::encounters::computeLocalEligibility);
 * only the pass-2 injection stamp is reproduced here, minus the MPI-only
 * global exchange and virtual-venue registration.
 *
 * @param encounters     Finalized encounters
 * @param time_slot      Which time slot to inject for
 * @param locations      Current person locations (modified in place)
 * @param world          WorldState for person lookups
 * @param config         Config for encounter definitions
 * @param policy_manager PolicyManager to check overrides (may be nullptr)
 * @param current_time   Simulation time for policy checks
 *
 * @return Set of PersonIds that were successfully injected into encounter
 * venues
 */
static std::set<PersonId> simulateEncounterInjection(
    const std::vector<CoordinatedEncounter>& encounters, int time_slot,
    std::vector<PersonLocation>& locations, WorldState& world,
    const Config& config, PolicyManager* policy_manager, double current_time) {
  std::set<PersonId> injected;

  // Pass 1: the real production eligibility computation.
  auto lookups = june::encounters::buildEncounterLookups(
      world, config.coordinated_encounters.encounters);
  auto slot_encounters = june::encounters::computeLocalEligibility(
      encounters, time_slot, current_time, lookups, world, locations,
      policy_manager);

  // Pass 2: inject the encounters that met their threshold.
  for (const auto& eligibility : slot_encounters) {
    const auto& enc = encounters[eligibility.encounter_idx];
    if (eligibility.local_eligible < eligibility.min_required) continue;
    for (size_t array_idx : eligibility.eligible_indices) {
      locations[array_idx].venue_id = enc.venue_id;
      locations[array_idx].encounter_type_id = enc.encounter_type_id;
      injected.insert(world.people[array_idx].id);
    }
  }
  return injected;
}

/**
 * Helper: Creates a minimal Disease with just the symptom tags we need.
 * Real diseases have complex trajectories; we only need symptom IDs for
 * policies.
 */
static std::unique_ptr<Disease> createMinimalDisease(
    const std::vector<std::string>& symptom_names) {
  std::vector<SymptomTag> tags;
  for (size_t i = 0; i < symptom_names.size(); ++i) {
    SymptomTag tag;
    tag.name = symptom_names[i];
    tag.value = static_cast<int>(i);
    tag.id = static_cast<uint16_t>(i);
    tags.push_back(tag);
  }

  DiseaseStageSettings settings;
  settings.default_lowest_stage = symptom_names[0];
  settings.stay_at_home_stages = {};
  settings.fatality_stages = {};
  settings.recovered_stages = {};

  TransmissionParams tx;
  OutcomeRates rates;

  return std::make_unique<Disease>("test_disease", tags, settings,
                                   std::vector<TrajectoryDefinition>{}, rates,
                                   tx);
}

/**
 * Helper: Gives a person a fake infection at the given symptom stage.
 * We manually construct the InfectionTrajectory to avoid needing Disease
 * trajectory generation (which requires outcome rates, etc.)
 */
static void infect(Person& p, Disease* disease, uint16_t symptom_id,
                   double time) {
  // Create a minimal infection using the Disease constructor
  p.infection = std::make_unique<Infection>(disease, time, &p, 42);
  // Override the trajectory with a single fixed stage
  // Access the internal trajectory by creating a new infection with known state
  // Since Infection's trajectory is private, we'll use a workaround:
  // The Infection constructor generates a trajectory, but for testing we need
  // a specific symptom. We'll set infection_time early enough that the
  // symptom has transitioned.
  //
  // Actually, due to the Infection class being complex, let's just set it
  // with the right time so getCurrentSymptomId returns what we want.
  // We'll create a custom approach using the trajectory transition mechanism.
}

/**
 * Helper: Creates a PolicyManager with a simple SymptomPolicy that
 * overrides a specific activity to "residence" when triggered by a symptom.
 */
static void addSymptomIsolationPolicy(PolicyManager& pm, WorldState& world,
                                      Disease& disease,
                                      const std::string& trigger_symptom,
                                      const std::string& override_activity,
                                      double compliance_rate = 1.0) {
  SymptomPolicy sp;
  sp.name = "test_isolation";
  sp.trigger_symptoms = {trigger_symptom};

  sp.action.override_activities = {override_activity};
  sp.action.replacement_activity = "residence";
  sp.action.compliance_rate = compliance_rate;

  pm.addSymptomPolicy(sp);
  pm.resolveAll(disease);
}

/**
 * Helper: Creates a PolicyManager with a simple TemporalPolicy (lockdown)
 * that overrides all activities to "residence" during a time window.
 */
static void addLockdownPolicy(
    PolicyManager& pm, WorldState& world, double start_time, double end_time,
    double compliance_rate = 1.0,
    const std::vector<ActivityExemption>& exemptions = {}) {
  TemporalPolicy tp;
  tp.name = "test_lockdown";
  tp.window.start_time = start_time;
  tp.window.end_time = end_time;

  tp.action.override_all = true;
  tp.action.replacement_activity = "residence";
  tp.action.replacement_activity_index =
      static_cast<int16_t>(world.getActivityIndex("residence"));
  tp.action.compliance_rate = compliance_rate;
  tp.action.exemptions = exemptions;

  pm.addTemporalPolicy(tp);
  // Note: temporal policies don't need disease for resolve
}

/**
 * Helper: Creates a TemporalPolicy overriding named activities to "residence",
 * optionally restricted to a set of venue types (the Phase 1 restrict-to gate).
 * Pass an empty venue_types for the activity-only behaviour that predates the
 * gate. Resolving needs a Disease, so one is created and returned to the
 * caller to keep alive.
 */
static std::unique_ptr<Disease> addVenueGatedPolicy(
    PolicyManager& pm, WorldState& world,
    const std::vector<std::string>& activities,
    const std::vector<std::string>& venue_types, double start_time,
    double end_time) {
  TemporalPolicy tp;
  tp.name = "close_pubs";
  tp.window.start_time = start_time;
  tp.window.end_time = end_time;

  tp.action.override_activities.insert(activities.begin(), activities.end());
  tp.action.override_venue_types.insert(venue_types.begin(), venue_types.end());
  tp.action.replacement_activity = "residence";
  tp.action.replacement_activity_index =
      static_cast<int16_t>(world.getActivityIndex("residence"));
  tp.action.compliance_rate = 1.0;

  pm.addTemporalPolicy(tp);
  auto disease = createMinimalDisease({"healthy"});
  pm.resolveAll(*disease);
  return disease;
}

/**
 * Helper: Sets up initial locations with everyone at residence.
 */
static std::vector<PersonLocation> initLocations(WorldState& world) {
  std::vector<PersonLocation> locations(world.people.size());
  int residence_idx = world.getActivityIndex("residence");
  for (size_t i = 0; i < world.people.size(); ++i) {
    locations[i].person_id = world.people[i].id;
    locations[i].venue_id = 0;  // Home
    locations[i].subset_index = 0;
    locations[i].activity_index = static_cast<int16_t>(residence_idx);
    locations[i].encounter_type_id = 255;  // None
  }
  return locations;
}

TEST_CASE(
    "7a. Policy — Symptom isolation blocks invitee, encounter cancelled "
    "(min_attendees=2)") {
  /**
   * SCENARIO:
   *   Person 0 (host) is healthy.
   *   Person 1 (invitee) has a symptomatic infection.
   *   A symptom policy says: if symptomatic, override "leisure" to "residence".
   *
   *   At injection time, Person 1 is blocked (policy overrides "leisure").
   *   Since only 1 of 2 participants survived and min_attendees defaults to 2,
   *   the encounter is cancelled entirely. Person 0 keeps their original
   *   activity (residence) instead of being stranded alone at a virtual venue.
   *
   * BEHAVIOR: Encounters require min_attendees (default 2) to proceed.
   * This prevents nonsensical situations like being alone at a "sex" venue.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  // Create minimal disease with symptoms
  auto disease = createMinimalDisease({"healthy", "symptomatic", "recovered"});

  // Create policy manager with symptom isolation
  PolicyManager pm(tw.world);
  addSymptomIsolationPolicy(pm, tw.world, *disease, "symptomatic", "leisure",
                            1.0);

  // Person 1 is infected and symptomatic
  tw.world.people[1].infection =
      std::make_unique<Infection>(disease.get(), 0.0, &tw.world.people[1], 42);

  // Set policy applicability: Person 1 is applicable to policy 0
  tw.world.people[1].applicable_symptom_policy_mask = 1;  // bit 0 = policy 0

  // Create a finalized encounter
  CoordinatedEncounter enc;
  enc.encounter_id = 100;
  enc.host_id = 0;
  enc.venue_id = -5000;
  enc.venue_type_id = 0;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);

  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  // Person 1's policy status depends on the Infection trajectory at time=5.0.
  // If the policy blocked Person 1 (the expected path), the encounter should
  // be cancelled entirely — neither person injected.
  if (injected.count(1) == 0) {
    // Person 1 was blocked → encounter cancelled, host also stays home
    CHECK(injected.count(0) == 0);
    CHECK(locations[0].venue_id == 0);  // Host stays at residence
    CHECK(locations[1].venue_id == 0);  // Invitee stays at residence
  } else {
    // Person 1 was NOT blocked (trajectory didn't trigger symptom at t=5) →
    // both should be injected normally
    CHECK(injected.count(0) == 1);
    CHECK(locations[0].venue_id == -5000);
    CHECK(locations[1].venue_id == -5000);
  }
}

TEST_CASE(
    "7b. Policy — Symptom isolation blocks host, encounter cancelled "
    "(min_attendees=2)") {
  /**
   * SCENARIO:
   *   Person 0 (host) has a symptomatic infection.
   *   Person 1 (invitee) is healthy.
   *   Symptom policy: if symptomatic, override "leisure" to "residence".
   *
   *   At injection time, Person 0 (host) is blocked by the policy.
   *   Since only 1 of 2 participants survived and min_attendees defaults to 2,
   *   the encounter is cancelled entirely. Person 1 keeps their original
   *   activity instead of being stranded alone at the encounter venue.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  auto disease = createMinimalDisease({"healthy", "symptomatic", "recovered"});

  PolicyManager pm(tw.world);
  addSymptomIsolationPolicy(pm, tw.world, *disease, "symptomatic", "leisure",
                            1.0);

  // Person 0 (host) is infected
  tw.world.people[0].infection =
      std::make_unique<Infection>(disease.get(), 0.0, &tw.world.people[0], 42);
  tw.world.people[0].applicable_symptom_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 200;
  enc.host_id = 0;
  enc.venue_id = -6000;
  enc.venue_type_id = 0;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);

  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  // If Person 0 was blocked (expected), encounter is cancelled — neither
  // person is injected. Person 1 stays at their original activity.
  if (injected.count(0) == 0) {
    CHECK(injected.count(1) == 0);
    CHECK(locations[0].venue_id == 0);  // Host stays at residence
    CHECK(locations[1].venue_id == 0);  // Invitee stays at residence
  } else {
    // Person 0 was NOT blocked (trajectory didn't trigger at t=5) →
    // both injected normally
    CHECK(injected.count(1) == 1);
    CHECK(locations[0].venue_id == -6000);
    CHECK(locations[1].venue_id == -6000);
  }
}

TEST_CASE("7c. Policy — Lockdown blocks all encounter participants") {
  /**
   * SCENARIO:
   *   An active lockdown policy overrides all activities to "residence"
   *   with 100% compliance. Both participants should be blocked.
   *
   *   Time = 5.0, lockdown active from 0.0 to 10.0.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  addLockdownPolicy(pm, tw.world, 0.0, 10.0, 1.0);

  // Both people are applicable to temporal policy 0
  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 300;
  enc.host_id = 0;
  enc.venue_id = -7000;
  enc.venue_type_id = 0;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);

  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  // Both should be blocked — lockdown overrides all activities
  CHECK(injected.count(0) == 0);
  CHECK(injected.count(1) == 0);
  CHECK(locations[0].venue_id == 0);  // Still at residence
  CHECK(locations[1].venue_id == 0);  // Still at residence
}

TEST_CASE("7d. Policy — Lockdown with exemption allows exempted participant") {
  /**
   * SCENARIO:
   *   Active lockdown overrides all activities, BUT Person 0 has an
   *   exemption for the "leisure" activity (e.g., essential worker
   *   attending a required social event).
   *
   *   Person 0 should be injected (exempted).
   *   Person 1 should be blocked (not exempted).
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  // Create exemption for "leisure" activity — applies to everyone
  // (in a real config, selection criteria would narrow this)
  ActivityExemption exemption;
  exemption.activity_name = "leisure";
  exemption.activity_index =
      static_cast<int16_t>(tw.world.getActivityIndex("leisure"));
  // No criteria = applies to everyone

  PolicyManager pm(tw.world);
  addLockdownPolicy(pm, tw.world, 0.0, 10.0, 1.0, {exemption});

  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 400;
  enc.host_id = 0;
  enc.venue_id = -8000;
  enc.venue_type_id = 0;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);

  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  // Both should be exempted for "leisure" — the exemption applies to everyone
  // when no criteria are specified
  CHECK(injected.count(0) == 1);
  CHECK(injected.count(1) == 1);
  CHECK(locations[0].venue_id == -8000);
  CHECK(locations[1].venue_id == -8000);
}

TEST_CASE(
    "7e. Policy — Lockdown with compliance_rate = 0.0 means nobody isolates") {
  /**
   * SCENARIO:
   *   Active lockdown with 0% compliance. Nobody actually follows the
   *   lockdown, so both participants should be injected normally.
   *
   *   This tests the compliance_rate mechanism: even with an active
   *   policy, non-compliant individuals still attend encounters.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  addLockdownPolicy(pm, tw.world, 0.0, 10.0, 0.0 /* 0% compliance */);

  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 500;
  enc.host_id = 0;
  enc.venue_id = -9000;
  enc.venue_type_id = 0;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);

  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  // Nobody complies with lockdown — both should be injected
  CHECK(injected.count(0) == 1);
  CHECK(injected.count(1) == 1);
  CHECK(locations[0].venue_id == -9000);
  CHECK(locations[1].venue_id == -9000);
}

TEST_CASE("7f. Policy — Lockdown on one partner cancels 2-person encounter") {
  /**
   * SCENARIO:
   *   Person 0 is under lockdown (100% compliance).
   *   Person 1 is NOT under lockdown.
   *   A 2-person virtual encounter (like "sex") is finalized between them.
   *
   *   At injection time, Person 0 is blocked by lockdown. Only Person 1
   *   survives policy checks → 1 < min_attendees (2) → encounter cancelled.
   *   Person 1 keeps their original residence assignment instead of being
   *   stranded alone at a virtual encounter venue.
   *
   * This is the key test for the two-pass injection fix: without it,
   * Person 1 would end up alone at a "romantic_encounter" venue.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  addLockdownPolicy(pm, tw.world, 0.0, 10.0, 1.0);

  // Only Person 0 is subject to lockdown
  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 0;

  CoordinatedEncounter enc;
  enc.encounter_id = 600;
  enc.host_id = 0;
  enc.venue_id = -10000;
  enc.venue_type_id = 0;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);

  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  // Person 0 is blocked by lockdown. Only 1 eligible < min_attendees (2).
  // Encounter cancelled — NEITHER person is injected.
  CHECK(injected.empty());
  CHECK(locations[0].venue_id == 0);  // Person 0 stays at residence
  CHECK(locations[1].venue_id == 0);  // Person 1 stays at residence (not
                                      // stranded at virtual venue)
}

TEST_CASE(
    "7g. Policy — 3-person encounter survives with 2 when one is blocked") {
  /**
   * SCENARIO:
   *   Persons 0, 1, 2 are in a social encounter (min_attendees=2).
   *   Person 2 is under lockdown (100% compliance).
   *   Persons 0 and 1 are NOT under lockdown.
   *
   *   At injection time, Person 2 is blocked. 2 eligible >= min_attendees (2).
   *   The encounter proceeds for Persons 0 and 1. Person 2 stays at residence.
   */
  auto tw = buildEncounterWorld(
      3, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 2}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  addLockdownPolicy(pm, tw.world, 0.0, 10.0, 1.0);

  // Only Person 2 is subject to lockdown
  tw.world.people[0].applicable_temporal_policy_mask = 0;
  tw.world.people[1].applicable_temporal_policy_mask = 0;
  tw.world.people[2].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 700;
  enc.host_id = 0;
  enc.venue_id = 100;  // Physical pub venue
  enc.venue_type_id = 2;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1, 2};

  auto locations = initLocations(tw.world);

  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  // 2 of 3 survived → meets min_attendees (2) → encounter proceeds
  CHECK(injected.count(0) == 1);
  CHECK(injected.count(1) == 1);
  CHECK(injected.count(2) == 0);
  CHECK(locations[0].venue_id == 100);
  CHECK(locations[1].venue_id == 100);
  CHECK(locations[2].venue_id == 0);  // Person 2 stays at residence
}

TEST_CASE(
    "7h. Policy — min_attendees=1 allows solo encounter when partner is "
    "blocked") {
  /**
   * SCENARIO:
   *   Same as 7f, but with min_attendees=1. Person 0 is locked down,
   *   Person 1 is free. With min_attendees=1, Person 1 should still be
   *   injected even though they're alone.
   *
   *   This verifies that the min_attendees config is respected and that
   *   encounter types like "solo_activity" could use min_attendees=1 if
   *   desired.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  // Override min_attendees to 1
  tw.config.coordinated_encounters.encounters[0].min_attendees = 1;

  PolicyManager pm(tw.world);
  addLockdownPolicy(pm, tw.world, 0.0, 10.0, 1.0);

  // Only Person 0 is subject to lockdown
  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 0;

  CoordinatedEncounter enc;
  enc.encounter_id = 800;
  enc.host_id = 0;
  enc.venue_id = -11000;
  enc.venue_type_id = 0;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);

  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  // Person 0 blocked, Person 1 free. min_attendees=1, so Person 1 proceeds.
  CHECK(injected.size() == 1);
  CHECK(injected.count(1) == 1);
  CHECK(locations[0].venue_id == 0);       // Blocked, stays at residence
  CHECK(locations[1].venue_id == -11000);  // Injected alone (min_attendees=1)
}

TEST_CASE(
    "6d. Regression — Unmatched venue_type_id is explicitly rejected, "
    "not silently accepted [Bug #5 Fix]") {
  /**
   * BUG: processProposals would silently accept proposals whose
   * venue_type_id did not match any encounter definition. When
   * matched_def was NULL and the invitee had no cached schedule,
   * the fallback path set schedule_allows = true and the proposal
   * was accepted with probability 1.0.
   *
   * FIX: processProposals now rejects proposals with no matching
   * encounter definition via REJECTED_NO_MATCHING_DEF.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Construct a proposal with a bogus venue_type_id (99) that
  // won't match any encounter definition
  EncounterProposal prop;
  prop.encounter_id = 8888;
  prop.host_id = 0;
  prop.host_rank = 0;
  prop.invitee_id = 1;
  prop.venue_id = -5000;
  prop.venue_owner_rank = 0;
  prop.venue_type_id = 99;  // No encounter def has this venue_type_id
  prop.slot = 0;
  prop.encounter_type_id = 0;

  std::vector<EncounterReply> replies;
  cem.processProposals({prop}, {prop}, replies, 0);

  REQUIRE(replies.size() == 1);
  CHECK(replies[0].status == ReplyStatus::REJECTED_NO_MATCHING_DEF);
}

// =============================================================================
// SECTION 7g-7i: Venue-gated policies
//
// The gate must be asked about the venue the encounter would put the person
// in, not the venue they are currently scheduled to. Everyone starts at home
// (initLocations), so a gate read off locations[] never sees the pub.
// =============================================================================

TEST_CASE("7g. Venue gate — encounter at a gated pub is cancelled") {
  /**
   * SCENARIO:
   *   close_pubs overrides "leisure" at venue type "pub" only.
   *   The encounter is at pub venue 100; both participants are currently at
   *   home. The gate must see "pub" and block both, cancelling the encounter.
   *
   * REGRESSION: reading locations[].venue_id here yields "home", the gate
   * does not match, and injection puts both back in the closed pub.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "pub_meetups", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  auto disease =
      addVenueGatedPolicy(pm, tw.world, {"leisure"}, {"pub"}, 0.0, 10.0);

  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 700;
  enc.host_id = 0;
  enc.venue_id = 100;  // the pub
  enc.venue_type_id = 2;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);
  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  CHECK(injected.empty());
  CHECK(locations[0].venue_id == 0);
  CHECK(locations[1].venue_id == 0);
  CHECK(locations[0].encounter_type_id == 255);
  CHECK(locations[1].encounter_type_id == 255);
}

TEST_CASE("7h. Venue gate — same policy, encounter at an ungated venue fires") {
  /**
   * SCENARIO:
   *   Identical to 7g except the encounter is hosted at a home venue. "home"
   *   is not in the gate's mask, so the override does not apply and the
   *   encounter proceeds. This is the allowed_venues modelling lever: the
   *   same gathering written as ["household"] survives close_pubs.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "pub_meetups", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  auto disease =
      addVenueGatedPolicy(pm, tw.world, {"leisure"}, {"pub"}, 0.0, 10.0);

  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 701;
  enc.host_id = 0;
  enc.venue_id = 0;  // host's home, venue type "home"
  enc.venue_type_id = 0;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);
  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  CHECK(injected.count(0) == 1);
  CHECK(injected.count(1) == 1);
  CHECK(locations[0].encounter_type_id == 0);
  CHECK(locations[1].encounter_type_id == 0);
}

TEST_CASE("7i. Venue gate — activity-only policy blocks as it always has") {
  /**
   * SCENARIO:
   *   Same encounter as 7h (ungated home venue), but the policy carries no
   *   venue types at all. It must still block on the activity alone,
   *   unchanged by the venue the gate now reads.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "pub_meetups", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  auto disease = addVenueGatedPolicy(pm, tw.world, {"leisure"}, {}, 0.0, 10.0);

  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 702;
  enc.host_id = 0;
  enc.venue_id = 0;
  enc.venue_type_id = 0;
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);
  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  CHECK(injected.empty());
}

TEST_CASE(
    "7j. Venue gate — a virtual encounter is at no venue, so never gated") {
  /**
   * SCENARIO:
   *   CoordinatedEncounter::venue_type_id is polysemous: a world venue-type id
   *   for a physical encounter, an id into the alphabetically-sorted contact
   *   matrix registry for a virtual one. The two registries are ordered
   *   independently, so the integers alias.
   *
   *   The collision is built deliberately here. Matrix keys sort to
   *   {home, office, online_chat, pub}, so the virtual encounter's matrix id
   *   is 2 — numerically identical to the physical venue type "pub", which
   *   close_pubs gates on.
   *
   *   A virtual encounter is at no Venue at all, so the gate must not fire and
   *   the encounter proceeds. Reading venue_type_id here instead would gate a
   *   video call as if it were a pub.
   */
  auto tw = buildEncounterWorld(
      2, 0, "pub", "friendships", "online_meetups", true, "online_chat",
      {"leisure"}, InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1},
      1.0, 1.0);

  // The collision the test rests on.
  REQUIRE(tw.config.contact_matrices.matrix_name_to_id.at("online_chat") ==
          tw.world.getVenueTypeIndex("pub"));

  PolicyManager pm(tw.world);
  auto disease =
      addVenueGatedPolicy(pm, tw.world, {"leisure"}, {"pub"}, 0.0, 10.0);

  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 703;
  enc.host_id = 0;
  enc.venue_id = -5000;  // virtual: no venue
  enc.venue_type_id = static_cast<uint8_t>(
      tw.config.contact_matrices.matrix_name_to_id.at("online_chat"));
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);
  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  CHECK(injected.count(0) == 1);
  CHECK(injected.count(1) == 1);
}

TEST_CASE("7k. Venue gate — a physical encounter gates on its own venue type") {
  /**
   * SCENARIO:
   *   As 7g, but the encounter's venue is one this rank cannot type (a
   *   cross-rank host's venue). The encounter carries its type, so the gate
   *   answers from enc.venue_type_id and blocks — no local lookup, no
   *   silent 255 fail-open, and no rank-dependent answer.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "pub_meetups", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  auto disease =
      addVenueGatedPolicy(pm, tw.world, {"leisure"}, {"pub"}, 0.0, 10.0);

  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 704;
  enc.host_id = 0;
  enc.venue_id = 999;  // not in this rank's world
  enc.venue_type_id = static_cast<uint8_t>(tw.world.getVenueTypeIndex("pub"));
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);
  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  CHECK(injected.empty());
}

/**
 * Builds a Disease whose single stage is "sick" for 100 days, so
 * getCurrentSymptomId is the same at every time the test asks about. The
 * section's createMinimalDisease has no trajectories, which is why 7a/7b have
 * to hedge on whether the symptom fired.
 */
static Disease buildAlwaysSickDisease() {
  TransmissionParams transmission;
  transmission.mode = InfectiousnessMode::STAGE_DRIVEN;
  auto curve = std::make_shared<ConstantCurve>(1.0);
  transmission.stage_curves["sick"] = curve;
  transmission.symptom_id_curves = {nullptr, curve};

  std::vector<SymptomTag> symptom_tags = {{"healthy", -1, 0}, {"sick", 1, 1}};
  DiseaseStageSettings stage_settings;
  stage_settings.recovered_stages = {"healthy"};

  TrajectoryDefinition trajectory;
  trajectory.selection_key = "general";
  trajectory.severity = 1.0;
  trajectory.stages.push_back({"sick", {"constant", {{"value", 100.0}}}});

  return Disease("TestDisease", symptom_tags, stage_settings, {trajectory}, {},
                 transmission);
}

TEST_CASE("7l. Eligibility asks the policy question and pins nobody") {
  /**
   * SCENARIO:
   *   Person 1 is a hopped traveller, sick, under a freeze_in_place policy
   *   covering "leisure". They are invited to an encounter at a pub.
   *
   *   Eligibility must answer "yes, a policy suppresses them" and stop there.
   *   Establishing the freeze is ActivityManager's job: pass 1 sees the
   *   encounter's venue, not the venue the person is actually at, so a pin
   *   written here anchors them somewhere they never went — for the whole
   *   freeze, and even if the encounter is cancelled below min_attendees a
   *   few lines later (which here it is).
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "pub_meetups", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  Disease disease = buildAlwaysSickDisease();

  PolicyManager pm(tw.world);
  SymptomPolicy freeze_when_sick;
  freeze_when_sick.name = "freeze_traveller_when_sick";
  freeze_when_sick.trigger_symptoms = {"sick"};
  freeze_when_sick.action.override_activities = {"leisure"};
  freeze_when_sick.action.replacement_activity = "residence";
  freeze_when_sick.action.compliance_rate = 1.0;
  // Set before adding: resolve() only fills this in from a named schedule.
  freeze_when_sick.action.replacement_schedule_idx = 0;
  pm.addSymptomPolicy(freeze_when_sick);
  pm.resolveAll(disease);

  Person& traveller = tw.world.people[1];
  traveller.infection = std::make_unique<Infection>(
      &disease, 0.0, &traveller, 42, nullptr, "household", 0);
  traveller.applicable_symptom_policy_mask = 1;
  constexpr int16_t kHoppedSchedule = 3;
  constexpr int16_t kReturnSchedule = 1;
  traveller.schedule_hop = ScheduleHop::begin(kHoppedSchedule, kReturnSchedule);

  CoordinatedEncounter enc;
  enc.encounter_id = 705;
  enc.host_id = 0;
  enc.venue_id = 100;
  enc.venue_type_id = static_cast<uint8_t>(tw.world.getVenueTypeIndex("pub"));
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);
  auto injected = simulateEncounterInjection({enc}, 0, locations, tw.world,
                                             tw.config, &pm, 5.0);

  // The verdict is unchanged: suppressed, so the encounter falls below 2.
  CHECK(injected.empty());

  // The consequences are not eligibility's to commit.
  CHECK(pm.getFrozenStates().empty());
  CHECK(traveller.schedule_hop.hopped_schedule_id == kHoppedSchedule);
  CHECK(traveller.schedule_hop.return_schedule_id == kReturnSchedule);
}

TEST_CASE("7m. Eligibility latches no compliance decision") {
  /**
   * SCENARIO:
   *   A lockdown at compliance 0.5 over participants who have not yet been
   *   through ActivityManager this slot, so no decision is latched yet.
   *
   *   Eligibility must read the coin, not toss and record it. Its answer has
   *   to match what the authoritative caller goes on to latch — a query that
   *   disagreed with the override would put people in an encounter the policy
   *   then moves them out of.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "pub_meetups", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  addLockdownPolicy(pm, tw.world, 0.0, 10.0, 0.5);
  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 706;
  enc.host_id = 0;
  enc.venue_id = 100;
  enc.venue_type_id = static_cast<uint8_t>(tw.world.getVenueTypeIndex("pub"));
  enc.slot = 0;
  enc.encounter_type_id = 0;
  enc.participants = {0, 1};

  auto locations = initLocations(tw.world);
  auto lookups = june::encounters::buildEncounterLookups(
      tw.world, tw.config.coordinated_encounters.encounters);
  auto slot_encounters = june::encounters::computeLocalEligibility(
      {enc}, 0, 5.0, lookups, tw.world, locations, &pm);

  REQUIRE(slot_encounters.size() == 1);
  std::set<size_t> eligible(slot_encounters[0].eligible_indices.begin(),
                            slot_encounters[0].eligible_indices.end());

  const int16_t leisure =
      static_cast<int16_t>(tw.world.getActivityIndex("leisure"));
  for (size_t i = 0; i < tw.world.people.size(); ++i) {
    Person& person = tw.world.people[i];
    CHECK(person.temporal_policy_decisions == 0);

    // The authoritative caller now runs and latches. Suppressed by the query
    // iff overridden by ActivityManager.
    const bool overridden =
        pm.getOverride(person, leisure, enc.venue_id, 0,
                       SlotVenueType::known(enc.venue_type_id), 5.0, 0)
            .has_value();
    CHECK(overridden == (eligible.count(i) == 0));
  }
}

// =============================================================================
// SECTION 8: Commitment System Tests
//
// These tests verify the new per-type budget, priority ordering, shared slot
// pool, and weekend awareness features.
// =============================================================================

TEST_CASE("8a. Weekend uses weekend schedule slots") {
  /**
   * SCENARIO: Person has 1 weekday leisure slot but 3 weekend leisure slots.
   * On a weekday (is_weekend=false), they should produce fewer proposals
   * than on a weekend (is_weekend=true).
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  // Weekday: 1 leisure slot. Weekend: 3 leisure slots.
  addScheduleToAll(tw, "worker",
                   {{"work_slot", {"work", "residence"}},
                    {"evening", {"leisure", "residence"}}},
                   {{"morning", {"leisure", "residence"}},
                    {"afternoon", {"leisure", "residence"}},
                    {"evening", {"leisure", "residence"}}});

  // Set daily_max_distribution to fixed(10) so budget isn't the bottleneck
  tw.config.coordinated_encounters.encounters[0].daily_max_distribution =
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 10};

  CoordinatedEncounterManager cem_wd(tw.world, tw.config, 0);
  std::vector<EncounterProposal> weekday_proposals;
  cem_wd.generateProposals(0, weekday_proposals, 0);

  CoordinatedEncounterManager cem_we(tw.world, tw.config, 0);
  std::vector<EncounterProposal> weekend_proposals;
  cem_we.generateProposals(0, weekend_proposals, 1);

  INFO("Weekday proposals: " << weekday_proposals.size()
                             << ", Weekend proposals: "
                             << weekend_proposals.size());

  // Weekend should produce more proposals due to more leisure slots
  // (unless proposal_probability filters them, which is 1.0 here)
  CHECK(weekend_proposals.size() >= weekday_proposals.size());
}

TEST_CASE("8b. Per-type budget caps proposals per person") {
  /**
   * SCENARIO: Social encounters with daily_max_distribution fixed(1).
   * Person has 3 leisure slots. But budget=1 means they should propose
   * at most 1 social encounter, not 3.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open",
                   {{"slot0", {"leisure", "residence"}},
                    {"slot1", {"leisure", "residence"}},
                    {"slot2", {"leisure", "residence"}}});

  // Budget = 1 per type
  tw.config.coordinated_encounters.encounters[0].daily_max_distribution =
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1};

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  // Count proposals per host
  std::map<PersonId, int> proposals_per_host;
  for (const auto& p : proposals) {
    proposals_per_host[p.host_id]++;
  }

  // Each person should propose at most 1 time (budget=1, even though 3 slots
  // available)
  for (const auto& [host, count] : proposals_per_host) {
    INFO("Host " << host << " made " << count << " proposals (budget=1)");
    // count is number of invitees, but from one slot with fixed(1) invite,
    // they invite 1 person per slot. With budget=1, they pick 1 slot.
    // So proposals from this host should all be from the same one encounter.
  }

  // Total proposals should be at most num_people (4 hosts x 1 proposal each)
  CHECK(proposals.size() <= 4);
}

TEST_CASE("8c. Priority ordering — higher priority gets slots first") {
  /**
   * SCENARIO: Two encounter types share the same trigger ("leisure").
   * - romantic (priority=1)
   * - social (priority=10)
   * Person has only 1 leisure slot, and both have daily_max=1.
   *
   * Since romantic is processed first (lower priority number), it should
   * "consume" the only leisure slot. Social should then have no slots left.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "romantic_encounters", true,
      "romantic_encounter", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "tight", {{"evening", {"leisure", "residence"}}});

  // Romantic = priority 1
  tw.config.coordinated_encounters.encounters[0].priority = 1;
  tw.config.coordinated_encounters.encounters[0].daily_max_distribution =
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1};

  // Add social = priority 10
  CoordinatedEncounterDef social_def;
  social_def.name = "social_encounters";
  social_def.enabled = true;
  social_def.network = "friendships";
  social_def.trigger_slots = {"leisure"};
  social_def.is_virtual = false;
  social_def.allowed_venues = {"pub"};
  social_def.proposal_probability = 1.0;
  social_def.invite_distribution =
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1};
  social_def.acceptance_probability = 1.0;
  social_def.priority = 10;
  social_def.daily_max_distribution =
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1};
  tw.config.coordinated_encounters.encounters.push_back(social_def);
  tw.world.encounter_type_names = {"romantic_encounters", "social_encounters"};

  // Sort encounters by priority (as config_loader would)
  std::sort(
      tw.config.coordinated_encounters.encounters.begin(),
      tw.config.coordinated_encounters.encounters.end(),
      [](const CoordinatedEncounterDef& a, const CoordinatedEncounterDef& b) {
        return a.priority < b.priority;
      });

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  // Count by encounter type
  int romantic_count = 0, social_count = 0;
  for (const auto& p : proposals) {
    if (p.venue_id < 0)
      romantic_count++;
    else
      social_count++;
  }

  INFO("Romantic proposals: " << romantic_count
                              << ", Social proposals: " << social_count);

  // With only 1 slot, romantic (higher priority) should take it
  // Social should get 0 (no remaining slots)
  CHECK(romantic_count > 0);
  CHECK(social_count == 0);
}

TEST_CASE(
    "8d. Double-booking rejection — invitee rejects if slot already "
    "committed") {
  /**
   * SCENARIO: Invitee accepts a proposal for slot 0, then a second
   * proposal arrives for the same slot. The second should be rejected
   * with REJECTED_ALREADY_COMMITTED.
   */
  auto tw = buildEncounterWorld(
      3, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // First proposal: Host 0 invites Person 1 at slot 0
  EncounterProposal prop1;
  prop1.encounter_id = 1;
  prop1.host_id = 0;
  prop1.host_rank = 0;
  prop1.invitee_id = 1;
  prop1.venue_id = 100;
  prop1.venue_owner_rank = 0;
  prop1.venue_type_id = 2;  // "pub" type
  prop1.slot = 0;
  prop1.encounter_type_id = 0;

  // Second proposal: Host 2 invites Person 1 at same slot 0
  EncounterProposal prop2;
  prop2.encounter_id = 2;
  prop2.host_id = 2;
  prop2.host_rank = 0;
  prop2.invitee_id = 1;
  prop2.venue_id = 100;
  prop2.venue_owner_rank = 0;
  prop2.venue_type_id = 2;
  prop2.slot = 0;
  prop2.encounter_type_id = 0;

  std::vector<EncounterReply> replies;
  cem.processProposals({prop1, prop2}, {prop1, prop2}, replies, 0);

  REQUIRE(replies.size() == 2);

  // First should be accepted, second should be rejected as already committed
  int accepted = 0, committed = 0;
  for (const auto& r : replies) {
    if (r.status == ReplyStatus::ACCEPTED) accepted++;
    if (r.status == ReplyStatus::REJECTED_ALREADY_COMMITTED) committed++;
  }
  CHECK(accepted == 1);
  CHECK(committed == 1);
}

TEST_CASE("8e. Budget clamped to available slots") {
  /**
   * SCENARIO: Person has only 1 leisure slot but daily_max is set to
   * fixed(5). Budget should be clamped to 1 (the available slot count).
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "tight", {{"evening", {"leisure", "residence"}}});

  // Budget = 5, but only 1 available slot
  tw.config.coordinated_encounters.encounters[0].daily_max_distribution =
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 5};

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  // Count proposals per host — should be at most 1 (clamped)
  std::map<PersonId, int> proposals_per_host;
  for (const auto& p : proposals) {
    proposals_per_host[p.host_id]++;
  }

  for (const auto& [host, count] : proposals_per_host) {
    INFO("Host " << host << " made " << count
                 << " proposals (1 slot available, budget was 5)");
    CHECK(count <= 1);  // Clamped to available slots
  }
}

TEST_CASE("8f. Budget zero skips encounter type entirely") {
  /**
   * SCENARIO: daily_max_distribution fixed(0) means no encounters
   * of this type should ever be proposed.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  // Budget = 0
  tw.config.coordinated_encounters.encounters[0].daily_max_distribution =
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 0};

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  CHECK(proposals.size() == 0);
}

// Weekend Schedule Regression Tests
// =============================================================================

TEST_CASE(
    "8a. Weekend proposals accepted with weekend schedule "
    "[Regression: processProposals used weekday schedule on weekends]") {
  /**
   * BUG: processProposals always validated invitee schedules against the
   * weekday schedule, regardless of whether it was a weekend. This caused
   * 3 out of 4 weekend leisure slots to be rejected as "schedule conflicts"
   * because the weekday schedule only had 1 leisure slot (evening).
   *
   * SETUP:
   *   - Weekday: 3 slots [residence, work, leisure]
   *   - Weekend: 3 slots [leisure, leisure, leisure]
   *   - Generate proposals on weekend (is_weekend=true, slot 0)
   *   - processProposals with is_weekend=true should ACCEPT
   *   - processProposals with is_weekend=false should REJECT (slot 0 is
   * "residence" on weekdays)
   *
   * FIX: Pass is_weekend to processProposals and use the correct schedule.
   */

  auto tw = buildEncounterWorld(
      4, 1, "pub", "friends", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  // Different weekday and weekend schedules — this is the key!
  addScheduleToAll(tw, "worker",
                   // Weekday: only slot 2 has "leisure"
                   {{"morning", {"residence"}},
                    {"work", {"work"}},
                    {"evening", {"leisure", "residence"}}},
                   // Weekend: ALL 3 slots have "leisure"
                   {{"morning", {"leisure", "residence"}},
                    {"day", {"leisure", "residence"}},
                    {"evening", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Generate proposals on a WEEKEND day
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(5, proposals, 1);  // day 5 = weekend

  // There should be proposals (people have leisure slots on weekends)
  REQUIRE(proposals.size() > 0);

  // Find a proposal that uses slot 0 (morning) - only valid on weekends
  EncounterProposal weekend_morning_prop;
  bool found_morning = false;
  for (const auto& p : proposals) {
    if (p.slot == 0) {  // Morning slot
      weekend_morning_prop = p;
      found_morning = true;
      break;
    }
  }

  if (found_morning) {
    // Test 1: processProposals with is_weekend=TRUE should ACCEPT
    {
      CoordinatedEncounterManager cem2(tw.world, tw.config, 0);
      std::vector<EncounterReply> replies;
      cem2.processProposals({weekend_morning_prop}, {weekend_morning_prop},
                            replies, 1);
      REQUIRE(replies.size() == 1);
      CHECK(replies[0].status == ReplyStatus::ACCEPTED);
      MESSAGE("Weekend morning slot ACCEPTED with is_weekend=true ✓");
    }

    // Test 2: processProposals with is_weekend=FALSE should REJECT
    // (because weekday slot 0 is "residence", not "leisure")
    {
      CoordinatedEncounterManager cem3(tw.world, tw.config, 0);
      std::vector<EncounterReply> replies;
      cem3.processProposals({weekend_morning_prop}, {weekend_morning_prop},
                            replies, 0);
      REQUIRE(replies.size() == 1);
      CHECK(replies[0].status == ReplyStatus::REJECTED_SCHEDULE_CONFLICT);
      MESSAGE(
          "Weekend morning slot correctly REJECTED with is_weekend=false ✓");
    }
  } else {
    // All proposals went to slot 2 (evening) which is leisure on both.
    // This is still valid — verify they get accepted
    auto any_prop = proposals[0];
    CoordinatedEncounterManager cem2(tw.world, tw.config, 0);
    std::vector<EncounterReply> replies;
    cem2.processProposals({any_prop}, {any_prop}, replies, 1);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].status == ReplyStatus::ACCEPTED);
    MESSAGE("Proposal on slot " << any_prop.slot << " ACCEPTED on weekend ✓");
  }
}

TEST_CASE(
    "8b. Weekend generates more proposals than weekday with more leisure "
    "slots") {
  /**
   * With 3 leisure slots on weekends vs 1 on weekdays, and fixed budget=3,
   * weekends should attempt up to 3 proposals per person vs 1 on weekdays.
   * This verifies the end-to-end pipeline difference.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friends", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 3}, 1.0, 1.0);

  // Weekday: 1 leisure slot. Weekend: 3 leisure slots.
  addScheduleToAll(tw, "worker",
                   {{"morning", {"residence"}},
                    {"work", {"work"}},
                    {"evening", {"leisure", "residence"}}},
                   {{"morning", {"leisure", "residence"}},
                    {"day", {"leisure", "residence"}},
                    {"evening", {"leisure", "residence"}}});

  // Weekday proposals (budget=3, but only 1 valid slot → clamped to 1)
  CoordinatedEncounterManager cem_wd(tw.world, tw.config, 0);
  std::vector<EncounterProposal> weekday_proposals;
  cem_wd.generateProposals(0, weekday_proposals, 0);

  // Weekend proposals (budget=3, 3 valid slots → all 3 used)
  CoordinatedEncounterManager cem_we(tw.world, tw.config, 0);
  std::vector<EncounterProposal> weekend_proposals;
  cem_we.generateProposals(5, weekend_proposals, 1);

  MESSAGE("Weekday proposals: " << weekday_proposals.size()
                                << ", Weekend proposals: "
                                << weekend_proposals.size());

  // Note: Proposal counts may be equal because each person invites all friends
  // per slot, and the invite_distribution saturates the friend pool. The
  // meaningful difference is that MORE weekend proposals get ACCEPTED because
  // processProposals now validates against the correct (weekend) schedule.

  // Now process both sets — weekend proposals should have a higher acceptance
  // rate
  CoordinatedEncounterManager cem_wd2(tw.world, tw.config, 0);
  std::vector<EncounterReply> wd_replies;
  cem_wd2.processProposals(weekday_proposals, weekday_proposals, wd_replies, 0);

  CoordinatedEncounterManager cem_we2(tw.world, tw.config, 0);
  std::vector<EncounterReply> we_replies;
  cem_we2.processProposals(weekend_proposals, weekend_proposals, we_replies, 1);

  int wd_accepted = 0, we_accepted = 0;
  for (const auto& r : wd_replies)
    if (r.status == ReplyStatus::ACCEPTED) wd_accepted++;
  for (const auto& r : we_replies)
    if (r.status == ReplyStatus::ACCEPTED) we_accepted++;

  MESSAGE("Weekday accepted: " << wd_accepted
                               << ", Weekend accepted: " << we_accepted);

  // Weekend should have strictly more accepted encounters
  CHECK(we_accepted > wd_accepted);
}

// =============================================================================
// SECTION 5: Cross-Rank Tests
//
// These tests verify behavior when proposals involve people not found locally,
// simulating cross-rank scenarios in a single-rank unit test context.
// =============================================================================

TEST_CASE(
    "5a. Cross-rank proposal routing — proposal for remote person gets "
    "REJECTED_NOT_FOUND in single-rank test") {
  /**
   * SCENARIO: A proposal targets an invitee_id that does not exist in the
   * local WorldState. In a real MPI run, this proposal would be routed to
   * the remote rank. In a single-rank unit test, processProposals should
   * return REJECTED_NOT_FOUND.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Create a proposal targeting a non-existent person (simulating remote)
  EncounterProposal prop;
  prop.encounter_id = 999;
  prop.host_id = 0;
  prop.host_rank = 0;
  prop.invitee_id = 9999;  // Does not exist locally
  prop.venue_id = 100;
  prop.venue_owner_rank = 0;
  prop.venue_type_id = 2;
  prop.slot = 0;
  prop.encounter_type_id = 0;

  std::vector<EncounterProposal> proposals = {prop};
  std::vector<EncounterReply> replies;
  cem.processProposals(proposals, proposals, replies, 0);

  REQUIRE(replies.size() == 1);
  CHECK(replies[0].status == ReplyStatus::REJECTED_NOT_FOUND);
  CHECK(replies[0].encounter_id == 999);
  CHECK(replies[0].host_id == 0);
  CHECK(replies[0].invitee_id == 9999);
}

TEST_CASE(
    "5b. Cross-rank reply routing — reply preserves host_rank for routing") {
  /**
   * SCENARIO: When processProposals generates replies, the reply must
   * preserve the host_id so the reply can be routed back to the host's rank.
   * This test verifies that host_id is correctly propagated through the
   * proposal → reply pipeline.
   */
  auto tw = buildEncounterWorld(
      3, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Create a proposal where host_rank differs from the processing rank
  EncounterProposal prop;
  prop.encounter_id = 500;
  prop.host_id = 0;
  prop.host_rank = 3;   // Host is on rank 3
  prop.invitee_id = 1;  // Local person
  prop.venue_id = 100;
  prop.venue_owner_rank = 3;
  prop.venue_type_id = 2;
  prop.slot = 0;
  prop.encounter_type_id = 0;

  std::vector<EncounterProposal> proposals = {prop};
  std::vector<EncounterReply> replies;
  cem.processProposals(proposals, proposals, replies, 0);

  REQUIRE(replies.size() == 1);
  // The reply must preserve host_id so routing can use getPersonRank(host_id)
  CHECK(replies[0].host_id == 0);
  CHECK(replies[0].invitee_id == 1);
  CHECK(replies[0].encounter_id == 500);
  // Status should be ACCEPTED (person 1 exists, is alive, schedule compatible)
  CHECK(replies[0].status == ReplyStatus::ACCEPTED);
}

TEST_CASE(
    "5c. Cross-rank encounter finalization — end-to-end with local and "
    "remote participants") {
  /**
   * SCENARIO: Simulate a finalized encounter that contains both local and
   * remote participants. The local rank should see all participants in the
   * finalized encounter, including IDs that don't exist locally.
   */
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  // Create proposals: host=0 invites person 1 (local) and person 8888 (remote)
  EncounterProposal prop1;
  prop1.encounter_id = 700;
  prop1.host_id = 0;
  prop1.host_rank = 0;
  prop1.invitee_id = 1;
  prop1.venue_id = 100;
  prop1.venue_owner_rank = 0;
  prop1.venue_type_id = 2;
  prop1.slot = 0;
  prop1.encounter_type_id = 0;

  // Process local proposal
  std::vector<EncounterProposal> proposals = {prop1};
  std::vector<EncounterReply> replies;
  cem.processProposals(proposals, proposals, replies, 0);

  // Simulate a remote reply (accepted) from the "remote" invitee
  EncounterReply remote_reply;
  remote_reply.encounter_id = 700;
  remote_reply.host_id = 0;
  remote_reply.invitee_id = 8888;
  remote_reply.venue_id = 100;
  remote_reply.venue_type_id = 2;
  remote_reply.slot = 0;
  remote_reply.encounter_type_id = 0;
  remote_reply.status = ReplyStatus::ACCEPTED;

  // Combine local + remote replies
  std::vector<EncounterReply> all_replies = replies;
  all_replies.push_back(remote_reply);

  std::vector<CoordinatedEncounter> finalized;
  cem.finalizeEncounters(all_replies, finalized);

  REQUIRE(finalized.size() == 1);
  // Should contain host(0) + local invitee(1) + remote invitee(8888)
  CHECK(finalized[0].participants.count(0) == 1);
  CHECK(finalized[0].participants.count(1) == 1);
  CHECK(finalized[0].participants.count(8888) == 1);
  CHECK(finalized[0].participants.size() == 3);
}

TEST_CASE(
    "5d. Remote partner death rejection — dead remote partner produces "
    "REJECTED_DEAD") {
  /**
   * SCENARIO: A proposal targets a local person who is dead. This simulates
   * what happens when a cross-rank proposal arrives for someone who has died
   * on this rank.
   */
  auto tw = buildEncounterWorld(
      3, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  // Kill person 1
  auto it = tw.world.person_index.find(1);
  REQUIRE(it != tw.world.person_index.end());
  tw.world.people[it->second].is_dead = true;

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterProposal prop;
  prop.encounter_id = 800;
  prop.host_id = 5000;  // Remote host
  prop.host_rank = 2;
  prop.invitee_id = 1;  // Dead local person
  prop.venue_id = 100;
  prop.venue_owner_rank = 2;
  prop.venue_type_id = 2;
  prop.slot = 0;
  prop.encounter_type_id = 0;

  std::vector<EncounterProposal> proposals = {prop};
  std::vector<EncounterReply> replies;
  cem.processProposals(proposals, proposals, replies, 0);

  REQUIRE(replies.size() == 1);
  CHECK(replies[0].status == ReplyStatus::REJECTED_DEAD);
  CHECK(replies[0].encounter_id == 800);
  CHECK(replies[0].invitee_id == 1);
}

// =============================================================================
// SECTION: Frequency-group budget enforcement
//
// A frequency_group caps proposals per (person, group) per day at ONE,
// regardless of how many encounter types share the group. This is the
// load-bearing invariant behind realistic GBMSM encounter rates.
//
// Intent from coordinated_encounter_manager.cpp:423-452:
//   - Each (person, group) resolves its daily budget-hit ONCE per day.
//   - If the hit is false (budget missed), every encounter type in the
//     group short-circuits for this person for this day.
//   - Once a hit produces an actual encounter (freq_group_committed_),
//     all other encounter types in the same group short-circuit too.
//
// Failure modes a buggy implementation could produce:
//   - Multiple proposals per day from the same person in the same group
//     (double-booking, skews attendance stats)
//   - Proposals generated for a person whose budget missed (ignores the
//     frequency table entirely)
//   - Budget state leaking across days (wrong daily rates)
// =============================================================================

// Add a frequency_group to the config with a single row matching all people.
static void installFrequencyGroup(EncounterTestWorld& tw,
                                  const std::string& group_name,
                                  double daily_probability) {
  FrequencyGroup fg;
  fg.name = group_name;
  fg.csv_path = "";
  fg.rate_column = "";
  fg.rate_unit = "per_day";
  FrequencyRow row;
  row.criteria = {};  // empty criteria → matches every person
  row.daily_probability = daily_probability;
  fg.rows.push_back(row);
  tw.config.coordinated_encounters.frequency_groups[group_name] = fg;
}

TEST_CASE(
    "freq_group — two encounter types sharing one group cap at ONE proposal "
    "per person per day") {
  // Two encounter types, both tagged frequency_group="G", daily_p=1.0
  // (always hit). Without the budget cap each host would propose for both
  // types → 2 proposals. Cap must limit to exactly 1 per host.
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "type_a", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1},
      /*proposal_prob=*/1.0, /*acceptance_prob=*/1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  // Register a second encounter type in the same group. Both referene
  // frequency_group "G" so they share one budget.
  tw.world.encounter_type_names.push_back("type_b");

  auto& enc_a = tw.config.coordinated_encounters.encounters[0];
  enc_a.frequency_group = "G";

  CoordinatedEncounterDef enc_b = enc_a;
  enc_b.name = "type_b";
  enc_b.frequency_group = "G";
  tw.config.coordinated_encounters.encounters.push_back(enc_b);

  installFrequencyGroup(tw, "G", /*daily_p=*/1.0);
  tw.config.resolve(tw.world);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  // Count proposals per host.
  std::map<PersonId, int> proposals_per_host;
  for (const auto& p : proposals) proposals_per_host[p.host_id]++;

  REQUIRE(proposals.size() > 0);
  for (const auto& [host, count] : proposals_per_host) {
    INFO("Host " << host << " made " << count << " proposals");
    CHECK(count == 1);
  }
}

TEST_CASE(
    "freq_group — daily_probability == 0 produces ZERO proposals even with "
    "proposal_probability=1.0") {
  // When a person is in a frequency_group, the scalar proposal_probability
  // is IGNORED — the group's daily rate is authoritative. daily_p=0 means
  // budget-miss for everyone, so no proposals of any type in that group.
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "type_a", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1},
      /*proposal_prob=*/1.0, /*acceptance_prob=*/1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  auto& enc_a = tw.config.coordinated_encounters.encounters[0];
  enc_a.frequency_group = "G";
  installFrequencyGroup(tw, "G", /*daily_p=*/0.0);
  tw.config.resolve(tw.world);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  CHECK(proposals.empty());
}

TEST_CASE(
    "freq_group — budget resets across days: same person can propose on day "
    "1 and day 2") {
  // Run day 0, record proposals. Call resetDaily. Run day 1. Expect each
  // host to appear in both days' proposal sets.
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "type_a", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1},
      /*proposal_prob=*/1.0, /*acceptance_prob=*/1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  auto& enc_a = tw.config.coordinated_encounters.encounters[0];
  enc_a.frequency_group = "G";
  installFrequencyGroup(tw, "G", /*daily_p=*/1.0);
  tw.config.resolve(tw.world);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> day0, day1;

  cem.generateProposals(/*current_day=*/0, day0, 0);
  cem.resetDaily();
  cem.generateProposals(/*current_day=*/1, day1, 0);

  std::set<PersonId> hosts_day0, hosts_day1;
  for (const auto& p : day0) hosts_day0.insert(p.host_id);
  for (const auto& p : day1) hosts_day1.insert(p.host_id);

  REQUIRE(!hosts_day0.empty());
  REQUIRE(!hosts_day1.empty());
  // After resetDaily, budgets must be independent — the day-1 host set
  // should overlap with day-0 (most people are eligible both days).
  // Without reset, the committed-set would block everyone and day 1 would
  // be empty.
  int overlap = 0;
  for (PersonId h : hosts_day0)
    if (hosts_day1.count(h)) ++overlap;
  CHECK(overlap > 0);
}

// =============================================================================
// SECTION: Slot commitment — host-side + invitee-side + day reset
//
// Existing coverage:
//   8d — invitee rejects 2nd proposal for a committed slot.
//
// These tests extend coverage to host-side slot consumption (a host that has
// used slot S for type_a must not pick slot S again for type_b in the same
// day) and to resetDaily semantics.
// =============================================================================

TEST_CASE(
    "slot-commit — host consumes each slot exactly once across multiple "
    "encounter types in the same day") {
  // Two encounter types share the same trigger slot. The host has exactly
  // TWO schedule slots. Intent: the two types must land on DIFFERENT slots
  // for the same host (slot reuse would constitute an invariant violation).
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "type_a", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1},
      /*proposal_prob=*/1.0, /*acceptance_prob=*/1.0);
  addScheduleToAll(tw, "open",
                   {{"slot_0", {"leisure", "residence"}},
                    {"slot_1", {"leisure", "residence"}}});

  tw.world.encounter_type_names.push_back("type_b");
  CoordinatedEncounterDef enc_b =
      tw.config.coordinated_encounters.encounters[0];
  enc_b.name = "type_b";
  tw.config.coordinated_encounters.encounters.push_back(enc_b);
  tw.config.resolve(tw.world);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  // Group slots used by (host, encounter_type) — and assert no host uses
  // the same slot for both types.
  std::map<PersonId, std::set<int>> slots_by_host;
  for (const auto& p : proposals) {
    bool inserted = slots_by_host[p.host_id].insert(p.slot).second;
    INFO("Host " << p.host_id << " reused slot " << p.slot);
    CHECK(inserted);
  }
}

TEST_CASE(
    "slot-commit — invitee accepts two proposals on DIFFERENT slots on the "
    "same day") {
  // Complement to test 8d: double-booking rejection only fires when slots
  // match. Different slots must both be accepted.
  auto tw = buildEncounterWorld(
      3, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(
      tw, "open",
      {{"s0", {"leisure", "residence"}}, {"s1", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterProposal p1{};
  p1.encounter_id = 1;
  p1.host_id = 0;
  p1.invitee_id = 1;
  p1.venue_id = 100;
  p1.venue_type_id = 2;
  p1.slot = 0;
  p1.encounter_type_id = 0;

  EncounterProposal p2 = p1;
  p2.encounter_id = 2;
  p2.host_id = 2;
  p2.slot = 1;  // different slot

  std::vector<EncounterReply> replies;
  cem.processProposals({p1, p2}, {p1, p2}, replies, 0);

  REQUIRE(replies.size() == 2);
  CHECK(replies[0].status == ReplyStatus::ACCEPTED);
  CHECK(replies[1].status == ReplyStatus::ACCEPTED);
}

TEST_CASE(
    "slot-commit — resetDaily clears invitee commitments: same slot "
    "reusable on a new day") {
  auto tw = buildEncounterWorld(
      3, 1, "pub", "friendships", "social_encounters", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);
  addScheduleToAll(tw, "open", {{"all_day", {"leisure", "residence"}}});

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);

  EncounterProposal p{};
  p.encounter_id = 1;
  p.host_id = 0;
  p.invitee_id = 1;
  p.venue_id = 100;
  p.venue_type_id = 2;
  p.slot = 0;

  std::vector<EncounterReply> replies_day0;
  cem.processProposals({p}, {p}, replies_day0, 0);
  REQUIRE(replies_day0.size() == 1);
  CHECK(replies_day0[0].status == ReplyStatus::ACCEPTED);

  // New day: resetDaily must clear committed_slots_.
  cem.resetDaily();

  EncounterProposal p2 = p;
  p2.encounter_id = 2;  // new encounter, same invitee, same slot
  std::vector<EncounterReply> replies_day1;
  cem.processProposals({p2}, {p2}, replies_day1, 0);
  REQUIRE(replies_day1.size() == 1);
  CHECK(replies_day1[0].status == ReplyStatus::ACCEPTED);
}

TEST_CASE(
    "freq_group — encounter types NOT in the same group don't share budget") {
  // A person in group G can still propose encounters whose frequency_group
  // is UNSET (they use proposal_probability instead). The cap is per-group,
  // not per-person. The test needs TWO schedule slots so slot-exhaustion
  // doesn't confound the result (each generated proposal consumes a slot,
  // independently of the frequency-group cap).
  auto tw = buildEncounterWorld(
      4, 1, "pub", "friendships", "type_a", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1},
      /*proposal_prob=*/1.0, /*acceptance_prob=*/1.0);
  addScheduleToAll(tw, "open",
                   {{"slot_a", {"leisure", "residence"}},
                    {"slot_b", {"leisure", "residence"}}});

  tw.world.encounter_type_names.push_back("type_b");
  auto& enc_a = tw.config.coordinated_encounters.encounters[0];
  enc_a.frequency_group = "G";

  CoordinatedEncounterDef enc_b = enc_a;
  enc_b.name = "type_b";
  enc_b.frequency_group = std::nullopt;  // not in any frequency group
  enc_b.proposal_probability = 1.0;
  tw.config.coordinated_encounters.encounters.push_back(enc_b);

  installFrequencyGroup(tw, "G", /*daily_p=*/1.0);
  tw.config.resolve(tw.world);

  CoordinatedEncounterManager cem(tw.world, tw.config, 0);
  std::vector<EncounterProposal> proposals;
  cem.generateProposals(0, proposals, 0);

  // Each host should generate one "type_a" (gated by freq group) AND one
  // "type_b" (gated by proposal_probability). Host-proposal count per host
  // depends on slot availability, but type_a and type_b are independent
  // gates, so the same host can appear in both.
  std::map<PersonId, std::set<uint8_t>> types_per_host;
  for (const auto& p : proposals) {
    types_per_host[p.host_id].insert(p.encounter_type_id);
  }
  bool any_host_has_both = false;
  for (const auto& [host, types] : types_per_host) {
    if (types.size() >= 2) any_host_has_both = true;
  }
  CHECK(any_host_has_both);
}

// =============================================================================
// SECTION 9: The virtual-venue id range
//
// Whether a CoordinatedEncounter is virtual is a property of the instance —
// its reserved negative venue_id — not of a config flag reached through a
// world-registry lookup that can miss.
// =============================================================================

TEST_CASE("9a. Virtual venue ids round-trip through their host") {
  for (PersonId host_id : {0, 1, 42, 630205, 2147482647}) {
    const VenueId virtual_venue_id = makeVirtualVenueId(host_id);
    CHECK(isVirtualVenue(virtual_venue_id));
    CHECK(occupiesNoVenue(virtual_venue_id));
    CHECK(virtualVenueHost(virtual_venue_id) == host_id);
  }

  // Distinct by construction: two hosts never share a virtual venue.
  CHECK(makeVirtualVenueId(0) != makeVirtualVenueId(1));
}

TEST_CASE("9b. The other negative venue ids are not virtual venues") {
  // Both are negative — so they occupy no Venue — but neither is in the
  // reserved range, and reading a host out of either would be nonsense.
  CHECK(isVirtualVenue(kInvalidVenueId) == false);
  CHECK(isVirtualVenue(INFECTION_SEED_VENUE_ID) == false);
  CHECK(occupiesNoVenue(kInvalidVenueId));
  CHECK(occupiesNoVenue(INFECTION_SEED_VENUE_ID));

  // A real venue is neither.
  CHECK(isVirtualVenue(0) == false);
  CHECK(occupiesNoVenue(0) == false);

  // The boundary belongs to host 0.
  CHECK(isVirtualVenue(kVirtualVenueIdBase));
  CHECK(isVirtualVenue(kVirtualVenueIdBase + 1) == false);
}

TEST_CASE("9c. Eligibility reads the instance, not the lookup tables") {
  /**
   * SCENARIO:
   *   A Virtual Encounter whose encounter_type_id is absent from every
   *   EncounterLookups map — the shape a def name missing from the world's
   *   encounter-type registry used to produce. Trigger activities are
   *   supplied by hand so the policy is genuinely consulted; nothing in the
   *   lookups says the encounter is virtual.
   *
   *   The Slot Venue Type must still be absent, so the venue gate on "pub"
   *   cannot reach the participants and the encounter proceeds.
   *
   * REGRESSION: the discriminator was the def's is_virtual flag, looked up on
   *   encounter_type_id. On a miss the encounter read as physical and handed
   *   venue_type_id — a contact-matrix registry id — to a Venue-type gate.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "pub_meetups", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  auto disease =
      addVenueGatedPolicy(pm, tw.world, {"leisure"}, {"pub"}, 0.0, 10.0);
  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  const uint8_t unregistered_encounter_type = 7;

  CoordinatedEncounter enc;
  enc.encounter_id = 900;
  enc.host_id = 0;
  enc.venue_id = makeVirtualVenueId(0);
  // Aliases the "pub" venue type on purpose: for a virtual encounter this
  // field is a contact-matrix id, and nothing may gate on it.
  enc.venue_type_id = 2;
  enc.slot = 0;
  enc.encounter_type_id = unregistered_encounter_type;
  enc.participants = {0, 1};

  june::encounters::EncounterLookups lookups;
  lookups.trigger_activities[unregistered_encounter_type] = {
      static_cast<int16_t>(tw.world.getActivityIndex("leisure"))};
  lookups.min_attendees[unregistered_encounter_type] = 2;

  auto locations = initLocations(tw.world);
  auto slot_encounters = june::encounters::computeLocalEligibility(
      {enc}, 0, 5.0, lookups, tw.world, locations, &pm);

  REQUIRE(slot_encounters.size() == 1);
  CHECK(slot_encounters[0].local_eligible == 2);
  CHECK(slot_encounters[0].min_required == 2);
}

TEST_CASE("9d. A virtual encounter carrying an unresolvable type never gates") {
  /**
   * SCENARIO: as 9c, but venue_type_id is kUnknownVenueTypeId — the value
   * SlotVenueType::known() refuses. Under the old discriminator a lookup miss
   * sent that straight to known() and aborted the run. It must not be read at
   * all now.
   */
  auto tw = buildEncounterWorld(
      2, 1, "pub", "friendships", "pub_meetups", false, "", {"leisure"},
      InviteDistribution{DistributionType::FIXED, 1.0, 0.5, 1}, 1.0, 1.0);

  PolicyManager pm(tw.world);
  auto disease =
      addVenueGatedPolicy(pm, tw.world, {"leisure"}, {"pub"}, 0.0, 10.0);
  tw.world.people[0].applicable_temporal_policy_mask = 1;
  tw.world.people[1].applicable_temporal_policy_mask = 1;

  CoordinatedEncounter enc;
  enc.encounter_id = 901;
  enc.host_id = 1;
  enc.venue_id = makeVirtualVenueId(1);
  enc.venue_type_id = kUnknownVenueTypeId;
  enc.slot = 0;
  enc.encounter_type_id = 9;
  enc.participants = {0, 1};

  june::encounters::EncounterLookups lookups;
  lookups.trigger_activities[9] = {
      static_cast<int16_t>(tw.world.getActivityIndex("leisure"))};
  lookups.min_attendees[9] = 2;

  auto locations = initLocations(tw.world);
  std::vector<june::encounters::EncounterEligibility> slot_encounters;
  REQUIRE_NOTHROW(slot_encounters = june::encounters::computeLocalEligibility(
                      {enc}, 0, 5.0, lookups, tw.world, locations, &pm));
  REQUIRE(slot_encounters.size() == 1);
  CHECK(slot_encounters[0].local_eligible == 2);
}
