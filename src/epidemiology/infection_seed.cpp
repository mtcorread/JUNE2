#include "epidemiology/infection_seed.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <unordered_map>

#include "epidemiology/disease.h"
#include "epidemiology/seeding/seed_cluster_planner.h"
#include "epidemiology/seeding/seed_offer_exchange.h"
#include "epidemiology/seeding/seed_selector.h"
#include "utils/deterministic_rng.h"
#include "utils/filtered_csv.h"
#include "utils/random.h"

namespace june {

namespace {

// How a report should name one budget: the labels of the target groups it draws
// from, joined where a scalar budget spans several. Empty when the seed keeps
// no labels — bulk CSV seeds build a criteria profile per row and have none to
// keep — and the report falls back to the budget index alone.
std::string budgetLabel(const SeedBudget& budget,
                        const std::vector<SeedTargetGroup>& target_groups) {
  std::string label;
  for (size_t group_index : budget.eligible_target_groups) {
    if (group_index >= target_groups.size()) continue;
    const std::string& group_label = target_groups[group_index].label;
    if (group_label.empty()) continue;
    if (!label.empty()) label += ", ";
    label += group_label;
  }
  return label;
}

}  // namespace

// =============================================================================
// Configuration Loader Implementation
// =============================================================================

std::vector<SelectionCriterion> InfectionSeedConfigLoader::parseCriterion(
    const std::string& key, const std::string& val) {
  std::string property = key;
  // Map age_groups to age for internal property evaluation
  if (property == "age_groups") {
    property = "age";
    // Silent mapping: internal config normalization
  }

  std::vector<SelectionCriterion> results;

  // Support ranges like "18-30" or "65-100"
  size_t dash = val.find('-');
  if (dash != std::string::npos && dash > 0 && dash < val.size() - 1) {
    try {
      SelectionCriterion c_min, c_max;
      c_min.property_path = property;
      c_min.operator_type = ">=";
      c_min.value = std::stoi(val.substr(0, dash));

      c_max.property_path = property;
      c_max.operator_type = "<=";
      c_max.value = std::stoi(val.substr(dash + 1));

      results.push_back(c_min);
      results.push_back(c_max);
      return results;
    } catch (...) {
      // If parsing failed, fall back to treats it as a single string
    }
  }

  SelectionCriterion c;
  c.property_path = property;
  c.operator_type = "==";

  // Try numeric/bool first, fallback to string
  try {
    if (val == "true" || val == "True")
      c.value = true;
    else if (val == "false" || val == "False")
      c.value = false;
    else if (val.find('.') != std::string::npos) {
      c.value = std::stod(val);
    } else {
      c.value = std::stoi(val);
    }
  } catch (...) {
    c.value = val;
  }

  results.push_back(c);
  return results;
}

InfectionSeedType InfectionSeedConfigLoader::parseSeedType(
    const std::string& type_str) {
  std::string lower_type = type_str;
  std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
                 ::tolower);

  if (lower_type == "uniform") return InfectionSeedType::UNIFORM;
  if (lower_type == "exact") return InfectionSeedType::EXACT;
  if (lower_type == "clustered") return InfectionSeedType::CLUSTERED;

  throw std::runtime_error("Unknown infection seed type: " + type_str);
}

