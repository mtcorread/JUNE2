#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace june {

// One seed budget that could not be filled, and why. A unit can fall short
// against several of its budgets at once — overlapping target groups make its
// budgets compete for the same people — so the budget is named alongside the
// unit.
//
// A shortfall has exactly two causes and the record separates them. `lost`
// counts people the budget was offered whom another budget of the same unit
// took first; the rest of the gap drew no offer at all, which means nobody
// eligible anywhere rather than nobody on this rank. Reporting only the total
// used to attribute the whole gap to the second cause, so a run that
// under-seeded because two budgets contested the same people explained itself
// with a population that was never short. See ADR 0011.
//
// `lost` may exceed the gap: a budget asking for one case can lose ten
// contested people. Only min(lost, requested - placed) of it could have gone
// to this budget, so the report clamps rather than subtracting blind.
struct SeedShortfall {
  std::string seed_name;
  std::string geo_level;
  std::string unit_id;
  size_t budget_index = 0;
  // The target group the budget stands for, empty when the seed's groups carry
  // no label — bulk CSV seeds build a criteria profile per row — in which case
  // the index names the budget alone.
  std::string budget_label;
  int requested = 0;
  int placed = 0;
  int lost = 0;
  // The clustered path resolves a contest by declaration order rather than by
  // key, so a budget there loses people specifically to an earlier-declared
  // budget (ADR 0013). Same count, a different and worse reason, named as such.
  bool lost_to_earlier_declared = false;
};

// Format the shortfalls of one seeding step as one warning block, empty when
// nothing fell short. Pure, so rank 0 can emit it with no extra collective:
// every rank sees the same pooled offers and so derives the same records.
std::string formatSeedShortfallReport(
    const std::vector<SeedShortfall>& shortfalls);

}  // namespace june
