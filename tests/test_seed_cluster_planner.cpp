#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <vector>

#include "../include/epidemiology/seeding/seed_cluster_planner.h"
#include "doctest.h"

using namespace june;

namespace {

// Offers for one unit, as the ranks holding the households would make them:
// one per (candidate, budget the candidate matches), keyed by household.
// A candidate matching no budget still counts towards its household's size.
const std::vector<SeedOffer> kOffers = {
    {0x30ULL, 1, 1}, {0x30ULL, 2, 1}, {0x30ULL, 3, 1},  // three matched, size 3
    {0x10ULL, 4, 2}, {0x10ULL, 5, 2},                   // two matched, size 2
    {0x20ULL, 6, 1}, {0x20ULL, 7, 0},                   // one matched, size 2
    {0x40ULL, 8, 1}, {0x40ULL, 8, 2},                   // one matched, size 1
};

std::vector<std::vector<SeedOffer>> splitInto(const std::vector<SeedOffer>& all,
                                              size_t num_lists) {
  std::vector<std::vector<SeedOffer>> lists(num_lists);
  for (size_t i = 0; i < all.size(); ++i) {
    lists[i % num_lists].push_back(all[i]);
  }
  return lists;
}

std::vector<std::pair<PersonId, uint32_t>> named(const ClusterPlan& plan) {
  std::vector<std::pair<PersonId, uint32_t>> pairs;
  for (const auto& assignment : plan.assignments) {
    pairs.push_back({assignment.person_id, assignment.budget_index});
  }
  return pairs;
}

}  // namespace

TEST_CASE("Planner: the plan does not depend on how offers are partitioned") {
  const std::vector<int> budgets = {2, 2};

  ClusterPlan from_one_list = planClusteredSeed(splitInto(kOffers, 1), budgets);
  ClusterPlan from_three_lists =
      planClusteredSeed(splitInto(kOffers, 3), budgets);

  // Densest household first, then the next, until the total budget is met.
  CHECK(named(from_one_list) == std::vector<std::pair<PersonId, uint32_t>>{
                                    {1, 0}, {2, 0}, {4, 1}, {5, 1}});
  CHECK(named(from_three_lists) == named(from_one_list));
  CHECK(from_one_list.filled_per_budget == std::vector<int>{2, 2});
}

TEST_CASE("Planner: households of equal density are ordered by key") {
  // Two households of one matched member each: only the key can separate them.
  const std::vector<SeedOffer> low_key_first = {{0x11ULL, 1, 1},
                                                {0x22ULL, 2, 1}};
  const std::vector<SeedOffer> high_key_first = {{0x22ULL, 2, 1},
                                                 {0x11ULL, 1, 1}};

  ClusterPlan offered_low_first = planClusteredSeed({low_key_first}, {1});
  ClusterPlan offered_high_first = planClusteredSeed({high_key_first}, {1});
  ClusterPlan offered_apart =
      planClusteredSeed({{high_key_first[0]}, {high_key_first[1]}}, {1});

  CHECK(named(offered_low_first) ==
        std::vector<std::pair<PersonId, uint32_t>>{{1, 0}});
  CHECK(named(offered_high_first) == named(offered_low_first));
  CHECK(named(offered_apart) == named(offered_low_first));
}

TEST_CASE("Planner: the fill respects each budget and stops at their total") {
  // One case for the first budget, none for the second.
  const std::vector<SeedOffer> offers = {
      {0x50ULL, 1, 1}, {0x50ULL, 2, 1}, {0x50ULL, 3, 2}, {0x60ULL, 4, 1}};

  ClusterPlan plan = planClusteredSeed({offers}, {1, 0});

  // The first member takes the only case; their housemates find no budget
  // still open, and the next household is never reached.
  CHECK(named(plan) == std::vector<std::pair<PersonId, uint32_t>>{{1, 0}});
  CHECK(plan.filled_per_budget == std::vector<int>{1, 0});
}

TEST_CASE("Planner: offers sharing a key are one household, however split") {
  // The plan follows the pooled offers alone, never the lists they arrived in.
  // Merged, 0x70 is the denser household and takes the whole budget; read a
  // list at a time each piece would rank below the rival 0x69 and lose to it.
  const std::vector<std::vector<SeedOffer>> offer_lists = {
      {{0x70ULL, 1, 1}, {0x69ULL, 4, 1}},
      {{0x70ULL, 2, 1}, {0x69ULL, 5, 1}},
      {{0x70ULL, 3, 1}}};

  ClusterPlan plan = planClusteredSeed(offer_lists, {3});

  CHECK(named(plan) ==
        std::vector<std::pair<PersonId, uint32_t>>{{1, 0}, {2, 0}, {3, 0}});
}

