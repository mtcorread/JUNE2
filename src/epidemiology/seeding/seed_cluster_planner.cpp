#include "epidemiology/seeding/seed_cluster_planner.h"

#include <algorithm>
#include <map>

namespace june {

namespace {

// One household as the pooled offers describe it: its candidate members, each
// with the budgets it matches. A household split across ranks arrives as
// several groups of offers sharing a key and merges here into one.
struct Household {
  uint64_t key = 0;
  std::map<PersonId, std::vector<uint32_t>> members;
  size_t matchedMembers() const {
    size_t matched = 0;
    for (const auto& [person_id, budgets] : members) {
      (void)person_id;
      if (!budgets.empty()) ++matched;
    }
    return matched;
  }
};

// Denser households first, by the seed's own score, matched / sqrt(size).
// Compared as the integer identity matched_a^2 * size_b > matched_b^2 * size_a
// so the order cannot turn on two ranks' libm agreeing bit for bit.
bool denserFirst(const Household& a, const Household& b) {
  const uint64_t matched_a = a.matchedMembers();
  const uint64_t matched_b = b.matchedMembers();
  const uint64_t left = matched_a * matched_a * b.members.size();
  const uint64_t right = matched_b * matched_b * a.members.size();
  if (left != right) return left > right;
  return a.key < b.key;
}

}  // namespace

ClusterPlan planClusteredSeed(
    const std::vector<std::vector<SeedOffer>>& offer_lists,
    const std::vector<int>& budgets) {
  std::map<uint64_t, Household> households_by_key;
  for (const auto& offers : offer_lists) {
    for (const auto& offer : offers) {
      // Slot 0 is a candidate matching no budget and slot n+1 is budget n, so
      // the last budget's slot is budgets.size() and anything above it names a
      // budget the unit does not have. Rejected before the household is
      // touched: an offer we cannot read is no evidence its candidate exists,
      // so it must not swell the household's size either.
      if (offer.budget_slot > budgets.size()) continue;
      Household& household = households_by_key[offer.key];
      household.key = offer.key;
      auto& matched_budgets = household.members[offer.person_id];
      if (offer.budget_slot > 0) {
        matched_budgets.push_back(offer.budget_slot - 1);
      }
    }
  }

  std::vector<Household> households;
  for (auto& [key, household] : households_by_key) {
    (void)key;
    if (household.matchedMembers() > 0) households.push_back(household);
  }
  std::sort(households.begin(), households.end(), denserFirst);

  int total_budget = 0;
  for (int budget : budgets) total_budget += budget;

  ClusterPlan plan;
  plan.filled_per_budget.assign(budgets.size(), 0);
  plan.lost_per_budget.assign(budgets.size(), 0);
  int filled = 0;
  for (const auto& household : households) {
    if (filled >= total_budget) break;
    for (const auto& [person_id, matched_budgets] : household.members) {
      // A person takes the first budget still open to them, in the order their
      // offers arrived, so a household whose members all match the same
      // exhausted budget yields nothing.
      for (size_t offered = 0; offered < matched_budgets.size(); ++offered) {
        const uint32_t budget_index = matched_budgets[offered];
        if (plan.filled_per_budget[budget_index] >= budgets[budget_index]) {
          continue;
        }
        plan.assignments.push_back({person_id, budget_index});
        ++plan.filled_per_budget[budget_index];
        ++filled;
        // The budgets this member also matched but never reached. Every one of
        // them was still open — the loop stopped before testing it — so each
        // lost a member it could have taken to a budget declared before it.
        // The budgets already passed over are not counted: those were closed,
        // which is why the member moved on, and no ordering rule would have
        // changed that.
        for (size_t skipped = offered + 1; skipped < matched_budgets.size();
             ++skipped) {
          ++plan.lost_per_budget[matched_budgets[skipped]];
        }
        break;
      }
    }
  }
  return plan;
}

}  // namespace june
