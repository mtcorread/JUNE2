#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "core/config.h"
#include "core/world_state.h"
#include "doctest.h"
#include "loaders/config_loader_detail.h"
#include "loaders/policy_loader.h"
#include "utils/filtering.h"

using namespace june;

namespace {

// Two-level geography mirroring the GB world's shape in miniature: named
// nations at the coarse level, output-area-like units at the fine level, and
// every person sitting at the fine level.
static WorldState buildNationWorld() {
  WorldState world;
  world.geo_level_names = {"XLGU", "SGU"};

  auto addUnit = [&world](GeoUnitId id, const std::string& name,
                          uint8_t level_id, GeoUnitId parent_id) {
    GeographicalUnit unit;
    unit.id = id;
    unit.name = name;
    unit.level_id = level_id;
    unit.parent_id = parent_id;
    world.geo_units.push_back(unit);
  };

  addUnit(0, "Scotland", 0, -1);
  addUnit(1, "London", 0, -1);
  addUnit(2, "Wales", 0, -1);
  addUnit(10, "S00000001", 1, 0);
  addUnit(11, "E00000001", 1, 1);
  addUnit(12, "W00000001", 1, 2);

  auto addPerson = [&world](PersonId id, GeoUnitId geo_unit_id) {
    Person& person = world.people.emplace_back();
    person.id = id;
    person.geo_unit_id = geo_unit_id;
  };

  addPerson(0, 10);  // Scotland
  addPerson(1, 11);  // London
  addPerson(2, 12);  // Wales

  world.buildIndices();
  return world;
}

// Resolve `criterion` against `world`, returning whatever it wrote to stderr.
static std::string resolveCapturingStderr(SelectionCriterion& criterion,
                                          const WorldState& world) {
  std::ostringstream captured;
  std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
  criterion.resolveOrThrow(world, "test");
  std::cerr.rdbuf(previous);
  return captured.str();
}

}  // namespace

TEST_CASE("geo_unit.<LEVEL> == name selects people under that ancestor") {
  WorldState world = buildNationWorld();

  SelectionCriterion criterion;
  criterion.property_path = "geo_unit.XLGU";
  criterion.operator_type = "==";
  criterion.value = std::string("Scotland");
  criterion.resolveOrThrow(world, "test");

  CHECK(criterion.evaluate(world.people[0], &world));
  CHECK_FALSE(criterion.evaluate(world.people[1], &world));

  // Dense ids, and no world passed: same answers.
  CHECK(criterion.evaluate(world.people[0]));
  CHECK_FALSE(criterion.evaluate(world.people[1]));
}

TEST_CASE("geo_unit.<LEVEL> in a list of names selects people under any of them") {
  WorldState world = buildNationWorld();

  SelectionCriterion criterion;
  criterion.property_path = "geo_unit.XLGU";
  criterion.operator_type = "in";
  criterion.value = std::vector<std::string>{"London", "Wales"};
  criterion.resolveOrThrow(world, "test");

  CHECK_FALSE(criterion.evaluate(world.people[0], &world));
  CHECK(criterion.evaluate(world.people[1], &world));
  CHECK(criterion.evaluate(world.people[2], &world));
}

TEST_CASE("geo_unit.<LEVEL> != excludes, and over a list means not-in") {
  WorldState world = buildNationWorld();

  SUBCASE("single name") {
    SelectionCriterion criterion;
    criterion.property_path = "geo_unit.XLGU";
    criterion.operator_type = "!=";
    criterion.value = std::string("Scotland");
    criterion.resolveOrThrow(world, "test");

    CHECK_FALSE(criterion.evaluate(world.people[0], &world));
    CHECK(criterion.evaluate(world.people[1], &world));
  }

  SUBCASE("list of names") {
    SelectionCriterion criterion;
    criterion.property_path = "geo_unit.XLGU";
    criterion.operator_type = "!=";
    criterion.value = std::vector<std::string>{"Scotland", "Wales"};
    criterion.resolveOrThrow(world, "test");

    CHECK_FALSE(criterion.evaluate(world.people[0], &world));
    CHECK(criterion.evaluate(world.people[1], &world));
    CHECK_FALSE(criterion.evaluate(world.people[2], &world));
  }
}

