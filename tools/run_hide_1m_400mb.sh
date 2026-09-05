#!/usr/bin/env bash
# 1M DiskANN Hide: corpus on CXL-SSD (vmem 900 GiB), page cache capped at 400 MiB,
# P3 prefetcher + --cont-batch (shared window). Host keeps only nav + DramWindow.
# /dev/dax* is absent — scoring window is host numa (not Montage CXL.mem).
# Does not replace 50.25 / 85.8. Does not write 420/460 GiB.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=${NAV:-$OUT/nav_1m_10k.bin}
VMEM_OFF=${VMEM_OFF:-966367641600}
VMEM_LEN=${VMEM_LEN:-2048917504}
NQ=${NQ:-20}
HOST_BYTES=$((2 * 1024 * 1024 * 1024))

mkdir -p "$OUT"
g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I"$ROOT" \
  "$ROOT/serving/search_beam.cpp" -o "$BIN" -lnuma

lim=$(cat /sys/class/vmem/vmem0/cache_limit)
if [[ "$lim" -ne 419430400 ]]; then
  echo "FAIL: cache_limit=$lim want 419430400 (400 MiB). Reload vmem_sw cache_size_mib=400."
  exit 1
fi

run_one() {
  local T=$1
  local log=$2
  echo "==== Hide 1M cache400MiB P3+cont-batch T=$T nq=$NQ $(date -Is) ====" | tee "$log"
  {
    echo "cache_limit=$(cat /sys/class/vmem/vmem0/cache_limit)"
    echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used)"
    echo "evictions=$(cat /sys/class/vmem/vmem0/evictions)"
    echo "backing_read0=$(cat /sys/class/vmem/vmem0/backing0_read_ios) backing_read1=$(cat /sys/class/vmem/vmem0/backing1_read_ios)"
  } | tee -a "$log"
  local extra=(--threads "$T" --shared-window)
  if [[ "$T" -gt 1 ]]; then
    extra=(--cont-batch "$T")
  fi
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout \
    --nav-graph "$NAV" \
    --vmem-dev /dev/vmem0 --vmem-offset "$VMEM_OFF" --vmem-len "$VMEM_LEN" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" \
    --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --dram-backend numa --dram-numa 1 \
    --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
    --cpu-affinity \
    --policy P3 --pipe-w 16 --budget $((64 << 20)) \
    --no-score-page --expand-batch 8 --issue-ahead 2 \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --max-q "$NQ" --shuffle-seed 42 --pin-entry \
    "${extra[@]}" \
    2>&1 | tee -a "$log"
  {
    echo "cache_used_after=$(cat /sys/class/vmem/vmem0/cache_used)"
    echo "evictions_after=$(cat /sys/class/vmem/vmem0/evictions)"
  } | tee -a "$log"
}

# Discarded warmup so T=1..32 see a 400 MiB-capped cache, not a 8 KiB true-cold first run.
echo "==== warmup nq=$NQ (not a claim row) ===="
run_one 1 "$OUT/hide_1m_400mb_warmup.log"

THREADS=("$@")
if [[ ${#THREADS[@]} -eq 0 ]]; then
  THREADS=(1 4 8 16 32)
fi
for T in "${THREADS[@]}"; do
  run_one "$T" "$OUT/hide_1m_400mb_T${T}_nq${NQ}.log"
done