TEST_CASE("Planner: a later-declared budget loses its member to an earlier one") {
  // One household, one matched member, and they match both budgets. The member
  // takes budget 0 because it is declared first, so budget 1 ends short with
  // nobody left to take — the declaration-order bias ADR 0011 rejects for the
  // exact path, still in force here and now counted rather than hidden.
  const std::vector<SeedOffer> offers = {{0x30ULL, 1, 1}, {0x30ULL, 1, 2}};

  ClusterPlan plan = planClusteredSeed({offers}, {1, 1});

  CHECK(plan.filled_per_budget == std::vector<int>{1, 0});
  CHECK(plan.lost_per_budget == std::vector<int>{0, 1});
}

TEST_CASE("Planner: a budget nobody matches loses nothing") {
  // The other cause. Budget 1 is short because no member ever matched it, not
  // because a member was taken from it first.
  const std::vector<SeedOffer> offers = {{0x30ULL, 1, 1}};

  ClusterPlan plan = planClusteredSeed({offers}, {1, 1});

  CHECK(plan.filled_per_budget == std::vector<int>{1, 0});
  CHECK(plan.lost_per_budget == std::vector<int>{0, 0});
}

TEST_CASE("Planner: an offer naming a budget the unit lacks is discarded") {
  // Slot n+1 means budget n, so with one budget only slots 0 and 1 exist.
  // Slot 2 names a budget that is not there: the plan must come out as though
  // that offer had never arrived, rather than indexing past the budgets.
  const std::vector<SeedOffer> well_formed = {{0x30ULL, 1, 1}, {0x30ULL, 2, 1}};
  std::vector<SeedOffer> with_stray = well_formed;
  with_stray.push_back({0x30ULL, 3, 2});

  ClusterPlan plan = planClusteredSeed({with_stray}, {2});

  CHECK(named(plan) == named(planClusteredSeed({well_formed}, {2})));
  CHECK(plan.filled_per_budget == std::vector<int>{2});
  CHECK(plan.lost_per_budget == std::vector<int>{0});
}

TEST_CASE("Planner: the last budget's slot is one past the budget count") {
  // The boundary the encoding's plus-one puts in an awkward place: with two
  // budgets the valid slots are 0, 1 and 2, and slot 2 is budget 1. A bound
  // borrowed from the exact path, where the slot is the index itself, would
  // read this as out of range and quietly starve the last budget declared.
  const std::vector<SeedOffer> offers = {{0x30ULL, 1, 2}};

  ClusterPlan plan = planClusteredSeed({offers}, {1, 1});

  CHECK(named(plan) == std::vector<std::pair<PersonId, uint32_t>>{{1, 1}});
  CHECK(plan.filled_per_budget == std::vector<int>{0, 1});
}

TEST_CASE("Planner: a rejected offer does not count towards household size") {
  // A discarded offer is not a candidate who matched nothing — it is a
  // candidate we never heard of. The distinction is visible only in the
  // density order, so it is pinned there. Household 0x22 holds one matched
  // member and one whose sole offer names a budget that does not exist;
  // household 0x11 holds one matched member and one genuine slot-0 member.
  // Read as never heard of, 0x22 is a household of one and outranks 0x11.
  // Read as matched nothing, the two are both households of two, tie on
  // density, and the lower key takes the case instead.
  const std::vector<SeedOffer> offers = {
      {0x22ULL, 1, 1}, {0x22ULL, 2, 3},  // person 2's budget does not exist
      {0x11ULL, 3, 1}, {0x11ULL, 4, 0},  // person 4 matched no budget
  };

  ClusterPlan plan = planClusteredSeed({offers}, {1});

  CHECK(named(plan) == std::vector<std::pair<PersonId, uint32_t>>{{1, 0}});
}

TEST_CASE("Planner: a budget passed over while full is not counted as a loss") {
  // Budget 0 fills, then the next member skips past it to budget 1. Budget 0
  // did not lose them to anything — it was closed — so no ordering rule would
  // have changed the outcome and the count must stay clear of it.
  const std::vector<SeedOffer> offers = {
      {0x30ULL, 1, 1}, {0x30ULL, 1, 2},  // member 1 matches both
      {0x30ULL, 2, 1}, {0x30ULL, 2, 2},  // member 2 matches both
  };

  ClusterPlan plan = planClusteredSeed({offers}, {1, 2});

  CHECK(plan.filled_per_budget == std::vector<int>{1, 1});
  CHECK(plan.lost_per_budget == std::vector<int>{0, 1});
}
