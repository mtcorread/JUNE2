#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "activity/activity_manager.h"
#include "core/config.h"
#include "doctest.h"
#include "epidemiology/policy.h"
#include "test_utils.h"

using namespace june;

TEST_CASE("ActivityManager - Bug Fixes for Schedule Caching") {
  // 1. Setup minimal world
  WorldState world = TestWorldFactory::createMinimalWorld(1, 1);

  // Add schedule type names to registry
  world.schedule_type_names = {"default_schedule", "worker_schedule"};

  // Setup person with existing schedule_type_id (simulating HDF5 load)
  world.people[0].schedule_type_id = 1;  // "worker_schedule"
  world.people[0].cached_schedule_type_ =
      nullptr;  // Bug condition: loaded from file, pointer is null

  // Setup config
  Config config;
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};

  ScheduleType worker_sched;
  worker_sched.name = "worker_schedule";
  TimeSlot slot1;
  slot1.name = "morning";
  slot1.allowed_activities = {"work"};
  worker_sched.slots_by_day_type["workday"].push_back(slot1);

  config.schedule.schedule_types.push_back(worker_sched);
  config.schedule.default_schedule_type = "worker_schedule";
  config.performance.precompute_schedules = false;
  config.performance.stochastic_activities = {
      "work"};  // Make it stochastic to trigger Bug 2

  // Check that we have valid registry string for the activity
  world.activity_names = {
      "residence", "work", "none",
      "dead"};  // Needed for ActivityManager to find indices
  // Resolve config caches (required: selectActivity now uses pre-resolved
  // indices)
  config.resolve(world);

  ActivityManager manager(world, config);

  SUBCASE("assignScheduleTypes correctly restores cached pointer") {
    manager.assignScheduleTypes();

    // Bug 1 fix: cached_schedule_type_ should now be set
    REQUIRE(world.people[0].cached_schedule_type_ != nullptr);
    CHECK(world.people[0].cached_schedule_type_->name == "worker_schedule");
  }

  SUBCASE("assignActivitiesFromSchedule avoids null pointer crash") {
    // First ensure the schedule type pointer is restored
    manager.assignScheduleTypes();

    // Indicate we want to use the first slot (day_type 0 = workday)
    world.num_day_types = 1;
    world.precomputed_schedules.resize(1);
    world.precomputed_schedules[0].push_back(
        ScheduleEntry(1, -1, -1, false));  // stochastic entry
    world.schedule_starts.assign(world.people.size() * 1, 0);
    world.schedule_counts.assign(world.people.size() * 1, 0);
    world.schedule_starts[0] = 0;  // person 0, day_type 0
    world.schedule_counts[0] = 1;
    world.people[0].schedule_computed = true;  // Act as if computed

    std::vector<PersonLocation> locations;
    locations.resize(world.people.size());

    // This should not crash (Bug 2 fix)
    manager.assignActivitiesFromSchedule(0, 0, locations);

    REQUIRE(locations.size() == 1);
    CHECK(locations[0].person_id == world.people[0].id);
  }
}

