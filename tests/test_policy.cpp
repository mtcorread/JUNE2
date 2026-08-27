#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "epidemiology/policy.h"

#include "doctest.h"
#include <fstream>

#include "epidemiology/disease.h"
#include "epidemiology/epidemiology.h"
#include "loaders/policy_loader.h"
#include "test_utils.h"

using namespace june;

// =============================================================================
// Scheduled-venue filter — both directions (override_venue_types,
// exempt_venue_types).
//
// A policy action targets Activities only, and one Activity reaches many Venue
// types (leisure → pub, grocery, care_home...). The gate narrows an override to
// a named set of venue types, ANDed with the activity mask, so pubs can close
// while groceries stay open.
// =============================================================================

namespace {

constexpr int16_t kResidence = 0;
constexpr int16_t kLeisure = 1;
constexpr int16_t kPrimaryActivity = 2;

constexpr VenueId kHouseholdVenue = 0;
constexpr VenueId kPubVenue = 1;
constexpr VenueId kGroceryVenue = 2;
constexpr VenueId kCareHomeVenue = 3;
constexpr VenueId kClassroomVenue = 4;
constexpr VenueId kOfficeVenue = 5;

constexpr uint8_t kPubVenueType = 1;

constexpr int kVenueTypeCount = 6;

// venue types: household=0, pub=1, grocery=2, care_home=3, classroom=4,
//              office=5
// activities:  residence=0, leisure=1, primary_activity=2
// Every person's residence is the household venue, so an override that fires
// redirects them there.
WorldState buildVenueGateWorld(int num_people = 1) {
  WorldState world =
      TestWorldFactory::createMinimalWorld(num_people, kVenueTypeCount);
  world.activity_names = {"residence", "leisure", "primary_activity"};
  world.venue_type_names = {"household",  "pub",       "grocery",
                            "care_home",  "classroom", "office"};
  for (int i = 0; i < kVenueTypeCount; ++i) {
    world.venues[i].type_id = static_cast<uint8_t>(i);
  }
  world.buildIndices();

  for (auto& person : world.people) {
    person.activity_meta_start =
        static_cast<uint32_t>(world.activity_meta.size());
    person.activity_meta_count = 1;
    world.activity_meta.push_back(
        {kResidence, static_cast<uint32_t>(world.activity_venues.size()), 1});
    world.activity_venues.push_back({kHouseholdVenue, 0});
    // Normally set by precomputePolicyApplicability.
    person.applicable_temporal_policy_mask = 1;
  }
  return world;
}

TemporalPolicy makeClosurePolicy(
    const std::vector<std::string>& activities,
    const std::vector<std::string>& venue_types,
    VenueGateDirection direction = VenueGateDirection::RestrictTo) {
  TemporalPolicy policy;
  policy.name = "closure";
  policy.window.start_time = 0.0;
  policy.window.end_time = 10.0;
  policy.action.override_activities.insert(activities.begin(),
                                           activities.end());
  auto& gated_types = (direction == VenueGateDirection::ExemptFrom)
                          ? policy.action.exempt_venue_types
                          : policy.action.override_venue_types;
  gated_types.insert(venue_types.begin(), venue_types.end());
  policy.action.replacement_activity = "residence";
  policy.action.compliance_rate = 1.0;
  return policy;
}

// The common case: the person is pinned at, and occupies, the same venue.
std::optional<PersonLocation> runOverride(PolicyManager& policy_manager,
                                          Person& person,
                                          int16_t activity_index,
                                          VenueId venue_id) {
  return policy_manager.getOverride(person, activity_index, venue_id, 0,
                                    SlotVenueType::fromVenue(venue_id), 5.0, 0);
}

// Pin and Slot Venue Type stated separately: the two diverge for a traveller
// in transit, pinned at their last overnight venue while occupying no venue.
std::optional<PersonLocation> runOverride(PolicyManager& policy_manager,
                                          Person& person,
                                          int16_t activity_index,
                                          VenueId pin_venue_id,
                                          SlotVenueType slot_venue_type) {
  return policy_manager.getOverride(person, activity_index, pin_venue_id, 0,
                                    slot_venue_type, 5.0, 0);
}

}  // namespace

TEST_CASE("Venue gate - restrict-to fires only at listed venue types") {
  WorldState world = buildVenueGateWorld();
  PolicyManager policy_manager(world);

  TemporalPolicy close_pubs = makeClosurePolicy({"leisure"}, {"pub"});
  close_pubs.resolve(world);
  policy_manager.addTemporalPolicy(close_pubs);

  Person& person = world.people[0];

  SUBCASE("fires at a listed venue type") {
    auto override_location =
        runOverride(policy_manager, person, kLeisure, kPubVenue);
    REQUIRE(override_location.has_value());
    CHECK(override_location->venue_id == kHouseholdVenue);
    CHECK(override_location->activity_index == kResidence);
  }

  SUBCASE("does not fire at an unlisted venue type, same activity") {
    CHECK_FALSE(
        runOverride(policy_manager, person, kLeisure, kGroceryVenue).has_value());
  }

  SUBCASE("absent venue (venue_id < 0) does not fire") {
    CHECK_FALSE(
        runOverride(policy_manager, person, kLeisure, kInvalidVenueId)
            .has_value());
  }

  SUBCASE("unresolvable venue id throws") {
    // A venue that exists but cannot be typed is a defect, not an absence.
    CHECK_THROWS_AS(runOverride(policy_manager, person, kLeisure, 9999),
                    std::runtime_error);
  }
}

TEST_CASE("Venue gate - ANDs with the activity mask") {
  WorldState world = buildVenueGateWorld();
  PolicyManager policy_manager(world);

  SUBCASE("named activity: venue matches but activity does not") {
    TemporalPolicy close_pubs = makeClosurePolicy({"leisure"}, {"pub"});
    close_pubs.resolve(world);
    policy_manager.addTemporalPolicy(close_pubs);

    CHECK_FALSE(runOverride(policy_manager, world.people[0], kPrimaryActivity,
                            kPubVenue)
                    .has_value());
  }

  SUBCASE("wildcard activity: gate still restricts by venue type") {
    TemporalPolicy close_pubs = makeClosurePolicy({"*"}, {"pub"});
    close_pubs.resolve(world);
    policy_manager.addTemporalPolicy(close_pubs);

    Person& person = world.people[0];
    CHECK(runOverride(policy_manager, person, kPrimaryActivity, kPubVenue)
              .has_value());
    CHECK_FALSE(runOverride(policy_manager, person, kLeisure, kGroceryVenue)
                    .has_value());
  }
}

TEST_CASE("Venue gate - multi-role venue type separates the three roles") {
  // care_home is reached by leisure (visitors), primary_activity (staff) and
  // residence (residents). Banning visits must not evict residents or stop
  // staff working.
  WorldState world = buildVenueGateWorld(3);
  PolicyManager policy_manager(world);

  TemporalPolicy ban_visits = makeClosurePolicy({"leisure"}, {"care_home"});
  ban_visits.resolve(world);
  policy_manager.addTemporalPolicy(ban_visits);

  Person& visitor = world.people[0];
  Person& worker = world.people[1];
  Person& resident = world.people[2];

  CHECK(runOverride(policy_manager, visitor, kLeisure, kCareHomeVenue)
            .has_value());
  CHECK_FALSE(runOverride(policy_manager, worker, kPrimaryActivity,
                          kCareHomeVenue)
                  .has_value());
  CHECK_FALSE(
      runOverride(policy_manager, resident, kResidence, kCareHomeVenue)
          .has_value());
}

