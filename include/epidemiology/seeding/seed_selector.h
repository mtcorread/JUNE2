#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/types.h"

namespace june {

// One rank's offer of one candidate against one seed budget: the candidate's
// deterministic key, derived from the run seed and the person's own identity,
// so the key does not depend on which rank holds the person.
struct SeedOffer {
  uint64_t key = 0;
  PersonId person_id = 0;
  // Which budget of the seed event this offer stands against.
  uint32_t budget_slot = 0;
};

// One person the plan seeds, and the budget the case counts against. A person
// appears at most once: a candidate matching two budgets takes one case, not
// two, whatever the target groups overlap.
struct SeedAssignment {
  PersonId person_id = 0;
  uint32_t budget_index = 0;
};

// The winners of one unit's budgets, plus how many cases each budget placed
// and how many of its offers named a person another budget of the unit had
// already taken.
//
// A budget that falls short never fills, so it stays open for the whole walk
// and every one of its offers was either taken or skipped for a person already
// seeded: `filled + lost` is exactly the offers it saw. The rest of its request
// drew no offer at all, and by the depth cap (ADR 0011) nothing that could have
// won is ever truncated away, so that remainder is people who do not exist
// rather than people not proposed.
//
// `lost_per_budget` is only meaningful for a budget that fell short. For a
// budget that filled, offers are skipped for the ordinary reason that it
// closed, and the count says nothing about contention.
struct SeedSelection {
  std::vector<SeedAssignment> chosen;
  std::vector<int> filled_per_budget;
  std::vector<int> lost_per_budget;
};

// Choose the winners of one unit's seed budgets from the offers every rank
// made. `budget_slot` on an offer is the budget it stands against, indexed
// within the unit.
// Pure: the result depends only on the multiset of offers, never on how they
// were split across the input lists, so every rank reaches the same answer
// from the same pooled offers.
SeedSelection selectSeedWinners(
    const std::vector<std::vector<SeedOffer>>& offer_lists,
    const std::vector<int>& budgets);

// How deep into one rank's own order for a budget an offer can sit and still
// win globally, so that a rank need contribute no more than this many offers
// per budget. Two things can outrank a candidate: offers that take a case
// against the same budget, of which there are at most `targets[budget_index]`,
// and offers whose person a *different* budget of the unit took, which is only
// possible for a person matching more than one budget. The second kind is
// counted locally by `overlapping_per_budget`, but it is also capped by the
// cases every other budget places, so the unit's total is a bound on both
// together however the target groups overlap. Without that cap a nested band
// — every "0-17" is also a "0-64" — makes the depth grow with the population
// rather than the budgets. See ADR 0011.
size_t seedOfferDepth(const std::vector<int>& targets,
                      const std::vector<int>& overlapping_per_budget,
                      size_t budget_index);

}  // namespace june
