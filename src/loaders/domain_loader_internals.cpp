#include "loaders/domain_loader_internals.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <variant>

#include "core/config.h"
#include "core/world_state.h"
#include "loaders/hdf5_loader.h"

namespace june {
namespace detail {

namespace {

// Intern a PropertyValue against a per-property string registry and cache.
// Returns the interned code, or -1 for null/empty values and for strings
// that are network-encoded (starting with '[' or '{'); those are handled
// separately by parseNetworkPartnersFromString. Lazily seeds index_cache
// from registry on first use after a registry was populated elsewhere.
int32_t internPropertyValue(
    const PropertyValue& val, std::vector<std::string>& registry,
    std::unordered_map<std::string, int32_t>& index_cache) {
  if (std::holds_alternative<int32_t>(val)) return std::get<int32_t>(val);
  if (std::holds_alternative<double>(val))
    return static_cast<int32_t>(std::get<double>(val));
  if (std::holds_alternative<bool>(val)) return std::get<bool>(val) ? 1 : 0;
  if (!std::holds_alternative<std::string>(val)) return -1;

  const std::string& s = std::get<std::string>(val);
  if (s.empty()) return -1;
  if (s[0] == '[' || s[0] == '{') return -1;  // network-encoded; handled later

  if (index_cache.empty() && !registry.empty()) {
    for (size_t r_idx = 0; r_idx < registry.size(); ++r_idx) {
      index_cache[registry[r_idx]] = static_cast<int32_t>(r_idx);
    }
  }
  auto it = index_cache.find(s);
  if (it != index_cache.end()) return it->second;

  int32_t code = static_cast<int32_t>(registry.size());
  registry.push_back(s);
  index_cache[s] = code;
  return code;
}

// Parse a JSON-array-encoded list of integer partner IDs from `s` (e.g.
// "[1,2,3]") and append them to world.network_partners. Returns a populated
// NetworkMeta when at least one partner was parsed; returns std::nullopt
// when `s` is not a network-encoded string or yielded no partners.
std::optional<Person::NetworkMeta> parseNetworkPartnersFromString(
    WorldState& world, const std::string& s, uint16_t network_type_id) {
  if (s.empty() || (s[0] != '[' && s[0] != '{')) return std::nullopt;

  uint32_t partner_start = static_cast<uint32_t>(world.network_partners.size());
  int32_t current_id = 0;
  bool has_id = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      current_id = current_id * 10 + (c - '0');
      has_id = true;
    } else if (has_id) {
      world.network_partners.push_back(current_id);
      current_id = 0;
      has_id = false;
    }
  }
  if (has_id) world.network_partners.push_back(current_id);

  uint32_t partner_count =
      static_cast<uint32_t>(world.network_partners.size()) - partner_start;
  if (partner_count == 0) return std::nullopt;

