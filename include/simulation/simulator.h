#pragma once

#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "activity/coordinated_encounter_manager.h"
#include "activity/on_the_fly_venue_allocator.h"
#include "activity/runtime_group_allocator.h"
#include "core/config.h"
#include "core/world_state.h"
#include "epidemiology/calendar_event.h"
#include "epidemiology/disease.h"
#include "epidemiology/epidemiology.h"
#include "epidemiology/infection_seed.h"
#include "epidemiology/interaction_manager.h"
#include "epidemiology/policy.h"
#ifdef USE_MPI
#include "parallel/seed_offer_exchange_mpi.h"
#endif
#include "epidemiology/vaccination_manager.h"
#include "loaders/disease_loader.h"
#include "loaders/policy_loader.h"
#include "simulation/compartmental_model_manager.h"
#include "utils/event_logging/event_logger.h"
#include "utils/memory_utils.h"
#include "utils/profiler.h"
#include "utils/random.h"
#include "utils/time_utils.h"

#ifdef USE_MPI
#include "parallel/domain.h"
#include "parallel/domain_manager.h"
#endif

namespace june {

#ifndef USE_MPI
// Forward declaration for when MPI is not available
class DomainManager;
#endif

class Simulator {
 public:
  Simulator(
      WorldState& world, Config& config, DomainManager* domain_mgr = nullptr,
      const std::string& infection_seeds_file = "config/infection_seeds.yaml",
      const std::string& output_filename = "simulation_events.h5");

  // Run the full simulation
  void run();

  // Apply configured infection seeds at a specific time
  void applyInfectionSeeds(const std::string& current_datetime);

  // Get event logger for external access
  EventLogger* getEventLogger() { return &event_logger_; }

  // Write a checkpoint of all mutable simulation state (P3). Atomic: state
  // is written into "<root>.tmp/", then renamed; manifest.yaml is written
  // last as the commit marker. Per-rank shards keyed on global ids make the
  // checkpoint rank-count independent.
  void writeCheckpoint(int completed_day, const std::string& date_iso);

  // Restore all mutable state from a checkpoint directory (P4). Overlays the
  // delta onto the already-loaded world by global id, restores manager +
  // scalar state, rebuilds derived caches, and makes run() resume at
  // completed_day + 1. Rank-count independent.
  void restoreFromCheckpoint(const std::string& checkpoint_dir);

 private:
  WorldState& world_;
  Config& config_;
  DomainManager*
      domain_mgr_;  // Optional: nullptr for serial mode, set for parallel mode

  // Disease and infection management
  std::unique_ptr<Disease> disease_;
  std::unique_ptr<InfectionSeeder> infection_seeder_;
#ifdef USE_MPI
  MpiSeedOfferExchange seed_offer_exchange_;
#endif

  // Calendar event management (no-op when no CSV paths are configured)
  CalendarEventManager calendar_event_manager_;
  std::unordered_map<int32_t, std::vector<GeoUnitId>> catchment_rules_;

  // On-the-fly venue allocator (nullopt when no config path is set)
  std::optional<OnTheFlyVenueAllocator> on_the_fly_allocator_;

  // Activity management
  ActivityManager activity_manager_;

  // Runtime group allocator (a train's carriages, a ferry's decks). Cheap
  // no-op when SimulationConfig::partial_presence is empty.
  std::unique_ptr<RuntimeGroupAllocator> runtime_group_allocator_;

  // Policy management (symptom-based behavior, lockdowns, etc.)
  std::unique_ptr<PolicyManager> policy_manager_;

  // Interaction and transmission management
  std::unique_ptr<InteractionManager> interaction_manager_;
  std::unique_ptr<VaccinationManager> vaccination_manager_;
  std::unique_ptr<CoordinatedEncounterManager> coordinated_encounter_manager_;

  std::unique_ptr<CompartmentalModelManager> compartmental_model_manager_;

  // Event logging
  EventLogger event_logger_;
  // Monotonic per-rank counter stamped into CoordinatedEncounterEvent.group_id
  // so every pair-row of the same real encounter shares the same id. Rank is
  // packed into the high 16 bits at stamping time for cross-rank uniqueness.
  uint64_t next_encounter_group_id_ = 0;

