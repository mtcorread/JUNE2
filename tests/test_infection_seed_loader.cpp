#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "doctest.h"
#include "epidemiology/disease.h"
#include "epidemiology/infection_seed.h"

using namespace june;

namespace {

// Writes YAML to a throwaway file and loads it, so the tests exercise the
// loader's real public entry point rather than a parsing helper.
InfectionSeedConfig loadYaml(const std::string& yaml) {
  static int counter = 0;
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("june_seed_test_" + std::to_string(counter++) +
                                ".yaml");
  {
    std::ofstream out(path);
    out << yaml;
  }
  try {
    InfectionSeedConfig config = InfectionSeedConfigLoader::loadFromFile(path);
    std::filesystem::remove(path);
    return config;
  } catch (...) {
    std::filesystem::remove(path);
    throw;
  }
}

// A one-stage disease, enough for the seeder to construct an Infection.
Disease makeDisease() {
  TransmissionParams transmission;
  transmission.mode = InfectiousnessMode::STAGE_DRIVEN;
  auto constant_curve = std::make_shared<ConstantCurve>(1.0);
  transmission.stage_curves["mild"] = constant_curve;
  transmission.symptom_id_curves = {nullptr, constant_curve};

  std::vector<SymptomTag> symptom_tags = {{"healthy", -1, 0}, {"mild", 1, 1}};
  TrajectoryDefinition trajectory;
  trajectory.selection_key = "general";
  trajectory.severity = 1.0;
  trajectory.stages.push_back({"mild", {"constant", {{"value", 10.0}}}});

  return Disease("TestDisease", symptom_tags, {}, {trajectory}, {},
                 transmission);
}

// One geographical unit holding `population` people, ages spread 0-99.
WorldState makeWorld(const std::string& unit_name, int population) {
  WorldState world;
  world.geo_level_names = {"MGU"};

  GeographicalUnit unit;
  unit.id = 0;
  unit.name = unit_name;
  unit.level_id = 0;
  unit.parent_id = -1;
  world.geo_units.push_back(unit);

  for (int i = 0; i < population; ++i) {
    Person person;
    person.id = i;
    person.age = static_cast<float>(i % 100);
    person.sex = Sex::MALE;
    person.geo_unit_id = 0;
    world.people.push_back(std::move(person));
  }
  world.buildIndices();
  return world;
}

}  // namespace

// =============================================================================
// Cycle 1: a scalar count is a single budget, not one budget per target group
// =============================================================================

TEST_CASE("scalar count with declared groups is one budget of that count") {
  auto config = loadYaml(R"(
infection_seeds:
  - name: "scalar"
    type: "exact"
    date: "2020-02-01 08:00"
    geo_level: "MGU"
    parameters:
      age_groups: ["0-17", "18-64", "65-100"]
      units:
        "E02001234": 20
)");

  REQUIRE(config.seeds.size() == 1);
  const auto& structured = config.seeds[0].structured_config;
  REQUIRE(structured.target_groups.size() == 3);
  REQUIRE(structured.unit_cases.size() == 1);

  const auto& budgets = structured.unit_cases[0].budgets;
  REQUIRE(budgets.size() == 1);
  CHECK(budgets[0].cases == 20);
}

// =============================================================================
// Cycle 2: a scalar draws from the union of the declared groups
// =============================================================================

TEST_CASE("scalar budget accepts anyone matching any declared group") {
  auto config = loadYaml(R"(
infection_seeds:
  - name: "scalar"
    type: "exact"
    date: "2020-02-01 08:00"
    geo_level: "MGU"
    parameters:
      age_groups: ["0-17", "18-64", "65-100"]
      units:
        "E02001234": 20
)");

  WorldState world;
  config.resolve(world);
  const auto& structured = config.seeds[0].structured_config;
  const auto& budget = structured.unit_cases[0].budgets[0];

  Person infant;
  infant.age = 5;
  Person pensioner;
  pensioner.age = 80;
  Person centenarian;
  centenarian.age = 105;

  CHECK(budget.accepts(infant, &world, structured.target_groups));
  CHECK(budget.accepts(pensioner, &world, structured.target_groups));
  CHECK_FALSE(budget.accepts(centenarian, &world, structured.target_groups));
}

