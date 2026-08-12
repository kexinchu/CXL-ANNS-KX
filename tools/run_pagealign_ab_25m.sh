#!/usr/bin/env bash
# A+B vs packed baseline: pagealign layout + id-neighborhood soft-pin (B in search_beam).
# Recipe: P3 pipe-w=4 install-top=4 fetch-top=0, L=300, nq=100, oneshot-fp, pin-entry, no flush.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
OUT=/mnt/disk0/chukexin_motivation/serving_25m
RES="$ROOT/results"
source "$ROOT/tools/cap_enforce.sh"

BIN="$ROOT/serving/search_beam"
DRAM=$((1 * 1024 * 1024 * 1024))
HOST_CAP=$((32 * 1024 * 1024))
BUDGET=$((64 << 20))
L=300
MAX_Q=${MAX_Q:-100}
SEED=${SHUFFLE_SEED:-42}
CSV="$RES/oneshot_fp_pagealign_ab.csv"
LOG="$RES/oneshot_fp_pagealign_ab.log"
mkdir -p "$RES"
echo "tag,policy,budget,dram,nq,beam,iters,qps,mean_ms,p50,p90,p99,recall,hit_pct,dram_hits,ssd_misses" > "$CSV"
: > "$LOG"

run_one() {
  local tag=$1 envf=$2
  # shellcheck disable=SC1090
  source "$envf"
  echo "=== $(date -Is) tag=$tag offset=$CXAN_SSD_OFFSET len=$CXAN_LAYOUT_LEN ===" | tee -a "$LOG"
  set +e
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --vmem-dev "$CXAN_VMEM_DEV" \
    --vmem-offset "$CXAN_SSD_OFFSET" \
    --vmem-len "$CXAN_LAYOUT_LEN" \
    --entry "$CXAN_ENTRY" \
    --queries "$CXAN_QUERIES" \
    --gt "$CXAN_GT" \
    --dram-bytes "$DRAM" \
    --host-cap "$HOST_CAP" \
    --dram-backend dax \
    --dax-dev "${CXAN_DAX_DEV:-/dev/dax0.0}" \
    --dax-offset "${CXAN_DAX_OFFSET:-0}" \
    --policy P3 \
    --budget "$BUDGET" \
    --pipe-w 4 \
    --install-top 4 \
    --fetch-top 0 \
    --beam "$L" --k 10 --iters 0 \
    --oneshot-fp \
    --pin-entry \
    --shuffle-seed "$SEED" \
    --max-q "$MAX_Q" \
    2>&1 | tee -a "$LOG" | tee /tmp/cxan_ab_last.txt
  set -e
  local line
  line=$(grep '^CSV,' /tmp/cxan_ab_last.txt | tail -1 || true)
  if [[ -n "$line" ]]; then
    echo "${tag},${line#CSV,}" >> "$CSV"
  fi
}

run_one "packed_baseline" "$OUT/serve_vmem.env"
run_one "pagealign_AB" "$OUT/serve_vmem_pagealign.env"
echo "DONE $(date -Is)" | tee -a "$LOG"
column -t -s, "$CSV" 2>/dev/null || cat "$CSV"
