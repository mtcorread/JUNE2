#include "utils/mpi_logging.h"

#ifdef USE_MPI
#include <mpi.h>
#endif

namespace june {

bool logRank0() {
#ifdef USE_MPI
  int initialized = 0;
  MPI_Initialized(&initialized);
  if (!initialized) return true;
  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  return rank == 0;
#else
  return true;
#endif
}

}  // namespace june
