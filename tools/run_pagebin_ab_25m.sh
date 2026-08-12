#!/usr/bin/env bash
# Packed baseline vs starBFS-nbrblock packed + page-group B. Sequential warm (no layout thrash).
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
CSV="$RES/oneshot_fp_pagebin_ab.csv"
LOG="$RES/oneshot_fp_pagebin_ab.log"
mkdir -p "$RES"
echo "tag,pass,qps,mean_ms,recall,hit_pct,dram_hits,ssd_misses,dist,vec_stride" > "$CSV"
: > "$LOG"

run() {
  local tag=$1 envf=$2 pass=$3
  set +u
  # shellcheck disable=SC1090
  source "$envf"
  set -u
  local extra=()
  if [[ -n "${CXAN_ID_MAP:-}" ]]; then extra+=(--id-map "$CXAN_ID_MAP"); fi
  echo "=== $(date -Is) tag=$tag pass=$pass offset=$CXAN_SSD_OFFSET ===" | tee -a "$LOG"
  set +e
  out=$(numactl --cpunodebind=0 --membind=0 "$BIN" \
    --vmem-dev "$CXAN_VMEM_DEV" --vmem-offset "$CXAN_SSD_OFFSET" --vmem-len "$CXAN_LAYOUT_LEN" \
    --entry "$CXAN_ENTRY" --queries "$CXAN_QUERIES" --gt "$CXAN_GT" \
    "${extra[@]}" \
    --dram-bytes "$DRAM" --host-cap "$HOST_CAP" \
    --dram-backend dax --dax-dev "${CXAN_DAX_DEV:-/dev/dax0.0}" --dax-offset "${CXAN_DAX_OFFSET:-0}" \
    --policy P3 --budget "$BUDGET" --pipe-w 4 --install-top 4 --fetch-top 0 \
    --beam "$L" --k 10 --iters 0 --oneshot-fp --pin-entry \
    --shuffle-seed "$SEED" --max-q "$MAX_Q" 2>&1)
  set -e
  echo "$out" | tee -a "$LOG" | tail -16
  qps=$(echo "$out" | grep -oP 'throughput_QPS=\K[0-9.]+' | tail -1)
  mean=$(echo "$out" | grep -oP 'latency_ms mean=\K[0-9.]+' | tail -1)
  rec=$(echo "$out" | grep -oP 'recall@10=\K[0-9.]+' | tail -1)
  hit=$(echo "$out" | grep -oP 'cxl_dram_hit_pct=\K[0-9.]+' | tail -1)
  dh=$(echo "$out" | grep -oP 'dram_hits=\K[0-9]+' | tail -1)
  sm=$(echo "$out" | grep -oP 'ssd_misses=\K[0-9]+' | tail -1)
  dist=$(echo "$out" | grep -oP ' dist=\K[0-9]+' | tail -1)
  vs=$(echo "$out" | grep -oP 'vec_stride=\K[0-9]+' | head -1)
  echo "$tag,$pass,$qps,$mean,$rec,$hit,$dh,$sm,$dist,$vs" | tee -a "$CSV"
}

echo '==== PACKED baseline x3 ====' | tee -a "$LOG"
unset CXAN_ID_MAP || true
for p in warm1 warm2 warm3; do run packed_B "$OUT/serve_vmem.env" "$p"; done
echo '==== PAGEBIN A+B x3 ====' | tee -a "$LOG"
for p in warm1 warm2 warm3; do run pagebin_AB "$OUT/serve_vmem_pagebin.env" "$p"; done
echo "DONE $(date -Is)" | tee -a "$LOG"
column -t -s, "$CSV" 2>/dev/null || cat "$CSV"
