#!/usr/bin/env bash
# Cold paper eval on this boot. Reloads vmem_sw between true-cold rows.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
BIN=$ROOT/serving/search_beam
KO=/root/chukexin/mem2nvme/host/vmem_sw.ko
OUTDIR=$ROOT/results/paper_figs
LOG=$OUTDIR/2026-08-30-eval.log
CSV=$OUTDIR/2026-08-30-eval.csv
MD=$OUTDIR/2026-08-30-eval-run.md

NVME=/dev/nvme4n1
BDF=0000:d9:00.0
SSD_BYTES=1920383410176

unset CXAN_ID_MAP
# shellcheck disable=SC1091
source "$SRV/serve_vmem.env"

reload_vmem() {
  if lsmod | awk '{print $1}' | grep -qx vmem_sw; then
    rmmod vmem_sw
  fi
  insmod "$KO" \
    nvme_dev="$NVME" \
    target_bdf="$BDF" \
    expected_ssd_size_bytes="$SSD_BYTES" \
    ram_size_gib=28 \
    cache_size_gib=4 \
    stripe_size_mib=2
  local i
  for i in $(seq 1 50); do
    [[ -e /dev/vmem0 ]] && break
    sleep 0.05
  done
  [[ -e /dev/vmem0 ]] || { echo "FAIL: /dev/vmem0 missing after reload" >&2; return 1; }
  local used
  used=$(cat /sys/class/vmem/vmem0/cache_used)
  echo "vmem_reloaded cache_used=$used nvme=$(cat /sys/class/vmem/vmem0/nvme_dev)"
}

run_row() {
  local name=$1
  local layout=$2  # packed|pagebin
  shift 2
  echo
  echo "======== $name layout=$layout $(date -Is) ========"
  reload_vmem
  unset CXAN_ID_MAP
  local envf="$SRV/serve_vmem.env"
  if [[ "$layout" == pagebin ]]; then
    envf="$SRV/serve_vmem_pagebin.env"
  fi
  # shellcheck disable=SC1090
  source "$envf"
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
    --dram-backend numa --dram-numa 0 \
    --dram-bytes "${DRAM_BYTES:-1073741824}" \
    --host-cap $((64 << 20)) \
    --oneshot-fp --k 10 --iters 0 \
    --flush-window --shuffle-seed 42 --pin-entry \
    "$@" 2>&1)
  local ec=$?
  set -e
  echo "$log" | tee -a "$LOG"
  if [[ $ec -ne 0 ]]; then
    echo "FAIL $name ec=$ec" | tee -a "$LOG"
    return $ec
  fi
  local csv
  csv=$(echo "$log" | grep '^CSV,' | tail -1 | sed 's/^CSV,//')
  local qps rec hit gbs p99 ev
  qps=$(echo "$log" | sed -n 's/.*throughput_QPS=//p' | awk '{print $1}' | tail -1)
  rec=$(echo "$log" | sed -n 's/^recall@[0-9]*=//p' | tail -1)
  hit=$(echo "$log" | sed -n 's/^cxl_dram_hit_pct=//p' | tail -1)
  gbs=$(echo "$log" | sed -n 's/.*cxl_ssd_to_dram_promote_GBps=//p' | awk '{print $1}' | tail -1)
  p99=$(echo "$log" | awk '/^latency_ms/{print $5}' | tail -1)
  ev=$(echo "$log" | awk '{for(i=1;i<=NF;i++) if($i ~ /^evicts=/) {split($i,a,"="); print a[2]}}' | tail -1)
  echo "$name,$layout,$csv" >> "$CSV"
  {
    echo
    echo "### $name ($layout)"
    echo
    echo "- QPS=$qps recall@10=$rec hit%=$hit promote_GBps=$gbs p99_ms=$p99 evicts=${ev:-?}"
    echo "- CSV: \`$csv\`"
    echo "- finished: $(date -Is)"
  } >> "$MD"
  echo "ROW_OK $name QPS=$qps recall=$rec hit=$hit GBps=$gbs"
}

mkdir -p "$OUTDIR"
touch "$LOG"
if [[ ! -s "$CSV" ]]; then
  echo "row,layout,policy,budget,dram,nq,beam,iters,qps,mean_ms,p50,p90,p99,recall,hit_pct,dram_hits,ssd_misses" > "$CSV"
fi

echo "=== IDENTITY $(date -Is) kernel=$(uname -r) ===" | tee -a "$LOG"
echo "nvme4=$(cat /sys/class/vmem/vmem0/nvme_dev) cache=$(cat /sys/class/vmem/vmem0/cache_used)" | tee -a "$LOG"
ls /dev/dax* 2>/dev/null | tee -a "$LOG" || echo "DAX absent" | tee -a "$LOG"

case "${1:-all}" in
  smoke)
    DRAM_BYTES=1073741824 run_row smoke_p3_nq10 packed \
      --policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 --beam 400 --max-q 10
    ;;
  smoke-pagebin)
    DRAM_BYTES=1073741824 run_row smoke_p3_nq10 pagebin \
      --policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 --beam 400 --max-q 10
    ;;
  money)
    DRAM_BYTES=1073741824 run_row p0_cold packed \
      --policy P0 --budget 0 --beam 400 --max-q 50
    DRAM_BYTES=1073741824 run_row p3_cold packed \
      --policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 --beam 400 --max-q 50
    DRAM_BYTES=1073741824 run_row p3_no_prefetch packed \
      --policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 --no-vmem-prefetch --beam 400 --max-q 50
    DRAM_BYTES=1073741824 run_row p3_install_all packed \
      --policy P3 --budget $((64<<20)) --pipe-w 4 --install-all-fetched --beam 400 --max-q 50
    ;;
  money-pagebin)
    DRAM_BYTES=1073741824 run_row p3_pagebin_cold pagebin \
      --policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 --beam 400 --max-q 50
    ;;
  scale)
    local_layout=${2:-packed}
    DRAM_BYTES=268435456 run_row scale_t1 "$local_layout" \
      --policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 --beam 400 --max-q 50 \
      --threads 1 --per-thread-window
    DRAM_BYTES=268435456 run_row scale_t4 "$local_layout" \
      --policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 --beam 400 --max-q 50 \
      --threads 4 --per-thread-window
    DRAM_BYTES=268435456 run_row scale_t8 "$local_layout" \
      --policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 --beam 400 --max-q 50 \
      --threads 8 --per-thread-window
    ;;
  all)
    "$0" smoke
    "$0" money
    "$0" scale packed
    ;;
  *)
    echo "usage: $0 smoke|money|money-pagebin|scale [packed|pagebin]|all" >&2
    exit 2
    ;;
esac
