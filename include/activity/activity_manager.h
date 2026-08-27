#pragma once

#include <iostream>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "activity/venue_resolve_context.h"
#include "core/config.h"
#include "core/types.h"
#include "core/world_state.h"
#include "epidemiology/policy.h"  // SlotVenueType, passed by value below

#ifdef USE_MPI
#include <mpi.h>
#endif

namespace june {

class PolicyManager;
class CalendarEventManager;
class OnTheFlyVenueAllocator;

class ActivityManager {
 public:
  struct PerformanceStats {
    size_t map_allocations = 0;
    size_t string_lookups = 0;
    size_t weights_cached = 0;
    size_t rng_constructions = 0;

    void print() const {
      std::cout << "\n=== ActivityManager Performance Stats ===" << std::endl;
      std::cout << "  Map allocations:   " << map_allocations << std::endl;
      std::cout << "  String lookups:    " << string_lookups << std::endl;
      std::cout << "  Weights cached:    " << weights_cached << std::endl;
      std::cout << "  RNG constructions: " << rng_constructions << std::endl;
    }
  };

  ActivityManager(WorldState& world, const Config& config);

  // Assign schedule types to all people based on selection criteria
  void assignScheduleTypes();

  // Set policy manager (for runtime behavior overrides)
  void setPolicyManager(PolicyManager* policy_manager);

  // Set calendar-event manager (optional; drives calendar-triggered venue
  // resolution in selectVenue and active-event clearing on hop completion).
  void setCalendarEventManager(CalendarEventManager* calendar_event_manager);

  // Set on-the-fly venue allocator (optional; provides dynamic venue pools for
  // activities without a pre-baked mapping).
  void setOnTheFlyVenueAllocator(OnTheFlyVenueAllocator* allocator);

  // Set current simulation time (needed for policy checks)
  void setCurrentTime(double current_time) {
    current_simulation_time_ = current_time;
  }

  // Set current simulation day as an integer. Used by the hop path to derive
  // the day type of each hopped slot without floating-point drift from
  // current_simulation_time_ at day boundaries.
  void setCurrentDay(int sim_day) { current_sim_day_ = sim_day; }

  // Assign all people to activities for a given time slot
  void assignActivities(const TimeSlot& slot, int day_type_idx,
                        std::vector<PersonLocation>& locations);

  // Get performance stats
  PerformanceStats& getStats() { return stats_; }

  // Initialize locations (everyone starts at residence)
  void initializeLocations(std::vector<PersonLocation>& locations);

  // Pre-compute schedules for all people
  void precomputeSchedules();

  // Assign activities using pre-computed schedules
  void assignActivitiesFromSchedule(int time_slot_index, int day_type_idx,
                                    std::vector<PersonLocation>& locations);

  // Scans backwards through an already-hopped schedule's flat_slots to find
  // the last slot that produced a real venue (venue_id >= 0). Used to resolve
  // a pin venue when the policy fires during a no_venue transit slot.
  // Falls back to home residence if no prior overnight slot is found.
  std::pair<VenueId, SubsetIndex> findLastNonNullVenueOnHop(
      const Person& person);

 private:
  WorldState& world_;
  const Config& config_;

  // Policy manager (optional - nullptr if no policies)
  PolicyManager* policy_manager_ = nullptr;

  // Calendar-event manager (optional - nullptr if no calendar events)
  CalendarEventManager* calendar_event_manager_ = nullptr;

  // On-the-fly venue allocator (optional - nullptr if not configured)
  OnTheFlyVenueAllocator* on_the_fly_allocator_ = nullptr;

  // Current simulation time (for policy checks)
  double current_simulation_time_ = 0.0;

  // Current simulation day as an integer (set by Simulator::simulateDay). Used
  // to derive hopped-slot day types without fp drift.
  int current_sim_day_ = 0;

  // Select activity for a person based on time slot and participation rates
  // Returns activity index (int16_t) instead of string for performance
  int16_t selectActivity(const Person& person, const TimeSlot& slot,
                         size_t slot_idx, const ScheduleType* schedule_type,
                         int day_type_idx, uint64_t time_key);

