#include "activity/activity_manager.h"

#include <iomanip>

#include "activity/on_the_fly_venue_allocator.h"
#include "epidemiology/calendar_event.h"
#include "epidemiology/policy.h"
#include "utils/deterministic_rng.h"
#include "utils/random.h"

namespace june {

ActivityManager::ActivityManager(WorldState& world, const Config& config)
    : world_(world),
      config_(config),
      base_seed_(config.simulation.random_seed) {}

void ActivityManager::setPolicyManager(PolicyManager* policy_manager) {
  policy_manager_ = policy_manager;
}

void ActivityManager::setCalendarEventManager(
    CalendarEventManager* calendar_event_manager) {
  calendar_event_manager_ = calendar_event_manager;
}

void ActivityManager::setOnTheFlyVenueAllocator(
    OnTheFlyVenueAllocator* allocator) {
  on_the_fly_allocator_ = allocator;
}

void ActivityManager::assignScheduleTypes() {
  if (config_.schedule.schedule_types.empty()) {
    std::cerr
        << "ERROR: No schedule types defined! Please use v2 schedule format."
        << std::endl;
    return;
  }

  for (auto& person : world_.people) {
    if (person.is_dead) continue;
    assignScheduleTypeForPerson(person);
  }
}

const ScheduleType* ActivityManager::findScheduleTypeByName(
    const std::string& name) const {
  for (const auto& sched_type : config_.schedule.schedule_types) {
    if (sched_type.name == name) return &sched_type;
  }
  return nullptr;
}

void ActivityManager::assignScheduleTypeForPerson(Person& person) {
  // Skip if person already has a schedule type assigned (e.g., from HDF5)
  if (person.schedule_type_id != 0xFFFF) {
    std::string type_name =
        person.schedule_type_id < world_.schedule_type_names.size()
            ? world_.schedule_type_names[person.schedule_type_id]
            : "unknown";
    // BUG FIX 1: Set cached schedule type pointer correctly when loaded from
    // state
    person.cached_schedule_type_ = findScheduleTypeByName(type_name);
    return;
  }

  // 1. Try CSV-based probabilistic assignment first
  //    Use a per-person seed for reproducible assignment across MPI ranks.
  std::mt19937 csv_rng(mix_seed(base_seed_, person.id, 0xC5A0ULL));
  const ScheduleType* matched_type =
      config_.schedule.tryCSVAssignment(person, world_, csv_rng);

  // 2. Fall back to YAML selection_criteria if CSV returned nothing
  if (!matched_type)
    matched_type = config_.schedule.getScheduleTypeForPerson(person, &world_);

  if (matched_type) {
    // Find ID for the name
    int type_id = world_.getScheduleTypeIndex(matched_type->name);
    if (type_id >= 0) {
      person.schedule_type_id = static_cast<uint16_t>(type_id);
    }
    person.cached_schedule_type_ = matched_type;
  } else {
    // Fallback: use default schedule type
    const std::string& def_type = config_.schedule.default_schedule_type;
    int type_id = world_.getScheduleTypeIndex(def_type);
    if (type_id >= 0) {
      person.schedule_type_id = static_cast<uint16_t>(type_id);
    }
    // Cache the default schedule type pointer
    person.cached_schedule_type_ = findScheduleTypeByName(def_type);
  }
}

void ActivityManager::setDeadLocation(PersonLocation& loc) const {
  loc.venue_id = -1;
  loc.subset_index = -1;
  loc.activity_index = dead_act_idx_;
  loc.encounter_type_id = 255;
}

bool ActivityManager::applyPolicyOverride(PersonLocation& loc, Person& person,
                                          int16_t activity, VenueId venue,
                                          SubsetIndex subset,
                                          SlotVenueType slot_venue_type,
                                          int time_slot_index) {
  if (policy_manager_ == nullptr) return false;
  auto override = policy_manager_->getOverride(
      person, activity, venue, subset, slot_venue_type,
      current_simulation_time_, time_slot_index);
  if (!override.has_value()) return false;
  loc = override.value();
  return true;
}

void ActivityManager::setResidenceOrNoneLocation(PersonLocation& loc,
                                                 const Person& person) {
  auto residence = world_.getActivityVenues(person, residence_act_idx_);
  if (!residence.empty()) {
    auto [venue_id, subset_idx] = residence[0];
    loc.venue_id = venue_id;
    loc.subset_index = subset_idx;
    loc.activity_index = residence_act_idx_;
  } else {
    loc.venue_id = -1;
    loc.subset_index = -1;
    loc.activity_index = none_act_idx_;
  }
  loc.encounter_type_id = 255;
}

void ActivityManager::ensureIndicesCached() {
  if (dead_act_idx_ == -1) {
    dead_act_idx_ = static_cast<int16_t>(world_.getActivityIndex("dead"));
    residence_act_idx_ =
        static_cast<int16_t>(world_.getActivityIndex("residence"));
    none_act_idx_ = static_cast<int16_t>(world_.getActivityIndex("none"));
    no_venue_act_idx_ =
        static_cast<int16_t>(world_.getActivityIndex("no_venue"));
  }
}

void ActivityManager::initializeLocations(
    std::vector<PersonLocation>& locations) {
  ensureIndicesCached();
  locations.resize(world_.people.size());

  for (size_t i = 0; i < world_.people.size(); ++i) {
    const Person& p = world_.people[i];
    locations[i].person_id = p.id;

    // Skip dead people - they don't have a location
    if (p.is_dead) {
      setDeadLocation(locations[i]);
      continue;
    }

    // Find their residence (using cached index)
    setResidenceOrNoneLocation(locations[i], p);
    locations[i].person_array_index = i;
  }
}

std::tuple<int16_t, VenueId, SubsetIndex> ActivityManager::resolveHopSlot(
    const Person& person, const ScheduleType& hopped, int16_t k,
    int hop_start_day) {
  const int16_t n = static_cast<int16_t>(hopped.flat_slots.size());
  const int16_t s = k % n;
  const TimeSlot& slot = hopped.flat_slots[s];
  const int day_type_idx =
      config_.schedule.getDayTypeIndex(hop_start_day + k / n);
  const uint64_t hop_key =
      mix_seed(base_seed_, person.id, static_cast<uint64_t>(s));
  const int16_t act =
      selectActivity(person, slot, s, &hopped, day_type_idx, hop_key);
  auto [v, sub] =
      selectVenue(person, act, slot, hop_key, hop_start_day + k / n);
  return {act, v, sub};
}

bool ActivityManager::advanceHoppedSchedule(Person& person, PersonLocation& loc,
                                            size_t person_array_idx) {
  const ScheduleType& hopped =
      config_.schedule.schedule_types[person.schedule_hop.hopped_schedule_id];
  if (!hopped.is_temporary) {
    return false;
  }

  // temp_slot_progress is kept monotonically increasing across day-boundary
  // wraps so that findLastNonNullVenueOnHop can scan back through previous
  // repeats via (k % n).  This slot runs before the increment, so its absolute
  // index is temp_slot_progress; integer division by n gives its hop-day, hence
  // hop_start_day = current_sim_day_ - temp_slot_progress / n.
  const int16_t n = static_cast<int16_t>(hopped.flat_slots.size());
  const int hop_start_day = ScheduleHop::hopStartDay(
      current_sim_day_, n, person.schedule_hop.temp_slot_progress);
  auto [act, v, s] = resolveHopSlot(
      person, hopped, person.schedule_hop.temp_slot_progress, hop_start_day);
  loc.venue_id = v;
  loc.subset_index = s;
  loc.activity_index = act;
  loc.encounter_type_id = 255;
  loc.person_id = person.id;
  loc.person_array_index = person_array_idx;

  // Advance one slot; auto-return when a full cycle completes with no repeats.
  if (person.schedule_hop.advanceAndCheckComplete(n)) {
    int16_t return_to = person.schedule_hop.effectiveReturnSchedule(
        static_cast<int16_t>(person.schedule_type_id));
    person.cached_schedule_type_ = &config_.schedule.schedule_types[return_to];
    person.schedule_hop.clear();
  }
  return true;
}

void ActivityManager::assignActivities(const TimeSlot& slot, int day_type_idx,
                                       std::vector<PersonLocation>& locations) {
  ensureIndicesCached();
  // Assign each person to an activity
  for (size_t i = 0; i < world_.people.size(); ++i) {
    const Person& person = world_.people[i];

    // Hopped person: bypass normal slot
    if (person.schedule_hop.isActive()) {
      assignHoppedSingleSlot(person, i, slot, day_type_idx, locations);
      continue;
    }

    // Skip dead people - they don't move
    if (person.is_dead) {
      setDeadLocation(locations[i]);
      continue;
    }

    assignSingleSlotForLivePerson(person, i, slot, day_type_idx, locations);
  }
}

void ActivityManager::assignSingleSlotForLivePerson(
    const Person& person, size_t person_array_idx, const TimeSlot& slot,
    int day_type_idx, std::vector<PersonLocation>& locations) {
  const size_t i = person_array_idx;
  // Get person's schedule type
  const ScheduleType* schedule_type = person.cached_schedule_type_;
  if (!schedule_type) {
    schedule_type = config_.schedule.getScheduleTypeForPerson(person);
    const_cast<Person&>(person).cached_schedule_type_ = schedule_type;
  }

  if (!schedule_type) {
    std::cerr << "ERROR: No schedule type for person " << person.id
              << std::endl;
    return;
  }

  // Select activity for this person (returns int16_t index)
  uint64_t time_key = static_cast<uint64_t>(current_simulation_time_ * 1000);
  int16_t scheduled_activity_index =
      selectActivity(person, slot, -1, schedule_type, day_type_idx, time_key);

  // Select specific venue for that activity
  auto [venue_id, subset_idx] =
      selectVenue(person, scheduled_activity_index, slot, time_key);

  // Update location
  locations[i].venue_id = venue_id;
  locations[i].subset_index = subset_idx;
  locations[i].activity_index = scheduled_activity_index;
  locations[i].encounter_type_id = 255;  // Default for normal activities
  locations[i].person_id = person.id;
  locations[i].person_array_index = i;

  // Check for policy overrides (symptom-based, lockdowns, etc.)
  if (applyPolicyOverride(
          locations[i], const_cast<Person&>(person), scheduled_activity_index,
          locations[i].venue_id, locations[i].subset_index,
          SlotVenueType::fromVenue(locations[i].venue_id), -1)) {
    locations[i].person_id = person.id;
    locations[i].person_array_index = i;
  }
}

void ActivityManager::precomputeSchedules() {
  if (!config_.performance.precompute_schedules) {
    std::cout << "Schedule pre-computation disabled by configuration"
              << std::endl;
    return;
  }

  int num_day_types = static_cast<int>(config_.schedule.day_type_names.size());
  world_.num_day_types = static_cast<size_t>(num_day_types);
  world_.precomputed_schedules.resize(num_day_types);
  world_.schedule_starts.assign(world_.people.size() * num_day_types, 0);
  world_.schedule_counts.assign(world_.people.size() * num_day_types, 0);

  for (size_t person_idx = 0; person_idx < world_.people.size(); ++person_idx) {
    auto& person = world_.people[person_idx];
    if (person.is_dead) continue;

    // Use cached schedule type pointer if available
    const ScheduleType* schedule_type = person.cached_schedule_type_;
    if (!schedule_type) {
      schedule_type = config_.schedule.getScheduleTypeForPerson(person);
      person.cached_schedule_type_ = schedule_type;
    }
    if (!schedule_type) {
      std::cerr << "ERROR: No schedule type found for person " << person.id
                << std::endl;
      continue;
    }

    for (int dt_idx = 0; dt_idx < num_day_types; ++dt_idx) {
      precomputePersonDayType(person, person_idx, dt_idx, num_day_types,
                              schedule_type);
    }

    person.schedule_computed = true;
  }
}

void ActivityManager::precomputePersonDayType(
    Person& person, size_t person_idx, int dt_idx, int num_day_types,
    const ScheduleType* schedule_type) {
  const std::vector<TimeSlot>* slots =
      (dt_idx < static_cast<int>(schedule_type->slots_by_day_type_idx.size()))
          ? schedule_type->slots_by_day_type_idx[dt_idx]
          : nullptr;
  if (!slots) return;

  auto& dt_schedules = world_.precomputed_schedules[dt_idx];
  uint32_t start = static_cast<uint32_t>(dt_schedules.size());
  world_.schedule_starts[person_idx * num_day_types + dt_idx] = start;
  world_.schedule_counts[person_idx * num_day_types + dt_idx] =
      static_cast<uint16_t>(slots->size());

  for (size_t slot_idx = 0; slot_idx < slots->size(); ++slot_idx) {
    const auto& slot = (*slots)[slot_idx];
    uint64_t precomp_key = mix_seed(0xBE00ULL, slot_idx, dt_idx);
    precomputeOneSlot(person, slot, slot_idx, dt_idx, schedule_type,
                      precomp_key, dt_schedules);
  }
}

void ActivityManager::assignActivitiesFromSchedule(
    int time_slot_index, int day_type_idx,
    std::vector<PersonLocation>& locations) {
  ensureIndicesCached();

  // Compute time_key for per-person deterministic RNG
  uint64_t time_key =
      mix_seed(static_cast<uint64_t>(current_simulation_time_ * 1000),
               time_slot_index, static_cast<uint64_t>(day_type_idx));

  for (size_t i = 0; i < world_.people.size(); ++i) {
    Person& person = world_.people[i];  // Non-const for potential modifications

    // Hopped person: bypass precomputed schedule
    if (person.schedule_hop.isActive()) {
      assignHoppedScheduleSlot(person, i, time_slot_index, day_type_idx,
                               time_key, locations);
      continue;
    }

    // Skip dead people
    if (person.is_dead) {
      setDeadLocation(locations[i]);
      locations[i].person_id = person.id;
      continue;
    }

    // Fallback to residence if schedule not computed
    if (!person.schedule_computed) {
      setResidenceOrNoneLocation(locations[i], person);
      locations[i].person_id = person.id;
      continue;
    }

    assignFromPrecomputedSchedule(i, person, time_slot_index, day_type_idx,
                                  time_key, locations);
  }
}

void ActivityManager::assignFromPrecomputedSchedule(
    size_t person_array_idx, Person& person, int time_slot_index,
    int day_type_idx, uint64_t time_key,
    std::vector<PersonLocation>& locations) {
  const size_t i = person_array_idx;
  auto schedule = world_.getSchedule(person, day_type_idx);

  if (time_slot_index >= 0 &&
      time_slot_index < static_cast<int>(schedule.size())) {
    resolveAndWriteValidScheduleSlot(i, person, schedule[time_slot_index],
                                     time_slot_index, day_type_idx, time_key,
                                     locations);
  } else {
    // Fallback to residence if slot index invalid
    setResidenceOrNoneLocation(locations[i], person);
  }

  locations[i].person_id = person.id;
  locations[i].person_array_index = i;
}

void ActivityManager::resolveAndWriteValidScheduleSlot(
    size_t person_array_idx, Person& person, const ScheduleEntry& entry,
    int time_slot_index, int day_type_idx, uint64_t time_key,
    std::vector<PersonLocation>& locations) {
  const size_t i = person_array_idx;
  // Get the current time slot definition for specified_activity
  const ScheduleType* schedule_type = person.cached_schedule_type_;
  const TimeSlot* current_slot =
      lookupCurrentSlot(schedule_type, day_type_idx, time_slot_index);

  // Determine scheduled activity
  VenueId scheduled_venue_id = entry.venue_id;
  SubsetIndex scheduled_subset_idx = entry.subset_index;
  int16_t scheduled_activity_index = entry.activity_index;

  if (!entry.is_deterministic) {
    const ScheduleType* sched_type = schedule_type;
    resolveStochasticEntry(person, entry, time_slot_index, day_type_idx,
                           time_key, sched_type, current_slot,
                           scheduled_activity_index, scheduled_venue_id,
                           scheduled_subset_idx);
  }

  // Check for schedule hop trigger
  maybeTriggerScheduleHop(person, current_slot, day_type_idx, time_key,
                          scheduled_activity_index, scheduled_venue_id,
                          scheduled_subset_idx);

  // Check for policy overrides (symptom-based, lockdowns, etc.)
  if (applyPolicyOverride(locations[i], person, scheduled_activity_index,
                          scheduled_venue_id, scheduled_subset_idx,
                          SlotVenueType::fromVenue(scheduled_venue_id),
                          time_slot_index)) {
    return;
  }

  // SUCCESS: Assign pre-computed or runtime-selected activity/venue
  locations[i].venue_id = scheduled_venue_id;
  locations[i].subset_index = scheduled_subset_idx;
  locations[i].activity_index = scheduled_activity_index;
  locations[i].encounter_type_id = 255;
}

void ActivityManager::resolveHybridEntry(
    Person& person, const ScheduleEntry& entry, int time_slot_index,
    int day_type_idx, uint64_t time_key, const ScheduleType* sched_type,
    const TimeSlot& current_slot, int16_t& scheduled_activity_index,
    VenueId& scheduled_venue_id, SubsetIndex& scheduled_subset_idx) {
  // HYBRID: Re-evaluate participation, use precomputed venue if passed
  int16_t runtime_activity_idx =
      selectActivity(person, current_slot, time_slot_index, sched_type,
                     day_type_idx, time_key);

  if (runtime_activity_idx == scheduled_activity_index) {
    // Passed participation! Use precomputed venue
    scheduled_venue_id = entry.venue_id;
    scheduled_subset_idx = entry.subset_index;
    scheduled_activity_index = entry.activity_index;
  } else {
    // Failed participation or chose different activity
    auto [venue_id, subset_idx] =
        selectVenue(person, runtime_activity_idx, current_slot, time_key);
    scheduled_venue_id = venue_id;
    scheduled_subset_idx = subset_idx;
    scheduled_activity_index = runtime_activity_idx;
  }
}

void ActivityManager::resolveStochasticEntry(
    Person& person, const ScheduleEntry& entry, int time_slot_index,
    int day_type_idx, uint64_t time_key, const ScheduleType*& sched_type,
    const TimeSlot*& current_slot, int16_t& scheduled_activity_index,
    VenueId& scheduled_venue_id, SubsetIndex& scheduled_subset_idx) {
  // Check if this is a hybrid activity using bitmask (no string
  // comparison). Also honour the per-schedule force_hybrid_mask so the
  // cached venue is reused on participation pass.
  bool is_hybrid_entry =
      (entry.venue_id != -1) &&
      (config_.performance.isHybridIdx(scheduled_activity_index) ||
       (sched_type && scheduled_activity_index >= 0 &&
        (sched_type->force_hybrid_mask &
         (ActivityMask(1) << scheduled_activity_index)) != 0));

  // Ensure we have valid slot/schedule_type pointers
  if (!sched_type) {
    sched_type = config_.schedule.getScheduleTypeForPerson(person);
    person.cached_schedule_type_ = sched_type;
  }
  if (!current_slot) {
    current_slot = lookupCurrentSlot(sched_type, day_type_idx, time_slot_index);
  }

  if (is_hybrid_entry) {
    resolveHybridEntry(person, entry, time_slot_index, day_type_idx, time_key,
                       sched_type, *current_slot, scheduled_activity_index,
                       scheduled_venue_id, scheduled_subset_idx);
  } else {
    // FULLY STOCHASTIC: select both activity and venue at runtime
    int16_t runtime_activity_idx =
        selectActivity(person, *current_slot, time_slot_index, sched_type,
                       day_type_idx, time_key);
    auto [venue_id, subset_idx] =
        selectVenue(person, runtime_activity_idx, *current_slot, time_key);
    scheduled_venue_id = venue_id;
    scheduled_subset_idx = subset_idx;
    scheduled_activity_index = runtime_activity_idx;
  }
}

void ActivityManager::maybeTriggerScheduleHop(
    Person& person, const TimeSlot* current_slot, int day_type_idx,
    uint64_t time_key, int16_t& scheduled_activity_index,
    VenueId& scheduled_venue_id, SubsetIndex& scheduled_subset_idx) {
  int16_t hop_idx = -1;
  if (current_slot && scheduled_activity_index >= 0 &&
      scheduled_activity_index <
          static_cast<int16_t>(
              current_slot->hop_schedule_by_activity_idx.size())) {
    hop_idx =
        current_slot->hop_schedule_by_activity_idx[scheduled_activity_index];
  }
  // Generic property-dispatched hop: activity, property name, and schedule
  // name template are all specified in YAML hop_on_activity; no activity
  // names or property names are hard-coded here.
  if (hop_idx == -1 && current_slot) {
    hop_idx = resolvePropertyDispatchedHopIdx(person, *current_slot,
                                              scheduled_activity_index);
  }
  if (hop_idx == -1) return;

  const ScheduleType& target = config_.schedule.schedule_types[hop_idx];
  if (target.is_temporary && !target.flat_slots.empty()) {
    person.schedule_hop = ScheduleHop::begin(
        hop_idx, (target.return_schedule_idx != -1)
                     ? target.return_schedule_idx
                     : static_cast<int16_t>(person.schedule_type_id));
    // Immediate: slot N of old = slot 0 of new schedule
    const TimeSlot& slot0 = target.flat_slots[0];
    uint64_t hop_key = mix_seed(base_seed_, person.id, time_key);

    int16_t new_act =
        selectActivity(person, slot0, 0, &target, day_type_idx, hop_key);

    auto [v, s] = selectVenue(person, new_act, slot0, hop_key);
    scheduled_activity_index = new_act;
    scheduled_venue_id = v;
    scheduled_subset_idx = s;
    person.schedule_hop.consumeSlot0();
  } else {
    // Permanent hop: update schedule pointer, no auto-return
    person.schedule_hop.setPermanent(hop_idx);
    person.cached_schedule_type_ = &config_.schedule.schedule_types[hop_idx];
  }
}

void ActivityManager::precomputeOneSlot(
    Person& person, const TimeSlot& slot, size_t slot_idx, int dt_idx,
    const ScheduleType* schedule_type, uint64_t precomp_key,
    std::vector<ScheduleEntry>& dt_schedules) {
  int16_t act_idx = selectActivity(person, slot, slot_idx, schedule_type,
                                   dt_idx, precomp_key);

  bool is_det = config_.performance.isDeterministicIdx(act_idx, &slot);
  bool is_hyb = config_.performance.isHybridIdx(act_idx);

  // Per-schedule override: demote DETERMINISTIC -> HYBRID for activities
  // listed in this schedule type's force_hybrid_activities.
  if (is_det && act_idx >= 0 &&
      (schedule_type->force_hybrid_mask & (ActivityMask(1) << act_idx)) != 0) {
    is_det = false;
    is_hyb = true;
  }
  // Linked-activities coupling: if this slot's allowed activities
  // include any linked activity, force the entry to be re-rolled at
  // runtime regardless of which outcome precompute chose. Otherwise
  // the "skip" outcome (e.g. residence) would be stored
  // deterministically and the person would never re-roll on later
  // days, freezing their decision.
  if (is_det && schedule_type->linked_activities_mask != 0 &&
      (slot.allowed_activity_mask & schedule_type->linked_activities_mask) !=
          0) {
    is_det = false;
    is_hyb = true;
  }

  if (is_det) {
    auto [venue_id, subset_idx] =
        selectVenue(person, act_idx, slot, precomp_key);
    dt_schedules.emplace_back(act_idx, venue_id, subset_idx, true);
  } else if (is_hyb) {
    auto [venue_id, subset_idx] =
        selectVenue(person, act_idx, slot, precomp_key);
    dt_schedules.emplace_back(act_idx, venue_id, subset_idx, false);
  } else {
    dt_schedules.emplace_back(act_idx, -1, -1, false);
  }
}

int16_t ActivityManager::resolvePropertyDispatchedHopIdx(
    const Person& person, const TimeSlot& slot, int16_t activity_idx) const {
  auto dispatch_it =
      slot.property_hop_dispatch_by_activity_idx.find(activity_idx);
  if (dispatch_it == slot.property_hop_dispatch_by_activity_idx.end()) {
    return -1;
  }
  const auto& dispatch = dispatch_it->second;
  auto prop = world_.getPersonProperty(person, dispatch.property_name);
  if (!prop) return -1;
  int32_t value = getOr<int32_t>(*prop, 0);
  if (value <= 0) return -1;
  std::string sched_name = dispatch.schedule_name_template;
  const std::string placeholder = "{value}";
  size_t pos = sched_name.find(placeholder);
  if (pos != std::string::npos) {
    sched_name.replace(pos, placeholder.size(), std::to_string(value));
  }
  return static_cast<int16_t>(world_.getScheduleTypeIndex(sched_name));
}

const TimeSlot* ActivityManager::lookupCurrentSlot(
    const ScheduleType* schedule_type, int day_type_idx,
    int time_slot_index) const {
  if (!schedule_type) return nullptr;
  if (day_type_idx >=
      static_cast<int>(schedule_type->slots_by_day_type_idx.size())) {
    return nullptr;
  }
  if (schedule_type->slots_by_day_type_idx[day_type_idx] == nullptr) {
    return nullptr;
  }
  const auto& slots = *schedule_type->slots_by_day_type_idx[day_type_idx];
  if (time_slot_index < 0 ||
      time_slot_index >= static_cast<int>(slots.size())) {
    return nullptr;
  }
  return &slots[time_slot_index];
}

void ActivityManager::assignHoppedSingleSlot(
    const Person& person, size_t person_array_idx, const TimeSlot& slot,
    int day_type_idx, std::vector<PersonLocation>& locations) {
  const size_t i = person_array_idx;
  Person& mutable_person = const_cast<Person&>(person);
  const ScheduleType& hopped_sched =
      config_.schedule.schedule_types[person.schedule_hop.hopped_schedule_id];
  uint64_t time_key_hop =
      static_cast<uint64_t>(current_simulation_time_ * 1000);

  if (hopped_sched.is_temporary) {
    advanceHoppedSchedule(mutable_person, locations[i], i);
  } else {
    // Non-temporary hop: execute normal day-type slots
    if (day_type_idx <
            static_cast<int>(hopped_sched.slots_by_day_type_idx.size()) &&
        hopped_sched.slots_by_day_type_idx[day_type_idx] != nullptr) {
      // assignActivities is called with a single slot; use it directly
      int16_t act = selectActivity(person, slot, 0, &hopped_sched, day_type_idx,
                                   time_key_hop);
      auto [v, s] = selectVenue(person, act, slot, time_key_hop);
      locations[i].venue_id = v;
      locations[i].subset_index = s;
      locations[i].activity_index = act;
      locations[i].encounter_type_id = 255;
    }
  }

  if (policy_manager_ != nullptr) {
    // The substituted venue is a pin, not a gate key: a traveller in no_venue
    // transit occupies no venue this slot, whatever they are pinned to.
    const SlotVenueType slot_venue_type =
        SlotVenueType::fromVenue(locations[i].venue_id);
    VenueId effective_venue = locations[i].venue_id;
    SubsetIndex effective_subset = locations[i].subset_index;
    if (effective_venue < 0 && hopped_sched.is_temporary) {
      auto [lv, ls] = findLastNonNullVenueOnHop(mutable_person);
      effective_venue = lv;
      effective_subset = ls;
    }
    applyPolicyOverride(locations[i], mutable_person,
                        locations[i].activity_index, effective_venue,
                        effective_subset, slot_venue_type, -1);
  }

  locations[i].person_id = person.id;
  locations[i].person_array_index = i;
}

void ActivityManager::assignHoppedScheduleSlot(
    Person& person, size_t person_array_idx, int time_slot_index,
    int day_type_idx, uint64_t time_key,
    std::vector<PersonLocation>& locations) {
  const size_t i = person_array_idx;
  const ScheduleType& hopped_sched =
      config_.schedule.schedule_types[person.schedule_hop.hopped_schedule_id];

  if (hopped_sched.is_temporary) {
    advanceHoppedSchedule(person, locations[i], i);
  } else {
    // Non-temporary hop (e.g. freeze_in_place): execute normal day-type
    // slots
    if (day_type_idx <
            static_cast<int>(hopped_sched.slots_by_day_type_idx.size()) &&
        hopped_sched.slots_by_day_type_idx[day_type_idx] != nullptr) {
      const auto& slots = *hopped_sched.slots_by_day_type_idx[day_type_idx];
      if (time_slot_index >= 0 &&
          time_slot_index < static_cast<int>(slots.size())) {
        const TimeSlot& hop_slot = slots[time_slot_index];
        int16_t act = selectActivity(person, hop_slot, time_slot_index,
                                     &hopped_sched, day_type_idx, time_key);
        auto [v, s] = selectVenue(person, act, hop_slot, time_key);
        locations[i].venue_id = v;
        locations[i].subset_index = s;
        locations[i].activity_index = act;
        locations[i].encounter_type_id = 255;
      }
    }
  }

  // Apply policy overrides (e.g. sick traveller freeze / unfreeze)
  if (policy_manager_ != nullptr) {
    // Read before the substitution below: that venue is the pin, this is the
    // venue the person actually occupies this slot.
    const SlotVenueType slot_venue_type =
        SlotVenueType::fromVenue(locations[i].venue_id);
    VenueId effective_venue = locations[i].venue_id;
    SubsetIndex effective_subset = locations[i].subset_index;
    // If in transit (no_venue), resolve last real overnight venue so the
    // policy can pin the person there instead of at home
    if (effective_venue < 0 && hopped_sched.is_temporary) {
      auto [lv, ls] = findLastNonNullVenueOnHop(person);
      effective_venue = lv;
      effective_subset = ls;
    }
    applyPolicyOverride(locations[i], person, locations[i].activity_index,
                        effective_venue, effective_subset, slot_venue_type,
                        time_slot_index);
  }

  locations[i].person_id = person.id;
  locations[i].person_array_index = i;
}

std::pair<VenueId, SubsetIndex> ActivityManager::findLastNonNullVenueOnHop(
    const Person& person) {
  ensureIndicesCached();
  // temp_slot_progress is monotonically increasing (not reset on day-boundary
  // wrap).  k iterates backward through the sequence of executed slots; s = k
  // % n gives the actual flat_slots index and the deterministic key, so each
  // logical slot always hashes identically regardless of which repeat it falls
  // in.  When k crosses a day boundary (k < n), the modulo naturally reaches
  // back into the previous repeat's slots.
  const ScheduleType& hopped =
      config_.schedule.schedule_types[person.schedule_hop.hopped_schedule_id];
  const int16_t n = static_cast<int16_t>(hopped.flat_slots.size());
  // This runs after advanceHoppedSchedule has incremented temp_slot_progress,
  // so the no-venue transit slot that just executed was at absolute index
  // temp_slot_progress - 1; hence hop_start_day = current_sim_day_ -
  // (temp_slot_progress - 1) / n.  resolveHopSlot then derives each scanned
  // slot's day type from hop_start_day + k / n, matching the forward path.
  const int hop_start_day = ScheduleHop::hopStartDay(
      current_sim_day_, n,
      static_cast<int16_t>(person.schedule_hop.temp_slot_progress - 1));
  for (int16_t k = person.schedule_hop.temp_slot_progress - 2; k >= 0; --k) {
    auto [prev_act, v, sub] = resolveHopSlot(person, hopped, k, hop_start_day);
    if (v >= 0) return {v, sub};
  }
  // No prior overnight venue; fall back to home residence
  auto home = world_.getActivityVenues(person, residence_act_idx_);
  if (!home.empty()) return home[0];
  return {-1, -1};
}

}  // namespace june
