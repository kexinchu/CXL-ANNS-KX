#!/usr/bin/env bash
# Same-protocol 10M matrix: Baseline (oneshot-FP) / FlashANNS (PQ-64 steal) / Oracle.
# T=1,8,16. nq=500 seed-42 extent@1100. Does not overwrite freeze claim logs.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=$OUT/nav_10k.bin
GRAPH=$SRV/diskann_t2i_10m.graph.bin
PQ_PIV=/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin
PQ_CMP=/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
WIN_MIB=128
LEN=20480004096
NQ=${NQ:-500}

pollute() {
  python3 - <<'PY'
import os, mmap
fd = os.open("/dev/vmem0", os.O_RDONLY)
m = mmap.mmap(fd, 200<<20, mmap.MAP_SHARED, mmap.PROT_READ, offset=450971566080)
acc = 0
for i in range(0, 200<<20, 4096):
    acc += m[i]
m.close(); os.close(fd)
print(f"pollute checksum={acc} evict={open('/sys/class/vmem/vmem0/evictions').read().strip()}", flush=True)
PY
}

hide_common() {
  echo --diskann-layout --nav-graph "$NAV" --graph-file "$GRAPH" \
    --vmem-dev /dev/vmem0 --vmem-len "$LEN" \
    --vmem-offset 1181116006400 --id-slot-map "$SRV/id_to_slot_10m_extent.bin" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --dram-backend numa --dram-numa 1 \
    --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
    --cpu-affinity --policy P3 --budget $((64 << 20)) \
    --beam 400 --k 10 --iters 0 \
    --max-q "$NQ" --shuffle-seed 42 --pin-entry \
    --shared-window --pipe-w 16 --no-sync-hop \
    --no-score-cache --no-hide-warm-entry --no-direct-install \
    --no-score-page --no-stripe-fill --extent-run --no-pipe-drive
}

summarize() {
  grep -E 'oneshot_fp=|pq-nav|steal_sched=|throughput_QPS|latency_ms mean|recall@10|from_win=|nvme_real_GBps=' "$1" | head -20
}

run_oracle() {
  local th=$1
  local log=$OUT/compare_10m_oracle_T${th}_nq${NQ}.log
  echo "==== COMPARE oracle T=$th nq=$NQ $(date -Is) ====" | tee "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout --oracle-dram --dram-backend dax \
    --image "$SRV/diskann_t2i_10m.bin" \
    --nav-graph "$NAV" --graph-file "$GRAPH" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --beam 400 --k 10 --iters 0 \
    --max-q "$NQ" --shuffle-seed 42 --cpu-affinity \
    --threads "$th" --shared-window --policy P0 \
    --pq-nav --pq-pivots "$PQ_PIV" --pq-compressed "$PQ_CMP" \
    2>&1 | tee -a "$log"
  echo "---- oracle T=$th ----"
  summarize "$log"
}

run_flashanns() {
  local th=$1
  fuser /dev/vmem0 2>/dev/null && echo "WARN vmem busy" >&2 || true
  pollute
  local log=$OUT/compare_10m_flashanns_T${th}_nq${NQ}.log
  echo "==== COMPARE FlashANNS T=$th nq=$NQ $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used)" | tee -a "$log"
  local extra=()
  if [[ "$th" == "1" ]]; then
    extra=(--threads 1 --expand-batch 8 --issue-ahead 1)
  else
    local wb=$((WIN_MIB * 1024 * 1024))
    extra=(--threads "$th" --per-thread-window --dram-bytes "$wb" --host-bytes "$wb"
           --dram-numa 0 --pipe-depth 2 --steal-sched --stagger-us 0 --issue-qd 0
           --expand-batch 8 --issue-ahead 1)
  fi
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    $(hide_common) "${extra[@]}" \
    --pq-nav --pq-pivots "$PQ_PIV" --pq-compressed "$PQ_CMP" \
    2>&1 | tee -a "$log"
  echo "---- FlashANNS T=$th ----"
  summarize "$log"
}

run_baseline() {
  local th=$1
  fuser /dev/vmem0 2>/dev/null && echo "WARN vmem busy" >&2 || true
  pollute
  local log=$OUT/compare_10m_baseline_oneshot_T${th}_nq${NQ}.log
  echo "==== COMPARE baseline oneshot-FP T=$th nq=$NQ $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used)" | tee -a "$log"
  local extra=()
  if [[ "$th" == "1" ]]; then
    extra=(--threads 1 --oneshot-fp --expand-batch 4 --issue-ahead 1)
  else
    local wb=$((WIN_MIB * 1024 * 1024))
    extra=(--threads "$th" --oneshot-fp --expand-batch 4 --issue-ahead 1
           --per-thread-window --dram-bytes "$wb" --host-bytes "$wb" --dram-numa 0
           --no-steal-sched)
  fi
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    $(hide_common) "${extra[@]}" \
    2>&1 | tee -a "$log"
  echo "---- baseline T=$th ----"
  summarize "$log"
}

cmd=${1:-all}
case "$cmd" in
  oracle) run_oracle "${2:-1}" ;;
  flashanns) run_flashanns "${2:-1}" ;;
  baseline) run_baseline "${2:-1}" ;;
  all)
    for t in 1 8 16; do run_oracle "$t"; done
    for t in 1 8 16; do run_flashanns "$t"; done
    for t in 1 8 16; do run_baseline "$t"; done
    ;;
  *) echo "usage: NQ=500 $0 all|oracle|flashanns|baseline [T]"; exit 2 ;;
esac
