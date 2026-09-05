#!/usr/bin/env bash
# Frozen 10M prefetcher (2026-09-04 replace):
# PQ-64 beam + end-batch FP rerank. PIPE must stay 0.
# Does not write 420/460/800/900/930/950/1100.
# Does not replace 50.25 / 49.25 / 85.8.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=$OUT/nav_10k.bin
GRAPH=$SRV/diskann_t2i_10m.graph.bin
if [[ "${PQ_BYTES:-64}" == "64" ]]; then
  PQ_PIV=${PQ_PIV:-/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin}
  PQ_CMP=${PQ_CMP:-/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin}
else
  PQ_PIV=${PQ_PIV:-/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i_pq_pivots.bin}
  PQ_CMP=${PQ_CMP:-/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i_pq_compressed.bin}
fi
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
LEN=20480004096

pq_flags() {
  echo --pq-nav --pq-pivots "$PQ_PIV" --pq-compressed "$PQ_CMP"
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

# frozen PQ-64 end-batch (2026-09-04): PQ-64 beam + end-batch FP rerank. PIPE=0 is the
# locked hide row. PIPE=1 is the dropped ablation (extra empty pages).
NQ=${NQ:-100}
PIPE=${PIPE:-0}
cmd=${1:-}
shift || true
mkdir -p "$OUT"

hide_pqbeam_run() {
  local pipe_flag pipe_tag
  if [[ "$PIPE" == "1" ]]; then
    pipe_flag=--pipe-drive
    pipe_tag=e8pipe
  else
    pipe_flag=--no-pipe-drive
    pipe_tag=nopipe
  fi
  fuser /dev/vmem0 2>/dev/null && echo "WARN vmem busy" >&2 || true
  pollute
  local log=$OUT/hide_10m_pq${PQ_BYTES:-32}beam_${pipe_tag}_nq${NQ}.log
  echo "==== hide 10M PQ${PQ_BYTES:-32}-beam $pipe_tag nq=$NQ $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions)" | tee -a "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout --nav-graph "$NAV" --graph-file "$GRAPH" \
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
    --threads 1 --shared-window --pipe-w 16 --no-sync-hop \
    --no-score-cache --no-hide-warm-entry --no-direct-install \
    --expand-batch 8 --issue-ahead 1 --no-score-page --no-stripe-fill --extent-run \
    $pipe_flag \
    $(pq_flags) \
    2>&1 | tee -a "$log"
  grep -E 'pq-nav|pipe_drive|throughput_QPS|latency_ms mean|recall@10|nvme_read_B=|overlap=|from_win=' "$log" | head -25
}

case "$cmd" in
  oracle)
    log=$OUT/oracle_10m_pq${PQ_BYTES:-32}_T1_nq${NQ}.log
    echo "==== oracle 10M PQ-nav nq=$NQ $(date -Is) ====" | tee "$log"
    numactl --cpunodebind=0 --membind=0 "$BIN" \
      --diskann-layout --oracle-dram --dram-backend dax \
      --image "$SRV/diskann_t2i_10m.bin" \
      --nav-graph "$NAV" --graph-file "$GRAPH" \
      --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
      --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
      --id-map "$SRV/new_to_old_pagebin.bin" \
      --beam 400 --k 10 --iters 0 \
      --max-q "$NQ" --shuffle-seed 42 --cpu-affinity \
      --threads 1 --policy P0 \
      $(pq_flags) \
      2>&1 | tee -a "$log"
    grep -E 'pq-nav|throughput_QPS|latency_ms mean|recall@10|nvme_read_B=' "$log" | head -20
    ;;
  hide_pqbeam)
    hide_pqbeam_run
    ;;
  hide_e8)
    echo "obsolete: use hide_pqbeam" >&2
    exit 2
    ;;
  *)
    echo "usage: PQ_BYTES=32|64 NQ=20|100 PIPE=0|1 $0 oracle|hide_pqbeam"
    exit 2
    ;;
esac