  // Select specific venue/subset for an activity.
  // Not re-entrant: hierarchical sampling uses thread_local scratch buffers
  // shared across calls on the same thread, so calls must be sequential per
  // thread (safe across threads, not safe recursively/interleaved).
  std::pair<VenueId, SubsetIndex> selectVenue(
      const Person& person, int16_t activity_idx,
      const TimeSlot&
          slot,  // Used for specified_activity (to select specific venue index)
      uint64_t time_key, int logical_day);
  // Thin wrapper: forwards current_sim_day_ as logical_day.
  std::pair<VenueId, SubsetIndex> selectVenue(
      const Person& person, int16_t activity_idx, const TimeSlot& slot,
      uint64_t time_key);

  PerformanceStats stats_;

  // Caching
  int16_t dead_act_idx_ = -1;
  int16_t residence_act_idx_ = -1;
  int16_t none_act_idx_ = -1;
  int16_t no_venue_act_idx_ = -1;

  void ensureIndicesCached();

  // Marks a PersonLocation as belonging to a dead person. Does not touch
  // person_id / person_array_index, caller sets those if needed.
  void setDeadLocation(PersonLocation& loc) const;

  // Sets a PersonLocation to the person's residence if any, otherwise to
  // none_act_idx_. Used by fallback paths (uninitialised schedule, invalid
  // slot index). Does not touch person_id / person_array_index.
  void setResidenceOrNoneLocation(PersonLocation& loc, const Person& person);

  // Bridges to PolicyManager::getOverride. Returns true (with loc overwritten
  // by the override) iff policy_manager_ is set and yields an override.
  // Effective-venue resolution and post-override person_id/person_array_index
  // re-set are left to callers (they vary across the four call sites).
  //
  // venue/subset are the Pin Venue only. The Slot Venue Type is passed
  // separately because the hop call sites substitute the last overnight venue
  // into the pin for a traveller in transit, who occupies no venue at all.
  bool applyPolicyOverride(PersonLocation& loc, Person& person,
                           int16_t activity, VenueId venue, SubsetIndex subset,
                           SlotVenueType slot_venue_type,
                           int time_slot_index);

  // Returns the TimeSlot at time_slot_index for the given schedule type and
  // day type, or nullptr if any index is out of range / the pointer is null.
  const TimeSlot* lookupCurrentSlot(const ScheduleType* schedule_type,
                                    int day_type_idx,
                                    int time_slot_index) const;

  // Resolves a hop schedule index from current_slot's
  // property_hop_dispatch_by_activity_idx table for the given activity.
  // The schedule_name_template's "{value}" placeholder is filled with the
  // person property's int32 value. Returns -1 if no dispatch is configured,
  // the property is missing or non-positive, or the resulting schedule
  // name does not exist.
  int16_t resolvePropertyDispatchedHopIdx(const Person& person,
                                          const TimeSlot& slot,
                                          int16_t activity_idx) const;

  // Step 1 of selectVenue's hierarchical pick: groups `venues` by their
  // venue type id into the thread_local venues_by_id_buffer, tracks the
  // populated type ids in venue_type_ids_buffer, and collects cross-rank /
  // unknown-type venues into cross_rank_venues_buffer. All three buffers
  // are cleared first.
  void groupVenuesByType(
      std::span<const std::pair<VenueId, SubsetIndex>> venues);

  // Step 2 of selectVenue's hierarchical pick: picks a venue type id from
  // venue_type_ids_buffer weighted by activity preferences (filling
  // weights_buffer and cumulative_buffer as it goes). Caller must have
  // populated the buffers via groupVenuesByType and verified
  // !venue_type_ids_buffer.empty().
  uint8_t pickWeightedVenueType(const Person& person, int16_t activity_idx,
                                SplitMix64& rng);

  // For slots with a specified_activity that targets activity_idx, picks
  // a venue from `venues` (optionally filtered by the slot's specified
  // venue type id) at the slot's specified index. Returns nullopt if the
  // slot is not specified, the activity does not match, or the filtered
  // list is empty.
  std::optional<std::pair<VenueId, SubsetIndex>> tryPickSpecifiedVenue(
      const TimeSlot& slot, int16_t activity_idx,
      std::span<const std::pair<VenueId, SubsetIndex>> venues) const;

  // Walks `available_indices` in order and returns the first activity that
  // passes its participation roll. Linked activities consult the cached
  // per-(person, sim_day) decision instead of rolling. Returns
  // available_indices.back() as the default if no candidate passes.
  int16_t pickActivityByRate(const Person& person,
                             const ScheduleType& schedule_type,
                             const std::vector<double>& participation_by_id,
                             const std::vector<int16_t>& available_indices,
                             SplitMix64& rng) const;