void InfectionSeedConfigLoader::loadBulkCsvSeeds(const std::string& csv_path,
                                                 InfectionSeedConfig& config) {
  config.bulk_csv_path = csv_path;
  csv::FilteredTable table = csv::loadFilteredCSV(csv_path);

  auto has_col = [&](const std::string& name) {
    for (const auto& c : table.value_columns)
      if (c == name) return true;
    return false;
  };
  if (!has_col("name") || !has_col("date") || !has_col("type")) {
    throw std::runtime_error(
        "Bulk CSV missing required columns: name, date, type");
  }

  auto get = [](const csv::FilteredRow& r,
                const std::string& name) -> std::string {
    auto it = r.values.find(name);
    return it == r.values.end() ? "" : it->second;
  };

  struct SeedKey {
    std::string name;
    std::string date;
    InfectionSeedType type;
    std::string trajectory_key;
    std::string start_symptom;
    bool operator<(const SeedKey& o) const {
      return std::tie(name, date, type, trajectory_key, start_symptom) <
             std::tie(o.name, o.date, o.type, o.trajectory_key,
                      o.start_symptom);
    }
  };

  struct SeedDraft {
    InfectionSeedEvent event;
    std::vector<std::pair<std::vector<SelectionCriterion>, size_t>> profiles;
    std::map<std::string, std::vector<std::pair<size_t, int>>> unit_pending;

    size_t getOrCreateGroup(const std::vector<SelectionCriterion>& profile) {
      for (const auto& [p, idx] : profiles) {
        if (p.size() != profile.size()) continue;
        bool match = true;
        for (size_t i = 0; i < p.size(); ++i) {
          const auto& a = p[i];
          const auto& b = profile[i];
          if (a.property_path != b.property_path ||
              a.operator_type != b.operator_type || a.value != b.value) {
            match = false;
            break;
          }
        }
        if (match) return idx;
      }
      size_t new_idx = event.structured_config.target_groups.size();
      profiles.push_back({profile, new_idx});
      SeedTargetGroup group;
      group.criteria = profile;
      event.structured_config.target_groups.push_back(group);
      return new_idx;
    }
  };

  std::map<SeedKey, SeedDraft> drafts;

  int row_num = 0;
  for (const auto& row : table.rows) {
    ++row_num;
    std::string name_val = get(row, "name");
    std::string date_val = get(row, "date");
    std::string type_val = get(row, "type");
    if (name_val.empty() || date_val.empty() || type_val.empty()) {
      throw std::runtime_error(
          "Bulk seed CSV '" + csv_path + "' row " + std::to_string(row_num) +
          " missing required value in name/date/type (got name='" + name_val +
          "', date='" + date_val + "', type='" + type_val + "')");
    }

    SeedKey key = {name_val, date_val, parseSeedType(type_val),
                   get(row, "trajectory_key"), get(row, "start_symptom")};
    auto& draft = drafts[key];
    if (draft.event.name.empty()) {
      draft.event.name = key.name;
      draft.event.date_time = key.date;
      draft.event.type = key.type;
      draft.event.trajectory_key = key.trajectory_key;
      draft.event.start_symptom = key.start_symptom;
    }

    if (key.type == InfectionSeedType::UNIFORM) {
      std::string pc = get(row, "cases_per_capita");
      if (!pc.empty()) {
        try {
          draft.event.uniform_config.cases_per_capita = std::stod(pc);
        } catch (const std::exception&) {
          throw std::runtime_error("Bulk seed CSV '" + csv_path + "' row " +
                                   std::to_string(row_num) +
                                   " has non-numeric cases_per_capita='" + pc +
                                   "'");
        }
      }
      draft.event.attribute_filters.insert(draft.event.attribute_filters.end(),
                                           row.criteria.begin(),
                                           row.criteria.end());
    } else {
      std::string geo_level = get(row, "geo_level");
      if (!geo_level.empty()) {
        draft.event.structured_config.geo_level = geo_level;
      }

      std::string geo_unit = get(row, "geo_unit");
      int cases = 0;
      std::string cases_val = get(row, "cases");
      if (!cases_val.empty()) {
        try {
          cases = std::stoi(cases_val);
        } catch (const std::exception&) {
          throw std::runtime_error("Bulk seed CSV '" + csv_path + "' row " +
                                   std::to_string(row_num) +
                                   " has non-integer cases='" + cases_val +
                                   "'");
        }
      }

      size_t profile_idx = draft.getOrCreateGroup(row.criteria);

      if (!geo_unit.empty() && cases > 0) {
        draft.unit_pending[geo_unit].push_back({profile_idx, cases});
      }
    }
  }

  // Finalize structured seeds
  for (auto& [key, draft] : drafts) {
    if (draft.event.type != InfectionSeedType::UNIFORM) {
      for (const auto& [unit_id, groups] : draft.unit_pending) {
        // One budget per distinct criteria set, in group order.
        UnitCases uc;
        uc.unit_id = unit_id;
        for (size_t g_idx = 0;
             g_idx < draft.event.structured_config.target_groups.size();
             ++g_idx) {
          SeedBudget budget;
          budget.eligible_target_groups = {g_idx};
          uc.budgets.push_back(budget);
        }
        for (const auto& [g_idx, count] : groups) {
          uc.budgets[g_idx].cases += count;
        }
        draft.event.structured_config.unit_cases.push_back(uc);
      }
    }
    config.seeds.push_back(draft.event);
  }
  // CSV parse summary removed (not actionable)
}

