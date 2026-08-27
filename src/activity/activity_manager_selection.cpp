#include <random>

#include "activity/activity_manager.h"
#include "activity/on_the_fly_venue_allocator.h"
#include "epidemiology/calendar_event.h"
#include "utils/deterministic_rng.h"
#include "utils/random.h"

namespace june {

namespace {
// Hierarchical venue-selection scratch buffers, shared by selectVenue,
// groupVenuesByType and pickWeightedVenueType. thread_local so concurrent
// calls from different threads don't corrupt each other's state; calls on
// the same thread must still be sequential (not re-entrant).
thread_local std::vector<std::vector<std::pair<VenueId, SubsetIndex>>>
    venues_by_id_buffer;
thread_local std::vector<uint8_t> venue_type_ids_buffer;
thread_local std::vector<double> weights_buffer;
thread_local std::vector<double> cumulative_buffer;
thread_local std::vector<std::pair<VenueId, SubsetIndex>>
    cross_rank_venues_buffer;
}  // namespace

int16_t ActivityManager::selectActivity(const Person& person,
                                        const TimeSlot& slot, size_t slot_idx,
                                        const ScheduleType* schedule_type,
                                        int day_type_idx, uint64_t time_key) {
  static thread_local std::vector<int16_t> available_indices;
  filterAvailableActivities(person, slot, available_indices);

  if (available_indices.empty()) {
    return residence_act_idx_;  // Fallback
  }

  // Per-person deterministic RNG for MPI reproducibility
  SplitMix64 rng(mix_seed(base_seed_, person.id, time_key));

  // Try each activity in order based on participation probability (using
  // pre-resolved vectors)
  const std::vector<double>* participation_ptr = nullptr;
  if (day_type_idx >= 0 &&
      day_type_idx < static_cast<int>(
                         schedule_type->participation_by_day_type_id.size())) {
    participation_ptr =
        &schedule_type->participation_by_day_type_id[day_type_idx];
  }
  static const std::vector<double> empty_participation;
  const auto& participation_by_id =
      participation_ptr ? *participation_ptr : empty_participation;

  maybeRollLinkedActivitiesDay(person, slot, *schedule_type,
                               participation_by_id);

  return pickActivityByRate(person, *schedule_type, participation_by_id,
                            available_indices, rng);
}

VenueResolveContext ActivityManager::buildVenueResolveContext(
    const Person& person) const {
  VenueResolveContext ctx;
  ctx.resident_geo_unit_id = person.geo_unit_id;
  if (calendar_event_manager_) {
    ctx.hosting_geo_unit_id =
        calendar_event_manager_->getActiveHostingGeoUnit(person.id);
  }
  return ctx;
}

void ActivityManager::filterAvailableActivities(
    const Person& person, const TimeSlot& slot,
    std::vector<int16_t>& available) const {
  available.clear();
  // Build resolve context lazily — only if OTF allocator is present and
  // a venue-less activity is encountered.
  bool context_built = false;
  VenueResolveContext ctx;
  for (int16_t act_idx : slot.allowed_activity_indices) {
    if (act_idx == no_venue_act_idx_ ||
        slot.property_hop_dispatch_by_activity_idx.count(act_idx)) {
      available.push_back(act_idx);
      continue;
    }
    if (!world_.getActivityVenues(person, act_idx).empty()) {
      available.push_back(act_idx);
      continue;
    }
    // No pre-baked venues: ask OTF allocator if it can supply a pool.
    if (on_the_fly_allocator_) {
      const std::string_view act_name =
          world_.activity_names[static_cast<size_t>(act_idx)];
      if (on_the_fly_allocator_->hasRule(act_name)) {
        if (!context_built) {
          ctx = buildVenueResolveContext(person);
          context_built = true;
        }
        if (!on_the_fly_allocator_->resolve(act_name, ctx, world_).empty()) {
          available.push_back(act_idx);
        }
      }
    }
  }
}

// Linked activities: if the schedule type opts in via `linked_activities`
// AND this slot's allowed activities touch the linked set, roll ONCE per
// (person, sim_day) and reuse the cached decision across every listed
// activity. Couples e.g. an outbound route, a primary activity at the
// destination, and a return route into a single per-day attendance
// decision. Slots that don't touch linked activities (e.g. weekend slots
// or non-routing slots) are unaffected.
void ActivityManager::maybeRollLinkedActivitiesDay(
    const Person& person, const TimeSlot& slot,
    const ScheduleType& schedule_type,
    const std::vector<double>& participation_by_id) {
  const bool slot_touches_linked =
      schedule_type.linked_activities_mask != 0 &&
      (slot.allowed_activity_mask & schedule_type.linked_activities_mask) != 0;
  if (!slot_touches_linked) return;
  int day_now = static_cast<int>(current_simulation_time_);
  if (person.linked_activities_day == day_now) return;
  // Use the rate of any linked activity from the participation table.
  // We deliberately do NOT filter by available_indices; the rate is a
  // schedule-level configuration that shouldn't depend on whether THIS
  // person happens to have the venue (e.g. a person without a route
  // leg should still re-roll the same attendance decision used by the
  // primary-activity slot later in the day). All linked activities
  // should be configured with the same rate.
  double linked_rate = 0.0;
  for (size_t a = 0; a < participation_by_id.size(); ++a) {
    if ((schedule_type.linked_activities_mask & (ActivityMask(1) << a)) != 0) {
      linked_rate = participation_by_id[a];
      break;
    }
  }
  SplitMix64 day_rng(
      mix_seed(base_seed_, person.id, mix_seed(0xDA17ULL, day_now, 0ULL)));
  std::uniform_real_distribution<double> day_dist(0.0, 1.0);
  bool pass = day_dist(day_rng) < linked_rate;
  const_cast<Person&>(person).linked_activities_day = day_now;
  const_cast<Person&>(person).linked_activities_pass = pass;
}

int16_t ActivityManager::pickActivityByRate(
    const Person& person, const ScheduleType& schedule_type,
    const std::vector<double>& participation_by_id,
    const std::vector<int16_t>& available_indices, SplitMix64& rng) const {
  for (int16_t act_idx : available_indices) {
    if (act_idx < 0) continue;

    // Linked activity: outcome already decided for today.
    if (schedule_type.linked_activities_mask != 0 &&
        (schedule_type.linked_activities_mask & (ActivityMask(1) << act_idx)) !=
            0) {
      if (person.linked_activities_pass) {
        return act_idx;  // attending today; pick this activity if available
      }
      continue;  // skipping today; fall through without consuming rng
    }

    if (act_idx < static_cast<int16_t>(participation_by_id.size())) {
      double rate = participation_by_id[act_idx];
      if (rate > 0.0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) < rate) {
          return act_idx;  // Participate in this activity
        }
      }
    }
  }

  // Default to last available activity (usually residence)
  return available_indices.back();
}

