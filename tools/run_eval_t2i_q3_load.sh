#!/usr/bin/env bash
# Freeze measured saturation points, then execute the T2I open-loop load curve.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
RECALL_ANCHORS=$ROOT/results/eval/flashanns/calibration/t2i10m.json
LOAD_ANCHORS=$ROOT/results/eval/flashanns/calibration/t2i10m-load.json
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/q3-load-formal-20260905
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/q3_load
source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"

eval_wait_for_accepted_count flashanns-eval-q3t8-20260905.service \
  "$ROOT/results/eval/flashanns/accepted/t2i10m/q3_t8" 50
eval_wait_for_accepted_count flashanns-eval-q2pipeann-20260905.service \
  "$ROOT/results/eval/flashanns/accepted/t2i10m/q2" 85

if [[ ! -f "$LOAD_ANCHORS" ]]; then
  python3 -m experiments.eval.flashanns.freeze_load \
    "$ROOT/results/eval/flashanns/accepted/t2i10m/q3_t8" \
    "$ROOT/results/eval/flashanns/accepted/t2i10m/q2" \
    --recall-anchors "$RECALL_ANCHORS" --target-recall 0.90 --out "$LOAD_ANCHORS"
fi
jq -e '.accepted == true and (.arrival_rates | length) == 5' "$LOAD_ANCHORS" >/dev/null
[[ -x "$ROOT/tools/pipeann_open_loop" ]]

run_point() {
  local system_name=$1 level=$2 arrival_rate=$3 repeat_id=$4
  local rate_tag=${arrival_rate/./p}
  local run_id="t2i10m-q3_load-L${level}-r${repeat_id}-cold-${system_name}-T8-R${rate_tag}"
  local run_dir="$RAW/$run_id"
  local sealed="$ACCEPTED/$run_id/run.json"
  local evidence="$ROOT/results/eval/flashanns/preflight/t2i-q3load-${system_name}-R${rate_tag}-r${repeat_id}-volatile.json"
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  [[ ! -e "$run_dir" && ! -e "$sealed" ]]
  if [[ "$system_name" == pipeann ]]; then
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset t2i10m --phase q3_load --system "$system_name" --L "$level" \
      --threads 8 --arrival-rate "$arrival_rate" --repeat "$repeat_id" \
      --out "$RAW" --anchors "$LOAD_ANCHORS"
  else
    [[ ! -e "$evidence" ]]
    eval_reset_and_restore "$ROOT" t2i10m "$evidence" 4
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset t2i10m --phase q3_load --system "$system_name" --L "$level" \
      --threads 8 --arrival-rate "$arrival_rate" --repeat "$repeat_id" \
      --out "$RAW" --anchors "$LOAD_ANCHORS" \
      --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
  fi
  python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
  echo "ACCEPTED $run_id"
}

mapfile -t rates < <(jq -r '.arrival_rates[]' "$LOAD_ANCHORS")
pipe_l=$(jq -r '.primary.pipeann.L' "$LOAD_ANCHORS")
flash_l=$(jq -r '.primary.flashanns.L' "$LOAD_ANCHORS")
for arrival_rate in "${rates[@]}"; do
  for repeat_id in 0 1 2 3 4; do
    # Rotate the external baseline around the two internal systems; the order is
    # deterministic and recorded in run IDs while every point remains cold.
    if (( repeat_id % 2 == 0 )); then
      run_point pipeann "$pipe_l" "$arrival_rate" "$repeat_id"
      run_point wise-only "$flash_l" "$arrival_rate" "$repeat_id"
      run_point flashanns "$flash_l" "$arrival_rate" "$repeat_id"
    else
      run_point flashanns "$flash_l" "$arrival_rate" "$repeat_id"
      run_point wise-only "$flash_l" "$arrival_rate" "$repeat_id"
      run_point pipeann "$pipe_l" "$arrival_rate" "$repeat_id"
    fi
  done
done

echo "Q3_LOAD_COMPLETE"