InfectionSeedConfig InfectionSeedConfigLoader::loadFromFile(
    const std::string& filename) {
  InfectionSeedConfig config;

  try {
    YAML::Node root = YAML::LoadFile(filename);

    if (root["global_parameters"]) {
      auto global = root["global_parameters"];
      if (global["base_cases_per_capita"]) {
        config.global_params.base_cases_per_capita =
            global["base_cases_per_capita"].as<double>();
      }
    }

    if (root["bulk_csv"]) {
      loadBulkCsvSeeds(root["bulk_csv"].as<std::string>(), config);
    }

    if (root["infection_seeds"]) {
      for (const auto& seed_node : root["infection_seeds"]) {
        InfectionSeedEvent seed;
        seed.name = seed_node["name"].as<std::string>();
        seed.type = parseSeedType(seed_node["type"].as<std::string>());
        seed.date_time = seed_node["date"].as<std::string>();

        if (seed_node["trajectory_key"])
          seed.trajectory_key = seed_node["trajectory_key"].as<std::string>();
        if (seed_node["start_symptom"])
          seed.start_symptom = seed_node["start_symptom"].as<std::string>();

        if (seed_node["parameters"]) {
          auto params = seed_node["parameters"];
          seed.seed_strength = params["seed_strength"].as<double>(
              config.global_params.default_seed_strength);

          if (params["attribute_filters"]) {
            auto filters = params["attribute_filters"];
            for (auto it = filters.begin(); it != filters.end(); ++it) {
              auto cs = parseCriterion(it->first.as<std::string>(),
                                       it->second.as<std::string>());
              seed.attribute_filters.insert(seed.attribute_filters.end(),
                                            cs.begin(), cs.end());
            }
          }
        }

        if (seed.type == InfectionSeedType::UNIFORM) {
          if (seed_node["parameters"]) {
            auto params = seed_node["parameters"];
            if (params["cases_per_capita_multiplier"]) {
              seed.uniform_config.cases_per_capita =
                  config.global_params.base_cases_per_capita *
                  params["cases_per_capita_multiplier"].as<double>();
            }
          }
        } else {
          seed.structured_config.geo_level =
              seed_node["geo_level"].as<std::string>("MGU");

          if (seed_node["parameters"]) {
            auto params = seed_node["parameters"];

            // Age groups from YAML are converted to generic TargetGroups
            if (params["age_groups"]) {
              for (const auto& age_str : params["age_groups"]) {
                std::string s = age_str.as<std::string>();
                SeedTargetGroup g;
                g.label = s;
                auto cs = parseCriterion("age", s);
                g.criteria.insert(g.criteria.end(), cs.begin(), cs.end());
                seed.structured_config.target_groups.push_back(g);
              }
            }

            if (params["units"]) {
              for (const auto& entry : params["units"]) {
                const size_t group_count =
                    seed.structured_config.target_groups.size();
                UnitCases uc;
                uc.unit_id = entry.first.as<std::string>();
                if (entry.second.IsSequence()) {
                  // A per-group list: each declared group gets its own budget.
                  if (entry.second.size() != group_count) {
                    throw std::runtime_error(
                        "Infection seed '" + seed.name + "', unit '" +
                        uc.unit_id + "': per-group case list has " +
                        std::to_string(entry.second.size()) +
                        " entries but the seed declares " +
                        std::to_string(group_count) + " target groups");
                  }
                  size_t group_index = 0;
                  for (const auto& cases : entry.second) {
                    SeedBudget budget;
                    budget.cases = static_cast<int>(cases.as<double>());
                    budget.eligible_target_groups = {group_index++};
                    uc.budgets.push_back(budget);
                  }
                } else {
                  // A scalar: one budget, drawn from anyone matching any
                  // declared group (or from anyone, if none are declared).
                  SeedBudget budget;
                  budget.cases = static_cast<int>(entry.second.as<double>());
                  budget.eligible_target_groups.resize(group_count);
                  std::iota(budget.eligible_target_groups.begin(),
                            budget.eligible_target_groups.end(), size_t{0});
                  uc.budgets.push_back(budget);
                }
                seed.structured_config.unit_cases.push_back(uc);
              }
            }
          }
        }
        config.seeds.push_back(seed);
      }
    }
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to load infection seed config: " +
                             std::string(e.what()));
  }
  return config;
}

