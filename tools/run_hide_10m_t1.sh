#!/usr/bin/env bash
# T=1 hide on the frozen prefetcher + full T2I-10M DiskANN at 800 GiB.
# Does not write 420/460/900/930/950. Does not replace 50.25 / 85.8.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=$OUT/nav_10k.bin
GRAPH=${GRAPH:-$SRV/diskann_t2i_10m.graph.bin}
OFF=858993459200
LEN=20480004096
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
NQ=${NQ:-20}

if [[ ! -s "$GRAPH" ]]; then
  echo "extract $GRAPH from diskann_t2i_10m.bin"
  python3 "$ROOT/tools/extract_diskann_graph.py" --src "$SRV/diskann_t2i_10m.bin" --out "$GRAPH"
fi

log=$OUT/hide_10m_t1_e4a1_nq${NQ}.log
echo "==== hide 10M T=1 e4 a1 nq=$NQ $(date -Is) ====" | tee "$log"
echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) limit=$(cat /sys/class/vmem/vmem0/cache_limit)" | tee -a "$log"

numactl --cpunodebind=0 --membind=0 "$BIN" \
  --diskann-layout --nav-graph "$NAV" --graph-file "$GRAPH" \
  --vmem-dev /dev/vmem0 --vmem-offset "$OFF" --vmem-len "$LEN" \
  --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
  --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
  --id-map "$SRV/new_to_old_pagebin.bin" \
  --dram-backend numa --dram-numa 1 \
  --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
  --cpu-affinity --policy P3 --budget $((64 << 20)) \
  --oneshot-fp --beam 400 --k 10 --iters 0 \
  --max-q "$NQ" --shuffle-seed 42 --pin-entry \
  --threads 1 --shared-window --no-score-page \
  --expand-batch 4 --issue-ahead 1 --pipe-w 16 --no-sync-hop \
  --no-score-cache --no-hide-warm-entry --no-direct-install --no-stripe-fill \
  2>&1 | tee -a "$log"

echo "---- summary ----"
grep -E 'expand_batch|throughput_QPS|latency_ms mean|recall@10|from_win=|crit_wait|overlap=|nvme_real_GBps|page_use=' "$log" | head -20