TEST_CASE("Venue gate - absent means unchanged behaviour") {
  WorldState world = buildVenueGateWorld();
  PolicyManager policy_manager(world);

  TemporalPolicy close_leisure = makeClosurePolicy({"leisure"}, {});
  close_leisure.resolve(world);
  policy_manager.addTemporalPolicy(close_leisure);

  Person& person = world.people[0];
  CHECK(runOverride(policy_manager, person, kLeisure, kPubVenue).has_value());
  CHECK(runOverride(policy_manager, person, kLeisure, kGroceryVenue)
            .has_value());
  CHECK(runOverride(policy_manager, person, kLeisure, kInvalidVenueId)
            .has_value());
}

TEST_CASE("Venue gate - validation throws from resolve") {
  WorldState world = buildVenueGateWorld();

  SUBCASE("unknown venue type name") {
    TemporalPolicy policy = makeClosurePolicy({"leisure"}, {"tavern"});
    policy.name = "close_taverns";
    CHECK_THROWS_AS(policy.resolve(world), std::runtime_error);
  }

  SUBCASE("venue type index at or beyond the 64-bit mask width") {
    world.venue_type_names.resize(70);
    for (size_t i = 4; i < world.venue_type_names.size(); ++i) {
      world.venue_type_names[i] = "filler_" + std::to_string(i);
    }
    TemporalPolicy policy = makeClosurePolicy({"leisure"}, {"filler_64"});
    CHECK_THROWS_AS(policy.resolve(world), std::runtime_error);
  }
}

// =============================================================================
// Exempt-from direction
// =============================================================================

TEST_CASE("Venue gate - exempt-from is the inverse of restrict-to") {
  WorldState world = buildVenueGateWorld();
  PolicyManager policy_manager(world);

  // Close all leisure except the pub.
  TemporalPolicy close_all_but_pubs = makeClosurePolicy(
      {"leisure"}, {"pub"}, VenueGateDirection::ExemptFrom);
  close_all_but_pubs.resolve(world);
  policy_manager.addTemporalPolicy(close_all_but_pubs);

  Person& person = world.people[0];

  SUBCASE("does not fire at a listed venue type") {
    CHECK_FALSE(
        runOverride(policy_manager, person, kLeisure, kPubVenue).has_value());
  }

  SUBCASE("fires at an unlisted venue type, same activity") {
    auto override_location =
        runOverride(policy_manager, person, kLeisure, kGroceryVenue);
    REQUIRE(override_location.has_value());
    CHECK(override_location->venue_id == kHouseholdVenue);
    CHECK(override_location->activity_index == kResidence);
  }

  SUBCASE("absent venue (venue_id < 0) does not fire either") {
    // Absent never passes the filter in either direction: "not at a listed
    // venue type" and "at no venue at all" are different questions, and only
    // the first is what exempt-from asks.
    CHECK_FALSE(runOverride(policy_manager, person, kLeisure, kInvalidVenueId)
                    .has_value());
  }

  SUBCASE("unresolvable venue id throws") {
    CHECK_THROWS_AS(runOverride(policy_manager, person, kLeisure, 9999),
                    std::runtime_error);
  }

  SUBCASE("still ANDs with the activity mask") {
    CHECK_FALSE(runOverride(policy_manager, person, kPrimaryActivity,
                            kGroceryVenue)
                    .has_value());
  }
}

TEST_CASE("Venue gate - both directions on one action throws from resolve") {
  WorldState world = buildVenueGateWorld();

  TemporalPolicy policy = makeClosurePolicy({"leisure"}, {"pub"});
  policy.name = "contradictory_closure";
  policy.action.exempt_venue_types = {"grocery"};
  CHECK_THROWS_AS(policy.resolve(world), std::runtime_error);
}

TEST_CASE("Venue gate - a swallowed resolve failure poisons the gate") {
  // The throw from resolve is the only thing between a typo in policies.yaml
  // and a policy that appears configured but never fires. Simulate the one
  // refactor away: a caller that catches and carries on.
  WorldState world = buildVenueGateWorld();

  SUBCASE("unknown venue type name leaves the gate throwing when consulted") {
    TemporalPolicy policy = makeClosurePolicy({"leisure"}, {"tavern"});
    policy.name = "close_taverns";
    try {
      policy.resolve(world);
    } catch (const std::runtime_error&) {
    }

    // Not silently ungated: still gated, and refuses to answer.
    CHECK(policy.action.hasVenueGate());
    CHECK_THROWS_AS(policy.action.passesVenueGate(SlotVenueType::known(kPubVenueType)),
                    std::runtime_error);

    PolicyManager policy_manager(world);
    policy_manager.addTemporalPolicy(policy);
    CHECK_THROWS_AS(runOverride(policy_manager, world.people[0], kLeisure,
                                kPubVenue),
                    std::runtime_error);
  }

  SUBCASE("both directions set leaves the gate poisoned too") {
    TemporalPolicy policy = makeClosurePolicy({"leisure"}, {"pub"});
    policy.action.exempt_venue_types = {"grocery"};
    try {
      policy.resolve(world);
    } catch (const std::runtime_error&) {
    }
    CHECK(policy.action.hasVenueGate());
    CHECK_THROWS_AS(policy.action.passesVenueGate(SlotVenueType::known(kPubVenueType)),
                    std::runtime_error);
  }

  SUBCASE("an ungated action is never poisoned") {
    TemporalPolicy policy = makeClosurePolicy({"leisure"}, {});
    policy.resolve(world);
    CHECK_FALSE(policy.action.hasVenueGate());
    CHECK(policy.action.passesVenueGate(SlotVenueType::known(kPubVenueType)));
  }

  SUBCASE("a successful re-resolve clears an earlier poisoning") {
    TemporalPolicy policy = makeClosurePolicy({"leisure"}, {"tavern"});
    try {
      policy.resolve(world);
    } catch (const std::runtime_error&) {
    }
    policy.action.override_venue_types = {"pub"};
    policy.resolve(world);
    CHECK(policy.action.hasVenueGate());
    CHECK(policy.action.passesVenueGate(SlotVenueType::known(kPubVenueType)));
  }
}

// =============================================================================
// Slot Venue Type — the gate key, separate from the pin
// =============================================================================

