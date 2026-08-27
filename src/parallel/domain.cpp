#ifdef USE_MPI

#include "parallel/domain.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "utils/deterministic_rng.h"
#include "utils/random.h"

namespace june {

Domain::Domain(int rank, int num_ranks, WorldState* world, uint64_t base_seed)
    : rank(rank),
      num_ranks(num_ranks),
      world(world),
      local_population(0),
      local_venues(0) {
  // Deterministic per-rank seed derived from global seed (MPI reproducible)
  rng.seed(static_cast<unsigned int>(mix_seed(base_seed, rank, 0xD0A1AULL)));
}

void Domain::assignPeopleAndVenues() {
  // Clear existing assignments
  resident_ids.clear();
  resident_set.clear();
  local_venue_ids.clear();
  local_venue_set.clear();

  // Assign people: A person belongs to the domain that owns their residence
  // geo_unit
  for (const auto& person : world->people) {
    if (ownsGeoUnit(person.geo_unit_id)) {
      resident_ids.push_back(person.id);
      resident_set.insert(person.id);
    }
  }

  // Assign venues: A venue belongs to the domain that owns its geo_unit
  for (const auto& venue : world->venues) {
    if (ownsGeoUnit(venue.geo_unit_id)) {
      local_venue_ids.push_back(venue.id);
      local_venue_set.insert(venue.id);
    }
  }

  local_population = resident_ids.size();
  local_venues = local_venue_ids.size();

  checkResidencesAreLocal();
}

void Domain::checkResidencesAreLocal() const {
  // Resolved once: the string overload of getActivityVenues scans
  // activity_names per call, which over every resident is a linear search a
  // population deep.
  const int16_t residence_activity_index =
      static_cast<int16_t>(world->getActivityIndex("residence"));
  if (residence_activity_index < 0) return;  // world has no residence activity

  // Geo units, not rank ownership: at np=1 one rank owns everything, so an
  // ownership test passes on every world and a malformed one would first fail
  // on the cluster. Geo unit equality fails identically at any rank count.
  struct Offender {
    PersonId person_id;
    GeoUnitId person_geo_unit;
    VenueId residence_id;
    GeoUnitId residence_geo_unit;
  };
  constexpr size_t kOffendersNamed = 10;
  std::vector<Offender> named;
  size_t offenders = 0;

  for (PersonId person_id : resident_ids) {
    const Person* person = world->getPerson(person_id);
    if (person == nullptr) continue;
    auto residence =
        world->getActivityVenues(*person, residence_activity_index);
    if (residence.empty()) continue;  // no home to be wrong about

    const VenueId residence_id = residence[0].first;
    const Venue* venue = world->getVenue(residence_id);
    // A null venue is an offence in its own right: this rank holds someone
    // whose home it never loaded, which is what a torn household looks like
    // from the side that holds no venue.
    const GeoUnitId residence_geo_unit =
        (venue != nullptr) ? venue->geo_unit_id : -1;
    if (venue != nullptr && residence_geo_unit == person->geo_unit_id) continue;

    ++offenders;
    if (named.size() < kOffendersNamed) {
      named.push_back(
          {person_id, person->geo_unit_id, residence_id, residence_geo_unit});
    }
  }

  if (offenders == 0) return;

  std::ostringstream message;
  // No rank prefix: main's handler already stamps one on what() before this
  // reaches the terminal.
  message << offenders
          << " resident(s) whose residence venue sits in another geo unit. A "
             "household must be held whole by one rank (ADR 0012): the "
             "partition assigns a person by their geo unit and a venue by "
             "its own, so a household split across geo units is torn across "
             "ranks.";
  for (const Offender& offender : named) {
    message << "\n  person " << offender.person_id << " in geo unit "
            << offender.person_geo_unit << " -> residence venue "
            << offender.residence_id << " in geo unit "
            << offender.residence_geo_unit;
  }
  if (offenders > named.size()) {
    message << "\n  ...and " << (offenders - named.size()) << " more";
  }
  throw std::runtime_error(message.str());
}

void Domain::printStatistics() const {
  std::cout << "  Rank " << rank << ": " << geo_unit_ids.size()
            << " geo units, " << local_population << " people, " << local_venues
            << " venues" << std::endl;
}

}  // namespace june

#endif  // USE_MPI