// Not re-entrant: hierarchical sampling below uses thread_local scratch
// buffers shared across calls on the same thread, so calls must be
// sequential per thread (safe across threads, not safe recursively or
// interleaved on the same thread).
std::pair<VenueId, SubsetIndex> ActivityManager::selectVenue(
    const Person& person, int16_t activity_idx, const TimeSlot& slot,
    uint64_t time_key) {
  return selectVenue(person, activity_idx, slot, time_key, current_sim_day_);
}

std::pair<VenueId, SubsetIndex> ActivityManager::selectVenue(
    const Person& person, int16_t activity_idx, const TimeSlot& slot,
    uint64_t time_key, int logical_day) {
  if (activity_idx == no_venue_act_idx_) {
    return {-1, -1};
  }
  auto venues = world_.getActivityVenues(person, activity_idx);
  if (venues.empty()) {
    if (on_the_fly_allocator_) {
      const std::string_view act_name =
          world_.activity_names[static_cast<size_t>(activity_idx)];
      VenueResolveContext ctx = buildVenueResolveContext(person);
      const auto& pool = on_the_fly_allocator_->resolve(act_name, ctx, world_);
      if (!pool.empty()) {
        uint64_t seed = on_the_fly_allocator_->isFixed(act_name)
                            ? mix_seed(base_seed_, person.id, activity_idx)
                            : mix_seed(base_seed_, person.id, activity_idx,
                                       static_cast<uint64_t>(logical_day));
        SplitMix64 rng(seed);
        std::uniform_int_distribution<size_t> dist(0, pool.size() - 1);
        return {pool[dist(rng)], 0};
      }
    }
    return {-1, -1};
  }

  // Check if this time slot specifies a particular activity and index
  if (auto specified = tryPickSpecifiedVenue(slot, activity_idx, venues)) {
    return *specified;
  }

  // A journey is not a choice between lines. Someone with a three-leg commute
  // rides all three, so drawing one of them at random would be picking a leg,
  // not a venue. Name the first leg and leave it there: the lines take their
  // passengers from the rider table, so this only has to be stable and the same
  // on every rank.
  const uint64_t pp_mask =
      config_.simulation.partial_presence.enabled_venue_type_mask;
  if (pp_mask != 0) {
    for (const auto& [venue_id, subset_idx] : venues) {
      const uint8_t vt = world_.getVenueTypeId(venue_id);
      if (vt < 64 && ((pp_mask >> vt) & 1ULL)) return {venue_id, subset_idx};
    }
  }

  // Per-person deterministic RNG for MPI reproducibility
  SplitMix64 rng(mix_seed(base_seed_, person.id, activity_idx, time_key));

  // Default: Hierarchical Selection (Category -> Venue)
  // 1. Group available venues by their category (type ID)
  groupVenuesByType(venues);

  if (venue_type_ids_buffer.empty()) {
    // No local venues found – fall back to cross-rank venues if any
    if (!cross_rank_venues_buffer.empty()) {
      std::uniform_int_distribution<size_t> dist(
          0, cross_rank_venues_buffer.size() - 1);
      return cross_rank_venues_buffer[dist(rng)];
    }
    return {-1, -1};
  }

  // 2. Select a category weighted by activity preferences
  uint8_t chosen_type_id = pickWeightedVenueType(person, activity_idx, rng);

  // 3. Select a specific venue within that category
  const auto& filtered_venues = venues_by_id_buffer[chosen_type_id];
  std::uniform_int_distribution<size_t> venue_dist(0,
                                                   filtered_venues.size() - 1);
  return filtered_venues[venue_dist(rng)];
}

