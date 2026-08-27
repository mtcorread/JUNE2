#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <string>
#include <vector>

#include "../include/epidemiology/seeding/seed_shortfall.h"
#include "doctest.h"

using namespace june;

TEST_CASE("Shortfall report: a short unit names seed, unit, level and counts") {
  std::vector<SeedShortfall> shortfalls = {
      {"february_2020", "LGU", "E06000005", 0, "", 20, 12, 0, false}};

  std::string report = formatSeedShortfallReport(shortfalls);

  CHECK(report.find("february_2020") != std::string::npos);
  CHECK(report.find("E06000005") != std::string::npos);
  CHECK(report.find("LGU") != std::string::npos);
  CHECK(report.find("20") != std::string::npos);
  CHECK(report.find("12") != std::string::npos);
}

namespace {

int countOccurrences(const std::string& text, const std::string& needle) {
  int count = 0;
  for (size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

std::vector<SeedShortfall> manyShortfalls(int units) {
  std::vector<SeedShortfall> shortfalls;
  for (int unit = 0; unit < units; ++unit) {
    shortfalls.push_back(
        {"february_2020", "LGU", "E060000" + std::to_string(unit), 0, "",
         20, 12, 0, false});
  }
  return shortfalls;
}

}  // namespace

TEST_CASE("Shortfall report: beyond the cap it counts what it suppressed") {
  std::string report = formatSeedShortfallReport(manyShortfalls(17));

  CHECK(countOccurrences(report, "requested") == 10);
  CHECK(report.find("7 further short budgets suppressed") != std::string::npos);
}

TEST_CASE("Shortfall report: a unit eligible nowhere reports like any other") {
  // A mistyped unit code resolves to nobody at all. It is the shortfall most
  // worth seeing, so it gets a line of its own rather than being swallowed.
  std::vector<SeedShortfall> shortfalls = {
      {"february_2020", "LGU", "E06000005", 0, "", 20, 12, 0, false},
      {"february_2020", "LGU", "E06TYPO", 0, "", 15, 0, 0, false}};

  std::string report = formatSeedShortfallReport(shortfalls);

  CHECK(countOccurrences(report, "requested") == 2);
  CHECK(report.find("'E06TYPO' (LGU) budget 0: requested 15, placed 0, 0 "
                    "lost to another budget, 15 with nobody eligible") !=
        std::string::npos);
}

TEST_CASE("Shortfall report: nothing short emits no block") {
  CHECK(formatSeedShortfallReport({}).empty());
}

TEST_CASE("Shortfall report: two short budgets of one unit are told apart") {
  // Overlapping target groups can leave one unit short against two budgets at
  // once; the lines must say which age band, not repeat the unit twice.
  std::vector<SeedShortfall> shortfalls = {
      {"february_2020", "LGU", "E06000005", 0, "", 5, 3, 0, false},
      {"february_2020", "LGU", "E06000005", 2, "", 10, 8, 0, false}};

  std::string report = formatSeedShortfallReport(shortfalls);

  CHECK(report.find("budget 0") != std::string::npos);
  CHECK(report.find("budget 2") != std::string::npos);
  CHECK(countOccurrences(report, "requested") == 2);
}

TEST_CASE("Shortfall report: a contested budget does not blame the population") {
  // The nested-band miss of ADR 0011: "0-17" asked for one case and the only
  // child went to "0-64". Reporting the gap alone used to read as nobody
  // eligible, which was the one thing it was not.
  std::vector<SeedShortfall> shortfalls = {
      {"february_2020", "LGU", "E06000005", 0, "0-17", 1, 0, 1, false}};

  std::string report = formatSeedShortfallReport(shortfalls);

  CHECK(report.find("budget 0 ('0-17'): requested 1, placed 0, 1 lost to "
                    "another budget, 0 with nobody eligible") !=
        std::string::npos);
}

TEST_CASE("Shortfall report: a clustered budget names the earlier declaration") {
  // The clustered path resolves a contest by declaration order, not by key,
  // so its losses are told apart from the exact path's rather than sharing
  // wording that would imply the neutral rule.
  std::vector<SeedShortfall> shortfalls = {
      {"february_2020", "LGU", "E06000005", 1, "18-64", 4, 1, 2, true}};

  std::string report = formatSeedShortfallReport(shortfalls);

  CHECK(report.find("2 lost to an earlier-declared budget, 1 with nobody "
                    "eligible") != std::string::npos);
  CHECK(report.find("lost to another budget") == std::string::npos);
}

TEST_CASE("Shortfall report: more contested people than the gap does not underflow") {
  // A budget asking one case can be offered ten contested people. Only the gap
  // could ever have been filled from them, so the clauses still sum to it.
  std::vector<SeedShortfall> shortfalls = {
      {"february_2020", "LGU", "E06000005", 0, "0-17", 1, 0, 10, false}};

  std::string report = formatSeedShortfallReport(shortfalls);

  CHECK(report.find("1 lost to another budget, 0 with nobody eligible") !=
        std::string::npos);
  // Subtracting blind would print a negative count here.
  CHECK(report.find(", -") == std::string::npos);
}

TEST_CASE("Shortfall report: an unlabelled budget falls back to its index") {
  // Bulk CSV seeds build a criteria profile per row and keep no group name.
  std::vector<SeedShortfall> shortfalls = {
      {"bulk", "LGU", "E06000005", 3, "", 2, 0, 0, false}};

  std::string report = formatSeedShortfallReport(shortfalls);

  CHECK(report.find("budget 3: requested 2") != std::string::npos);
  CHECK(report.find("()") == std::string::npos);
}