// =============================================================================
// Infection Seeder Implementation
// =============================================================================

InfectionSeeder::InfectionSeeder(WorldState& world, const Disease* disease,
                                 const InfectionSeedConfig& config,
                                 EventLogger* event_logger, uint64_t base_seed)
    : world_(world),
      disease_(disease),
      config_(config),
      event_logger_(event_logger),
      current_simulation_time_(0.0),
      base_seed_(base_seed) {}

std::vector<PersonId> InfectionSeeder::seedInfections(
    const std::string& current_datetime, double simulation_time) {
  current_simulation_time_ = simulation_time;
  std::vector<PersonId> all_infected;
  seed_shortfalls_.clear();

  for (const auto& seed : config_.seeds) {
    // Standardized comparison: skip whitespace/case if needed,
    // though currently matching exact string.
    if (seed.date_time == current_datetime) {
      std::string seed_key =
          seed.name + "|" + seed.trajectory_key + "|" + seed.start_symptom;
      if (applied_seeds_.count(seed_key) > 0) {
        continue;
      }
      std::vector<PersonId> infected = applySeed(seed);
      applied_seeds_.insert(seed_key);
      all_infected.insert(all_infected.end(), infected.begin(), infected.end());

      // Per-seed message removed; global count reported by Simulator
    }
  }

  return all_infected;
}

std::vector<PersonId> InfectionSeeder::applySeed(
    const InfectionSeedEvent& seed) {
  switch (seed.type) {
    case InfectionSeedType::UNIFORM:
      return applyUniformSeed(seed);
    case InfectionSeedType::EXACT:
      return applyExactSeed(seed);
    case InfectionSeedType::CLUSTERED:
      return applyClusteredSeed(seed);
    default:
      throw std::runtime_error("Unknown seed type");
  }
}

std::vector<PersonId> InfectionSeeder::applyUniformSeed(
    const InfectionSeedEvent& seed) {
  std::vector<PersonId> infected_ids;

  double cases_per_capita =
      seed.uniform_config.cases_per_capita * seed.seed_strength;

  // Target rate message removed (not actionable)

  // MPI-reproducible seeding: each person gets a per-person deterministic
  // decision based on their ID. This ensures the same person is always
  // seeded regardless of which rank owns them or the local population size.
  uint64_t seed_name_hash = std::hash<std::string>{}(seed.name);
  uint64_t time_bits = static_cast<uint64_t>(current_simulation_time_ * 1000);

  for (auto& person : world_.people) {
    if (person.infection != nullptr) continue;
    if (person.getSusceptibility(current_simulation_time_,
                                 disease_->getName()) < 0.01)
      continue;
    if (!matchesAttributes(&person, seed.attribute_filters)) continue;

    // Per-person deterministic draw keyed to person ID
    SplitMix64 prng(mix_seed(base_seed_, person.id, seed_name_hash, time_bits));
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double rng_val = dist(prng);
    bool seeded = rng_val < cases_per_capita;
    if (seeded) {
      infectPerson(&person, seed.trajectory_key, seed.start_symptom);
      if (person.infection != nullptr) {
        infected_ids.push_back(person.id);
      }
    }
  }

  // Seeded count removed; global count reported by Simulator

  return infected_ids;
}