TEST_CASE("SlotVenueType - the three states") {
  WorldState world = buildVenueGateWorld();

  SUBCASE("known() refuses the unresolvable sentinel") {
    CHECK_THROWS_AS(SlotVenueType::known(kUnknownVenueTypeId),
                    std::runtime_error);
  }

  SUBCASE("a negative venue id is absent, not deferred") {
    CHECK(SlotVenueType::fromVenue(kInvalidVenueId)
              .resolveAgainst(world)
              .isAbsent());
  }

  SUBCASE("a real venue resolves to its type") {
    CHECK(SlotVenueType::fromVenue(kPubVenue).resolveAgainst(world).typeId() ==
          kPubVenueType);
  }

  SUBCASE("an id naming no Venue throws rather than yielding 255") {
    CHECK_THROWS_AS(SlotVenueType::fromVenue(9999).resolveAgainst(world),
                    std::runtime_error);
  }

  SUBCASE("typeId() on a non-Known value is a programming error") {
    CHECK_THROWS_AS(SlotVenueType::absent().typeId(), std::runtime_error);
    CHECK_THROWS_AS(SlotVenueType::fromVenue(kPubVenue).typeId(),
                    std::runtime_error);
  }
}

TEST_CASE("Venue gate - the pin does not stand in for the slot venue type") {
  // A traveller in no_venue transit: ActivityManager substitutes their last
  // overnight venue so a freeze_in_place override has something to pin them
  // to, but they occupy no venue this slot. Gating on the substituted venue
  // would apply a household-restricted policy to someone on a coach.
  WorldState world = buildVenueGateWorld();

  SUBCASE("restrict-to does not fire on the pinned venue type") {
    PolicyManager policy_manager(world);
    TemporalPolicy close_households =
        makeClosurePolicy({"leisure"}, {"household"});
    close_households.resolve(world);
    policy_manager.addTemporalPolicy(close_households);

    // Pinned at the household, occupying nothing.
    CHECK_FALSE(runOverride(policy_manager, world.people[0], kLeisure,
                            kHouseholdVenue, SlotVenueType::absent())
                    .has_value());
    // Same policy, actually at the household: fires.
    CHECK(runOverride(policy_manager, world.people[0], kLeisure,
                      kHouseholdVenue)
              .has_value());
  }

  SUBCASE("exempt-from does not fire either") {
    PolicyManager policy_manager(world);
    TemporalPolicy close_all_but_households = makeClosurePolicy(
        {"leisure"}, {"household"}, VenueGateDirection::ExemptFrom);
    close_all_but_households.resolve(world);
    policy_manager.addTemporalPolicy(close_all_but_households);

    CHECK_FALSE(runOverride(policy_manager, world.people[0], kLeisure,
                            kHouseholdVenue, SlotVenueType::absent())
                    .has_value());
  }
}

TEST_CASE("Venue gate - an ungated policy never pays for the lookup") {
  // The resolve is lazy and only a gated action triggers it, so an
  // unresolvable venue must pass through an ungated run untouched. If this
  // throws, every ungated production run now hard-fails on a cross-rank venue.
  WorldState world = buildVenueGateWorld();
  PolicyManager policy_manager(world);

  TemporalPolicy close_leisure = makeClosurePolicy({"leisure"}, {});
  close_leisure.resolve(world);
  policy_manager.addTemporalPolicy(close_leisure);

  CHECK(runOverride(policy_manager, world.people[0], kLeisure, 9999)
            .has_value());
}

// =============================================================================
// Composition with the conjuncts either side of the gate
// =============================================================================

TEST_CASE("Venue gate - Sticky Compliance is unaffected by the gate") {
  // The gate sits after Sticky Compliance, so a slot the gate blocks must
  // still leave the person's compliance decision exactly as an ungated policy
  // would, and that decision must stick across slots.
  constexpr int kNumPeople = 16;

  WorldState gated_world = buildVenueGateWorld(kNumPeople);
  PolicyManager gated_manager(gated_world);
  TemporalPolicy close_pubs = makeClosurePolicy({"leisure"}, {"pub"});
  close_pubs.action.compliance_rate = 0.5;
  close_pubs.resolve(gated_world);
  gated_manager.addTemporalPolicy(close_pubs);

  WorldState ungated_world = buildVenueGateWorld(kNumPeople);
  PolicyManager ungated_manager(ungated_world);
  TemporalPolicy close_leisure = makeClosurePolicy({"leisure"}, {});
  close_leisure.action.compliance_rate = 0.5;
  close_leisure.resolve(ungated_world);
  ungated_manager.addTemporalPolicy(close_leisure);

  int compliers = 0;
  for (int i = 0; i < kNumPeople; ++i) {
    Person& gated_person = gated_world.people[i];

    // First slot at the gated type reveals the compliance decision.
    const bool complies =
        runOverride(gated_manager, gated_person, kLeisure, kPubVenue)
            .has_value();
    compliers += complies ? 1 : 0;

    // An intervening slot the gate blocks must not disturb the latch.
    CHECK_FALSE(runOverride(gated_manager, gated_person, kLeisure, kGroceryVenue)
                    .has_value());
    CHECK(runOverride(gated_manager, gated_person, kLeisure, kPubVenue)
              .has_value() == complies);

    // Same decision as the identical policy without a gate.
    CHECK(runOverride(ungated_manager, ungated_world.people[i], kLeisure,
                      kGroceryVenue)
              .has_value() == complies);
  }

  // Test has power only if both outcomes occur.
  CHECK(compliers > 0);
  CHECK(compliers < kNumPeople);
}

TEST_CASE("Venue gate - gate and exemption are independent conjuncts") {
  // Close schools (primary_activity at classroom), key workers exempt.
  // Key worker is proxied by age: people are aged 20 + index.
  WorldState world = buildVenueGateWorld(2);
  PolicyManager policy_manager(world);

  TemporalPolicy close_schools =
      makeClosurePolicy({"primary_activity"}, {"classroom"});
  close_schools.name = "close_schools";
  ActivityExemption key_worker_exemption;
  key_worker_exemption.activity_name = "primary_activity";
  SelectionCriterion age_criterion;
  age_criterion.property_path = "age";
  age_criterion.operator_type = ">";
  age_criterion.value = 20.5;
  key_worker_exemption.criteria.push_back(age_criterion);
  close_schools.action.exemptions.push_back(key_worker_exemption);
  close_schools.resolve(world);
  policy_manager.addTemporalPolicy(close_schools);

  Person& non_key_worker = world.people[0];  // age 20
  Person& key_worker = world.people[1];      // age 21

  // Blocked by the exemption, at a gated venue type.
  CHECK_FALSE(runOverride(policy_manager, key_worker, kPrimaryActivity,
                          kClassroomVenue)
                  .has_value());
  // Blocked by the gate, exemption does not apply.
  CHECK_FALSE(runOverride(policy_manager, non_key_worker, kPrimaryActivity,
                          kOfficeVenue)
                  .has_value());
  // Neither blocks: overridden.
  CHECK(runOverride(policy_manager, non_key_worker, kPrimaryActivity,
                    kClassroomVenue)
            .has_value());
}

