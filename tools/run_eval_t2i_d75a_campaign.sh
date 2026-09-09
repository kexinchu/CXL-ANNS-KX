#!/usr/bin/env bash
# Complete T2I Q2--Q4 under the immutable AVX2 runtime for the 32 GiB host.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
EXPECTED_BINARY=${FLASHANNS_EXPECTED_BINARY_SHA256:-4ad796de9cbdd89f03833c8b655bda9a33c15dcbe228d0dcd9e8ca9910fd221a}
TAG=${FLASHANNS_CAMPAIGN_TAG:-${EXPECTED_BINARY:0:4}}
BINARY_TAG=${EXPECTED_BINARY:0:4}
[[ "$EXPECTED_BINARY" == "$TAG"* || "$TAG" == "$BINARY_TAG"-* ]] || {
  echo "campaign tag $TAG does not identify binary SHA-256 $EXPECTED_BINARY" >&2
  exit 2
}
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
RECALL_ANCHORS=$ROOT/results/eval/flashanns/calibration/t2i10m.json
BASE=$ROOT/results/eval/flashanns
RAW=$BASE/raw/t2i10m/$TAG
ACCEPTED=$BASE/accepted/t2i10m/$TAG
PREFLIGHT=$BASE/preflight/$TAG
LOAD_ANCHORS=$BASE/calibration/t2i10m-load-$TAG.json

source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"
mkdir -p "$RAW" "$ACCEPTED" "$PREFLIGHT"

check_binary() {
  [[ "$(sha256sum serving/search_beam | awk '{print $1}')" == ${EXPECTED_BINARY}* ]]
}

seal_internal() {
  local phase=$1 system_name=$2 level=$3 repeat_id=$4 run_id=$5
  shift 5
  local phase_raw="$RAW/$phase" run_dir="$RAW/$phase/$run_id"
  local phase_accepted="$ACCEPTED/$phase" sealed="$ACCEPTED/$phase/$run_id/run.json"
  local evidence="$PREFLIGHT/${run_id}-volatile.json"
  local anchor_file=$RECALL_ANCHORS
  [[ "$phase" == q3_load ]] && anchor_file=$LOAD_ANCHORS
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  check_binary
  [[ ! -e "$run_dir" && ! -e "$evidence" && ! -e "$sealed" ]]
  eval_reset_and_restore "$ROOT" t2i10m "$evidence" "${CACHE_GIB:-4}"
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset t2i10m --phase "$phase" --system "$system_name" --L "$level" \
    --repeat "$repeat_id" --run-tag "$TAG" --out "$phase_raw" \
    --anchors "$anchor_file" --identity-evidence "$IDENTITY" \
    --volatile-evidence "$evidence" "$@"
  jq -e --arg prefix "$EXPECTED_BINARY" \
    '.binary_sha256 | startswith($prefix)' "$run_dir/run.json" >/dev/null
  jq -e '.metrics.score_bounce == 0 and .metrics.score_flash == 0 and
         .metrics.completed_queries == .nq' "$run_dir/run.json" >/dev/null
  python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$phase_accepted"
  echo "ACCEPTED $run_id"
}

seal_external() {
  local phase=$1 level=$2 repeat_id=$3 run_id=$4
  shift 4
  local phase_raw="$RAW/$phase" run_dir="$RAW/$phase/$run_id"
  local phase_accepted="$ACCEPTED/$phase" sealed="$ACCEPTED/$phase/$run_id/run.json"
  local anchor_file=$RECALL_ANCHORS
  [[ "$phase" == q3_load ]] && anchor_file=$LOAD_ANCHORS
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  eval_wait_for_commit_headroom
  [[ ! -e "$run_dir" && ! -e "$sealed" ]]
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset t2i10m --phase "$phase" --system pipeann --L "$level" \
    --repeat "$repeat_id" --run-tag "$TAG" --out "$phase_raw" \
    --anchors "$anchor_file" "$@"
  python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$phase_accepted"
  echo "ACCEPTED $run_id"
}

# Q3 mechanism isolation: first establish the T=1 prefetch progression.
for system_name in serial-t1 batch-t1 extent-t1; do
  for repeat_id in 0 1 2 3 4; do
    run_id="t2i10m-q3_t1-L400-r${repeat_id}-cold-${system_name}-${TAG}"
    seal_internal q3_t1 "$system_name" 400 "$repeat_id" "$run_id"
  done
done

# Q3 continuous-batching scale-out.
for threads in 1 2 4 8 16; do
  for system_name in wise-only flashanns; do
    for repeat_id in 0 1 2 3 4; do
      run_id="t2i10m-q3_t8-L400-r${repeat_id}-cold-${system_name}-T${threads}-${TAG}"
      seal_internal q3_t8 "$system_name" 400 "$repeat_id" "$run_id" --threads "$threads"
    done
  done
done

# Q4: hide-vs-demand, cold/warm robustness, and cache sensitivity.
for system_name in demand flashanns; do
  for repeat_id in 0 1 2 3 4; do
    run_id="t2i10m-q4_hide-L400-r${repeat_id}-cold-${system_name}-${TAG}"
    seal_internal q4_hide "$system_name" 400 "$repeat_id" "$run_id"
  done