TEST_CASE("geo_unit.<LEVEL> at the person's own level matches the unit itself") {
  WorldState world = buildNationWorld();

  SelectionCriterion criterion;
  criterion.property_path = "geo_unit.SGU";
  criterion.operator_type = "==";
  criterion.value = std::string("S00000001");
  criterion.resolveOrThrow(world, "test");

  CHECK(criterion.evaluate(world.people[0], &world));
  CHECK_FALSE(criterion.evaluate(world.people[1], &world));
}

TEST_CASE("a person with no ancestor at the level matches neither == nor !=") {
  WorldState world = buildNationWorld();
  // A unit sitting at the fine level with no parent: absent at XLGU.
  GeographicalUnit stateless;
  stateless.id = 13;
  stateless.name = "Z00000001";
  stateless.level_id = 1;
  stateless.parent_id = -1;
  world.geo_units.push_back(stateless);
  Person& person = world.people.emplace_back();
  person.id = 3;
  person.geo_unit_id = 13;
  world.buildIndices();

  SelectionCriterion equals;
  equals.property_path = "geo_unit.XLGU";
  equals.operator_type = "==";
  equals.value = std::string("Scotland");
  equals.resolveOrThrow(world, "test");

  SelectionCriterion differs;
  differs.property_path = "geo_unit.XLGU";
  differs.operator_type = "!=";
  differs.value = std::string("Scotland");
  differs.resolveOrThrow(world, "test");

  CHECK_FALSE(equals.evaluate(world.people[3], &world));
  CHECK_FALSE(differs.evaluate(world.people[3], &world));
}

TEST_CASE("the no-ancestor warning fires only for units people live in") {
  SUBCASE("a correctly specified world stays silent") {
    WorldState world = buildNationWorld();
    // The nations have no ancestor at SGU, but nobody lives in one directly.
    SelectionCriterion criterion;
    criterion.property_path = "geo_unit.SGU";
    criterion.operator_type = "==";
    criterion.value = std::string("S00000001");

    CHECK(resolveCapturingStderr(criterion, world).empty());
  }

  SUBCASE("a unit people live in with no ancestor at the level warns") {
    WorldState world = buildNationWorld();
    GeographicalUnit stateless;
    stateless.id = 13;
    stateless.name = "Z00000001";
    stateless.level_id = 1;
    stateless.parent_id = -1;
    world.geo_units.push_back(stateless);
    Person& person = world.people.emplace_back();
    person.id = 3;
    person.geo_unit_id = 13;
    world.buildIndices();

    SelectionCriterion criterion;
    criterion.property_path = "geo_unit.XLGU";
    criterion.operator_type = "==";
    criterion.value = std::string("Scotland");

    const std::string warning = resolveCapturingStderr(criterion, world);
    CHECK(warning.find("1 inhabited geographical units") != std::string::npos);
  }
}

TEST_CASE("a geo_unit criterion this world cannot answer fails loudly at load") {
  WorldState world = buildNationWorld();

  auto criterionFor = [](const std::string& path, const std::string& op,
                         PropertyValue value) {
    SelectionCriterion criterion;
    criterion.property_path = path;
    criterion.operator_type = op;
    criterion.value = std::move(value);
    return criterion;
  };

  SUBCASE("unknown level name") {
    auto criterion =
        criterionFor("geo_unit.REGION", "==", std::string("Scotland"));
    CHECK_THROWS_AS(criterion.resolveOrThrow(world, "test"),
                    std::runtime_error);
  }

  SUBCASE("unknown unit name") {
    auto criterion =
        criterionFor("geo_unit.XLGU", "==", std::string("Scotlnad"));
    CHECK_THROWS_AS(criterion.resolveOrThrow(world, "test"),
                    std::runtime_error);
  }

  SUBCASE("name that exists only at another level") {
    auto criterion =
        criterionFor("geo_unit.XLGU", "==", std::string("S00000001"));
    CHECK_THROWS_AS(criterion.resolveOrThrow(world, "test"),
                    std::runtime_error);
  }

  SUBCASE("one bad name in an otherwise good list") {
    auto criterion = criterionFor(
        "geo_unit.XLGU", "in", std::vector<std::string>{"Scotland", "Mercia"});
    CHECK_THROWS_AS(criterion.resolveOrThrow(world, "test"),
                    std::runtime_error);
  }

  SUBCASE("ambiguous name at the level") {
    GeographicalUnit duplicate;
    duplicate.id = 3;
    duplicate.name = "Scotland";
    duplicate.level_id = 0;
    duplicate.parent_id = -1;
    world.geo_units.push_back(duplicate);
    world.buildIndices();

    auto criterion =
        criterionFor("geo_unit.XLGU", "==", std::string("Scotland"));
    CHECK_THROWS_AS(criterion.resolveOrThrow(world, "test"),
                    std::runtime_error);
  }

  SUBCASE("a raw unit id instead of a name") {
    auto criterion = criterionFor("geo_unit.XLGU", "==", int32_t(0));
    CHECK_THROWS_AS(criterion.resolveOrThrow(world, "test"),
                    std::runtime_error);
  }

  SUBCASE("an operator geography cannot answer") {
    auto criterion =
        criterionFor("geo_unit.XLGU", ">", std::string("Scotland"));
    CHECK_THROWS_AS(criterion.resolveOrThrow(world, "test"),
                    std::runtime_error);
  }
}

