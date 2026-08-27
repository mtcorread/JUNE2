#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#ifdef USE_MPI
#include <mpi.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/world_state.h"
#include "parallel/domain.h"
#include "parallel/domain_manager.h"
#include "parallel/geography_partitioner.h"

using namespace june;

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);

  doctest::Context context;
  context.applyCommandLine(argc, argv);
  int res = context.run();

  MPI_Finalize();
  return res;
}

struct AdvancedMPIFixture {
  WorldState world;
  Config config;
  int rank;
  int num_ranks;

  AdvancedMPIFixture() {
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

    config.parallel.partition_level = "MGU";
    config.parallel.metis_imbalance_tolerance = 0.1;
  }

  void setupMockGeographyFiles() {
    config.parallel.centroids_file = "test_centroids.csv";
    config.parallel.adjacency_file = "test_adjacency.yaml";

    if (rank == 0) {
      std::ofstream cfile(config.parallel.centroids_file);
      cfile << "name,x,y\n";
      cfile << "MGU1,0.0,0.0\n";
      cfile << "MGU2,1.0,1.0\n";
      cfile.close();

      std::ofstream afile(config.parallel.adjacency_file);
      afile << "MGU1: [MGU2]\n";
      afile << "MGU2: [MGU1]\n";
      afile.close();
    }
    MPI_Barrier(MPI_COMM_WORLD);
  }

  void cleanupMockFiles() {
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) {
      std::remove(config.parallel.centroids_file.c_str());
      std::remove(config.parallel.adjacency_file.c_str());
    }
  }
};

TEST_CASE_FIXTURE(AdvancedMPIFixture,
                  "GeographyPartitioner: metadata loading and hierarchical "
                  "parent resolution") {
  setupMockGeographyFiles();
  world.geo_level_names = {"SGU", "MGU", "LGU"};

  GeographicalUnit mgu1;
  mgu1.id = 0;
  mgu1.name = "MGU1";
  mgu1.level_id = 1;
  mgu1.parent_id = -1;
  world.geo_units.push_back(std::move(mgu1));

  GeographicalUnit sgu1;
  sgu1.id = 1;
  sgu1.name = "SGU1";
  sgu1.level_id = 0;
  sgu1.parent_id = 0;
  world.geo_units.push_back(std::move(sgu1));

  world.buildIndices();
  GeographyPartitioner partitioner(world, config);
  partitioner.loadGeographyMetadata();

  const auto& geo_data = partitioner.getGeoData();
  CHECK(geo_data.count("MGU1") == 1);
  CHECK(geo_data.at("MGU1").id == 0);

  GeoUnitId parent_id = partitioner.findParentAtLevel(1, "MGU");
  CHECK(parent_id == 0);

  cleanupMockFiles();
}

TEST_CASE_FIXTURE(
    AdvancedMPIFixture,
    "DomainManager: Virtual Venue ownership (Coordinated Encounters)") {
  DomainManager manager(world, config);
  manager.setMPI(rank, num_ranks);

  VenueId vv0 = -1005;
  VenueId vv1 = -10000000 - 1005;

  // Virtual venue ownership is now determined by setVenueRank (dynamic
  // registry), not by encoding rank in the ID.
  manager.setVenueRank(vv0, 0);
  CHECK(manager.getVenueRank(vv0) == 0);
  if (num_ranks > 1) {
    manager.setVenueRank(vv1, 1);
    CHECK(manager.getVenueRank(vv1) == 1);
  }
}

TEST_CASE_FIXTURE(AdvancedMPIFixture,
                  "DomainManager: Registry Sync and Empty Rank robustness") {
  DomainManager manager(world, config);
  manager.setMPI(rank, num_ranks);

  // Explicitly set max_person_id for the test
  manager.setMaxPersonId(num_ranks);

  Person p;
  p.id = rank;
  p.schedule_type_id = (uint16_t)(rank + 10);

  // Mock ownership manually since we are skipping full initialization
  manager.getDomain().resident_set.insert(p.id);

  world.people.push_back(std::move(p));

  manager.setPersonRank(rank, rank);
  manager.exchangeScheduleTypes();

  for (int r = 0; r < num_ranks; ++r) {
    CHECK(manager.getGlobalScheduleType(r) == (uint16_t)(r + 10));
  }
}

