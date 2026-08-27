#include "epidemiology/policy.h"

#include "epidemiology/disease.h"

namespace june {

PolicyManager::PolicyManager(WorldState& world)
    : world_(world), base_seed_(0) {}

void PolicyManager::addSymptomPolicy(const SymptomPolicy& policy) {
  symptom_policies_.push_back(policy);
}

void PolicyManager::addTemporalPolicy(const TemporalPolicy& policy) {
  temporal_policies_.push_back(policy);
}

bool PolicyManager::checkCompliance(double compliance_rate, PersonId person_id,
                                    uint32_t policy_index) const {
  SplitMix64 rng(mix_seed(base_seed_, person_id, policy_index, 0xC0E1A9ULL));
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  return dist(rng) < compliance_rate;
}

std::optional<PersonLocation> PolicyManager::getReplacementLocation(
    const Person& person, const std::string& replacement_activity,
    int16_t replacement_activity_index) {
  int16_t act_idx = replacement_activity_index;
  if (act_idx < 0) {
    act_idx =
        static_cast<int16_t>(world_.getActivityIndex(replacement_activity));
  }

  auto venues = world_.getActivityVenues(person, act_idx);
  if (venues.empty()) {
    int16_t residence_idx =
        static_cast<int16_t>(world_.getActivityIndex("residence"));
    if (act_idx != residence_idx) {
      return getReplacementLocation(person, "residence", residence_idx);
    }
    return std::nullopt;
  }

  auto [venue_id, subset_idx] = venues[0];

  PersonLocation loc;
  loc.person_id = person.id;
  loc.venue_id = venue_id;
  loc.subset_index = subset_idx;
  loc.activity_index = act_idx;
  loc.encounter_type_id = 255;  // None

  return loc;
}

std::optional<PersonLocation> PolicyManager::getOverride(
    Person& person, int16_t scheduled_activity_index, VenueId pin_venue_id,
    SubsetIndex pin_subset_index, SlotVenueType slot_venue_type,
    double current_time, int time_slot_index, const Person* partner) {
  // Resolved lazily, and cached across both policy loops: this runs once per
  // person per slot across the whole population, so an ungated run must never
  // pay for the lookup. A Deferred value naming a venue this rank cannot type
  // throws here rather than degrading to kUnknownVenueTypeId.
  SlotVenueType resolved_slot_venue_type = slot_venue_type;
  bool venue_type_resolved = false;
  auto slotVenueType = [&]() -> const SlotVenueType& {
    if (!venue_type_resolved) {
      resolved_slot_venue_type = slot_venue_type.resolveAgainst(world_);
      venue_type_resolved = true;
    }
    return resolved_slot_venue_type;
  };

  // Priority 1: symptom-based policies
  if (person.infection != nullptr) {
    uint16_t current_symptom_id =
        person.infection->getTrajectory().getCurrentSymptomId(current_time);

    uint32_t symptom_mask = person.applicable_symptom_policy_mask;
    for (size_t i = 0; symptom_mask && i < symptom_policies_.size(); ++i) {
      if (!(symptom_mask & (1u << i))) continue;

      const auto& policy = symptom_policies_[i];

      // Out of window: the policy is not in force, so it holds nobody. A
      // calendar edge is a change of government instruction, not a change in
      // the person, so no follow-up policy inherits the decision.
      if (!policy.window.contains(current_time)) {
        releaseFreeze(person, i);
        person.active_symptom_policy_participation &= ~(1u << i);
        person.symptom_policy_decisions &= ~(1u << i);
        continue;
      }

      bool is_triggered = policy.triggeredBy(current_symptom_id);

      // STICKY COMPLIANCE: if not triggered, clear participation/decision and
      // continue (with follow-up policy inheritance check)
      if (!is_triggered) {
        if (policy.follow_up_policy_index >= 0) {
          bool decision_made = (person.symptom_policy_decisions & (1u << i));
          bool participating =
              (person.active_symptom_policy_participation & (1u << i));

          if (decision_made) {
            bool should_inherit = false;
            if (participating && policy.inherit_compliance) {
              person.active_symptom_policy_participation |=
                  (1u << policy.follow_up_policy_index);
              should_inherit = true;
            }
            if (!participating && policy.inherit_refusal) {
              should_inherit = true;
            }

            if (should_inherit) {
              person.symptom_policy_decisions |=
                  (1u << policy.follow_up_policy_index);
            }
          }
        }

        // Restore paused hop state if this policy was responsible for freezing
        releaseFreeze(person, i);

        person.active_symptom_policy_participation &= ~(1u << i);
        person.symptom_policy_decisions &= ~(1u << i);
        continue;
      }

      bool has_made_decision = (person.symptom_policy_decisions & (1u << i));
      if (!has_made_decision) {
        if (checkCompliance(policy.action.compliance_rate, person.id,
                            static_cast<uint32_t>(i))) {
          person.active_symptom_policy_participation |= (1u << i);
        }
        person.symptom_policy_decisions |= (1u << i);
      }

      bool is_participating =
          (person.active_symptom_policy_participation & (1u << i));
      if (!is_participating) continue;

      if (!actionApplies(policy.action, person, scheduled_activity_index,
                         slotVenueType, partner)) {
        continue;
      }

      // Freeze-hop path: pin person at current/last overnight venue. Only
      // applies to hopped persons.
      if (policy.action.replacement_schedule_idx >= 0 &&
          person.schedule_hop.isActive()) {
        ensureResidenceIndexCached();

        auto frozen_it = frozen_states_.find(person.id);
        if (frozen_it != frozen_states_.end() &&
            frozen_it->second.triggering_policy_index ==
                static_cast<uint8_t>(i)) {
          PersonLocation loc;
          loc.person_id = person.id;
          loc.venue_id = frozen_it->second.pin_venue_id;
          loc.subset_index = frozen_it->second.pin_subset_index;
          loc.activity_index = residence_act_idx_;
          loc.encounter_type_id = 255;
          return loc;
        }

        // First freeze: ActivityManager pre-resolves no_venue transit slots
        // to the last overnight venue, so pin_venue_id is the best candidate.
        VenueId pin_venue = pin_venue_id;
        SubsetIndex pin_subset = pin_subset_index;
        if (pin_venue < 0) {
          auto home = world_.getActivityVenues(person, residence_act_idx_);
          if (!home.empty()) {
            pin_venue = home[0].first;
            pin_subset = home[0].second;
          }
        }

        int16_t saved_hop = person.schedule_hop.hopped_schedule_id;
        int16_t saved_return = person.schedule_hop.return_schedule_id;
        frozen_states_[person.id] =
            FrozenPersonState{static_cast<uint8_t>(i), saved_hop, saved_return,
                              pin_venue, pin_subset};
        person.schedule_hop.swapTarget(policy.action.replacement_schedule_idx);

        PersonLocation loc;
        loc.person_id = person.id;
        loc.venue_id = pin_venue;
        loc.subset_index = pin_subset;
        loc.activity_index = residence_act_idx_;
        loc.encounter_type_id = 255;
        return loc;
      }

      return getReplacementLocation(person, policy.action.replacement_activity,
                                    policy.action.replacement_activity_index);
    }
  } else {
    if (person.symptom_policy_decisions != 0) {
      person.resetPolicyState();
    }
  }

  // Priority 2: temporal policies (lockdowns, etc.)
  uint32_t temporal_mask = person.applicable_temporal_policy_mask;
  for (size_t i = 0; temporal_mask && i < temporal_policies_.size(); ++i) {
    if (!(temporal_mask & (1u << i))) continue;

    const auto& policy = temporal_policies_[i];

    bool is_active = policy.window.contains(current_time);

    if (!is_active) {
      person.active_temporal_policy_participation &= ~(1u << i);
      person.temporal_policy_decisions &= ~(1u << i);
      continue;
    }

    bool has_made_decision = (person.temporal_policy_decisions & (1u << i));
    if (!has_made_decision) {
      if (checkCompliance(policy.action.compliance_rate, person.id,
                          static_cast<uint32_t>(i + 100))) {
        person.active_temporal_policy_participation |= (1u << i);
      }
      person.temporal_policy_decisions |= (1u << i);
    }

    bool is_participating =
        (person.active_temporal_policy_participation & (1u << i));

    if (!is_participating) continue;

    if (!actionApplies(policy.action, person, scheduled_activity_index,
                       slotVenueType, partner)) {
      continue;
    }

    return getReplacementLocation(person, policy.action.replacement_activity,
                                  policy.action.replacement_activity_index);
  }

  return std::nullopt;
}

bool PolicyManager::suppressesParticipation(
    const Person& person, int16_t activity_index,
    const SlotVenueType& slot_venue_type, double current_time,
    const Person* partner) const {
  // A frozen Person is pinned at the venue the freeze anchored them to, so they
  // are not at the encounter's venue or the host's venue whatever those are.
  // Answered ahead of the loops: there is no meaningful venue-gate question to
  // ask about a venue they cannot be in.
  if (frozen_states_.count(person.id) > 0) return true;

  // Same lazy resolution as getOverride, and for the same reason: this runs
  // once per person per slot across the population, so an ungated run must
  // never pay for the lookup.
  SlotVenueType resolved_slot_venue_type = slot_venue_type;
  bool venue_type_resolved = false;
  auto slotVenueType = [&]() -> const SlotVenueType& {
    if (!venue_type_resolved) {
      resolved_slot_venue_type = slot_venue_type.resolveAgainst(world_);
      venue_type_resolved = true;
    }
    return resolved_slot_venue_type;
  };

  // Priority 1: symptom-based policies. Untriggered policies are skipped
  // outright — getOverride uses that branch to release a freeze and propagate
  // follow-up inheritance, and neither is this function's business.
  if (person.infection != nullptr) {
    uint16_t current_symptom_id =
        person.infection->getTrajectory().getCurrentSymptomId(current_time);

    uint32_t symptom_mask = person.applicable_symptom_policy_mask;
    for (size_t i = 0; symptom_mask && i < symptom_policies_.size(); ++i) {
      if (!(symptom_mask & (1u << i))) continue;

      const auto& policy = symptom_policies_[i];
      if (!policy.window.contains(current_time)) continue;
      if (!policy.triggeredBy(current_symptom_id)) continue;

      if (!isParticipating(person.symptom_policy_decisions,
                           person.active_symptom_policy_participation, i,
                           policy.action.compliance_rate, person.id,
                           static_cast<uint32_t>(i))) {
        continue;
      }

      if (actionApplies(policy.action, person, activity_index, slotVenueType,
                        partner)) {
        return true;
      }
    }
  }

  // Priority 2: temporal policies (lockdowns, etc.)
  uint32_t temporal_mask = person.applicable_temporal_policy_mask;
  for (size_t i = 0; temporal_mask && i < temporal_policies_.size(); ++i) {
    if (!(temporal_mask & (1u << i))) continue;

    const auto& policy = temporal_policies_[i];
    if (!policy.window.contains(current_time)) continue;

    if (!isParticipating(person.temporal_policy_decisions,
                         person.active_temporal_policy_participation, i,
                         policy.action.compliance_rate, person.id,
                         static_cast<uint32_t>(i + 100))) {
      continue;
    }

    if (actionApplies(policy.action, person, activity_index, slotVenueType,
                      partner)) {
      return true;
    }
  }

  return false;
}

void PolicyManager::precomputePolicyApplicability(std::vector<Person>& people) {
  for (auto& person : people) {
    if (person.is_dead) continue;

    person.applicable_symptom_policy_mask = 0;
    person.applicable_temporal_policy_mask = 0;

    for (size_t i = 0;
         i < std::min(symptom_policies_.size(), kMaxPoliciesPerKind); ++i) {
      const auto& policy = symptom_policies_[i];
      if (policy.appliesTo(person, &world_)) {
        person.applicable_symptom_policy_mask |= (1u << i);
      }
    }

    for (size_t i = 0;
         i < std::min(temporal_policies_.size(), kMaxPoliciesPerKind); ++i) {
      const auto& policy = temporal_policies_[i];
      if (policy.appliesTo(person, &world_)) {
        person.applicable_temporal_policy_mask |= (1u << i);
      }
    }
  }
}

}  // namespace june
