#pragma once

#include <vector>

#include "epidemiology/seeding/seed_selector.h"

namespace june {

// Pools the seed offers every rank made, so each rank can select the winners
// of a globally exact budget without knowing anything about the partition.
// Epidemiology only ever sees this interface; the MPI implementation lives in
// the parallel layer, and a serial run needs no implementation at all.
class SeedOfferExchange {
 public:
  virtual ~SeedOfferExchange() = default;

  // Returns every rank's offers concatenated, identically on every rank.
  // Collective: every rank calls it the same number of times per seed event,
  // a rank with nothing to offer contributing an empty list.
  virtual std::vector<SeedOffer> pool(
      const std::vector<SeedOffer>& local_offers) const = 0;
};

}  // namespace june
