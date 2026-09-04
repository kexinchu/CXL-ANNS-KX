#!/usr/bin/env bash
# T=1 Hide occupancy + neighbor-page remap steps. Does not write 420/460/900 GiB.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=${NAV:-$OUT/nav_1m_10k.bin}
VMEM_SEQ=${VMEM_SEQ:-966367641600}
VMEM_REMAP=${VMEM_REMAP:-977105059840}
VMEM_LEN=${VMEM_LEN:-2048917504}
NQ=${NQ:-20}
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
MAP=$SRV/id_to_slot_1m.bin

mkdir -p "$OUT"
g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I"$ROOT" \
  "$ROOT/serving/search_beam.cpp" -o "$BIN" -lnuma

lim=$(cat /sys/class/vmem/vmem0/cache_limit)
if [[ "$lim" -ne 419430400 ]]; then
  echo "FAIL: cache_limit=$lim want 419430400"
  exit 1
fi

run_t1() {
  local tag=$1
  local off=$2
  shift 2
  local log=$OUT/hide_1m_t1_${tag}_nq${NQ}.log
  echo "==== T=1 $tag nq=$NQ $(date -Is) ====" | tee "$log"
  {
    echo "cache_limit=$(cat /sys/class/vmem/vmem0/cache_limit)"
    echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used)"
    echo "evictions=$(cat /sys/class/vmem/vmem0/evictions)"
  } | tee -a "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout \
    --nav-graph "$NAV" \
    --vmem-dev /dev/vmem0 --vmem-offset "$off" --vmem-len "$VMEM_LEN" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" \
    --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --dram-backend numa --dram-numa 1 \
    --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
    --cpu-affinity \
    --policy P3 --pipe-w 16 --budget $((64 << 20)) \
    --expand-batch 8 --issue-ahead 2 \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --max-q "$NQ" --shuffle-seed 42 --pin-entry \
    --threads 1 --shared-window \
    "$@" \
    2>&1 | tee -a "$log"
  {
    echo "cache_used_after=$(cat /sys/class/vmem/vmem0/cache_used)"
    echo "evictions_after=$(cat /sys/class/vmem/vmem0/evictions)"
  } | tee -a "$log"
  echo "---- extract $tag ----"
  grep -E 'throughput_QPS|latency_ms|page_occ|slot_use|from_win|spec_beam|id-slot-map|score_page' "$log" | tail -n 20
}

if [[ "${1:-}" == "baseline" || "${1:-}" == "all" || -z "${1:-}" ]]; then
  run_t1 s0_e8a2_seq "$VMEM_SEQ" --no-score-page
fi
if [[ "${1:-}" == "spec" || "${1:-}" == "all" ]]; then
  run_t1 s1_spec16_seq "$VMEM_SEQ" --no-score-page --spec-beam-nbrs 16
fi
if [[ "${1:-}" == "remap"* || "${1:-}" == "all" ]]; then
  run_t1 "${2:-s2_remap_e8a2}" "$VMEM_REMAP" --no-score-page --id-slot-map "$MAP" ${3:-}
fi
