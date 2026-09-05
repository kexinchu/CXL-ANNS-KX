#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/q4-cache-formal-20260905
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/q4_cache
ANCHORS=$ROOT/results/eval/flashanns/calibration/t2i10m.json
source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"

eval_wait_for_accepted_count flashanns-eval-q4coldwarm-20260905-v2.service \
  "$ROOT/results/eval/flashanns/accepted/t2i10m/q4_cold_warm" 10

for cache_gib in 1 2 4 8; do
  for repeat_id in 0 1 2 3 4; do
    run_id="t2i10m-q4_cache-L400-r${repeat_id}-cold-flashanns-C${cache_gib}G"
    run_dir="$RAW/$run_id"
    sealed="$ACCEPTED/$run_id/run.json"
    evidence="$ROOT/results/eval/flashanns/preflight/t2i-q4cache-C${cache_gib}G-r${repeat_id}-volatile.json"
    if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
      echo "SKIP accepted $run_id"
      continue
    fi
    [[ ! -e "$run_dir" && ! -e "$evidence" && ! -e "$sealed" ]]
    eval_reset_and_restore "$ROOT" t2i10m "$evidence" "$cache_gib"
    python3 -m experiments.eval.flashanns.run_matrix \
      --dataset t2i10m --phase q4_cache --system flashanns --L 400 \
      --cache-gib "$cache_gib" --repeat "$repeat_id" --out "$RAW" --anchors "$ANCHORS" \
      --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
    python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
    echo "ACCEPTED $run_id"
  done
done
echo "Q4_CACHE_COMPLETE"
