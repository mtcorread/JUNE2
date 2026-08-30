#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "loaders/config_loader.h"
#include "loaders/config_loader_detail.h"
#include "utils/mpi_logging.h"

namespace june {

namespace {

using ::june::logRank0;
using ::june::config_detail::parseSelectionCriteria;

// Parse a vaccine efficacy node: either a flat `disease -> scalar` map
// (taken as 0-100 age-uniform efficacy) or a nested `disease -> {age-range
// -> value}` map (each age-range encoded as "min-max"). Used for both
// infection_efficacy and symptom_efficacy on a DoseConfig.
std::unordered_map<std::string, std::vector<AgeEfficacy>> parseDoseEfficacies(
    const YAML::Node& node) {
  std::unordered_map<std::string, std::vector<AgeEfficacy>> res;
  if (!node) return res;
  for (const auto& eff_kv : node) {
    std::string disease = eff_kv.first.as<std::string>();
    if (eff_kv.second.IsMap()) {
      for (const auto& age_kv : eff_kv.second) {
        std::string age_range = age_kv.first.as<std::string>();
        size_t dash = age_range.find('-');
        if (dash != std::string::npos) {
          AgeEfficacy ae;
          ae.min_age = std::stoi(age_range.substr(0, dash));
          ae.max_age = std::stoi(age_range.substr(dash + 1));
          ae.efficacy = age_kv.second.as<double>();
          res[disease].push_back(ae);
        }
      }
    } else {
      res[disease].push_back({0, 100, eff_kv.second.as<double>()});
    }
  }
  return res;
}

// Parse one entry from a vaccine's `doses:` sequence. The position in the
// sequence becomes DoseConfig::number.
DoseConfig parseDose(int number, const YAML::Node& d_node) {
  DoseConfig dc;
  dc.number = number;
  if (d_node["days_to_effective"])
    dc.days_to_effective = d_node["days_to_effective"].as<double>();
  if (d_node["days_to_waning"])
    dc.days_to_waning = d_node["days_to_waning"].as<double>();
  if (d_node["days_to_finished"])
    dc.days_to_finished = d_node["days_to_finished"].as<double>();
  if (d_node["waning_factor"])
    dc.waning_factor = d_node["waning_factor"].as<double>();
  dc.infection_efficacy = parseDoseEfficacies(d_node["infection_efficacy"]);
  dc.symptom_efficacy = parseDoseEfficacies(d_node["symptom_efficacy"]);
  return dc;
}

// Parse one entry from the `vaccines:` map (a named vaccine and its dose
// schedule).
VaccineConfig parseVaccine(const std::string& name,
                           const YAML::Node& vacc_node) {
  VaccineConfig vc;
  vc.name = name;
  if (vacc_node["doses"]) {
    const auto& doses_node = vacc_node["doses"];
    for (size_t i = 0; i < doses_node.size(); ++i) {
      vc.doses.push_back(parseDose(static_cast<int>(i), doses_node[i]));
    }
  }
  return vc;
}

// Parse one entry from the `campaigns:` map (a named vaccination campaign).
VaccinationCampaignConfig parseVaccinationCampaign(
    const std::string& name, const YAML::Node& camp_node) {
  VaccinationCampaignConfig camp;
  camp.name = name;
  if (camp_node["start_date"])
    camp.start_date = camp_node["start_date"].as<std::string>();
  if (camp_node["end_date"])
    camp.end_date = camp_node["end_date"].as<std::string>();
  if (camp_node["vaccine_type"])
    camp.vaccine_type = camp_node["vaccine_type"].as<std::string>();
  if (camp_node["dose_sequence"])
    camp.dose_sequence = camp_node["dose_sequence"].as<std::vector<int>>();
  if (camp_node["days_to_next_dose"])
    camp.days_to_next_dose =
        camp_node["days_to_next_dose"].as<std::vector<double>>();
  if (camp_node["daily_coverage"])
    camp.daily_coverage = camp_node["daily_coverage"].as<double>();

  if (camp_node["selection"]) {
    parseSelectionCriteria(camp_node["selection"], camp.selection_criteria);
  }
  if (camp_node["last_dose_type_filter"]) {
    camp.last_dose_type_filter =
        camp_node["last_dose_type_filter"].as<std::vector<std::string>>();
  }
  return camp;
}

}  // namespace

VaccinationConfig ConfigLoader::loadVaccination(const std::string& filename) {
  VaccinationConfig config;
  if (!std::filesystem::exists(filename)) {
    if (logRank0()) {
      std::cout << "Warning: " << filename
                << " not found. Vaccination will be disabled." << std::endl;
    }
    config.enabled = false;
    return config;
  }
  try {
    YAML::Node root = YAML::LoadFile(filename);
    if (!root) return config;

    if (root["enabled"]) {
      config.enabled = root["enabled"].as<bool>();
    } else {
      config.enabled = true;  // Fallback to true if file exists but no flag
    }

    if (root["vaccines"]) {
      for (const auto& vacc_kv : root["vaccines"]) {
        std::string name = vacc_kv.first.as<std::string>();
        config.vaccines[name] = parseVaccine(name, vacc_kv.second);
      }
    }

    if (root["campaigns"]) {
      for (const auto& camp_kv : root["campaigns"]) {
        config.campaigns.push_back(parseVaccinationCampaign(
            camp_kv.first.as<std::string>(), camp_kv.second));
      }
    }

  } catch (const std::exception& e) {
    std::cerr << "Warning: Could not load " << filename << ": " << e.what()
              << std::endl;
    config.enabled = false;
  }
  return config;
}

}  // namespace june