// =============================================================================
// Cycle 3: a per-group list keeps one budget per group
// =============================================================================

TEST_CASE("per-group list gives each group its own budget") {
  auto config = loadYaml(R"(
infection_seeds:
  - name: "per_group"
    type: "exact"
    date: "2020-02-01 08:00"
    geo_level: "MGU"
    parameters:
      age_groups: ["0-17", "18-64", "65-100"]
      units:
        "E02004292": [5, 10, 2]
)");

  WorldState world;
  config.resolve(world);
  const auto& structured = config.seeds[0].structured_config;
  const auto& budgets = structured.unit_cases[0].budgets;
  REQUIRE(budgets.size() == 3);
  CHECK(budgets[0].cases == 5);
  CHECK(budgets[1].cases == 10);
  CHECK(budgets[2].cases == 2);

  // Each budget draws only from its own group.
  Person adult;
  adult.age = 40;
  CHECK_FALSE(budgets[0].accepts(adult, &world, structured.target_groups));
  CHECK(budgets[1].accepts(adult, &world, structured.target_groups));
  CHECK_FALSE(budgets[2].accepts(adult, &world, structured.target_groups));
}

// =============================================================================
// Cycle 4: no declared groups means one unrestricted budget, and it seeds
// =============================================================================

TEST_CASE("scalar with no declared groups seeds the stated count") {
  auto config = loadYaml(R"(
infection_seeds:
  - name: "unrestricted"
    type: "exact"
    date: "2020-02-01 08:00"
    geo_level: "MGU"
    parameters:
      units:
        "E02001234": 20
)");

  WorldState world = makeWorld("E02001234", 100);
  config.resolve(world);
  REQUIRE(config.seeds[0].structured_config.target_groups.empty());

  Disease disease = makeDisease();
  InfectionSeeder seeder(world, &disease, config);
  auto infected = seeder.seedInfections("2020-02-01 08:00", 0.0);

  CHECK(infected.size() == 20);
}

TEST_CASE("scalar with three declared groups seeds twenty, not sixty") {
  auto config = loadYaml(R"(
infection_seeds:
  - name: "scalar"
    type: "exact"
    date: "2020-02-01 08:00"
    geo_level: "MGU"
    parameters:
      age_groups: ["0-17", "18-64", "65-100"]
      units:
        "E02001234": 20
)");

  WorldState world = makeWorld("E02001234", 300);
  config.resolve(world);

  Disease disease = makeDisease();
  InfectionSeeder seeder(world, &disease, config);
  auto infected = seeder.seedInfections("2020-02-01 08:00", 0.0);

  CHECK(infected.size() == 20);

  // Drawn from the union of the groups, not one slice of it.
  bool seeded_a_child = false;
  bool seeded_an_adult = false;
  for (PersonId id : infected) {
    float age = world.people[world.person_index.at(id)].age;
    if (age <= 17) seeded_a_child = true;
    if (age >= 18 && age <= 64) seeded_an_adult = true;
  }
  CHECK(seeded_a_child);
  CHECK(seeded_an_adult);
}

// =============================================================================
// Cycle 5: a list whose length misses the declared groups is fatal at load
// =============================================================================

TEST_CASE("per-group list of the wrong length throws, naming seed and unit") {
  const std::string yaml = R"(
infection_seeds:
  - name: "mismatched"
    type: "exact"
    date: "2020-02-01 08:00"
    geo_level: "MGU"
    parameters:
      age_groups: ["0-17", "18-64", "65-100"]
      units:
        "E02004292": [5, 10]
)";

  try {
    loadYaml(yaml);
    FAIL("expected the loader to throw");
  } catch (const std::runtime_error& error) {
    std::string message = error.what();
    CHECK(message.find("mismatched") != std::string::npos);
    CHECK(message.find("E02004292") != std::string::npos);
    CHECK(message.find("2") != std::string::npos);
    CHECK(message.find("3") != std::string::npos);
  }
}

