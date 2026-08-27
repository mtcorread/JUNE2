#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <random>
#include <utility>
#include <vector>

#include "../include/epidemiology/seeding/seed_selector.h"
#include "doctest.h"

using namespace june;

namespace {

// Offers as they would arrive from ranks: an arbitrary key per candidate, all
// standing against the unit's only budget.
const std::vector<SeedOffer> kOffers = {
    {0x9A11ULL, 101}, {0x0002ULL, 102}, {0x7F30ULL, 103}, {0x0100ULL, 104},
    {0xFFFFULL, 105}, {0x0041ULL, 106}, {0x5555ULL, 107}, {0x0003ULL, 108},
};

std::vector<std::vector<SeedOffer>> splitInto(const std::vector<SeedOffer>& all,
                                              size_t num_lists) {
  std::vector<std::vector<SeedOffer>> lists(num_lists);
  for (size_t i = 0; i < all.size(); ++i) {
    lists[i % num_lists].push_back(all[i]);
  }
  return lists;
}

std::vector<PersonId> chosenPeople(const SeedSelection& selection) {
  std::vector<PersonId> people;
  for (const auto& assignment : selection.chosen) {
    people.push_back(assignment.person_id);
  }
  return people;
}

}  // namespace

TEST_CASE("Selector: winners do not depend on how offers are partitioned") {
  const std::vector<int> budgets = {4};

  std::vector<PersonId> from_one_list =
      chosenPeople(selectSeedWinners(splitInto(kOffers, 1), budgets));
  std::vector<PersonId> from_two_lists =
      chosenPeople(selectSeedWinners(splitInto(kOffers, 2), budgets));
  std::vector<PersonId> from_five_lists =
      chosenPeople(selectSeedWinners(splitInto(kOffers, 5), budgets));

  CHECK(from_one_list.size() == 4);
  CHECK(from_two_lists == from_one_list);
  CHECK(from_five_lists == from_one_list);
}

TEST_CASE(
    "Selector: a budget beyond the offers takes them all and reports the "
    "shortfall") {
  SeedSelection selection = selectSeedWinners(splitInto(kOffers, 3), {11});

  CHECK(selection.chosen.size() == kOffers.size());
  CHECK(selection.filled_per_budget == std::vector<int>{8});
}

TEST_CASE("Selector: a budget of zero seeds nobody and is not short") {
  SeedSelection selection = selectSeedWinners(splitInto(kOffers, 3), {0});

  CHECK(selection.chosen.empty());
  CHECK(selection.filled_per_budget == std::vector<int>{0});
}

TEST_CASE("Selector: a candidate offered by one rank alone can still win") {
  // The lone rank's candidate holds the lowest key of the whole pool.
  std::vector<std::vector<SeedOffer>> offer_lists = splitInto(kOffers, 2);
  offer_lists.push_back({{0x0001ULL, 999}});

  SeedSelection selection = selectSeedWinners(offer_lists, {1});

  CHECK(chosenPeople(selection) == std::vector<PersonId>{999});
}

TEST_CASE("Selector: equal keys break ties by person, not by input order") {
  // Twenty candidates sharing one key: only the tiebreak can order them.
  std::vector<SeedOffer> tied;
  for (PersonId person_id = 20; person_id >= 1; --person_id) {
    tied.push_back({0x4242ULL, person_id});
  }

  SeedSelection descending_input = selectSeedWinners({tied}, {3});
  std::reverse(tied.begin(), tied.end());
  SeedSelection ascending_input = selectSeedWinners({tied}, {3});
  SeedSelection split_input = selectSeedWinners(splitInto(tied, 4), {3});

  CHECK(chosenPeople(descending_input) == std::vector<PersonId>{1, 2, 3});
  CHECK(chosenPeople(ascending_input) == chosenPeople(descending_input));
  CHECK(chosenPeople(split_input) == chosenPeople(descending_input));
}

TEST_CASE("Selector: a unit held by other ranks alone is not a shortfall") {
  // The pooled offers are what separate "nobody eligible anywhere" from
  // "nobody eligible here": a rank holding none of a unit still sees the
  // offers of the ranks that do, so it reports no shortfall.
  std::vector<std::vector<SeedOffer>> offer_lists = {{}, kOffers};

  SeedSelection selection = selectSeedWinners(offer_lists, {3});

  CHECK(selection.chosen.size() == 3);
  CHECK(selection.filled_per_budget == std::vector<int>{3});
}

