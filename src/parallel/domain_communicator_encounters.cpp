#ifdef USE_MPI

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "parallel/domain_communicator.h"
#include "parallel/domain_communicator_detail.h"
#include "parallel/domain_manager.h"
#include "parallel/mpi_utils.h"
#include "utils/deterministic_rng.h"
#include "utils/random.h"

namespace {

using june::domain_comm_detail::makeWireRecord;
using june::domain_comm_detail::packField;
using june::domain_comm_detail::unpackField;

// Single field list per record type; wire size, pack, and unpack are all
// derived from it (see WireRecord in domain_communicator_detail.h) instead
// of being hand-listed separately in each of those three places.
constexpr auto kProposalWire = makeWireRecord(
    &june::EncounterProposal::encounter_id, &june::EncounterProposal::host_id,
    &june::EncounterProposal::host_rank, &june::EncounterProposal::invitee_id,
    &june::EncounterProposal::venue_id,
    &june::EncounterProposal::venue_owner_rank,
    &june::EncounterProposal::venue_type_id, &june::EncounterProposal::slot,
    &june::EncounterProposal::encounter_type_id);
constexpr int PROPOSAL_WIRE_SIZE = kProposalWire.size();
// Tripwire: EncounterProposal is all-scalar fields, all listed above, so its
// sizeof is a proxy for "did a field get added/removed/resized". Not exact
// (a same-or-smaller field could land in trailing padding unnoticed), but
// catches the common case. If this fires, update kProposalWire and the
// literal below together.
static_assert(sizeof(june::EncounterProposal) == 36,
              "EncounterProposal size changed - check kProposalWire covers "
              "every field, then update this literal");

char* packProposal(char* ptr, const june::EncounterProposal& p) {
  return kProposalWire.pack(ptr, p);
}

const char* unpackProposal(const char* ptr, june::EncounterProposal& p) {
  return kProposalWire.unpack(ptr, p);
}

// kReplyWire covers the plain fields only; status is a ReplyStatus enum,
// deliberately narrowed to a fixed uint8_t rather than memcpy'd directly, so
// it stays outside WireRecord and is transcoded manually below.
constexpr auto kReplyWire = makeWireRecord(
    &june::EncounterReply::encounter_id, &june::EncounterReply::host_id,
    &june::EncounterReply::invitee_id, &june::EncounterReply::venue_id,
    &june::EncounterReply::venue_type_id, &june::EncounterReply::slot,
    &june::EncounterReply::encounter_type_id);
constexpr int REPLY_WIRE_SIZE = kReplyWire.size() + sizeof(uint8_t);
// Tripwire: EncounterReply is all-scalar (kReplyWire's fields plus the
// status enum packed manually below), so sizeof is a proxy for drift. Same
// caveat as PROPOSAL_WIRE_SIZE's tripwire above.
static_assert(sizeof(june::EncounterReply) == 28,
              "EncounterReply size changed - check kReplyWire (+ status_byte) "
              "covers every field, then update this literal");

// home_array_index is never on the wire (local-only; defaults to -1 and
// stays there after unpack), so it's skipped in this field list.
constexpr auto kInfectionWire = makeWireRecord(
    &june::PendingInfection::person_id, &june::PendingInfection::infector_id,
    &june::PendingInfection::infection_time,
    &june::PendingInfection::venue_type_id,
    &june::PendingInfection::encounter_type_id,
    &june::PendingInfection::venue_id,
    &june::PendingInfection::infector_symptom_id,
    &june::PendingInfection::transmission_mode_index);
// Tripwire: PendingInfection is all-scalar (kInfectionWire's fields plus
// the excluded local-only home_array_index), so sizeof is a proxy for
// drift. Same caveat as PROPOSAL_WIRE_SIZE's tripwire above.
static_assert(sizeof(june::PendingInfection) == 32,
              "PendingInfection size changed - check kInfectionWire (+ "
              "home_array_index) covers every field, then update this literal");

// Fixed header of a finalized encounter; participant_count + the
// variable-length participants tail are appended manually around this (see
// packFinalizedLocal / unpackFinalizedFromRank).
constexpr auto kFinalizedWire = makeWireRecord(
    &june::CoordinatedEncounter::encounter_id,
    &june::CoordinatedEncounter::host_id, &june::CoordinatedEncounter::venue_id,
    &june::CoordinatedEncounter::venue_type_id,
    &june::CoordinatedEncounter::slot,
    &june::CoordinatedEncounter::encounter_type_id,
    &june::CoordinatedEncounter::host_subset_index);
// Tripwire: unlike the scalar-only structs above, CoordinatedEncounter ends
// in a std::set<PersonId> participants (packed manually as a count-prefixed
// tail, not via WireRecord), so its sizeof is dominated by the set's own
// object layout and says nothing about the header. The header/tail boundary
// is instead checked in tests/test_domain_communicator_detail.cpp - not here
// as a static_assert(offsetof(...)), because a std::set member makes
// CoordinatedEncounter non-standard-layout, and offsetof on a
// non-standard-layout type is only conditionally-supported (GCC/Clang accept
// it with a -Winvalid-offsetof warning, but the computed value isn't a
// portable guarantee across standard libraries). The test uses well-defined
// pointer subtraction on a real object instead.

char* packReply(char* ptr, const june::EncounterReply& r) {
  ptr = kReplyWire.pack(ptr, r);
  uint8_t status_byte = static_cast<uint8_t>(r.status);
  ptr = packField(ptr, status_byte);
  return ptr;
}

const char* unpackReply(const char* ptr, june::EncounterReply& r) {
  ptr = kReplyWire.unpack(ptr, r);
  uint8_t status_byte;
  ptr = unpackField(ptr, status_byte);
  r.status = static_cast<june::ReplyStatus>(status_byte);
  return ptr;
}

// =============================================================================
// Diagnostic helpers for hunting MPI proposal/reply corruption.
// These check structural sanity only; no masking or clamping of values.
// On first failure the process MPI_Aborts with rich context so SLURM captures
// the diagnostic before any downstream collective can deadlock.
// =============================================================================

// Returns empty string if the proposal looks structurally sane. Otherwise
// returns a short description of the first field that failed. We use loose
// upper bounds (256 for venue_type_id/slot, encounter_type_names.size() for
// encounter_type_id), tight enough to catch the garbage values we've seen
// (0x3FF00000 etc.), loose enough not to accuse legitimate values.
std::string validateProposal(const june::EncounterProposal& p, int num_ranks,
                             size_t num_encounter_types) {
  if (p.encounter_id < 0) return "encounter_id < 0";
  if (p.host_id < 0) return "host_id < 0";
  if (p.host_rank < 0 || p.host_rank >= num_ranks)
    return "host_rank out of [0,num_ranks)";
  if (p.invitee_id < 0) return "invitee_id < 0";
  if (p.venue_owner_rank < 0 || p.venue_owner_rank >= num_ranks)
    return "venue_owner_rank out of [0,num_ranks)";
  if (p.venue_type_id < 0 || p.venue_type_id >= 256)
    return "venue_type_id out of [0,256)";
  if (p.slot < 0 || p.slot >= 256) return "slot out of [0,256)";
  if (num_encounter_types > 0 && p.encounter_type_id >= num_encounter_types)
    return "encounter_type_id >= encounter_type_names.size()";
  return {};
}

std::string validateReply(const june::EncounterReply& r,
                          size_t num_encounter_types) {
  if (r.encounter_id < 0) return "encounter_id < 0";
  if (r.host_id < 0) return "host_id < 0";
  if (r.invitee_id < 0) return "invitee_id < 0";
  if (r.venue_type_id < 0 || r.venue_type_id >= 256)
    return "venue_type_id out of [0,256)";
  if (r.slot < 0 || r.slot >= 256) return "slot out of [0,256)";
  if (num_encounter_types > 0 && r.encounter_type_id >= num_encounter_types)
    return "encounter_type_id >= encounter_type_names.size()";
  if (static_cast<uint8_t>(r.status) > 6) return "status out of range";
  return {};
}

void dumpProposal(std::ostream& os, const june::EncounterProposal& p) {
  os << "{encounter_id=" << p.encounter_id << " host_id=" << p.host_id
     << " host_rank=" << p.host_rank << " invitee_id=" << p.invitee_id
     << " venue_id=" << p.venue_id << " venue_owner_rank=" << p.venue_owner_rank
     << " venue_type_id=" << p.venue_type_id << " slot=" << p.slot
     << " encounter_type_id=" << static_cast<int>(p.encounter_type_id) << "}";
}

void dumpReply(std::ostream& os, const june::EncounterReply& r) {
  os << "{encounter_id=" << r.encounter_id << " host_id=" << r.host_id
     << " invitee_id=" << r.invitee_id << " venue_id=" << r.venue_id
     << " venue_type_id=" << r.venue_type_id << " slot=" << r.slot
     << " encounter_type_id=" << static_cast<int>(r.encounter_type_id)
     << " status=" << static_cast<int>(r.status) << "}";
}

// Hex-dumps a range of bytes around a focal offset. 'focal' bytes are marked
// with '>' at the start of their 16-byte row so the record in question is
// easy to spot.
void hexDumpRegion(std::ostream& os, const char* base, size_t buf_len,
                   size_t focal_offset, size_t focal_len,
                   size_t context_bytes = 64) {
  auto flags = os.flags();
  auto fill = os.fill();
  size_t start =
      (focal_offset > context_bytes) ? focal_offset - context_bytes : 0;
  size_t end = std::min(buf_len, focal_offset + focal_len + context_bytes);
  for (size_t row = start - (start % 16); row < end; row += 16) {
    bool in_focal =
        (row + 16 > focal_offset) && (row < focal_offset + focal_len);
    os << (in_focal ? "  > " : "    ") << std::setw(6) << std::setfill('0')
       << std::hex << row << ":";
    for (size_t col = 0; col < 16; ++col) {
      size_t off = row + col;
      if (off < start || off >= end) {
        os << "   ";
      } else {
        os << ' ' << std::setw(2) << std::setfill('0') << std::hex
           << static_cast<int>(static_cast<unsigned char>(base[off]));
      }
    }
    os << '\n';
  }
  os.flags(flags);
  os.fill(fill);
  os << std::dec;
}

// Checks that per-rank byte counts fit in int (MPI_Alltoallv uses int counts).
// Returns true on success; on failure prints context and calls MPI_Abort.
bool checkByteOverflow(const std::vector<int>& counts, int wire_size, int rank,
                       const char* tag) {
  const int64_t int_max = std::numeric_limits<int>::max();
  int64_t running = 0;
  for (size_t r = 0; r < counts.size(); ++r) {
    int64_t bytes = static_cast<int64_t>(counts[r]) * wire_size;
    if (counts[r] < 0 || bytes > int_max) {
      std::cerr << "[Rank " << rank << "] FATAL: " << tag
                << " byte-count overflow: counts[" << r << "]=" << counts[r]
                << " * wire_size=" << wire_size << " = " << bytes
                << " (int max " << int_max << ")" << std::endl;
      MPI_Abort(MPI_COMM_WORLD, 101);
      return false;
    }
    running += bytes;
    if (running > int_max) {
      std::cerr << "[Rank " << rank << "] FATAL: " << tag
                << " cumulative byte total overflows int at rank " << r
                << " (running=" << running << ")" << std::endl;
      MPI_Abort(MPI_COMM_WORLD, 102);
      return false;
    }
  }
  return true;
}

void dumpCountsAndDispls(std::ostream& os, const std::vector<int>& send_counts,
                         const std::vector<int>& recv_counts,
                         const std::vector<int>& sd, const std::vector<int>& rd,
                         int total_send, int total_recv) {
  os << "    send_counts:";
  for (int c : send_counts) os << ' ' << c;
  os << "\n    recv_counts:";
  for (int c : recv_counts) os << ' ' << c;
  os << "\n    send_byte_displs:";
  for (int d : sd) os << ' ' << d;
  os << "\n    recv_byte_displs:";
  for (int d : rd) os << ' ' << d;
  os << "\n    total_send_bytes=" << total_send
     << " total_recv_bytes=" << total_recv << '\n';
}

// =============================================================================
// Templated route-and-Alltoallv exchange shared by encounter proposals and
// replies. Both call sites had a near-identical 150-LOC body that differed
// only in the record type, wire size, per-record validate/dump/pack/unpack
// functions, routing field, abort codes, and diagnostic tag ("proposal" /
// "reply"). All of those are passed in; the byte-level wire format is the
// caller's responsibility (delegated to pack_fn / unpack_fn).
//
// Template parameters are deduced; pack_fn / unpack_fn / etc. are passed as
// free functions or lambdas: no std::function, no heap, no vtable dispatch.
// =============================================================================
template <typename T, typename Pack, typename Unpack, typename Validate,
          typename Dump, typename Route>
void exchangeRoutedRecords(const std::vector<T>& local_records,
                           std::vector<T>& records_for_this_rank, int rank,
                           int num_ranks, int wire_size, const char* kind,
                           int before_abort, int total_neg_abort,
                           int pack_abort, int unpack_abort, Pack pack_fn,
                           Unpack unpack_fn, Validate validate_fn, Dump dump_fn,
                           Route route_fn) {
  // [DIAG] Validate every local record before anything else. If we see
  // corruption here, the bug is in the generator, not in MPI.
  for (size_t i = 0; i < local_records.size(); ++i) {
    std::string err = validate_fn(local_records[i]);
    if (!err.empty()) {
      std::cerr << "[Rank " << rank << "] FATAL: corrupt local " << kind
                << " BEFORE MPI exchange"
                << " at local_records[" << i << "/" << local_records.size()
                << "]: " << err << "\n  ";
      dump_fn(std::cerr, local_records[i]);
      std::cerr << std::endl;
      MPI_Abort(MPI_COMM_WORLD, before_abort);
    }
  }

  // Route each record to its target rank. Unknown rank -> keep locally.
  std::vector<std::vector<T>> per_rank(num_ranks);
  for (const auto& rec : local_records) {
    int target = route_fn(rec);
    if (target < 0 || target >= num_ranks) {
      per_rank[rank].push_back(rec);
    } else {
      per_rank[target].push_back(rec);
    }
  }

  records_for_this_rank = std::move(per_rank[rank]);

  std::vector<int> send_counts(num_ranks, 0);
  for (int r = 0; r < num_ranks; ++r) {
    send_counts[r] = static_cast<int>(per_rank[r].size());
  }
  send_counts[rank] = 0;  // Don't send to self

  std::string send_tag = std::string(kind) + " send";
  if (!checkByteOverflow(send_counts, wire_size, rank, send_tag.c_str())) {
    return;
  }

  std::vector<int> recv_counts(num_ranks, 0);
  MPI_Alltoall(send_counts.data(), 1, MPI_INT, recv_counts.data(), 1, MPI_INT,
               MPI_COMM_WORLD);

  std::string recv_tag = std::string(kind) + " recv";
  if (!checkByteOverflow(recv_counts, wire_size, rank, recv_tag.c_str())) {
    return;
  }

  std::vector<int> sd, rd;
  int total_send, total_recv;
  june::mpi_utils::computeByteDisplacements(send_counts, wire_size, sd,
                                            total_send);
  june::mpi_utils::computeByteDisplacements(recv_counts, wire_size, rd,
                                            total_recv);

  if (total_send < 0 || total_recv < 0) {
    std::cerr << "[Rank " << rank << "] FATAL: " << kind
              << " byte totals negative after "
                 "computeByteDisplacements: total_send="
              << total_send << " total_recv=" << total_recv << std::endl;
    MPI_Abort(MPI_COMM_WORLD, total_neg_abort);
  }

  std::vector<char> sbuf(total_send);
  std::vector<char> rbuf(total_recv);

  // Pack + validate each record on the way out. A second validate here
  // catches any corruption introduced between the BEFORE check and now.
  for (int r = 0; r < num_ranks; ++r) {
    if (r == rank) continue;
    char* ptr = sbuf.data() + sd[r];
    for (size_t i = 0; i < per_rank[r].size(); ++i) {
      const auto& rec = per_rank[r][i];
      std::string err = validate_fn(rec);
      if (!err.empty()) {
        std::cerr << "[Rank " << rank << "] FATAL: corrupt " << kind
                  << " at PACK time for target rank " << r << ", per_rank[" << r
                  << "][" << i << "/" << per_rank[r].size() << "]: " << err
                  << "\n  ";
        dump_fn(std::cerr, rec);
        std::cerr << std::endl;
        MPI_Abort(MPI_COMM_WORLD, pack_abort);
      }
      ptr = pack_fn(ptr, rec);
    }
  }

  std::vector<int> sc(num_ranks), rc(num_ranks);
  for (int i = 0; i < num_ranks; ++i) {
    sc[i] = send_counts[i] * wire_size;
    rc[i] = recv_counts[i] * wire_size;
  }
  MPI_Alltoallv(sbuf.data(), sc.data(), sd.data(), MPI_BYTE, rbuf.data(),
                rc.data(), rd.data(), MPI_BYTE, MPI_COMM_WORLD);

  // Unpack + validate. First corrupt record gets a rich dump then MPI_Abort.
  for (int r = 0; r < num_ranks; ++r) {
    if (r == rank) continue;
    const char* ptr = rbuf.data() + rd[r];
    for (int i = 0; i < recv_counts[r]; ++i) {
      const char* record_start = ptr;
      T rec;
      ptr = unpack_fn(ptr, rec);
      std::string err = validate_fn(rec);
      if (!err.empty()) {
        size_t offset = static_cast<size_t>(record_start - rbuf.data());
        std::cerr << "[Rank " << rank << "] FATAL: corrupt " << kind
                  << " AFTER MPI unpack"
                  << " from rank " << r << ", record " << i << "/"
                  << recv_counts[r] << " at buffer offset " << offset
                  << " (wire_size=" << wire_size << "): " << err << "\n  ";
        dump_fn(std::cerr, rec);
        std::cerr << "\n";
        dumpCountsAndDispls(std::cerr, send_counts, recv_counts, sd, rd,
                            total_send, total_recv);
        std::cerr << "  hex dump of recv buffer around offset " << offset
                  << ":\n";
        hexDumpRegion(std::cerr, rbuf.data(), static_cast<size_t>(total_recv),
                      offset, static_cast<size_t>(wire_size));
        std::cerr.flush();
        MPI_Abort(MPI_COMM_WORLD, unpack_abort);
      }
      records_for_this_rank.push_back(rec);
    }
  }
}

// Walks one remote rank's slice of the Allgatherv receive buffer,
// unpacks each finalized encounter, and keeps the ones with at least one
// local participant. Mirrors packFinalizedLocal's layout.
void unpackFinalizedFromRank(
    const char* ptr, const char* end, const june::DomainManager& dm,
    int local_rank,
    std::vector<june::CoordinatedEncounter>& finalized_for_this_rank) {
  while (ptr < end) {
    june::CoordinatedEncounter enc;
    ptr = kFinalizedWire.unpack(ptr, enc);
    int participant_count;
    ptr = unpackField(ptr, participant_count);
    bool has_local = false;
    for (int i = 0; i < participant_count; ++i) {
      june::PersonId pid;
      ptr = unpackField(ptr, pid);
      enc.participants.insert(pid);
      if (dm.getPersonRank(pid) == local_rank) has_local = true;
    }
    if (has_local) {
      finalized_for_this_rank.push_back(enc);
    }
  }
}

// Serializes one rank's finalized encounters into the variable-length
// Allgatherv send buffer. Layout per encounter: kFinalizedWire's fixed
// header + participant_count (4 bytes) + participant_count * PersonId
// (4 bytes each). All sizes are computed up-front per record so the buffer
// is resized once per record (no slot left to grow on participant_count
// drift).
std::vector<char> packFinalizedLocal(
    const std::vector<june::CoordinatedEncounter>& local_finalized) {
  std::vector<char> local_buf;
  for (const auto& enc : local_finalized) {
    int participant_count = static_cast<int>(enc.participants.size());
    size_t entry_size = kFinalizedWire.size() + sizeof(int) +
                        participant_count * sizeof(june::PersonId);
    size_t offset = local_buf.size();
    local_buf.resize(offset + entry_size);
    char* ptr = local_buf.data() + offset;

    ptr = kFinalizedWire.pack(ptr, enc);
    ptr = packField(ptr, participant_count);
    for (june::PersonId pid : enc.participants) {
      ptr = packField(ptr, pid);
    }
  }
  return local_buf;
}

}  // anonymous namespace