TEST_CASE("Venue gate - symptom path is gated too") {
  WorldState world = buildVenueGateWorld();

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
  Disease disease("TestDisease", symptom_tags, stage_settings, {trajectory}, {},
                  transmission);

  PolicyManager policy_manager(world);
  SymptomPolicy stay_out_of_pubs;
  stay_out_of_pubs.name = "sick_avoid_pubs";
  stay_out_of_pubs.trigger_symptoms = {"sick"};
  stay_out_of_pubs.action.override_activities = {"leisure"};
  stay_out_of_pubs.action.override_venue_types = {"pub"};
  stay_out_of_pubs.action.replacement_activity = "residence";
  stay_out_of_pubs.action.compliance_rate = 1.0;
  policy_manager.addSymptomPolicy(stay_out_of_pubs);
  policy_manager.resolveAll(disease);

  Person& person = world.people[0];
  person.infection = std::make_unique<Infection>(&disease, 0.0, &person, 42,
                                                 nullptr, "household", 0);
  person.applicable_symptom_policy_mask = 1;

  CHECK(runOverride(policy_manager, person, kLeisure, kPubVenue).has_value());
  CHECK_FALSE(runOverride(policy_manager, person, kLeisure, kGroceryVenue)
                  .has_value());
}

// =============================================================================
// Policy Suppression — the question stripped of its consequence
//
// suppressesParticipation answers what getOverride answers, and writes none of
// what getOverride writes: no freeze established or released, no schedule hop
// swapped, no compliance decision latched. Drive any of these through
// getOverride instead and the writes reappear — that is the defect these guard.
// =============================================================================

namespace {

constexpr int16_t kFreezeScheduleIdx = 0;
constexpr int16_t kHoppedSchedule = 3;
constexpr int16_t kReturnSchedule = 1;

// Sick for `sick_days`, then healthy: a freeze can be established while the
// symptom holds and released once it lifts. A long illness leaves a window
// edge, rather than recovery, as the only thing that can end a policy.
Disease buildTwoStageDisease(double sick_days = 2.0) {
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
  trajectory.stages.push_back({"sick", {"constant", {{"value", sick_days}}}});
  trajectory.stages.push_back({"healthy", {"constant", {{"value", 100.0}}}});
  return Disease("TestDisease", symptom_tags, stage_settings, {trajectory}, {},
                 transmission);
}

// As above, but the trajectory ends in death rather than recovery.
Disease buildFatalDisease() {
  TransmissionParams transmission;
  transmission.mode = InfectiousnessMode::STAGE_DRIVEN;
  auto curve = std::make_shared<ConstantCurve>(1.0);
  transmission.stage_curves["sick"] = curve;
  transmission.symptom_id_curves = {nullptr, curve, nullptr};
  std::vector<SymptomTag> symptom_tags = {
      {"healthy", -1, 0}, {"sick", 1, 1}, {"dead", 2, 2}};
  DiseaseStageSettings stage_settings;
  stage_settings.recovered_stages = {"healthy"};
  stage_settings.fatality_stages = {"dead"};
  TrajectoryDefinition trajectory;
  trajectory.selection_key = "general";
  trajectory.severity = 1.0;
  trajectory.stages.push_back({"sick", {"constant", {{"value", 2.0}}}});
  trajectory.stages.push_back({"dead", {"constant", {{"value", 100.0}}}});
  return Disease("TestDisease", symptom_tags, stage_settings, {trajectory}, {},
                 transmission);
}

// The production shape: freeze_in_place over every activity. Only a hopped
// person reaches the freeze branch, and only via a symptom policy.
SymptomPolicy makeFreezePolicy(const std::vector<std::string>& venue_types = {},
                               double compliance_rate = 1.0) {
  SymptomPolicy policy;
  policy.name = "freeze_when_sick";
  policy.trigger_symptoms = {"sick"};
  policy.action.override_activities = {"*"};
  policy.action.override_venue_types.insert(venue_types.begin(),
                                            venue_types.end());
  policy.action.replacement_activity = "residence";
  policy.action.compliance_rate = compliance_rate;
  // resolve() only fills this from a non-empty replacement_schedule name, so
  // setting the index directly survives resolveAll.
  policy.action.replacement_schedule_idx = kFreezeScheduleIdx;
  return policy;
}

Person& makeSickPerson(WorldState& world, Disease& disease,
                       size_t person_idx = 0) {
  Person& person = world.people[person_idx];
  person.infection = std::make_unique<Infection>(&disease, 0.0, &person, 42,
                                                 nullptr, "household", 0);
  person.applicable_symptom_policy_mask = 1;
  return person;
}

Person& makeSickHoppedPerson(WorldState& world, Disease& disease,
                             size_t person_idx = 0) {
  Person& person = makeSickPerson(world, disease, person_idx);
  person.schedule_hop = ScheduleHop::begin(kHoppedSchedule, kReturnSchedule);
  return person;
}

// The query counterpart of runOverride, same person, slot and time.
bool querySuppressed(const PolicyManager& policy_manager, const Person& person,
                     int16_t activity_index, VenueId venue_id) {
  return policy_manager.suppressesParticipation(
      person, activity_index, SlotVenueType::fromVenue(venue_id), 5.0);
}

}  // namespace

TEST_CASE("Policy suppression does not establish a freeze") {
  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease();
  PolicyManager policy_manager(world);
  policy_manager.addSymptomPolicy(makeFreezePolicy({"pub"}));
  policy_manager.resolveAll(disease);

  Person& person = makeSickHoppedPerson(world, disease);

  CHECK(policy_manager.suppressesParticipation(
      person, kLeisure, SlotVenueType::known(kPubVenueType), 1.0));

  // Nothing was pinned, so the person is still on their own hop.
  CHECK(policy_manager.getFrozenStates().empty());
  CHECK(person.schedule_hop.hopped_schedule_id == kHoppedSchedule);
  CHECK(person.schedule_hop.return_schedule_id == kReturnSchedule);
}

TEST_CASE("Policy suppression does not release a freeze") {
  // The mirror of the above: a probe asked after the symptom lifts must not
  // thaw a person some other caller froze. Only the owner of the freeze ends
  // it, at the moment it chooses.
  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease();
  PolicyManager policy_manager(world);
  policy_manager.addSymptomPolicy(makeFreezePolicy());
  policy_manager.resolveAll(disease);

  Person& person = makeSickHoppedPerson(world, disease);

  // Day 1: still sick, so the override freezes and swaps the hop.
  REQUIRE(policy_manager
              .getOverride(person, kLeisure, kPubVenue, 0,
                           SlotVenueType::known(kPubVenueType), 1.0, 0)
              .has_value());
  REQUIRE(policy_manager.getFrozenStates().count(person.id) == 1);
  REQUIRE(person.schedule_hop.hopped_schedule_id == kFreezeScheduleIdx);

  // Day 5: healthy, the policy no longer triggers — the query must not act.
  CHECK(policy_manager.suppressesParticipation(
      person, kLeisure, SlotVenueType::known(kPubVenueType), 5.0));
  CHECK(policy_manager.getFrozenStates().count(person.id) == 1);
  CHECK(person.schedule_hop.hopped_schedule_id == kFreezeScheduleIdx);
}

