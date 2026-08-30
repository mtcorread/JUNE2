#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "loaders/config_loader.h"
#include "loaders/config_loader_detail.h"
#include "utils/filtered_csv.h"
#include "utils/mpi_logging.h"

namespace june {

namespace {

using ::june::logRank0;

// Map a frequency-group `rate_unit` literal (e.g. "per_month") onto the
// number of days it covers. Used to convert raw CSV rates into a daily
// probability (raw / divisor). Throws if the unit is not recognised.
double rateUnitDivisor(const std::string& rate_unit,
                       const std::string& fg_name) {
  if (rate_unit == "per_day") return 1.0;
  if (rate_unit == "per_week") return 7.0;
  if (rate_unit == "per_month") return 30.0;
  if (rate_unit == "per_year") return 365.0;
  throw std::runtime_error("frequency_group '" + fg_name +
                           "' has unknown rate_unit '" + rate_unit + "'");
}

// Parse a single `frequency_groups:` entry (one named CSV-backed rate source).
// Reads the CSV via csv::loadFilteredCSV, converts the per-person rate to a
// daily probability using the configured rate_unit, and populates the
// FrequencyGroup struct. Logs a one-line summary on rank 0.
FrequencyGroup parseFrequencyGroup(const std::string& name,
                                   const YAML::Node& gnode) {
  FrequencyGroup fg;
  fg.name = name;
  if (!gnode["csv"])
    throw std::runtime_error("frequency_group '" + fg.name +
                             "' missing required field: csv");
  fg.csv_path = gnode["csv"].as<std::string>();
  fg.rate_column = gnode["rate_column"] ? gnode["rate_column"].as<std::string>()
                                        : "events_per_month";
  fg.rate_unit =
      gnode["rate_unit"] ? gnode["rate_unit"].as<std::string>() : "per_month";

  const double divisor = rateUnitDivisor(fg.rate_unit, fg.name);

  csv::FilteredTable table = csv::loadFilteredCSV(fg.csv_path);
  bool has_rate_col = false;
  for (const auto& c : table.value_columns) {
    if (c == fg.rate_column) {
      has_rate_col = true;
      break;
    }
  }
  if (!has_rate_col)
    throw std::runtime_error("frequency_group '" + fg.name + "' CSV '" +
                             fg.csv_path + "' missing rate_column '" +
                             fg.rate_column + "'");

  for (const auto& row : table.rows) {
    FrequencyRow fr;
    fr.criteria = row.criteria;
    auto it = row.values.find(fg.rate_column);
    if (it == row.values.end() || it->second.empty()) continue;
    try {
      double raw = std::stod(it->second);
      fr.daily_probability = raw / divisor;
    } catch (...) {
      continue;
    }
    fg.rows.push_back(std::move(fr));
  }

  if (logRank0()) {
    std::cout << "[CoordEnc] Loaded frequency_group '" << fg.name << "' from '"
              << fg.csv_path << "' (" << fg.rows.size()
              << " rows, rate_column='" << fg.rate_column << "', rate_unit='"
              << fg.rate_unit << "')" << std::endl;
  }

  return fg;
}

// Parse a `{type, mean|p|count}` YAML node into an InviteDistribution.
// Strict variant used by `invite_distribution`: both the `type` key and the
// distribution-specific parameter are required (throws on absence). The
// `enc_name` is woven into error messages to identify the offending encounter.
void parseInviteDistributionStrict(const YAML::Node& dist_node,
                                   InviteDistribution& dist,
                                   const std::string& enc_name) {
  if (!dist_node["type"])
    throw std::runtime_error(
        "Coordinated encounter '" + enc_name +
        "' invite_distribution missing required field: type");
  dist.type = parseDistributionType(dist_node["type"].as<std::string>());

  if (dist.type == DistributionType::POISSON) {
    if (!dist_node["mean"])
      throw std::runtime_error("Poisson invite_distribution for '" + enc_name +
                               "' requires 'mean' parameter.");
    dist.mean = dist_node["mean"].as<double>();
  } else if (dist.type == DistributionType::BINOMIAL) {
    if (!dist_node["p"])
      throw std::runtime_error("Binomial invite_distribution for '" + enc_name +
                               "' requires 'p' parameter.");
    dist.p = dist_node["p"].as<double>();
  } else if (dist.type == DistributionType::FIXED) {
    if (!dist_node["count"])
      throw std::runtime_error("Fixed invite_distribution for '" + enc_name +
                               "' requires 'count' parameter.");
    dist.count = dist_node["count"].as<int>();
  }
}

// Parse the required scalar/list fields of a coordinated-encounter entry
// (name, network, trigger_slots, allowed_venues) plus the optional `enabled`
// flag. Throws if any required field is missing.
void parseEncounterRequiredFields(const YAML::Node& enc_node,
                                  CoordinatedEncounterDef& enc_def) {
  if (!enc_node["name"])
    throw std::runtime_error(
        "Coordinated encounter missing required field: name");
  enc_def.name = enc_node["name"].as<std::string>();

  if (enc_node["enabled"]) {
    enc_def.enabled = enc_node["enabled"].as<bool>();
  }

  if (!enc_node["network"])
    throw std::runtime_error("Coordinated encounter '" + enc_def.name +
                             "' missing required field: network");
  enc_def.network = enc_node["network"].as<std::string>();

  if (!enc_node["trigger_slots"])
    throw std::runtime_error("Coordinated encounter '" + enc_def.name +
                             "' missing required field: trigger_slots");
  if (enc_node["trigger_slots"].IsSequence()) {
    for (const auto& slot : enc_node["trigger_slots"]) {
      enc_def.trigger_slots.push_back(slot.as<std::string>());
    }
  }

  if (!enc_node["allowed_venues"])
    throw std::runtime_error("Coordinated encounter '" + enc_def.name +
                             "' missing required field: allowed_venues");
  if (enc_node["allowed_venues"].IsSequence()) {
    for (const auto& venue : enc_node["allowed_venues"]) {
      enc_def.allowed_venues.push_back(venue.as<std::string>());
    }
  }
}

// Resolve the per-encounter rate source: either an inline scalar
// `proposal_probability` (signalled by has_proposal_prob) or a named
// `frequency_group` referencing the surrounding config's frequency_groups
// map. Exactly one must be declared. Throws on unknown group, on both
// declared, or on neither.
void parseEncounterRateSource(
    const YAML::Node& enc_node, CoordinatedEncounterDef& enc_def,
    const std::unordered_map<std::string, FrequencyGroup>& frequency_groups,
    bool has_proposal_prob) {
  if (enc_node["frequency_group"]) {
    std::string fg_name = enc_node["frequency_group"].as<std::string>();
    if (!frequency_groups.count(fg_name)) {
      throw std::runtime_error("Coordinated encounter '" + enc_def.name +
                               "' references unknown frequency_group '" +
                               fg_name +
                               "' (not declared in frequency_groups block)");
    }
    enc_def.frequency_group = fg_name;
  }

  if (enc_def.frequency_group.has_value() && has_proposal_prob) {
    throw std::runtime_error(
        "Coordinated encounter '" + enc_def.name +
        "' declares both 'proposal_probability' and "
        "'frequency_group'; these are mutually exclusive. Remove "
        "proposal_probability when using a frequency_group.");
  }
  if (!enc_def.frequency_group.has_value() && !has_proposal_prob) {
    throw std::runtime_error(
        "Coordinated encounter '" + enc_def.name +
        "' missing rate source: specify either 'proposal_probability' "
        "or 'frequency_group'.");
  }
}

// Lenient counterpart to parseInviteDistributionStrict, used for
// `daily_max_distribution` blocks. Every key is optional: missing `type`
// leaves the existing default in place, and a missing distribution-specific
// parameter is silently skipped. No throws.
void parseDailyMaxDistribution(const YAML::Node& dmd_node,
                               InviteDistribution& dist) {
  if (dmd_node["type"]) {
    dist.type = parseDistributionType(dmd_node["type"].as<std::string>());
  }
  if (dist.type == DistributionType::POISSON && dmd_node["mean"]) {
    dist.mean = dmd_node["mean"].as<double>();
  } else if (dist.type == DistributionType::BINOMIAL && dmd_node["p"]) {
    dist.p = dmd_node["p"].as<double>();
  } else if (dist.type == DistributionType::FIXED && dmd_node["count"]) {
    dist.count = dmd_node["count"].as<int>();
  }
}

// Parse one entry from `coordinated_encounters.encounters[]`. Dispatches
// through the smaller per-section helpers (required fields, invite
// distribution, rate source, daily-max distribution) and reads the optional
// scalar/list fields (acceptance_probability, is_virtual /
// virtual_contact_matrix, min_attendees, priority, network_partner_filter).
CoordinatedEncounterDef parseEncounter(
    const YAML::Node& enc_node,
    const std::unordered_map<std::string, FrequencyGroup>& frequency_groups) {
  CoordinatedEncounterDef enc_def;
  parseEncounterRequiredFields(enc_node, enc_def);

  // proposal_probability is optional when frequency_group is set.
  // Validated by parseEncounterRateSource below.
  bool has_proposal_prob = static_cast<bool>(enc_node["proposal_probability"]);
  if (has_proposal_prob) {
    enc_def.proposal_probability =
        enc_node["proposal_probability"].as<double>();
  }

  if (!enc_node["invite_distribution"])
    throw std::runtime_error("Coordinated encounter '" + enc_def.name +
                             "' missing required field: invite_distribution");
  parseInviteDistributionStrict(enc_node["invite_distribution"],
                                enc_def.invite_distribution, enc_def.name);

  // acceptance_probability is optional. When absent, defaults to 1.0
  // (no refusal), which is appropriate whenever a frequency CSV is the
  // realized-rate source and any refusal dynamics are already baked in.
  if (enc_node["acceptance_probability"]) {
    enc_def.acceptance_probability =
        enc_node["acceptance_probability"].as<double>();
  }

  if (enc_node["is_virtual"]) {
    enc_def.is_virtual = enc_node["is_virtual"].as<bool>();
  }

  if (enc_def.is_virtual) {
    if (!enc_node["virtual_contact_matrix"])
      throw std::runtime_error(
          "Virtual coordinated encounter '" + enc_def.name +
          "' missing required field: virtual_contact_matrix");
    enc_def.virtual_contact_matrix =
        enc_node["virtual_contact_matrix"].as<std::string>();
  }

  if (enc_node["min_attendees"]) {
    enc_def.min_attendees = enc_node["min_attendees"].as<int>();
  }
  if (enc_node["priority"]) {
    enc_def.priority = enc_node["priority"].as<int>();
  }
  if (enc_node["network_partner_filter"]) {
    enc_def.network_partner_filter =
        enc_node["network_partner_filter"].as<std::string>();
  }

  parseEncounterRateSource(enc_node, enc_def, frequency_groups,
                           has_proposal_prob);

  if (enc_node["daily_max_distribution"]) {
    parseDailyMaxDistribution(enc_node["daily_max_distribution"],
                              enc_def.daily_max_distribution);
  }

  return enc_def;
}

}  // namespace

CoordinatedEncounterConfig ConfigLoader::loadCoordinatedEncounters(
    const std::string& filename) {
  CoordinatedEncounterConfig config;
  if (!std::filesystem::exists(filename)) {
    throw std::runtime_error(
        "coordinated_encounters file not found: " + filename +
        ". simulation.yaml's `coordinated_encounters_file` must point at "
        "an existing YAML — disabling the feature silently is not safe.");
  }
  YAML::Node root = YAML::LoadFile(filename);
  if (!root) {
    throw std::runtime_error(
        "coordinated_encounters file " + filename +
        " parsed to a null root node (file empty or malformed).");
  }
  if (!root["coordinated_encounters"]) return config;
  const auto& ce_node = root["coordinated_encounters"];

  if (ce_node["enabled"]) {
    config.enabled = ce_node["enabled"].as<bool>();
  }
  if (ce_node["log_commitments"]) {
    config.log_commitments = ce_node["log_commitments"].as<bool>();
  }

  // Optional: frequency_groups block. Each group specifies a CSV whose
  // per-person rate overrides the scalar proposal_probability for any
  // encounter that declares `frequency_group: "<name>"`.
  if (ce_node["frequency_groups"]) {
    for (const auto& kv : ce_node["frequency_groups"]) {
      std::string name = kv.first.as<std::string>();
      config.frequency_groups[name] = parseFrequencyGroup(name, kv.second);
    }
  }

  if (!ce_node["encounters"]) {
    throw std::runtime_error(
        "coordinated_encounters block must define 'encounters' list.");
  }
  for (const auto& enc_node : ce_node["encounters"]) {
    config.encounters.push_back(
        parseEncounter(enc_node, config.frequency_groups));
  }

  // Sort encounters by priority (lower number = higher precedence =
  // processed first)
  std::sort(
      config.encounters.begin(), config.encounters.end(),
      [](const CoordinatedEncounterDef& a, const CoordinatedEncounterDef& b) {
        return a.priority < b.priority;
      });

  // Optional follow rules. Independent of the encounters above: they work even
  // when coordinated_encounters.enabled is false. A scenario writes either a
  // single `follow:` block (sugar for one rule) or a `follows:` sequence, never
  // both. Each rule resolves independently in list order.
  // eligibility / host_eligibility take the engine's ordinary person criteria,
  // the same {property, operator, value} entries a policy's applies_to or a
  // frequency group's filters take. A rule that wants toddlers says
  // `property: age, operator: "<", value: 5`, and nothing in the loader knows
  // that age is special.
  auto parseEligibility = [](const YAML::Node& n, const std::string& which) {
    if (!n.IsSequence())
      throw std::runtime_error(
          "follow." + which +
          " must be a list of {property, operator, value} entries, e.g. "
          "- property: age / operator: \"<\" / value: 5");
    std::vector<SelectionCriterion> out;
    config_detail::parseSelectionCriteria(n, out);
    return out;
  };
  auto parseFollowRule = [&](const YAML::Node& fn) {
    FollowConfig f;
    if (fn["name"]) f.name = fn["name"].as<std::string>();
    if (fn["enabled"]) f.enabled = fn["enabled"].as<bool>();
    if (fn["pool_venue_type"])
      f.pool_venue_type = fn["pool_venue_type"].as<std::string>();
    if (fn["network"]) f.network = fn["network"].as<std::string>();
    if (fn["encounter_type"])
      f.encounter_type = fn["encounter_type"].as<std::string>();
    if (fn["probability"]) f.probability = fn["probability"].as<double>();
    if (fn["log"]) f.log = fn["log"].as<bool>();

    if (fn["establishment"]) {
      std::string e = fn["establishment"].as<std::string>();
      if (e == "stochastic")
        f.establishment = FollowConfig::Establishment::Stochastic;
      else if (e == "criteria")
        f.establishment = FollowConfig::Establishment::Criteria;
      else
        throw std::runtime_error(
            "follow.establishment must be 'stochastic' or 'criteria', got '" +
            e + "'");
    }
    if (fn["span"]) {
      std::string s = fn["span"].as<std::string>();
      if (s == "hop")
        f.span = FollowConfig::Span::Hop;
      else if (s == "standing")
        f.span = FollowConfig::Span::Standing;
      else
        throw std::runtime_error(
            "follow.span must be 'hop' or 'standing', got '" + s + "'");
    }

    if (fn["eligibility"])
      f.follower = parseEligibility(fn["eligibility"], "eligibility");
    if (fn["host_eligibility"])
      f.host = parseEligibility(fn["host_eligibility"], "host_eligibility");

    if (fn["activity_exceptions"])
      for (const auto& a : fn["activity_exceptions"])
        f.activity_exceptions.push_back(a.as<std::string>());
    if (fn["venue_exceptions"])
      for (const auto& m : fn["venue_exceptions"])
        f.venue_exceptions.push_back(m.as<std::string>());
    if (fn["follower_activity_exceptions"])
      for (const auto& a : fn["follower_activity_exceptions"])
        f.follower_activity_exceptions.push_back(a.as<std::string>());
    return f;
  };

  if (ce_node["follow"] && ce_node["follows"]) {
    throw std::runtime_error(
        "coordinated_encounters: set either 'follow' (one rule) or 'follows' "
        "(a list), not both");
  }
  if (ce_node["follow"]) {
    config.follows.push_back(parseFollowRule(ce_node["follow"]));
  } else if (ce_node["follows"]) {
    for (const auto& fn : ce_node["follows"])
      config.follows.push_back(parseFollowRule(fn));
  }

  // A rule's name defaults to its position and keys its checkpoint shard, so it
  // must be a valid HDF5 group token and unique across the list.
  std::unordered_set<std::string> seen_names;
  for (size_t i = 0; i < config.follows.size(); ++i) {
    FollowConfig& f = config.follows[i];
    if (f.name.empty()) f.name = std::to_string(i);
    if (f.name.find('/') != std::string::npos)
      throw std::runtime_error("follow rule name '" + f.name +
                               "' must not contain '/'");
    if (!seen_names.insert(f.name).second)
      throw std::runtime_error("duplicate follow rule name '" + f.name + "'");
  }

  return config;
}

}  // namespace june
