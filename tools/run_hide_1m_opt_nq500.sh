#!/usr/bin/env bash
# Sequential hide-copy opts on test-500 / 950 extent / 100 MiB cache.
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

run() {
  local tag=$1; shift
  pollute
  local log=$OUT/hide_1m_opt_${tag}_nq500.log
  echo "==== opt $tag $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions)" | tee -a "$log"
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
    --threads 1 --shared-window --no-score-page \
    --pipe-w 16 --no-sync-hop --no-score-cache --no-hide-warm-entry \
    "$@" 2>&1 | tee -a "$log"
  echo "---- $tag ----"
  grep -E 'direct_install|stripe_fill|expand_batch|throughput_QPS|latency_ms mean|recall@10|from_cache=|nvme_read_B=' "$log" | head -20
}

cmd=${1:-}
shift || true
case "$cmd" in
  baseline) run baseline --expand-batch 1 --issue-ahead 1 --no-direct-install --no-stripe-fill ;;
  opt1) run opt1_direct --expand-batch 1 --issue-ahead 1 --direct-install --no-stripe-fill ;;
  opt2a) run opt2_e4a1 --expand-batch 4 --issue-ahead 1 --no-direct-install --no-stripe-fill ;;
  opt2b) run opt2_e4a2 --expand-batch 4 --issue-ahead 2 --no-direct-install --no-stripe-fill ;;
  opt2c) run opt2_e8a2 --expand-batch 8 --issue-ahead 2 --no-direct-install --no-stripe-fill ;;
  opt3) run opt3_stripe --expand-batch 1 --issue-ahead 1 --no-direct-install --stripe-fill ;;
  final) run final_kept "$@" ;;
  *) echo "usage: $0 baseline|opt1|opt2a|opt2b|opt2c|opt3|final [flags]"; exit 2 ;;
esac