TEST_CASE_FIXTURE(
    AdvancedMPIFixture,
    "DomainManager: Ownership of venues above partition level (Bug Spotting)") {
  world.geo_level_names = {"SGU", "MGU", "LGU"};

  GeographicalUnit lgu;
  lgu.id = 10;
  lgu.name = "LGU1";
  lgu.level_id = 2;
  lgu.parent_id = -1;
  world.geo_units.push_back(std::move(lgu));

  GeographicalUnit mgu;
  mgu.id = 0;
  mgu.name = "MGU1";
  mgu.level_id = 1;
  mgu.parent_id = 10;
  world.geo_units.push_back(std::move(mgu));

  Venue v;
  v.id = 500;
  v.geo_unit_id = 10;  // In LGU1
  world.venues.push_back(v);

  world.buildIndices();

  DomainManager manager(world, config);
  manager.setMPI(rank, num_ranks);
  manager.setGeoUnitRank(0, 0);  // Rank 0 owns MGU0

  int owning_rank = manager.getVenueRank(500);

  if (rank == 0) {
    std::cout << "[BUG_CHECK] Venue 500 in LGU1 is owned by rank: "
              << owning_rank << " (expected 0)" << std::endl;
  }

  CHECK(owning_rank == 0);
}

// =============================================================================
// Phase 3 Integration Tests: Cross-Rank Relationship & Encounter Exchanges
// =============================================================================

// Fixture with 10 people per rank in the same LGU, suitable for testing
// cross-rank relationship formation and encounter proposal exchange.
struct CrossRankIntegrationFixture {
  static constexpr int PEOPLE_PER_RANK = 10;
  static constexpr int TOTAL_PEOPLE = 20;  // 2 ranks * 10

  int rank, num_ranks;
  WorldState world;
  Config config;
  std::unique_ptr<DomainManager> dm;

  CrossRankIntegrationFixture() {
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

    // --- Registries ---
    world.venue_type_names = {"household"};
    world.geo_level_names = {"LGU"};
    world.activity_names = {"residence", "work", "leisure",
                            "sex",       "none", "dead"};
    world.encounter_type_names = {};
    world.subset_type_names = {};
    world.person_property_names = {"age", "sex", "sexual_orientation"};
    world.person_property_value_registries["sexual_orientation"] = {
        "heterosexual"};

    // Both ranks share the same LGU (overlapping geography)
    GeographicalUnit lgu;
    lgu.id = 0;
    lgu.name = "SharedLGU";
    lgu.level_id = 0;
    lgu.parent_id = -1;
    world.geo_units.push_back(lgu);

    // Each rank creates its own local people and venues
    int base_id = rank * PEOPLE_PER_RANK;
    for (int i = 0; i < PEOPLE_PER_RANK; ++i) {
      PersonId pid = base_id + i;

      // Create a residence venue for each person
      Venue v;
      v.id = pid;  // venue id = person id for simplicity
      v.type_id = 0;
      v.geo_unit_id = 0;
      v.is_residence = true;
      world.venues.push_back(v);

      // Create the person
      Person& p = world.people.emplace_back();
      p.id = pid;
      p.age = 25.0f + static_cast<float>(i);
      p.sex = (i % 2 == 0) ? Sex::MALE : Sex::FEMALE;
      p.geo_unit_id = 0;

      // Person properties: age, sex, sexual_orientation
      p.properties_start =
          static_cast<uint32_t>(world.person_properties.size());
      p.properties_count = 3;
      world.person_properties.push_back(static_cast<int32_t>(p.age));
      world.person_properties.push_back(p.sex == Sex::MALE ? 0 : 1);
      world.person_properties.push_back(0);  // heterosexual
    }

    world.buildIndices();

    // --- Config ---
    config.parallel.partition_level = "LGU";
    config.parallel.geo_unit_chunk_size = 1000;

    // --- DomainManager (test mode) ---
    dm = std::make_unique<DomainManager>(world, config);
    dm->setMPI(rank, num_ranks);
    dm->setMaxPersonId(TOTAL_PEOPLE - 1);

    // Both ranks share the same LGU
    dm->setGeoUnitRank(0, 0);

    // Register all people and venues globally
    for (int r = 0; r < num_ranks; ++r) {
      int base = r * PEOPLE_PER_RANK;
      for (int i = 0; i < PEOPLE_PER_RANK; ++i) {
        PersonId pid = base + i;
        dm->setPersonRank(pid, r);
        dm->setVenueRank(pid, r);  // venue id = person id
      }
    }

    // Populate local domain ownership
    Domain& domain = dm->getDomain();
    domain.addGeoUnit(0);
    for (int i = 0; i < PEOPLE_PER_RANK; ++i) {
      PersonId pid = base_id + i;
      domain.resident_ids.push_back(pid);
      domain.resident_set.insert(pid);
      domain.local_venue_ids.push_back(pid);
      domain.local_venue_set.insert(pid);
    }
  }
};

