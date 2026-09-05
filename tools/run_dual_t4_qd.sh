#!/usr/bin/env bash
# Dual-disk true-cold peak + T=1 sanity + T=4-only QD. Does not change CLI defaults.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
BIN=$ROOT/serving/search_beam
PROBE=$ROOT/serving/tools/vmem_bw_probe
DUAL_KO=${DUAL_KO:-/root/chukexin/mem2nvme/host/vmem_sw.ko.6.18-dual}
OUT=$ROOT/results/paper_figs
PAGEBIN_OFF=450971566080

reload_dual() {
  if lsmod | awk '{print $1}' | grep -qx vmem_sw; then
    rmmod vmem_sw
  fi
  insmod "$DUAL_KO" \
    nvme_devs=/dev/nvme1n1,/dev/nvme2n1 \
    target_bdfs=0000:d8:00.0,0000:d9:00.0 \
    expected_ssd_sizes_bytes=1920383410176,1920383410176 \
    ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
  local i
  for i in $(seq 1 80); do
    [[ -e /dev/vmem0 && -e /sys/class/vmem/vmem0/cache_used ]] && break
    sleep 0.1
  done
  local used count nvme
  used=$(cat /sys/class/vmem/vmem0/cache_used)
  count=$(cat /sys/class/vmem/vmem0/backing_count)
  nvme=$(cat /sys/class/vmem/vmem0/nvme_dev)
  echo "vmem_reloaded cache_used=$used backing_count=$count nvme=$nvme"
  [[ "$used" == "0" ]] || { echo "FAIL cache_used=$used" >&2; return 1; }
  [[ "$count" == "2" ]] || { echo "FAIL backing_count=$count" >&2; return 1; }
  python3 - <<'PY'
import os, mmap, struct, sys
fd = os.open("/dev/vmem0", os.O_RDWR)
m = mmap.mmap(fd, 4096, offset=450971566080)
magic = struct.unpack_from("<Q", m, 0)[0]
m.close(); os.close(fd)
print(f"pagebin_magic={hex(magic)}")
if magic != 0x314e415843:
    sys.exit("FAIL pagebin magic")
PY
}

run_search() {
  local logname=$1
  local threads=$2
  local dram=$3
  local maxq=$4
  shift 4
  echo
  echo "======== $logname T=$threads maxq=$maxq extra=$* $(date -Is) ========"
  reload_dual
  # shellcheck disable=SC1091
  source "$SRV/serve_vmem_pagebin.env"
  unset CXAN_HOST_LAYOUT
  {
    echo "# dual $(date -Is) kernel=$(uname -r)"
    echo "# cache_used=$(cat /sys/class/vmem/vmem0/cache_used) nvme=$(cat /sys/class/vmem/vmem0/nvme_dev) backings=$(cat /sys/class/vmem/vmem0/backing_count)"
    echo "# extra_flags=$*"
    numactl --cpunodebind=0 --membind=0 "$BIN" \
      --vmem-dev /dev/vmem0 --vmem-offset "$PAGEBIN_OFF" --vmem-len 9600069632 \
      --entry "$CXAN_ENTRY" --queries "$CXAN_QUERIES" --gt "$CXAN_GT" --id-map "$CXAN_ID_MAP" \
      --graph-file "$SRV/pagebin_graph.bin" \
      --dram-backend numa --dram-bytes "$dram" \
      --oneshot-fp --k 10 --iters 0 --beam 400 --shuffle-seed 42 \
      --policy P3 --budget $((64<<20)) --pipe-w 16 \
      --lookahead-k 0 --nbr-bundle --no-score-page \
      --threads "$threads" --per-thread-window \
      --max-q "$maxq" \
      "$@"
  } 2>&1 | tee "$OUT/$logname"
  echo "WROTE $OUT/$logname"
}

mkdir -p "$OUT"

echo "===== isolated PREFETCH_BATCH peak ====="
reload_dual
{
  echo "# dual peak $(date -Is) kernel=$(uname -r)"
  echo "# nvme=$(cat /sys/class/vmem/vmem0/nvme_dev) cache=$(cat /sys/class/vmem/vmem0/cache_used)"
  "$PROBE" /dev/vmem0 "$PAGEBIN_OFF" rnd 16384
  echo "--- seq ---"
  "$PROBE" /dev/vmem0 "$PAGEBIN_OFF" seq 16384
} 2>&1 | tee "$OUT/hide_dual_peak.log"
echo WROTE "$OUT/hide_dual_peak.log"

run_search hide_dual_T1_nq20.log 1 1073741824 20
run_search hide_dual_T4_base_nq20.log 4 536870912 20
run_search hide_dual_T4_ahead3_nq20.log 4 536870912 20 --issue-ahead 3
run_search hide_dual_T4_ebatch16_nq20.log 4 536870912 20 --expand-batch 16
echo "ALL_DONE $(date -Is)"
