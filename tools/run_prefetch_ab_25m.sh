#!/usr/bin/env bash
# Re-run prefetch A/B on Wikipedia-Cohere 25M @ CXL-SSD (/dev/vmem0).
# Mirrors results/prefetch_ab_10k.md matrix (DRAM 64MiB, beam=32, iters=48, k=10).
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
OUT=/mnt/disk0/chukexin_motivation/serving_25m
RES="$ROOT/results"
source "$ROOT/tools/cap_enforce.sh"
source "$OUT/serve_vmem.env"

DRAM=$((64 * 1024 * 1024))   # match prior 200k A/B
BEAM=32
ITERS=48
K=10
MAX_Q=${MAX_Q:-10000}       # query_10k_heldout.bin
# Anti-inflation defaults: shuffle order + cold window per query
SHUFFLE_SEED=${SHUFFLE_SEED:-42}
FLUSH_WINDOW=${FLUSH_WINDOW:-1}
GT="${CXAN_GT:-$OUT/gt_10k_diskann_k10.ibin}"
LOG="$RES/prefetch_ab_25m_vmem_raw.log"
CSV="$RES/prefetch_ab_25m_vmem.csv"

mkdir -p "$RES"
: > "$LOG"
echo "policy,budget,dram_bytes,nq,qps,mean_ms,p50,p90,p99,recall,dram_hits,ssd_misses,rerank" > "$CSV"

run_one() {
  local pol=$1 budget=$2 rerank=$3
  local extra=()
  [[ "$rerank" == "1" ]] && extra+=(--rerank)
  [[ "$FLUSH_WINDOW" == "1" ]] && extra+=(--flush-window)
  [[ "$SHUFFLE_SEED" -ge 0 ]] && extra+=(--shuffle-seed "$SHUFFLE_SEED")
  echo "=== $(date -Is) pol=$pol budget=$budget rerank=$rerank shuffle=$SHUFFLE_SEED flush=$FLUSH_WINDOW ===" | tee -a "$LOG"
  # Fresh process = fresh 64MiB DRAM window each cell
  set +e
  numactl --cpunodebind=0 --membind=0 "$ROOT/serving/search_beam" \
    --vmem-dev "$CXAN_VMEM_DEV" \
    --vmem-offset "$CXAN_SSD_OFFSET" \
    --vmem-len "$CXAN_LAYOUT_LEN" \
    --entry "$CXAN_ENTRY" \
    --queries "$CXAN_QUERIES" \
    --gt "$GT" \
    --dram-bytes "$DRAM" \
    --policy "$pol" \
    --budget "$budget" \
    --beam "$BEAM" --k "$K" --iters "$ITERS" \
    --max-q "$MAX_Q" \
    "${extra[@]}" \
    2>&1 | tee -a "$LOG" | tee /tmp/cxan_ab_last.txt
  local rc=${PIPESTATUS[0]}
  set -e
  local line
  line=$(grep '^CSV,' /tmp/cxan_ab_last.txt | tail -1 || true)
  if [[ -n "$line" ]]; then
    # CSV,pol,budget,dram,nq,qps,mean,p50,p90,p99,recall,hits,misses
    echo "${line#CSV,},$rerank" >> "$CSV"
  else
    echo "$pol,$budget,$DRAM,$MAX_Q,FAIL,,,,,,,,$rerank" >> "$CSV"
  fi
  return 0
}

sync
echo 3 > /proc/sys/vm/drop_caches || true

# --- with rerank (same cells as prefetch_ab_10k.md) ---
run_one P0 0 1
run_one P1 0 1
run_one P1 $((256*1024)) 1
run_one P1 $((1024*1024)) 1
run_one P1 $((4*1024*1024)) 1
run_one P3 $((256*1024)) 1

# --- without rerank ---
run_one P0 0 0
run_one P1 0 0
run_one P1 $((256*1024)) 0
run_one P1 $((1024*1024)) 0

echo "DONE $(date -Is)" | tee -a "$LOG"