  // Linear lookup over config_.schedule.schedule_types by name. Returns
  // a pointer to the first match, or nullptr if none is found.
  const ScheduleType* findScheduleTypeByName(const std::string& name) const;

  // Resolves and assigns a schedule type to one (alive) person:
  // preserves the schedule_type_id loaded from state (HDF5) if present
  // and just rewires the cached pointer, otherwise tries CSV-based
  // probabilistic assignment, falls back to YAML selection_criteria,
  // and finally to the configured default schedule type.
  void assignScheduleTypeForPerson(Person& person);

  // Pre-computes every slot for one (person, day_type) pair. Writes the
  // person's starts / counts into world_.schedule_starts /
  // schedule_counts and delegates each slot to precomputeOneSlot. No-op
  // if the schedule type has no slot vector for this day_type.
  void precomputePersonDayType(Person& person, size_t person_idx, int dt_idx,
                               int num_day_types,
                               const ScheduleType* schedule_type);

  // Pre-computes one (person, day-type, slot) entry. Picks the activity
  // via selectActivity against the per-slot precomp_key, classifies it
  // as deterministic / hybrid / fully-stochastic (honouring the
  // per-schedule force_hybrid_mask and the linked-activities-mask
  // re-roll override), picks the venue for the first two cases, and
  // appends a ScheduleEntry to dt_schedules.
  void precomputeOneSlot(Person& person, const TimeSlot& slot, size_t slot_idx,
                         int dt_idx, const ScheduleType* schedule_type,
                         uint64_t precomp_key,
                         std::vector<ScheduleEntry>& dt_schedules);

  // Fills `available` with the slot's allowed activity indices, restricted
  // to those the person actually has venues for. no_venue_act_idx_ and
  // property-dispatch activities are always retained (their venue check
  // happens later). Output is cleared at the start.
  void filterAvailableActivities(const Person& person, const TimeSlot& slot,
                                 std::vector<int16_t>& available) const;

  // Builds a VenueResolveContext for `person`: resident_geo_unit_id from the
  // person's geo_unit; hosting_geo_unit_id from the active calendar event (if
  // any). Cheap — just integer lookups.
  VenueResolveContext buildVenueResolveContext(const Person& person) const;

  // For schedules that opt into linked_activities and slots whose allowed
  // activities touch the linked set, rolls the per-day participation
  // decision once per (person, sim_day) and caches it on the Person.
  // No-op if the schedule has no linked activities, the slot doesn't
  // touch them, or the cached day already matches the current sim day.
  void maybeRollLinkedActivitiesDay(
      const Person& person, const TimeSlot& slot,
      const ScheduleType& schedule_type,
      const std::vector<double>& participation_by_id);

  // Inner step of assignFromPrecomputedSchedule for the "slot index is in
  // range" path. Resolves current_slot, re-evaluates stochastic/hybrid
  // entries, applies schedule hops, applies any policy override, and
  // writes the final activity/venue/subset/encounter_type fields. Returns
  // early when a policy override fired (caller still writes
  // person_id / person_array_index in its tail).
  void resolveAndWriteValidScheduleSlot(size_t person_array_idx, Person& person,
                                        const ScheduleEntry& entry,
                                        int time_slot_index, int day_type_idx,
                                        uint64_t time_key,
                                        std::vector<PersonLocation>& locations);

  // Handles a person with a precomputed schedule: looks up the
  // ScheduleEntry at time_slot_index, re-evaluates stochastic/hybrid
  // entries, applies any schedule hop, applies any policy override, and
  // writes the final PersonLocation. Falls back to residence-or-none if
  // time_slot_index is out of range for this day's schedule.
  void assignFromPrecomputedSchedule(size_t person_array_idx, Person& person,
                                     int time_slot_index, int day_type_idx,
                                     uint64_t time_key,
                                     std::vector<PersonLocation>& locations);

  // Checks current_slot for a schedule-hop trigger on the chosen activity
  // (first via the static hop_schedule_by_activity_idx table, then via the
  // YAML property-dispatched fallback). If a hop target is found, mutates
  // person.schedule_hop and cached_schedule_type_ as required, and for
  // temporary hops rolls RNG
  // for slot 0 of the target and updates scheduled_* outputs.
  void maybeTriggerScheduleHop(Person& person, const TimeSlot* current_slot,
                               int day_type_idx, uint64_t time_key,
                               int16_t& scheduled_activity_index,
                               VenueId& scheduled_venue_id,
                               SubsetIndex& scheduled_subset_idx);

