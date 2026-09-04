#!/usr/bin/env bash
# Frozen: PQ-64 end-batch + steal CB + 2-deep pipeline on 10M.
# T=8 PTW: steal-sched, issue_qd=T, pipe_depth=2, 32-page C_L waves, no stagger.
# T=1 stays search_one_pq / from_win=100 (no steal).
# Does not replace 50.25 / 49.25 / 85.8 / T=1 110.94 / 116.29.
# See docs/notes/2026-09-04-hide-t8-steal-freeze.md
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=$OUT/nav_10k.bin
GRAPH=$SRV/diskann_t2i_10m.graph.bin
PQ_PIV=${PQ_PIV:-/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin}
PQ_CMP=${PQ_CMP:-/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin}
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
# C_L is ~1.4 MiB/q. 2 GiB × threads OOMs; 128 MiB is enough and removes the
# shared DramWindow lock that capped --threads 8 shared-window at ~447 QPS.
WIN_MIB=${WIN_MIB:-128}
LEN=20480004096
NQ=${NQ:-100}
T=${T:-8}
W=${W:-8}

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
    --expand-batch 8 --issue-ahead 1 --no-score-page --no-stripe-fill --extent-run \
    --no-pipe-drive
}

score_flags() {
  case "${SCORE:-}" in
    bounce) echo --score-bounce ;;
    vmem|cache) echo --score-vmem ;;
    ""|window) ;;
    *) echo "unknown SCORE=$SCORE (window|bounce|vmem)" >&2; exit 2 ;;
  esac
}

summarize() {
  grep -E 'pq-nav|hide_score=|steal_sched=|cont_batch_sched|pipe2_sched|throughput_QPS|latency_ms mean|recall@10|from_win=|from_bounce=|from_cache=|nvme_real_GBps|page_use=' "$1" | head -30
}

oracle() {
  local th=${1:-1}
  local log=$OUT/oracle_10m_pq64_T${th}_nq${NQ}.log
  echo "==== oracle 10M PQ-64 T=$th nq=$NQ $(date -Is) ====" | tee "$log"
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
    $(pq_flags) \
    2>&1 | tee -a "$log"
  echo "---- oracle T=$th ----"
  grep -E 'pq-nav|threads=|throughput_QPS|latency_ms mean|recall@10|nvme_read_B=' "$log" | head -20
}

hide_t1() {
  fuser /dev/vmem0 2>/dev/null && echo "WARN vmem busy" >&2 || true
  pollute
  local log=$OUT/hide_10m_pq64_T1_nq${NQ}.log
  echo "==== hide 10M PQ-64 T=1 nq=$NQ $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions)" | tee -a "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    $(hide_common) --threads 1 $(score_flags) \
    $(pq_flags) \
    2>&1 | tee -a "$log"
  echo "---- hide T=1 PQ-64 ----"
  summarize "$log"
}

hide_threads() {
  local th=$1
  fuser /dev/vmem0 2>/dev/null && echo "WARN vmem busy" >&2 || true
  pollute
  local ptw="${PTW:-0}"
  local extra=()
  local tag="thr${th}"
  if [[ "$ptw" == "1" ]]; then
    local wb=$((WIN_MIB * 1024 * 1024))
    extra=(--per-thread-window --dram-bytes "$wb" --host-bytes "$wb" --dram-numa 0
           --pipe-depth "${PIPE_DEPTH:-2}" --no-direct-install --steal-sched --stagger-us 0
           --issue-qd "${ISSUE_QD:-0}")
    if [[ -n "${ISSUE_QD:-}" && "${ISSUE_QD}" != "0" ]]; then
      tag="thr${th}_ptw${WIN_MIB}_d${PIPE_DEPTH:-2}_steal_qd${ISSUE_QD}"
    else
      tag="thr${th}_ptw${WIN_MIB}_d${PIPE_DEPTH:-2}_steal_qdT"
    fi
    if [[ "${SCORE:-}" == "bounce" ]]; then tag="${tag}_sb"; fi
    if [[ "${SCORE:-}" == "vmem" || "${SCORE:-}" == "cache" ]]; then tag="${tag}_sv"; fi
  else
    extra=(--shared-window)
  fi
  local log=$OUT/hide_10m_pq64_${tag}_nq${NQ}.log
  echo "==== hide 10M PQ-64 --threads $th ptw=$ptw win_mib=${WIN_MIB} nq=$NQ $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions)" | tee -a "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    $(hide_common) --threads "$th" "${extra[@]}" $(score_flags) \
    $(pq_flags) \
    2>&1 | tee -a "$log"
  echo "---- hide threads=$th ptw=$ptw ----"
  summarize "$log"
}

hide_cb() {
  local inflight=$1 workers=$2
  fuser /dev/vmem0 2>/dev/null && echo "WARN vmem busy" >&2 || true
  pollute
  local log=$OUT/hide_10m_pq64_I${inflight}_W${workers}_nq${NQ}.log
  echo "==== hide 10M PQ-64 I=$inflight W=$workers nq=$NQ $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions)" | tee -a "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    $(hide_common) --cont-batch "$inflight" --cont-workers "$workers" \
    $(pq_flags) \
    2>&1 | tee -a "$log"
  echo "---- hide I=$inflight W=$workers ----"
  summarize "$log"
}

cmd=${1:-t8}
case "$cmd" in
  oracle) oracle 1 ;;
  oracle8) oracle 8 ;;
  t1) hide_t1 ;;
  t8)
    # Frozen method: steal-sched + PTW 128 MiB + depth 2 + issue_qd=T.
    PTW=1 hide_threads "${W:-8}"
    ;;
  cb) hide_cb "$T" "$W" ;;
  thr) hide_threads "${W:-8}" ;;
  ptw) PTW=1 hide_threads "${W:-8}" ;;
  sweep|all)
    oracle 8
    hide_t1
    PTW=1 hide_threads 8
    ;;
  *) echo "usage: NQ=500 W=8 WIN_MIB=128 $0 oracle|oracle8|t1|t8|cb|thr|ptw|sweep|all"; exit 2 ;;
esac