TEST_CASE_FIXTURE(
    CrossRankIntegrationFixture,
    "Integration: Cross-rank encounter proposal and reply exchange") {
  REQUIRE(num_ranks == 2);

  // Rank 0 sends a proposal to person 10 (on rank 1)
  // Rank 1 sends a proposal to person 5 (on rank 0)
  std::vector<EncounterProposal> local_proposals;

  if (rank == 0) {
    EncounterProposal prop;
    prop.encounter_id = 100;
    prop.host_id = 0;
    prop.host_rank = 0;
    prop.invitee_id = 10;  // on rank 1
    prop.venue_id = -1000;
    prop.venue_owner_rank = 0;
    prop.venue_type_id = 0;
    prop.slot = 2;
    prop.encounter_type_id = 0;
    local_proposals.push_back(prop);
  } else {
    EncounterProposal prop;
    prop.encounter_id = 200;
    prop.host_id = 11;
    prop.host_rank = 1;
    prop.invitee_id = 5;  // on rank 0
    prop.venue_id = -2000;
    prop.venue_owner_rank = 1;
    prop.venue_type_id = 0;
    prop.slot = 3;
    prop.encounter_type_id = 0;
    local_proposals.push_back(prop);
  }

  std::vector<EncounterProposal> received_proposals;
  dm->exchangeEncounterProposals(local_proposals, received_proposals);

  // Each rank should receive the proposal targeting its local person
  bool found_cross_rank = false;
  for (const auto& p : received_proposals) {
    if (rank == 0 && p.invitee_id == 5) {
      found_cross_rank = true;
      CHECK(p.host_id == 11);
      CHECK(p.host_rank == 1);
      CHECK(p.encounter_id == 200);
    }
    if (rank == 1 && p.invitee_id == 10) {
      found_cross_rank = true;
      CHECK(p.host_id == 0);
      CHECK(p.host_rank == 0);
      CHECK(p.encounter_id == 100);
    }
  }
  CHECK(found_cross_rank);

  // Now send replies back
  std::vector<EncounterReply> local_replies;
  for (const auto& p : received_proposals) {
    // Accept proposals for our local people
    int base_id = rank * PEOPLE_PER_RANK;
    if (p.invitee_id >= base_id && p.invitee_id < base_id + PEOPLE_PER_RANK) {
      EncounterReply reply;
      reply.encounter_id = p.encounter_id;
      reply.host_id = p.host_id;
      reply.invitee_id = p.invitee_id;
      reply.venue_id = p.venue_id;
      reply.venue_type_id = p.venue_type_id;
      reply.slot = p.slot;
      reply.encounter_type_id = p.encounter_type_id;
      reply.status = ReplyStatus::ACCEPTED;
      local_replies.push_back(reply);
    }
  }

  std::vector<EncounterReply> received_replies;
  dm->exchangeEncounterReplies(local_replies, received_replies);

  // Each rank should receive the ACCEPTED reply for its proposal
  bool got_reply = false;
  for (const auto& r : received_replies) {
    if (rank == 0 && r.encounter_id == 100) {
      got_reply = true;
      CHECK(r.status == ReplyStatus::ACCEPTED);
      CHECK(r.invitee_id == 10);
    }
    if (rank == 1 && r.encounter_id == 200) {
      got_reply = true;
      CHECK(r.status == ReplyStatus::ACCEPTED);
      CHECK(r.invitee_id == 5);
    }
  }
  CHECK(got_reply);
}

