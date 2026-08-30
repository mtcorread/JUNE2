#include "core/config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "core/world_state.h"
#include "utils/config_checks.h"
#include "utils/filtered_csv.h"
#include "utils/filtering.h"
#include "utils/mpi_logging.h"

namespace june {

DistributionType parseDistributionType(const std::string& s) {
  if (s == "poisson") return DistributionType::POISSON;
  if (s == "binomial") return DistributionType::BINOMIAL;
  if (s == "fixed") return DistributionType::FIXED;
  throw std::runtime_error("Unknown distribution type: '" + s +
                           "'. Must be 'poisson', 'binomial', or 'fixed'.");
}

const char* distributionTypeToString(DistributionType t) {
  switch (t) {
    case DistributionType::POISSON:
      return "poisson";
    case DistributionType::BINOMIAL:
      return "binomial";
    case DistributionType::FIXED:
      return "fixed";
    default:
      return "unknown";
  }
}

bool SelectionCriterion::comparesAgainstUnitNames(
    const std::string& property_path) {
  // Must agree with the GEO_ANCESTOR arm of the path dispatch below.
  return property_path.compare(0, 9, "geo_unit.") == 0;
}

void SelectionCriterion::resolve(const WorldState& world) {
  // 1. First ensure type is cached
  if (cached_type == PropertyType::UNKNOWN) {
    if (property_path == "age")
      cached_type = PropertyType::AGE;
    else if (property_path == "sex")
      cached_type = PropertyType::SEX;
    else if (property_path == "geo_unit_id")
      cached_type = PropertyType::GEO_ID;
    else if (property_path == "id")
      cached_type = PropertyType::PERSON_ID;
    else if (property_path.compare(0, 11, "activities.") == 0) {
      size_t dot1 = property_path.find('.');
      size_t dot2 = property_path.find('.', dot1 + 1);
      if (dot2 != std::string::npos) {
        cached_activity_name = property_path.substr(dot1 + 1, dot2 - dot1 - 1);
        cached_sub_property = property_path.substr(dot2 + 1);
        if (cached_sub_property == "length")
          cached_type = PropertyType::ACTIVITY_LENGTH;
        else if (cached_sub_property == "venue_type")
          cached_type = PropertyType::ACTIVITY_VENUE_TYPE;
      }
    } else if (property_path.compare(0, 11, "properties.") == 0) {
      cached_type = PropertyType::CUSTOM_PROPERTY;
      cached_sub_property = property_path.substr(11);
      cached_prop_idx = world.getPersonPropertyIndex(cached_sub_property);
    } else if (property_path.compare(0, 9, "networks.") == 0) {
      size_t dot1 = property_path.find('.');
      size_t dot2 = property_path.find('.', dot1 + 1);
      if (dot2 != std::string::npos) {
        cached_activity_name = property_path.substr(
            dot1 + 1, dot2 - dot1 - 1);  // Use for network name
        cached_sub_property = property_path.substr(dot2 + 1);
        if (cached_sub_property == "length")
          cached_type = PropertyType::NETWORK_SIZE;
      }
    } else if (comparesAgainstUnitNames(property_path)) {
      // Distinct from the exact-match "geo_unit_id" above: that is the
      // person's own flat unit id, this is their ancestor at a named level.
      cached_type = PropertyType::GEO_ANCESTOR;
      cached_sub_property = property_path.substr(9);
    } else if (property_path == "is_alive") {
      cached_type = PropertyType::IS_ALIVE;
    } else if (property_path.compare(0, 19, "partner_in_network(") == 0) {
      size_t open_paren = property_path.find('(');
      size_t close_paren = property_path.find(')', open_paren);
      if (open_paren != std::string::npos && close_paren != std::string::npos) {
        cached_activity_name =
            property_path.substr(open_paren + 1, close_paren - open_paren - 1);
        cached_type = PropertyType::PARTNER_IN_NETWORK;
      }
    }
  }

  // 2. Resolve target_code for equality comparisons
  if (operator_type == "==" || operator_type == "!=") {
    if (std::holds_alternative<std::string>(value)) {
      const std::string& target_val = std::get<std::string>(value);

      if (cached_type == PropertyType::SEX) {
        if (target_val == "male" || target_val == "M" || target_val == "Male")
          target_code = 0;
        else if (target_val == "female" || target_val == "F" ||
                 target_val == "Female")
          target_code = 1;
        else
          target_code = 2;  // unknown
      } else if (cached_type == PropertyType::CUSTOM_PROPERTY &&
                 cached_prop_idx >= 0) {
        const auto& prop_name = world.person_property_names[cached_prop_idx];
        auto it_reg = world.person_property_value_registries.find(prop_name);
        if (it_reg != world.person_property_value_registries.end()) {
          const auto& registry = it_reg->second;
          auto it = std::find(registry.begin(), registry.end(), target_val);
          if (it != registry.end()) {
            target_code =
                static_cast<int32_t>(std::distance(registry.begin(), it));
          }
        }
      } else if (cached_type == PropertyType::ACTIVITY_VENUE_TYPE) {
        auto it = std::find(world.venue_type_names.begin(),
                            world.venue_type_names.end(), target_val);
        if (it != world.venue_type_names.end()) {
          target_code = static_cast<int32_t>(
              std::distance(world.venue_type_names.begin(), it));
        }
      }
    }
  }

  // 3. Ancestor geography: pre-compute membership for every unit in the
  // world, so evaluate never walks a parent chain.
  if (cached_type == PropertyType::GEO_ANCESTOR && geo_ancestor_mask.empty() &&
      geo_resolve_error.empty()) {
    buildGeoAncestorMask(world);
  }
}

void SelectionCriterion::buildGeoAncestorMask(const WorldState& world) const {
  const std::string& level_name = cached_sub_property;

  auto level_it = std::find(world.geo_level_names.begin(),
                            world.geo_level_names.end(), level_name);
  if (level_it == world.geo_level_names.end()) {
    std::string known;
    for (const std::string& name : world.geo_level_names) {
      known += (known.empty() ? "" : ", ") + name;
    }
    geo_resolve_error = "geographical level '" + level_name +
                        "' is not one this world declares (levels: " + known +
                        ")";
    return;
  }
  const uint8_t level_id = static_cast<uint8_t>(
      std::distance(world.geo_level_names.begin(), level_it));

  std::vector<std::string> target_names;
  if (std::holds_alternative<std::string>(value)) {
    target_names.push_back(std::get<std::string>(value));
  } else if (std::holds_alternative<std::vector<std::string>>(value)) {
    target_names = std::get<std::vector<std::string>>(value);
  } else {
    geo_resolve_error =
        "'" + property_path +
        "' compares against a geographical unit name, not a number or an id";
    return;
  }

  std::vector<GeoUnitId> target_ids;
  for (const std::string& target_name : target_names) {
    std::vector<GeoUnitId> matches;
    std::string level_of_other_match;
    for (const GeographicalUnit& unit : world.geo_units) {
      if (unit.name != target_name) continue;
      if (unit.level_id == level_id) {
        matches.push_back(unit.id);
      } else if (level_of_other_match.empty() &&
                 unit.level_id < world.geo_level_names.size()) {
        level_of_other_match = world.geo_level_names[unit.level_id];
      }
    }
    if (matches.empty()) {
      geo_resolve_error = "no geographical unit named '" + target_name +
                          "' at level '" + level_name + "'";
      if (!level_of_other_match.empty()) {
        geo_resolve_error += " (it exists at level '" + level_of_other_match +
                             "' — did you mean that level?)";
      }
      return;
    }
    if (matches.size() > 1) {
      geo_resolve_error = "geographical unit name '" + target_name +
                          "' is ambiguous at level '" + level_name + "' (" +
                          std::to_string(matches.size()) + " units share it)";
      return;
    }
    target_ids.push_back(matches.front());
  }

  // Dense id keying is the common case, and makes evaluate a single array read;
  // fall back to a sorted id table when the id space is sparse enough for a
  // dense mask to waste more than it saves. Both keyings are self-contained, so
  // the answer does not depend on the id distribution of the world file.
  GeoUnitId max_id = -1;
  for (const GeographicalUnit& unit : world.geo_units) {
    max_id = std::max(max_id, unit.id);
  }
  const size_t dense_size = static_cast<size_t>(max_id + 1);
  geo_mask_unit_ids.clear();
  if (dense_size > 4 * world.geo_units.size()) {
    geo_mask_unit_ids.reserve(world.geo_units.size());
    for (const GeographicalUnit& unit : world.geo_units) {
      geo_mask_unit_ids.push_back(unit.id);
    }
    std::sort(geo_mask_unit_ids.begin(), geo_mask_unit_ids.end());
    geo_ancestor_mask.assign(geo_mask_unit_ids.size(), 2);
  } else {
    geo_ancestor_mask.assign(dense_size, 2);
  }

  size_t units_with_no_ancestor = 0;
  for (const GeographicalUnit& unit : world.geo_units) {
    GeoUnitId ancestor = world.ancestorAtLevel(unit.id, level_name);
    uint8_t state = 2;
    if (ancestor == -1) {
      // Only units people are assigned to directly are worth warning about: a
      // unit coarser than the queried level has no ancestor there by
      // construction. people_by_geo_unit cannot answer this — it keys people
      // under their ancestors too, so every coarse unit is in it.
      if (world.directly_inhabited_geo_units.count(unit.id) > 0)
        ++units_with_no_ancestor;
    } else {
      state = std::find(target_ids.begin(), target_ids.end(), ancestor) !=
                      target_ids.end()
                  ? 1
                  : 0;
    }
    geo_ancestor_mask[geoMaskSlot(unit.id)] = state;
  }

  // Rank-gated: geo_units is global on every rank, so only the inhabited-unit
  // count is rank-local, and one rank's report is enough to flag the geography.
  if (units_with_no_ancestor > 0 && logRank0()) {
    std::cerr << "Warning: '" << property_path
              << "': " << units_with_no_ancestor
              << " inhabited geographical units have no ancestor at level '"
              << level_name
              << "'; people in them match neither == nor !=" << std::endl;
  }
}

size_t SelectionCriterion::geoMaskSlot(GeoUnitId id) const {
  if (id < 0) return geo_ancestor_mask.size();
  if (geo_mask_unit_ids.empty()) {
    const size_t slot = static_cast<size_t>(id);
    return slot < geo_ancestor_mask.size() ? slot : geo_ancestor_mask.size();
  }
  auto it =
      std::lower_bound(geo_mask_unit_ids.begin(), geo_mask_unit_ids.end(), id);
  if (it == geo_mask_unit_ids.end() || *it != id) {
    return geo_ancestor_mask.size();
  }
  return static_cast<size_t>(std::distance(geo_mask_unit_ids.begin(), it));
}

bool SelectionCriterion::evaluate(const Person& person, const WorldState* world,
                                  const Person* partner) const {
  // 1. Resolve property type and path if not cached
  if (cached_type == PropertyType::UNKNOWN) {
    const_cast<SelectionCriterion*>(this)->resolve(*world);
    if (cached_type == PropertyType::UNKNOWN) return false;
  }

  // Boolean predicates: bypass the target-code / fallback machinery entirely.
  // The criterion's `value` is bool; the operator must be == or !=.
  auto eval_bool = [this](bool actual) -> bool {
    if (!std::holds_alternative<bool>(value)) return false;
    bool target = std::get<bool>(value);
    if (operator_type == "==") return actual == target;
    if (operator_type == "!=") return actual != target;
    return false;
  };
  if (cached_type == PropertyType::IS_ALIVE) {
    return eval_bool(!person.is_dead);
  }

  // Ancestor geography: one array read against the mask built at resolve time.
  if (cached_type == PropertyType::GEO_ANCESTOR) {
    const size_t slot = geoMaskSlot(person.geo_unit_id);
    if (slot >= geo_ancestor_mask.size()) return false;
    const uint8_t state = geo_ancestor_mask[slot];
    // Absent is an answer, not a missing one: no ancestor at this level fails
    // in both directions, as a Slot Venue Type does (see docs/CONTEXT.md).
    if (state == 2) return false;
    if (operator_type == "==" || operator_type == "in") return state == 1;
    if (operator_type == "!=") return state == 0;
    return false;
  }

  // 2. Integer comparison for interned properties
  if (target_code != -1) {
    int32_t p_val_code = -1;
    if (cached_type == PropertyType::SEX) {
      p_val_code = static_cast<int32_t>(person.sex);
    } else if (cached_type == PropertyType::CUSTOM_PROPERTY) {
      if (world) {
        auto props = world->getPersonProperties(person);
        if (cached_prop_idx >= 0 && cached_prop_idx < (int)props.size()) {
          p_val_code = props[cached_prop_idx];
        }
      }
    } else if (cached_type == PropertyType::ACTIVITY_VENUE_TYPE) {
      if (!world) return false;
      auto activity_venues =
          world->getActivityVenues(person, cached_activity_name);
      for (const auto& av : activity_venues) {
        if (av.first >= 0 && av.first < (int)world->venues.size()) {
          const auto& v = world->venues[av.first];
          int32_t v_type_code = static_cast<int32_t>(v.type_id);
          if (operator_type == "==") {
            if (v_type_code == target_code) return true;
          } else if (operator_type == "!=") {
            if (v_type_code != target_code) return true;
          }
        }
      }
      return (operator_type ==
              "!=");  // True if none matched and looking for !=
    }

    if (p_val_code != -1) {
      if (operator_type == "==") return p_val_code == target_code;
      if (operator_type == "!=") return p_val_code != target_code;
    }
  }

  // 3. Fallback: Fetch person value(s) based on type
  std::vector<PropertyValue> person_values;

  switch (cached_type) {
    case PropertyType::AGE:
      // Age is float on Person; preserve the fractional part. The compare
      // lambda below handles int-threshold ⇆ double-value comparisons by
      // promoting both sides to double, so YAML expressions like
      // `filter.age<=59` correctly reject an age of 59.5 (matching the
      // legacy hardcoded check against rd.max_active_age).
      person_values.push_back(PropertyValue(static_cast<double>(person.age)));
      break;
    case PropertyType::SEX: {
      std::string sex_str = (person.sex == Sex::MALE)     ? "male"
                            : (person.sex == Sex::FEMALE) ? "female"
                                                          : "unknown";
      person_values.push_back(PropertyValue(sex_str));
      break;
    }
    case PropertyType::GEO_ID:
      person_values.push_back(
          PropertyValue(static_cast<int>(person.geo_unit_id)));
      break;
    case PropertyType::PERSON_ID:
      person_values.push_back(PropertyValue(static_cast<int32_t>(person.id)));
      break;
    case PropertyType::ACTIVITY_LENGTH:
      if (!world) return false;
      person_values.push_back(PropertyValue(static_cast<int>(
          world->getActivityVenues(person, cached_activity_name).size())));
      break;
    case PropertyType::ACTIVITY_VENUE_TYPE: {
      if (!world) return false;
      auto activity_venues =
          world->getActivityVenues(person, cached_activity_name);
      for (const auto& av : activity_venues) {
        if (av.first >= 0 && av.first < (int)world->venues.size()) {
          const auto& v = world->venues[av.first];
          if (v.type_id < world->venue_type_names.size()) {
            person_values.push_back(
                PropertyValue(world->venue_type_names[v.type_id]));
          }
        }
      }
      break;
    }
    case PropertyType::CUSTOM_PROPERTY: {
      if (!world) return false;
      auto prop_opt = world->getPersonProperty(
          person, world->person_property_names[cached_prop_idx]);
      if (prop_opt.has_value()) {
        person_values.push_back(*prop_opt);
      }
      break;
    }
    case PropertyType::NETWORK_SIZE: {
      if (!world) return false;
      size_t n = world->getNetworkPartners(person, cached_activity_name).size();
      person_values.push_back(PropertyValue(static_cast<int>(n)));
      break;
    }
    case PropertyType::PARTNER_IN_NETWORK: {
      if (!world || !partner) return false;
      auto partners = world->getNetworkPartners(person, cached_activity_name);
      bool found = std::find(partners.begin(), partners.end(), partner->id) !=
                   partners.end();
      person_values.push_back(PropertyValue(found ? 1 : 0));
      break;
    }
    default:
      return false;
  }

  if (person_values.empty()) return false;

  // 4. Perform comparison for each fetched value (multi-venue support)
  auto compare = [this](const PropertyValue& p_val) -> bool {
    if (operator_type == "==") return p_val == value;
    if (operator_type == "!=") return p_val != value;

    // Numeric helper: extract a double from PropertyValue if it holds an
    // int or double. Returns false in `out_set` if the value is non-numeric
    // (e.g. a string), in which case the caller falls through to the
    // type-mismatch return below. This lets `filter.age<=59` work even
    // when age is stored as double and the threshold parses as int.
    auto as_number = [](const PropertyValue& pv, double& out) -> bool {
      if (std::holds_alternative<int>(pv)) {
        out = static_cast<double>(std::get<int>(pv));
        return true;
      }
      if (std::holds_alternative<double>(pv)) {
        out = std::get<double>(pv);
        return true;
      }
      return false;
    };

    double lhs = 0.0, rhs = 0.0;
    if (operator_type == ">" || operator_type == "<" || operator_type == ">=" ||
        operator_type == "<=") {
      if (!as_number(p_val, lhs) || !as_number(value, rhs)) return false;
      if (operator_type == ">") return lhs > rhs;
      if (operator_type == "<") return lhs < rhs;
      if (operator_type == ">=") return lhs >= rhs;
      if (operator_type == "<=") return lhs <= rhs;
    } else if (operator_type == "in") {
      if (std::holds_alternative<std::vector<int32_t>>(value) &&
          std::holds_alternative<int>(p_val)) {
        const auto& list = std::get<std::vector<int32_t>>(value);
        int val = std::get<int>(p_val);
        return std::find(list.begin(), list.end(), val) != list.end();
      }
    } else if (operator_type == "contains") {
      if (std::holds_alternative<std::string>(p_val) &&
          std::holds_alternative<std::string>(value)) {
        return std::get<std::string>(p_val).find(
                   std::get<std::string>(value)) != std::string::npos;
      }
    }
    return false;
  };

  for (const auto& pv : person_values) {
    if (compare(pv)) return true;
  }
  return false;
}

void SimulationConfig::resolve(const WorldState& world) {
  // Resolve the partial-presence venue type names into a bitmask of
  // venue_type_ids + a per-id target_group_size lookup. Unknown names are
  // silently ignored (the world may not contain every declared type, e.g.
  // tube_line in a Durham-only world has no successful tube routes and is
  // absent from the registry). Throws if a declared type id exceeds the
  // bitmask width or if target_group_size is non-positive.
  partial_presence.enabled_venue_type_mask = 0;
  partial_presence.target_group_size_by_type_id.assign(
      world.venue_type_names.size(), 0);
  for (const auto& [name, tgs] : partial_presence.target_group_size_by_name) {
    int idx = world.getVenueTypeIndex(name);
    if (idx < 0) continue;
    if (idx >= 64) {
      throw std::runtime_error(
          "SimulationConfig::resolve: partial_presence venue type id " +
          std::to_string(idx) + " ('" + name +
          "') exceeds 64-bit mask width; promote enabled_venue_type_mask to "
          "a wider bitset.");
    }
    if (tgs <= 0) {
      throw std::runtime_error(
          "SimulationConfig::resolve: partial_presence target_group_size for "
          "venue type '" +
          name + "' must be > 0 (got " + std::to_string(tgs) + ").");
    }
    partial_presence.enabled_venue_type_mask |= (uint64_t(1) << idx);
    partial_presence.target_group_size_by_type_id[idx] = tgs;
  }
}

namespace {

// Fills in male_bin/female_bin/bin_by_subset_type/age_to_bin for one matrix
// against a resolved WorldState. Called (via resolve_matrix_bins, which
// adds per-call dedup) from ContactMatrixConfig::resolve(), and directly
// from ContactMatrixConfig::finalizeDefaultModeMatrices() so both paths keep
// matrices reachable only via the default fallback correctly bin-resolved.
void resolveContactMatrixBins(ContactMatrix& matrix, const WorldState& world) {
  std::fill(std::begin(matrix.age_to_bin), std::end(matrix.age_to_bin), -1);
  matrix.has_age_bins = false;
  for (size_t b = 0; b < matrix.bins.size(); ++b) {
    const std::string& bin_name = matrix.bins[b];
    int min_age = -1, max_age = -1;
    if (!bin_name.empty()) {
      if (bin_name.back() == '+') {
        try {
          min_age = std::stoi(bin_name.substr(0, bin_name.size() - 1));
          max_age = 99;
        } catch (...) {
        }
      } else {
        size_t sep_pos = bin_name.find_first_of("-,");
        if (sep_pos != std::string::npos) {
          try {
            size_t start_pos = (bin_name[0] == '[') ? 1 : 0;
            min_age =
                std::stoi(bin_name.substr(start_pos, sep_pos - start_pos));
            size_t end_pos = bin_name.find(']', sep_pos + 1);
            if (end_pos == std::string::npos) end_pos = bin_name.size();
            max_age =
                std::stoi(bin_name.substr(sep_pos + 1, end_pos - sep_pos - 1));
          } catch (...) {
            min_age = -1;
            max_age = -1;
          }
        }
      }
    }
    if (min_age >= 0 && max_age >= min_age) {
      matrix.has_age_bins = true;
      for (int a = std::max(0, min_age); a <= std::min(99, max_age); ++a) {
        if (matrix.age_to_bin[a] < 0) {
          matrix.age_to_bin[a] = static_cast<int>(b);
        }
      }
    }
  }
  matrix.male_bin = matrix.findBinIndex("male");
  matrix.female_bin = matrix.findBinIndex("female");
  matrix.bin_by_subset_type.assign(world.subset_type_names.size(), -1);
  for (size_t st = 0; st < world.subset_type_names.size(); ++st) {
    matrix.bin_by_subset_type[st] =
        matrix.findBinIndex(world.subset_type_names[st]);
  }
}

}  // namespace

void ContactMatrixConfig::resolve(const WorldState& world) {
  betas_by_id.assign(world.venue_type_names.size(), default_beta);
  for (const auto& [name, beta] : betas) {
    int idx = world.getVenueTypeIndex(name);
    if (idx >= 0 && idx < (int)betas_by_id.size()) betas_by_id[idx] = beta;
  }

  // Resolve every matrix's bin fields against this world, whatever it is
  // keyed under. finalizeResolvedMatrices picks matrices out of these same
  // containers by name, so covering the containers covers everything that
  // can end up in the resolved table -- venue types, virtual encounters and
  // the defaults alike.
  //
  // Several keys often alias one matrix (ooe_encounter, romantic_encounters
  // and cohabiting_encounters all point at "romantic_encounter"), so track
  // what has been done rather than resolving the same matrix repeatedly.
  std::unordered_set<const ContactMatrix*> bin_resolved;
  auto resolve_matrix_bins = [&](ContactMatrix& cm) {
    if (!bin_resolved.insert(&cm).second) return;
    resolveContactMatrixBins(cm, world);
  };

  for (auto& [name, matrix] : matrices) resolve_matrix_bins(matrix);
  for (auto& [venue_name, mode_map] : mode_matrices) {
    for (auto& [mode_name, matrix] : mode_map) resolve_matrix_bins(matrix);
  }
  if (default_matrix.has_value()) resolve_matrix_bins(default_matrix.value());
  if (default_mode_matrices.has_value()) {
    for (auto& [mode_name, matrix] : *default_mode_matrices) {
      resolve_matrix_bins(matrix);
    }
  }
}

void ContactMatrixConfig::finalizeDefaultModeMatrices(
    const WorldState& world,
    const std::vector<std::string>& disease_mode_names) {
  default_mode_matrices_by_id.assign(disease_mode_names.size(), nullptr);

  std::vector<std::string> missing_modes;
  for (size_t m = 0; m < disease_mode_names.size(); ++m) {
    const std::string& mode_name = disease_mode_names[m];
    if (default_mode_matrices.has_value()) {
      auto it = default_mode_matrices->find(mode_name);
      if (it != default_mode_matrices->end()) {
        default_mode_matrices_by_id[m] = &it->second;
        resolveContactMatrixBins(it->second, world);
        continue;
      }
    }
    if (!default_matrix.has_value()) {
      missing_modes.push_back(mode_name);
    }
  }

  if (!missing_modes.empty()) {
    std::string missing_list;
    for (size_t i = 0; i < missing_modes.size(); ++i) {
      if (i) missing_list += ", ";
      missing_list += missing_modes[i];
    }
    throw std::runtime_error(
        "default_contacts_matrix has no entry for disease mode(s): " +
        missing_list + " (and no flat default_matrix to fall back on).");
  }
}

void ContactMatrixConfig::throwUnresolved(const char* what, int id,
                                          int mode_index) const {
  std::string msg = "contact matrix lookup for " + std::string(what) + " " +
                    std::to_string(id);
  if (mode_index >= 0) msg += ", mode index " + std::to_string(mode_index);
  msg +=
      " has no resolved entry. Every id a world registry produces is resolved "
      "at load, so this id did not come from one.";
  throw std::runtime_error(msg);
}

namespace {

// A pair that had to borrow the scenario default, for the load-time report.
struct BorrowedDefault {
  std::string type_name;
  std::string mode_name;
};

std::string joinBorrowed(const std::vector<BorrowedDefault>& borrowed) {
  std::string out;
  for (const auto& b : borrowed) {
    out += "\n  - " + b.type_name + " / " + b.mode_name;
  }
  return out;
}

}  // namespace

void ContactMatrixConfig::finalizeResolvedMatrices(
    const WorldState& world,
    const std::vector<std::string>& disease_mode_names) {
  // A disease that declares no modes still transmits through one unnamed
  // channel, and the FOI loop still asks for mode 0, so resolve a single
  // nameless mode for it rather than leaving an empty table behind.
  const int n_modes = std::max<int>(1, disease_mode_names.size());
  auto mode_name_at = [&](int m) -> std::string {
    return m < (int)disease_mode_names.size() ? disease_mode_names[m]
                                              : std::string();
  };
  const size_t n_venue_types = world.venue_type_names.size();
  const size_t n_enc_types = world.encounter_type_names.size();

  resolved_by_id.assign(n_venue_types,
                        std::vector<const ContactMatrix*>(n_modes, nullptr));
  resolved_virtual_by_id.assign(
      n_enc_types, std::vector<const ContactMatrix*>(n_modes, nullptr));
  bin_structure_by_id.assign(n_venue_types, nullptr);
  virtual_bin_structure_by_id.assign(n_enc_types, nullptr);

  std::vector<BorrowedDefault> borrowed;

  // Picks the matrix for one (type, mode), preferring what the scenario said
  // about this type and falling back to the scenario default only as a last
  // resort, recording it when it does so.
  auto resolveOne = [&](const std::string& type_name, int mode_index,
                        bool is_virtual) -> const ContactMatrix* {
    const std::string mode_name = mode_name_at(mode_index);

    auto mode_it = mode_matrices.find(type_name);
    if (mode_it != mode_matrices.end()) {
      auto it = mode_it->second.find(mode_name);
      if (it != mode_it->second.end()) return &it->second;
    }

    auto flat_it = matrices.find(type_name);
    if (flat_it != matrices.end()) return &flat_it->second;

    borrowed.push_back(
        {type_name + (is_virtual ? " (encounter type)" : ""), mode_name});

    if (mode_index < (int)default_mode_matrices_by_id.size() &&
        default_mode_matrices_by_id[mode_index] != nullptr) {
      return default_mode_matrices_by_id[mode_index];
    }
    return default_matrix.has_value() ? &default_matrix.value() : nullptr;
  };

  // A type's bins say who its occupants are, so they cannot legitimately
  // change with the transmission route. Disagreement means one of the mode
  // blocks was edited without the others, which would otherwise surface as
  // people being scored against the wrong stratum.
  auto checkBinsAgree = [&](const std::string& type_name,
                            const std::vector<const ContactMatrix*>& per_mode) {
    const ContactMatrix* first = nullptr;
    int first_mode = -1;
    for (int m = 0; m < n_modes; ++m) {
      if (per_mode[m] == nullptr) continue;
      if (!first) {
        first = per_mode[m];
        first_mode = m;
        continue;
      }
      if (per_mode[m]->bins != first->bins) {
        throw std::runtime_error(
            "contact matrices for '" + type_name +
            "' disagree about bins between mode '" + mode_name_at(first_mode) +
            "' and mode '" + mode_name_at(m) +
            "'. Bins describe who is present, so every mode of a type must "
            "declare the same ones.");
      }
    }
    return first;
  };

  std::vector<std::string> unresolved;

  for (size_t v = 0; v < n_venue_types; ++v) {
    const std::string& name = world.venue_type_names[v];
    for (int m = 0; m < n_modes; ++m) {
      resolved_by_id[v][m] = resolveOne(name, m, /*is_virtual=*/false);
      if (!resolved_by_id[v][m]) unresolved.push_back(name);
    }
    bin_structure_by_id[v] = checkBinsAgree(name, resolved_by_id[v]);
  }

  // Only encounter types held at a virtual venue. One held at a physical
  // venue mixes under that venue's matrix, so it needs none of its own and
  // demanding one would refuse a perfectly well-formed scenario. Sorted, so
  // the pairs reported below come out in the same order on every rank.
  std::vector<uint8_t> virtual_ids = virtual_encounter_type_ids;
  std::sort(virtual_ids.begin(), virtual_ids.end());
  virtual_ids.erase(std::unique(virtual_ids.begin(), virtual_ids.end()),
                    virtual_ids.end());

  for (uint8_t eid : virtual_ids) {
    if (static_cast<size_t>(eid) >= n_enc_types) continue;
    auto name_it = virtual_matrix_names.find(eid);
    const std::string& matrix_name = name_it != virtual_matrix_names.end()
                                         ? name_it->second
                                         : world.encounter_type_names[eid];
    for (int m = 0; m < n_modes; ++m) {
      resolved_virtual_by_id[eid][m] =
          resolveOne(matrix_name, m, /*is_virtual=*/true);
      if (!resolved_virtual_by_id[eid][m]) unresolved.push_back(matrix_name);
    }
    virtual_bin_structure_by_id[eid] =
        checkBinsAgree(matrix_name, resolved_virtual_by_id[eid]);
  }

  if (!unresolved.empty()) {
    throw std::runtime_error(
        "no contact matrix could be resolved for '" + unresolved.front() +
        "', and there is no default_contacts_matrix to fall back on.");
  }

  if (!borrowed.empty() && !allow_default_matrix) {
    throw std::runtime_error(
        "these (type, mode) pairs have no contact matrix of their own and "
        "would silently use default_contacts_matrix:" +
        joinBorrowed(borrowed) +
        "\nDeclare a matrix for each, or set 'allow_default_matrix: true' in "
        "the contact matrices file to accept the default for them.");
  }
  if (!borrowed.empty()) {
    std::cerr << "[contact matrices] using default_contacts_matrix for "
              << borrowed.size()
              << " (type, mode) pair(s):" << joinBorrowed(borrowed)
              << std::endl;
  }

  resolved_finalized_ = true;
}

void ContactMatrixConfig::finalizeDiseaseModeAlignment(
    const std::vector<std::string>& disease_mode_names) {
  std::unordered_set<std::string> seen_disease_mode_names;
  for (const auto& name : disease_mode_names) {
    if (!seen_disease_mode_names.insert(name).second) {
      throw std::runtime_error(
          "disease transmission_params.modes has duplicate mode name '" + name +
          "'; contact-matrix alignment requires unique mode names.");
    }
  }

  // A mode this config declares that no disease mode claims is dead weight in
  // the scenario: nothing will ever look it up. Worth saying so, since it is
  // usually a rename that only landed on one side.
  for (size_t m = 0; m < mode_names.size(); ++m) {
    const bool claimed =
        std::find(disease_mode_names.begin(), disease_mode_names.end(),
                  mode_names[m]) != disease_mode_names.end();
    if (!claimed) {
      std::cerr << "Warning: contact_matrices.yaml mode '" << mode_names[m]
                << "' does not match any disease transmission mode; ignoring."
                << std::endl;
    }
  }
}

void PreferenceProfile::resolve(const WorldState& world) {
  for (auto& crit : selection_criteria) {
    crit.resolveOrThrow(world,
                        "activity preference profile for '" + activity + "'");
  }

  activity_id = world.getActivityIndex(activity);

  weights_by_id.assign(world.venue_type_names.size(), 1.0);
  for (const auto& [name, weight] : preference_weights) {
    int idx = world.getVenueTypeIndex(name);
    if (idx >= 0 && idx < (int)weights_by_id.size()) {
      weights_by_id[idx] = weight;
    }
  }
}

// Helper: Compute a bitmask of activity indices from a list of activity names
static ActivityMask computeActivityMaskFromNames(
    const std::vector<std::string>& activities,
    const std::vector<std::string>& activity_names) {
  ActivityMask mask = 0;
  for (const auto& act : activities) {
    for (size_t i = 0; i < activity_names.size(); ++i) {
      if (activity_names[i] == act) {
        mask |= (ActivityMask(1) << i);
        break;
      }
    }
  }
  return mask;
}

void ScheduleConfig::resolveSlots(const WorldState& world) {
  // Build cycle_to_type_idx: cycle position -> index into day_type_names
  cycle_to_type_idx.resize(day_type_cycle.size());
  for (size_t i = 0; i < day_type_cycle.size(); ++i) {
    auto it = std::find(day_type_names.begin(), day_type_names.end(),
                        day_type_cycle[i]);
    cycle_to_type_idx[i] =
        (it != day_type_names.end())
            ? static_cast<int>(std::distance(day_type_names.begin(), it))
            : 0;
  }

  int num_dt = static_cast<int>(day_type_names.size());
  size_t num_acts = world.activity_names.size();

  // Helper: resolve slot vector caches
  auto resolveSlotVec = [&](std::vector<TimeSlot>& slots) {
    for (auto& slot : slots) {
      slot.allowed_activity_mask = computeActivityMaskFromNames(
          slot.allowed_activities, world.activity_names);
      slot.coordinated_only_activity_mask = computeActivityMaskFromNames(
          slot.coordinated_only_activities, world.activity_names);
      slot.allowed_activity_indices.clear();
      for (const auto& act : slot.allowed_activities) {
        int idx = world.getActivityIndex(act);
        if (idx >= 0)
          slot.allowed_activity_indices.push_back(static_cast<int16_t>(idx));
      }
      if (slot.specified_activity.has_value()) {
        auto& spec = slot.specified_activity.value();
        spec.cached_activity_idx =
            static_cast<int16_t>(world.getActivityIndex(spec.type));
        if (spec.venue_type.has_value()) {
          spec.cached_venue_type_idx =
              world.getVenueTypeIndex(spec.venue_type.value());
        }
      }
      // Resolve hop_on_activity -> hop_schedule_by_activity_idx
      slot.hop_schedule_by_activity_idx.assign(world.activity_names.size(), -1);
      for (const auto& [act_name, sched_name] : slot.hop_on_activity) {
        int act_idx = world.getActivityIndex(act_name);
        auto s_it = std::find_if(
            schedule_types.begin(), schedule_types.end(),
            [&](const ScheduleType& s) { return s.name == sched_name; });
        if (act_idx >= 0 && s_it != schedule_types.end()) {
          slot.hop_schedule_by_activity_idx[act_idx] =
              static_cast<int16_t>(s_it - schedule_types.begin());
        }
      }
      // Resolve property_hop_dispatch -> property_hop_dispatch_by_activity_idx
      for (const auto& [act_name, dispatch] : slot.property_hop_dispatch) {
        int act_idx = world.getActivityIndex(act_name);
        if (act_idx >= 0) {
          slot.property_hop_dispatch_by_activity_idx[static_cast<int16_t>(
              act_idx)] = dispatch;
        }
      }
    }
  };

  for (auto& sched_type : schedule_types) {
    // Resolve force_hybrid_mask and linked_activities_mask. Activities listed
    // in linked_activities are implicitly force-hybrid (must be re-rolled at
    // runtime to honour the daily cached decision).
    sched_type.force_hybrid_mask = 0;
    for (const auto& act_name : sched_type.force_hybrid_activities) {
      int idx = world.getActivityIndex(act_name);
      if (idx >= 0) {
        sched_type.force_hybrid_mask |= (ActivityMask(1) << idx);
      }
    }
    sched_type.linked_activities_mask = 0;
    for (const auto& act_name : sched_type.linked_activities) {
      int idx = world.getActivityIndex(act_name);
      if (idx >= 0) {
        ActivityMask bit = (ActivityMask(1) << idx);
        sched_type.linked_activities_mask |= bit;
        sched_type.force_hybrid_mask |= bit;  // implies force_hybrid
      }
    }

    // Build participation_by_day_type_id[dt_idx][act_idx]
    sched_type.participation_by_day_type_id.assign(
        num_dt, std::vector<double>(num_acts, 0.0));
    for (int dt_idx = 0; dt_idx < num_dt; ++dt_idx) {
      const std::string& dt_name = day_type_names[dt_idx];
      auto it = sched_type.participation_by_day_type.find(dt_name);
      if (it == sched_type.participation_by_day_type.end()) continue;
      for (const auto& [act_name, rate] : it->second) {
        int act_idx = world.getActivityIndex(act_name);
        if (act_idx >= 0 && act_idx < static_cast<int>(num_acts)) {
          sched_type.participation_by_day_type_id[dt_idx][act_idx] = rate;
        }
      }
    }

    // Build slots_by_day_type_idx[dt_idx] and resolve slot caches
    sched_type.slots_by_day_type_idx.assign(num_dt, nullptr);
    for (int dt_idx = 0; dt_idx < num_dt; ++dt_idx) {
      const std::string& dt_name = day_type_names[dt_idx];
      auto it = sched_type.slots_by_day_type.find(dt_name);
      if (it == sched_type.slots_by_day_type.end()) continue;
      resolveSlotVec(it->second);
      sched_type.slots_by_day_type_idx[dt_idx] = &it->second;
    }

    // Resolve flat_slots for temporary schedules
    resolveSlotVec(sched_type.flat_slots);

    // A temporary schedule with no flat_slots is malformed: every hop consumer
    // divides by flat_slots.size() (hopStartDay, advanceAndCheckComplete), so
    // an empty one would divide by zero mid-simulation. Fail fast at load
    // instead.
    if (sched_type.is_temporary && sched_type.flat_slots.empty()) {
      throw std::runtime_error("temporary schedule '" + sched_type.name +
                               "' has no flat_slots");
    }
  }

  // Resolve return_schedule_idx for temporary schedules
  for (auto& sched_type : schedule_types) {
    if (!sched_type.return_schedule.empty()) {
      auto it = std::find_if(schedule_types.begin(), schedule_types.end(),
                             [&](const ScheduleType& s) {
                               return s.name == sched_type.return_schedule;
                             });
      if (it != schedule_types.end())
        sched_type.return_schedule_idx =
            static_cast<int16_t>(it - schedule_types.begin());
    }
  }
}

void PerformanceConfig::resolve(const WorldState& world) {
  deterministic_mask = computeActivityMaskFromNames(deterministic_activities,
                                                    world.activity_names);
  hybrid_mask =
      computeActivityMaskFromNames(hybrid_activities, world.activity_names);
  stochastic_mask =
      computeActivityMaskFromNames(stochastic_activities, world.activity_names);
  masks_resolved = true;
}

void SelectionCriterion::resolveOrThrow(const WorldState& world,
                                        const std::string& context) {
  resolve(world);

  if (cached_type == PropertyType::UNKNOWN) {
    throw std::runtime_error(
        context + ": property '" + property_path +
        "' is not one this engine can read off a person. "
        "Known forms: age, sex, geo_unit_id, id, is_alive, "
        "properties.<name>, activities.<name>.length, "
        "activities.<name>.venue_type, "
        "networks.<name>.length, partner_in_network(<n>), "
        "geo_unit.<LEVEL>");
  }
  if (!geo_resolve_error.empty()) {
    throw std::runtime_error(context + ": " + geo_resolve_error);
  }
  if (cached_type == PropertyType::GEO_ANCESTOR && operator_type != "==" &&
      operator_type != "!=" && operator_type != "in") {
    throw std::runtime_error(context + ": '" + property_path +
                             "' supports only == != in, not '" + operator_type +
                             "'");
  }
  if (cached_type == PropertyType::CUSTOM_PROPERTY && cached_prop_idx < 0) {
    throw std::runtime_error(context + ": person property '" +
                             cached_sub_property +
                             "' is not carried by this world");
  }

  static const std::array<const char*, 8> kOperators = {
      ">", "<", ">=", "<=", "==", "!=", "in", "contains"};
  if (std::find_if(kOperators.begin(), kOperators.end(), [&](const char* op) {
        return operator_type == op;
      }) == kOperators.end()) {
    throw std::runtime_error(context + ": operator '" + operator_type +
                             "' is not supported (use one of > < >= <= == != "
                             "in contains)");
  }
}

void CoordinatedEncounterConfig::resolve(
    WorldState& world, ContactMatrixConfig& contact_matrices) {
  // Follow is resolved first because it works even with encounters disabled.
  // Each rule in the list resolves independently against the world.
  for (FollowConfig& follow : follows) {
    if (!follow.enabled) continue;
    bool has_venue = !follow.pool_venue_type.empty();
    bool has_network = follow.usesNetwork();
    if (has_venue == has_network) {
      throw std::runtime_error(
          "follow requires exactly one pool source: set either "
          "'pool_venue_type' or 'network', not both/neither");
    }
    if (has_venue) {
      follow.pool_venue_type_id =
          world.getVenueTypeIndex(follow.pool_venue_type);
      if (follow.pool_venue_type_id < 0) {
        throw std::runtime_error("follow.pool_venue_type '" +
                                 follow.pool_venue_type +
                                 "' is not a venue type in this world");
      }
    } else {
      follow.network_idx = world.getNetworkTypeIndex(follow.network);
      if (follow.network_idx < 0) {
        throw std::runtime_error("follow.network '" + follow.network +
                                 "' is not a network in this world");
      }
    }
    if (!follow.encounter_type.empty()) {
      int e = world.getEncounterTypeIndex(follow.encounter_type);
      if (e >= 0) follow.encounter_type_id = static_cast<uint8_t>(e);
    }

    // Criteria establishment resolves both criteria lists against the world. An
    // empty list is fine and matches everyone, but a criterion the world cannot
    // answer is a config mistake, not a filter that nobody passes.
    auto resolveCriteria = [&](std::vector<SelectionCriterion>& criteria,
                               const char* which) {
      for (SelectionCriterion& c : criteria)
        c.resolveOrThrow(world, std::string("follow.") + which);
    };
    resolveCriteria(follow.follower, "eligibility");
    resolveCriteria(follow.host, "host_eligibility");

    // A network partner can live on another rank, where its age and properties
    // are not visible, so host_eligibility cannot be applied to a network pool.
    // The host there is simply the lowest-id partner. Reject the combination
    // rather than silently ignore the criteria.
    if (follow.usesNetwork() && !follow.host.empty()) {
      throw std::runtime_error(
          "follow.host_eligibility is not supported with a network pool; "
          "the host is the lowest-id partner. Use a venue pool to constrain "
          "the host by age or property.");
    }

    // The three suppression lists name activities and venue types. Reject any
    // name the world does not know rather than silently ignoring it.
    for (const auto& a : follow.activity_exceptions) {
      int idx = world.getActivityIndex(a);
      if (idx < 0)
        throw std::runtime_error("follow.activity_exceptions: '" + a +
                                 "' is not an activity in this world");
      follow.activity_exception_ids.push_back(static_cast<int16_t>(idx));
    }
    for (const auto& a : follow.follower_activity_exceptions) {
      int idx = world.getActivityIndex(a);
      if (idx < 0)
        throw std::runtime_error("follow.follower_activity_exceptions: '" + a +
                                 "' is not an activity in this world");
      follow.follower_activity_exception_ids.push_back(
          static_cast<int16_t>(idx));
    }
    for (const auto& m : follow.venue_exceptions) {
      int idx = world.getVenueTypeIndex(m);
      if (idx < 0)
        throw std::runtime_error("follow.venue_exceptions: '" + m +
                                 "' is not a venue type in this world");
      follow.venue_exception_type_ids.push_back(static_cast<uint8_t>(idx));
    }

    // All eight pool x establishment x span combinations are implemented, so
    // there is nothing left to reject here. The one guard that remains is the
    // network host_eligibility case above.
  }

  if (!enabled) return;

  // Build the name→id mapping from both flat and mode-specific matrices.
  // Mode-specific entries are valid matrix names for virtual encounters too.
  {
    contact_matrices.matrix_name_to_id.clear();
    std::vector<std::string> sorted_names;
    for (auto& [name, _] : contact_matrices.matrices)
      sorted_names.push_back(name);
    for (auto& [venue_name, mode_map] : contact_matrices.mode_matrices) {
      (void)venue_name;
      for (auto& [mode_name, _] : mode_map) sorted_names.push_back(mode_name);
    }
    std::sort(sorted_names.begin(), sorted_names.end());
    sorted_names.erase(std::unique(sorted_names.begin(), sorted_names.end()),
                       sorted_names.end());
    for (int i = 0; i < (int)sorted_names.size(); ++i)
      contact_matrices.matrix_name_to_id.emplace(sorted_names[i], i);
  }

  // Populate encounter_type_names in WorldState if not already there
  for (auto& enc : encounters) {
    if (std::find(world.encounter_type_names.begin(),
                  world.encounter_type_names.end(),
                  enc.name) == world.encounter_type_names.end()) {
      world.encounter_type_names.push_back(enc.name);
    }

    // Build encounter_type_id -> virtual_contact_matrix name mapping, and
    // record which encounter types are virtual at all. The second list
    // deliberately includes virtual encounters that named no matrix: those
    // are the ones whose matrix is missing or misspelt, and load-time
    // resolution reports them rather than letting them quietly pick up the
    // scenario default.
    if (enc.is_virtual) {
      int type_id = world.getEncounterTypeIndex(enc.name);
      if (type_id >= 0) {
        if (!enc.virtual_contact_matrix.empty()) {
          contact_matrices.virtual_matrix_names[static_cast<uint8_t>(type_id)] =
              enc.virtual_contact_matrix;
        }
        contact_matrices.virtual_encounter_type_ids.push_back(
            static_cast<uint8_t>(type_id));
      }
    }

    // Pre-resolve caches for each encounter def. The loop above guarantees
    // the name is in the registry, so the only way this lookup can fail is
    // the registry outgrowing the uint8_t id: 255 is kDefaultEncounterTypeId
    // and kUnknownVenueTypeId, so an index at or past it would alias onto
    // "no encounter type" and silently lose the def its trigger activities,
    // its min_attendees and its contact matrix.
    const int encounter_type_index = world.getEncounterTypeIndex(enc.name);
    if (encounter_type_index < 0 || encounter_type_index >= 255) {
      throw std::runtime_error(
          "coordinated encounters: encounter type '" + enc.name +
          "' resolved to index " + std::to_string(encounter_type_index) +
          "; the encounter type registry holds " +
          std::to_string(world.encounter_type_names.size()) +
          " names and is capped at 255");
    }
    enc.cached_encounter_type_id = static_cast<uint8_t>(encounter_type_index);
    enc.trigger_mask =
        computeActivityMaskFromNames(enc.trigger_slots, world.activity_names);

    // allowed_venue_mask: bit i set if venue_type_names[i] is in allowed_venues
    enc.allowed_venue_mask = 0;
    for (const auto& vname : enc.allowed_venues) {
      int idx = world.getVenueTypeIndex(vname);
      if (idx >= 0) {
        enc.allowed_venue_mask |= (ActivityMask(1) << idx);
      }
    }

    // Network resolution: looked up from the static network registry.
    enc.cached_network_idx = world.getNetworkTypeIndex(enc.network);

    // Virtual venue type ID: use deterministic sorted registry
    if (enc.is_virtual && !enc.virtual_contact_matrix.empty()) {
      auto id_it =
          contact_matrices.matrix_name_to_id.find(enc.virtual_contact_matrix);
      if (id_it != contact_matrices.matrix_name_to_id.end()) {
        enc.cached_virtual_venue_type_id = id_it->second;
      }
    }
  }
}

const ScheduleType* ScheduleConfig::tryCSVAssignment(const Person& person,
                                                     const WorldState& world,
                                                     std::mt19937& rng) const {
  if (csv_rows.empty()) return nullptr;

  std::uniform_real_distribution<double> dist(0.0, 1.0);

  for (const auto& row : csv_rows) {
    if (filtering::matchesCriteria(person, &world, row.criteria)) {
      double rand_val = dist(rng);
      for (const auto& [type_idx, cumulative] : row.schedule_probs) {
        if (rand_val < cumulative) {
          return &schedule_types[type_idx];
        }
      }
      // rand_val >= last cumulative bound → fallback to YAML
      return nullptr;
    }
  }
  return nullptr;  // No row matched
}

void ScheduleConfig::resolveCSV(const WorldState& world) {
  if (csv_path.empty()) return;
  csv_rows.clear();

  csv::FilteredTable table = csv::loadFilteredCSV(csv_path);

  // Classify value columns: schedule.<name> → schedule_type index, plus the
  // geo_level / geo_unit pair used for world-graph BFS.
  std::vector<std::pair<std::string, int>>
      schedule_cols;  // (value-col name, schedule_type_idx)
  for (const auto& col : table.value_columns) {
    if (col.find("schedule.") == 0) {
      std::string sched_name = col.substr(9);
      int idx = -1;
      for (int j = 0; j < (int)schedule_types.size(); ++j) {
        if (schedule_types[j].name == sched_name) {
          idx = j;
          break;
        }
      }
      if (idx < 0) {
        throw std::runtime_error("Schedule '" + sched_name +
                                 "' referenced in column 'schedule." +
                                 sched_name + "' not found in schedule_types");
      }
      schedule_cols.push_back({col, idx});
    }
  }

  auto get = [](const csv::FilteredRow& r,
                const std::string& name) -> std::string {
    auto it = r.values.find(name);
    return it == r.values.end() ? "" : it->second;
  };

  int row_num = 0;
  for (const auto& r : table.rows) {
    ++row_num;
    ScheduleAssignmentRow row;
    row.criteria = r.criteria;

    // Geo filter: resolve (geo_level, geo_unit) → "in" criterion over SGU ids
    std::string geo_level_val = get(r, "geo_level");
    std::string geo_unit_val = get(r, "geo_unit");
    if (geo_level_val.empty() != geo_unit_val.empty()) {
      throw std::runtime_error(
          "Schedule assignment CSV '" + csv_path + "' row " +
          std::to_string(row_num) +
          " has only one of geo_level/geo_unit set (both or neither required); "
          "got geo_level='" +
          geo_level_val + "', geo_unit='" + geo_unit_val + "'");
    }
    if (!geo_level_val.empty() && !geo_unit_val.empty()) {
      GeoUnitId found_id = -1;
      for (const auto& unit : world.geo_units) {
        if (unit.level_id < (int)world.geo_level_names.size() &&
            world.geo_level_names[unit.level_id] == geo_level_val &&
            unit.name == geo_unit_val) {
          found_id = unit.id;
          break;
        }
      }
      if (found_id == -1) {
        throw std::runtime_error(
            "Schedule assignment CSV '" + csv_path + "' row " +
            std::to_string(row_num) + ": geo_level='" + geo_level_val +
            "', geo_unit='" + geo_unit_val + "' not found in world");
      }
      // BFS to collect all descendant geo_unit_ids (including self)
      std::vector<int32_t> descendant_ids;
      std::vector<GeoUnitId> to_visit = {found_id};
      while (!to_visit.empty()) {
        GeoUnitId current = to_visit.back();
        to_visit.pop_back();
        descendant_ids.push_back(static_cast<int32_t>(current));
        for (const auto& unit : world.geo_units) {
          if (unit.parent_id == current) to_visit.push_back(unit.id);
        }
      }
      SelectionCriterion geo_crit;
      geo_crit.property_path = "geo_unit_id";
      geo_crit.operator_type = "in";
      geo_crit.value = descendant_ids;
      row.criteria.push_back(geo_crit);
    }

    // Parse schedule probabilities
    double sum = 0.0;
    std::vector<std::pair<int, double>> raw_probs;
    for (const auto& [col_name, type_idx] : schedule_cols) {
      std::string v = get(r, col_name);
      if (v.empty()) continue;
      double prob;
      try {
        prob = std::stod(v);
      } catch (const std::exception&) {
        throw std::runtime_error("Schedule assignment CSV '" + csv_path +
                                 "' row " + std::to_string(row_num) +
                                 " column '" + col_name +
                                 "' has non-numeric value '" + v + "'");
      }
      if (prob > 0.0) {
        raw_probs.push_back({type_idx, prob});
        sum += prob;
      }
    }

    if (sum > 1.02) {
      throw std::runtime_error("Schedule probabilities in CSV row " +
                               std::to_string(row_num) + " sum to " +
                               std::to_string(sum) + ", must not exceed 1.02");
    }

    if (sum >= 0.98) {
      if (std::abs(sum - 1.0) > 1e-6) {
        std::cout << "Warning: schedule probabilities in row " << row_num
                  << " sum to " << sum << ", normalizing to 1.0" << std::endl;
      }
      row.fallback_prob = 0.0;
      for (auto& [type_idx, prob] : raw_probs) prob /= sum;
    } else {
      row.fallback_prob = 1.0 - sum;
    }

    double cumulative = 0.0;
    for (const auto& [type_idx, prob] : raw_probs) {
      cumulative += prob;
      row.schedule_probs.push_back({type_idx, cumulative});
    }

    csv_rows.push_back(row);
  }

  std::cout << "Loaded " << csv_rows.size()
            << " schedule assignment rows from: " << csv_path << std::endl;
}

void Config::resolve(WorldState& world) {
  checkConfigConsistency(*this, world);
  simulation.resolve(world);
  schedule.resolve(world);
  schedule.resolveCSV(world);
  activity_preferences.resolve(world);
  vaccination.resolve(world);
  contact_matrices.resolve(world);
  coordinated_encounters.resolve(world, contact_matrices);
  performance.resolve(world);
}

}  // namespace june