TEST_CASE("ActivityManager - Locations Initialization") {
  WorldState world = TestWorldFactory::createMinimalWorld(2, 2);
  world.activity_names = {"residence", "work", "none", "dead"};

  // Setup venues
  world.venues[0].type_id = 0;  // office
  world.venues[1].type_id = 1;  // home
  world.venue_type_names = {"office", "home"};

  // Assign residence to person 0
  world.venues[1].geo_unit_id = 0;

  world.buildIndices();  // rebuilding because we modified venues

  // Manually assign residence activity (index 1 in activity_names usually, but
  // TestWorldFactory doesn't set it up specifically, let's just use the index
  // we gave it: 0=residence) Actually, residence is index 0 in the list above
  world.people[0].activity_meta_start =
      static_cast<uint32_t>(world.activity_meta.size());
  world.people[0].activity_meta_count = 1;
  world.activity_meta.push_back(
      {0, static_cast<uint32_t>(world.activity_venues.size()),
       1});                                 // residence, venue count 1
  world.activity_venues.push_back({1, 0});  // venue 1, subset 0

  // Person 1 has no residence
  world.people[1].activity_meta_start = 0;
  world.people[1].activity_meta_count = 0;

  Config config;
  config.resolve(world);
  ActivityManager manager(world, config);

  SUBCASE("initializeLocations assigns residence or none") {
    std::vector<PersonLocation> locations;
    manager.initializeLocations(locations);

    REQUIRE(locations.size() == 2);

    // Person 0 should be at residence (venue 1)
    CHECK(locations[0].person_id == 0);
    CHECK(locations[0].venue_id == 1);
    CHECK(locations[0].activity_index == 0);  // "residence"

    // Person 1 has no residence, should be "none" (index 2)
    CHECK(locations[1].person_id == 1);
    CHECK(locations[1].venue_id == -1);
    CHECK(locations[1].activity_index == 2);  // "none"
  }

  SUBCASE("initializeLocations handles dead people") {
    world.people[0].is_dead = true;
    std::vector<PersonLocation> locations;
    manager.initializeLocations(locations);

    REQUIRE(locations.size() == 2);

    // Person 0 is dead, should be assigned "dead" activity (index 3) and -1
    // venue
    CHECK(locations[0].person_id == 0);
    CHECK(locations[0].venue_id == -1);
    CHECK(locations[0].activity_index == 3);  // "dead"
  }
}

TEST_CASE("ActivityManager - Precompute Schedules") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 2);
  world.activity_names = {"residence", "work", "leisure", "none", "dead"};

  // Setup venues for work (type 0) and leisure (type 2)
  world.venue_type_names = {"office", "home", "pub"};
  world.venues[0].type_id = 0;  // office
  world.venues[1].type_id = 2;  // pub
  world.buildIndices();

  // Assign work venue to person 0
  world.people[0].activity_meta_start =
      static_cast<uint32_t>(world.activity_meta.size());
  world.people[0].activity_meta_count = 2;
  world.activity_meta.push_back(
      {1, static_cast<uint32_t>(world.activity_venues.size()), 1});  // work
  world.activity_venues.push_back({0, 0});                           // venue 0
  world.activity_meta.push_back(
      {2, static_cast<uint32_t>(world.activity_venues.size()), 1});  // leisure
  world.activity_venues.push_back({1, 0});                           // venue 1

  // Provide an activity preference config so selectVenue doesn't crash on empty
  // weights (We mock out the weights for the venue types)

  Config config;
  config.performance.precompute_schedules = true;
  config.performance.stochastic_activities = {
      "leisure"};                             // Leisure is stochastic
  config.performance.hybrid_activities = {};  // No hybrid
  config.schedule.day_type_cycle = {"workday"};
  config.schedule.day_type_names = {"workday"};

  ScheduleType sched;
  sched.name = "mixed_schedule";

  // Slot 0: deterministic (work) -> venue should be precomputed
  TimeSlot slot0;
  slot0.name = "morning";
  slot0.allowed_activities = {"work"};
  sched.slots_by_day_type["workday"].push_back(slot0);

  // Slot 1: stochastic (leisure) -> venue should be -1
  TimeSlot slot1;
  slot1.name = "evening";
  slot1.allowed_activities = {"leisure"};
  sched.slots_by_day_type["workday"].push_back(slot1);

  config.schedule.schedule_types.push_back(sched);
  config.schedule.default_schedule_type = "mixed_schedule";

  // Assign schedule via direct ID matching default
  world.schedule_type_names = {"mixed_schedule"};
  world.people[0].schedule_type_id = 0;

  // Resolve config caches (required: selectActivity now uses pre-resolved
  // indices)
  config.resolve(world);

  ActivityManager manager(world, config);
  manager
      .assignScheduleTypes();  // Ensure cached pointers are set map correctly

  SUBCASE("Deterministic vs Stochastic precomputation") {
    manager.precomputeSchedules();

    REQUIRE(world.people[0].schedule_computed == true);
    // Person 0, day_type 0 (workday): should have 2 schedule entries
    REQUIRE(world.schedule_counts[0 * world.num_day_types + 0] == 2);
    REQUIRE(world.precomputed_schedules[0].size() == 2);

    // Slot 0: Work (Deterministic). Venue 0 should be saved
    uint32_t start = world.schedule_starts[0 * world.num_day_types + 0];
    const auto& entry0 = world.precomputed_schedules[0][start + 0];
    CHECK(entry0.activity_index == 1);  // "work"
    CHECK(entry0.venue_id == 0);        // precomputed!
    CHECK(entry0.is_deterministic == true);

    // Slot 1: Leisure (Stochastic). Venue should be -1
    const auto& entry1 = world.precomputed_schedules[0][start + 1];
    CHECK(entry1.activity_index == 2);  // "leisure"
    CHECK(entry1.venue_id == -1);       // stochastic, venue selected at runtime
    CHECK(entry1.is_deterministic == false);
  }
}