  // Hybrid branch of resolveStochasticEntry: re-rolls participation via
  // selectActivity. If the runtime activity matches the precomputed
  // scheduled_activity_index (participation passed), reuses the
  // precomputed venue from `entry`. Otherwise re-picks both activity
  // and venue.
  void resolveHybridEntry(Person& person, const ScheduleEntry& entry,
                          int time_slot_index, int day_type_idx,
                          uint64_t time_key, const ScheduleType* sched_type,
                          const TimeSlot& current_slot,
                          int16_t& scheduled_activity_index,
                          VenueId& scheduled_venue_id,
                          SubsetIndex& scheduled_subset_idx);

  // Re-evaluates a non-deterministic precomputed schedule entry at runtime.
  // For hybrid entries, re-rolls participation and reuses the precomputed
  // venue iff the same activity is chosen. For fully-stochastic entries,
  // picks both activity and venue freshly. Lazily backfills sched_type and
  // current_slot if either was null. Mutates the scheduled_* outputs in
  // place. Caller must have verified !entry.is_deterministic.
  void resolveStochasticEntry(
      Person& person, const ScheduleEntry& entry, int time_slot_index,
      int day_type_idx, uint64_t time_key, const ScheduleType*& sched_type,
      const TimeSlot*& current_slot, int16_t& scheduled_activity_index,
      VenueId& scheduled_venue_id, SubsetIndex& scheduled_subset_idx);

  // Handles the non-hopped, non-dead branch of assignActivities (single
  // slot form): resolves the person's schedule type (lazily caching),
  // picks an activity and venue against the caller-supplied slot, applies
  // any policy override, and writes the final PersonLocation. Skips the
  // person and logs an error if no schedule type can be resolved.
  void assignSingleSlotForLivePerson(const Person& person,
                                     size_t person_array_idx,
                                     const TimeSlot& slot, int day_type_idx,
                                     std::vector<PersonLocation>& locations);

  // Handles the hopped-schedule branch of assignActivities (single slot
  // form): runs advanceHoppedSchedule for temporary hops or executes the
  // non-temporary day-type slot directly against `slot`, applies any
  // policy override (with effective-venue resolution), and finalises
  // locations[person_array_idx]. The companion of
  // assignHoppedScheduleSlot, kept separate because the single-slot form
  // uses the caller's slot rather than looking one up from
  // slots_by_day_type_idx[time_slot_index].
  void assignHoppedSingleSlot(const Person& person, size_t person_array_idx,
                              const TimeSlot& slot, int day_type_idx,
                              std::vector<PersonLocation>& locations);

  // Handles the hopped-schedule branch of assignActivitiesFromSchedule:
  // dispatches to advanceHoppedSchedule (temporary) or the non-temporary
  // freeze-in-place day-type-slot path, then applies any policy override
  // and finalises locations[person_array_idx].
  void assignHoppedScheduleSlot(Person& person, size_t person_array_idx,
                                int time_slot_index, int day_type_idx,
                                uint64_t time_key,
                                std::vector<PersonLocation>& locations);

  // Base seed for deterministic per-entity RNG (MPI reproducibility)
  uint64_t base_seed_ = 0;

  // Single source of truth for resolving one hopped flat-slot into its
  // (activity, venue, subset) triple via mix_seed -> selectActivity ->
  // selectVenue.  k is the monotonic (non-modular) slot index; the modular
  // index s = k % n drives both slot access and the deterministic key, so the
  // same logical slot always hashes identically.  hop_start_day is the sim day
  // the hop began; the slot's day type is derived from hop_start_day + k / n.
  // Shared by advanceHoppedSchedule (forward) and findLastNonNullVenueOnHop
  // (backward) so the two can never diverge.
  std::tuple<int16_t, VenueId, SubsetIndex> resolveHopSlot(
      const Person& person, const ScheduleType& hopped, int16_t k,
      int hop_start_day);

  // Handles a person who is currently on a hopped (temporary) schedule.
  // Assigns from flat_slots[schedule_hop.temp_slot_progress] and advances it.
  // Auto-returns the person when all flat_slots are exhausted.
  // Returns true if the person was handled (caller should continue the loop).
  bool advanceHoppedSchedule(Person& person, PersonLocation& loc,
                             size_t person_array_idx);

};

}  // namespace june