// =============================================================================
// Cycle 6: a list with no groups declared is fatal at load
// =============================================================================

TEST_CASE("per-group list with no declared groups throws") {
  const std::string yaml = R"(
infection_seeds:
  - name: "groupless_list"
    type: "exact"
    date: "2020-02-01 08:00"
    geo_level: "MGU"
    parameters:
      units:
        "E02004292": [5, 10]
)";

  CHECK_THROWS_AS(loadYaml(yaml), std::runtime_error);
}

// =============================================================================
// Cycle 7: the bulk-CSV path still gives each criteria set its own budget
// =============================================================================

TEST_CASE("bulk CSV keeps one budget per distinct criteria set") {
  std::filesystem::path csv_path =
      std::filesystem::temp_directory_path() / "june_bulk_seed_test.csv";
  {
    std::ofstream out(csv_path);
    out << "name,date,type,geo_level,geo_unit,cases,filter.sex\n"
           "bubonic,1348-06-02 08:00,exact,County,DURHAM,100,male\n"
           "bubonic,1348-06-02 08:00,exact,County,DURHAM,7,female\n"
           "bubonic,1348-06-02 08:00,exact,County,YORK,3,female\n";
  }

  InfectionSeedConfig config;
  InfectionSeedConfigLoader::loadBulkCsvSeeds(csv_path.string(), config);
  std::filesystem::remove(csv_path);

  REQUIRE(config.seeds.size() == 1);
  const auto& structured = config.seeds[0].structured_config;
  REQUIRE(structured.target_groups.size() == 2);  // one per distinct filter
  REQUIRE(structured.unit_cases.size() == 2);

  auto budgetsFor = [&](const std::string& unit) {
    for (const auto& unit_case : structured.unit_cases) {
      if (unit_case.unit_id == unit) return unit_case.budgets;
    }
    return std::vector<SeedBudget>{};
  };

  auto durham = budgetsFor("DURHAM");
  REQUIRE(durham.size() == 2);
  CHECK(durham[0].cases == 100);
  CHECK(durham[0].eligible_target_groups == std::vector<size_t>{0});
  CHECK(durham[1].cases == 7);
  CHECK(durham[1].eligible_target_groups == std::vector<size_t>{1});

  auto york = budgetsFor("YORK");
  REQUIRE(york.size() == 2);
  CHECK(york[0].cases == 0);
  CHECK(york[1].cases == 3);
}

TEST_CASE("bulk CSV seeds each criteria set its own count") {
  std::filesystem::path csv_path =
      std::filesystem::temp_directory_path() / "june_bulk_seed_run_test.csv";
  {
    std::ofstream out(csv_path);
    out << "name,date,type,geo_level,geo_unit,cases,filter.age\n"
           "bubonic,1348-06-02 08:00,exact,MGU,DURHAM,10,0-17\n"
           "bubonic,1348-06-02 08:00,exact,MGU,DURHAM,4,65-100\n";
  }

  InfectionSeedConfig config;
  InfectionSeedConfigLoader::loadBulkCsvSeeds(csv_path.string(), config);
  std::filesystem::remove(csv_path);

  WorldState world = makeWorld("DURHAM", 300);
  config.resolve(world);

  Disease disease = makeDisease();
  InfectionSeeder seeder(world, &disease, config);
  auto infected = seeder.seedInfections("1348-06-02 08:00", 0.0);

  REQUIRE(infected.size() == 14);
  int children = 0;
  int elders = 0;
  for (PersonId id : infected) {
    float age = world.people[world.person_index.at(id)].age;
    if (age <= 17) ++children;
    if (age >= 65) ++elders;
  }
  CHECK(children == 10);
  CHECK(elders == 4);
}
