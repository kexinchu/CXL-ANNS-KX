#!/usr/bin/env bash
# Sequential 10M T=1 keep/drop sweep. Pollute 200 MiB @420 before each run.
# LAYOUT=extent (1100 GiB) or seq (800 GiB). Does not write protected offsets.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=$OUT/nav_10k.bin
GRAPH=$SRV/diskann_t2i_10m.graph.bin
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
LEN=20480004096
LAYOUT=${LAYOUT:-extent}

layout_args() {
  case "$LAYOUT" in
    extent)
      echo --vmem-offset 1181116006400 --id-slot-map "$SRV/id_to_slot_10m_extent.bin"
      ;;
    seq)
      echo --vmem-offset 858993459200
      ;;
    *) echo "LAYOUT=extent|seq" >&2; exit 2 ;;
  esac
}

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
  local log=$OUT/hide_10m_opt_${LAYOUT}_${tag}_nq100.log
  echo "==== opt $LAYOUT/$tag $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions)" | tee -a "$log"
  # shellcheck disable=SC2046
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout --nav-graph "$NAV" --graph-file "$GRAPH" \
    --vmem-dev /dev/vmem0 --vmem-len "$LEN" \
    $(layout_args) \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --dram-backend numa --dram-numa 1 \
    --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
    --cpu-affinity --policy P3 --budget $((64 << 20)) \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --max-q 100 --shuffle-seed 42 --pin-entry \
    --threads 1 --shared-window --pipe-w 16 --no-sync-hop \
    --no-score-cache --no-hide-warm-entry --no-direct-install \
    "$@" 2>&1 | tee -a "$log"
  echo "---- $LAYOUT/$tag ----"
  grep -E 'expand_batch|issue_ahead|score_page=|extent_run|stripe_fill|throughput_QPS|latency_ms mean|recall@10|nvme_read_B=|page_use=' "$log" | head -20
}

cmd=${1:-}
shift || true
case "$cmd" in
  baseline) run baseline --expand-batch 4 --issue-ahead 1 --no-score-page --no-stripe-fill --no-extent-run ;;
  opt_seq) LAYOUT=seq run seq_e4a1 --expand-batch 4 --issue-ahead 1 --no-score-page --no-stripe-fill --no-extent-run ;;
  opt_e4a2) run e4a2 --expand-batch 4 --issue-ahead 2 --no-score-page --no-stripe-fill --no-extent-run ;;
  opt_e8a2) run e8a2 --expand-batch 8 --issue-ahead 2 --no-score-page --no-stripe-fill --no-extent-run ;;
  opt_erun) run erun --expand-batch 4 --issue-ahead 1 --no-score-page --no-stripe-fill --extent-run ;;
  opt_spage) run spage --expand-batch 4 --issue-ahead 1 --score-page --no-stripe-fill --no-extent-run ;;
  opt_pipe) run pipe_e4 --expand-batch 4 --issue-ahead 1 --no-score-page --no-stripe-fill --extent-run --pipe-drive ;;
  opt_pipe8) run pipe_e8 --expand-batch 8 --issue-ahead 1 --no-score-page --no-stripe-fill --extent-run --pipe-drive ;;
  opt_pipe16) run pipe_e16 --expand-batch 16 --issue-ahead 1 --no-score-page --no-stripe-fill --extent-run --pipe-drive ;;
  final) run final_kept "$@" ;;
  *) echo "usage: LAYOUT=extent|seq $0 baseline|opt_seq|opt_e4a2|opt_e8a2|opt_erun|opt_spage|opt_pipe|final [flags]"; exit 2 ;;
esac
