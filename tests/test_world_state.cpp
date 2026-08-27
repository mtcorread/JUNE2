#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/world_state.h"
#include "doctest.h"
#include "test_utils.h"

using namespace june;

TEST_CASE("WorldState basic functionality") {
  WorldState world = TestWorldFactory::createMinimalWorld(10, 5);

  SUBCASE("Building indices") {
    CHECK(world.people.size() == 10);
    CHECK(world.venues.size() == 5);
    CHECK(world.geo_units.size() == 1);

    // Check lookup
    CHECK(world.getPerson(0) != nullptr);
    CHECK(world.getPerson(9) != nullptr);
    CHECK(world.getPerson(10) == nullptr);

    CHECK(world.getVenue(0) != nullptr);
    CHECK(world.getVenue(4) != nullptr);
  }

  SUBCASE("Geographic lookup") {
    auto people_in_city = world.getPeopleInUnit(0);
    CHECK(people_in_city.size() == 10);

    auto city_by_name = world.getPeopleInUnit("city", "TestCity");
    CHECK(city_by_name.size() == 10);
  }
}

TEST_CASE("WorldState networks") {
  WorldState world = TestWorldFactory::createMinimalWorld(2, 0);
  TestWorldFactory::addNetwork(world, "friendship", {{0, 1}});

  SUBCASE("Network partners") {
    auto p0_partners =
        world.getNetworkPartners(*world.getPerson(0), "friendship");
    REQUIRE(p0_partners.size() == 1);
    CHECK(p0_partners[0] == 1);

    auto p1_partners =
        world.getNetworkPartners(*world.getPerson(1), "friendship");
    REQUIRE(p1_partners.size() == 1);
    CHECK(p1_partners[0] == 0);
  }
}

// =============================================================================
// Critical Test 7: Flat array overflow guards
// =============================================================================

TEST_CASE("WorldState flat array overflow returns empty spans") {
  WorldState world = TestWorldFactory::createMinimalWorld(2, 1);

  SUBCASE("activity_meta_start past end of array") {
    Person& p = world.people[0];
    p.activity_meta_start = 99999;
    p.activity_meta_count = 1;
    auto metas = world.getActivityMetas(p);
    CHECK(metas.empty());
  }

  SUBCASE("activity_meta start+count exceeds array size") {
    // Put one real entry then set count too high
    world.activity_meta.push_back({0, 0, 0});
    Person& p = world.people[0];
    p.activity_meta_start = 0;
    p.activity_meta_count = 100;  // Way past end
    auto metas = world.getActivityMetas(p);
    CHECK(metas.empty());
  }

  SUBCASE("network_meta_start past end") {
    Person& p = world.people[0];
    p.network_meta_start = 99999;
    p.network_meta_count = 1;
    auto metas = world.getNetworkMetas(p);
    CHECK(metas.empty());
  }

  SUBCASE("person_properties_start past end") {
    Person& p = world.people[0];
    p.properties_start = 99999;
    p.properties_count = 1;
    auto props = world.getPersonProperties(p);
    CHECK(props.empty());
  }

  SUBCASE("schedule_start past end returns empty") {
    // With the new day_type-based schedule API, an out-of-range person
    // index should return an empty schedule span.
    Person& p = world.people[0];
    p.schedule_computed = false;
    auto sched = world.getSchedule(p, 0);
    CHECK(sched.empty());
  }

  SUBCASE("venue subset_start past end") {
    Venue& v = world.venues[0];
    v.subset_start = 99999;
    v.subset_count = 1;
    auto subs = world.getSubsets(v);
    CHECK(subs.empty());
  }

  SUBCASE("venue properties_start past end") {
    Venue& v = world.venues[0];
    v.properties_start = 99999;
    v.properties_count = 1;
    auto props = world.getVenueProperties(v);
    CHECK(props.empty());
  }

  SUBCASE("zero count always returns empty span") {
    Person& p = world.people[0];
    p.activity_meta_start = 0;
    p.activity_meta_count = 0;
    CHECK(world.getActivityMetas(p).empty());

    p.network_meta_start = 0;
    p.network_meta_count = 0;
    CHECK(world.getNetworkMetas(p).empty());

    p.properties_start = 0;
    p.properties_count = 0;
    CHECK(world.getPersonProperties(p).empty());
  }
}

