#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <iostream>
#include <sstream>
#include <string>

#include "core/world_state.h"
#include "doctest.h"
#include "loaders/domain_loader_internals.h"

using namespace june;

// The seam behind buildGlobalVenueMaps(). A rank must be able to name the type
// of every Venue in the world, not just the ones decomposed onto it — otherwise
// kUnknownVenueTypeId means both "no such Venue" and "not mine", and
// venue-gated policy becomes rank-dependent.
TEST_CASE("fillGlobalVenueMaps types venues this rank does not own") {
  WorldState world;
  world.venue_type_names = {"household", "school"};

  GeographicalUnit geo_unit;
  geo_unit.id = 10;
  geo_unit.parent_id = -1;
  geo_unit.level_id = 0;
  world.geo_units.push_back(geo_unit);

  // One local venue; venue 200 belongs to another rank. No activity_venues, so
  // no local person can reach 200 — it is outside any halo.
  Venue local_venue;
  local_venue.id = 100;
  local_venue.type_id = 0;
  local_venue.geo_unit_id = 10;
  world.venues.push_back(local_venue);
  world.buildIndices();

  detail::fillGlobalVenueMaps(world, {100, 200}, {0, 1}, {10, 10});

  SUBCASE("both venues land in the type index") {
    REQUIRE(world.venue_type_by_id.size() == 201);
    CHECK(world.venue_type_by_id[100] == 0);
    CHECK(world.venue_type_by_id[200] == 1);
  }

  SUBCASE("a foreign venue types via getVenueTypeId") {
    CHECK(world.getVenueTypeId(200) == 1);
  }

  SUBCASE("a hole naming no Venue is unresolvable") {
    CHECK(world.getVenueTypeId(150) == kUnknownVenueTypeId);
  }

  SUBCASE("an id past the end is unresolvable") {
    CHECK(world.getVenueTypeId(300) == kUnknownVenueTypeId);
  }

  SUBCASE("a negative id is unresolvable") {
    CHECK(world.getVenueTypeId(makeVirtualVenueId(100)) == kUnknownVenueTypeId);
  }
}

// fillGlobalVenueMaps is public API, callable without an HDF5Loader, and reads
// the type/geo arrays off venue_ids.size(). A short array would read out of
// bounds, so the length agreement is checked, not assumed.
TEST_CASE("fillGlobalVenueMaps rejects mismatched input lengths") {
  WorldState world;
  world.venue_type_names = {"household", "school"};

  CHECK_THROWS(detail::fillGlobalVenueMaps(world, {100, 200}, {0}, {10, 10}));
  CHECK_THROWS(detail::fillGlobalVenueMaps(world, {100, 200}, {0, 1}, {10}));
}

// Sparse /venues/ids costs one byte per hole. That is a warning, never a
// throw: the load must still succeed and every id must still type correctly.
TEST_CASE("fillGlobalVenueMaps warns on sparse ids but still loads") {
  WorldState world;
  world.venue_type_names = {"household", "school"};

  std::ostringstream captured;
  std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
  detail::fillGlobalVenueMaps(world, {100, 200}, {0, 1}, {10, 10});
  std::cerr.rdbuf(previous);

  CHECK(captured.str().find("sparse") != std::string::npos);
  CHECK(world.getVenueTypeId(100) == 0);
  CHECK(world.getVenueTypeId(200) == 1);
}