TEST_CASE("Policy suppression does not latch a compliance decision") {
  // Compliance is sticky: the first caller to ask fixes the answer for the
  // rest of the person's time under the policy. That first caller must be
  // ActivityManager, not a speculative probe — so the query redraws instead of
  // latching, and must reach the same answer the latch would.
  constexpr int kNumPeople = 16;
  WorldState world = buildVenueGateWorld(kNumPeople);
  Disease disease = buildTwoStageDisease();
  PolicyManager policy_manager(world);
  policy_manager.addSymptomPolicy(makeFreezePolicy({}, 0.5));
  policy_manager.resolveAll(disease);

  int suppressed_count = 0;
  for (int i = 0; i < kNumPeople; ++i) {
    Person& person = makeSickPerson(world, disease, static_cast<size_t>(i));

    const bool suppressed = policy_manager.suppressesParticipation(
        person, kLeisure, SlotVenueType::known(kPubVenueType), 1.0);
    suppressed_count += suppressed ? 1 : 0;

    // Nothing decided, so ActivityManager still gets to make the decision.
    CHECK(person.symptom_policy_decisions == 0);
    CHECK(person.active_symptom_policy_participation == 0);

    // ...and when it does, it lands where the query said it would.
    const bool overridden =
        policy_manager
            .getOverride(person, kLeisure, kPubVenue, 0,
                         SlotVenueType::known(kPubVenueType), 1.0, 0)
            .has_value();
    CHECK(overridden == suppressed);
    CHECK(person.symptom_policy_decisions == 1);
  }

  // Test has power only if both outcomes occur.
  CHECK(suppressed_count > 0);
  CHECK(suppressed_count < kNumPeople);
}

TEST_CASE("A frozen person is suppressed whatever venue type is asked about") {
  // A frozen person is pinned somewhere unrelated to the venue in the
  // question, so the gate has nothing to say: the encounter's venue type or
  // the host's is not where they are. Answering the gate instead would let a
  // frozen traveller be counted into a pub encounter because the freeze policy
  // only named groceries.
  constexpr uint8_t kGroceryVenueType = 2;

  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease();
  PolicyManager policy_manager(world);
  policy_manager.addSymptomPolicy(makeFreezePolicy({"pub"}));
  policy_manager.resolveAll(disease);

  Person& person = makeSickHoppedPerson(world, disease);
  REQUIRE(policy_manager
              .getOverride(person, kLeisure, kPubVenue, 0,
                           SlotVenueType::known(kPubVenueType), 1.0, 0)
              .has_value());
  REQUIRE(policy_manager.getFrozenStates().count(person.id) == 1);

  // The gate names pub only, yet every question is answered "suppressed".
  CHECK(policy_manager.suppressesParticipation(
      person, kLeisure, SlotVenueType::known(kGroceryVenueType), 1.0));
  CHECK(policy_manager.suppressesParticipation(person, kLeisure,
                                               SlotVenueType::absent(), 1.0));
  CHECK(policy_manager.suppressesParticipation(
      person, kPrimaryActivity, SlotVenueType::known(kGroceryVenueType), 1.0));
}

TEST_CASE("Query and override agree on the venue gate") {
  // Only the writes were removed; the verdict is the same sentence. The gate
  // ordering and the ADR-0008 throw live in actionApplies, shared by both, and
  // this is what catches them drifting apart.
  WorldState world = buildVenueGateWorld();
  PolicyManager policy_manager(world);

  SUBCASE("restrict-to") {
    TemporalPolicy close_pubs = makeClosurePolicy({"leisure"}, {"pub"});
    close_pubs.resolve(world);
    policy_manager.addTemporalPolicy(close_pubs);
    Person& person = world.people[0];

    CHECK(querySuppressed(policy_manager, person, kLeisure, kPubVenue));
    CHECK_FALSE(querySuppressed(policy_manager, person, kLeisure,
                                kGroceryVenue));
    CHECK_FALSE(querySuppressed(policy_manager, person, kLeisure,
                                kInvalidVenueId));
    CHECK_FALSE(querySuppressed(policy_manager, person, kPrimaryActivity,
                                kPubVenue));
    CHECK_THROWS_AS(querySuppressed(policy_manager, person, kLeisure, 9999),
                    std::runtime_error);

    // Same verdict as the override, on the same person and slot.
    CHECK(runOverride(policy_manager, person, kLeisure, kPubVenue).has_value());
    CHECK_FALSE(runOverride(policy_manager, person, kLeisure, kGroceryVenue)
                    .has_value());
  }

  SUBCASE("exempt-from") {
    TemporalPolicy close_all_but_pubs = makeClosurePolicy(
        {"leisure"}, {"pub"}, VenueGateDirection::ExemptFrom);
    close_all_but_pubs.resolve(world);
    policy_manager.addTemporalPolicy(close_all_but_pubs);
    Person& person = world.people[0];

    CHECK_FALSE(querySuppressed(policy_manager, person, kLeisure, kPubVenue));
    CHECK(querySuppressed(policy_manager, person, kLeisure, kGroceryVenue));
    // Absent occupies no venue, so it fails the filter in this direction too.
    CHECK_FALSE(querySuppressed(policy_manager, person, kLeisure,
                                kInvalidVenueId));
    CHECK_THROWS_AS(querySuppressed(policy_manager, person, kLeisure, 9999),
                    std::runtime_error);

    CHECK_FALSE(
        runOverride(policy_manager, person, kLeisure, kPubVenue).has_value());
    CHECK(runOverride(policy_manager, person, kLeisure, kGroceryVenue)
              .has_value());
  }

  SUBCASE("ungated: an unresolvable venue never costs a lookup") {
    TemporalPolicy close_leisure = makeClosurePolicy({"leisure"}, {});
    close_leisure.resolve(world);
    policy_manager.addTemporalPolicy(close_leisure);
    Person& person = world.people[0];

    CHECK(querySuppressed(policy_manager, person, kLeisure, 9999));
    CHECK_FALSE(querySuppressed(policy_manager, person, kPrimaryActivity, 9999));
  }
}

// =============================================================================
// ActiveWindow — the shared date range.
//
// Half-open [start, end): end_time is the first moment the policy is NOT in
// force. Mirrors the simulation's own window (simulator.cpp: `day <
// total_days_`), so adjacent windows abut exactly with no overlapping slot.
// =============================================================================

TEST_CASE("ActiveWindow - half-open in both directions") {
  ActiveWindow window{5.0, 10.0};

  CHECK_FALSE(window.contains(4.99));
  CHECK(window.contains(5.0));
  CHECK(window.contains(9.99));
  CHECK_FALSE(window.contains(10.0));
}

TEST_CASE("ActiveWindow - the no-end sentinel never closes") {
  ActiveWindow window{5.0, -1.0};

  CHECK_FALSE(window.contains(4.99));
  CHECK(window.contains(5.0));
  CHECK(window.contains(100000.0));
}

TEST_CASE("ActiveWindow - declaring nothing is in force for the whole run") {
  ActiveWindow window;

  CHECK(window.contains(0.0));
  CHECK(window.contains(100000.0));
}

