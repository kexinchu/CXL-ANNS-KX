#!/usr/bin/env bash
# After wiki_cohere_25m/base.bin is ready: replace 1M pointers, build DiskANN mem index on CXL, search smoke.
set -euo pipefail
DATA=/mnt/disk0/chukexin_motivation/data/wiki_cohere_25m
DISKANN=/mnt/disk0/chukexin_motivation/diskann_data_25m
APP=/mnt/disk0/chukexin_motivation/DiskANN_cpp/build/apps
BASE="$DATA/base.bin"
NEED=$((8 + 25000000 * 768 * 4))

echo "Waiting for $BASE size=$NEED ..."
while true; do
  if [[ -f "$BASE" ]]; then
    sz=$(stat -c%s "$BASE")
    if [[ "$sz" -eq "$NEED" ]]; then break; fi
    echo "  have $sz / $NEED"
  else
    # still downloading parts?
    du -sh "$DATA" || true
  fi
  sleep 60
done
echo "base ready"

mkdir -p "$DISKANN"
# Replace old 1M serving pointers with symlinks
ln -sfn "$BASE" "$DISKANN/base.bin"
ln -sfn "$DATA/query.bin" "$DISKANN/query.bin"
# Keep a marker that supersedes laion1m
echo "wiki-cohere-25M (768-d, first 25M of wikipedia_base.bin) replaces laion≈1M" > "$DISKANN/REPLACE_NOTE.txt"

sync; echo 3 > /proc/sys/vm/drop_caches
numactl -H | head -12

OUT="$DISKANN/mem_R32"
if [[ ! -f "$OUT" ]]; then
  numactl --preferred=1 "$APP/build_memory_index" \
    --data_type float --dist_fn mips \
    --data_path "$BASE" \
    --index_path_prefix "$OUT" \
    --max_degree 32 --Lbuild 64 --alpha 1.2 -T 64 \
    2>&1 | tee "$OUT.build.log"
fi

# Load into CXL-DRAM (copy) + DiskANN search membind
ROOT=/root/chukexin/CXL-ANNS-KX
g++ -O2 -std=c++17 "$ROOT/tools/load_index_cxl_dram.cpp" -o "$ROOT/tools/load_index_cxl_dram" -lnuma
"$ROOT/tools/load_index_cxl_dram" --base "$BASE" --index-prefix "$OUT" --touch --hold-s 3

numactl --membind=1 --cpunodebind=0 \
  "$APP/search_memory_index" \
  --data_type float --dist_fn mips \
  --index_path_prefix "$OUT" \
  --query_file "$DATA/query.bin" \
  --result_path "$DISKANN/search_cxl" \
  --recall_at 10 --search_list 64 \
  2>&1 | tee "$DISKANN/search_cxl.log"

echo ALL_DONE