TEST_CASE("ActivityManager - PolicyManager Override") {
  WorldState world = TestWorldFactory::createMinimalWorld(1, 2);
  world.activity_names = {"residence", "work", "none", "dead"};
  world.venue_type_names = {"home", "office"};
  world.venues[0].type_id = 0;  // home
  world.venues[1].type_id = 1;  // office
  world.buildIndices();

  // Person 0 has residence (venue 0) and work (venue 1)
  world.people[0].activity_meta_start =
      static_cast<uint32_t>(world.activity_meta.size());
  world.people[0].activity_meta_count = 2;
  world.activity_meta.push_back(
      {0, static_cast<uint32_t>(world.activity_venues.size()),
       1});  // residence
  world.activity_venues.push_back({0, 0});
  world.activity_meta.push_back(
      {1, static_cast<uint32_t>(world.activity_venues.size()), 1});  // work
  world.activity_venues.push_back({1, 0});

  Config config;
  ActivityManager manager(world, config);
  PolicyManager pm(world);

  TemporalPolicy lockdown;
  lockdown.name = "Lockdown";
  lockdown.window.start_time = 0.0;
  lockdown.window.end_time = 10.0;
  lockdown.action.override_activities.insert("work");
  lockdown.action.replacement_activity = "residence";
  lockdown.action.compliance_rate = 1.0;

  // Resolve string names to indices
  lockdown.resolve(world);
  pm.addTemporalPolicy(lockdown);

  // Give person applicable mask
  world.people[0].applicable_temporal_policy_mask = 1;

  manager.setPolicyManager(&pm);
  manager.setCurrentTime(5.0);  // Lockdown is active

  SUBCASE("assignActivities respects PolicyManager lockdown override") {
    // We will directly call assignActivities on a slot that allows "work"
    TimeSlot work_slot;
    work_slot.name = "morning_work";
    work_slot.allowed_activities = {"work"};

    // Set up the schedule type for the fallback lookup
    config.schedule.day_type_cycle = {"workday"};
    config.schedule.day_type_names = {"workday"};
    ScheduleType worker_sched;
    worker_sched.name = "worker";
    worker_sched.slots_by_day_type["workday"].push_back(work_slot);
    config.schedule.schedule_types.push_back(worker_sched);
    config.schedule.default_schedule_type = "worker";

    // Resolve config caches (required: selectActivity now uses pre-resolved
    // indices)
    config.resolve(world);

    world.people[0].cached_schedule_type_ = &config.schedule.schedule_types[0];

    std::vector<PersonLocation> locations;
    locations.resize(world.people.size());

    manager.assignActivities(work_slot, 0, locations);

    REQUIRE(locations.size() == 1);
    CHECK(locations[0].person_id == world.people[0].id);

    // Instead of work (venue 1), they should be at residence (venue 0) due to
    // lockdown
    CHECK(locations[0].activity_index == 0);  // "residence"
    CHECK(locations[0].venue_id == 0);
  }
}