TEST_CASE("Temporal policy - the end bound is the first slot NOT in force") {
  WorldState world = buildVenueGateWorld();
  PolicyManager policy_manager(world);
  TemporalPolicy close_leisure = makeClosurePolicy({"leisure"}, {});
  close_leisure.window = ActiveWindow{5.0, 10.0};
  close_leisure.resolve(world);
  policy_manager.addTemporalPolicy(close_leisure);
  Person& person = world.people[0];

  auto fires_at = [&](double time) {
    return policy_manager
        .getOverride(person, kLeisure, kPubVenue, 0,
                     SlotVenueType::fromVenue(kPubVenue), time, 0)
        .has_value();
  };

  CHECK_FALSE(fires_at(4.99));
  CHECK(fires_at(5.0));
  CHECK(fires_at(9.99));
  // 10.0 is the first slot of the day the policy is no longer in force, not
  // the last slot of its final day.
  CHECK_FALSE(fires_at(10.0));
  CHECK_FALSE(fires_at(10.33));
}

// =============================================================================
// Symptom policy windows
//
// A symptom policy carries the same ActiveWindow as a temporal one: outside it
// the policy is not in force, however symptomatic the person is.
// =============================================================================

TEST_CASE("Symptom policy - a window bounds it as well as the symptom") {
  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease(1000.0);
  PolicyManager policy_manager(world);
  SymptomPolicy stay_home = makeFreezePolicy();
  stay_home.window = ActiveWindow{5.0, 10.0};
  policy_manager.addSymptomPolicy(stay_home);
  policy_manager.resolveAll(disease);

  Person& person = makeSickPerson(world, disease);

  auto fires_at = [&](double time) {
    return policy_manager
        .getOverride(person, kLeisure, kPubVenue, 0,
                     SlotVenueType::known(kPubVenueType), time, 0)
        .has_value();
  };

  // Sick throughout, so only the window moves the answer.
  CHECK_FALSE(fires_at(4.99));
  CHECK(fires_at(5.0));
  CHECK(fires_at(9.99));
  CHECK_FALSE(fires_at(10.0));
}

TEST_CASE("Symptom policy - the window closing thaws a still-sick person") {
  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease(1000.0);
  PolicyManager policy_manager(world);
  SymptomPolicy stay_home = makeFreezePolicy();
  stay_home.window = ActiveWindow{0.0, 10.0};
  policy_manager.addSymptomPolicy(stay_home);
  policy_manager.resolveAll(disease);

  Person& person = makeSickHoppedPerson(world, disease);

  // In window: pinned, and the compliance decision latched.
  REQUIRE(policy_manager
              .getOverride(person, kLeisure, kPubVenue, 0,
                           SlotVenueType::known(kPubVenueType), 1.0, 0)
              .has_value());
  REQUIRE(policy_manager.getFrozenStates().count(person.id) == 1);
  REQUIRE(person.schedule_hop.hopped_schedule_id == kFreezeScheduleIdx);
  REQUIRE(person.symptom_policy_decisions == 1);

  // Out of window, and still just as sick.
  CHECK_FALSE(policy_manager
                  .getOverride(person, kLeisure, kPubVenue, 0,
                               SlotVenueType::known(kPubVenueType), 10.0, 0)
                  .has_value());
  CHECK(person.infection->getTrajectory().getCurrentSymptomId(10.0) == 1);

  // The person is back on their own travel schedule, holding no decision.
  CHECK(policy_manager.getFrozenStates().empty());
  CHECK(person.schedule_hop.hopped_schedule_id == kHoppedSchedule);
  CHECK(person.schedule_hop.return_schedule_id == kReturnSchedule);
  CHECK(person.symptom_policy_decisions == 0);
  CHECK(person.active_symptom_policy_participation == 0);
}

TEST_CASE("Symptom policy - a closing window is not symptom progression") {
  // inherit_compliance models the person carrying a decision from one stage of
  // their illness into the next. A window edge is a change of instruction, not
  // of the person, so the follow-up policy inherits nothing.
  //
  // The follow-up is registered first so that it is evaluated before the
  // policy that hands to it, and its own compliance rate is zero: the only
  // thing that can leave it participating is the inheritance.
  constexpr uint32_t kAftercareBit = 1u << 0;

  auto addPolicies = [](WorldState& world, Disease& disease,
                        PolicyManager& policy_manager) -> Person* {
    SymptomPolicy aftercare = makeFreezePolicy({}, 0.0);
    aftercare.name = "aftercare";
    aftercare.trigger_symptoms = {"healthy"};
    policy_manager.addSymptomPolicy(aftercare);

    SymptomPolicy stay_home = makeFreezePolicy();
    stay_home.name = "stay_home_when_sick";
    stay_home.follow_up_policy_name = "aftercare";
    stay_home.inherit_compliance = true;
    stay_home.window = ActiveWindow{0.0, 10.0};
    policy_manager.addSymptomPolicy(stay_home);

    policy_manager.resolveAll(disease);
    Person& person = makeSickPerson(world, disease);
    person.applicable_symptom_policy_mask = 0b11;
    return &person;
  };

  SUBCASE("the symptom lifting hands the decision on") {
    WorldState world = buildVenueGateWorld();
    Disease disease = buildTwoStageDisease(2.0);
    PolicyManager policy_manager(world);
    Person& person = *addPolicies(world, disease, policy_manager);

    REQUIRE(policy_manager
                .getOverride(person, kLeisure, kPubVenue, 0,
                             SlotVenueType::known(kPubVenueType), 1.0, 0)
                .has_value());

    // Day 3: recovered, so the policy is untriggered but still in force.
    policy_manager.getOverride(person, kLeisure, kPubVenue, 0,
                               SlotVenueType::known(kPubVenueType), 3.0, 0);

    CHECK((person.active_symptom_policy_participation & kAftercareBit) != 0);
  }

  SUBCASE("the window closing does not") {
    WorldState world = buildVenueGateWorld();
    Disease disease = buildTwoStageDisease(1000.0);
    PolicyManager policy_manager(world);
    Person& person = *addPolicies(world, disease, policy_manager);

    REQUIRE(policy_manager
                .getOverride(person, kLeisure, kPubVenue, 0,
                             SlotVenueType::known(kPubVenueType), 1.0, 0)
                .has_value());

    // Day 10: out of window, and just as sick as on day 1.
    policy_manager.getOverride(person, kLeisure, kPubVenue, 0,
                               SlotVenueType::known(kPubVenueType), 10.0, 0);

    CHECK((person.active_symptom_policy_participation & kAftercareBit) == 0);
  }
}

TEST_CASE("Query and override agree on the symptom policy window") {
  // The query answers the same sentence as the override — and, as ever, still
  // writes nothing, so it cannot thaw a person on its own.
  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease(1000.0);
  PolicyManager policy_manager(world);
  SymptomPolicy stay_home = makeFreezePolicy();
  stay_home.window = ActiveWindow{5.0, 10.0};
  policy_manager.addSymptomPolicy(stay_home);
  policy_manager.resolveAll(disease);

  Person& person = makeSickHoppedPerson(world, disease);

  for (double time : {4.99, 5.0, 9.99, 10.0}) {
    const bool suppressed = policy_manager.suppressesParticipation(
        person, kLeisure, SlotVenueType::known(kPubVenueType), time);
    CHECK(suppressed == policy_manager.getSymptomPolicies()[0].window.contains(
                            time));
  }

  // Nothing was decided or frozen by the questions alone.
  CHECK(person.symptom_policy_decisions == 0);
  CHECK(person.active_symptom_policy_participation == 0);
  CHECK(policy_manager.getFrozenStates().empty());
  CHECK(person.schedule_hop.hopped_schedule_id == kHoppedSchedule);
}

