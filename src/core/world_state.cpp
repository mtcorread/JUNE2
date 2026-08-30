#include "core/world_state.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace june {

void WorldState::buildIndices() {
  person_index.clear();
  venue_index.clear();
  geo_unit_index.clear();
  venues_by_type.clear();

  for (size_t i = 0; i < people.size(); ++i) {
    person_index[people[i].id] = i;
  }

  for (size_t i = 0; i < venues.size(); ++i) {
    venue_index[venues[i].id] = i;
    if (venues[i].type_id < venue_type_names.size()) {
      venues_by_type[venue_type_names[venues[i].type_id]].push_back(i);
    } else {
      venues_by_type["unknown"].push_back(i);
    }
    // venue_type_by_id is not touched here: for a hand-built WorldState
    // getVenueTypeId() resolves every venue via getVenue(), and for an
    // HDF5-loaded one the loader's buildGlobalVenueMaps has already filled it
    // for every Venue in the world, at any rank count.
  }

  for (size_t i = 0; i < geo_units.size(); ++i) {
    geo_unit_index[geo_units[i].id] = i;
  }

  people_by_geo_unit.clear();
  directly_inhabited_geo_units.clear();
  for (size_t i = 0; i < people.size(); ++i) {
    GeoUnitId current_id = people[i].geo_unit_id;
    if (current_id != -1) directly_inhabited_geo_units.insert(current_id);
    while (current_id != -1) {
      people_by_geo_unit[current_id].push_back(i);

      auto it = geo_unit_index.find(current_id);
      if (it == geo_unit_index.end()) break;
      current_id = geo_units[it->second].parent_id;
    }
  }

  buildGlobalVenueMaps();
}

void WorldState::addVenueToTypeIndex(VenueId venue_id, uint8_t type_id) {
  const std::string& type_name = (type_id < venue_type_names.size())
                                     ? venue_type_names[type_id]
                                     : "unknown";
  global_venues_by_type_name[type_name].push_back(venue_id);
}

void WorldState::setGlobalVenueType(VenueId venue_id, uint8_t type_id) {
  if (venue_id < 0) return;
  const size_t index = static_cast<size_t>(venue_id);
  if (index >= venue_type_by_id.size())
    venue_type_by_id.resize(index + 1, kUnknownVenueTypeId);
  venue_type_by_id[index] = type_id;
}

void WorldState::sortGlobalVenuesByTypeName() {
  for (auto& [_, ids] : global_venues_by_type_name)
    std::sort(ids.begin(), ids.end());
}

void WorldState::buildGlobalVenueMaps() {
  if (!global_venues_by_type_name.empty()) return;  // MPI loader already filled
  for (size_t i = 0; i < venues.size(); ++i) {
    global_venue_geo_unit_map.emplace(venues[i].id, venues[i].geo_unit_id);
    addVenueToTypeIndex(venues[i].id, venues[i].type_id);
  }
  sortGlobalVenuesByTypeName();
}

Person* WorldState::getPerson(PersonId id) {
  auto it = person_index.find(id);
  return (it != person_index.end()) ? &people[it->second] : nullptr;
}

const Person* WorldState::getPerson(PersonId id) const {
  auto it = person_index.find(id);
  return (it != person_index.end()) ? &people[it->second] : nullptr;
}

Venue* WorldState::getVenue(VenueId id) {
  auto it = venue_index.find(id);
  return (it != venue_index.end()) ? &venues[it->second] : nullptr;
}

const Venue* WorldState::getVenue(VenueId id) const {
  auto it = venue_index.find(id);
  return (it != venue_index.end()) ? &venues[it->second] : nullptr;
}

GeographicalUnit* WorldState::getGeoUnit(GeoUnitId id) {
  auto it = geo_unit_index.find(id);
  return (it != geo_unit_index.end()) ? &geo_units[it->second] : nullptr;
}

const GeographicalUnit* WorldState::getGeoUnit(GeoUnitId id) const {
  auto it = geo_unit_index.find(id);
  return (it != geo_unit_index.end()) ? &geo_units[it->second] : nullptr;
}

std::vector<Venue*> WorldState::getVenuesByType(const std::string& type) {
  std::vector<Venue*> result;
  auto it = venues_by_type.find(type);
  if (it != venues_by_type.end()) {
    result.reserve(it->second.size());
    for (uint32_t idx : it->second) {
      result.push_back(&venues[idx]);
    }
  }
  return result;
}

