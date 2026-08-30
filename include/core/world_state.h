#pragma once

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "epidemiology/disease.h"
#include "types.h"

namespace june {

// =============================================================================
// WorldState - Main container for the simulation world
// =============================================================================
class WorldState {
 public:
  // Data storage (Structure of Arrays style internally, but exposed as objects)
  std::vector<Person> people;
  std::vector<Venue> venues;
  std::vector<GeographicalUnit> geo_units;

  // Registries
  std::vector<std::string> activity_names;
  std::vector<std::string> venue_type_names;
  std::vector<std::string> subset_type_names;
  std::vector<std::string> network_names;
  std::vector<std::string> schedule_type_names;
  std::vector<std::string> encounter_type_names;
  std::vector<std::string> symptom_names;
  std::vector<std::string> geo_level_names;

  // Property registries
  std::vector<std::string> person_property_names;
  std::unordered_map<std::string, std::vector<std::string>>
      person_property_value_registries;

  std::vector<std::string> venue_property_names;
  std::unordered_map<std::string, std::vector<std::string>>
      venue_property_value_registries;

  std::vector<std::string> geo_unit_property_names;
  std::unordered_map<std::string, std::vector<std::string>>
      geo_unit_property_value_registries;

  // Helper to get indices from registries
  int getActivityIndex(const std::string& name) const {
    auto it = std::find(activity_names.begin(), activity_names.end(), name);
    int idx = (it != activity_names.end())
                  ? static_cast<int>(std::distance(activity_names.begin(), it))
                  : -1;
    return idx;
  }

  int getVenueTypeIndex(const std::string& name) const {
    auto it = std::find(venue_type_names.begin(), venue_type_names.end(), name);
    return (it != venue_type_names.end())
               ? static_cast<int>(std::distance(venue_type_names.begin(), it))
               : -1;
  }

  int getSubsetTypeIndex(const std::string& name) const {
    auto it =
        std::find(subset_type_names.begin(), subset_type_names.end(), name);
    return (it != subset_type_names.end())
               ? static_cast<int>(std::distance(subset_type_names.begin(), it))
               : -1;
  }

  int getNetworkTypeIndex(const std::string& name) const {
    auto it = std::find(network_names.begin(), network_names.end(), name);
    return (it != network_names.end())
               ? static_cast<int>(std::distance(network_names.begin(), it))
               : -1;
  }

  int getScheduleTypeIndex(const std::string& name) const {
    auto it =
        std::find(schedule_type_names.begin(), schedule_type_names.end(), name);
    return (it != schedule_type_names.end())
               ? static_cast<int>(
                     std::distance(schedule_type_names.begin(), it))
               : -1;
  }

  int getEncounterTypeIndex(const std::string& name) const {
    auto it = std::find(encounter_type_names.begin(),
                        encounter_type_names.end(), name);
    return (it != encounter_type_names.end())
               ? static_cast<int>(
                     std::distance(encounter_type_names.begin(), it))
               : -1;
  }

  int getPersonPropertyIndex(const std::string& name) const {
    auto it = std::find(person_property_names.begin(),
                        person_property_names.end(), name);
    return (it != person_property_names.end())
               ? static_cast<int>(
                     std::distance(person_property_names.begin(), it))
               : -1;
  }

  int getVenuePropertyIndex(const std::string& name) const {
    auto it = std::find(venue_property_names.begin(),
                        venue_property_names.end(), name);
    return (it != venue_property_names.end())
               ? static_cast<int>(
                     std::distance(venue_property_names.begin(), it))
               : -1;
  }

  int getGeoUnitPropertyIndex(const std::string& name) const {
    auto it = std::find(geo_unit_property_names.begin(),
                        geo_unit_property_names.end(), name);
    return (it != geo_unit_property_names.end())
               ? static_cast<int>(
                     std::distance(geo_unit_property_names.begin(), it))
               : -1;
  }

  // GLOBAL FLAT STORAGE
  // Networks: person.network_meta_start -> network_meta -> network_partners
  std::vector<Person::NetworkMeta> network_meta;
  std::vector<PersonId> network_partners;

  // Activities: person.activity_meta_start -> activity_meta -> activity_venues
  std::vector<Person::ActivityMeta> activity_meta;
  std::vector<std::pair<VenueId, SubsetIndex>> activity_venues;

  // Optional per-(person, venue) membership metadata (Design B side-table,
  // /activity_mappings/membership_metadata in HDF5). Carries per-leg fields
  // such as boarding/alighting times for route activities. Keyed by flat
  // index into activity_venues. Sparse: most assignments carry no metadata.
  std::vector<std::string> membership_field_names;
  std::vector<std::unordered_map<uint32_t, float>> membership_field_values;