// =============================================================================
// Window loading — the same four keys, the same precedence, for every policy
// kind that carries an ActiveWindow.
// =============================================================================

namespace {

// Writes a one-policy policies.yaml and returns the loaded window.
ActiveWindow loadTemporalWindow(const std::string& date_keys,
                                const std::string& simulation_start_date) {
  const std::string path = "/tmp/june_test_policy_window.yaml";
  {
    std::ofstream out(path);
    out << "policies:\n"
           "  temporal_policies:\n"
           "    - name: \"lockdown\"\n"
           "      override_activities: [\"leisure\"]\n"
           "      replacement: \"residence\"\n"
        << date_keys;
  }

  WorldState world = buildVenueGateWorld();
  PolicyManager policy_manager(world);
  PolicyLoader::loadPolicies(policy_manager, path, simulation_start_date);
  std::remove(path.c_str());

  REQUIRE(policy_manager.getTemporalPolicyCount() == 1);
  return policy_manager.getTemporalPolicies()[0].window;
}

// Writes a one-policy policies.yaml and returns the loaded symptom window.
ActiveWindow loadSymptomWindow(const std::string& date_keys,
                               const std::string& simulation_start_date) {
  const std::string path = "/tmp/june_test_symptom_window.yaml";
  {
    std::ofstream out(path);
    out << "policies:\n"
           "  symptom_policies:\n"
           "    - name: \"stay_home_when_sick\"\n"
           "      symptoms: [\"sick\"]\n"
           "      override_activities: [\"leisure\"]\n"
           "      replacement: \"residence\"\n"
        << date_keys;
  }

  WorldState world = buildVenueGateWorld();
  PolicyManager policy_manager(world);
  PolicyLoader::loadPolicies(policy_manager, path, simulation_start_date);
  std::remove(path.c_str());

  REQUIRE(policy_manager.getSymptomPolicyCount() == 1);
  return policy_manager.getSymptomPolicies()[0].window;
}

}  // namespace

TEST_CASE("Window loading - a symptom policy reads the same four keys") {
  SUBCASE("dates convert against the simulation start date") {
    ActiveWindow window = loadSymptomWindow(
        "      start_date: \"2020-03-12\"\n"
        "      end_date: \"2020-05-11\"\n",
        "2020-02-01");

    CHECK(window.start_time == doctest::Approx(40.0));
    CHECK(window.end_time == doctest::Approx(100.0));
  }

  SUBCASE("declaring neither is in force for the whole run") {
    ActiveWindow window = loadSymptomWindow("", "2020-02-01");

    CHECK(window.start_time == doctest::Approx(0.0));
    CHECK(window.end_time == doctest::Approx(-1.0));
  }
}

TEST_CASE("Window loading - dates convert against the simulation start date") {
  ActiveWindow window = loadTemporalWindow(
      "      start_date: \"2020-03-12\"\n"
      "      end_date: \"2020-05-11\"\n",
      "2020-02-01");

  // February 2020 has 29 days, so 12 March is day 40.
  CHECK(window.start_time == doctest::Approx(40.0));
  CHECK(window.end_time == doctest::Approx(100.0));
}

TEST_CASE("Window loading - numeric keys are the fallback, not the winner") {
  SUBCASE("used when the date keys are absent") {
    ActiveWindow window = loadTemporalWindow(
        "      start_time: 7.0\n"
        "      end_time: 21.0\n",
        "2020-02-01");

    CHECK(window.start_time == doctest::Approx(7.0));
    CHECK(window.end_time == doctest::Approx(21.0));
  }

  SUBCASE("ignored when the date keys are present") {
    ActiveWindow window = loadTemporalWindow(
        "      start_date: \"2020-03-12\"\n"
        "      start_time: 7.0\n"
        "      end_date: \"2020-05-11\"\n"
        "      end_time: 21.0\n",
        "2020-02-01");

    CHECK(window.start_time == doctest::Approx(40.0));
    CHECK(window.end_time == doctest::Approx(100.0));
  }

  SUBCASE("declaring neither is in force for the whole run") {
    ActiveWindow window = loadTemporalWindow("", "2020-02-01");

    CHECK(window.start_time == doctest::Approx(0.0));
    CHECK(window.end_time == doctest::Approx(-1.0));
  }
}

TEST_CASE("Adjacent symptom windows never both hold the same slot") {
  // end_date: X and start_date: X abut exactly. Overlap would let a person be
  // counted under two policies at once, which the compliance draw would then
  // compound; the half-open bound is what rules that out.
  const std::string path = "/tmp/june_test_adjacent_windows.yaml";
  {
    std::ofstream out(path);
    out << "policies:\n"
           "  symptom_policies:\n"
           "    - name: \"first_instruction\"\n"
           "      symptoms: [\"sick\"]\n"
           "      override_activities: [\"leisure\"]\n"
           "      replacement: \"residence\"\n"
           "      end_date: \"2020-03-12\"\n"
           "    - name: \"second_instruction\"\n"
           "      symptoms: [\"sick\"]\n"
           "      override_activities: [\"primary_activity\"]\n"
           "      replacement: \"residence\"\n"
           "      start_date: \"2020-03-12\"\n";
  }

  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease(1000.0);
  PolicyManager policy_manager(world);
  PolicyLoader::loadPolicies(policy_manager, path, "2020-02-01");
  std::remove(path.c_str());
  policy_manager.resolveAll(disease);

  Person& person = makeSickPerson(world, disease);
  person.applicable_symptom_policy_mask = 0b11;

  auto fires_at = [&](int16_t activity_index, double time) {
    return policy_manager
        .getOverride(person, activity_index, kPubVenue, 0,
                     SlotVenueType::known(kPubVenueType), time, 0)
        .has_value();
  };

  // 12 March is day 40. Every slot of that day belongs to the second
  // instruction alone.
  for (double time : {40.0, 40.33, 40.67}) {
    CHECK_FALSE(fires_at(kLeisure, time));
    CHECK(fires_at(kPrimaryActivity, time));
  }

  // ...and the last slot of the day before belongs to the first alone.
  CHECK(fires_at(kLeisure, 39.67));
  CHECK_FALSE(fires_at(kPrimaryActivity, 39.67));
}