TEST_CASE(
    "Selector: a candidate matching two budgets takes one case, not two") {
  // Overlapping target groups offer the same person against both budgets,
  // under a different key each time.
  std::vector<SeedOffer> offers = {
      {0x0010ULL, 201, 0},
      {0x0020ULL, 201, 1},
      {0x0030ULL, 202, 0},
      {0x0040ULL, 203, 1},
  };

  SeedSelection selection = selectSeedWinners({offers}, {1, 1});

  CHECK(selection.chosen.size() == 2);
  CHECK(chosenPeople(selection) == std::vector<PersonId>{201, 203});
  CHECK(selection.filled_per_budget == std::vector<int>{1, 1});
}

TEST_CASE(
    "Selector: the budget that loses a candidate refills from its next best "
    "offer") {
  // 301 is the best offer of both budgets; budget 1 must fall through to 302
  // rather than place one case fewer than it asked for.
  std::vector<SeedOffer> offers = {
      {0x0001ULL, 301, 0},
      {0x0002ULL, 301, 1},
      {0x0009ULL, 302, 1},
  };

  SeedSelection selection = selectSeedWinners({offers}, {1, 1});

  CHECK(chosenPeople(selection) == std::vector<PersonId>{301, 302});
  CHECK(selection.chosen[0].budget_index == 0);
  CHECK(selection.chosen[1].budget_index == 1);
  CHECK(selection.filled_per_budget == std::vector<int>{1, 1});
}

TEST_CASE(
    "Selector: a contested candidate goes to the budget that keys them best") {
  // 401 keys far better against budget 1 than budget 0, so budget 1 takes them
  // even though budget 0 was declared first, and budget 0 refills with 402.
  std::vector<SeedOffer> offers = {
      {0x0050ULL, 401, 0},
      {0x0005ULL, 401, 1},
      {0x0060ULL, 402, 0},
  };

  SeedSelection selection = selectSeedWinners({offers}, {1, 1});

  CHECK(chosenPeople(selection) == std::vector<PersonId>{401, 402});
  CHECK(selection.chosen[0].budget_index == 1);
  CHECK(selection.chosen[1].budget_index == 0);
}

TEST_CASE(
    "Selector: a narrower budget losing its only candidate is short by "
    "contest, not by population") {
  // ADR 0011's nested bands. Budget 0 is "0-17" and accepts only the child;
  // budget 1 is "0-64" and accepts both. The child keys best against budget 1,
  // takes it, and budget 0 has nobody to fall through to — while the
  // assignment child->0, adult->1 would have filled both. The greedy outcome
  // stands; what must not stand is calling the miss an empty population.
  const PersonId child = 701;
  const PersonId adult = 702;
  std::vector<SeedOffer> offers = {
      {0x0001ULL, child, 1},
      {0x0050ULL, child, 0},
      {0x0060ULL, adult, 1},
  };

  SeedSelection selection = selectSeedWinners({offers}, {1, 1});

  CHECK(chosenPeople(selection) == std::vector<PersonId>{child});
  CHECK(selection.filled_per_budget == std::vector<int>{0, 1});
  CHECK(selection.lost_per_budget == std::vector<int>{1, 0});
}

TEST_CASE("Selector: a budget short with nobody to lose says nobody") {
  // The other cause, and the one the report used to give for both: budget 0
  // asks for two and only one person anywhere matches it. Nothing was
  // contested, so nothing is counted lost.
  std::vector<SeedOffer> offers = {
      {0x0001ULL, 801, 0},
      {0x0002ULL, 802, 1},
  };

  SeedSelection selection = selectSeedWinners({offers}, {2, 1});

  CHECK(selection.filled_per_budget == std::vector<int>{1, 1});
  CHECK(selection.lost_per_budget == std::vector<int>{0, 0});
}