std::vector<PersonId> InfectionSeeder::applyExactSeed(
    const InfectionSeedEvent& seed) {
  // A structured seed's count is absolute, so it cannot be resolved from one
  // rank's slice of a unit: a unit above the partition level is split across
  // ranks. Each rank instead offers its own candidates, keyed off the run seed
  // and the person, and every rank then selects the same winners from the
  // pooled offers. Each rank infects only the winners it holds.
  // Target groups may overlap, so a candidate is offered against every budget
  // it matches but wins at most one case: the budgets of a unit are resolved
  // together, and the budget that loses a contested candidate refills from its
  // next-best offer.
  // No rank may return early: every rank walks every unit and reaches the one
  // exchange below, contributing an empty slice where it holds nothing.
  struct ExactUnit {
    std::string unit_id;
    std::vector<int> targets;  // cases per budget, after seed strength
    std::vector<std::string> budget_labels;  // for reports only, may be empty
    uint32_t first_slot = 0;
  };
  struct LocalCandidate {
    Person* person = nullptr;
    std::vector<uint32_t> matched_budgets;
  };

  std::vector<ExactUnit> units;
  std::vector<uint32_t> unit_of_slot;
  std::vector<SeedOffer> local_offers;
  std::unordered_map<PersonId, Person*> local_candidates;

  const uint64_t event_base =
      mix_seed(base_seed_, std::hash<std::string>{}(seed.name));

  for (const auto& unit_case : seed.structured_config.unit_cases) {
    ExactUnit unit;
    unit.unit_id = unit_case.unit_id;
    unit.first_slot = static_cast<uint32_t>(unit_of_slot.size());
    int total_target = 0;
    for (const auto& budget : unit_case.budgets) {
      unit.targets.push_back(
          static_cast<int>(budget.cases * seed.seed_strength));
      unit.budget_labels.push_back(
          budgetLabel(budget, seed.structured_config.target_groups));
      total_target += unit.targets.back();
    }
    unit_of_slot.resize(unit_of_slot.size() + unit.targets.size(),
                        static_cast<uint32_t>(units.size()));
    units.push_back(unit);
    if (total_target <= 0) continue;

    const uint64_t unit_hash = std::hash<std::string>{}(unit_case.unit_id);
    // One pass over the unit: the event's filters are evaluated once per
    // person, and each candidate's budgets are known together, which is what
    // makes the overlap below countable.
    std::vector<LocalCandidate> candidates;
    std::vector<int> overlapping_per_budget(unit.targets.size(), 0);
    for (Person* person : world_.getPeopleInUnit(
             seed.structured_config.geo_level, unit_case.unit_id)) {
      if (person->infection != nullptr) continue;
      if (person->getSusceptibility(current_simulation_time_,
                                    disease_->getName()) < 0.01)
        continue;
      if (!matchesAttributes(person, seed.attribute_filters)) continue;

      LocalCandidate candidate{person, {}};
      for (size_t budget_index = 0; budget_index < unit_case.budgets.size();
           ++budget_index) {
        if (unit.targets[budget_index] > 0 &&
            unit_case.budgets[budget_index].accepts(
                *person, &world_, seed.structured_config.target_groups)) {
          candidate.matched_budgets.push_back(
              static_cast<uint32_t>(budget_index));
        }
      }
      if (candidate.matched_budgets.empty()) continue;
      if (candidate.matched_budgets.size() > 1) {
        for (uint32_t budget_index : candidate.matched_budgets) {
          ++overlapping_per_budget[budget_index];
        }
      }
      candidates.push_back(std::move(candidate));
    }

    std::vector<std::vector<SeedOffer>> offers_per_budget(unit.targets.size());
    for (const LocalCandidate& candidate : candidates) {
      for (uint32_t budget_index : candidate.matched_budgets) {
        offers_per_budget[budget_index].push_back(
            {mix_seed(event_base, unit_hash, budget_index,
                      static_cast<uint64_t>(candidate.person->id)),
             candidate.person->id, unit.first_slot + budget_index});
      }
    }

    for (size_t budget_index = 0; budget_index < unit.targets.size();
         ++budget_index) {
      // Offer at most the budget plus the candidates another budget could
      // take from it, and no more than the unit places in total: a locally
      // better candidate is lost only to a budget it also matches, and every
      // such loss spends a case elsewhere in the unit. That keeps every
      // possible winner while the exchanged message stays proportional to the
      // budgets, not to the population — the cap is what holds that under
      // nested groups, where every candidate of the narrow band overlaps the
      // wide one. See ADR 0011.
      std::vector<SeedOffer>& offers = offers_per_budget[budget_index];
      const size_t depth =
          seedOfferDepth(unit.targets, overlapping_per_budget, budget_index);
      if (offers.size() > depth) {
        std::nth_element(offers.begin(), offers.begin() + depth, offers.end(),
                         [](const SeedOffer& a, const SeedOffer& b) {
                           if (a.key != b.key) return a.key < b.key;
                           return a.person_id < b.person_id;
                         });
        offers.resize(depth);
      }
      local_offers.insert(local_offers.end(), offers.begin(), offers.end());
    }
    // Only the people still offered need holding: a truncated-away candidate
    // cannot win, so the map stays the size of the budgets rather than of the
    // unit's population.
    std::unordered_map<PersonId, Person*> person_by_id;
    for (const LocalCandidate& candidate : candidates) {
      person_by_id[candidate.person->id] = candidate.person;
    }
    for (const auto& offers : offers_per_budget) {
      for (const auto& offer : offers) {
        local_candidates[offer.person_id] = person_by_id[offer.person_id];
      }
    }
  }

  std::vector<SeedOffer> pooled_offers =
      seed_offer_exchange_ ? seed_offer_exchange_->pool(local_offers)
                           : local_offers;

  std::vector<std::vector<SeedOffer>> offers_by_unit(units.size());
  for (const auto& offer : pooled_offers) {
    if (offer.budget_slot >= unit_of_slot.size()) continue;
    const uint32_t unit_index = unit_of_slot[offer.budget_slot];
    offers_by_unit[unit_index].push_back(
        {offer.key, offer.person_id,
         offer.budget_slot - units[unit_index].first_slot});
  }

  std::vector<PersonId> infected_ids;
  for (size_t unit_index = 0; unit_index < units.size(); ++unit_index) {
    const ExactUnit& unit = units[unit_index];
    SeedSelection selection =
        selectSeedWinners({offers_by_unit[unit_index]}, unit.targets);
    // The pooled offers are every rank's, so a gap here is a gap everywhere,
    // not merely on this rank. Two things make one: people another budget of
    // the unit took first, and people who do not exist. The record carries both
    // so the report need not guess which. Never fatal.
    for (size_t budget_index = 0; budget_index < unit.targets.size();
         ++budget_index) {
      if (selection.filled_per_budget[budget_index] <
          unit.targets[budget_index]) {
        seed_shortfalls_.push_back({seed.name, seed.structured_config.geo_level,
                                    unit.unit_id, budget_index,
                                    unit.budget_labels[budget_index],
                                    unit.targets[budget_index],
                                    selection.filled_per_budget[budget_index],
                                    selection.lost_per_budget[budget_index],
                                    /*lost_to_earlier_declared=*/false});
      }
    }
    for (const auto& assignment : selection.chosen) {
      auto held = local_candidates.find(assignment.person_id);
      if (held == local_candidates.end()) continue;  // another rank holds them
      if (held->second->infection != nullptr) continue;
      infectPerson(held->second, seed.trajectory_key, seed.start_symptom);
      if (held->second->infection != nullptr) {
        infected_ids.push_back(assignment.person_id);
      }
    }
  }
  return infected_ids;
}