TEST_CASE_FIXTURE(
    CrossRankIntegrationFixture,
    "Integration: Finalized encounter exchange delivers to remote "
    "participant ranks") {
  REQUIRE(num_ranks == 2);

  // Only rank 0 finalizes an encounter involving person 10 (on rank 1)
  std::vector<CoordinatedEncounter> local_finalized;

  if (rank == 0) {
    CoordinatedEncounter enc;
    enc.encounter_id = 300;
    enc.host_id = 0;
    enc.venue_id = -3000;
    enc.venue_type_id = 0;
    enc.slot = 1;
    enc.encounter_type_id = 0;
    enc.participants.insert(0);   // local to rank 0
    enc.participants.insert(10);  // remote, on rank 1
    local_finalized.push_back(enc);
  }

  std::vector<CoordinatedEncounter> received;
  dm->exchangeFinalizedEncounters(local_finalized, received);

  if (rank == 1) {
    // Rank 1 had no local encounters, but should receive encounter 300
    // because person 10 (local to rank 1) is a participant
    CHECK(local_finalized.empty());
    REQUIRE(received.size() == 1);
    CHECK(received[0].encounter_id == 300);
    CHECK(received[0].participants.count(0) == 1);
    CHECK(received[0].participants.count(10) == 1);
    CHECK(received[0].host_id == 0);
  }

  if (rank == 0) {
    // Rank 0 finalized the encounter locally. The exchange should NOT
    // return it back (line 1077 of domain_communicator.cpp skips own rank).
    // Rank 1 had no finalized encounters, so rank 0 receives nothing.
    CHECK(received.empty());
    // Original local_finalized is untouched
    CHECK(local_finalized.size() == 1);
  }
}

// A world whose people all live in the geo unit that holds their home, plus an
// optional straddler: person 2 sits in geo unit 0 but is housed in venue 1,
// which belongs to geo unit 1.
static WorldState buildResidenceWorld(bool with_straddler) {
  WorldState world;
  world.activity_names = {"residence"};
  world.venue_type_names = {"home"};
  world.geo_level_names = {"MGU"};

  for (GeoUnitId geo_unit_id : {0, 1}) {
    GeographicalUnit unit;
    unit.id = geo_unit_id;
    unit.name = "MGU" + std::to_string(geo_unit_id + 1);
    unit.level_id = 0;
    unit.parent_id = -1;
    world.geo_units.push_back(unit);

    Venue home;
    home.id = geo_unit_id;
    home.type_id = 0;
    home.geo_unit_id = geo_unit_id;
    world.venues.push_back(home);
  }

  // (person geo unit, home venue): two housed where they live, then the
  // straddler.
  std::vector<std::pair<GeoUnitId, VenueId>> residents = {{0, 0}, {0, 0}};
  if (with_straddler) residents.push_back({0, 1});

  for (size_t i = 0; i < residents.size(); ++i) {
    Person& person = world.people.emplace_back();
    person.id = static_cast<PersonId>(i);
    person.geo_unit_id = residents[i].first;
    person.activity_meta_start =
        static_cast<uint32_t>(world.activity_meta.size());
    person.activity_meta_count = 1;
    world.activity_meta.push_back(
        {0, static_cast<uint32_t>(world.activity_venues.size()), 1});
    world.activity_venues.push_back({residents[i].second, 0});
  }

  world.buildIndices();
  return world;
}

TEST_CASE("Domain: a residence venue and its occupants share a geo unit") {
  int rank = 0;
  int num_ranks = 1;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

  // No collectives here: every rank builds the same world and checks it alone,
  // so the result does not depend on the rank count. That is the point of
  // testing geo units rather than ownership (ADR 0012) -- an ownership test
  // would pass vacuously at np=1.
  SUBCASE("a household inside the owned geo unit passes") {
    WorldState world = buildResidenceWorld(false);
    Domain domain(rank, num_ranks, &world);
    domain.addGeoUnit(0);

    CHECK_NOTHROW(domain.assignPeopleAndVenues());
    CHECK(domain.local_population == 2);
  }

  SUBCASE("a resident housed in another geo unit is fatal, and is named") {
    WorldState world = buildResidenceWorld(true);
    Domain domain(rank, num_ranks, &world);
    domain.addGeoUnit(0);

    REQUIRE_THROWS_AS(domain.assignPeopleAndVenues(), std::runtime_error);
    try {
      domain.assignPeopleAndVenues();
    } catch (const std::runtime_error& error) {
      const std::string message = error.what();
      CHECK(message.find("person 2") != std::string::npos);
      CHECK(message.find("ADR 0012") != std::string::npos);
    }
  }
}

#endif
