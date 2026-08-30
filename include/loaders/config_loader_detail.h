#pragma once

#include <vector>

#include "core/config.h"

// Forward-declare YAML::Node to keep yaml-cpp out of the public include
// surface. Only TUs that actually parse YAML need the full header.
namespace YAML {
class Node;
}  // namespace YAML

namespace june {
namespace config_detail {

// Parse a YAML sequence of `{property, operator, value}` entries into a
// vector of SelectionCriterion. The scalar `value` is dispatched
// int -> double -> string; a sequence `value` goes through
// parseCriterionSequenceValue (loaders/selection_criterion_value.h).
// Shared by the loadSchedule / loadActivityPreferences code in
// config_loader.cpp and by loadVaccination in config_loader_vaccination.cpp.
void parseSelectionCriteria(const YAML::Node& selection_node,
                            std::vector<SelectionCriterion>& out);

}  // namespace config_detail
}  // namespace june