std::vector<PersonId> InfectionSeeder::applyClusteredSeed(
    const InfectionSeedEvent& seed) {
  // Like an exact seed, a clustered seed's count is absolute, so it cannot be
  // resolved from one rank's slice of a unit above the partition level. Each
  // rank offers the households it holds and every rank then replays the same
  // greedy fill over the pooled offers, infecting only the people it holds.
  // A household is offered as one offer per (member, budget the member
  // matches), sharing the household's key; a member matching no budget is
  // offered against the unit's slot 0, so it counts towards the household's
  // size but never its matched members.
  // No rank may return early: every rank walks every unit and reaches the one
  // exchange below, contributing an empty slice where it holds nothing.
  struct ClusterUnit {
    std::string unit_id;
    std::vector<int> targets;  // cases per budget, after seed strength
    std::vector<std::string> budget_labels;  // for reports only, may be empty
    uint32_t first_slot = 0;  // targets.size() + 1 slots, 0 = no budget
  };
  struct LocalMember {
    Person* person = nullptr;
    std::vector<uint32_t> matched_budgets;
  };
  struct LocalHousehold {
    uint64_t key = 0;
    std::vector<LocalMember> members;
    size_t matched = 0;
  };

  const uint64_t event_base =
      mix_seed(base_seed_, std::hash<std::string>{}(seed.name));

  std::vector<ClusterUnit> units;
  std::vector<uint32_t> unit_of_slot;
  std::vector<SeedOffer> local_offers;
  std::unordered_map<PersonId, Person*> local_candidates;

  for (const auto& unit_case : seed.structured_config.unit_cases) {
    ClusterUnit unit;
    unit.unit_id = unit_case.unit_id;
    unit.first_slot = static_cast<uint32_t>(unit_of_slot.size());
    int total_target = 0;
    for (const auto& budget : unit_case.budgets) {
      unit.targets.push_back(
          static_cast<int>(budget.cases * seed.seed_strength));
      unit.budget_labels.push_back(
          budgetLabel(budget, seed.structured_config.target_groups));
      total_target += unit.targets.back();
    }
    unit_of_slot.resize(unit_of_slot.size() + unit.targets.size() + 1,
                        static_cast<uint32_t>(units.size()));
    units.push_back(unit);
    if (total_target <= 0) continue;

    const uint64_t unit_hash = std::hash<std::string>{}(unit_case.unit_id);
    std::map<VenueId, LocalHousehold> households;
    for (Person* person : world_.getPeopleInUnit(
             seed.structured_config.geo_level, unit_case.unit_id)) {
      if (person->infection != nullptr) continue;
      if (person->getSusceptibility(current_simulation_time_,
                                    disease_->getName()) < 0.01)
        continue;
      if (!matchesAttributes(person, seed.attribute_filters)) continue;
      auto residence = world_.getActivityVenues(*person, "residence");
      if (residence.empty()) continue;

      LocalHousehold& household = households[residence[0].first];
      // One rank holds the whole household: a residence venue and its
      // occupants share a geo unit, checked in Domain (ADR 0012).
      household.key = mix_seed(event_base, unit_hash,
                               static_cast<uint64_t>(residence[0].first));
      LocalMember member{person, {}};
      for (size_t budget_index = 0; budget_index < unit_case.budgets.size();
           ++budget_index) {
        if (unit_case.budgets[budget_index].accepts(
                *person, &world_, seed.structured_config.target_groups)) {
          member.matched_budgets.push_back(static_cast<uint32_t>(budget_index));
        }
      }
      if (!member.matched_budgets.empty()) ++household.matched;
      household.members.push_back(std::move(member));
    }

    std::vector<const LocalHousehold*> densest_first;
    for (const auto& [venue_id, household] : households) {
      (void)venue_id;
      if (household.matched > 0) densest_first.push_back(&household);
    }
    std::sort(
        densest_first.begin(), densest_first.end(),
        [](const LocalHousehold* a, const LocalHousehold* b) {
          const uint64_t left = a->matched * a->matched * b->members.size();
          const uint64_t right = b->matched * b->matched * a->members.size();
          if (left != right) return left > right;
          return a->key < b->key;
        });

    // Offer only as far down the local order as the fill can possibly reach:
    // once a budget has seen total_target members eligible for it, a household
    // below cannot take a case against that budget without the fill having
    // already completed above it. Exact because the rank sorting a household
    // sees all of it, which Domain checks and ADR 0012 records.
    std::vector<int> eligible_seen(unit.targets.size(), 0);
    auto fillCannotReachFurther = [&]() {
      for (size_t budget_index = 0; budget_index < unit.targets.size();
           ++budget_index) {
        if (unit.targets[budget_index] > 0 &&
            eligible_seen[budget_index] < total_target) {
          return false;
        }
      }
      return true;
    };

    for (const LocalHousehold* household : densest_first) {
      if (fillCannotReachFurther()) break;
      for (const LocalMember& member : household->members) {
        local_candidates[member.person->id] = member.person;
        if (member.matched_budgets.empty()) {
          local_offers.push_back(
              {household->key, member.person->id, unit.first_slot});
          continue;
        }
        for (uint32_t budget_index : member.matched_budgets) {
          local_offers.push_back({household->key, member.person->id,
                                  unit.first_slot + budget_index + 1});
          ++eligible_seen[budget_index];
        }
      }
    }
  }

  std::vector<SeedOffer> pooled_offers =
      seed_offer_exchange_ ? seed_offer_exchange_->pool(local_offers)
                           : local_offers;

  std::vector<std::vector<SeedOffer>> offers_by_unit(units.size());
  for (const auto& offer : pooled_offers) {
    if (offer.budget_slot >= unit_of_slot.size()) continue;
    const uint32_t unit_index = unit_of_slot[offer.budget_slot];
    offers_by_unit[unit_index].push_back(
        {offer.key, offer.person_id,
         offer.budget_slot - units[unit_index].first_slot});
  }

  std::vector<PersonId> infected_ids;
  for (size_t unit_index = 0; unit_index < units.size(); ++unit_index) {
    const ClusterUnit& unit = units[unit_index];
    ClusterPlan plan =
        planClusteredSeed({offers_by_unit[unit_index]}, unit.targets);
    // The offers are every rank's, so a budget the households could not fill
    // is short everywhere, not merely here. As on the exact path the gap has
    // two causes, but here a budget loses a member only to one declared before
    // it, so the record says so rather than borrowing the neutral wording.
    // Never fatal.
    for (size_t budget_index = 0; budget_index < unit.targets.size();
         ++budget_index) {
      if (plan.filled_per_budget[budget_index] < unit.targets[budget_index]) {
        seed_shortfalls_.push_back(
            {seed.name, seed.structured_config.geo_level, unit.unit_id,
             budget_index, unit.budget_labels[budget_index],
             unit.targets[budget_index], plan.filled_per_budget[budget_index],
             plan.lost_per_budget[budget_index],
             /*lost_to_earlier_declared=*/true});
      }
    }
    for (const auto& assignment : plan.assignments) {
      auto held = local_candidates.find(assignment.person_id);
      if (held == local_candidates.end()) continue;  // another rank holds them
      // Already infected means the count, not the infection, is at stake:
      // infectPerson is a no-op there, so recording the id would report a case
      // this step did not place.
      if (held->second->infection != nullptr) continue;
      infectPerson(held->second, seed.trajectory_key, seed.start_symptom);
      if (held->second->infection != nullptr) {
        infected_ids.push_back(assignment.person_id);
      }
    }
  }
  return infected_ids;
}

