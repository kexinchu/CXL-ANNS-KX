#!/usr/bin/env bash
# One-shot FP MIPS: find L for recall@10>=TARGET; compare P0 vs P1 (neighbor vector pages).
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
OUT=/mnt/disk0/chukexin_motivation/serving_25m
RES="$ROOT/results"
source "$ROOT/tools/cap_enforce.sh"
source "$OUT/serve_vmem.env"

DRAM=$((1 * 1024 * 1024 * 1024))
HOST_CAP=$((32 * 1024 * 1024))
TARGET=${TARGET:-0.90}
MAX_Q=${MAX_Q:-100}
SHUFFLE_SEED=${SHUFFLE_SEED:-42}
BUDGET_P1=${BUDGET_P1:-$((16 * 1024 * 1024))}
FLUSH=${FLUSH:---flush-window}

BIN="$ROOT/serving/search_beam"
CSV="$RES/oneshot_fp_r90_sweep.csv"
LOG="$RES/oneshot_fp_r90_sweep.log"
mkdir -p "$RES"
: > "$LOG"
echo "policy,budget,dram,nq,beam,iters,qps,mean_ms,p50,p90,p99,recall,hit_pct,dram_hits,ssd_misses" > "$CSV"

run() {
  local pol=$1 budget=$2 beam=$3
  echo "=== $(date -Is) pol=$pol L=$beam iters=0(fixed) budget=$budget flush=$FLUSH ===" | tee -a "$LOG"
  set +e
  # shellcheck disable=SC2086
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --vmem-dev "$CXAN_VMEM_DEV" \
    --vmem-offset "$CXAN_SSD_OFFSET" \
    --vmem-len "$CXAN_LAYOUT_LEN" \
    --entry "$CXAN_ENTRY" \
    --queries "$CXAN_QUERIES" \
    --gt "$CXAN_GT" \
    --dram-bytes "$DRAM" \
    --host-cap "$HOST_CAP" \
    --policy "$pol" \
    --budget "$budget" \
    --beam "$beam" --k 10 --iters 0 \
    --oneshot-fp \
    $FLUSH \
    --shuffle-seed "$SHUFFLE_SEED" \
    --max-q "$MAX_Q" \
    2>&1 | tee -a "$LOG" | tee /tmp/cxan_fp_last.txt
  set -e
  local line
  line=$(grep '^CSV,' /tmp/cxan_fp_last.txt | tail -1 || true)
  if [[ -n "$line" ]]; then
    echo "${line#CSV,}" >> "$CSV"
  fi
}

BEST_L=""
for L in 100 150 200 300 400 600 800; do
  run P0 0 "$L"
  rec=$(tail -1 "$CSV" | awk -F, '{print $12}')
  echo "P0 L=$L recall=$rec" | tee -a "$LOG"
  if awk -v r="$rec" -v t="$TARGET" 'BEGIN{exit !(r+0>=t)}'; then
    BEST_L=$L
    echo "P0 hit target at L=$L recall=$rec" | tee -a "$LOG"
    break
  fi
done

if [[ -z "$BEST_L" ]]; then
  echo "WARN: no L reached target; using L=800" | tee -a "$LOG"
  BEST_L=800
fi

run P1 "$BUDGET_P1" "$BEST_L"
echo "DONE $(date -Is) BEST_L=$BEST_L" | tee -a "$LOG"
