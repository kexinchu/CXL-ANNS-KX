#!/usr/bin/env bash
# Fair host-window Oracle: T=1/4/8/16/32 share one 2 GiB DramWindow.
# Usage: run_oracle_host_window.sh [threads...]   default: 1 4 8 16 32
# CXL-SSD = single-disk vmem_sw on d9 (pagebin layout). Scoring stays on host.
# Does not mmap FPGA BAR, does not --require-cxl-dram, does not --switch.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX
MEM2=/root/chukexin/mem2nvme
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
SSD_BDF=0000:d9:00.0
SSD_SN=2F50A1360XK3
SSD_BYTES=1920383410176
PAGEBIN_OFF=450971566080
PAGEBIN_LEN=9600069632

resolve_ns() {
  local bdf="$1"
  local d pci bytes
  for d in /sys/block/nvme*n*; do
    [[ -e "$d/size" ]] || continue
    pci=$(basename "$(readlink -f "$d/device/device" 2>/dev/null || true)")
    [[ "$pci" == "$bdf" ]] || continue
    bytes=$(($(cat "$d/size") * 512))
    [[ "$bytes" == "$SSD_BYTES" ]] || continue
    echo "/dev/$(basename "$d")"
    return 0
  done
  return 1
}

ensure_vmem_sw() {
  local ns ko ver
  ns=$(resolve_ns "$SSD_BDF") || { echo "FAIL: no ns for $SSD_BDF"; exit 1; }
  echo "CXL-SSD pagebin disk $ns ($SSD_BDF SN=$SSD_SN)"

  ko=$MEM2/host/vmem_sw.ko
  ver=$(modinfo -F vermagic "$ko" 2>/dev/null | awk '{print $1}')
  if [[ ! -f "$ko" || "$ver" != "$(uname -r)" ]]; then
    echo "building vmem_sw for $(uname -r)"
    make -C "/lib/modules/$(uname -r)/build" "M=$MEM2/host" VMEM_SW_ONLY=1 modules
  fi

  if [[ -e /dev/vmem0 ]] && fuser /dev/vmem0 >/dev/null 2>&1; then
    echo "BUSY: /dev/vmem0 open"; fuser -v /dev/vmem0; exit 3
  fi
  if lsmod | awk '{print $1}' | grep -qx vmem_sw; then
    local live
    live=$(cat /sys/class/vmem/vmem0/nvme_dev 2>/dev/null || true)
    if [[ "$live" != "$ns" ]]; then
      echo "reloading vmem_sw $live -> $ns (single-disk pagebin)"
      rmmod vmem_sw
    fi
  fi
  if ! lsmod | awk '{print $1}' | grep -qx vmem_sw; then
    insmod "$MEM2/host/vmem_sw.ko" \
      nvme_dev="$ns" \
      target_bdf="$SSD_BDF" \
      expected_ssd_size_bytes="$SSD_BYTES" \
      ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
  fi
  local i
  for i in $(seq 1 50); do
    [[ -e /dev/vmem0 ]] && break
    sleep 0.1
  done
  [[ -e /dev/vmem0 ]] || { echo "FAIL: /dev/vmem0 missing"; exit 1; }

  python3 - <<PY
import mmap, os, sys
off = ${PAGEBIN_OFF}
fd = os.open("/dev/vmem0", os.O_RDONLY)
m = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=off)
b = m[:5]
m.close()
os.close(fd)
if b != b"CXAN1":
    print("FAIL: pagebin magic %r at %d (want CXAN1)" % (b, off))
    sys.exit(1)
print("pagebin magic CXAN1 ok")
PY
}

run_one() {
  local threads=$1
  local log=$2
  echo "======== oracle host-window T=${threads} 2GiB $(date -Is) ========"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --vmem-dev /dev/vmem0 \
    --vmem-offset "$PAGEBIN_OFF" \
    --vmem-len "$PAGEBIN_LEN" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" \
    --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --graph-file "$SRV/pagebin_graph.bin" \
    --dram-backend numa --dram-numa 1 \
    --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
    --shared-window --cpu-affinity --threads "$threads" \
    --policy P3 --nbr-bundle --no-score-page \
    --expand-batch 8 --issue-ahead 2 \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --max-q 20 --shuffle-seed 42 --pin-entry \
    --oracle-window \
    2>&1 | tee "$log"
}

g++ -O3 -mavx2 -mfma -std=c++17 -pthread \
  -I"$ROOT" "$ROOT/serving/search_beam.cpp" -o "$BIN" -lnuma

ensure_vmem_sw
mkdir -p "$OUT"
if [[ $# -gt 0 ]]; then
  TS=("$@")
else
  TS=(1 4 8 16 32)
fi
for t in "${TS[@]}"; do
  run_one "$t" "$OUT/oracle_host2g_T${t}_nq20.log"
  echo "done T=${t} -> $OUT/oracle_host2g_T${t}_nq20.log"
done