TEST_CASE("Selector: a short budget's offers are all placed or all lost") {
  // Why the two counts partition the gap: a budget that falls short never
  // fills, so it is open for the whole walk and no offer of its own can be
  // turned away for any reason but the person being gone.
  std::vector<SeedOffer> offers;
  for (PersonId person_id = 901; person_id <= 906; ++person_id) {
    offers.push_back({0x0100ULL + person_id, person_id, 0});
    offers.push_back({0x0010ULL + person_id, person_id, 1});
  }

  SeedSelection selection = selectSeedWinners({offers}, {4, 4});

  const int offers_against_zero = 6;
  CHECK(selection.filled_per_budget[0] + selection.lost_per_budget[0] ==
        offers_against_zero);
  CHECK(selection.filled_per_budget[0] < 4);  // short, so the identity applies
}

TEST_CASE("Selector: budgets sharing too few candidates fall short honestly") {
  // Three people, both budgets asking two, and every person matches both: the
  // seed can only place three cases, and says so.
  std::vector<SeedOffer> offers;
  for (PersonId person_id = 501; person_id <= 503; ++person_id) {
    offers.push_back({0x0100ULL + person_id, person_id, 0});
    offers.push_back({0x0200ULL + person_id, person_id, 1});
  }

  SeedSelection selection = selectSeedWinners({offers}, {2, 2});

  CHECK(selection.chosen.size() == 3);
  CHECK(selection.filled_per_budget[0] + selection.filled_per_budget[1] == 3);
}

TEST_CASE(
    "Selector: overlapping budgets resolve the same however the offers are "
    "split") {
  std::vector<SeedOffer> offers;
  for (PersonId person_id = 601; person_id <= 612; ++person_id) {
    offers.push_back({0x3000ULL + (person_id * 7919) % 4096, person_id, 0});
    if (person_id % 2 == 0) {
      offers.push_back({0x3000ULL + (person_id * 104729) % 4096, person_id, 1});
    }
  }

  SeedSelection from_one_list = selectSeedWinners({offers}, {3, 2});
  SeedSelection from_four_lists =
      selectSeedWinners(splitInto(offers, 4), {3, 2});

  CHECK(from_one_list.chosen.size() == 5);
  CHECK(chosenPeople(from_four_lists) == chosenPeople(from_one_list));
  CHECK(from_four_lists.filled_per_budget == from_one_list.filled_per_budget);
}

namespace {

// One person as the exact seeder sees them: the rank holding them, and the
// key they carry against each budget of the unit they match.
struct Candidate {
  PersonId person_id = 0;
  size_t rank = 0;
  std::vector<std::pair<uint32_t, uint64_t>> budget_keys;
};

std::vector<std::vector<SeedOffer>> offersPerRank(
    const std::vector<Candidate>& candidates, size_t num_ranks) {
  std::vector<std::vector<SeedOffer>> per_rank(num_ranks);
  for (const Candidate& candidate : candidates) {
    for (const auto& [budget_index, key] : candidate.budget_keys) {
      per_rank[candidate.rank].push_back({key, candidate.person_id,
                                          budget_index});
    }
  }
  return per_rank;
}

// What applyExactSeed contributes: each rank counts its own overlapping
// candidates, then keeps only its best seedOfferDepth offers per budget.
std::vector<std::vector<SeedOffer>> truncateAsRanksDo(
    const std::vector<Candidate>& candidates, const std::vector<int>& targets,
    size_t num_ranks, size_t* deepest_offered) {
  std::vector<std::vector<SeedOffer>> kept(num_ranks);
  for (size_t rank = 0; rank < num_ranks; ++rank) {
    std::vector<int> overlapping_per_budget(targets.size(), 0);
    std::vector<std::vector<SeedOffer>> per_budget(targets.size());
    for (const Candidate& candidate : candidates) {
      if (candidate.rank != rank) continue;
      for (const auto& [budget_index, key] : candidate.budget_keys) {
        per_budget[budget_index].push_back({key, candidate.person_id,
                                            budget_index});
        if (candidate.budget_keys.size() > 1) {
          ++overlapping_per_budget[budget_index];
        }
      }
    }
    for (size_t budget_index = 0; budget_index < targets.size();
         ++budget_index) {
      std::vector<SeedOffer>& offers = per_budget[budget_index];
      const size_t depth =
          seedOfferDepth(targets, overlapping_per_budget, budget_index);
      *deepest_offered = std::max(*deepest_offered, depth);
      std::sort(offers.begin(), offers.end(),
                [](const SeedOffer& a, const SeedOffer& b) {
                  if (a.key != b.key) return a.key < b.key;
                  return a.person_id < b.person_id;
                });
      if (offers.size() > depth) offers.resize(depth);
      kept[rank].insert(kept[rank].end(), offers.begin(), offers.end());
    }
  }
  return kept;
}

std::vector<std::pair<PersonId, uint32_t>> assignments(
    const SeedSelection& selection) {
  std::vector<std::pair<PersonId, uint32_t>> pairs;
  for (const auto& assignment : selection.chosen) {
    pairs.emplace_back(assignment.person_id, assignment.budget_index);
  }
  return pairs;
}

}  // namespace

