#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/q2-pipeann-formal-20260905
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/q2
source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"

eval_wait_for_accepted_count flashanns-eval-q2internal-20260905-v3.service \
  "$ACCEPTED" 55

for level in 50 100 200 400 800 1600; do
  for repeat_id in 0 1 2 3 4; do
    run_id="t2i10m-q2-L${level}-r${repeat_id}-cold-pipeann"
    run_dir="$RAW/$run_id"
    sealed="$ACCEPTED/$run_id/run.json"
    if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
      echo "SKIP accepted $run_id"
      continue
    fi
    [[ ! -e "$run_dir" && ! -e "$sealed" ]]
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset t2i10m --phase q2 --system pipeann --L "$level" \
      --repeat "$repeat_id" --out "$RAW"
    python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
    echo "ACCEPTED $run_id"
  done
done
echo "Q2_PIPEANN_COMPLETE"
