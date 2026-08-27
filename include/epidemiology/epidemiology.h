#pragma once

#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/types.h"
#include "core/world_state.h"
#include "disease.h"
#include "utils/event_logging/event_logger.h"

namespace june {

class PolicyManager;

struct EpiSlotStats {
  int transitions = 0;
  int recoveries = 0;
  int deaths = 0;
  int active_remaining = 0;
};

class Epidemiology {
 public:
  Epidemiology(WorldState& world, const Disease* disease,
               EventLogger* event_logger = nullptr);

  // Update infection states (symptom changes, recoveries, deaths) for all
  // actively infected people
  EpiSlotStats updateInfectionStates(
      double current_simulation_time,
      const std::vector<PersonLocation>& locations);

  // Apply decay to venue fomite loads based on time elapsed
  void updateVenueFomites(double current_simulation_time, double delta_hours);

  // Recovery and death end any policy freeze the person is under, so
  // Epidemiology needs the manager holding it. Absent (tests that run no
  // policies), nothing is released.
  void setPolicyManager(PolicyManager* policy_manager) {
    policy_manager_ = policy_manager;
  }

  // Track a newly infected person
  void trackInfection(PersonId pid);

  // Remove a person from active infection tracking
  void untrackInfection(PersonId pid);

  // Get the set of actively infected person IDs
  const std::unordered_set<PersonId>& getActiveInfections() const {
    return active_infections_;
  }

  // Get mutable reference to active infections (e.g. for interaction manager)
  std::unordered_set<PersonId>& getActiveInfectionsMutable() {
    return active_infections_;
  }

  // --- Checkpoint serialization ---
  // last_processed_transition_time_ is NOT derivable from per-person state:
  // the baseline processes some transitions lazily after their scheduled
  // time, so it must be saved and restored verbatim or those transitions are
  // lost across a resume.
  const std::unordered_map<PersonId, double>& getLastProcessedTransitionTimes()
      const {
    return last_processed_transition_time_;
  }

  // Rebuild derived caches after a checkpoint restore. active_infections_ is
  // reconstructed by scanning who currently carries an Infection;
  // last_processed_transition_time_ is restored verbatim from the checkpoint.
  void restoreAfterCheckpoint(
      const std::unordered_map<PersonId, double>& last_processed) {
    active_infections_.clear();
    for (const auto& p : world_.people)
      if (p.infection) active_infections_.insert(p.id);
    last_processed_transition_time_ = last_processed;
  }

 private:
  WorldState& world_;
  const Disease* disease_;
  EventLogger* event_logger_;
  PolicyManager* policy_manager_ = nullptr;

  // Only track people who are currently infected
  std::unordered_set<PersonId> active_infections_;

  // Track last processed transition time from trajectory for each infected
  // person This allows us to replay only new transitions from the trajectory
  std::unordered_map<PersonId, double> last_processed_transition_time_;
};

}  // namespace june
