#pragma once

namespace june {

// True on MPI rank 0 (and unconditionally true when MPI is not initialised
// or not compiled in). Gates load-time log lines that would otherwise be
// repeated once per rank.
bool logRank0();

}  // namespace june
