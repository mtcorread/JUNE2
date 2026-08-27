#include "epidemiology/epidemiology.h"

#include "epidemiology/policy.h"

namespace june {

Epidemiology::Epidemiology(WorldState& world, const Disease* disease,
                           EventLogger* event_logger)
    : world_(world), disease_(disease), event_logger_(event_logger) {}

EpiSlotStats Epidemiology::updateInfectionStates(
    double current_simulation_time,
    const std::vector<PersonLocation>& locations) {
  int recoveries = 0;
  int deaths = 0;
  int total_transitions_processed = 0;

  std::vector<PersonId> to_remove;  // Track people to remove from active set

  for (PersonId pid : active_infections_) {
    Person* person = world_.getPerson(pid);
    if (!person || person->is_dead) {
      to_remove.push_back(pid);
      continue;
    }

    if (person->infection != nullptr) {
      // Get the full trajectory
      const auto& trajectory = person->infection->getTrajectory();

      // Get current venue for logging (if locations vector is provided and
      // index is valid)
      VenueId current_venue_id = -1;
      auto it_idx = world_.person_index.find(pid);
      if (it_idx != world_.person_index.end() &&
          it_idx->second < locations.size()) {
        current_venue_id = locations[it_idx->second].venue_id;
      }

      // Get last processed time (or infection time if first check)
      double last_processed_time = trajectory.infection_time;
      auto it = last_processed_transition_time_.find(pid);
      if (it != last_processed_transition_time_.end()) {
        last_processed_time = it->second;
      }

      // Replay all transitions that occurred since last check
      uint16_t previous_symptom_id =
          trajectory.getCurrentSymptomId(last_processed_time);
      bool was_in_hospital = false;
      bool was_in_icu = false;

      for (const auto& [transition_time, symptom_id] : trajectory.transitions) {
        // Only process transitions in the window (last_processed_time,
        // current_simulation_time]
        if (transition_time <= last_processed_time ||
            transition_time > current_simulation_time) {
          continue;
        }

        const std::string& symptom = disease_->getSymptomName(symptom_id);

        if (event_logger_) {
          event_logger_->logSymptomChange(
              pid, current_venue_id, transition_time,
              static_cast<uint8_t>(previous_symptom_id),
              static_cast<uint8_t>(symptom_id));
        }
        total_transitions_processed++;

        // Check for hospital admission (entering hospital system)
        bool now_in_hospital = disease_->isHospitalisedStage(symptom) ||
                               disease_->isICUStage(symptom);
        if (now_in_hospital && !was_in_hospital) {
          if (event_logger_) {
            std::string reason = disease_->isICUStage(symptom)
                                     ? "intensive_care"
                                     : "hospitalised";
            event_logger_->logHospitalAdmission(pid, current_venue_id,
                                                transition_time, reason);
          }
          was_in_hospital = true;
        }

        // Check for ICU admission
        bool now_in_icu = disease_->isICUStage(symptom);
        if (now_in_icu && !was_in_icu) {
          if (event_logger_) {
            event_logger_->logICUAdmission(pid, current_venue_id,
                                           transition_time);
          }
          was_in_icu = true;
        }

        // Check for hospital discharge (leaving hospital/ICU)
        if (was_in_hospital && !now_in_hospital) {
          if (event_logger_) {
            std::string outcome;
            if (disease_->isRecoveredStage(symptom)) {
              outcome = "recovered";
            } else if (!disease_->isFatalStage(symptom)) {
              outcome = "discharged_to_home";
            } else {
              outcome = "other";
            }
            event_logger_->logHospitalDischarge(pid, current_venue_id,
                                                transition_time, outcome);
          }
          was_in_hospital = false;
          was_in_icu = false;
        }

        // Update previous symptom for next iteration
        previous_symptom_id = symptom_id;
      }

      // Update last processed time to current simulation time
      last_processed_transition_time_[pid] = current_simulation_time;

      // Check if person has recovered or died
      bool recovered = person->infection->isRecovered(current_simulation_time);
      bool dead = person->infection->isDead(current_simulation_time);

      if (recovered) {
        // Set natural immunity if recovered
        const auto& immunity_params =
            disease_->getTransmissionParams().natural_immunity;
        person->immunity.natural_level = immunity_params.level;
        person->immunity.natural_acquisition_time = current_simulation_time;
        person->immunity.natural_waning_rate = immunity_params.waning_rate;

        // Clear infection
        person->infection.reset();
        person->resetPolicyState();
        if (policy_manager_) policy_manager_->releaseAnyFreeze(*person);
        last_processed_transition_time_.erase(pid);
        to_remove.push_back(pid);
        recoveries++;
      } else if (dead) {
        // Find the exact death time from trajectory
        double death_time = current_simulation_time;
        for (const auto& [transition_time, symptom_id] :
             trajectory.transitions) {
          if (disease_->isFatalStage(disease_->getSymptomName(symptom_id)) &&
              transition_time <= current_simulation_time) {
            death_time = transition_time;
            break;  // Take the first (earliest) death transition
          }
        }

        if (event_logger_) {
          event_logger_->logDeath(pid, current_venue_id, death_time);
        }

        // Mark person as dead
        person->is_dead = true;
        person->death_time = death_time;

        // Clear infection
        person->infection.reset();
        person->resetPolicyState();
        if (policy_manager_) policy_manager_->releaseAnyFreeze(*person);
        last_processed_transition_time_.erase(pid);
        to_remove.push_back(pid);
        deaths++;
      }
    } else {
      // Person has no infection but is in active set (shouldn't happen, but
      // clean up)
      last_processed_transition_time_.erase(pid);
      to_remove.push_back(pid);
    }
  }

  // Remove resolved infections from active set
  for (PersonId pid : to_remove) {
    active_infections_.erase(pid);
  }

  return {total_transitions_processed, recoveries, deaths,
          static_cast<int>(active_infections_.size())};
}

void Epidemiology::updateVenueFomites(double current_simulation_time,
                                      double delta_hours) {
  if (!disease_) return;
  const auto& trans_params = disease_->getTransmissionParams();

  // Build ordered list of fomite modes (same order as interaction_manager)
  struct FomiteModeRef {
    int mode_index;
    const FomiteConfig* cfg;
  };
  std::vector<FomiteModeRef> fomite_modes;
  for (int midx = 0; midx < (int)trans_params.modes.size(); ++midx) {
    const auto& tmode = trans_params.modes[midx];
    if (tmode.type == TransmissionModeType::Fomite) {
      fomite_modes.push_back(
          FomiteModeRef{midx, &std::get<FomiteConfig>(tmode.config)});
    }
  }
  if (fomite_modes.empty()) return;

  for (auto& venue : world_.venues) {
    auto& history = venue.fomite_history;
    // Ensure history has enough slots for all fomite modes
    if ((int)history.size() < (int)fomite_modes.size()) {
      history.resize(fomite_modes.size());
    }
    // Prune old entries from each mode's deposition history
    for (int local_fm = 0; local_fm < (int)fomite_modes.size(); ++local_fm) {
      double max_age = fomite_modes[local_fm].cfg->max_age;
      auto& deque = history[local_fm];
      while (!deque.empty() &&
             (current_simulation_time - deque.front().time) > max_age) {
        deque.pop_front();
      }
    }
  }
}

void Epidemiology::trackInfection(PersonId pid) {
  active_infections_.insert(pid);
}

void Epidemiology::untrackInfection(PersonId pid) {
  active_infections_.erase(pid);
  last_processed_transition_time_.erase(pid);
}

}  // namespace june
