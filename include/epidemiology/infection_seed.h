#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"  // For AgeGroup definition
#include "core/types.h"
#include "core/world_state.h"
#include "epidemiology/seeding/seed_shortfall.h"
#include "utils/event_logging/event_logger.h"

namespace june {

// =============================================================================
// Infection Seed Types and Configuration
// =============================================================================

enum class InfectionSeedType {
  UNIFORM,   // Uniform distribution across population
  EXACT,     // Exact number of cases in specific locations/ages
  CLUSTERED  // Clustered by households in specific locations/ages
};

// Configuration for global infection seeding parameters
struct InfectionSeedGlobalConfig {
  double default_seed_strength = 1.0;
  double base_cases_per_capita = 0.0000001;
};

// Target group defining which people to infect based on any property
struct SeedTargetGroup {
  std::vector<SelectionCriterion> criteria;
  // How the config named this group, kept only so a report can say which group
  // it means — "0-17" rather than "budget 0". Never read by matching. Empty
  // where the config gives no name to keep, as bulk CSV seeds do, and a report
  // then falls back to the budget index.
  std::string label;

  bool matches(const Person& person, const WorldState* world) const {
    if (criteria.empty()) return true;
    for (const auto& c : criteria) {
      if (!c.evaluate(person, world)) return false;
    }
    return true;
  }

  void resolve(const WorldState& world) {
    for (auto& c : criteria) {
      c.resolveOrThrow(world, "infection seed target group");
    }
  }
};

// A number of cases to place, plus the target groups a person may match to be
// drawn against it. An empty eligible_target_groups means no demographic
// restriction. Budget shape is settled once, at config load:
//   per-group list -> one budget per declared group;
//   scalar         -> one budget, eligible across every declared group;
//   no groups      -> one unrestricted budget.
struct SeedBudget {
  int cases = 0;
  std::vector<size_t> eligible_target_groups;

  bool accepts(const Person& person, const WorldState* world,
               const std::vector<SeedTargetGroup>& target_groups) const {
    if (eligible_target_groups.empty()) return true;
    for (size_t group_index : eligible_target_groups) {
      if (target_groups[group_index].matches(person, world)) return true;
    }
    return false;
  }
};

// Cases for a specific geographic area
struct UnitCases {
  std::string unit_id;  // e.g., geographic unit code or name
  std::vector<SeedBudget> budgets;
};

// Configuration for exact/clustered seeding
struct StructuredSeedConfig {
  std::string geo_level;  // e.g., "SGU", "MGU", "LGU"
  std::vector<SeedTargetGroup> target_groups;
  std::vector<UnitCases> unit_cases;
};

// Configuration for uniform seeding
struct UniformSeedConfig {
  double cases_per_capita = 0.0;
};

// Single infection seed event
struct InfectionSeedEvent {
  std::string name;
  InfectionSeedType type;
  std::string date_time;  // ISO format: "2025-08-28 09:00"

  // Type-specific configuration
  StructuredSeedConfig structured_config;  // For EXACT/CLUSTERED
  UniformSeedConfig uniform_config;        // For UNIFORM

  // Parameters
  double seed_strength = 1.0;
  std::vector<SelectionCriterion>
      attribute_filters;  // Global filters for this event

  // Optional trajectory override for seeded infections.
  // trajectory_key names a selection_key directly; empty = sample from
  // probability distribution. start_symptom overrides the trajectory's
  // start_stage field.
  std::string trajectory_key =
      "";  // e.g. "pneumonic_perished"; empty = use rates
  std::string start_symptom =
      "";  // e.g. "pneumonia"; empty = trajectory's own start_stage
};

// Collection of all infection seeds
struct InfectionSeedConfig {
  InfectionSeedGlobalConfig global_params;
  std::vector<InfectionSeedEvent> seeds;
  std::string bulk_csv_path;

  void resolve(const WorldState& world) {
    for (auto& seed : seeds) {
      for (auto& filter : seed.attribute_filters) {
        filter.resolve(world);
      }
      for (auto& group : seed.structured_config.target_groups) {
        group.resolve(world);
      }
    }
  }
};

// =============================================================================
// Infection Seeding Implementation
// =============================================================================

// Forward declarations
class Disease;
class SeedOfferExchange;

class InfectionSeeder {
 public:
  InfectionSeeder(WorldState& world, const Disease* disease,
                  const InfectionSeedConfig& config,
                  EventLogger* event_logger = nullptr, uint64_t base_seed = 0);

  // Seed infections for a given simulation time
  // Returns IDs of people infected
  std::vector<PersonId> seedInfections(const std::string& current_datetime,
                                       double simulation_time);

  // Structured seeds are counted globally: the seeder offers its local
  // candidates and the exchange pools every rank's offers. Without one (a
  // serial run) the local offers are the whole world.
  void setOfferExchange(const SeedOfferExchange* exchange) {
    seed_offer_exchange_ = exchange;
  }

  // Budgets the last seeding step could not fill. Identical on every rank —
  // the offers they are derived from are pooled — so rank 0 can report them
  // without a further collective.
  const std::vector<SeedShortfall>& getSeedShortfalls() const {
    return seed_shortfalls_;
  }

  // Re-resolve all attribute_filter SelectionCriterion against the current
  // WorldState. Called after the world is fully loaded.
  void resolveConfig(const WorldState& world) { config_.resolve(world); }

  // --- Checkpoint serialization ---
  // applied_seeds_ tracks which seed events have already fired. It MUST be
  // saved and restored across a checkpoint, otherwise a resume re-fires
  // already-applied seeds and double-infects.
  const std::set<std::string>& getAppliedSeeds() const {
    return applied_seeds_;
  }
  void setAppliedSeeds(const std::set<std::string>& s) { applied_seeds_ = s; }

 private:
  WorldState& world_;
  const Disease* disease_;
  InfectionSeedConfig config_;
  EventLogger* event_logger_;
  double current_simulation_time_;
  uint64_t base_seed_ = 0;
  const SeedOfferExchange* seed_offer_exchange_ = nullptr;
  std::vector<SeedShortfall> seed_shortfalls_;

  // Track which seeds have been applied
  std::set<std::string> applied_seeds_;

  // Apply a single seed event
  std::vector<PersonId> applySeed(const InfectionSeedEvent& seed);

  // Type-specific seeding methods
  std::vector<PersonId> applyUniformSeed(const InfectionSeedEvent& seed);
  std::vector<PersonId> applyExactSeed(const InfectionSeedEvent& seed);
  std::vector<PersonId> applyClusteredSeed(const InfectionSeedEvent& seed);

  // Helper methods
  bool matchesAttributes(const Person* person,
                         const std::vector<SelectionCriterion>& filters);

  void infectPerson(Person* person, const std::string& trajectory_key = "",
                    const std::string& start_symptom = "");
};

// =============================================================================
// Configuration Loader
// =============================================================================

class InfectionSeedConfigLoader {
 public:
  static InfectionSeedConfig loadFromFile(const std::string& filename);

  static void loadBulkCsvSeeds(const std::string& csv_path,
                               InfectionSeedConfig& config);
  static std::vector<SelectionCriterion> parseCriterion(const std::string& key,
                                                        const std::string& val);

 private:
  static InfectionSeedType parseSeedType(const std::string& type_str);
};

}  // namespace june