  int getMembershipFieldIndex(const std::string& name) const {
    auto it = std::find(membership_field_names.begin(),
                        membership_field_names.end(), name);
    return (it != membership_field_names.end())
               ? static_cast<int>(
                     std::distance(membership_field_names.begin(), it))
               : -1;
  }

  // Sentinel for "field absent for this membership". Matches the value MAY
  // writes when a (person, venue) row has no value for a given field.
  static constexpr float kMembershipFieldAbsent = -1.0f;

  float getMembershipField(uint32_t activity_venue_flat_idx,
                           int field_idx) const {
    if (field_idx < 0 ||
        field_idx >= static_cast<int>(membership_field_values.size()))
      return kMembershipFieldAbsent;
    const auto& m = membership_field_values[field_idx];
    auto it = m.find(activity_venue_flat_idx);
    return (it == m.end()) ? kMembershipFieldAbsent : it->second;
  }

  // Dynamic Properties: person.properties_start -> person_properties
  // Stores interned IDs for categorical properties and raw ints for others
  std::vector<int32_t> person_properties;

  // Pre-computed schedules: one vector per day type
  // schedule_starts[person_idx * num_day_types + dt_idx] -> start in
  // precomputed_schedules[dt_idx]
  std::vector<std::vector<ScheduleEntry>> precomputed_schedules;
  std::vector<uint32_t> schedule_starts;
  std::vector<uint16_t> schedule_counts;
  size_t num_day_types = 0;

  // Venues & Subsets
  std::vector<Subset> subsets;
  std::vector<PersonId> subset_members;
  std::vector<int32_t> venue_properties;
  std::vector<int32_t> geo_unit_properties;

  // Lookup maps
  std::unordered_map<PersonId, size_t> person_index;  // id -> index in people
  std::unordered_map<VenueId, size_t> venue_index;    // id -> index in venues
  std::unordered_map<GeoUnitId, size_t>
      geo_unit_index;  // id -> index in geo_units

  // Lookup maps by name/type
  std::unordered_map<std::string, std::vector<uint32_t>>
      venues_by_type;  // type -> indices

  // Global venue maps: cover ALL venues (local + cross-rank). The HDF5 loader
  // pre-populates these before buildIndices() at every rank count, serial
  // included; for a hand-built WorldState buildGlobalVenueMaps() fills the
  // geo-unit and by-type-name ones from world.venues during buildIndices().
  // Single source of truth — getVenuesInGeoUnit() uses only these maps so the
  // candidate pool is identical in serial and parallel runs.
  //
  // venue_type_by_id is indexed by VenueId and covers every Venue in the world,
  // not just this rank's, so that kUnknownVenueTypeId means "no such Venue" and
  // never "not mine". Ids naming no Venue are holes holding kUnknownVenueTypeId.
  // Empty only for a hand-built WorldState, where getVenue() is total.
  std::vector<uint8_t> venue_type_by_id;
  std::unordered_map<VenueId, GeoUnitId> global_venue_geo_unit_map;
  // type_name → sorted list of all VenueIds of that type (globally)
  std::unordered_map<std::string, std::vector<VenueId>>
      global_venues_by_type_name;

  // Helper: get venue type_id, falling back to the all-venue index for
  // cross-rank venues. Ids outside the index — past the end, a hole, or the
  // negative virtual-venue range — are unresolvable.
  uint8_t getVenueTypeId(VenueId id) const {
    const Venue* v = getVenue(id);
    if (v) return v->type_id;
    if (id < 0 || static_cast<size_t>(id) >= venue_type_by_id.size())
      return kUnknownVenueTypeId;
    return venue_type_by_id[static_cast<size_t>(id)];
  }

  // Record the type of `venue_id` in venue_type_by_id, growing it with
  // kUnknownVenueTypeId holes as needed. Negative ids name no Venue and are
  // ignored. For one-off writes; the loader sizes the index once instead.
  void setGlobalVenueType(VenueId venue_id, uint8_t type_id);

  // Geographic index: geo_unit_id -> indices of people in this unit AND all its
  // descendants
  std::unordered_map<GeoUnitId, std::vector<uint32_t>> people_by_geo_unit;

  // The units people are assigned to directly, i.e. the set of values of
  // Person::geo_unit_id. A strict subset of people_by_geo_unit's keys, which
  // also carry every ancestor of those units. Diagnostics that ask "would this
  // exclude anybody?" want this one.
  std::unordered_set<GeoUnitId> directly_inhabited_geo_units;

  // Build lookup indices (call after loading)
  void buildIndices();

  // Populate global_venue_geo_unit_map and global_venues_by_type_name from
  // world.venues. No-op if already filled by the MPI HDF5 loader.
  void buildGlobalVenueMaps();

