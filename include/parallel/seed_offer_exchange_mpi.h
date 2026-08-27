#pragma once

#ifdef USE_MPI

#include <vector>

#include "epidemiology/seeding/seed_offer_exchange.h"

namespace june {

// Pools structured-seed offers across the communicator with a single
// Allgatherv per seed event, however many units and budgets the event carries.
// A rank holding none of the seeded units contributes a zero-length slice, so
// there is no early return to deadlock on.
class MpiSeedOfferExchange : public SeedOfferExchange {
 public:
  std::vector<SeedOffer> pool(
      const std::vector<SeedOffer>& local_offers) const override;
};

}  // namespace june

#endif  // USE_MPI
