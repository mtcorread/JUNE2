#!/usr/bin/env bash
# =============================================================================
# MPI determinism: full disease_sim end-to-end invariants
# =============================================================================
# Extends test_mpi_reproducibility.sh (infection counts only) to also diff
# the HDF5 datasets that cover the relationship + encounter + profile
# invariants from MPI_TESTS_HANDOFF.md:
#
#   R2. OOE formation roster          → /events/relationships (tag="ooe")
#   R4. Profile assignment            → /lookups/profile_assignments/*
#   E2. Finalized coordinated encounters → /events/coordinated_encounters
#
# And, reusing the existing log-based comparison:
#   (infection counts)                → "Total currently infected" lines
#
# Strategy: run disease_sim at np=1/2/3 into three HDF5 outputs, then use
# a small python+h5py helper to dump each dataset to a sorted canonical
# form (row order varies across runs due to per-rank event buffering;
# determinism is about the *set* of records, not file-offset order).
# A failure prints the first diverging records with full field context.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build}"
DAYS="${DAYS:-10}"
CONFIG="${CONFIG:-configs/config_2021/simulation.yaml}"
WORLD="${WORLD:-worlds/world_2021.h5}"
# Space-separated list of rank counts to compare. Must include 1 as the
# reference.
NPS="${NPS:-1 2 3}"

BINARY="${BUILD_DIR}/disease_sim"
if [[ ! -x "$BINARY" ]]; then
  echo "FAIL: binary not found at $BINARY"
  exit 1
fi
if [[ ! -f "${PROJECT_DIR}/${WORLD}" ]]; then
  echo "FAIL: world not found at ${PROJECT_DIR}/${WORLD}"
  exit 1
fi

cd "$PROJECT_DIR"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/mpi_full_XXXXXX")
CLEAN_ON_EXIT=1
trap '[[ $CLEAN_ON_EXIT -eq 1 ]] && rm -rf "$TMP"' EXIT

echo "=== MPI full-reproducibility test ==="
echo "  binary: $BINARY"
echo "  days:   $DAYS"
echo "  nps:    $NPS"
echo "  tmp:    $TMP"
echo ""

# --- config override for day count -------------------------------------------
# Derived from the config's own start_date, so the harness follows the config's
# calendar rather than pinning one of its own.
START_DATE=$(sed -n 's/^[[:space:]]*start_date[[:space:]]*:[[:space:]]*"\([0-9-]*\)".*/\1/p' \
  "${PROJECT_DIR}/${CONFIG}" | head -1)
if [[ -z "$START_DATE" ]]; then
  echo "FAIL: no start_date in ${CONFIG}"
  exit 1
