#!/usr/bin/env bash
# Stage extent-clustered 1M DiskANN at 950 GiB. Does not write 420/460/900/930.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
SRC=${SRC:-$SRV/diskann_t2i_1m_extent.bin}
VMEM_OFF=${VMEM_OFF:-1020054732800}

forbid=(450971566080 460571635712 966367641600 977105059840 987842478080 998579896320 1009317314560)
for o in "${forbid[@]}"; do
  if [[ "$VMEM_OFF" -eq "$o" ]]; then
    echo "FAIL: refuse protected offset $VMEM_OFF"
    exit 1
  fi
done
[[ -e /dev/vmem0 ]] || { echo "FAIL: no /dev/vmem0"; exit 1; }
[[ -f "$SRC" ]] || { echo "FAIL: no $SRC"; exit 1; }
if fuser /dev/vmem0 >/dev/null 2>&1; then
  echo "FAIL: /dev/vmem0 is open" >&2
  fuser -v /dev/vmem0 >&2 || true
  exit 1
fi

python3 - "$SRC" /dev/vmem0 "$VMEM_OFF" <<'PY'
import os, sys, mmap, struct
src, dst, off = sys.argv[1], sys.argv[2], int(sys.argv[3])
ln = os.path.getsize(src)
two_m = 2 << 20
map_len = (ln + two_m - 1) & ~(two_m - 1)
print(f"copy {src} ({ln}) -> {dst} off={off} map={map_len}", flush=True)
sfd = os.open(src, os.O_RDONLY)
dfd = os.open(dst, os.O_RDWR)
sm = mmap.mmap(sfd, ln, mmap.MAP_SHARED, mmap.PROT_READ, offset=0)
dm = mmap.mmap(dfd, map_len, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
WIN = two_m
for i in range(0, ln, WIN):
    n = min(WIN, ln - i)
    dm[i:i+n] = sm[i:i+n]
    if i and i % (256 << 20) == 0:
        print(f"  {i >> 20} MiB", flush=True)
sm.close(); dm.close()
os.close(sfd); os.close(dfd)
sfd = os.open(src, os.O_RDONLY)
dfd = os.open(dst, os.O_RDONLY)
sm = mmap.mmap(sfd, ln, mmap.MAP_SHARED, mmap.PROT_READ, offset=0)
dm = mmap.mmap(dfd, map_len, mmap.MAP_SHARED, mmap.PROT_READ, offset=off)
assert sm[:16] == dm[:16], (sm[:16], dm[:16])
magic, ver, n, dim, R = struct.unpack_from("<QIIII", sm, 0)
off_v, len_v = struct.unpack_from("<QQ", sm, 76)
stride = len_v // n
import random
rng = random.Random(1)
for i in rng.sample(range(n), 4):
    a = sm[off_v + i * stride:off_v + i * stride + 932]
    b = dm[off_v + i * stride:off_v + i * stride + 932]
    assert a == b, i
sm.close(); dm.close()
os.close(sfd); os.close(dfd)
print("verify header+4 slots OK")
PY
echo -n "cache_used_after_write="; cat /sys/class/vmem/vmem0/cache_used
echo "VMEM_OFF=$VMEM_OFF SRC=$SRC"