  // Shared by WorldState::buildGlobalVenueMaps() and the MPI HDF5 loader's
  // detail::buildGlobalVenueMaps(): append venue_id to the type-name bucket,
  // falling back to "unknown" for an out-of-range type_id.
  void addVenueToTypeIndex(VenueId venue_id, uint8_t type_id);

  // Sort each global_venues_by_type_name bucket ascending by VenueId.
  void sortGlobalVenuesByTypeName();

  // Load susceptibility factors from CSV
  void loadRegionalRiskFactors(const std::string& csv_path);

  // Accessors
  Person* getPerson(PersonId id);
  const Person* getPerson(PersonId id) const;

  Venue* getVenue(VenueId id);
  const Venue* getVenue(VenueId id) const;

  GeographicalUnit* getGeoUnit(GeoUnitId id);
  const GeographicalUnit* getGeoUnit(GeoUnitId id) const;

  std::vector<Venue*> getVenuesByType(const std::string& type);

  // Return ids of all venues of `venue_type_name` located in
  // `hosting_geo_unit_id` or any of its descendants. Sorted by venue_id for
  // deterministic assignment.
  std::vector<VenueId> getVenuesInGeoUnit(
      GeoUnitId hosting_geo_unit_id, const std::string& venue_type_name) const;

  // Returns id of the nearest ancestor (or self) of `id` at `level_name`.
  // Returns -1 if not found.
  GeoUnitId ancestorAtLevel(GeoUnitId id, std::string_view level_name) const;

  // Free the two all-venue maps used only by getVenuesInGeoUnit, after the OTF
  // allocator has precomputed every pool it needs. Logs freed sizes.
  // venue_type_by_id is kept for the whole run: cross-rank type lookups
  // happen on every step, and every Venue must stay nameable.
  void dropGlobalVenueMaps();

  // Get all people in a geographic unit (including descendants)
  std::vector<Person*> getPeopleInUnit(GeoUnitId id);
  std::vector<Person*> getPeopleInUnit(const std::string& level,
                                       const std::string& name);

  // Flat networks and activities
  std::span<const Person::NetworkMeta> getNetworkMetas(const Person& p) const {
    if (p.network_meta_count == 0) return {};
    if (p.network_meta_start >= network_meta.size() ||
        p.network_meta_start + p.network_meta_count > network_meta.size())
      return {};
    return std::span(network_meta.data() + p.network_meta_start,
                     p.network_meta_count);
  }

  std::span<const PersonId> getNetworkPartners(
      const Person::NetworkMeta& meta) const {
    if (meta.partner_count == 0) return {};
    if (meta.partner_start >= network_partners.size() ||
        meta.partner_start + meta.partner_count > network_partners.size())
      return {};
    return std::span(network_partners.data() + meta.partner_start,
                     meta.partner_count);
  }

  std::span<const PersonId> getNetworkPartners(
      const Person& p, const std::string& network_name) const {
    int type_id = getNetworkTypeIndex(network_name);
    if (type_id < 0) return {};
    for (const auto& meta : getNetworkMetas(p)) {
      if (meta.network_type_id == (uint16_t)type_id)
        return getNetworkPartners(meta);
    }
    return {};
  }

  std::span<const PersonId> getNetworkPartners(const Person& p,
                                               int network_type_id) const {
    if (network_type_id < 0) return {};
    for (const auto& meta : getNetworkMetas(p)) {
      if (meta.network_type_id == (uint16_t)network_type_id)
        return getNetworkPartners(meta);
    }
    return {};
  }

  std::span<const Person::ActivityMeta> getActivityMetas(
      const Person& p) const {
    if (p.activity_meta_count == 0) return {};
    if (p.activity_meta_start >= activity_meta.size() ||
        p.activity_meta_start + p.activity_meta_count > activity_meta.size())
      return {};
    return std::span(activity_meta.data() + p.activity_meta_start,
                     p.activity_meta_count);
  }

  std::span<const std::pair<VenueId, SubsetIndex>> getActivityVenues(
      const Person::ActivityMeta& meta) const {
    if (meta.venue_count == 0) return {};
    if (meta.venue_start >= activity_venues.size() ||
        meta.venue_start + meta.venue_count > activity_venues.size())
      return {};
    return std::span(activity_venues.data() + meta.venue_start,
                     meta.venue_count);
  }

  // Venue/Subset accessors
  std::span<const Subset> getSubsets(const Venue& v) const {
    if (v.subset_count == 0) return {};
    if (v.subset_start >= subsets.size() ||
        v.subset_start + v.subset_count > subsets.size())
      return {};
    return std::span(subsets.data() + v.subset_start, v.subset_count);
  }

