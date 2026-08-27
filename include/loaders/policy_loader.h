#pragma once

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <stdexcept>
#include <string>

#include "core/config.h"
#include "epidemiology/policy.h"
#include "loaders/selection_criterion_value.h"
#include "utils/time_utils.h"

namespace june {

class PolicyLoader {
 public:
  // Load policies from YAML file
  static void loadPolicies(PolicyManager& policy_manager,
                           const std::string& filename,
                           const std::string& simulation_start_date);

 private:
  // Load selection criteria (same format as schedule selection criteria)
  static std::vector<SelectionCriterion> loadSelectionCriteria(
      const YAML::Node& node);

  // Load a policy action
  static PolicyAction loadPolicyAction(const YAML::Node& node);

  // Load the four date-range keys shared by every policy kind that carries an
  // ActiveWindow: start_date > start_time > 0.0, end_date > end_time > no end.
  static ActiveWindow loadActiveWindow(const YAML::Node& node,
                                       const std::string& simulation_start_date);

  // Load symptom policies
  static void loadSymptomPolicies(PolicyManager& policy_manager,
                                  const YAML::Node& node,
                                  const std::string& simulation_start_date);

  // Load temporal policies (lockdowns, etc.)
  static void loadTemporalPolicies(PolicyManager& policy_manager,
                                   const YAML::Node& node,
                                   const std::string& simulation_start_date);
};

// =============================================================================
// Implementation
// =============================================================================

inline void PolicyLoader::loadPolicies(
    PolicyManager& policy_manager, const std::string& filename,
    const std::string& simulation_start_date) {
  try {
    YAML::Node root = YAML::LoadFile(filename);

    if (!root["policies"]) {
      std::cout << "No policies section found in " << filename << std::endl;
      return;
    }

    const YAML::Node& policies = root["policies"];

    // Load symptom-based policies
    if (policies["symptom_policies"]) {
      loadSymptomPolicies(policy_manager, policies["symptom_policies"],
                          simulation_start_date);
    }

    // Load temporal policies
    if (policies["temporal_policies"]) {
      loadTemporalPolicies(policy_manager, policies["temporal_policies"],
                           simulation_start_date);
    }

  } catch (const YAML::Exception& e) {
    throw std::runtime_error("Error loading policy file '" + filename +
                             "': " + e.what());
  }
}

inline std::vector<SelectionCriterion> PolicyLoader::loadSelectionCriteria(
    const YAML::Node& node) {
  std::vector<SelectionCriterion> criteria;

  if (!node || !node.IsSequence()) {
    return criteria;
  }

  for (const auto& criterion_node : node) {
    SelectionCriterion criterion;
    criterion.property_path = criterion_node["property"].as<std::string>();
    criterion.operator_type = criterion_node["operator"].as<std::string>();

    // Parse value (can be int, float, string, or list)
    const auto& value_node = criterion_node["value"];
    if (value_node.IsSequence()) {
      criterion.value = config_detail::parseCriterionSequenceValue(
          value_node, criterion.property_path);
    } else if (value_node.IsScalar()) {
      // Try boolean first, then int, then float, then string
      std::string str_val = value_node.as<std::string>();
      if (str_val == "true" || str_val == "false") {
        criterion.value = value_node.as<bool>() ? 1 : 0;
      } else {
        try {
          criterion.value = value_node.as<int>();
        } catch (...) {
          try {
            criterion.value = value_node.as<float>();
          } catch (...) {
            criterion.value = value_node.as<std::string>();
          }
        }
      }
    }

    criteria.push_back(criterion);
  }

  return criteria;
}

inline PolicyAction PolicyLoader::loadPolicyAction(const YAML::Node& node) {
  PolicyAction action;

  // Override activities
  if (node["override_activities"]) {
    if (node["override_activities"].IsSequence()) {
      // Load as vector then convert to unordered_set
      auto activities_vec =
          node["override_activities"].as<std::vector<std::string>>();
      action.override_activities = std::unordered_set<std::string>(
          activities_vec.begin(), activities_vec.end());
    } else {
      // Single activity or "*"
      std::string activity = node["override_activities"].as<std::string>();
      action.override_activities.insert(activity);
    }
  }

  // Restrict the override to a set of venue types (absent = any venue type).
  // Same scalar-or-sequence idiom as override_activities.
  if (node["override_venue_types"]) {
    if (node["override_venue_types"].IsSequence()) {
      auto venue_types_vec =
          node["override_venue_types"].as<std::vector<std::string>>();
      action.override_venue_types = std::unordered_set<std::string>(
          venue_types_vec.begin(), venue_types_vec.end());
    } else {
      action.override_venue_types.insert(
          node["override_venue_types"].as<std::string>());
    }
  }

  // Exempt the override from a set of venue types (absent = no exemption).
  // Mutually exclusive with override_venue_types; PolicyAction::resolve throws
  // if both are set.
  if (node["exempt_venue_types"]) {
    if (node["exempt_venue_types"].IsSequence()) {
      auto venue_types_vec =
          node["exempt_venue_types"].as<std::vector<std::string>>();
      action.exempt_venue_types = std::unordered_set<std::string>(
          venue_types_vec.begin(), venue_types_vec.end());
    } else {
      action.exempt_venue_types.insert(
          node["exempt_venue_types"].as<std::string>());
    }
  }

  // Replacement activity (ignored when replacement_schedule is set)
  if (node["replacement"]) {
    action.replacement_activity = node["replacement"].as<std::string>();
  } else {
    action.replacement_activity = "residence";  // Default fallback
  }

  // Replacement schedule: policy triggers a schedule hop instead of an
  // activity replacement. Only effective for persons already on a hop.
  if (node["replacement_schedule"]) {
    action.replacement_schedule =
        node["replacement_schedule"].as<std::string>();
  }

  // Generic exemptions
  if (node["exempt"]) {
    if (node["exempt"].IsSequence()) {
      for (const auto& exempt_node : node["exempt"]) {
        ActivityExemption exemption;
        exemption.activity_name = exempt_node["activity"].as<std::string>();
        if (exempt_node["selection"]) {
          exemption.criteria = loadSelectionCriteria(exempt_node["selection"]);
        }
        action.exemptions.push_back(exemption);
      }
    }
  }

  // Compliance rate
  if (node["compliance_rate"]) {
    action.compliance_rate = node["compliance_rate"].as<double>();
  }

  return action;
}

inline ActiveWindow PolicyLoader::loadActiveWindow(
    const YAML::Node& node, const std::string& simulation_start_date) {
  ActiveWindow window;

  // Start: a date wins over a raw offset; declaring neither starts immediately.
  if (node["start_date"]) {
    std::tm sim_start_tm = parseDate(simulation_start_date);
    std::tm policy_start_tm = parseDate(node["start_date"].as<std::string>());
    window.start_time =
        static_cast<double>(daysBetween(sim_start_tm, policy_start_tm));
  } else if (node["start_time"]) {
    window.start_time = node["start_time"].as<double>();
  }

  // End: same precedence. The bound is half-open, so end_date names the first
  // day the policy is NOT in force. Declaring neither leaves the -1 sentinel.
  if (node["end_date"]) {
    std::tm sim_start_tm = parseDate(simulation_start_date);
    std::tm policy_end_tm = parseDate(node["end_date"].as<std::string>());
    window.end_time =
        static_cast<double>(daysBetween(sim_start_tm, policy_end_tm));
  } else if (node["end_time"]) {
    window.end_time = node["end_time"].as<double>();
  }

  return window;
}

inline void PolicyLoader::loadSymptomPolicies(
    PolicyManager& policy_manager, const YAML::Node& node,
    const std::string& simulation_start_date) {
  if (!node.IsSequence()) {
    throw std::runtime_error("symptom_policies must be a list");
  }

  for (const auto& policy_node : node) {
    SymptomPolicy policy;

    // Name
    if (!policy_node["name"]) {
      throw std::runtime_error("symptom_policy must have a 'name' field");
    }
    policy.name = policy_node["name"].as<std::string>();

    // Trigger symptoms
    if (!policy_node["symptoms"]) {
      throw std::runtime_error("symptom_policy '" + policy.name +
                               "' must have 'symptoms' field");
    }
    policy.trigger_symptoms =
        policy_node["symptoms"].as<std::vector<std::string>>();

    policy.window = loadActiveWindow(policy_node, simulation_start_date);

    // Action
    policy.action = loadPolicyAction(policy_node);

    // Follow-up policy (optional)
    if (policy_node["follow_up_policy"]) {
      policy.follow_up_policy_name =
          policy_node["follow_up_policy"].as<std::string>();
    }
    if (policy_node["inherit_compliance"]) {
      policy.inherit_compliance = policy_node["inherit_compliance"].as<bool>();
    }
    if (policy_node["inherit_refusal"]) {
      policy.inherit_refusal = policy_node["inherit_refusal"].as<bool>();
    }

    // Selection criteria (optional)
    if (policy_node["applies_to"]) {
      policy.applies_to = loadSelectionCriteria(policy_node["applies_to"]);
    }

    policy_manager.addSymptomPolicy(policy);
  }
}

inline void PolicyLoader::loadTemporalPolicies(
    PolicyManager& policy_manager, const YAML::Node& node,
    const std::string& simulation_start_date) {
  if (!node.IsSequence()) {
    throw std::runtime_error("temporal_policies must be a list");
  }

  for (const auto& policy_node : node) {
    TemporalPolicy policy;

    // Name
    if (!policy_node["name"]) {
      throw std::runtime_error("temporal_policy must have a 'name' field");
    }
    policy.name = policy_node["name"].as<std::string>();

    policy.window = loadActiveWindow(policy_node, simulation_start_date);

    // Action
    policy.action = loadPolicyAction(policy_node);

    // Selection criteria (optional)
    if (policy_node["applies_to"]) {
      policy.applies_to = loadSelectionCriteria(policy_node["applies_to"]);
    }

    policy_manager.addTemporalPolicy(policy);
  }
}

}  // namespace june