bool InfectionSeeder::matchesAttributes(
    const Person* person, const std::vector<SelectionCriterion>& filters) {
  if (filters.empty()) return true;
  for (const auto& filter : filters) {
    if (!filter.evaluate(*person, &world_)) return false;
  }
  return true;
}

void InfectionSeeder::infectPerson(Person* person,
                                   const std::string& trajectory_key,
                                   const std::string& start_symptom) {
  if (person->infection != nullptr) return;
  if (person->getSusceptibility(current_simulation_time_, disease_->getName()) <
      0.01)
    return;

  uint64_t infection_seed =
      mix_seed(base_seed_, person->id,
               static_cast<uint64_t>(current_simulation_time_ * 1000), 0x5EED);

  float severity_factor = 1.0f;
  auto* gu = world_.getGeoUnit(person->geo_unit_id);
  if (gu) severity_factor = gu->severity_factor;

  person->infection = std::make_unique<Infection>(
      disease_, current_simulation_time_, person,
      static_cast<unsigned int>(infection_seed), &world_,
      "seed",  // venue type
      INFECTION_SEED_VENUE_ID, severity_factor,
      0,  // infector_symptom_id -- no infector for seeds
      trajectory_key, start_symptom);

  if (event_logger_ != nullptr) {
    event_logger_->logInfection(
        person->id, kInvalidPersonId, INFECTION_SEED_VENUE_ID,
        current_simulation_time_, kDefaultEncounterTypeId,
        kNoSymptomId);  // no infector for seeds
  }
}

}  // namespace june