TEST_CASE("Offer depth: truncating at it loses no winner, at any rank count") {
  // The bound only has to hold against the selector, so drive it with many
  // shapes of unit rather than one: budget counts, overlaps and rank layouts
  // that a config could produce.
  std::mt19937_64 prng(20260820);

  for (int trial = 0; trial < 400; ++trial) {
    const size_t num_budgets = 1 + prng() % 3;
    std::vector<int> targets(num_budgets);
    int total_target = 0;
    for (size_t budget_index = 0; budget_index < num_budgets; ++budget_index) {
      targets[budget_index] = static_cast<int>(prng() % 5);
      total_target += targets[budget_index];
    }

    const size_t population = 1 + prng() % 40;
    const size_t num_ranks = 1 + prng() % 4;
    std::vector<Candidate> candidates;
    for (size_t index = 0; index < population; ++index) {
      Candidate candidate;
      candidate.person_id = static_cast<PersonId>(1000 + index);
      candidate.rank = prng() % num_ranks;
      for (size_t budget_index = 0; budget_index < num_budgets;
           ++budget_index) {
        // Skewed towards matching, so overlap is common rather than rare.
        if (targets[budget_index] > 0 && prng() % 3 != 0) {
          candidate.budget_keys.emplace_back(
              static_cast<uint32_t>(budget_index), prng() % 512);
        }
      }
      if (!candidate.budget_keys.empty()) candidates.push_back(candidate);
    }

    size_t deepest_offered = 0;
    SeedSelection untruncated =
        selectSeedWinners(offersPerRank(candidates, num_ranks), targets);
    SeedSelection truncated = selectSeedWinners(
        truncateAsRanksDo(candidates, targets, num_ranks, &deepest_offered),
        targets);

    CHECK(assignments(truncated) == assignments(untruncated));
    CHECK(truncated.filled_per_budget == untruncated.filled_per_budget);
    // The point of the bound: no rank is ever asked for more offers than the
    // unit places cases, however large the population behind them.
    CHECK(deepest_offered <= static_cast<size_t>(total_target));
  }
}

TEST_CASE("Offer depth: a nested band is capped by the unit, not the band") {
  // "0-17" inside "0-64": every candidate of budget 0 also matches budget 1,
  // so the local overlap count is the whole child population. The depth must
  // not follow it.
  const std::vector<int> targets = {500, 2000};
  std::vector<int> overlapping_per_budget = {30000, 30000};

  CHECK(seedOfferDepth(targets, overlapping_per_budget, 0) == 2500);
  CHECK(seedOfferDepth(targets, overlapping_per_budget, 1) == 2500);
}

TEST_CASE("Offer depth: disjoint groups offer exactly as deep as the budget") {
  const std::vector<int> targets = {5, 10, 2};
  const std::vector<int> no_overlap = {0, 0, 0};

  CHECK(seedOfferDepth(targets, no_overlap, 0) == 5);
  CHECK(seedOfferDepth(targets, no_overlap, 1) == 10);
  CHECK(seedOfferDepth(targets, no_overlap, 2) == 2);
}

TEST_CASE("Offer depth: a lone budget cannot lose a candidate to anyone") {
  CHECK(seedOfferDepth({7}, {0}, 0) == 7);
  // A budget of zero asks for nothing, whatever overlaps it.
  CHECK(seedOfferDepth({0, 4}, {9, 9}, 0) == 0);
}