done

for repeat_id in 0 1 2 3 4; do
  cold_id="t2i10m-q4_cold_warm-L400-r${repeat_id}-cold-flashanns-${TAG}"
  seal_internal q4_cold_warm flashanns 400 "$repeat_id" "$cold_id" --state cold
  warm_id="t2i10m-q4_cold_warm-L400-r${repeat_id}-warm-flashanns-${TAG}"
  warm_dir="$RAW/q4_cold_warm/$warm_id"
  warm_sealed="$ACCEPTED/q4_cold_warm/$warm_id/run.json"
  if [[ ! -f "$warm_sealed" ]]; then
    [[ ! -e "$warm_dir" ]]
    warm_evidence="$RAW/q4_cold_warm/$cold_id/warm-evidence.json"
    [[ -f "$warm_evidence" ]]
    eval_wait_for_commit_headroom
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset t2i10m --phase q4_cold_warm --system flashanns --L 400 \
      --repeat "$repeat_id" --state warm --run-tag "$TAG" \
      --out "$RAW/q4_cold_warm" --anchors "$RECALL_ANCHORS" \
      --identity-evidence "$warm_evidence"
    jq -e --arg prefix "$EXPECTED_BINARY" \
      '.binary_sha256 | startswith($prefix)' "$warm_dir/run.json" >/dev/null
    jq -e '.metrics.score_bounce == 0 and .metrics.score_flash == 0 and
           .metrics.completed_queries == .nq' "$warm_dir/run.json" >/dev/null
    python3 -m experiments.eval.flashanns.validate_run "$warm_dir" \
      --seal-dir "$ACCEPTED/q4_cold_warm"
  fi
done

for cache_gib in 1 2 4 8; do
  for repeat_id in 0 1 2 3 4; do
    run_id="t2i10m-q4_cache-L400-r${repeat_id}-cold-flashanns-C${cache_gib}G-${TAG}"
    CACHE_GIB=$cache_gib seal_internal q4_cache flashanns 400 "$repeat_id" "$run_id" \
      --cache-gib "$cache_gib"
  done
done

# Q2 recall frontiers. Demand and FlashANNS use the same graph/search contract.
for level in 50 100 200 400 800 1600; do
  for repeat_id in 0 1 2 3 4; do
    demand_id="t2i10m-q2-L${level}-r${repeat_id}-cold-demand-${TAG}"
    flash_id="t2i10m-q2-L${level}-r${repeat_id}-cold-flashanns-${TAG}"
    seal_internal q2 demand "$level" "$repeat_id" "$demand_id"
    seal_internal q2 flashanns "$level" "$repeat_id" "$flash_id"
    python3 -m experiments.eval.flashanns.validate_run \
      "$RAW/q2/$demand_id" "$RAW/q2/$flash_id" --compare-same-search
    pipe_id="t2i10m-q2-L${level}-r${repeat_id}-cold-pipeann-${TAG}"
    seal_external q2 "$level" "$repeat_id" "$pipe_id"
  done
done

# Extended Q2 points required by recall anchors. Native PipeANN first reaches
# recall@10 >= 0.90 at L=2400 on the frozen T2I artifacts.
for repeat_id in 0 1 2 3 4; do
  pipe_id="t2i10m-q2-L2400-r${repeat_id}-cold-pipeann-${TAG}"
  seal_external q2 2400 "$repeat_id" "$pipe_id"
done

# Freeze saturation only from this campaign, then run the open-loop curve.
python3 -m experiments.eval.flashanns.freeze_load \
  "$ACCEPTED/q3_t8" "$ACCEPTED/q2" --recall-anchors "$RECALL_ANCHORS" \
  --target-recall 0.90 --out "$LOAD_ANCHORS"
jq -e '.accepted == true and (.arrival_rates | length) == 5' "$LOAD_ANCHORS" >/dev/null
mapfile -t rates < <(jq -r '.arrival_rates[]' "$LOAD_ANCHORS")
pipe_l=$(jq -r '.primary.pipeann.L' "$LOAD_ANCHORS")
flash_l=$(jq -r '.primary.flashanns.L' "$LOAD_ANCHORS")
for arrival_rate in "${rates[@]}"; do
  rate_tag=$(eval_rate_tag "$arrival_rate")
  for repeat_id in 0 1 2 3 4; do
    pipe_id="t2i10m-q3_load-L${pipe_l}-r${repeat_id}-cold-pipeann-T8-R${rate_tag}-${TAG}"
    seal_external q3_load "$pipe_l" "$repeat_id" "$pipe_id" \
      --threads 8 --arrival-rate "$arrival_rate"
    for system_name in wise-only flashanns; do
      run_id="t2i10m-q3_load-L${flash_l}-r${repeat_id}-cold-${system_name}-T8-R${rate_tag}-${TAG}"
      seal_internal q3_load "$system_name" "$flash_l" "$repeat_id" "$run_id" \
        --threads 8 --arrival-rate "$arrival_rate"
    done
  done
done

echo "T2I_CAMPAIGN_COMPLETE tag=$TAG binary=$EXPECTED_BINARY"