TEST_CASE("a sparse geo id space gives the same answers") {
  WorldState world;
  world.geo_level_names = {"XLGU", "SGU"};

  auto addUnit = [&world](GeoUnitId id, const std::string& name,
                          uint8_t level_id, GeoUnitId parent_id) {
    GeographicalUnit unit;
    unit.id = id;
    unit.name = name;
    unit.level_id = level_id;
    unit.parent_id = parent_id;
    world.geo_units.push_back(unit);
  };

  addUnit(1000000, "Scotland", 0, -1);
  addUnit(2000000, "London", 0, -1);
  addUnit(3000000, "S00000001", 1, 1000000);
  addUnit(4000000, "E00000001", 1, 2000000);

  Person& scot = world.people.emplace_back();
  scot.id = 0;
  scot.geo_unit_id = 3000000;
  Person& londoner = world.people.emplace_back();
  londoner.id = 1;
  londoner.geo_unit_id = 4000000;
  world.buildIndices();

  SelectionCriterion criterion;
  criterion.property_path = "geo_unit.XLGU";
  criterion.operator_type = "==";
  criterion.value = std::string("Scotland");
  criterion.resolveOrThrow(world, "test");

  CHECK(criterion.evaluate(world.people[0], &world));
  CHECK_FALSE(criterion.evaluate(world.people[1], &world));

  // Sparse ids force the fallback keying; the answer must not depend on it, nor
  // on the caller having a WorldState in hand (ActivityManager has none).
  CHECK(criterion.evaluate(world.people[0]));
  CHECK_FALSE(criterion.evaluate(world.people[1]));

  // A unit this world does not carry is no ancestor of anything.
  Person stranger;
  stranger.id = 2;
  stranger.geo_unit_id = 3500000;
  CHECK_FALSE(criterion.evaluate(stranger));
}

TEST_CASE("a policies.yaml applies_to accepts a list of unit names") {
  WorldState world = buildNationWorld();
  world.activity_names = {"residence", "primary_activity"};
  world.buildIndices();

  const std::string path = "/tmp/june_test_geo_policies.yaml";
  {
    std::ofstream out(path);
    out << "policies:\n"
           "  temporal_policies:\n"
           "    - name: \"english_and_welsh_lockdown\"\n"
           "      start_date: \"2020-03-23\"\n"
           "      end_date: \"2020-05-15\"\n"
           "      override_activities: [\"primary_activity\"]\n"
           "      replacement: \"residence\"\n"
           "      compliance_rate: 1.0\n"
           "      applies_to:\n"
           "        - property: \"geo_unit.XLGU\"\n"
           "          operator: \"in\"\n"
           "          value: [\"London\", \"Wales\"]\n";
  }

  PolicyManager policy_manager(world);
  PolicyLoader::loadPolicies(policy_manager, path, "2020-01-01");
  REQUIRE(policy_manager.getTemporalPolicyCount() == 1);

  const TemporalPolicy& policy = policy_manager.getTemporalPolicies()[0];
  CHECK_FALSE(policy.appliesTo(world.people[0], &world));
  CHECK(policy.appliesTo(world.people[1], &world));
  CHECK(policy.appliesTo(world.people[2], &world));

  std::remove(path.c_str());
}

