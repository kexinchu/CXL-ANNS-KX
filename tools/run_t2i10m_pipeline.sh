#!/usr/bin/env bash
# Build Text2Image-10M (200-d ≈5 vec/page) serving stack + P0/P3/P3+pagebin AB.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
DATA=/mnt/disk0/chukexin_motivation/data/text2image_10m
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
DISKANN=/mnt/disk0/chukexin_motivation/DiskANN_cpp/build/apps
source "$ROOT/tools/cap_enforce.sh"

N=10000000
DIM=200
R=32
LBUILD=100
ALPHA=1.2
VMEM_OFF=$((400<<30))
PAGEBIN_OFF=$((420<<30))

mkdir -p "$SRV"
BASE="$DATA/base.10M.fbin"
QUERY="$DATA/query.public.100K.fbin"
GT_RAW="$DATA/text2image-10M"

echo "[1] base present"
python3 - <<PY
import struct, os
p="$BASE"
assert os.path.exists(p), p
with open(p,"rb") as f: n,d=struct.unpack("<II", f.read(8))
assert (n,d)==($N,$DIM), (n,d)
print("base OK", n, d, "bytes", os.path.getsize(p))
PY

echo "[2] GT + queries"
python3 - <<'PY'
import struct, os
raw="/mnt/disk0/chukexin_motivation/data/text2image_10m/text2image-10M"
out="/mnt/disk0/chukexin_motivation/serving_t2i_10m/gt_10k_k10.ibin"
nq_use, k_use = 10000, 10
with open(raw,"rb") as f:
  nq,k=struct.unpack("<II", f.read(8))
  assert nq==100000 and k==100
  ids=f.read(nq*k*4)
with open(out,"wb") as o:
  o.write(struct.pack("<II", nq_use, k_use))
  for qi in range(nq_use):
    o.write(ids[qi*k*4 : qi*k*4 + k_use*4])
print("gt", out, os.path.getsize(out))
src="/mnt/disk0/chukexin_motivation/data/text2image_10m/query.public.100K.fbin"
dst="/mnt/disk0/chukexin_motivation/serving_t2i_10m/query_10k.fbin"
with open(src,"rb") as f:
  n,d=struct.unpack("<II", f.read(8))
  payload=f.read(10000*d*4)
with open(dst,"wb") as o:
  o.write(struct.pack("<II", 10000, d)); o.write(payload)
print("query_10k", os.path.getsize(dst))
PY

echo "[3] DiskANN mem index"
IDX="$SRV/mem_R${R}"
if [ ! -f "$IDX" ]; then
  "$DISKANN/build_memory_index" \
    --data_type float --dist_fn mips \
    --data_path "$BASE" \
    --index_path_prefix "$IDX" \
    -R "$R" -L "$LBUILD" --alpha "$ALPHA" -T 64 \
    2>&1 | tee "$SRV/build_index.log"
fi
ls -la "$IDX" "$IDX.data"

echo "[4] export graph"
if [ ! -f "$SRV/graph_R${R}.bin" ]; then
  python3 "$ROOT/tools/export_diskann_graph.py" \
    --diskann-index "$IDX" --n "$N" --R "$R" \
    --out "$SRV/graph_R${R}.bin" \
    --out-entry-id "$SRV/entry_id.txt"
fi
ENTRY=$(cat "$SRV/entry_id.txt")

echo "[5] packed layout @ $VMEM_OFF"
if [ ! -f "$SRV/layout_t2i_10m.json" ]; then
  "$ROOT/tools/build_layout_stream" \
    --base "$BASE" \
    --graph "$SRV/graph_R${R}.bin" \
    --out-image "$SRV/layout_t2i_10m.bin" \
    --out-entry "$SRV/serving_entry_t2i_10m.bin" \
    --out-json "$SRV/layout_t2i_10m.json" \
    --vmem-dev /dev/vmem0 --vmem-offset "$VMEM_OFF" \
    --entry-id "$ENTRY" --entry-nodes 8192 --R "$R" \
    --vec-bytes 4 --pq-bytes 32 \
    2>&1 | tee "$SRV/build_layout.log"
fi
LEN=$(python3 -c "import json; print(json.load(open('$SRV/layout_t2i_10m.json'))['image_bytes'])")
cat > "$SRV/serve_vmem.env" <<EOF
export CXAN_VMEM_DEV=/dev/vmem0
export CXAN_SSD_OFFSET=$VMEM_OFF
export CXAN_LAYOUT_LEN=$LEN
export CXAN_ENTRY=$SRV/serving_entry_t2i_10m.bin
export CXAN_DRAM_BYTES=1073741824
export CXAN_QUERIES=$SRV/query_10k.fbin
export CXAN_GT=$SRV/gt_10k_k10.ibin
export CXAN_DAX_DEV=/dev/dax0.0
export CXAN_DAX_OFFSET=0
EOF

echo "[6] pagebin A @ $PAGEBIN_OFF"
if [ ! -f "$SRV/layout_t2i_10m_pagebin.json" ]; then
  "$ROOT/tools/pack_layout_pagebin" \
    --in-vmem-dev /dev/vmem0 --in-vmem-offset "$VMEM_OFF" --in-vmem-len "$LEN" \
    --out-vmem-dev /dev/vmem0 --out-vmem-offset "$PAGEBIN_OFF" \
    --out-json "$SRV/layout_t2i_10m_pagebin.json" \
    --out-entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --out-map "$SRV/new_to_old_pagebin.bin" \
    --threads 4 \
    2>&1 | tee "$SRV/pack_pagebin.log"
fi
LEN2=$(python3 -c "import json; print(json.load(open('$SRV/layout_t2i_10m_pagebin.json'))['image_bytes'])")
cat > "$SRV/serve_vmem_pagebin.env" <<EOF
export CXAN_VMEM_DEV=/dev/vmem0
export CXAN_SSD_OFFSET=$PAGEBIN_OFF
export CXAN_LAYOUT_LEN=$LEN2
export CXAN_ENTRY=$SRV/serving_entry_t2i_10m_pagebin.bin
export CXAN_ID_MAP=$SRV/new_to_old_pagebin.bin
export CXAN_DRAM_BYTES=1073741824
export CXAN_QUERIES=$SRV/query_10k.fbin
export CXAN_GT=$SRV/gt_10k_k10.ibin
export CXAN_DAX_DEV=/dev/dax0.0
export CXAN_DAX_OFFSET=0
EOF

echo "PIPELINE_BUILD_DONE"
