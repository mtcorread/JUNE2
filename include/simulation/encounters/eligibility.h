#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "activity/coordinated_encounter_types.h"
#include "core/config.h"
#include "core/types.h"
#include "core/world_state.h"
#include "epidemiology/policy.h"

// Pass 1 of coordinated-encounter injection: which local participants of each
// encounter survive the alive + policy checks. Exposed here (rather than kept
// private to simulator_encounters.cpp) so the encounter tests drive the real
// production logic instead of a replica.
namespace june {
namespace encounters {

// Per-slot lookup tables derived from the coordinated-encounters config:
// encounter_type_id -> trigger activity indices and min_attendees threshold.
//
// Deliberately holds no virtual/physical flag. CoordinatedEncounter::
// venue_type_id is polysemous — a world venue-type id for a physical
// encounter, a contact-matrix registry id for a virtual one — and the two
// registries are independently ordered, so the integers alias. Which one it
// is, is answered by isVirtualVenue(enc.venue_id): a property of the instance,
// not a config flag reached through a world-registry lookup that can miss.
struct EncounterLookups {
  std::unordered_map<uint8_t, std::vector<int16_t>> trigger_activities;
  std::unordered_map<uint8_t, int> min_attendees;
};

// Pass-1 result for one daily_encounter: which local participants pass the
// eligibility checks (alive + not policy-blocked at this slot), how many of
// them, and the threshold needed before this encounter gets injected.
struct EncounterEligibility {
  int encounter_idx;                     // index into daily_encounters
  std::vector<size_t> eligible_indices;  // local people passing policy
  int local_eligible;
  int min_required;
};

EncounterLookups buildEncounterLookups(
    const WorldState& world,
    const std::vector<CoordinatedEncounterDef>& encounters);

// Per-encounter, compute the local participants who survive the alive +
// policy-block checks. Encounters not scheduled for this slot are skipped.
// Each returned entry points back at its source encounter via .encounter_idx.
//
// The policy question asked is "would a policy remove this person from this
// trigger activity, at the encounter's venue type" — a Policy Suppression
// query, carrying no consequence. Pass 1 is speculative: the encounter may
// still be cancelled below min_required, so nothing here may commit a freeze,
// a hop swap or a compliance latch on the strength of it. No venue is handed
// to the policy, only the type, because nothing is pinned.
//
// A virtual encounter is at no Venue at all, so its Slot Venue Type is absent
// rather than enc.venue_type_id.
std::vector<EncounterEligibility> computeLocalEligibility(
    const std::vector<CoordinatedEncounter>& daily_encounters,
    int time_slot_index, double current_simulation_time,
    const EncounterLookups& lookups, const WorldState& world,
    const std::vector<PersonLocation>& locations,
    PolicyManager* policy_manager);

}  // namespace encounters
}  // namespace june
