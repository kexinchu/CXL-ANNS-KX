#!/usr/bin/env bash
# Restore software CXL-SSD (/dev/vmem0) without switching kernels.
# Rebuilds vmem_sw for the *running* kernel if vermagic mismatches.
#
# Identity (Dell CD8P @ d8): serial 7EU0A01P0XK1, 1920383410176 bytes.
# Namespace name may drift (was nvme3n1 / nvme3n2; currently nvme1n1).
# Serving 25M layout requires ram_size_gib=28 (stripe map).
set -euo pipefail

SRC_KO_DIR=${SRC_KO_DIR:-/root/chukexin/mem2nvme/host}
REF_TREE=${REF_TREE:-/home/victoryang00/nvme-mem2nvm}
BDF=0000:d8:00.0
EXPECTED_SIZE=1920383410176
EXPECTED_SN=7EU0A01P0XK1
RAM_GIB=28
CACHE_GIB=4
STRIPE_MIB=2

# Resolve namespace by BDF (name may be nvmeXn1 or nvmeXn2 after renumber).
NVME_NS=""
NVME_CHAR=""
for d in /sys/block/nvme*n*; do
  [[ -e "$d/size" ]] || continue
  pci=$(readlink -f "$d/device/device" 2>/dev/null || true)
  [[ "$(basename "$pci")" == "$BDF" ]] || continue
  BYTES_CAND=$(($(cat "$d/size") * 512))
  [[ "$BYTES_CAND" == "$EXPECTED_SIZE" ]] || continue
  NVME_NS="/dev/$(basename "$d")"
  # Controller chardev (namespace name index may not match ctrl index).
  ctrl=$(basename "$(readlink -f "$d/device")")
  NVME_CHAR="/dev/$ctrl"
  break
done
[[ -n "$NVME_NS" ]] || { echo "FAIL: no nvme namespace for $BDF size=$EXPECTED_SIZE"; exit 1; }

SN=$(nvme id-ctrl "$NVME_CHAR" 2>/dev/null | awk '/^sn /{print $3}')
BYTES=$(($(cat /sys/block/$(basename "$NVME_NS")/size) * 512))
echo "found $NVME_NS ctrl=$NVME_CHAR sn=$SN bytes=$BYTES"
[[ "$SN" == "$EXPECTED_SN" ]] || { echo "FAIL: serial mismatch (got '$SN')"; exit 1; }
[[ "$BYTES" == "$EXPECTED_SIZE" ]] || { echo "FAIL: size mismatch"; exit 1; }

# Build for running kernel if needed
KO="$SRC_KO_DIR/vmem_sw.ko"
NEED_BUILD=1
if [[ -f "$KO" ]]; then
  ver=$(modinfo "$KO" 2>/dev/null | awk '/vermagic/{print $2}')
  [[ "$ver" == "$(uname -r)" ]] && NEED_BUILD=0
fi
if (( NEED_BUILD )); then
  echo "building vmem_sw for $(uname -r) (no kernel switch)"
  make -C "/lib/modules/$(uname -r)/build" "M=$SRC_KO_DIR" VMEM_SW_ONLY=1 modules
fi

lsmod | awk '{print $1}' | grep -qx vmem_sw && rmmod vmem_sw
insmod "$KO" \
  nvme_dev="$NVME_NS" \
  target_bdf="$BDF" \
  expected_ssd_size_bytes="$EXPECTED_SIZE" \
  ram_size_gib="$RAM_GIB" \
  cache_size_gib="$CACHE_GIB" \
  stripe_size_mib="$STRIPE_MIB"

for i in $(seq 1 50); do
  [[ -e /dev/vmem0 ]] && break
  sleep 0.1
done
[[ -e /dev/vmem0 ]] || { echo "FAIL: /dev/vmem0 missing"; exit 1; }

echo "OK /dev/vmem0 nvme=$(cat /sys/class/vmem/vmem0/nvme_dev) size=$(cat /sys/class/vmem/vmem0/size) ram=$(cat /sys/class/vmem/vmem0/ram_size) backend=$(cat /sys/class/vmem/vmem0/backend)"
