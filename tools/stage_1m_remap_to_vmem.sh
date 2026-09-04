#!/usr/bin/env bash
# Remap 1M DiskANN so each 4K page is a graph-neighbor pair, stage at 910 GiB.
# Does not write 420/460/900 GiB.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
SRC=${SRC:-$SRV/diskann_t2i_1m.bin}
OUT=${OUT:-$SRV/diskann_t2i_1m_nbrpage.bin}
MAP=${MAP:-$SRV/id_to_slot_1m.bin}
VMEM_OFF=${VMEM_OFF:-977105059840}

if [[ "$VMEM_OFF" -eq 450971566080 || "$VMEM_OFF" -eq 460571635712 || "$VMEM_OFF" -eq 966367641600 ]]; then
  echo "FAIL: refuse pagebin/bundle/seq-1M offsets"
  exit 1
fi
[[ -e /dev/vmem0 ]] || { echo "FAIL: no /dev/vmem0"; exit 1; }
[[ -f "$SRC" ]] || { echo "FAIL: no $SRC"; exit 1; }

g++ -O3 -std=c++17 -I"$ROOT" "$ROOT/tools/remap_diskann_pages.cpp" -o "$ROOT/tools/remap_diskann_pages"
"$ROOT/tools/remap_diskann_pages" --src "$SRC" --out "$OUT" --map "$MAP"

python3 - "$OUT" /dev/vmem0 "$VMEM_OFF" <<'PY'
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
sh = os.pread(sfd, 96, 0)
dh = os.pread(dfd, 96, off)
assert sh[:16] == dh[:16], (sh[:16], dh[:16])
magic, ver, n, dim, R = struct.unpack_from("<QIIII", sh, 0)
off_v, len_v = struct.unpack_from("<QQ", sh, 76)
stride = len_v // n
import random
rng = random.Random(1)
for i in rng.sample(range(n), 4):
    a = os.pread(sfd, 932, off_v + i * stride)
    b = os.pread(dfd, 932, off + off_v + i * stride)
    assert a == b, i
os.close(sfd); os.close(dfd)
print("verify header+4 slots OK")
PY
echo -n "cache_used_after_write="; cat /sys/class/vmem/vmem0/cache_used
echo "VMEM_OFF=$VMEM_OFF OUT=$OUT MAP=$MAP"
