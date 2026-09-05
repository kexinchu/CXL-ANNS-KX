#!/usr/bin/env bash
# Ablation only: 1M oneshot-fp e4 a1 + --cont-batch 8.
# Frozen serving prefetcher is PQ-64 end-batch (tools/run_hide_10m_t8_pq.sh).
# Does not replace 50.25 / 85.8 / 110.94 / 116.29.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=$OUT/nav_1m_10k.bin
GRAPH=$SRV/diskann_t2i_1m.graph.bin
TEST=$OUT/qids_test500.bin
MAP=$SRV/id_to_slot_1m_extent.bin
OFF=1020054732800
LEN=2048917504
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
T=${T:-8}

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

oracle() {
  local log=$OUT/oracle_1m_host_T${T}_nq500.log
  echo "==== Oracle 1M host-file T=$T nq=500 $(date -Is) ====" | tee "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout --oracle-dram --dram-backend dax \
    --image "$SRV/diskann_t2i_1m.bin" \
    --nav-graph "$NAV" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --query-ids "$TEST" --cpu-affinity \
    --threads "$T" --shared-window --policy P0 \
    2>&1 | tee -a "$log"
  echo "---- oracle T=$T ----"
  grep -E 'threads=|throughput_QPS|latency_ms mean|recall@10|nvme_read_B=' "$log" | head -20
}

hide() {
  pollute
  local log=$OUT/hide_1m_t8_e4a1_cont_nq500.log
  echo "==== hide e4 a1 + cont-batch $T nq=500 $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions) limit=$(cat /sys/class/vmem/vmem0/cache_limit)" | tee -a "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout --nav-graph "$NAV" --graph-file "$GRAPH" \
    --vmem-dev /dev/vmem0 --vmem-offset "$OFF" --vmem-len "$LEN" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" --id-slot-map "$MAP" \
    --dram-backend numa --dram-numa 1 \
    --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
    --cpu-affinity --policy P3 --budget $((64 << 20)) \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --query-ids "$TEST" --pin-entry \
    --cont-batch "$T" --no-score-page \
    --pipe-w 16 --expand-batch 4 --issue-ahead 1 --no-sync-hop \
    --no-score-cache --no-hide-warm-entry --no-direct-install --no-stripe-fill \
    2>&1 | tee -a "$log"
  echo "---- hide T=$T cont-batch ----"
  grep -E 'cont_batch=|throughput_QPS|latency_ms mean|recall@10|from_win=|crit_wait|overlap=|nvme_real_GBps|page_use=' "$log" | head -20
}

cmd=${1:-all}
case "$cmd" in
  oracle) oracle ;;
  hide) hide ;;
  all) oracle; hide ;;
  *) echo "usage: $0 oracle|hide|all"; exit 2 ;;
esac