namespace june {

std::vector<PendingInfection> DomainCommunicator::receivePendingInfections(
    const std::vector<PendingInfection>& pending) {
  std::vector<std::vector<PendingInfection>> updates;
  std::vector<int> send_counts;
  routePendingByHomeRank(pending, updates, send_counts);

  std::vector<int> recv_counts(num_ranks_, 0);
  MPI_Alltoall(send_counts.data(), 1, MPI_INT, recv_counts.data(), 1, MPI_INT,
               MPI_COMM_WORLD);

  const int INFECTION_SIZE = kInfectionWire.size();
  std::vector<int> sd, rd;
  int stotal, rtotal;
  mpi_utils::computeByteDisplacements(send_counts, INFECTION_SIZE, sd, stotal);
  mpi_utils::computeByteDisplacements(recv_counts, INFECTION_SIZE, rd, rtotal);

  std::vector<char> sbuf(stotal);
  std::vector<char> rbuf(rtotal);

  for (int r = 0; r < num_ranks_; ++r) {
    char* ptr = sbuf.data() + sd[r];
    for (const auto& u : updates[r]) {
      ptr = kInfectionWire.pack(ptr, u);
    }
  }

  std::vector<int> sc(num_ranks_), rc(num_ranks_);
  for (int i = 0; i < num_ranks_; ++i) {
    sc[i] = send_counts[i] * INFECTION_SIZE;
    rc[i] = recv_counts[i] * INFECTION_SIZE;
  }
  MPI_Alltoallv(sbuf.data(), sc.data(), sd.data(), MPI_BYTE, rbuf.data(),
                rc.data(), rd.data(), MPI_BYTE, MPI_COMM_WORLD);

  return unpackAndApplyIncoming(rbuf, rd, recv_counts);
}

std::vector<PendingInfection> DomainCommunicator::unpackAndApplyIncoming(
    const std::vector<char>& rbuf, const std::vector<int>& rd,
    const std::vector<int>& recv_counts) {
  std::vector<PendingInfection> newly_infected;
  for (int r = 0; r < num_ranks_; ++r) {
    if (r == rank_) continue;
    const char* ptr = rbuf.data() + rd[r];
    for (int i = 0; i < recv_counts[r]; ++i) {
      PendingInfection record;
      ptr = kInfectionWire.unpack(ptr, record);

      if (auto applied = applyOnePendingInfection(record)) {
        newly_infected.push_back(*applied);
      }
    }
  }
  return newly_infected;
}

void DomainCommunicator::routePendingByHomeRank(
    const std::vector<PendingInfection>& pending,
    std::vector<std::vector<PendingInfection>>& updates,
    std::vector<int>& send_counts) {
  updates.assign(num_ranks_, {});
  send_counts.assign(num_ranks_, 0);

  for (const auto& p : pending) {
    for (const auto& v : domain_.incoming_visitors) {
      if (v.person_id == p.person_id) {
        updates[v.home_rank].push_back(p);
        send_counts[v.home_rank]++;
        break;
      }
    }
  }
}

std::optional<PendingInfection> DomainCommunicator::applyOnePendingInfection(
    const PendingInfection& pending) {
  Person* person = world_.getPerson(pending.person_id);
  if (!person || person->infection || !domain_.ownsPerson(pending.person_id) ||
      !disease_) {
    return std::nullopt;
  }

  std::string venue_type_name = "";
  if (pending.venue_type_id < world_.venue_type_names.size()) {
    venue_type_name = world_.venue_type_names[pending.venue_type_id];
  }

  float severity_factor = 1.0f;
  auto* gu = world_.getGeoUnit(person->geo_unit_id);
  if (gu) severity_factor = gu->severity_factor;

  // venue_key consistent with the local infection path
  // (interaction_manager.cpp): for a virtual venue, key on the host's
  // person_id so the infection seed is the same regardless of which rank
  // creates it.
  uint64_t venue_key = static_cast<uint64_t>(pending.venue_id);
  if (isVirtualVenue(pending.venue_id)) {
    venue_key = static_cast<uint64_t>(virtualVenueHost(pending.venue_id));
  }
  uint64_t infection_seed =
      mix_seed(config_.simulation.random_seed, pending.person_id,
               static_cast<uint64_t>(pending.infection_time * 1000), venue_key);
  person->infection = std::make_unique<Infection>(
      disease_, pending.infection_time, person,
      static_cast<unsigned int>(infection_seed), &world_, venue_type_name,
      pending.venue_id, severity_factor, pending.infector_symptom_id, "", "",
      pending.transmission_mode_index);

  return pending;
}

// =============================================================================
// Cross-rank coordinated encounter exchange
// =============================================================================

void DomainCommunicator::exchangeEncounterProposals(
    const std::vector<EncounterProposal>& local_proposals,
    const DomainManager& dm,
    std::vector<EncounterProposal>& proposals_for_this_rank) {
  const size_t num_enc_types = world_.encounter_type_names.size();
  const int num_ranks_capture = num_ranks_;

  exchangeRoutedRecords<EncounterProposal>(
      local_proposals, proposals_for_this_rank, rank_, num_ranks_,
      PROPOSAL_WIRE_SIZE, "proposal",
      /*before_abort=*/110, /*total_neg_abort=*/103,
      /*pack_abort=*/111, /*unpack_abort=*/112, packProposal, unpackProposal,
      [num_ranks_capture, num_enc_types](const EncounterProposal& p) {
        return validateProposal(p, num_ranks_capture, num_enc_types);
      },
      dumpProposal,
      [&dm](const EncounterProposal& p) {
        return dm.getPersonRank(p.invitee_id);
      });
}

void DomainCommunicator::exchangeEncounterReplies(
    const std::vector<EncounterReply>& local_replies, const DomainManager& dm,
    std::vector<EncounterReply>& replies_for_this_rank) {
  const size_t num_enc_types = world_.encounter_type_names.size();

  exchangeRoutedRecords<EncounterReply>(
      local_replies, replies_for_this_rank, rank_, num_ranks_, REPLY_WIRE_SIZE,
      "reply",
      /*before_abort=*/120, /*total_neg_abort=*/104,
      /*pack_abort=*/121, /*unpack_abort=*/122, packReply, unpackReply,
      [num_enc_types](const EncounterReply& r) {
        return validateReply(r, num_enc_types);
      },
      dumpReply,
      [&dm](const EncounterReply& r) { return dm.getPersonRank(r.host_id); });
}

void DomainCommunicator::exchangeFinalizedEncounters(
    const std::vector<CoordinatedEncounter>& local_finalized,
    const DomainManager& dm,
    std::vector<CoordinatedEncounter>& finalized_for_this_rank) {
  // Allgatherv: each rank broadcasts its finalized encounters and the
  // others filter for encounters containing their local people. The wire
  // format is variable-length; see packFinalizedLocal for the layout.
  std::vector<char> local_buf = packFinalizedLocal(local_finalized);

  int local_size = static_cast<int>(local_buf.size());
  std::vector<int> all_sizes(num_ranks_);
  MPI_Allgather(&local_size, 1, MPI_INT, all_sizes.data(), 1, MPI_INT,
                MPI_COMM_WORLD);

  std::vector<int> displs(num_ranks_, 0);
  int total_size = 0;
  for (int r = 0; r < num_ranks_; ++r) {
    displs[r] = total_size;
    total_size += all_sizes[r];
  }

  std::vector<char> all_buf(total_size);
  MPI_Allgatherv(local_buf.data(), local_size, MPI_BYTE, all_buf.data(),
                 all_sizes.data(), displs.data(), MPI_BYTE, MPI_COMM_WORLD);

  finalized_for_this_rank.clear();
  for (int r = 0; r < num_ranks_; ++r) {
    if (r == rank_) continue;  // Skip our own (already have them)
    const char* ptr = all_buf.data() + displs[r];
    const char* end = ptr + all_sizes[r];
    unpackFinalizedFromRank(ptr, end, dm, rank_, finalized_for_this_rank);
  }
}

}  // namespace june

#endif  // USE_MPI
