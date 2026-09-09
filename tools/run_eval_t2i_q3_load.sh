#!/usr/bin/env bash
# Rerun only the T2I open-loop load curve under a new, binary-bound tag.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
EXPECTED_BINARY=${FLASHANNS_EXPECTED_BINARY_SHA256:?set FLASHANNS_EXPECTED_BINARY_SHA256}
TAG=${FLASHANNS_CAMPAIGN_TAG:?set FLASHANNS_CAMPAIGN_TAG}
LOAD_SOURCE_TAG=${FLASHANNS_LOAD_SOURCE_TAG:-4ad7-c3ef}
BINARY_TAG=${EXPECTED_BINARY:0:4}
[[ "$TAG" == "$BINARY_TAG" || "$TAG" == "$BINARY_TAG"-* ]] || {
  echo "campaign tag $TAG does not identify binary SHA-256 $EXPECTED_BINARY" >&2
  exit 2
}

BASE=$ROOT/results/eval/flashanns
IDENTITY=$BASE/preflight/t2i-full-identity.json
LOAD_ANCHORS=$BASE/calibration/t2i10m-load-$LOAD_SOURCE_TAG.json
RAW=$BASE/raw/t2i10m/$TAG/q3_load
ACCEPTED=$BASE/accepted/t2i10m/$TAG/q3_load
PREFLIGHT=$BASE/preflight/$TAG/q3_load

source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"
mkdir -p "$RAW" "$ACCEPTED" "$PREFLIGHT"

[[ "$(sha256sum serving/search_beam | awk '{print $1}')" == "$EXPECTED_BINARY" ]]
[[ -x "$ROOT/tools/pipeann_open_loop" ]]
jq -e --arg dataset t2i10m \
  '.accepted == true and .dataset == $dataset and (.arrival_rates | length) == 5' \
  "$LOAD_ANCHORS" >/dev/null

seal_internal() {
  local system_name=$1 level=$2 arrival_rate=$3 repeat_id=$4
  local rate_tag run_id run_dir sealed evidence
  rate_tag=$(eval_rate_tag "$arrival_rate")
  run_id="t2i10m-q3_load-L${level}-r${repeat_id}-cold-${system_name}-T8-R${rate_tag}-${TAG}"
  run_dir="$RAW/$run_id"
  sealed="$ACCEPTED/$run_id/run.json"
  evidence="$PREFLIGHT/${run_id}-volatile.json"
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  [[ "$(sha256sum serving/search_beam | awk '{print $1}')" == "$EXPECTED_BINARY" ]]
  [[ ! -e "$run_dir" && ! -e "$sealed" && ! -e "$evidence" ]]
  eval_reset_and_restore "$ROOT" t2i10m "$evidence" 4
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset t2i10m --phase q3_load --system "$system_name" --L "$level" \
    --threads 8 --arrival-rate "$arrival_rate" --repeat "$repeat_id" \
    --run-tag "$TAG" --out "$RAW" --anchors "$LOAD_ANCHORS" \
    --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
  jq -e --arg sha "$EXPECTED_BINARY" \
    '.binary_sha256 == $sha and .metrics.score_bounce == 0 and
     .metrics.score_flash == 0 and .metrics.completed_queries == .nq' \
    "$run_dir/run.json" >/dev/null
  python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
  echo "ACCEPTED $run_id"
}

seal_external() {
  local level=$1 arrival_rate=$2 repeat_id=$3
  local rate_tag run_id run_dir sealed
  rate_tag=$(eval_rate_tag "$arrival_rate")
  run_id="t2i10m-q3_load-L${level}-r${repeat_id}-cold-pipeann-T8-R${rate_tag}-${TAG}"
  run_dir="$RAW/$run_id"
  sealed="$ACCEPTED/$run_id/run.json"
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  eval_wait_for_commit_headroom
  [[ ! -e "$run_dir" && ! -e "$sealed" ]]
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset t2i10m --phase q3_load --system pipeann --L "$level" \
    --threads 8 --arrival-rate "$arrival_rate" --repeat "$repeat_id" \
    --run-tag "$TAG" --out "$RAW" --anchors "$LOAD_ANCHORS"
  python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
  echo "ACCEPTED $run_id"
}

mapfile -t rates < <(jq -r '.arrival_rates[]' "$LOAD_ANCHORS")
pipe_l=$(jq -r '.primary.pipeann.L' "$LOAD_ANCHORS")
flash_l=$(jq -r '.primary.flashanns.L' "$LOAD_ANCHORS")
for arrival_rate in "${rates[@]}"; do
  for repeat_id in 0 1 2 3 4; do
    if (( repeat_id % 2 == 0 )); then
      seal_external "$pipe_l" "$arrival_rate" "$repeat_id"
      seal_internal wise-only "$flash_l" "$arrival_rate" "$repeat_id"
      seal_internal flashanns "$flash_l" "$arrival_rate" "$repeat_id"
    else
      seal_internal flashanns "$flash_l" "$arrival_rate" "$repeat_id"
      seal_internal wise-only "$flash_l" "$arrival_rate" "$repeat_id"
      seal_external "$pipe_l" "$arrival_rate" "$repeat_id"
    fi
  done
done

echo "Q3_LOAD_COMPLETE tag=$TAG binary=$EXPECTED_BINARY load_source=$LOAD_SOURCE_TAG"
