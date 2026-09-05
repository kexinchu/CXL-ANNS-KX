#!/usr/bin/env bash
# Resume the approved T2I q3_t1 matrix in a detached host service.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
KO=/root/chukexin/mem2nvme/host/vmem_sw.ko
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/q3-t1-formal-20260905
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/q3_t1
ANCHORS=$ROOT/results/eval/flashanns/calibration/t2i10m.json

cd "$ROOT"

run_point() {
  local system_name=$1
  local repeat_id=$2
  local run_id="t2i10m-q3_t1-L400-r${repeat_id}-cold-${system_name}"
  local run_dir="$RAW/$run_id"
  local sealed="$ACCEPTED/$run_id/run.json"
  local evidence="$ROOT/results/eval/flashanns/preflight/t2i-q3t1-${system_name}-r${repeat_id}-volatile.json"

  if [[ -f "$sealed" ]] && jq -e '.validation.status == "accepted"' "$sealed" >/dev/null; then
    echo "SKIP accepted $run_id"
    return
  fi
  if [[ -e "$run_dir" || -e "$evidence" || -e "$sealed" ]]; then
    echo "REFUSE partial or conflicting point $run_id" >&2
    return 2
  fi
  if [[ -n "$(fuser /dev/vmem0 2>/dev/null || true)" ]]; then
    echo "REFUSE open /dev/vmem0 users before $run_id" >&2
    return 2
  fi
  if lsmod | awk '{print $1}' | grep -qx vmem_sw; then
    [[ "$(cat /sys/module/vmem_sw/srcversion)" == 76B6E44368BF753535CD293 ]]
    rmmod vmem_sw
  fi
  insmod "$KO" \
    nvme_devs=/dev/nvme1n1,/dev/nvme2n1 \
    target_bdfs=0000:d8:00.0,0000:d9:00.0 \
    expected_ssd_sizes_bytes=1920383410176,1920383410176 \
    ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
  python3 -m experiments.eval.flashanns.restore_volatile \
    --dataset t2i10m --write --out "$evidence"
  python3 -m experiments.eval.flashanns.run_matrix \
    --dataset t2i10m --phase q3_t1 --system "$system_name" --L 400 \
    --repeat "$repeat_id" --out "$RAW" --anchors "$ANCHORS" \
    --identity-evidence "$IDENTITY" --volatile-evidence "$evidence"
  python3 -m experiments.eval.flashanns.validate_run "$run_dir" \
    --seal-dir "$ACCEPTED"
  echo "ACCEPTED $run_id"
}

for point in serial-t1:3 serial-t1:4 \
             batch-t1:0 batch-t1:1 batch-t1:2 batch-t1:3 batch-t1:4 \
             extent-t1:0 extent-t1:1 extent-t1:2 extent-t1:3 extent-t1:4; do
  run_point "${point%%:*}" "${point##*:}"
done

echo "Q3_T1_REMAINING_COMPLETE"