  Person::NetworkMeta meta;
  meta.network_type_id = network_type_id;
  meta.partner_start = partner_start;
  meta.partner_count = partner_count;
  return meta;
}

// Batched per-type venue property load for one venue span: groups venues of
// the same type whose ranks_in_type form contiguous runs, reads each property
// column over those runs, and writes interned codes into world.venue_properties
// at the slots reserved by the caller during the venue emplacement loop.
void loadVenuePropertiesForSpan(
    HDF5Loader& loader, const std::vector<Venue>& span_venues,
    const std::unordered_map<uint8_t, std::vector<std::pair<size_t, int32_t>>>&
        venues_by_type_in_span,
    const std::unordered_map<std::string, std::vector<std::string>>&
        venue_type_prop_names,
    std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>&
        venue_property_indices_cache) {
  for (auto const& [type_id, venue_infos] : venues_by_type_in_span) {
    const std::string& type_name = loader.world_.venue_type_names[type_id];
    auto pn_it = venue_type_prop_names.find(type_name);
    if (pn_it == venue_type_prop_names.end()) continue;
    const auto& prop_names = pn_it->second;

    auto sorted_infos = venue_infos;
    std::sort(sorted_infos.begin(), sorted_infos.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    struct PropSpan {
      int32_t start_r;
      size_t count;
      std::vector<size_t> internal_indices;
    };
    std::vector<PropSpan> prop_spans;
    for (const auto& info : sorted_infos) {
      if (!prop_spans.empty() &&
          info.second == prop_spans.back().start_r +
                             static_cast<int32_t>(prop_spans.back().count)) {
        prop_spans.back().count++;
        prop_spans.back().internal_indices.push_back(info.first);
      } else {
        prop_spans.push_back({info.second, 1, {info.first}});
      }
    }

    for (size_t p_idx = 0; p_idx < prop_names.size(); ++p_idx) {
      const std::string& p_name = prop_names[p_idx];
      std::string p_path = "/venues/properties/" + type_name + "/" + p_name;

      for (const auto& span_p : prop_spans) {
        auto p_vals = loader.readPropertyDatasetRange(p_path, span_p.start_r,
                                                      span_p.count, p_name);
        for (size_t k = 0; k < span_p.count; ++k) {
          if (k >= p_vals.size()) continue;
          if (std::holds_alternative<std::monostate>(p_vals[k])) continue;

          int32_t interned_val = internPropertyValue(
              p_vals[k], loader.world_.venue_property_value_registries[p_name],
              venue_property_indices_cache[p_name]);

          size_t global_v_idx = span_p.internal_indices[k];
          uint32_t flat_idx =
              span_venues[global_v_idx].properties_start + p_idx;
          loader.world_.venue_properties[flat_idx] = interned_val;
        }
      }
    }
  }
}

}  // namespace

GeoPartitionMap buildPartitionMap(HDF5Loader& loader,
                                  const std::string& path_prefix) {
  auto gu_ids =
      loader.readNumericDataset<int32_t>(path_prefix + "/geo_unit_ids");
  auto starts =
      loader.readNumericDataset<int32_t>(path_prefix + "/start_indices");
  auto counts = loader.readNumericDataset<int32_t>(path_prefix + "/counts");

  GeoPartitionMap map;
  for (size_t i = 0; i < gu_ids.size(); ++i) {
    map[gu_ids[i]] = {static_cast<size_t>(starts[i]),
                      static_cast<size_t>(counts[i])};
  }
  return map;
}

std::vector<ChunkSpan> detectChunkSpans(
    const GeoPartitionMap& partition_map,
    const std::vector<GeoUnitId>& geo_units_vec, size_t start_idx,
    size_t end_idx) {
  std::vector<ChunkSpan> spans;
  for (size_t i = start_idx; i < end_idx; ++i) {
    GeoUnitId gu = geo_units_vec[i];
    auto it = partition_map.find(gu);
    if (it == partition_map.end()) continue;
    auto [start, count] = it->second;
    if (count == 0) continue;
    if (!spans.empty() && start == spans.back().start + spans.back().count) {
      spans.back().count += count;
      spans.back().gu_indices.push_back(i);
    } else {
      spans.push_back({start, count, {i}});
    }
  }
  return spans;
}

void registerSystemAndConfigActivities(WorldState& world,
                                       const Config& config) {
  auto register_activity = [&](const std::string& name) {
    if (!name.empty() &&
        std::find(world.activity_names.begin(), world.activity_names.end(),
                  name) == world.activity_names.end()) {
      world.activity_names.push_back(name);
    }
  };
  register_activity("dead");
  register_activity("none");
  register_activity("visiting");
  register_activity("no_venue");
  for (const auto& sched_type : config.schedule.schedule_types) {
    for (const auto& [dt_name, dt_slots] : sched_type.slots_by_day_type) {
      for (const auto& slot : dt_slots) {
        for (const auto& act : slot.allowed_activities) register_activity(act);
        for (const auto& act : slot.coordinated_only_activities)
          register_activity(act);
      }
    }
    for (const auto& slot : sched_type.flat_slots) {
      for (const auto& act : slot.allowed_activities) register_activity(act);
      for (const auto& act : slot.coordinated_only_activities)
        register_activity(act);
    }
  }
}

void syncScheduleTypeNames(WorldState& world, const Config& config) {
  for (const auto& st : config.schedule.schedule_types) {
    if (std::find(world.schedule_type_names.begin(),
                  world.schedule_type_names.end(),
                  st.name) == world.schedule_type_names.end()) {
      world.schedule_type_names.push_back(st.name);
    }
  }
}

std::unordered_map<std::string, std::vector<std::string>>
discoverVenuePropertyNames(HDF5Loader& loader) {
  std::unordered_map<std::string, std::vector<std::string>> result;
  const std::string base = "/venues/properties";
  if (!loader.groupExists(base)) return result;
  for (const auto& v_type : loader.getGroupNames(base)) {
    result[v_type] = loader.getDatasetNames(base + "/" + v_type);
  }
  return result;
}

void loadPersonsInSpan(
    HDF5Loader& loader, const ChunkSpan& span,
    const GeoPartitionMap& pop_partition_map,
    const std::vector<GeoUnitId>& geo_units_vec,
    const std::vector<std::string>& population_property_names,
    std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>&
        property_indices_cache) {
  auto chunk_ids = loader.readNumericDatasetRange<int32_t>(
      "/population/ids", span.start, span.count);
  auto chunk_ages = loader.readNumericDatasetRange<float>(
      "/population/ages", span.start, span.count);
  auto chunk_sexes = loader.readNumericDatasetRange<uint8_t>(
      "/population/sexes", span.start, span.count);

  std::vector<std::vector<PropertyValue>> chunk_property_columns;
  for (const auto& prop_name : population_property_names) {
    chunk_property_columns.push_back(
        loader.readPropertyDatasetRange("/population/properties/" + prop_name,
                                        span.start, span.count, prop_name));
  }

  size_t local_read_offset = 0;
  for (size_t gu_idx : span.gu_indices) {
    GeoUnitId geo_unit = geo_units_vec[gu_idx];
    size_t gu_count = pop_partition_map.at(geo_unit).second;

    for (size_t j = 0; j < gu_count; ++j) {
      Person p;
      size_t j_off = local_read_offset + j;
      p.id = chunk_ids[j_off];
      p.age = chunk_ages[j_off];
      p.sex = static_cast<Sex>(chunk_sexes[j_off]);
      p.geo_unit_id = geo_unit;

      // Dynamic properties (Phase 3: Flat Storage & Interning)
      p.properties_start =
          static_cast<uint32_t>(loader.world_.person_properties.size());
      p.properties_count = static_cast<uint8_t>(chunk_property_columns.size());
      for (size_t k = 0; k < chunk_property_columns.size(); ++k) {
        const auto& prop_name = population_property_names[k];
        const auto& prop_val = chunk_property_columns[k][j_off];
        loader.world_.person_properties.push_back(internPropertyValue(
            prop_val, loader.world_.person_property_value_registries[prop_name],
            property_indices_cache[prop_name]));
      }

      // Network parsing
      p.network_meta_start =
          static_cast<uint32_t>(loader.world_.network_meta.size());
      for (size_t k = 0; k < population_property_names.size(); ++k) {
        const auto& prop_val = chunk_property_columns[k][j_off];
        if (!std::holds_alternative<std::string>(prop_val)) continue;
        auto meta = parseNetworkPartnersFromString(
            loader.world_, std::get<std::string>(prop_val),
            static_cast<uint16_t>(k));
        if (!meta) continue;
        loader.world_.network_meta.push_back(*meta);
        p.network_meta_count++;
      }

      loader.world_.people.push_back(std::move(p));
    }
    local_read_offset += gu_count;
  }
}

void loadVenuesInSpan(
    HDF5Loader& loader, const ChunkSpan& span,
    const GeoPartitionMap& venue_partition_map,
    const std::vector<GeoUnitId>& geo_units_vec,
    const std::unordered_map<std::string, std::vector<std::string>>&
        venue_type_prop_names,
    std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>&
        venue_property_indices_cache) {
  auto chunk_ids = loader.readNumericDatasetRange<int32_t>(
      "/venues/ids", span.start, span.count);
  auto chunk_type_ids = loader.readNumericDatasetRange<uint8_t>(
      "/venues/types", span.start, span.count);
  auto chunk_ranks_in_type = loader.readNumericDatasetRange<int32_t>(
      "/venues/ranks_in_type", span.start, span.count);
  auto chunk_parent_ids = loader.readNumericDatasetRange<int32_t>(
      "/venues/parent_ids", span.start, span.count);

  std::vector<float> chunk_latitudes, chunk_longitudes;
  if (loader.datasetExists("/venues/latitudes")) {
    chunk_latitudes = loader.readNumericDatasetRange<float>(
        "/venues/latitudes", span.start, span.count);
    chunk_longitudes = loader.readNumericDatasetRange<float>(
        "/venues/longitudes", span.start, span.count);
  }

  std::vector<uint8_t> chunk_is_residence_raw;
  if (loader.datasetExists("/venues/is_residence")) {
    chunk_is_residence_raw = loader.readNumericDatasetRange<uint8_t>(
        "/venues/is_residence", span.start, span.count);
  }

  std::vector<Venue> span_venues;
  span_venues.reserve(span.count);
  std::unordered_map<uint8_t, std::vector<std::pair<size_t, int32_t>>>
      venues_by_type_in_span;

  size_t local_read_offset = 0;
  for (size_t gu_idx : span.gu_indices) {
    GeoUnitId geo_unit = geo_units_vec[gu_idx];
    size_t gu_count = venue_partition_map.at(geo_unit).second;

    for (size_t j = 0; j < gu_count; ++j) {
      size_t v_idx = span_venues.size();
      Venue v;
      size_t j_off = local_read_offset + j;
      v.id = chunk_ids[j_off];
      v.type_id = chunk_type_ids[j_off];
      v.geo_unit_id = geo_unit;
      v.parent_id = chunk_parent_ids[j_off];
      v.latitude = chunk_latitudes.empty() ? 0.0f : chunk_latitudes[j_off];
      v.longitude = chunk_longitudes.empty() ? 0.0f : chunk_longitudes[j_off];
      v.is_residence = chunk_is_residence_raw.empty()
                           ? false
                           : (chunk_is_residence_raw[j_off] != 0);

      const std::string& type_name = loader.world_.venue_type_names[v.type_id];
      auto pn_it = venue_type_prop_names.find(type_name);
      size_t prop_count =
          (pn_it == venue_type_prop_names.end()) ? 0 : pn_it->second.size();

      v.properties_start =
          static_cast<uint32_t>(loader.world_.venue_properties.size());
      v.properties_count = static_cast<uint16_t>(prop_count);

      // Pre-allocate space in flat vector
      loader.world_.venue_properties.resize(v.properties_start + prop_count,
                                            -1);

      int32_t global_rank = chunk_ranks_in_type[j_off];
      venues_by_type_in_span[v.type_id].push_back({v_idx, global_rank});
      span_venues.push_back(std::move(v));
    }
    local_read_offset += gu_count;
  }

  loadVenuePropertiesForSpan(loader, span_venues, venues_by_type_in_span,
                             venue_type_prop_names,
                             venue_property_indices_cache);

  for (auto& v : span_venues) loader.world_.venues.push_back(std::move(v));
}

void loadActivityMappingsInSpan(
    HDF5Loader& loader, const ChunkSpan& span,
    const std::unordered_map<PersonId, size_t>& local_person_idx_map) {
  auto span_activity_data = loader.read2DNumericDatasetRange<int32_t>(
      "/activity_mappings/activity_map/activity_data", span.start, span.count);

  Person* last_p = nullptr;
  int16_t last_act_idx = -1;

  for (size_t row_idx = 0; row_idx < span.count; ++row_idx) {
    const auto& row = span_activity_data[row_idx];
    PersonId pid = row[0];
    int16_t act_idx = static_cast<int16_t>(row[1]);

    auto it = local_person_idx_map.find(pid);
    if (it == local_person_idx_map.end()) continue;
    Person* p = &loader.world_.people[it->second];

    if (p != last_p) {
      p->activity_meta_start =
          static_cast<uint32_t>(loader.world_.activity_meta.size());
      p->activity_meta_count = 0;
      last_p = p;
      last_act_idx = -1;
    }

    if (act_idx != last_act_idx) {
      Person::ActivityMeta ameta;
      ameta.activity_index = act_idx;
      ameta.venue_start =
          static_cast<uint32_t>(loader.world_.activity_venues.size());
      ameta.venue_count = 0;
      loader.world_.activity_meta.push_back(ameta);
      p->activity_meta_count++;
      last_act_idx = act_idx;
    }

    loader.world_.activity_venues.push_back({row[2], row[3]});
    loader.world_.activity_meta.back().venue_count++;
  }
}

uint32_t matchMembershipRowToFlatIndex(const WorldState& world,
                                       const Person& person, VenueId venue_id,
                                       const SubsetIndex* subset_index) {
  for (const auto& meta : world.getActivityMetas(person)) {
    auto venues = world.getActivityVenues(meta);
    for (size_t k = 0; k < venues.size(); ++k) {
      if (venues[k].first != venue_id) continue;
      if (subset_index != nullptr && venues[k].second != *subset_index)
        continue;
      return meta.venue_start + static_cast<uint32_t>(k);
    }
  }
  return kAbsentFlatIndex;
}

void loadMembershipMetadata(
    HDF5Loader& loader,
    const std::unordered_map<PersonId, size_t>& local_person_idx_map) {
  if (!loader.groupExists("/activity_mappings/membership_metadata")) return;

  const std::string base = "/activity_mappings/membership_metadata";
  std::vector<std::string> field_names =
      loader.readStringDataset(base + "/field_names");
  auto pids = loader.readNumericDataset<int32_t>(base + "/person_ids");
  auto vids = loader.readNumericDataset<int32_t>(base + "/venue_ids");

  if (pids.size() != vids.size() || field_names.empty()) return;

  // subset_index disambiguates multiple Subsets a person holds at the same
  // Venue (e.g. two Feast accommodation memberships sharing one guest
  // house). Absent on worlds serialised before this column existed; fall
  // back to venue_id-only matching for those.
  std::vector<int32_t> sids;
  bool have_subset_indices = loader.datasetExists(base + "/subset_indices");
  if (have_subset_indices) {
    sids = loader.readNumericDataset<int32_t>(base + "/subset_indices");
    have_subset_indices = (sids.size() == pids.size());
  }

  loader.world_.membership_field_names = field_names;
  loader.world_.membership_field_values.assign(field_names.size(), {});

  // Locate each side-table row in this rank's activity_venues. Rows that
  // belong to a non-local person are kept as kAbsentFlatIndex and skipped
  // later.
  std::vector<uint32_t> row_flat_idx(pids.size(), kAbsentFlatIndex);
  for (size_t i = 0; i < pids.size(); ++i) {
    auto pit = local_person_idx_map.find(pids[i]);
    if (pit == local_person_idx_map.end()) continue;
    const Person& person = loader.world_.people[pit->second];
    const SubsetIndex* subset_index = have_subset_indices ? &sids[i] : nullptr;
    row_flat_idx[i] = matchMembershipRowToFlatIndex(loader.world_, person,
                                                    vids[i], subset_index);
  }

  for (size_t f = 0; f < field_names.size(); ++f) {
    auto vals = loader.readNumericDataset<float>(base + "/" + field_names[f]);
    if (vals.size() != pids.size()) continue;
    auto& sink = loader.world_.membership_field_values[f];
    sink.reserve(vals.size() / 4);
    for (size_t i = 0; i < vals.size(); ++i) {
      if (row_flat_idx[i] == kAbsentFlatIndex) continue;
      if (vals[i] == WorldState::kMembershipFieldAbsent) continue;
      sink[row_flat_idx[i]] = vals[i];
    }
  }
}

void loadVenueSubsets(HDF5Loader& loader,
                      const std::unordered_set<GeoUnitId>& owned_geo_units) {
  if (!loader.groupExists("/venues/subsets") ||
      !loader.groupExists("/venues/subsets/partition_index") ||
      !loader.groupExists("/venues/subsets/members_partition_index")) {
    return;
  }

  // Load both partition indexes
  auto subset_meta_geo_units = loader.readNumericDataset<int32_t>(
      "/venues/subsets/partition_index/geo_unit_ids");
  auto subset_meta_starts = loader.readNumericDataset<int32_t>(
      "/venues/subsets/partition_index/start_indices");
  auto subset_meta_counts = loader.readNumericDataset<int32_t>(
      "/venues/subsets/partition_index/counts");

  auto member_geo_units = loader.readNumericDataset<int32_t>(
      "/venues/subsets/members_partition_index/geo_unit_ids");
  auto member_starts = loader.readNumericDataset<int64_t>(
      "/venues/subsets/members_partition_index/start_indices");
  auto member_counts = loader.readNumericDataset<int32_t>(
      "/venues/subsets/members_partition_index/counts");

  // Local storage to gather subsets before sorting and flattening
  std::vector<Subset> temp_subsets;
  std::unordered_map<std::string, uint16_t> subset_type_cache;
  for (size_t i = 0; i < loader.world_.subset_type_names.size(); ++i) {
    subset_type_cache[loader.world_.subset_type_names[i]] = i;
  }

  for (GeoUnitId geo_unit : owned_geo_units) {
    auto meta_it = std::find(subset_meta_geo_units.begin(),
                             subset_meta_geo_units.end(), geo_unit);
    if (meta_it == subset_meta_geo_units.end()) continue;

    size_t meta_idx = std::distance(subset_meta_geo_units.begin(), meta_it);
    size_t subset_start_hdf5 = subset_meta_starts[meta_idx];
    size_t subset_count_hdf5 = subset_meta_counts[meta_idx];
    if (subset_count_hdf5 == 0) continue;

    auto v_ids = loader.readNumericDatasetRange<int32_t>(
        "/venues/subsets/venue_ids", subset_start_hdf5, subset_count_hdf5);
    auto s_indices = loader.readNumericDatasetRange<int32_t>(
        "/venues/subsets/subset_indices", subset_start_hdf5, subset_count_hdf5);
    auto m_counts = loader.readNumericDatasetRange<int32_t>(
        "/venues/subsets/member_counts", subset_start_hdf5, subset_count_hdf5);
    auto m_offsets = loader.readNumericDatasetRange<int64_t>(
        "/venues/subsets/members_offsets", subset_start_hdf5,
        subset_count_hdf5);

    std::vector<std::string> s_names;
    if (loader.datasetExists("/metadata/names/subsets")) {
      s_names = loader.readStringDatasetRange(
          "/metadata/names/subsets", subset_start_hdf5, subset_count_hdf5);
    } else {
      s_names = loader.readStringDatasetRange(
          "/venues/subsets/subset_names", subset_start_hdf5, subset_count_hdf5);
    }

    // Find member data range for this geo_unit
    auto member_it =
        std::find(member_geo_units.begin(), member_geo_units.end(), geo_unit);
    if (member_it == member_geo_units.end()) continue;

    size_t m_idx = std::distance(member_geo_units.begin(), member_it);
    int64_t m_base_offset = member_starts[m_idx];
    size_t m_total_count = member_counts[m_idx];

    auto all_members = loader.readNumericDatasetRange<int32_t>(
        "/venues/subsets/members_flat", m_base_offset, m_total_count);

    for (size_t i = 0; i < subset_count_hdf5; ++i) {
      Subset s;
      s.venue_id = v_ids[i];
      s.subset_index = s_indices[i];

      // Intern subset type
      const std::string& name = s_names[i];
      auto it_type = subset_type_cache.find(name);
      if (it_type == subset_type_cache.end()) {
        s.subset_type_id = loader.world_.subset_type_names.size();
        loader.world_.subset_type_names.push_back(name);
        subset_type_cache[name] = s.subset_type_id;
      } else {
        s.subset_type_id = it_type->second;
      }

      s.member_count = m_counts[i];
      if (s.member_count > 0) {
        s.member_start = loader.world_.subset_members.size();
        int64_t local_off = m_offsets[i] - m_base_offset;
        loader.world_.subset_members.insert(
            loader.world_.subset_members.end(), all_members.begin() + local_off,
            all_members.begin() + local_off + s.member_count);
      }

      temp_subsets.push_back(s);
    }
  }

  // Sort subsets by venue_id so they can be grouped contiguously per venue
  std::sort(temp_subsets.begin(), temp_subsets.end(),
            [](const Subset& a, const Subset& b) {
              if (a.venue_id != b.venue_id) return a.venue_id < b.venue_id;
              return a.subset_index < b.subset_index;
            });

  // Write to world_.subsets and link each contiguous run onto its Venue
  size_t s_i = 0;
  while (s_i < temp_subsets.size()) {
    VenueId vid = temp_subsets[s_i].venue_id;
    Venue* venue = loader.world_.getVenue(vid);
    if (venue) {
      venue->subset_start = loader.world_.subsets.size();
      while (s_i < temp_subsets.size() && temp_subsets[s_i].venue_id == vid) {
        loader.world_.subsets.push_back(temp_subsets[s_i]);
        s_i++;
      }
      venue->subset_count = loader.world_.subsets.size() - venue->subset_start;
    } else {
      // Venue not owned but subsets exist (unlikely given partitioning);
      // skip the entire run.
      while (s_i < temp_subsets.size() && temp_subsets[s_i].venue_id == vid)
        s_i++;
    }
  }
}

void fillGlobalVenueMaps(WorldState& world,
                         const std::vector<int32_t>& venue_ids,
                         const std::vector<uint8_t>& venue_type_ids,
                         const std::vector<GeoUnitId>& venue_geo_unit_ids) {
  const size_t n = venue_ids.size();
  if (venue_type_ids.size() != n || venue_geo_unit_ids.size() != n)
    throw std::runtime_error(
        "fillGlobalVenueMaps: parallel venue arrays disagree in length (ids=" +
        std::to_string(n) + " types=" + std::to_string(venue_type_ids.size()) +
        " geo_units=" + std::to_string(venue_geo_unit_ids.size()) + ")");

  // All three structures cover every Venue in the world. The type index is kept
  // for the whole run (see dropGlobalVenueMaps): a rank must be able to name
  // the type of a Venue it does not own, or kUnknownVenueTypeId means both "no
  // such Venue" and "not decomposed onto me". The geo/by-type maps are only
  // needed until the OTF allocator has precomputed its pools, then freed.
  //
  // Types go in a VenueId-indexed vector, 1 B/entry against ~40 B/entry hashed.
  // Density is not assumed — a hole costs one byte and already answers
  // kUnknownVenueTypeId — but very sparse ids waste memory silently, so warn.
  VenueId max_venue_id = -1;
  for (VenueId id : venue_ids) max_venue_id = std::max(max_venue_id, id);
  const size_t type_index_size = static_cast<size_t>(max_venue_id + 1);
  if (n > 0 && type_index_size > 4 * n)
    std::cerr << "[HALO] sparse /venues/ids: type index spans "
              << type_index_size << " slots for " << n << " venues (ratio "
              << static_cast<double>(type_index_size) / static_cast<double>(n)
              << "x); the holes cost one byte each\n";
  world.venue_type_by_id.assign(type_index_size, kUnknownVenueTypeId);

  world.global_venue_geo_unit_map.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (venue_ids[i] >= 0)
      world.venue_type_by_id[static_cast<size_t>(venue_ids[i])] =
          venue_type_ids[i];
    world.global_venue_geo_unit_map[venue_ids[i]] = venue_geo_unit_ids[i];
    world.addVenueToTypeIndex(venue_ids[i], venue_type_ids[i]);
  }
  world.sortGlobalVenuesByTypeName();
}

void buildGlobalVenueMaps(HDF5Loader& loader) {
  auto all_ids = loader.readNumericDataset<int32_t>("/venues/ids");
  auto all_types = loader.readNumericDataset<uint8_t>("/venues/types");

  // Reconstruct venue→geo_unit from the partition index (geo_unit_id →
  // contiguous range of venues in the global array).
  auto gu_ids = loader.readNumericDataset<int32_t>(
      "/venues/partition_index/geo_unit_ids");
  auto starts = loader.readNumericDataset<int32_t>(
      "/venues/partition_index/start_indices");
  auto counts =
      loader.readNumericDataset<int32_t>("/venues/partition_index/counts");

  std::vector<GeoUnitId> venue_geo(all_ids.size(), -1);
  for (size_t g = 0; g < gu_ids.size(); ++g)
    for (int32_t k = 0; k < counts[g]; ++k)
      venue_geo[static_cast<size_t>(starts[g]) + k] = gu_ids[g];

  fillGlobalVenueMaps(loader.world_, all_ids, all_types, venue_geo);
}

}  // namespace detail
}  // namespace june
