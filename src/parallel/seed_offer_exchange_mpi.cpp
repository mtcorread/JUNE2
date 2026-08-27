#ifdef USE_MPI

#include "parallel/seed_offer_exchange_mpi.h"

#include <mpi.h>

#include <type_traits>

#include "parallel/mpi_utils.h"

namespace june {

static_assert(std::is_trivially_copyable<SeedOffer>::value,
              "SeedOffer is exchanged as raw bytes");

std::vector<SeedOffer> MpiSeedOfferExchange::pool(
    const std::vector<SeedOffer>& local_offers) const {
  int num_ranks = 1;
  MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

  const int offer_bytes = static_cast<int>(sizeof(SeedOffer));
  int local_bytes = static_cast<int>(local_offers.size()) * offer_bytes;

  std::vector<int> all_bytes(num_ranks);
  MPI_Allgather(&local_bytes, 1, MPI_INT, all_bytes.data(), 1, MPI_INT,
                MPI_COMM_WORLD);

  std::vector<int> displacements;
  int total_bytes = 0;
  mpi_utils::computeDisplacements(all_bytes, displacements, total_bytes);

  std::vector<SeedOffer> pooled(total_bytes / offer_bytes);
  MPI_Allgatherv(local_offers.data(), local_bytes, MPI_BYTE, pooled.data(),
                 all_bytes.data(), displacements.data(), MPI_BYTE,
                 MPI_COMM_WORLD);
  return pooled;
}

}  // namespace june

#endif  // USE_MPI
