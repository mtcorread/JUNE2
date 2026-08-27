#include "epidemiology/seeding/seed_shortfall.h"

#include <algorithm>
#include <sstream>

namespace june {

namespace {
// A shortfall touching every unit of a national seed must not flood the log,
// and must not understate how wide it was either. One record is one short
// budget, so a single unit short against three of its budgets spends three of
// these — the cap is on lines, not on units.
constexpr size_t kMaxReportedBudgets = 10;
}  // namespace

std::string formatSeedShortfallReport(
    const std::vector<SeedShortfall>& shortfalls) {
  std::ostringstream report;
  const size_t reported = std::min(shortfalls.size(), kMaxReportedBudgets);
  for (size_t record = 0; record < reported; ++record) {
    const auto& shortfall = shortfalls[record];
    // Both clauses print even at zero: a fixed set of fields is greppable, and
    // a reader scanning many units can compare like with like.
    const int gap = shortfall.requested - shortfall.placed;
    const int contested = std::min(shortfall.lost, gap);
    report << "    [SEED SHORTFALL] seed '" << shortfall.seed_name << "' unit '"
           << shortfall.unit_id << "' (" << shortfall.geo_level << ") budget "
           << shortfall.budget_index;
    if (!shortfall.budget_label.empty()) {
      report << " ('" << shortfall.budget_label << "')";
    }
    report << ": requested " << shortfall.requested << ", placed "
           << shortfall.placed << ", " << contested << " lost to "
           << (shortfall.lost_to_earlier_declared ? "an earlier-declared budget"
                                                  : "another budget")
           << ", " << (gap - contested) << " with nobody eligible\n";
  }
  if (shortfalls.size() > reported) {
    report << "    [SEED SHORTFALL] " << (shortfalls.size() - reported)
           << " further short budgets suppressed\n";
  }
  return report.str();
}

}  // namespace june
