#!/usr/bin/env bash
# Re-run Q3 T=1 under one immutable post-open-loop binary hash.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
ANCHORS=$ROOT/results/eval/flashanns/calibration/t2i10m.json
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/q3-t1-b8725b-20260905
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/q3_t1_b8725b
TAG=b8725b
EXPECTED_BINARY=8725b11498fe
source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"

eval_wait_for_accepted_count flashanns-eval-q3load-20260905.service \
  "$ROOT/results/eval/flashanns/accepted/t2i10m/q3_load" 75

run_point() {
  local system_name=$1 repeat_id=$2
  local run_id="t2i10m-q3_t1-L400-r${repeat_id}-cold-${system_name}-${TAG}"
  local run_dir="$RAW/$run_id"
  local sealed="$ACCEPTED/$run_id/run.json"
  local evidence="$ROOT/results/eval/flashanns/preflight/t2i-q3t1-${TAG}-${system_name}-r${repeat_id}-volatile.json"
  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  [[ ! -e "$run_dir" && ! -e "$evidence" && ! -e "$sealed" ]]
  eval_reset_and_restore "$ROOT" t2i10m "$evidence" 4
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset t2i10m --phase q3_t1 --system "$system_name" --L 400 \
    --repeat "$repeat_id" --run-tag "$TAG" --out "$RAW" --anchors "$ANCHORS" \
    --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
  jq -e --arg prefix "$EXPECTED_BINARY" \
    '.binary_sha256 | startswith($prefix)' "$run_dir/run.json" >/dev/null
  python3 -m experiments.eval.flashanns.validate_run "$run_dir" --seal-dir "$ACCEPTED"
  echo "ACCEPTED $run_id"
}

for system_name in serial-t1 batch-t1 extent-t1; do
  for repeat_id in 0 1 2 3 4; do
    run_point "$system_name" "$repeat_id"
  done
done

echo "Q3_T1_CLEAN_COMPLETE"
