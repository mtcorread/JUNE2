#include "simulation/encounters/eligibility.h"

#include <utility>

namespace june {
namespace encounters {

EncounterLookups buildEncounterLookups(
    const WorldState& world,
    const std::vector<CoordinatedEncounterDef>& encounters) {
  EncounterLookups out;
  for (const auto& def : encounters) {
    int type_id = world.getEncounterTypeIndex(def.name);
    if (type_id < 0) continue;
    std::vector<int16_t> indices;
    for (const auto& slot_name : def.trigger_slots) {
      int idx = world.getActivityIndex(slot_name);
      if (idx >= 0) indices.push_back(static_cast<int16_t>(idx));
    }
    out.trigger_activities[static_cast<uint8_t>(type_id)] = std::move(indices);
    out.min_attendees[static_cast<uint8_t>(type_id)] = def.min_attendees;
  }
  return out;
}

std::vector<EncounterEligibility> computeLocalEligibility(
    const std::vector<CoordinatedEncounter>& daily_encounters,
    int time_slot_index, double current_simulation_time,
    const EncounterLookups& lookups, const WorldState& world,
    const std::vector<PersonLocation>& locations,
    PolicyManager* policy_manager) {
  std::vector<EncounterEligibility> slot_encounters;
  for (int ei = 0; ei < static_cast<int>(daily_encounters.size()); ++ei) {
    const auto& enc = daily_encounters[ei];
    if (enc.slot != time_slot_index) continue;

    auto trig_it = lookups.trigger_activities.find(enc.encounter_type_id);

    // A virtual encounter occupies no Venue, so it never passes a venue gate.
    // Reading enc.venue_type_id for one would take a contact-matrix id for a
    // world venue-type id and gate on an unrelated real venue type. The
    // discriminator is the instance's own venue id, which cannot miss a
    // lookup.
    const SlotVenueType slot_venue_type =
        isVirtualVenue(enc.venue_id) ? SlotVenueType::absent()
                                     : SlotVenueType::known(enc.venue_type_id);

    std::vector<size_t> eligible_indices;
    for (PersonId pid : enc.participants) {
      auto it = world.person_index.find(pid);
      if (it == world.person_index.end()) continue;

      size_t array_idx = it->second;
      if (array_idx >= locations.size()) continue;

      const Person& person = world.people[array_idx];
      if (person.is_dead) continue;

      bool policy_blocked = false;
      if (policy_manager && trig_it != lookups.trigger_activities.end()) {
        for (int16_t trigger_act_idx : trig_it->second) {
          if (policy_manager->suppressesParticipation(
                  person, trigger_act_idx, slot_venue_type,
                  current_simulation_time)) {
            policy_blocked = true;
            break;
          }
        }
      }
      if (!policy_blocked) eligible_indices.push_back(array_idx);
    }

    int min_required = 2;
    auto min_it = lookups.min_attendees.find(enc.encounter_type_id);
    if (min_it != lookups.min_attendees.end()) {
      min_required = min_it->second;
    }

    EncounterEligibility ee;
    ee.encounter_idx = ei;
    ee.eligible_indices = std::move(eligible_indices);
    ee.local_eligible = static_cast<int>(ee.eligible_indices.size());
    ee.min_required = min_required;
    slot_encounters.push_back(std::move(ee));
  }
  return slot_encounters;
}

}  // namespace encounters
}  // namespace june