TEST_CASE("Successive symptom windows tighten to the new rate, not past it") {
  // Two instructions in sequence: comply at 0.4, then at 0.8. Once the second
  // window opens, 80% of the population is held. Had the first policy stayed
  // in force, the two draws would compound to 1 - 0.6 * 0.2 = 88%.
  constexpr int kNumPeople = 500;
  WorldState world = buildVenueGateWorld(kNumPeople);
  Disease disease = buildTwoStageDisease(1000.0);
  PolicyManager policy_manager(world);

  SymptomPolicy first_instruction = makeFreezePolicy({}, 0.4);
  first_instruction.name = "first_instruction";
  first_instruction.window = ActiveWindow{0.0, 10.0};
  policy_manager.addSymptomPolicy(first_instruction);

  SymptomPolicy second_instruction = makeFreezePolicy({}, 0.8);
  second_instruction.name = "second_instruction";
  second_instruction.window = ActiveWindow{10.0, -1.0};
  policy_manager.addSymptomPolicy(second_instruction);

  policy_manager.resolveAll(disease);

  auto countHeld = [&](double time) {
    int held = 0;
    for (int i = 0; i < kNumPeople; ++i) {
      Person& person = world.people[static_cast<size_t>(i)];
      held += policy_manager
                      .getOverride(person, kLeisure, kPubVenue, 0,
                                   SlotVenueType::known(kPubVenueType), time, 0)
                      .has_value()
                  ? 1
                  : 0;
    }
    return held;
  };

  for (int i = 0; i < kNumPeople; ++i) {
    Person& person = makeSickPerson(world, disease, static_cast<size_t>(i));
    person.applicable_symptom_policy_mask = 0b11;
  }

  // Day 5: the first instruction alone, and its decisions are now latched.
  const double first_share = countHeld(5.0) / double(kNumPeople);
  CHECK(first_share > 0.35);
  CHECK(first_share < 0.45);

  // Day 15: the second instruction alone. Well clear of 0.88.
  const double second_share = countHeld(15.0) / double(kNumPeople);
  CHECK(second_share > 0.75);
  CHECK(second_share < 0.85);
}

// =============================================================================
// Freeze release on recovery and death
//
// getOverride releases a freeze from the untriggered branch of the symptom
// loop — a branch only reachable while the person still carries an Infection.
// Recovery and death clear the Infection, so nobody was left to thaw them.
// =============================================================================

TEST_CASE("Recovery releases the freeze") {
  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease();
  PolicyManager policy_manager(world);
  policy_manager.addSymptomPolicy(makeFreezePolicy());
  policy_manager.resolveAll(disease);

  Person& person = makeSickHoppedPerson(world, disease);

  // Day 1: sick and travelling, so the policy pins them in place.
  REQUIRE(policy_manager
              .getOverride(person, kLeisure, kPubVenue, 0,
                           SlotVenueType::known(kPubVenueType), 1.0, 0)
              .has_value());
  REQUIRE(policy_manager.getFrozenStates().count(person.id) == 1);
  REQUIRE(person.schedule_hop.hopped_schedule_id == kFreezeScheduleIdx);

  Epidemiology epidemiology(world, &disease);
  epidemiology.setPolicyManager(&policy_manager);
  epidemiology.trackInfection(person.id);

  // Day 3: recovered. The Infection is gone, so no later policy evaluation can
  // reach the release branch — recovery itself has to do it.
  epidemiology.updateInfectionStates(3.0, {});
  REQUIRE(person.infection == nullptr);

  CHECK(policy_manager.getFrozenStates().empty());
  CHECK(person.schedule_hop.hopped_schedule_id == kHoppedSchedule);
  CHECK(person.schedule_hop.return_schedule_id == kReturnSchedule);
  CHECK_FALSE(policy_manager.suppressesParticipation(
      person, kLeisure, SlotVenueType::known(kPubVenueType), 3.0));
}

TEST_CASE("Death releases the freeze") {
  // A dead person never evaluates a policy again, so a freeze they die under
  // would otherwise be written into every subsequent checkpoint.
  WorldState world = buildVenueGateWorld();
  Disease disease = buildFatalDisease();
  PolicyManager policy_manager(world);
  policy_manager.addSymptomPolicy(makeFreezePolicy());
  policy_manager.resolveAll(disease);

  Person& person = makeSickHoppedPerson(world, disease);

  REQUIRE(policy_manager
              .getOverride(person, kLeisure, kPubVenue, 0,
                           SlotVenueType::known(kPubVenueType), 1.0, 0)
              .has_value());
  REQUIRE(policy_manager.getFrozenStates().count(person.id) == 1);

  Epidemiology epidemiology(world, &disease);
  epidemiology.setPolicyManager(&policy_manager);
  epidemiology.trackInfection(person.id);

  epidemiology.updateInfectionStates(3.0, {});
  REQUIRE(person.is_dead);

  CHECK(policy_manager.getFrozenStates().empty());
}

// =============================================================================
// Policy count cap. The applicability masks are uint32_t, so a 33rd policy can
// never fire — it must fail loud at resolve time rather than load silently.
// =============================================================================

TEST_CASE("A 33rd symptom policy is rejected by name") {
  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease();
  PolicyManager policy_manager(world);

  for (int i = 0; i < 33; ++i) {
    SymptomPolicy policy = makeFreezePolicy();
    policy.name = "symptom_policy_" + std::to_string(i);
    policy_manager.addSymptomPolicy(policy);
  }

  std::string message;
  try {
    policy_manager.resolveAll(disease);
    FAIL("resolveAll accepted 33 symptom policies");
  } catch (const std::runtime_error& error) {
    message = error.what();
  }
  CHECK(message.find("symptom_policy_32") != std::string::npos);
}

TEST_CASE("A 33rd temporal policy is rejected by name") {
  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease();
  PolicyManager policy_manager(world);

  for (int i = 0; i < 33; ++i) {
    TemporalPolicy policy = makeClosurePolicy({"leisure"}, {"pub"});
    policy.name = "temporal_policy_" + std::to_string(i);
    policy_manager.addTemporalPolicy(policy);
  }

  std::string message;
  try {
    policy_manager.resolveAll(disease);
    FAIL("resolveAll accepted 33 temporal policies");
  } catch (const std::runtime_error& error) {
    message = error.what();
  }
  CHECK(message.find("temporal_policy_32") != std::string::npos);
}

TEST_CASE("32 policies of each kind resolve and all can fire") {
  WorldState world = buildVenueGateWorld();
  Disease disease = buildTwoStageDisease();
  PolicyManager policy_manager(world);

  for (int i = 0; i < 32; ++i) {
    SymptomPolicy symptom_policy = makeFreezePolicy();
    symptom_policy.name = "symptom_policy_" + std::to_string(i);
    policy_manager.addSymptomPolicy(symptom_policy);

    TemporalPolicy temporal_policy = makeClosurePolicy({"leisure"}, {"pub"});
    temporal_policy.name = "temporal_policy_" + std::to_string(i);
    policy_manager.addTemporalPolicy(temporal_policy);
  }

  CHECK_NOTHROW(policy_manager.resolveAll(disease));

  // The last of the 32 still reaches the top bit of the mask.
  policy_manager.precomputePolicyApplicability(world.people);
  CHECK(world.people[0].applicable_symptom_policy_mask == 0xFFFFFFFFu);
  CHECK(world.people[0].applicable_temporal_policy_mask == 0xFFFFFFFFu);
}