std::optional<std::pair<VenueId, SubsetIndex>>
ActivityManager::tryPickSpecifiedVenue(
    const TimeSlot& slot, int16_t activity_idx,
    std::span<const std::pair<VenueId, SubsetIndex>> venues) const {
  if (!slot.specified_activity.has_value()) return std::nullopt;
  const auto& spec_act = slot.specified_activity.value();
  if (spec_act.cached_activity_idx != activity_idx) return std::nullopt;

  std::vector<std::pair<VenueId, SubsetIndex>> filtered_venues;
  if (spec_act.cached_venue_type_idx >= 0) {
    // Filter by pre-resolved venue type ID
    for (const auto& [venue_id, subset_idx] : venues) {
      uint8_t v_type_id = world_.getVenueTypeId(venue_id);
      if (v_type_id == spec_act.cached_venue_type_idx) {
        filtered_venues.push_back({venue_id, subset_idx});
      }
    }
  } else {
    filtered_venues.assign(venues.begin(), venues.end());
  }

  if (filtered_venues.empty()) return std::nullopt;
  if (spec_act.index >= 0 &&
      spec_act.index < static_cast<int>(filtered_venues.size())) {
    return filtered_venues[spec_act.index];
  }
  return filtered_venues.back();
}

void ActivityManager::groupVenuesByType(
    std::span<const std::pair<VenueId, SubsetIndex>> venues) {
  // Lazy first-use sizing: thread_local statics can't be sized from the
  // constructor, so size on first call per thread instead.
  if (venues_by_id_buffer.size() != world_.venue_type_names.size()) {
    venues_by_id_buffer.resize(world_.venue_type_names.size());
  }
  for (auto& buffer : venues_by_id_buffer) buffer.clear();
  venue_type_ids_buffer.clear();
  cross_rank_venues_buffer.clear();

  for (const auto& v_entry : venues) {
    // Use getVenueTypeId which works for both local and cross-rank venues
    // (falls back to venue_type_by_id for cross-rank). This ensures
    // the same hierarchical selection regardless of MPI partitioning.
    uint8_t v_type_id = world_.getVenueTypeId(v_entry.first);
    if (v_type_id == kUnknownVenueTypeId) {
      // Unknown venue type; collect for fallback
      cross_rank_venues_buffer.push_back(v_entry);
      continue;
    }

    if (v_type_id < venues_by_id_buffer.size()) {
      if (venues_by_id_buffer[v_type_id].empty()) {
        venue_type_ids_buffer.push_back(v_type_id);
      }
      venues_by_id_buffer[v_type_id].push_back(v_entry);
    }
  }
}

uint8_t ActivityManager::pickWeightedVenueType(const Person& person,
                                               int16_t activity_idx,
                                               SplitMix64& rng) {
  weights_buffer.clear();
  for (uint8_t type_id : venue_type_ids_buffer) {
    weights_buffer.push_back(config_.activity_preferences.getWeight(
        person, activity_idx, type_id, &world_));
    stats_.weights_cached++;
  }

  // Build cumulative weights and sample (replaces std::discrete_distribution
  // with same complexity, no per-call allocation).
  double total_w = buildCumulative(weights_buffer, cumulative_buffer);
  size_t chosen_idx = 0;
  if (total_w > 0.0) {
    int s = sampleFromCumulative(cumulative_buffer, rng);
    if (s >= 0) chosen_idx = static_cast<size_t>(s);
  }
  return venue_type_ids_buffer[chosen_idx];
}

}  // namespace june
