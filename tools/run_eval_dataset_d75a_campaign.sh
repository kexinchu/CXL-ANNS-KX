#!/usr/bin/env bash
# Complete one admitted non-T2I dataset under the immutable 505b runtime.
set -euo pipefail

if [[ $# -ne 1 || ( "$1" != yfcc10m && "$1" != laion10m ) ]]; then
  echo "usage: $0 yfcc10m|laion10m" >&2
  exit 2
fi

DATASET=$1
ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
TAG=505b
EXPECTED_BINARY=505b9ca4e514
BASE=$ROOT/results/eval/flashanns
RAW=$BASE/raw/$DATASET/$TAG
ACCEPTED=$BASE/accepted/$DATASET/$TAG
PREFLIGHT=$BASE/preflight/$TAG
IDENTITY=$BASE/preflight/${DATASET}-full-identity.json
RECALL_ANCHORS=$BASE/calibration/${DATASET}-${TAG}.json
LOAD_ANCHORS=$BASE/calibration/${DATASET}-load-${TAG}.json
PROOF=$BASE/readiness/${DATASET}-proof-${TAG}.json

source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"
mkdir -p "$RAW" "$ACCEPTED" "$PREFLIGHT" "$(dirname "$RECALL_ANCHORS")" "$(dirname "$PROOF")"

check_binary() {
  [[ "$(sha256sum serving/search_beam | awk '{print $1}')" == ${EXPECTED_BINARY}* ]]
}

anchor_l() {
  local system_name=$1
  [[ "$system_name" == serial-t1 || "$system_name" == batch-t1 || \
     "$system_name" == extent-t1 || "$system_name" == wise-only ]] && system_name=flashanns
  jq -r --arg system_name "$system_name" '.primary[$system_name].L' "$RECALL_ANCHORS"
}

run_internal_raw() {
  local phase=$1 system_name=$2 level=$3 repeat_id=$4 run_id=$5 anchor_file=${6:-}
  shift $(( $# >= 6 ? 6 : 5 ))
  local run_dir="$RAW/$phase/$run_id"
  local evidence="$PREFLIGHT/${run_id}-volatile.json"
  [[ ! -e "$run_dir" && ! -e "$evidence" ]]
  check_binary
  eval_reset_and_restore "$ROOT" "$DATASET" "$evidence" 4
  local command=(python3 -m experiments.eval.flashanns.run_matrix
    --dataset "$DATASET" --phase "$phase" --system "$system_name" --L "$level"
    --repeat "$repeat_id" --run-tag "$TAG" --out "$RAW/$phase"
    --identity-evidence "$IDENTITY" --volatile-evidence "$evidence")
  [[ -n "$anchor_file" ]] && command+=(--anchors "$anchor_file")
  command+=("$@")
  "${command[@]}"
  jq -e --arg prefix "$EXPECTED_BINARY" \
    '.binary_sha256 | startswith($prefix)' "$run_dir/run.json" >/dev/null
  jq -e '.metrics.score_bounce == 0 and .metrics.score_flash == 0 and
         .metrics.completed_queries == .nq' "$run_dir/run.json" >/dev/null
}

run_external_raw() {
  local phase=$1 level=$2 repeat_id=$3 run_id=$4 anchor_file=${5:-}
  shift $(( $# >= 5 ? 5 : 4 ))
  local run_dir="$RAW/$phase/$run_id"
  [[ ! -e "$run_dir" ]]
  local command=(python3 -m experiments.eval.flashanns.run_matrix
    --dataset "$DATASET" --phase "$phase" --system pipeann --L "$level"
    --repeat "$repeat_id" --run-tag "$TAG" --out "$RAW/$phase")
  [[ -n "$anchor_file" ]] && command+=(--anchors "$anchor_file")
  command+=("$@")
  "${command[@]}"
  jq -e '.metrics.completed_queries == .nq' "$run_dir/run.json" >/dev/null
}

seal_internal() {
  local phase=$1 system_name=$2 level=$3 repeat_id=$4 run_id=$5
  shift 5
  local sealed="$ACCEPTED/$phase/$run_id/run.json"
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  run_internal_raw "$phase" "$system_name" "$level" "$repeat_id" "$run_id" "$RECALL_ANCHORS" "$@"
  python3 -m experiments.eval.flashanns.validate_run "$RAW/$phase/$run_id" \
    --seal-dir "$ACCEPTED/$phase"
  echo "ACCEPTED $run_id"
}

seal_external() {
  local phase=$1 level=$2 repeat_id=$3 run_id=$4
  shift 4
  local sealed="$ACCEPTED/$phase/$run_id/run.json"
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  run_external_raw "$phase" "$level" "$repeat_id" "$run_id" "$RECALL_ANCHORS" "$@"
  python3 -m experiments.eval.flashanns.validate_run "$RAW/$phase/$run_id" \
    --seal-dir "$ACCEPTED/$phase"
  echo "ACCEPTED $run_id"
}

[[ -f "$IDENTITY" ]]
jq -e --arg dataset "$DATASET" '.accepted == true and .dataset == $dataset and
  .identity_scope == "full" and .full_host_sha256 == .full_device_sha256' "$IDENTITY" >/dev/null
python3 -m experiments.eval.flashanns.verify_dataset --dataset "$DATASET" --full
check_binary

# Candidate/result proof under separate cold restorations.
for system_name in demand flashanns; do
  run_id="${DATASET}-smoke-L400-r0-proof-${system_name}-${TAG}"
  run_internal_raw smoke "$system_name" 400 0 "$run_id"
done
python3 -m experiments.eval.flashanns.validate_run --compare-same-search \
  "$RAW/smoke/${DATASET}-smoke-L400-r0-proof-demand-${TAG}" \
  "$RAW/smoke/${DATASET}-smoke-L400-r0-proof-flashanns-${TAG}" --out "$PROOF"

# One cold point per (system,L); extend only a system that misses recall 0.90.
for level in 50 100 200 400 800 1600; do
  for system_name in demand flashanns; do
    run_id="${DATASET}-calibration-L${level}-r0-cold-${system_name}-${TAG}"
    run_internal_raw calibration "$system_name" "$level" 0 "$run_id"
  done
  pipe_id="${DATASET}-calibration-L${level}-r0-cold-pipeann-${TAG}"
  run_external_raw calibration "$level" 0 "$pipe_id"
done
for system_name in demand flashanns pipeann; do
  if ! jq -e -s --arg system_name "$system_name" \
    'any(.[]; .system == $system_name and .metrics["recall@10"] >= 0.90)' \
    "$RAW"/calibration/*/run.json >/dev/null; then
    for level in 2400 3200; do
      run_id="${DATASET}-calibration-L${level}-r0-cold-${system_name}-${TAG}"
      if [[ "$system_name" == pipeann ]]; then
        run_external_raw calibration "$level" 0 "$run_id"
      else
        run_internal_raw calibration "$system_name" "$level" 0 "$run_id"
      fi
    done
  fi
done
python3 -m experiments.eval.flashanns.validate_run --freeze-anchor 0.90 \
  "$RAW/calibration" --out "$RECALL_ANCHORS"
jq -e '.accepted == true and (.primary | keys | sort) == ["demand","flashanns","pipeann"]' \
  "$RECALL_ANCHORS" >/dev/null

# Q2 recall/latency and recall/throughput frontiers.
for level in 50 100 200 400 800 1600; do
  for repeat_id in 0 1 2 3 4; do
    demand_id="${DATASET}-q2-L${level}-r${repeat_id}-cold-demand-${TAG}"
    flash_id="${DATASET}-q2-L${level}-r${repeat_id}-cold-flashanns-${TAG}"
    seal_internal q2 demand "$level" "$repeat_id" "$demand_id"
    seal_internal q2 flashanns "$level" "$repeat_id" "$flash_id"
    python3 -m experiments.eval.flashanns.validate_run \
      "$RAW/q2/$demand_id" "$RAW/q2/$flash_id" --compare-same-search
    pipe_id="${DATASET}-q2-L${level}-r${repeat_id}-cold-pipeann-${TAG}"
    seal_external q2 "$level" "$repeat_id" "$pipe_id"
  done
done

# Wise-prefetch and continuous-scheduling causal controls.
wise_l=$(anchor_l flashanns)
for system_name in serial-t1 batch-t1 extent-t1; do
  for repeat_id in 0 1 2 3 4; do
    run_id="${DATASET}-q3_t1-L${wise_l}-r${repeat_id}-cold-${system_name}-${TAG}"
    seal_internal q3_t1 "$system_name" "$wise_l" "$repeat_id" "$run_id"
  done
done
for threads in 1 2 4 8 16; do
  for system_name in wise-only flashanns; do
    for repeat_id in 0 1 2 3 4; do
      run_id="${DATASET}-q3_t8-L${wise_l}-r${repeat_id}-cold-${system_name}-T${threads}-${TAG}"
      seal_internal q3_t8 "$system_name" "$wise_l" "$repeat_id" "$run_id" --threads "$threads"
    done
  done
done

# Hide proof and paired robustness. Cache sensitivity remains T2I-only.
for system_name in demand flashanns; do
  level=$(anchor_l "$system_name")
  for repeat_id in 0 1 2 3 4; do
    run_id="${DATASET}-q4_hide-L${level}-r${repeat_id}-cold-${system_name}-${TAG}"
    seal_internal q4_hide "$system_name" "$level" "$repeat_id" "$run_id"
  done
done
flash_l=$(anchor_l flashanns)
for repeat_id in 0 1 2 3 4; do
  cold_id="${DATASET}-q4_cold_warm-L${flash_l}-r${repeat_id}-cold-flashanns-${TAG}"
  seal_internal q4_cold_warm flashanns "$flash_l" "$repeat_id" "$cold_id" --state cold
  warm_id="${DATASET}-q4_cold_warm-L${flash_l}-r${repeat_id}-warm-flashanns-${TAG}"
  warm_dir="$RAW/q4_cold_warm/$warm_id"
  warm_sealed="$ACCEPTED/q4_cold_warm/$warm_id/run.json"
  if [[ ! -f "$warm_sealed" ]]; then
    warm_evidence="$RAW/q4_cold_warm/$cold_id/warm-evidence.json"
    [[ ! -e "$warm_dir" && -f "$warm_evidence" ]]
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset "$DATASET" --phase q4_cold_warm --system flashanns --L "$flash_l" \
      --repeat "$repeat_id" --state warm --run-tag "$TAG" --out "$RAW/q4_cold_warm" \
      --anchors "$RECALL_ANCHORS" --identity-evidence "$warm_evidence"
    jq -e --arg prefix "$EXPECTED_BINARY" \
      '.binary_sha256 | startswith($prefix)' "$warm_dir/run.json" >/dev/null
    jq -e '.metrics.score_bounce == 0 and .metrics.score_flash == 0 and
           .metrics.completed_queries == .nq' "$warm_dir/run.json" >/dev/null
    python3 -m experiments.eval.flashanns.validate_run "$warm_dir" \
      --seal-dir "$ACCEPTED/q4_cold_warm"
  fi
done

# Freeze saturation from this dataset/campaign only and execute open-loop load.
python3 -m experiments.eval.flashanns.freeze_load \
  "$ACCEPTED/q3_t8" "$ACCEPTED/q2" --recall-anchors "$RECALL_ANCHORS" \
  --target-recall 0.90 --out "$LOAD_ANCHORS"
jq -e '.accepted == true and (.arrival_rates | length) == 5' "$LOAD_ANCHORS" >/dev/null
mapfile -t rates < <(jq -r '.arrival_rates[]' "$LOAD_ANCHORS")
pipe_l=$(jq -r '.primary.pipeann.L' "$LOAD_ANCHORS")
flash_l=$(jq -r '.primary.flashanns.L' "$LOAD_ANCHORS")
for arrival_rate in "${rates[@]}"; do
  rate_tag=${arrival_rate/./p}
  for repeat_id in 0 1 2 3 4; do
    pipe_id="${DATASET}-q3_load-L${pipe_l}-r${repeat_id}-cold-pipeann-T8-R${rate_tag}-${TAG}"
    seal_external q3_load "$pipe_l" "$repeat_id" "$pipe_id" --threads 8 --arrival-rate "$arrival_rate"
    for system_name in wise-only flashanns; do
      run_id="${DATASET}-q3_load-L${flash_l}-r${repeat_id}-cold-${system_name}-T8-R${rate_tag}-${TAG}"
      seal_internal q3_load "$system_name" "$flash_l" "$repeat_id" "$run_id" \
        --threads 8 --arrival-rate "$arrival_rate"
    done
  done
done

echo "DATASET_505B_CAMPAIGN_COMPLETE dataset=$DATASET"
