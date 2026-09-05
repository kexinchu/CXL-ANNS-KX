#!/usr/bin/env bash
# Abandon the single-disk d9 pagebin/bundle and restage through dual-disk vmem_sw.
# Caller must have approved destroying the 420/460 GiB single-disk image.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
DUAL_KO=${DUAL_KO:-/root/chukexin/mem2nvme/host/vmem_sw.ko.6.18-dual}
OUT=$ROOT/results/paper_figs
LOG=$OUT/hide_dual_restage.log
mkdir -p "$OUT"

load_dual() {
  REFUSE_DUAL_FORCE=1 /root/chukexin/mem2nvme/tools/refuse_dual_vmem_sw.sh
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
    [[ -e /dev/vmem0 && -e /sys/class/vmem/vmem0/backing_count ]] && break
    sleep 0.1
  done
  local count nvme
  count=$(cat /sys/class/vmem/vmem0/backing_count)
  nvme=$(cat /sys/class/vmem/vmem0/nvme_dev)
  echo "dual loaded backing_count=$count nvme=$nvme cache=$(cat /sys/class/vmem/vmem0/cache_used)"
  [[ "$count" == "2" ]] || { echo "FAIL backing_count=$count" >&2; return 1; }
}

{
  echo "# dual restage $(date -Is) kernel=$(uname -r) ko=$DUAL_KO"
  load_dual
  echo "===== copy pagebin ====="
  python3 "$ROOT/tools/copy_pagebin_to_vmem.py"
  echo "===== pack graph-order nbr bundle ====="
  "$ROOT/tools/pack_nbr_bundle" \
    --in-file "$SRV/layout_t2i_10m.bin" \
    --graph-file "$SRV/pagebin_graph.bin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --out-vmem-dev /dev/vmem0 \
    --out-vmem-offset 460571635712 \
    --out-json "$OUT/nbr_bundle_dual.json" \
    --threads 8
  echo "===== verify magic ====="
  python3 - <<'PY'
import os, mmap, struct, sys
fd = os.open("/dev/vmem0", os.O_RDONLY)
m = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=450971566080)
magic = struct.unpack_from("<Q", m, 0)[0]
n, dim, R = struct.unpack_from("<III", m, 12)
m.close(); os.close(fd)
print(f"pagebin_magic={hex(magic)} n={n} dim={dim} R={R}")
if magic != 0x314e415843 or n != 10000000:
    sys.exit("FAIL dual pagebin magic")
print("DUAL_RESTAGE_OK")
PY
  echo "DONE $(date -Is)"
} 2>&1 | tee "$LOG"
echo WROTE "$LOG"
