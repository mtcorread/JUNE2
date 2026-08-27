#pragma once

#include <cstdint>
#include <vector>

#include "core/types.h"
#include "epidemiology/seeding/seed_selector.h"

namespace june {

// One person the plan seeds, and the budget the case counts against.
struct ClusterAssignment {
  PersonId person_id = 0;
  uint32_t budget_index = 0;
};

// The households a clustered seed fills, expressed as the people to infect.
// Every rank replays the same plan and infects only the people it holds, so a
// household straddling ranks needs no special case.
// `lost_per_budget` counts members a budget matched but never got the chance
// to take, because a budget declared *earlier* was still open when the member
// was placed. A budget that falls short is open throughout, so it can only lose
// a member upwards like this, never to a later-declared budget — the bias is
// the ordering rule itself, and the count is what makes it visible. Meaningful
// only for a budget that fell short, as in SeedSelection.
struct ClusterPlan {
  std::vector<ClusterAssignment> assignments;
  std::vector<int> filled_per_budget;
  std::vector<int> lost_per_budget;
};

// Plan one unit's clustered seed from the offers every rank made.
// One offer per (candidate, budget the candidate matches): `key` is the
// candidate's household, shared by all its members, and `budget_slot` is the
// budget index plus one, or zero for a candidate matching no budget at all —
// which still counts towards its household's size, never its matched members.
// The last budget's slot is therefore `budgets.size()`, and an offer above it
// names a budget the unit does not have. Such an offer is dropped whole, as
// `selectSeedWinners` drops one out of range on the exact path — not read as a
// candidate matching nothing, because an offer we cannot read is no evidence
// its candidate exists, and counting it would swell the household it claims.
// Pure: the result depends only on the multiset of offers, never on how they
// were split across the input lists — with one caveat. A candidate matching
// several budgets takes the first still open in the order its own offers
// arrive, so reordering those can move the case to another budget. Callers
// emit a candidate's budgets ascending and the pooling preserves each rank's
// order, which pins it down in practice.
ClusterPlan planClusteredSeed(
    const std::vector<std::vector<SeedOffer>>& offer_lists,
    const std::vector<int>& budgets);

}  // namespace june