TEST_CASE("a schedules.yaml selection accepts a list of unit names") {
  YAML::Node node = YAML::Load(
      "- property: \"geo_unit.XLGU\"\n"
      "  operator: \"in\"\n"
      "  value: [\"London\", \"Wales\"]\n");

  std::vector<SelectionCriterion> criteria;
  config_detail::parseSelectionCriteria(node, criteria);

  REQUIRE(criteria.size() == 1);
  REQUIRE(std::holds_alternative<std::vector<std::string>>(criteria[0].value));
  CHECK(std::get<std::vector<std::string>>(criteria[0].value).size() == 2);
}

TEST_CASE("the schedule-assignment CSV filter syntax reaches ancestor geography") {
  WorldState world = buildNationWorld();

  auto criteria =
      filtering::parseConjunctiveExpression("filter.geo_unit.XLGU==Scotland");
  REQUIRE(criteria.size() == 1);
  criteria[0].resolveOrThrow(world, "test");

  CHECK(criteria[0].evaluate(world.people[0], &world));
  CHECK_FALSE(criteria[0].evaluate(world.people[1], &world));
}

TEST_CASE("a temporal policy refuses to resolve a filter this world cannot answer") {
  WorldState world = buildNationWorld();
  world.activity_names = {"residence", "primary_activity"};
  world.buildIndices();

  TemporalPolicy policy;
  policy.name = "misspelt_filter";
  policy.action.replacement_activity = "residence";
  SelectionCriterion misspelt;
  misspelt.property_path = "geo_unit.XLGU";
  misspelt.operator_type = "==";
  misspelt.value = std::string("Scotlnad");
  policy.applies_to.push_back(misspelt);

  CHECK_THROWS_AS(policy.resolve(world), std::runtime_error);
}

TEST_CASE("a list value on a non-geographical property must be whole numbers") {
  // Regression: a `catch (...)` fallback used to reinterpret any list that
  // failed int conversion as a list of unit names. Only geo_unit.<LEVEL>
  // reads those, so every other property got a criterion that loaded clean
  // and then matched nobody — a config mistake reading as "nobody qualifies".

  SUBCASE("a list of strings is rejected") {
    YAML::Node node = YAML::Load(
        "- property: \"properties.region\"\n"
        "  operator: \"in\"\n"
        "  value: [\"North East\", \"Yorkshire\"]\n");

    std::vector<SelectionCriterion> criteria;
    CHECK_THROWS_AS(config_detail::parseSelectionCriteria(node, criteria),
                    std::runtime_error);
  }

  SUBCASE("a list of floats is rejected") {
    YAML::Node node = YAML::Load(
        "- property: \"age\"\n"
        "  operator: \"in\"\n"
        "  value: [1.5, 2.5]\n");

    std::vector<SelectionCriterion> criteria;
    CHECK_THROWS_AS(config_detail::parseSelectionCriteria(node, criteria),
                    std::runtime_error);
  }

  SUBCASE("a list of ints still parses") {
    YAML::Node node = YAML::Load(
        "- property: \"geo_unit_id\"\n"
        "  operator: \"in\"\n"
        "  value: [10, 11]\n");

    std::vector<SelectionCriterion> criteria;
    config_detail::parseSelectionCriteria(node, criteria);

    REQUIRE(criteria.size() == 1);
    CHECK(std::holds_alternative<std::vector<int32_t>>(criteria[0].value));
  }
}

TEST_CASE("the policy loader rejects the same list values as the schedule loader") {
  WorldState world = buildNationWorld();
  world.activity_names = {"residence", "primary_activity"};
  world.buildIndices();

  const std::string path = "/tmp/june_test_bad_list_policies.yaml";
  {
    std::ofstream out(path);
    out << "policies:\n"
           "  temporal_policies:\n"
           "    - name: \"regional_lockdown\"\n"
           "      start_date: \"2020-03-23\"\n"
           "      override_activities: [\"primary_activity\"]\n"
           "      replacement: \"residence\"\n"
           "      applies_to:\n"
           "        - property: \"properties.region\"\n"
           "          operator: \"in\"\n"
           "          value: [\"North East\", \"Yorkshire\"]\n";
  }

  PolicyManager policy_manager(world);
  CHECK_THROWS_AS(
      PolicyLoader::loadPolicies(policy_manager, path, "2020-01-01"),
      std::runtime_error);

  std::remove(path.c_str());
}
