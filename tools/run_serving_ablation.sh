#!/usr/bin/env bash
# Cold E7 leave-one-out on T2I. Requires serve_vmem.env (and pagebin env if PAGEBIN=1).
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
SRV=${SRV:-/mnt/disk0/chukexin_motivation/serving_t2i_10m}
OUT=${OUT:-$ROOT/results/paper_figs/e7_ablation.csv}
BIN=${BIN:-$ROOT/serving/search_beam}
BEAM=${BEAM:-300}
MAX_Q=${MAX_Q:-100}
BUDGET=${BUDGET:-$((64<<20))}
PAGEBIN=${PAGEBIN:-0}

if [[ "$PAGEBIN" == "1" ]]; then
  # shellcheck disable=SC1091
  source "$SRV/serve_vmem_pagebin.env"
else
  # shellcheck disable=SC1091
  source "$SRV/serve_vmem.env"
fi

mkdir -p "$(dirname "$OUT")"
echo "row,policy,budget,dram,nq,beam,iters,qps,mean_ms,p50,p90,p99,recall,hit_pct,dram_hits,ssd_misses,precision_pct,promote_pages,promote_used,hotset_pins,graph_in_dram,hotset_bytes,early_stop_patience" > "$OUT"

run_row() {
  local name=$1
  shift
  echo "=== ablation $name ==="
  set +e
  local log
  log=$(numactl --cpunodebind=0 --membind=0 "$BIN" \
    --vmem-dev "$CXAN_VMEM_DEV" \
    --vmem-offset "$CXAN_SSD_OFFSET" \
    --vmem-len "$CXAN_LAYOUT_LEN" \
    --entry "$CXAN_ENTRY" \
    --queries "$CXAN_QUERIES" \
    --gt "$CXAN_GT" \
    ${CXAN_ID_MAP:+--id-map "$CXAN_ID_MAP"} \
    --dram-backend numa \
    --dram-bytes "${CXAN_DRAM_BYTES:-1073741824}" \
    --host-cap $((32<<20)) \
    --oneshot-fp --k 10 --iters 0 --beam "$BEAM" \
    --flush-window --no-hotset --shuffle-seed 42 --max-q "$MAX_Q" \
    "$@" 2>&1)
  local ec=$?
  set -e
  echo "$log" | tail -n 25
  [[ $ec -eq 0 ]] || { echo "FAIL $name" >&2; exit $ec; }
  local csv
  csv=$(echo "$log" | grep '^CSV,' | tail -1 | sed 's/^CSV,//')
  echo "$name,$csv" >> "$OUT"
}

run_row full --policy P3 --budget "$BUDGET" --pipe-w 4 --install-top 4 --fetch-top 0
run_row no_promote --policy P0 --budget 0
run_row no_async --policy P3 --budget "$BUDGET" --pipe-w 1 --install-top 4 --no-vmem-prefetch
run_row install_all --policy P3 --budget "$BUDGET" --pipe-w 4 --install-all-fetched

echo "WROTE $OUT"