TEST_CASE("WorldState sentinel ID lookups") {
  WorldState world = TestWorldFactory::createMinimalWorld(3, 2);

  SUBCASE("getPerson(-1) returns nullptr") {
    CHECK(world.getPerson(-1) == nullptr);
  }

  SUBCASE("getVenue(-1) returns nullptr") {
    CHECK(world.getVenue(-1) == nullptr);
  }

  SUBCASE("getGeoUnit(-1) returns nullptr") {
    CHECK(world.getGeoUnit(-1) == nullptr);
  }

  SUBCASE("getPeopleInUnit(-1) returns empty") {
    auto people = world.getPeopleInUnit(-1);
    CHECK(people.empty());
  }

  SUBCASE("getActivityVenues with negative activity index returns empty") {
    auto venues = world.getActivityVenues(world.people[0], (int16_t)-1);
    CHECK(venues.empty());
  }

  SUBCASE("getNetworkPartners with negative type returns empty") {
    auto partners = world.getNetworkPartners(world.people[0], -1);
    CHECK(partners.empty());
  }

  SUBCASE("duplicate person IDs: second overwrites first in index") {
    WorldState w;
    w.venue_type_names = {"office"};
    w.geo_level_names = {"city"};
    auto& p1 = w.people.emplace_back();
    p1.id = 42;
    p1.age = 20.0f;
    auto& p2 = w.people.emplace_back();
    p2.id = 42;  // Duplicate
    p2.age = 30.0f;
    w.buildIndices();
    // The second person (index 1) wins
    const Person* found = w.getPerson(42);
    REQUIRE(found != nullptr);
    CHECK(found->age == doctest::Approx(30.0f));
  }
}

// =============================================================================
// getVenuesInGeoUnit — exact match and descendant traversal
// =============================================================================

namespace {

static WorldState buildGeoHierarchyWorld() {
  WorldState world;
  world.venue_type_names = {"fair", "household"};

  GeographicalUnit root; root.id = 0; root.parent_id = -1; root.level_id = 0;
  world.geo_units.push_back(root);
  GeographicalUnit child; child.id = 1; child.parent_id = 0; child.level_id = 1;
  world.geo_units.push_back(child);
  GeographicalUnit grandchild;
  grandchild.id = 2; grandchild.parent_id = 1; grandchild.level_id = 2;
  world.geo_units.push_back(grandchild);

  Venue va; va.id = 10; va.type_id = 0; va.geo_unit_id = 1;
  Venue vb; vb.id = 11; vb.type_id = 0; vb.geo_unit_id = 2;
  Venue vc; vc.id = 12; vc.type_id = 1; vc.geo_unit_id = 1;
  world.venues.push_back(va);
  world.venues.push_back(vb);
  world.venues.push_back(vc);

  world.buildIndices();
  return world;
}

}  // namespace

TEST_CASE("getVenuesInGeoUnit returns venues in exact unit") {
  WorldState world = buildGeoHierarchyWorld();
  auto venues = world.getVenuesInGeoUnit(2, "fair");
  REQUIRE(venues.size() == 1);
  CHECK(venues[0] == 11);
}

TEST_CASE("getVenuesInGeoUnit includes descendants") {
  WorldState world = buildGeoHierarchyWorld();
  auto venues = world.getVenuesInGeoUnit(1, "fair");
  REQUIRE(venues.size() == 2);
  CHECK(venues[0] == 10);
  CHECK(venues[1] == 11);
}

TEST_CASE("getVenuesInGeoUnit from root covers all descendants") {
  WorldState world = buildGeoHierarchyWorld();
  auto fair_venues = world.getVenuesInGeoUnit(0, "fair");
  REQUIRE(fair_venues.size() == 2);
  auto hh_venues = world.getVenuesInGeoUnit(0, "household");
  REQUIRE(hh_venues.size() == 1);
  CHECK(hh_venues[0] == 12);
}

TEST_CASE("getVenuesInGeoUnit returns empty for unknown type") {
  WorldState world = buildGeoHierarchyWorld();
  auto venues = world.getVenuesInGeoUnit(0, "nonexistent");
  CHECK(venues.empty());
}

// Verify that getVenuesInGeoUnit returns non-local venues pre-loaded into the
// global maps — the MPI-mode path where world.venues only holds local venues
// but global_venues_by_type_name covers all ranks.
TEST_CASE("getVenuesInGeoUnit includes non-local venues from global maps") {
  WorldState world;
  world.geo_level_names = {"sgu"};
  world.venue_type_names = {"guest_house"};

  GeographicalUnit gu;
  gu.id = 10; gu.parent_id = -1; gu.level_id = 0;
  world.geo_units.push_back(gu);

  // Only one local venue (simulating MPI rank's local partition)
  Venue local_v; local_v.id = 100; local_v.type_id = 0; local_v.geo_unit_id = 10;
  world.venues.push_back(local_v);

  // Pre-populate global maps with both the local venue AND a remote one (id=200),
  // as the HDF5 loader would in MPI mode before buildIndices() is called.
  world.global_venues_by_type_name["guest_house"] = {100, 200};
  world.global_venue_geo_unit_map[100] = 10;
  world.global_venue_geo_unit_map[200] = 10;  // remote venue, same geo_unit
  world.setGlobalVenueType(100, 0);
  world.setGlobalVenueType(200, 0);

  world.buildIndices();  // must not overwrite the pre-populated global maps

  auto result = world.getVenuesInGeoUnit(10, "guest_house");
  REQUIRE(result.size() == 2);
  CHECK(result[0] == 100);
  CHECK(result[1] == 200);
}