  // Day to resume the main loop at (set by restoreFromCheckpoint; 0 = fresh
  // run). Equals checkpoint completed_day + 1.
  int resume_from_day_ = 0;

  // Current simulation state
  std::tm current_date_;
  int current_day_num_;
  double current_simulation_time_;  // In days from start
  int total_days_;
  // Per-day-type occurrence counts (indexed by day type index)
  std::vector<int> day_type_counts_;

  // Person locations (person_id -> location)
  std::vector<PersonLocation> locations_;

  // Incremental lookup tracking
  std::unordered_set<PersonId>
      lookups_written_;  // Tracks which people have been saved to HDF5

  std::unique_ptr<Epidemiology> epidemiology_;

  // Returns the MPI rank of this process, or 0 for serial / no-MPI builds.
  int getRank() const {
#ifdef USE_MPI
    return domain_mgr_ ? domain_mgr_->getRank() : 0;
#else
    return 0;
#endif
  }

  // Returns the MPI world size, or 1 for serial / no-MPI builds.
  int getNumRanks() const {
#ifdef USE_MPI
    return domain_mgr_ ? domain_mgr_->getNumRanks() : 1;
#else
    return 1;
#endif
  }

  // Simulation loop functions
  void runOneDay(int day, int rank);
  void simulateDay(int day_num);
  void simulateTimeSlot(const TimeSlot& slot, int time_slot_index,
                        int day_type_idx, double delta_hours);

  // Coordinated-encounter daily negotiation: generate proposals locally,
  // exchange across ranks, process, exchange replies, finalize, log locally,
  // and broadcast finalized encounters back so remote participants pick them
  // up. Runs the full Phases 1–4 when coordinated_encounter_manager_ is set.
  void negotiateAndLogDailyEncounters(int day, int rank);

  // Log this rank's finalized encounters with a rank-tagged group_id. Called
  // before the cross-rank merge so paired rows are never double-logged.
  void logFinalizedEncountersLocally(
      const std::vector<CoordinatedEncounter>& finalized, int rank);

  // Two-pass coordinated-encounter injection for this slot: build per-slot
  // lookups, dedup + sort daily_encounters, compute local eligibility,
  // Allgatherv global eligibility, then stamp venue_id / encounter_type_id
  // onto every eligible participant's PersonLocation.
  void injectCoordinatedEncountersIntoSlot(int time_slot_index);

  // Follow: each slot, every follower is placed at the venue its host resolved
  // to. Runs right after encounter injection so the host's location for the
  // slot is already settled. A scenario may configure several rules at once;
  // each keeps its own binding state and they are applied in config order.
  void injectFollowsIntoSlot(int time_slot_index);

  // One rule's binding state. follower_host maps a follower to the host it
  // shadows; active_hosts is the set of hosts currently bound (for a stochastic
  // trip this is the hosts already enrolled so they do not re-roll each slot;
  // for a criteria binding it is who a rebuild last picked). follow_day is the
  // last day the criteria bindings were rebuilt; it starts at -1 so a fresh or
  // just-restored run rebuilds on its first slot, and it is not saved in a
  // checkpoint, which is what makes a criteria binding derive itself after a
  // resume.
  struct FollowRuntime {
    std::unordered_map<PersonId, PersonId> follower_host;
    std::unordered_set<PersonId> active_hosts;
    int follow_day = -1;
  };
  std::vector<FollowRuntime> follow_state_;

  // Apply one follow rule for this slot. committed_hosts / committed_followers
  // accumulate across rules in config order: everyone already bound by an
  // earlier rule, so this rule yields to them (a follower is exclusive to the
  // first rule that binds it; a host may recur, but no one is both). The rule
  // adds its own hosts and followers to those sets on the way out.
  // rule_id is the rule's index in the follows list, and is what
  // /events/follows records against the follow_rules registry.
  // cotravel collects (follower, host) pairs whose host is on a line, for the
  // allocator to seat once every rule has run.
  void processFollowRule(int time_slot_index, int day, uint8_t rule_id,
                         const FollowConfig& fc, FollowRuntime& st,
                         std::unordered_set<PersonId>& committed_hosts,
                         std::unordered_set<PersonId>& committed_followers,
                         std::vector<std::pair<PersonId, PersonId>>& cotravel);