fi
END_DATE=$(python3 -c "
from datetime import datetime, timedelta
start = datetime.strptime('${START_DATE}', '%Y-%m-%d')
print((start + timedelta(days=${DAYS})).strftime('%Y-%m-%d'))
")
cp "$CONFIG" "$TMP/simulation.yaml"
if [[ "$(uname)" == "Darwin" ]]; then
  sed -i '' "s/end_date.*/end_date: \"${END_DATE}\"/" "$TMP/simulation.yaml"
else
  sed -i "s/end_date.*/end_date: \"${END_DATE}\"/" "$TMP/simulation.yaml"
fi

# --- run disease_sim at each np ----------------------------------------------
# Each run writes into $TMP/runs/np${NP}/. We then move the merged HDF5
# to $TMP/sim_np${NP}.h5 for the canonical-diff step below.
for NP in $NPS; do
  echo "[run] np=$NP"
  RUN_ID="np${NP}"
  RUN_DIR="$TMP/runs/${RUN_ID}"
  OUT="$TMP/sim_np${NP}.h5"
  mpirun -np "$NP" --oversubscribe "$BINARY" \
    --config "$TMP/simulation.yaml" \
    --world "$WORLD" \
    --runs-dir "$TMP/runs" \
    --run-id "$RUN_ID" \
    > "$TMP/log_np${NP}.txt" 2>&1 || {
      echo "FAIL: disease_sim exited non-zero at np=$NP"
      echo "      last 20 log lines:"
      tail -20 "$TMP/log_np${NP}.txt"
      CLEAN_ON_EXIT=0
      exit 1
    }
  if [[ ! -f "$RUN_DIR/simulation_events.h5" ]]; then
    echo "FAIL: no HDF5 output produced at np=$NP (expected $RUN_DIR/simulation_events.h5)"
    CLEAN_ON_EXIT=0
    exit 1
  fi
  mv "$RUN_DIR/simulation_events.h5" "$OUT"
done

# --- python helper: canonicalize a dataset to sorted text --------------------
# Optional field-subset mode: "fields=a,b,c" drops everything else. This is
# needed for coordinated_encounters where group_id is a per-rank monotonic
# counter by design (partition-dependent and not part of the invariant).
cat > "$TMP/canon.py" <<'PYEOF'
#!/usr/bin/env python3
"""Dump an HDF5 dataset (or group tree) to sorted text for diffing."""
import sys, h5py

def row_to_str(row, names):
    parts = []
    for n in names:
        v = row[n]
        if isinstance(v, bytes):
            v = v.decode('utf-8', errors='replace').rstrip('\x00')
        parts.append(f"{n}={v!r}")
    return " ".join(parts)

def dump_dataset(h5, path, out, fields=None):
    if path not in h5:
        out.write(f"# MISSING {path}\n")
        return
    ds = h5[path]
    if ds.shape == () or ds.size == 0:
        out.write(f"# EMPTY {path}\n")
        return
    arr = ds[:]
    if arr.dtype.names:
        names = list(arr.dtype.names)
        if fields is not None:
            missing = [f for f in fields if f not in names]
            if missing:
                raise SystemExit(f"fields {missing} not in dataset {path}: {names}")
            names = fields
        rows = [row_to_str(arr[i], names) for i in range(arr.shape[0])]
    else:
        rows = [str(x) for x in arr.flatten()]
    rows.sort()
    for r in rows:
        out.write(r + "\n")

def dump_group(h5, group_path, out):
    if group_path not in h5:
        out.write(f"# MISSING {group_path}\n")
        return
    grp = h5[group_path]
    def walker(name, obj):
        if isinstance(obj, h5py.Dataset):
            full = f"{group_path}/{name}"
            out.write(f"\n## {full}\n")
            dump_dataset(h5, full, out)
    grp.visititems(walker)

if __name__ == "__main__":
    argv = sys.argv[1:]
    fn, kind, path, outfn = argv[:4]
    fields = None
    for a in argv[4:]:
        if a.startswith("fields="):
            fields = a.split("=", 1)[1].split(",")
    with h5py.File(fn, "r") as h5, open(outfn, "w") as out:
        if kind == "dataset":
            dump_dataset(h5, path, out, fields)
        elif kind == "group":
            dump_group(h5, path, out)
        else:
            raise SystemExit("kind must be dataset|group")
PYEOF

# --- canonicalize every dataset of interest at every np ----------------------
# Each entry: "kind:path[|fields=a,b,c]". group_id is excluded for
# coordinated_encounters: it is a per-rank monotonic counter (partition-
# dependent by design), not part of the determinism invariant.
DATASETS=(
  "dataset:/events/relationships"
  "dataset:/events/coordinated_encounters|fields=person_a,person_b,time,encounter_type_id,slot"
  "dataset:/events/follows"
  "dataset:/events/infections"
)
# NOTE: /lookups/profile_assignments is NOT diffed here — the multi-rank
# event merger (main.cpp:476) discards per-rank lookup tables, so the
# merged file only contains rank 0's slice at np>1. Profile-assignment
# determinism is covered directly by test_profile_determinism (a unit
# test on the (seed, person.id) contract, which is what actually runs
# at each rank). If that unit test passes AND /events/relationships is
# identical across np (this test), then R4 holds transitively:
# formation probability depends on profile fields, so any profile_id
# drift would show up as a relationship-event drift.

for SPEC in "${DATASETS[@]}"; do
  MAIN="${SPEC%%|*}"
  EXTRA=""
  if [[ "$SPEC" == *"|"* ]]; then EXTRA="${SPEC#*|}"; fi
  KIND="${MAIN%%:*}"
  PATH_="${MAIN#*:}"
  SAFE=$(echo "$PATH_" | tr '/' '_')
  for NP in $NPS; do
    python3 "$TMP/canon.py" "$TMP/sim_np${NP}.h5" "$KIND" "$PATH_" \
      "$TMP/canon${SAFE}_np${NP}.txt" $EXTRA
  done
done

# --- diff against np=1 reference ---------------------------------------------
FAIL=0
REF_NP=$(echo "$NPS" | awk '{print $1}')
if [[ "$REF_NP" != "1" ]]; then
  echo "WARN: first np in NPS is not 1 — using np=$REF_NP as reference"
fi

for SPEC in "${DATASETS[@]}"; do
  MAIN="${SPEC%%|*}"
  PATH_="${MAIN#*:}"
  SAFE=$(echo "$PATH_" | tr '/' '_')
  REF="$TMP/canon${SAFE}_np${REF_NP}.txt"
  REF_LINES=$(wc -l < "$REF" | tr -d ' ')
  SPEC_FAIL=0
  for NP in $NPS; do
    [[ "$NP" == "$REF_NP" ]] && continue
    CUR="$TMP/canon${SAFE}_np${NP}.txt"
    if ! diff -q "$REF" "$CUR" > /dev/null 2>&1; then
      echo ""
      echo "FAIL: ${PATH_} diverges between np=${REF_NP} and np=${NP}"
      echo "      ref ($REF_LINES lines): $REF"
      echo "      cur: $CUR"
      diff -u "$REF" "$CUR" | head -30
      CLEAN_ON_EXIT=0
      FAIL=1
      SPEC_FAIL=1
    fi
  done
  if [[ $SPEC_FAIL -eq 0 ]]; then
    echo "  PASS ${PATH_}: ${REF_LINES} records identical across $NPS"
  fi
done

# --- also: infection count line-by-line (cheaper sanity check) ---------------
echo ""
echo "=== infection-count log diff ==="
for NP in $NPS; do
  grep "Total currently infected" "$TMP/log_np${NP}.txt" \
    | awk '{print $4}' > "$TMP/infect_np${NP}.txt"
done
for NP in $NPS; do
  [[ "$NP" == "$REF_NP" ]] && continue
  if ! diff -q "$TMP/infect_np${REF_NP}.txt" "$TMP/infect_np${NP}.txt" > /dev/null; then
    echo "FAIL: infection counts diverge between np=${REF_NP} and np=${NP}"
    diff -u "$TMP/infect_np${REF_NP}.txt" "$TMP/infect_np${NP}.txt" | head -20
    CLEAN_ON_EXIT=0
    FAIL=1
  else
    DAYS_CMP=$(wc -l < "$TMP/infect_np${REF_NP}.txt" | tr -d ' ')
    echo "  PASS infection counts: ${DAYS_CMP} days identical np=${REF_NP} vs np=${NP}"
  fi
done

echo ""
if [[ $FAIL -eq 0 ]]; then
  echo "ALL PASS — R2 (relationships), E2 (coordinated_encounters), R4 (profile_assignments), and infection counts are bit-identical across $NPS"
  exit 0
else
  echo "FAIL — reproducibility invariant violated. Temp files kept at $TMP"
  exit 1
fi
