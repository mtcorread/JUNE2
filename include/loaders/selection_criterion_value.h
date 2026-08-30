#pragma once

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/config.h"

namespace june {
namespace config_detail {

// Parse a sequence `value:` of a selection criterion.
//
// A list is whole numbers, except for `geo_unit.<LEVEL>`, the one property
// compared against geographical unit names. Nothing else reads a list of
// strings, so accepting one elsewhere would build a filter that loads clean
// and then matches nobody — a config mistake reading as "nobody qualifies".
// Anything that is neither is therefore a load-time error.
//
// The two parse sites (parseSelectionCriteria in config_loader.cpp and
// PolicyLoader::loadSelectionCriteria) share this so they cannot drift.
inline PropertyValue parseCriterionSequenceValue(
    const YAML::Node& value_node, const std::string& property_path) {
  if (SelectionCriterion::comparesAgainstUnitNames(property_path)) {
    try {
      return value_node.as<std::vector<std::string>>();
    } catch (const YAML::Exception&) {
      throw std::runtime_error("selection criterion '" + property_path +
                               "': list value must be geographical unit names");
    }
  }

  try {
    return value_node.as<std::vector<int32_t>>();
  } catch (const YAML::Exception&) {
    throw std::runtime_error(
        "selection criterion '" + property_path +
        "': list value must be whole numbers (only 'geo_unit.<LEVEL>' compares "
        "against a list of names)");
  }
}

}  // namespace config_detail
}  // namespace june
