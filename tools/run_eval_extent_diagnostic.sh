#!/usr/bin/env bash
# Bounded T2I diagnostic: theoretical contiguous-page opportunity vs realized extent I/O.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
BINARY=$ROOT/serving/search_beam
BINARY_SHA=$(sha256sum "$BINARY" | awk '{print $1}')
TAG=${FLASHANNS_CAMPAIGN_TAG:-${BINARY_SHA:0:4}-extdiag1}
[[ "$TAG" == "${BINARY_SHA:0:4}"-* ]] || {
  echo "tag $TAG does not identify binary $BINARY_SHA" >&2
  exit 2
}

BASE=$ROOT/results/eval/flashanns
RAW=$BASE/raw/t2i10m/$TAG/extent_diagnostic
PREFLIGHT=$BASE/preflight/$TAG/extent_diagnostic
OUT=$BASE/provisional/$TAG/extent-diagnostic
IDENTITY=$ROOT/results/motivation/flashanns-10m/preflight/c3-batching-t2i-full-identity-20260909b.json
SLOT_MAP=/mnt/disk0/chukexin_motivation/serving_t2i_10m/id_to_slot_10m_extent.bin

source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"
mkdir -p "$RAW" "$PREFLIGHT" "$OUT"

jq -e '.accepted == true and .dataset == "t2i10m" and .layout == "extent" and
  .identity_scope == "full" and .full_host_sha256 == .full_device_sha256' \
  "$IDENTITY" >/dev/null
[[ -f "$SLOT_MAP" && "$(stat -c %s "$SLOT_MAP")" == 40000000 ]]

for level in 400 800 1600; do
  for repeat_id in 0 1 2; do
    for system in batch-t1 extent-t1; do
      run_id="t2i10m-extent_diagnostic-L${level}-T1-r${repeat_id}-cold-${system}-${TAG}"
      run_dir=$RAW/$run_id
      evidence=$PREFLIGHT/${run_id}-volatile.json
      if [[ -f "$run_dir/run.json" ]] && jq -e --arg sha "$BINARY_SHA" \
        '.binary_sha256 == $sha and .validation.returncode == 0 and
         .metrics.completed_queries == 1000' "$run_dir/run.json" >/dev/null; then
        echo "SKIP complete $run_id"
        continue
      fi
      [[ ! -e "$run_dir" && ! -e "$evidence" ]]
      [[ "$(sha256sum "$BINARY" | awk '{print $1}')" == "$BINARY_SHA" ]]
      eval_reset_and_restore "$ROOT" t2i10m "$evidence" 4 extent
      python3 -m experiments.eval.flashanns.extent_diagnostic run-one \
        --root "$ROOT" --L "$level" --system "$system" --repeat "$repeat_id" \
        --tag "$TAG" --identity "$IDENTITY" --volatile "$evidence" \
        --out-root "$RAW"
      jq -e --arg sha "$BINARY_SHA" \
        '.binary_sha256 == $sha and .validation.returncode == 0 and
         .preflight_before.cache_used == 0 and .preflight_before.dirty_bytes == 0 and
         .metrics.completed_queries == 1000 and .metrics.score_flash == 0 and
         .metrics.score_bounce == 0' "$run_dir/run.json" >/dev/null
      echo "COMPLETE $run_id"
    done
  done
done

python3 -m experiments.eval.flashanns.extent_diagnostic aggregate \
  --raw-root "$RAW" --binary-sha256 "$BINARY_SHA" --slot-map "$SLOT_MAP" \
  --out-dir "$OUT"
jq -e '.status == "diagnostic_validated" and .run_count == 18' \
  "$OUT/diagnosis.json" >/dev/null
echo "DIAGNOSTIC $OUT/diagnosis.json"