  // Steps 5 + 6 of simulateTimeSlot: run the epidemiology state update
  // (symptom transitions / recoveries / deaths) and then decay the venue
  // fomite buffer for this slot. Both wrapped in try/catch with a Fatal
  // log + rethrow. Returns the EpiSlotStats from Step 5 so the per-slot
  // summary can print it.
  EpiSlotStats updateEpidemiologyAfterTransmission(double delta_hours);

  // Step 3 of simulateTimeSlot: drive InteractionManager::processTransmissions
  // on this rank's locations + (in MPI mode) incoming visitors. The three
  // visitor-related pointers are nullable so non-MPI / no-domain calls can
  // pass nullptr; pending_infections is filled by the call and consumed by
  // Step 4. Returns the local new-infection count.
  int runSlotTransmission(
      std::vector<PersonLocation>& transmission_locations, double delta_hours,
      int day_type_idx, std::unordered_set<PersonId>* visitor_ids,
      std::vector<PendingInfection>* pending_infections,
      std::unordered_map<PersonId, VisitorInfo>* visitor_data_map);

#ifdef USE_MPI
  // Step 2 of simulateTimeSlot in MPI builds: trigger the cross-rank
  // visitor exchange, then fill `augmented_locations` with this rank's
  // locals at locally-owned venues plus incoming visitors (sorted by
  // person_id for deterministic processing order), and populate the
  // visitor_ids set + visitor_data_map used by transmission processing.
  void exchangeVisitorsAndBuildAugmented(
      double delta_hours, std::vector<PersonLocation>& augmented_locations,
      std::unordered_set<PersonId>& visitor_ids,
      std::unordered_map<PersonId, VisitorInfo>& visitor_data_map);

  // Step 4 of simulateTimeSlot in MPI builds: route this rank's
  // pending_infections back to home ranks, then track + log every
  // applied infection. Bypass when no DomainManager.
  void receivePendingAndApply(
      const std::vector<PendingInfection>& pending_infections);
#endif

  // End-of-run output: write final epidemic events + lookups (collective on
  // all ranks; appends to the per-rank events file), then on rank 0 print
  // the wall-clock summary, activity/interaction stats, and encounter stats.
  void writeFinalEventsAndLookups(int rank);
  void printRunSummary();

  // End-of-day checkpoint trigger. Consults the configured cadence and, if
  // the day fires, announces on rank 0 and writes a checkpoint.
  void maybeWriteCheckpoint(int day, int rank);

  // Per-rank piece of writeCheckpoint: write this rank's owned world_ subset
  // (population + sparse infection / vaccine / fomite) plus the per-rank
  // global-id-keyed manager state (epidemiology lpt + policy frozen_states)
  // into delta_rank<r>.h5, embedding a partition_index so restore can
  // selectively read by geo_unit.
  void writeCheckpointRankShard(const std::filesystem::path& tmp, int rank,
                                int comp);

  // Read the global / scalar restore state from state.h5 into class members
  // (current_simulation_time_, next_encounter_group_id_, day_type_counts_,
  // and the infection_seeder's applied_seeds). Per-rank manager state
  // (lpt, frozen_states) is overlaid from the shards, not from here.
  void restoreCheckpointStateFile(const std::filesystem::path& cp);

  // Throw if --days / end_date leaves nothing to simulate after a resume.
  // --days is anchored to the ORIGINAL start_date, not the checkpoint, so a
  // resume that lands at or past total_days_ would silently no-op. Fail
  // loudly instead, pointing at the right --days value to use.
  void validateResumeBounds(int completed_day) const;

  // Rank-0-only piece of writeCheckpoint: emit state.h5 with the scalars
  // (completed_day, current_simulation_time_, next_encounter_group_id_, …),
  // the infection_seeder's applied_seeds, and the rank-0 event-log buffered
  // record counts. Per-rank manager state (lpt, frozen_states) lives in
  // each shard, not here, so the checkpoint stays rank-count-independent.
  void writeCheckpointStateFile(const std::filesystem::path& tmp,
                                int completed_day, int nranks);

  // Fomite initialization
  void initFomiteState();

  // Statistics and output
  void outputStatistics();
  void outputInfectionStatistics();
  void printSimulationState(const std::string& time_slot_name,
                            double delta_hours);

  // Dynamic flushing control
  std::string events_filename_;
  void checkAndFlushEvents(bool is_day_end = false);
};

}  // namespace june
