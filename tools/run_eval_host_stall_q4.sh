#!/usr/bin/env bash
# Run only the paired q4 host-stall experiment under a binary-bound tag.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || ( "$1" != t2i10m && "$1" != yfcc10m && "$1" != laion10m ) ]]; then
  echo "usage: $0 t2i10m|yfcc10m|laion10m [--stage]" >&2
  exit 2
fi
DATASET=$1
STAGE=${2:-}
[[ -z "$STAGE" || "$STAGE" == --stage ]] || { echo "unknown option: $STAGE" >&2; exit 2; }

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
BINARY=$ROOT/serving/search_beam
BINARY_SHA=$(sha256sum "$BINARY" | awk '{print $1}')
TAG=${FLASHANNS_CAMPAIGN_TAG:-${BINARY_SHA:0:4}-hstall1}
[[ "$TAG" == "${BINARY_SHA:0:4}"-* ]] || {
  echo "tag $TAG does not identify binary $BINARY_SHA" >&2
  exit 2
}

BASE=$ROOT/results/eval/flashanns
RAW=$BASE/raw/$DATASET/$TAG/q4_hide
ACCEPTED=$BASE/accepted/$DATASET/$TAG/q4_hide
PREFLIGHT=$BASE/preflight/$TAG
READINESS=$BASE/readiness/$TAG
ANCHORS=$BASE/calibration/${DATASET}-4ad7-c3ef.json
IDENTITY=$PREFLIGHT/${DATASET}-extent-full-identity.json
source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"
mkdir -p "$RAW" "$ACCEPTED" "$PREFLIGHT" "$READINESS"

if [[ "$STAGE" == --stage ]]; then
  [[ -z "$(fuser /dev/vmem0 2>/dev/null || true)" ]]
  [[ ! -e "$IDENTITY" ]]
  python3 -m experiments.eval.flashanns.stage --dataset "$DATASET" --layout extent
  python3 -m experiments.eval.flashanns.preflight \
    --dataset "$DATASET" --layout extent --state post --full-identity --out "$IDENTITY"
elif [[ "$DATASET" == t2i10m && ! -e "$IDENTITY" ]]; then
  # The same staged image was fully verified by the accepted C3 campaign today.
  cp results/motivation/flashanns-10m/preflight/c3-batching-t2i-full-identity-20260909b.json \
    "$IDENTITY"
fi

[[ -f "$IDENTITY" && -f "$ANCHORS" ]]
jq -e --arg dataset "$DATASET" \
  '.accepted == true and .dataset == $dataset and .identity_scope == "full" and
   .layout == "extent" and .host_artifact == "extent_image" and
   .full_host_sha256 == .full_device_sha256' "$IDENTITY" >/dev/null

level_for() {
  jq -er --arg system "$1" '.primary[$system].L' "$ANCHORS"
}

for repeat_id in 0 1 2 3 4; do
  pair=()
  for system in demand flashanns; do
    level=$(level_for "$system")
    run_id="${DATASET}-q4_hide-L${level}-r${repeat_id}-cold-${system}-${TAG}"
    run_dir=$RAW/$run_id
    sealed=$ACCEPTED/$run_id/run.json
    evidence=$PREFLIGHT/${run_id}-volatile.json
    if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
      pair+=("$(dirname "$sealed")")
      continue
    fi
    [[ ! -e "$run_dir" && ! -e "$evidence" && ! -e "$sealed" ]]
    [[ "$(sha256sum "$BINARY" | awk '{print $1}')" == "$BINARY_SHA" ]]
    eval_reset_and_restore "$ROOT" "$DATASET" "$evidence" 4 extent
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset "$DATASET" --phase q4_hide --system "$system" --L "$level" \
      --repeat "$repeat_id" --run-tag "$TAG" --out "$RAW" --anchors "$ANCHORS" \
      --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
    python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
    jq -e --arg binary "$BINARY_SHA" \
      '.binary_sha256 == $binary and .validation.status == "accepted" and
       .metrics.host_data_stall_ns ==
         (.metrics.coverage_wait_ns + .metrics.slot_backpressure_wait_ns) and
       .metrics.score_triggered_flash_fills == 0 and
       .metrics.score_prematerialized_pct == 100' "$sealed" >/dev/null
    pair+=("$(dirname "$sealed")")
  done
  python3 -m experiments.eval.flashanns.validate_run --compare-same-search \
    "${pair[0]}" "${pair[1]}" --out "$READINESS/${DATASET}-r${repeat_id}-paired.json"
done

find "$ACCEPTED" -name run.json -type f -print0 | sort -z | xargs -0 jq -s \
  --arg dataset "$DATASET" --arg binary "$BINARY_SHA" --arg tag "$TAG" \
  '{dataset:$dataset, binary_sha256:$binary, campaign_tag:$tag,
    accepted_runs:map(.run_id), accepted_count:length,
    all_stall_sums_valid:all(.[]; .metrics.host_data_stall_ns ==
      (.metrics.coverage_wait_ns + .metrics.slot_backpressure_wait_ns)),
    score_contract_valid:all(.[]; .metrics.score_triggered_flash_fills == 0 and
      .metrics.score_prematerialized_pct == 100)}' > "$READINESS/${DATASET}-manifest.json"
jq -e '.accepted_count == 10 and .all_stall_sums_valid and .score_contract_valid' \
  "$READINESS/${DATASET}-manifest.json" >/dev/null
echo "HOST_STALL_Q4_COMPLETE dataset=$DATASET tag=$TAG binary=$BINARY_SHA"
