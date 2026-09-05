#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/q2-internal-formal-20260905
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/q2
source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"

eval_wait_for_accepted_count flashanns-eval-q4cache-20260905.service \
  "$ROOT/results/eval/flashanns/accepted/t2i10m/q4_cache" 20

run_point() {
  local system_name=$1 level=$2 repeat_id=$3
  local run_id="t2i10m-q2-L${level}-r${repeat_id}-cold-${system_name}"
  local run_dir="$RAW/$run_id"
  local sealed="$ACCEPTED/$run_id/run.json"
  local evidence="$ROOT/results/eval/flashanns/preflight/t2i-q2-${system_name}-L${level}-r${repeat_id}-volatile.json"
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  [[ ! -e "$run_dir" && ! -e "$evidence" && ! -e "$sealed" ]]
  eval_reset_and_restore "$ROOT" t2i10m "$evidence" 4
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset t2i10m --phase q2 --system "$system_name" --L "$level" \
    --repeat "$repeat_id" --out "$RAW" \
    --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
  python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
  echo "ACCEPTED $run_id"
}

for level in 50 100 200 400 800; do
  for repeat_id in 0 1 2 3 4; do
    run_point demand "$level" "$repeat_id"
    run_point flashanns "$level" "$repeat_id"
    python3 -m experiments.eval.flashanns.validate_run \
      "$RAW/t2i10m-q2-L${level}-r${repeat_id}-cold-demand" \
      "$RAW/t2i10m-q2-L${level}-r${repeat_id}-cold-flashanns" \
      --compare-same-search
  done
done

# Demand's high-recall tail completes; FlashANNS L=1600 is separately rejected
# after two reproducible non-terminating calibration attempts.
for repeat_id in 0 1 2 3 4; do
  run_point demand 1600 "$repeat_id"
done
echo "Q2_INTERNAL_COMPLETE"
