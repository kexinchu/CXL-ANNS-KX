#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/q4-hide-formal-20260905
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/q4_hide
ANCHORS=$ROOT/results/eval/flashanns/calibration/t2i10m.json
source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"

eval_wait_for_accepted_count flashanns-eval-q3t8-20260905.service \
  "$ROOT/results/eval/flashanns/accepted/t2i10m/q3_t8" 50

for system_name in demand flashanns; do
  for repeat_id in 0 1 2 3 4; do
    run_id="t2i10m-q4_hide-L400-r${repeat_id}-cold-${system_name}"
    run_dir="$RAW/$run_id"
    sealed="$ACCEPTED/$run_id/run.json"
    evidence="$ROOT/results/eval/flashanns/preflight/t2i-q4hide-${system_name}-r${repeat_id}-volatile.json"
    if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
      echo "SKIP accepted $run_id"
      continue
    fi
    [[ ! -e "$run_dir" && ! -e "$evidence" && ! -e "$sealed" ]]
    eval_reset_and_restore "$ROOT" t2i10m "$evidence" 4
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset t2i10m --phase q4_hide --system "$system_name" --L 400 \
      --repeat "$repeat_id" --out "$RAW" --anchors "$ANCHORS" \
      --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
    python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
    echo "ACCEPTED $run_id"
  done
done
echo "Q4_HIDE_COMPLETE"
