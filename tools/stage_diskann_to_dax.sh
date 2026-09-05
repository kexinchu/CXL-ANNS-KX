#!/usr/bin/env bash
# Pack pagebin+graph onto /dev/dax0.0 as version-2 DiskANN (STRIDE=2048).
# Does not touch the 420 GiB pagebin vmem offset.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
DAX=${DAX:-/dev/dax0.0}
STRIDE=${STRIDE:-2048}
NAV=${NAV:-$ROOT/results/paper_figs/nav_10k.bin}
mkdir -p "$(dirname "$NAV")"

dax_sz=$(cat /sys/bus/dax/devices/dax0.0/size 2>/dev/null || echo 0)
echo "dax=$DAX size=$dax_sz"
if [[ "$dax_sz" -lt 20480000000 ]]; then
  echo "FAIL: dax too small for 10M×2048"
  exit 1
fi

g++ -O3 -std=c++17 -I"$ROOT" "$ROOT/tools/pack_diskann_entry.cpp" -o "$ROOT/tools/pack_diskann_entry"
g++ -O3 -std=c++17 -I"$ROOT" "$ROOT/tools/build_nav_graph.cpp" -o "$ROOT/tools/build_nav_graph"

HOST_IMG=${HOST_IMG:-$SRV/diskann_t2i_10m.bin}
echo "packing DiskANN to $HOST_IMG (host file; dax dense write is unreliable on this machine)"
"$ROOT/tools/pack_diskann_entry" \
  --vecs "$SRV/pagebin_image.bin" \
  --graph "$SRV/pagebin_graph.bin" \
  --out "$HOST_IMG" \
  --stride "$STRIDE"

echo "building nav_10k.bin seed=42"
"$ROOT/tools/build_nav_graph" --src "$HOST_IMG" --out "$NAV" --n0 10000 --seed 42

python3 - "$HOST_IMG" "$SRV/pagebin_image.bin" "$SRV/pagebin_graph.bin" <<'PY'
import struct, sys, random
dax, vecs, graph = sys.argv[1], sys.argv[2], sys.argv[3]
def rd(path, off, n):
    with open(path, 'rb') as f:
        f.seek(off)
        return f.read(n)
hdr = rd(dax, 0, 96)
magic, ver, n, dim, R = struct.unpack_from('<QIIII', hdr, 0)
assert magic == 0x314e415843, hex(magic)
assert ver == 2 and n == 10_000_000 and dim == 200 and R == 32
# packed header: 8 + 10*4 = 48? pad makes 44 + off_pq; off_vectors at 76.
off_vectors, len_vectors = struct.unpack_from('<QQ', hdr, 76)
print(f'header n={n} ver={ver} off_vectors={off_vectors} len_vectors={len_vectors}')
assert off_vectors == 4096
stride = len_vectors // n
assert stride == 2048
vh = rd(vecs, 0, 96)
v_off, v_len = struct.unpack_from('<QQ', vh, 76)
v_stride = v_len // n
rng = random.Random(42)
ids = [rng.randrange(n) for _ in range(8)]
ok = 0
for i in ids:
    e = rd(dax, off_vectors + i * stride, 932)
    v = rd(vecs, v_off + i * v_stride, 800)
    g = rd(graph, i * R * 4, R * 4)
    nn = struct.unpack_from('<I', e, 800)[0]
    assert e[:800] == v, i
    assert nn == R
    assert e[804:804+128] == g, i
    ok += 1
print(f'verify 8 ids OK {ids}')
PY

echo "OK staged $HOST_IMG + $NAV"