std::vector<VenueId> WorldState::getVenuesInGeoUnit(
    GeoUnitId hosting_geo_unit_id, const std::string& venue_type_name) const {
  std::vector<VenueId> result;
  auto type_it = global_venues_by_type_name.find(venue_type_name);
  if (type_it == global_venues_by_type_name.end()) return result;

  // global_venues_by_type_name is sorted by venue_id at build time, so the
  // result is already sorted — no trailing sort needed.
  for (VenueId vid : type_it->second) {
    auto geo_it = global_venue_geo_unit_map.find(vid);
    if (geo_it == global_venue_geo_unit_map.end()) continue;
    GeoUnitId current = geo_it->second;
    while (current != -1) {
      if (current == hosting_geo_unit_id) {
        result.push_back(vid);
        break;
      }
      auto gu_it = geo_unit_index.find(current);
      if (gu_it == geo_unit_index.end()) break;
      current = geo_units[gu_it->second].parent_id;
    }
  }
  return result;
}

GeoUnitId WorldState::ancestorAtLevel(GeoUnitId id,
                                      std::string_view level_name) const {
  GeoUnitId current = id;
  while (current != -1) {
    auto it = geo_unit_index.find(current);
    if (it == geo_unit_index.end()) break;
    const GeographicalUnit& gu = geo_units[it->second];
    if (gu.level_id < geo_level_names.size() &&
        geo_level_names[gu.level_id] == level_name)
      return current;
    current = gu.parent_id;
  }
  return -1;
}

void WorldState::dropGlobalVenueMaps() {
  size_t by_type_venues = 0;
  for (auto& [k, v] : global_venues_by_type_name) by_type_venues += v.size();
  std::cerr << "[HALO] freeing OTF maps: geo_unit_map_entries="
            << global_venue_geo_unit_map.size()
            << " by_type_name_venue_entries=" << by_type_venues
            << " (all-venue type index kept, entries="
            << venue_type_by_id.size()
            << " bytes=" << venue_type_by_id.capacity() * sizeof(uint8_t)
            << ")\n";
  std::unordered_map<VenueId, GeoUnitId>().swap(global_venue_geo_unit_map);
  std::unordered_map<std::string, std::vector<VenueId>>().swap(
      global_venues_by_type_name);
}

std::vector<Person*> WorldState::getPeopleInUnit(GeoUnitId id) {
  std::vector<Person*> result;
  auto it = people_by_geo_unit.find(id);
  if (it != people_by_geo_unit.end()) {
    result.reserve(it->second.size());
    for (uint32_t idx : it->second) {
      result.push_back(&people[idx]);
    }
  }
  return result;
}

std::vector<Person*> WorldState::getPeopleInUnit(const std::string& level,
                                                 const std::string& name) {
  for (const auto& unit : geo_units) {
    std::string unit_level = (unit.level_id < geo_level_names.size())
                                 ? geo_level_names[unit.level_id]
                                 : "unknown";
    if (unit_level == level && unit.name == name) {
      return getPeopleInUnit(unit.id);
    }
  }
  return {};
}

void WorldState::printSummary() const {
  std::cout << "WorldState Summary:" << std::endl;
  std::cout << "  People: " << people.size() << std::endl;
  std::cout << "  Venues: " << venues.size() << std::endl;
  std::cout << "  Geo Units: " << geo_units.size() << std::endl;
  std::cout << "  Activities: " << activity_names.size() << std::endl;

  std::cout << "  Venues by type:" << std::endl;
  for (const auto& [type, indices] : venues_by_type) {
    std::cout << "    " << type << ": " << indices.size() << std::endl;
  }
}

void WorldState::loadRegionalRiskFactors(const std::string& csv_path) {
  std::ifstream file(csv_path);
  if (!file.is_open()) {
    std::cerr << "Error: Could not open regional risk factors file: "
              << csv_path << std::endl;
    return;
  }

  std::string line;
  std::getline(file, line);  // Skip header

  std::unordered_map<std::string, std::pair<float, float>> factors;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    std::stringstream ss(line);
    std::string name, trans_str, sever_str;
    if (std::getline(ss, name, ',') && std::getline(ss, trans_str, ',') &&
        std::getline(ss, sever_str, ',')) {
      try {
        factors[name] = {std::stof(trans_str), std::stof(sever_str)};
      } catch (...) {
        continue;
      }
    }
  }

  int updated_units = 0;
  for (auto& gu : geo_units) {
    auto it = factors.find(gu.name);
    if (it != factors.end()) {
      gu.transmission_factor = it->second.first;
      gu.severity_factor = it->second.second;
      updated_units++;
    }
  }

  int updated_venues = 0;
  for (auto& v : venues) {
    if (v.geo_unit_id != -1) {
      auto it_idx = geo_unit_index.find(v.geo_unit_id);
      if (it_idx != geo_unit_index.end()) {
        float susc = geo_units[it_idx->second].transmission_factor;
        if (susc != 1.0f) {
          v.transmission_factor = susc;
          updated_venues++;
        }
      }
    }
  }

  std::cout << "[Regional Risk] Loaded factors for " << updated_units
            << " geographical units. Updated " << updated_venues
            << " venues for performance caching." << std::endl;
}

}  // namespace june
