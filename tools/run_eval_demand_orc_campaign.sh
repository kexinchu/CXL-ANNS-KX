#!/usr/bin/env bash
# Measure one original-layout Demand frontier and restore the prior extent image.
set -euo pipefail

if [[ $# -ne 1 || ( "$1" != t2i10m && "$1" != yfcc10m && "$1" != laion10m ) ]]; then
  echo "usage: $0 t2i10m|yfcc10m|laion10m" >&2
  exit 2
fi

DATASET=$1
ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
RESTORE_DATASET=${FLASHANNS_RESTORE_DATASET:?set the currently staged extent dataset}
[[ "$RESTORE_DATASET" == t2i10m || "$RESTORE_DATASET" == yfcc10m || "$RESTORE_DATASET" == laion10m ]] || {
  echo "invalid FLASHANNS_RESTORE_DATASET=$RESTORE_DATASET" >&2
  exit 2
}

EXPECTED_BINARY=${FLASHANNS_EXPECTED_BINARY_SHA256:-$(sha256sum "$ROOT/serving/search_beam" | awk '{print $1}')}
TAG=${FLASHANNS_CAMPAIGN_TAG:-${EXPECTED_BINARY:0:4}-orc1}
REFERENCE_TAG=${FLASHANNS_REFERENCE_TAG:-4ad7-c3ef}
[[ "$TAG" == "${EXPECTED_BINARY:0:4}"-* ]] || {
  echo "campaign tag $TAG does not identify binary $EXPECTED_BINARY" >&2
  exit 2
}

EXPECTED_ACCEPTED=30
LAYOUT=original
PHASE=q2_original
SYSTEM=demand-orc
BASE=$ROOT/results/eval/flashanns
RAW_BASE=$BASE/raw/$DATASET/$TAG
RAW=$RAW_BASE/$PHASE
ACCEPTED=$BASE/accepted/$DATASET/$TAG/$PHASE
PREFLIGHT=$BASE/preflight/$TAG
IDENTITY=$PREFLIGHT/${DATASET}-${TAG}-original-full-identity.json
EXTENT_IDENTITY=$PREFLIGHT/${DATASET}-${TAG}-extent-full-identity.json
INITIAL_IDENTITY=$PREFLIGHT/initial-${RESTORE_DATASET}-extent.json
RESTORED_IDENTITY=$PREFLIGHT/restored-${RESTORE_DATASET}-extent.json
RESTORE_ARMED=0

source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"
mkdir -p "$RAW" "$ACCEPTED" "$PREFLIGHT" "$RAW_BASE/smoke" "$RAW_BASE/smoke_original"

check_binary() {
  [[ "$(sha256sum serving/search_beam | awk '{print $1}')" == "$EXPECTED_BINARY" ]]
}

restore_extent() {
  local status=$?
  trap - EXIT
  if (( RESTORE_ARMED == 0 )); then
    exit "$status"
  fi
  set +e
  echo "RESTORE_BEGIN dataset=$RESTORE_DATASET layout=extent" >&2
  python3 -m experiments.eval.flashanns.stage \
    --dataset "$RESTORE_DATASET" --layout extent --device /dev/vmem0
  local restore_status=$?
  if (( restore_status == 0 )); then
    python3 -m experiments.eval.flashanns.preflight \
      --dataset "$RESTORE_DATASET" --layout extent --state post \
      --full-identity --out "$RESTORED_IDENTITY"
    restore_status=$?
  fi
  if (( restore_status != 0 )); then
    echo "RESTORE_FAILED dataset=$RESTORE_DATASET status=$restore_status" >&2
    (( status == 0 )) && status=$restore_status
  else
    echo "RESTORE_ACCEPTED dataset=$RESTORE_DATASET evidence=$RESTORED_IDENTITY" >&2
  fi
  exit "$status"
}

trap restore_extent EXIT

# The restore target must be the exact extent image currently in the aperture.
python3 -m experiments.eval.flashanns.preflight \
  --dataset "$RESTORE_DATASET" --layout extent --state post \
  --full-identity --out "$INITIAL_IDENTITY"
RESTORE_ARMED=1

check_binary
python3 -m experiments.eval.flashanns.verify_dataset --dataset "$DATASET" --full

# Build a same-binary, same-query extent reference before replacing the image.
python3 -m experiments.eval.flashanns.stage \
  --dataset "$DATASET" --layout extent --device /dev/vmem0
python3 -m experiments.eval.flashanns.preflight \
  --dataset "$DATASET" --layout extent --state post \
  --full-identity --out "$EXTENT_IDENTITY"

PAIR_ID="${DATASET}-smoke-L400-r0-proof-demand-${TAG}"
PAIR_DIR="$RAW_BASE/smoke/$PAIR_ID"
if [[ ! -f "$PAIR_DIR/run.json" ]]; then
  PAIR_EVIDENCE="$PREFLIGHT/${PAIR_ID}-volatile.json"
  [[ ! -e "$PAIR_DIR" && ! -e "$PAIR_EVIDENCE" ]]
  check_binary
  eval_reset_and_restore "$ROOT" "$DATASET" "$PAIR_EVIDENCE" 4 extent
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset "$DATASET" --phase smoke --system demand \
    --L 400 --repeat 0 --run-tag "$TAG" \
    --out "$RAW_BASE/smoke" --identity-evidence "$EXTENT_IDENTITY" \
    --volatile-evidence "$PAIR_EVIDENCE"
fi
jq -e --arg binary "$EXPECTED_BINARY" '
  .binary_sha256 == $binary and
  .system == "demand" and
  .layout == "extent" and
  .staged_artifact == "extent_image" and
  .nq == 100 and
  .metrics.completed_queries == .nq
' "$PAIR_DIR/run.json" >/dev/null
echo "PAIR_SMOKE_ACCEPTED dataset=$DATASET run=$PAIR_ID"

echo "STAGE_ORIGINAL_BEGIN dataset=$DATASET"
python3 -m experiments.eval.flashanns.stage \
  --dataset "$DATASET" --layout original --device /dev/vmem0
python3 -m experiments.eval.flashanns.preflight \
  --dataset "$DATASET" --layout original --state post \
  --full-identity --out "$IDENTITY"

run_original_raw() {
  local phase=$1 level=$2 repeat_id=$3 run_id=$4
  local run_dir="$RAW_BASE/$phase/$run_id"
  local evidence="$PREFLIGHT/${run_id}-volatile.json"
  [[ ! -e "$run_dir" && ! -e "$evidence" ]]
  check_binary
  eval_reset_and_restore "$ROOT" "$DATASET" "$evidence" 4 original
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset "$DATASET" --phase "$phase" --system demand-orc \
    --L "$level" --repeat "$repeat_id" --run-tag "$TAG" \
    --out "$RAW_BASE/$phase" --identity-evidence "$IDENTITY" \
    --volatile-evidence "$evidence"
  jq -e --arg binary "$EXPECTED_BINARY" '
    .binary_sha256 == $binary and
    .system == "demand-orc" and
    .layout == "original" and
    .staged_artifact == "oracle_image" and
    (.command | index("--id-slot-map") | not) and
    .metrics.completed_queries == .nq
  ' "$run_dir/run.json" >/dev/null
}

reference_record() {
  local level=$1 repeat_id=$2
  local path="$BASE/accepted/$DATASET/$REFERENCE_TAG/q2/${DATASET}-q2-L${level}-r${repeat_id}-cold-demand-${REFERENCE_TAG}/run.json"
  [[ -f "$path" ]] || {
    echo "missing same-search reference: $path" >&2
    return 2
  }
  printf '%s\n' "$path"
}

# One non-paper smoke run proves that original logical IDs produce the same search.
SMOKE_ID="${DATASET}-smoke_original-L400-r0-proof-demand-orc-${TAG}"
SMOKE_DIR="$RAW_BASE/smoke_original/$SMOKE_ID"
if [[ ! -f "$SMOKE_DIR/run.json" ]]; then
  run_original_raw smoke_original 400 0 "$SMOKE_ID"
fi
python3 -m experiments.eval.flashanns.validate_run --compare-same-search \
  "$PAIR_DIR/run.json" "$SMOKE_DIR/run.json"

for level in 50 100 200 400 800 1600; do
  for repeat_id in 0 1 2 3 4; do
    run_id="${DATASET}-${PHASE}-L${level}-r${repeat_id}-cold-${SYSTEM}-${TAG}"
    sealed="$ACCEPTED/$run_id/run.json"
    if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
      echo "SKIP accepted $run_id"
      continue
    fi
    run_original_raw "$PHASE" "$level" "$repeat_id" "$run_id"
    python3 -m experiments.eval.flashanns.validate_run --compare-same-search \
      "$(reference_record "$level" "$repeat_id")" "$RAW/$run_id/run.json"
    python3 -m experiments.eval.flashanns.validate_run "$RAW/$run_id" \
      --seal-dir "$ACCEPTED"
    echo "ACCEPTED $run_id"
  done
done

observed=$(find "$ACCEPTED" -name run.json -type f | wc -l)
[[ "$observed" -eq "$EXPECTED_ACCEPTED" ]] || {
  echo "accepted count $observed differs from $EXPECTED_ACCEPTED" >&2
  exit 2
}
echo "DEMAND_ORC_COMPLETE dataset=$DATASET tag=$TAG accepted=$observed binary=$EXPECTED_BINARY"
