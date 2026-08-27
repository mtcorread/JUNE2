#pragma once

#ifdef USE_MPI

#include <mpi.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "activity/runtime_group_allocator.h"
#include "core/config.h"
#include "core/world_state.h"
#include "domain.h"
#include "domain_communicator.h"
#include "geography_partitioner.h"

namespace june {

/**
 * DomainManager - Orchestrates MPI-based domain decomposition for parallel
 * simulation
 */
class DomainManager {
 public:
  DomainManager(WorldState& world, const Config& config);

  // Initialize MPI and partition world
  void initialize();

  void setWorldStateFile(const std::string& filename) {
    world_state_file_ = filename;
  }

  // Domain access
  Domain& getDomain() { return domain_; }
  const Domain& getDomain() const { return domain_; }

  // MPI info
  int getRank() const { return rank_; }
  int getNumRanks() const { return num_ranks_; }
  void setMPI(int rank, int num_ranks);  // For tests

  void setDisease(const Disease* disease);

  // Delegated exchanges
  void exchangeVisitors(const std::vector<PersonLocation>& locations,
                        double current_time, double delta_hours = 0.0,
                        const RuntimeGroupAllocator* alloc = nullptr);
  std::unordered_set<PersonId> getVisitorIds() const;
  std::vector<PendingInfection> receivePendingInfections(
      const std::vector<PendingInfection>& pending_infections);

  // Cross-rank coordinated encounter exchange
  void exchangeEncounterProposals(
      const std::vector<EncounterProposal>& local_proposals,
      std::vector<EncounterProposal>& proposals_for_this_rank);
  void exchangeEncounterReplies(
      const std::vector<EncounterReply>& local_replies,
      std::vector<EncounterReply>& replies_for_this_rank);
  void exchangeFinalizedEncounters(
      const std::vector<CoordinatedEncounter>& local_finalized,
      std::vector<CoordinatedEncounter>& finalized_for_this_rank);

  // Global Metadata Sharing
  void exchangeDeathFlags();
  bool isDeadGlobally(PersonId pid) const {
    if (pid < 0 || pid >= static_cast<PersonId>(global_death_flags_.size()))
      return false;
    return global_death_flags_[pid] != 0;
  }
  void exchangeScheduleTypes();
  void exchangeActivityMasks();
  uint16_t getGlobalScheduleType(PersonId pid) const;
  void setGlobalScheduleType(PersonId pid, uint16_t type_id);  // For tests
  ActivityMask getGlobalActivityMask(PersonId pid) const;
  void setGlobalActivityMask(PersonId pid, ActivityMask mask);  // For tests
  void setMaxPersonId(PersonId max_id) {
    max_person_id_ = max_id;
  }  // For tests
  void setPersonRank(PersonId pid, int rank);     // For tests
  void setGeoUnitRank(GeoUnitId guid, int rank);  // For tests
  void setVenueRank(VenueId vid, int rank) {
    global_venue_rank_[vid] = rank;
  }  // For tests

  // Clear virtual venue entries from the rank map.
  // Called each time slot alongside Domain::clearVirtualVenues() so that
  // stale virtual venue assignments don't prevent re-registration when
  // a new encounter reuses the same hash-derived venue ID.
  void clearVirtualVenueRanks() {
    for (auto it = global_venue_rank_.begin();
         it != global_venue_rank_.end();) {
      if (isVirtualVenue(it->first))
        it = global_venue_rank_.erase(it);
      else
        ++it;
    }
  }

  // Synchronize person property value registries across all ranks so that
  // config.resolve() builds identical compatibility matrices everywhere.
  // Must be called after initialize() and before config.resolve(world).
  void synchronizeRegistries();

  // Report local domain statistics (population, venues, memory)
  void reportDomainStats(const std::string& label) const;

  // Get the rank that owns a specific venue
  int getVenueRank(VenueId vid) const;
  // Check if a venue is known globally (local or remote)
  bool isKnownVenue(VenueId vid) const {
    if (domain_.ownsVenue(vid)) return true;
    return global_venue_rank_.count(vid) > 0;
  }

  // Get the rank that owns a specific person
  int getPersonRank(PersonId pid) const;

 private:
  void loadGlobalPersonMetadata();
  void broadcastPopulationCounts();
  void loadDomainData();
  void buildVenueOwnershipMap();
  void buildGlobalVenueOwnershipMap();  // Shares all venue IDs across ranks
  void loadGeographyOnNonZeroRanks();
  void computeGlobalMaxPersonId();

  // Generic MPI global property exchange via Allreduce.
  // Fills global_buf[pid] with the value from the owning rank for each person.
  template <typename T>
  void exchangeGlobalProperty(std::vector<T>& global_buf, T null_value,
                              MPI_Datatype mpi_type, MPI_Op mpi_op,
                              std::function<T(const Person&)> extract);

  // Context
  WorldState& world_;
  const Config& config_;
  std::string world_state_file_;

  // MPI State
  int rank_, num_ranks_;

  // Components
  Domain domain_;
  std::unique_ptr<GeographyPartitioner> partitioner_;
  std::unique_ptr<DomainCommunicator> communicator_;

  // Ownership
  std::unordered_map<GeoUnitId, int> geounit_to_rank_;
  std::unordered_map<VenueId, int>
      global_venue_rank_;  // Global venue_id → owning rank (all ranks)
  std::vector<GeoUnitId>
      global_person_geounit_;  // Mapping from PersonId to GeoUnitId
  std::vector<int>
      global_person_rank_;  // Mapping from PersonId to owning MPI rank
  std::vector<uint16_t>
      global_person_schedule_type_;  // Mapping from PersonId to ScheduleTypeID
  std::vector<ActivityMask>
      global_person_activity_mask_;  // Mapping from PersonId to
                                     // venue-availability bitmask
  std::vector<uint8_t>
      global_death_flags_;  // 1 if person is dead on any rank, 0 otherwise
  PersonId max_person_id_ = 0;
};

}  // namespace june

#endif  // USE_MPI
