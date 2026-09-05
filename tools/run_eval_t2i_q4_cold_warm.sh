#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/q4-cold-warm-formal-20260905
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/q4_cold_warm
ANCHORS=$ROOT/results/eval/flashanns/calibration/t2i10m.json
source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"

eval_wait_for_accepted_count flashanns-eval-q4hide-20260905.service \
  "$ROOT/results/eval/flashanns/accepted/t2i10m/q4_hide" 10

for repeat_id in 0 1 2 3 4; do
  cold_id="t2i10m-q4_cold_warm-L400-r${repeat_id}-cold-flashanns"
  warm_id="t2i10m-q4_cold_warm-L400-r${repeat_id}-warm-flashanns"
  cold_dir="$RAW/$cold_id"
  warm_dir="$RAW/$warm_id"
  cold_sealed="$ACCEPTED/$cold_id/run.json"
  warm_sealed="$ACCEPTED/$warm_id/run.json"
  evidence="$ROOT/results/eval/flashanns/preflight/t2i-q4coldwarm-r${repeat_id}-volatile.json"
  if [[ ! -f "$cold_sealed" ]]; then
    [[ ! -e "$cold_dir" && ! -e "$evidence" ]]
    eval_reset_and_restore "$ROOT" t2i10m "$evidence" 4
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset t2i10m --phase q4_cold_warm --system flashanns --L 400 \
      --repeat "$repeat_id" --state cold --out "$RAW" --anchors "$ANCHORS" \
      --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
    python3 -m experiments.eval.flashanns.validate_run "$cold_dir" --seal-dir "$ACCEPTED"
  fi
  warm_evidence="$cold_dir/warm-evidence.json"
  [[ -f "$warm_evidence" ]]
  if [[ ! -f "$warm_sealed" ]]; then
    [[ ! -e "$warm_dir" ]]
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset t2i10m --phase q4_cold_warm --system flashanns --L 400 \
      --repeat "$repeat_id" --state warm --out "$RAW" --anchors "$ANCHORS" \
      --identity-evidence "$warm_evidence"
    python3 -m experiments.eval.flashanns.validate_run "$warm_dir" --seal-dir "$ACCEPTED"
  fi
  echo "ACCEPTED pair repeat=$repeat_id"
done
echo "Q4_COLD_WARM_COMPLETE"
