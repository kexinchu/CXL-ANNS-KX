#!/usr/bin/env bash
# Task 4: T=4 QD / dataplane true-cold sweeps. Single-disk nvme2n1/d9 only.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
BIN=$ROOT/serving/search_beam
KO=/root/chukexin/mem2nvme/host/vmem_sw.ko
OUT=$ROOT/results/paper_figs

reload() {
  if lsmod | awk '{print $1}' | grep -qx vmem_sw; then
    rmmod vmem_sw
  fi
  insmod "$KO" \
    nvme_dev=/dev/nvme2n1 \
    target_bdf=0000:d9:00.0 \
    expected_ssd_size_bytes=1920383410176 \
    ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
  local i
  for i in $(seq 1 80); do
    [[ -e /dev/vmem0 && -e /sys/class/vmem/vmem0/cache_used ]] && break
    sleep 0.1
  done
  [[ -e /dev/vmem0 ]] || { echo "FAIL: /dev/vmem0 missing after reload" >&2; return 1; }
  local used nvme bdf
  used=$(cat /sys/class/vmem/vmem0/cache_used)
  nvme=$(cat /sys/class/vmem/vmem0/nvme_dev)
  bdf=$(cat /sys/module/vmem_sw/parameters/target_bdf)
  echo "vmem_reloaded cache_used=$used nvme=$nvme bdf=$bdf"
  [[ "$used" == "0" ]] || { echo "FAIL cache_used=$used" >&2; return 1; }
  [[ "$nvme" == "/dev/nvme2n1" ]] || { echo "FAIL nvme=$nvme (want nvme2n1)" >&2; return 1; }
  python3 - <<'PY'
import os, mmap, struct, sys
fd = os.open("/dev/vmem0", os.O_RDWR)
m = mmap.mmap(fd, 4096, offset=450971566080)
magic = struct.unpack_from("<Q", m, 0)[0]
m.close()
os.close(fd)
print(f"pagebin_magic={hex(magic)}")
if magic != 0x314e415843:
    sys.exit("FAIL pagebin magic")
PY
}

run_t4() {
  local logname=$1
  local maxq=$2
  shift 2
  echo
  echo "======== $logname maxq=$maxq extra=$* $(date -Is) ========"
  reload
  # shellcheck disable=SC1091
  source "$SRV/serve_vmem_pagebin.env"
  unset CXAN_HOST_LAYOUT
  {
    echo "# Task4 T=4 true-cold $(date -Is) kernel=$(uname -r)"
    echo "# cache_used=$(cat /sys/class/vmem/vmem0/cache_used) nvme=$(cat /sys/class/vmem/vmem0/nvme_dev) bdf=$(cat /sys/module/vmem_sw/parameters/target_bdf)"
    echo "# extra_flags=$*"
    numactl --cpunodebind=0 --membind=0 "$BIN" \
      --vmem-dev /dev/vmem0 --vmem-offset 450971566080 --vmem-len 9600069632 \
      --entry "$CXAN_ENTRY" --queries "$CXAN_QUERIES" --gt "$CXAN_GT" --id-map "$CXAN_ID_MAP" \
      --graph-file /mnt/disk0/chukexin_motivation/serving_t2i_10m/pagebin_graph.bin \
      --dram-backend numa --dram-bytes 536870912 \
      --oneshot-fp --k 10 --iters 0 --beam 400 --shuffle-seed 42 \
      --policy P3 --budget $((64<<20)) --pipe-w 16 \
      --lookahead-k 0 --nbr-bundle --no-score-page \
      --threads 4 --per-thread-window \
      --max-q "$maxq" \
      "$@"
  } 2>&1 | tee "$OUT/$logname"
  echo "WROTE $OUT/$logname"
}

mkdir -p "$OUT"
run_t4 hide_T4_base_nq20.log 20
run_t4 hide_T4_ahead3_nq20.log 20 --issue-ahead 3
run_t4 hide_T4_ebatch16_nq20.log 20 --expand-batch 16
run_t4 hide_T4_base_nq100.log 100
echo "ALL_DONE $(date -Is)"
