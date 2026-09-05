#!/usr/bin/env bash
# Run the T2I continuous-batching concurrency study after q3_t1 completes.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/q3-t8-formal-20260905
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/q3_t8
ANCHORS=$ROOT/results/eval/flashanns/calibration/t2i10m.json
source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"

eval_wait_for_accepted_count \
  flashanns-eval-q3t1-20260905.service \
  "$ROOT/results/eval/flashanns/accepted/t2i10m/q3_t1" 15

run_point() {
  local system_name=$1 threads=$2 repeat_id=$3
  local run_id="t2i10m-q3_t8-L400-r${repeat_id}-cold-${system_name}-T${threads}"
  local run_dir="$RAW/$run_id"
  local sealed="$ACCEPTED/$run_id/run.json"
  local evidence="$ROOT/results/eval/flashanns/preflight/t2i-q3t8-${system_name}-T${threads}-r${repeat_id}-volatile.json"
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  if [[ -e "$run_dir" || -e "$evidence" || -e "$sealed" ]]; then
    echo "REFUSE partial or conflicting point $run_id" >&2
    return 2
  fi
  eval_reset_and_restore "$ROOT" t2i10m "$evidence" 4
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset t2i10m --phase q3_t8 --system "$system_name" --L 400 \
    --threads "$threads" --repeat "$repeat_id" --out "$RAW" --anchors "$ANCHORS" \
    --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
  python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
  echo "ACCEPTED $run_id"
}

for threads in 1 2 4 8 16; do
  for system_name in wise-only flashanns; do
    for repeat_id in 0 1 2 3 4; do
      run_point "$system_name" "$threads" "$repeat_id"
    done
  done
done

echo "Q3_T8_COMPLETE"