  std::span<const PersonId> getSubsetMembers(const Subset& s) const {
    if (s.member_count == 0) return {};
    if (s.member_start >= subset_members.size() ||
        s.member_start + s.member_count > subset_members.size())
      return {};
    return std::span(subset_members.data() + s.member_start, s.member_count);
  }

  std::span<const int32_t> getVenueProperties(const Venue& v) const {
    if (v.properties_count == 0) return {};
    if (v.properties_start >= venue_properties.size() ||
        v.properties_start + v.properties_count > venue_properties.size())
      return {};
    return std::span(venue_properties.data() + v.properties_start,
                     v.properties_count);
  }

  std::span<const std::pair<VenueId, SubsetIndex>> getActivityVenues(
      const Person& p, const std::string& act_name) const {
    int act_idx = getActivityIndex(act_name);
    return getActivityVenues(p, static_cast<int16_t>(act_idx));
  }

  std::span<const std::pair<VenueId, SubsetIndex>> getActivityVenues(
      const Person& p, int16_t act_idx) const {
    if (act_idx < 0) return {};
    for (const auto& meta : getActivityMetas(p)) {
      if (meta.activity_index == act_idx) return getActivityVenues(meta);
    }
    return {};
  }

  // Accessors
  std::span<const int32_t> getPersonProperties(const Person& p) const {
    if (p.properties_count == 0) return {};
    if (p.properties_start >= person_properties.size() ||
        p.properties_start + p.properties_count > person_properties.size())
      return {};
    return std::span(person_properties.data() + p.properties_start,
                     p.properties_count);
  }

  std::optional<PropertyValue> getPersonProperty(
      const Person& p, const std::string& name) const {
    int idx = getPersonPropertyIndex(name);
    if (idx < 0 || idx >= p.properties_count) return std::nullopt;
    size_t abs_idx = p.properties_start + idx;
    if (abs_idx >= person_properties.size()) return std::nullopt;

    int32_t raw_val = person_properties[abs_idx];
    if (raw_val == -1) return std::nullopt;  // monostate/null

    // Check if this property has a registry (categorical)
    auto it_reg = person_property_value_registries.find(name);
    if (it_reg != person_property_value_registries.end()) {
      if (raw_val >= 0 && (size_t)raw_val < it_reg->second.size()) {
        return it_reg->second[raw_val];
      }
    }
    return raw_val;  // return as int if no registry
  }

  std::span<const ScheduleEntry> getSchedule(const Person& p,
                                             int day_type_idx) const {
    if (num_day_types == 0 || day_type_idx < 0 ||
        day_type_idx >= static_cast<int>(num_day_types))
      return {};
    size_t person_idx = static_cast<size_t>(&p - people.data());
    size_t idx = person_idx * num_day_types + day_type_idx;
    if (idx >= schedule_starts.size()) return {};
    uint32_t start = schedule_starts[idx];
    uint16_t count = schedule_counts[idx];
    if (count == 0) return {};
    const auto& dt_schedules = precomputed_schedules[day_type_idx];
    if (start >= dt_schedules.size() || start + count > dt_schedules.size())
      return {};
    return std::span(dt_schedules.data() + start, count);
  }

  // Geo Units
  std::span<const int32_t> getGeoUnitProperties(
      const GeographicalUnit& gu) const {
    if (gu.properties_count == 0) return {};
    if (gu.properties_start >= geo_unit_properties.size() ||
        gu.properties_start + gu.properties_count > geo_unit_properties.size())
      return {};
    return std::span(geo_unit_properties.data() + gu.properties_start,
                     gu.properties_count);
  }

  std::optional<PropertyValue> getGeoUnitProperty(
      const GeographicalUnit& gu, const std::string& name) const {
    // Find index of property name in the global registry
    auto it = std::find(geo_unit_property_names.begin(),
                        geo_unit_property_names.end(), name);
    if (it == geo_unit_property_names.end()) return std::nullopt;

    int idx = std::distance(geo_unit_property_names.begin(), it);
    if (idx >= gu.properties_count) return std::nullopt;

    int32_t raw_val = geo_unit_properties[gu.properties_start + idx];
    if (raw_val == -1) return std::nullopt;

    auto it_reg = geo_unit_property_value_registries.find(name);
    if (it_reg != geo_unit_property_value_registries.end()) {
      if (raw_val >= 0 && (size_t)raw_val < it_reg->second.size()) {
        return it_reg->second[raw_val];
      }
    }
    return raw_val;
  }

  // Statistics
  size_t numPeople() const { return people.size(); }
  size_t numVenues() const { return venues.size(); }
  size_t numGeoUnits() const { return geo_units.size(); }

  void printSummary() const;
};

}  // namespace june
