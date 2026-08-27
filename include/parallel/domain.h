#pragma once

#ifdef USE_MPI

#include <mpi.h>

#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/types.h"
#include "core/world_state.h"

namespace june {

/**
 * Domain - Represents a geographic partition of the world for parallel
 * execution
 *
 * Each MPI rank owns one Domain, which contains:
 * - A subset of geographic units (e.g., MGUs)
 * - All people who RESIDE in those geo units
 * - All venues LOCATED in those geo units
 * - Tracking of visitors from other domains during simulation
 */
class Domain {
 public:
  // MPI identifiers
  int rank;       // This process's MPI rank
  int num_ranks;  // Total number of MPI processes

  // Geographic coverage
  std::vector<GeoUnitId> geo_unit_ids;  // Geo units in this domain (e.g., MGUs)
  std::unordered_set<GeoUnitId> geo_unit_set;

  // Population partitioning
  std::vector<PersonId> resident_ids;  // People who live in this domain
  std::unordered_set<PersonId> resident_set;

  // Venue partitioning
  std::vector<VenueId> local_venue_ids;  // Venues in this domain
  std::unordered_set<VenueId> local_venue_set;

  // Cross-domain visitor data (minimal transfer for performance)
  struct VisitorData {
    PersonId person_id;
    int home_rank;           // Which rank owns this person
    VenueId venue_id;        // Which venue they're visiting
    SubsetIndex subset_idx;  // Which subset within the venue

    // Infection state (for transmission calculations)
    bool is_infected;
    bool is_infectious;
    float immunity_level;
    uint8_t encounter_type_id;  // Coordinated encounter type (ID in registry)

    // Stage-driven infectiousness fields
    uint16_t symptom_id = 0;     // Current symptom ID at packing time
    double time_in_stage = 0.0;  // Time in current stage at packing time

    // Pre-computed integrated infectiousness per mode (computed on sending
    // rank using the same code path as locals, ensuring bit-identical FP).
    // Sized at runtime to disease->numModes() at the visitor-build site
    // (DomainCommunicator::buildOutgoing). Wire format reads/writes the
    // active count's worth of doubles. See packVisitor/unpackVisitor.
    std::vector<double> integrated_infectiousness;

    // Return data: infection status changes
    bool newly_infected;
    double new_infection_time;
  };

  std::vector<VisitorData>
      incoming_visitors;  // Visitors at our venues (from other ranks)
  std::vector<VisitorData>
      outgoing_visitors;  // Our residents visiting other ranks' venues

  // Domain-local state
  WorldState* world;  // Pointer to shared read-only world state

  // Thread-local RNG for this domain
  std::mt19937 rng;

  // Statistics
  size_t local_population;  // Number of residents
  size_t local_venues;      // Number of venues

  // Constructor
  Domain(int rank, int num_ranks, WorldState* world, uint64_t base_seed = 0);

  // Query methods
  bool ownsGeoUnit(GeoUnitId id) const { return geo_unit_set.count(id) > 0; }

  bool ownsPerson(PersonId id) const { return resident_set.count(id) > 0; }

  // Dynamic virtual venue ownership set (populated by encounter injection)
  std::unordered_set<VenueId> virtual_venue_set;

  void registerVirtualVenue(VenueId id) { virtual_venue_set.insert(id); }
  void clearVirtualVenues() { virtual_venue_set.clear(); }

  bool ownsVenue(VenueId id) const {
    if (isVirtualVenue(id)) {
      // Check dynamic registry (set during encounter injection)
      return virtual_venue_set.count(id) > 0;
    }
    return local_venue_set.count(id) > 0;
  }

  // Add a geo unit to this domain
  void addGeoUnit(GeoUnitId id) {
    geo_unit_ids.push_back(id);
    geo_unit_set.insert(id);
  }

  // Assign people and venues based on geo unit ownership. Throws if the world
  // breaks the geo unit locality of residence: see checkResidencesAreLocal.
  void assignPeopleAndVenues();

  // Visitor management
  void clearVisitors() {
    incoming_visitors.clear();
    outgoing_visitors.clear();
  }

  void addIncomingVisitor(const VisitorData& visitor) {
    incoming_visitors.push_back(visitor);
  }

  void addOutgoingVisitor(const VisitorData& visitor) {
    outgoing_visitors.push_back(visitor);
  }

  // Print domain statistics
  void printStatistics() const;

 private:
  // A residence venue and its occupants share a geo unit, so a household is
  // held whole by one rank. The partition assumes it, and clustered seeding's
  // local density truncation is exact only under it. See ADR 0012. Throws
  // naming the offenders rather than running on a world that breaks it.
  void checkResidencesAreLocal() const;
};

}  // namespace june

#endif  // USE_MPI
