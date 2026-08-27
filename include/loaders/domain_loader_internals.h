#pragma once

// Internal helpers shared between domain_loader.cpp and
// domain_loader_internals.cpp. Not part of the public HDF5Loader API:
// kept in namespace `june::detail` and intended only for these two TUs.

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/types.h"

namespace june {

class HDF5Loader;
class WorldState;
struct Config;

namespace detail {

using GeoPartitionMap =
    std::unordered_map<GeoUnitId, std::pair<size_t, size_t>>;

// One contiguous run of HDF5 rows for a chunk: a single hyperslab read can
// cover multiple geo_units as long as their (start, count) regions are
// adjacent in the file. gu_indices lists which geo_units (indices into
// geo_units_vec) this span covers, in order.
struct ChunkSpan {
  size_t start;
  size_t count;
  std::vector<size_t> gu_indices;
};

// Load an HDF5 partition index (geo_unit_ids / start_indices / counts) into a
// map from GeoUnitId to (start, count).
GeoPartitionMap buildPartitionMap(HDF5Loader& loader,
                                  const std::string& path_prefix);

// Walk the geo_units in [start_idx, end_idx) and group those present in
// `partition_map` into spans of file-adjacent rows. Returns the spans in
// the order their first geo_unit appears.
std::vector<ChunkSpan> detectChunkSpans(
    const GeoPartitionMap& partition_map,
    const std::vector<GeoUnitId>& geo_units_vec, size_t start_idx,
    size_t end_idx);

// Ensure world.activity_names contains internal system activities plus every
// activity referenced by any time slot in the schedule config. Idempotent.
void registerSystemAndConfigActivities(WorldState& world, const Config& config);

// Append schedule type names from config that are not already in
// world.schedule_type_names, preserving config order.
void syncScheduleTypeNames(WorldState& world, const Config& config);

// Walk /venues/properties/<venue_type>/<prop_name> and return a map from
// venue type name to its list of property dataset names. Empty if the group
// does not exist.
std::unordered_map<std::string, std::vector<std::string>>
discoverVenuePropertyNames(HDF5Loader& loader);

// Read one population span (single hyperslab) and emplace its people into
// loader.world_, interning categorical properties and parsing any network-
// encoded fields. Mutates property_indices_cache as new categorical values
// are seen.
void loadPersonsInSpan(
    HDF5Loader& loader, const ChunkSpan& span,
    const GeoPartitionMap& pop_partition_map,
    const std::vector<GeoUnitId>& geo_units_vec,
    const std::vector<std::string>& population_property_names,
    std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>&
        property_indices_cache);

// Read one venue span (single hyperslab) and emplace its venues into
// loader.world_, reserving property slots before delegating to a private
// per-type property fill.
void loadVenuesInSpan(
    HDF5Loader& loader, const ChunkSpan& span,
    const GeoPartitionMap& venue_partition_map,
    const std::vector<GeoUnitId>& geo_units_vec,
    const std::unordered_map<std::string, std::vector<std::string>>&
        venue_type_prop_names,
    std::unordered_map<std::string, std::unordered_map<std::string, int32_t>>&
        venue_property_indices_cache);

// Read one activity-mapping span (single hyperslab) and stitch rows into
// per-person ActivityMeta + activity_venues entries on the local people only
// (rows for non-local people are skipped).
void loadActivityMappingsInSpan(
    HDF5Loader& loader, const ChunkSpan& span,
    const std::unordered_map<PersonId, size_t>& local_person_idx_map);

// Optional per-(person, venue) membership metadata (Design B side-table).
// Carries per-leg fields (e.g. boarding/alighting times for route activities)
// when present. Backward-compatible: old worlds without this dataset simply
// have no per-membership metadata, and partial_presence venues degrade to
// full-slot presence in the FOI loop.
void loadMembershipMetadata(
    HDF5Loader& loader,
    const std::unordered_map<PersonId, size_t>& local_person_idx_map);

// Sentinel returned by matchMembershipRowToFlatIndex when a side-table row
// has no corresponding entry in the person's activity_venues.
constexpr uint32_t kAbsentFlatIndex =
    std::numeric_limits<uint32_t>::max();

// Find the flat index into world.activity_venues for a single
// membership-metadata side-table row belonging to `person`. Matches on
// venue_id alone when subset_index is nullptr (old worlds, pre subset_index
// column); matches on (venue_id, subset_index) when it is non-null, which
// disambiguates multiple Subsets a person holds at the same Venue (e.g. two
// Feast accommodation memberships sharing one guest house). Returns
// kAbsentFlatIndex if no candidate venue matches.
uint32_t matchMembershipRowToFlatIndex(const WorldState& world,
                                       const Person& person,
                                       VenueId venue_id,
                                       const SubsetIndex* subset_index);

// Load /venues/subsets for every owned geo_unit, intern subset type names,
// then sort by (venue_id, subset_index) and link each contiguous run back
// onto its Venue's subset_start/subset_count. Requires world_.venue_index
// to be built so getVenue() resolves.
void loadVenueSubsets(HDF5Loader& loader,
                      const std::unordered_set<GeoUnitId>& owned_geo_units);

// Fill world's three global venue structures (VenueId-indexed type vector,
// geo_unit map, by-type-name index) from the world's full venue arrays. The
// three input vectors are parallel and cover EVERY Venue in the world, not just
// this rank's: a rank must be able to name the type of a Venue it does not own,
// otherwise the same value means both "no such Venue" and "not decomposed onto
// me" and venue-gated policy becomes rank-dependent. Throws if the three
// lengths disagree; warns (never throws) if the ids are sparse.
//
// Split out from buildGlobalVenueMaps so it can be tested without an
// HDF5Loader.
void fillGlobalVenueMaps(WorldState& world,
                         const std::vector<int32_t>& venue_ids,
                         const std::vector<uint8_t>& venue_type_ids,
                         const std::vector<GeoUnitId>& venue_geo_unit_ids);

// Read ALL venue IDs and type_ids from HDF5, reconstruct venue→geo_unit from
// the partition index, then hand all three to fillGlobalVenueMaps(). Required
// for deterministic getVenuesInGeoUnit() in MPI mode and for cross-rank type
// lookups in selectVenue().
void buildGlobalVenueMaps(HDF5Loader& loader);

}  // namespace detail
}  // namespace june
