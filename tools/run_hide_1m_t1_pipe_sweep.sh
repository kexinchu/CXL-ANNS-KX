#!/usr/bin/env bash
# T=1 pipe / spec-beam / lookahead sweep on cooccur+trace layout (930 GiB).
# Does not write 420/460/900. Not a claim row.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=$OUT/nav_1m_10k.bin
MAP=$SRV/id_to_slot_1m_cooccur.bin
OFF=998579896320
LEN=2048917504
NQ=${NQ:-20}
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
SUMMARY=$OUT/hide_1m_t1_pipe_sweep.tsv

g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I"$ROOT" \
  "$ROOT/serving/search_beam.cpp" -o "$BIN" -lnuma

run() {
  local tag=$1; shift
  local log=$OUT/hide_1m_t1_${tag}_nq${NQ}.log
  echo "==== T=1 $tag $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions)" | tee -a "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout --nav-graph "$NAV" \
    --vmem-dev /dev/vmem0 --vmem-offset "$OFF" --vmem-len "$LEN" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" --id-slot-map "$MAP" \
    --dram-backend numa --dram-numa 1 \
    --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
    --cpu-affinity --policy P3 --budget $((64 << 20)) \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --max-q "$NQ" --shuffle-seed 42 --pin-entry \
    --threads 1 --shared-window --no-score-page \
    "$@" 2>&1 | tee -a "$log"
  python3 - "$tag" "$log" "$SUMMARY" <<'PY'
import re, sys
tag, log, out = sys.argv[1], sys.argv[2], sys.argv[3]
t = open(log).read()
def g(pat, default=""):
    m = re.search(pat, t)
    return m.group(1) if m else default
qps = g(r"throughput_QPS=([0-9.]+)")
mean = g(r"latency_ms mean=([0-9.]+)")
occ = g(r"page_occ mean=([0-9.]+)")
issued = g(r"prefetch_use issued=([0-9]+)")
crit = g(r"crit_wait_ns=([0-9]+)")
nvme = g(r"nvme_read_B=([0-9]+)")
spec = g(r"spec_beam pages=([0-9]+)")
overlap = g(r"overlap=([0-9.]+)")
line = f"{tag}\t{qps}\t{mean}\t{occ}\t{issued}\t{crit}\t{nvme}\t{spec}\t{overlap}\n"
print("ROW", line, end="")
with open(out, "a") as f:
    f.write(line)
PY
}

echo -e "tag\tqps\tmean_ms\tpage_occ\tissued\tcrit_wait_ns\tnvme_B\tspec_pages\toverlap" > "$SUMMARY"
echo "==== warmup (discarded) ===="
run p0_warm --expand-batch 8 --issue-ahead 2 --pipe-w 16 --sync-hop

echo "==== A: sync-hop (old sequential hop_hide_score) ===="
run p1_sync_e1a1 --expand-batch 1 --issue-ahead 1 --pipe-w 16 --sync-hop
run p2_sync_e8a2 --expand-batch 8 --issue-ahead 2 --pipe-w 16 --sync-hop

echo "==== B: diskann IO pipe (issue batch then drain) ===="
run p3_pipe_e1a1 --expand-batch 1 --issue-ahead 1 --pipe-w 16 --no-sync-hop
run p4_pipe_e8a2 --expand-batch 8 --issue-ahead 2 --pipe-w 16 --no-sync-hop
run p5_pipe_e8a4 --expand-batch 8 --issue-ahead 4 --pipe-w 16 --no-sync-hop
run p6_pipe_e16a2 --expand-batch 16 --issue-ahead 2 --pipe-w 16 --no-sync-hop
run p7_pipe_e4a2 --expand-batch 4 --issue-ahead 2 --pipe-w 16 --no-sync-hop

echo "==== C: spec-beam (PipeANN-like: prefetch N(u) of top-M beam) ===="
run p8_spec8 --expand-batch 8 --issue-ahead 2 --pipe-w 16 --no-sync-hop --spec-beam-nbrs 8
run p9_spec16 --expand-batch 8 --issue-ahead 2 --pipe-w 16 --no-sync-hop --spec-beam-nbrs 16
run p10_spec32 --expand-batch 8 --issue-ahead 2 --pipe-w 16 --no-sync-hop --spec-beam-nbrs 32
run p11_spec64 --expand-batch 8 --issue-ahead 2 --pipe-w 16 --no-sync-hop --spec-beam-nbrs 64

echo "==== D: lookahead / wider pipe ===="
run p12_look8 --expand-batch 8 --issue-ahead 2 --pipe-w 16 --no-sync-hop --lookahead-k 8
run p13_spec16_look8 --expand-batch 8 --issue-ahead 2 --pipe-w 16 --no-sync-hop --spec-beam-nbrs 16 --lookahead-k 8
run p14_spec32_w32 --expand-batch 8 --issue-ahead 2 --pipe-w 32 --no-sync-hop --spec-beam-nbrs 32
run p15_spec32_e16a4 --expand-batch 16 --issue-ahead 4 --pipe-w 32 --no-sync-hop --spec-beam-nbrs 32

echo "==== sweep table ===="
column -t -s $'\t' "$SUMMARY"
