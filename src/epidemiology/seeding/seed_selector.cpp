#include "epidemiology/seeding/seed_selector.h"

#include <algorithm>
#include <unordered_set>

namespace june {

SeedSelection selectSeedWinners(
    const std::vector<std::vector<SeedOffer>>& offer_lists,
    const std::vector<int>& budgets) {
  std::vector<SeedOffer> pooled;
  for (const auto& offers : offer_lists) {
    pooled.insert(pooled.end(), offers.begin(), offers.end());
  }

  std::sort(pooled.begin(), pooled.end(),
            [](const SeedOffer& a, const SeedOffer& b) {
              // The person breaks ties, so equal keys cannot be reordered by
              // which rank happened to offer them.
              if (a.key != b.key) return a.key < b.key;
              if (a.person_id != b.person_id) return a.person_id < b.person_id;
              return a.budget_slot < b.budget_slot;
            });

  SeedSelection selection;
  selection.filled_per_budget.assign(budgets.size(), 0);
  selection.lost_per_budget.assign(budgets.size(), 0);
  // A candidate matching two budgets is offered against both, so the first
  // budget to reach them takes them, and which budget that is falls out of the
  // keys, not out of the order the budgets were declared in. The budget that
  // loses them carries on down its own order — which refills it only if a
  // candidate it accepts is still to come. A budget narrower than the one that
  // took the person may have no such candidate, and then it simply ends short:
  // the loss is counted here so the shortfall can name that as the cause
  // instead of blaming an eligible population that was never the problem.
  //
  // A person is held by one rank, so the pooled offers hold at most one
  // (person, budget) pair and the count needs no set of its own.
  std::unordered_set<PersonId> already_seeded;
  for (const auto& offer : pooled) {
    if (offer.budget_slot >= budgets.size()) continue;
    if (already_seeded.count(offer.person_id) != 0) {
      ++selection.lost_per_budget[offer.budget_slot];
      continue;
    }
    if (selection.filled_per_budget[offer.budget_slot] >=
        budgets[offer.budget_slot]) {
      continue;
    }
    selection.chosen.push_back({offer.person_id, offer.budget_slot});
    already_seeded.insert(offer.person_id);
    ++selection.filled_per_budget[offer.budget_slot];
  }
  return selection;
}

size_t seedOfferDepth(const std::vector<int>& targets,
                      const std::vector<int>& overlapping_per_budget,
                      size_t budget_index) {
  const int own = targets[budget_index];
  // A budget placing nothing wins nobody, so it need offer nobody.
  if (own <= 0) return 0;
  int total_target = 0;
  for (int cases : targets) total_target += cases;
  const int stolen =
      std::min(overlapping_per_budget[budget_index], total_target - own);
  return static_cast<size_t>(own + stolen);
}

}  // namespace june
