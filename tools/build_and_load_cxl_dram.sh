#!/usr/bin/env bash
# Build DiskANN in-memory index then pin vectors+index into CXL-DRAM (NUMA node1).
# Usage: tools/build_and_load_cxl_dram.sh /path/to/base.bin /path/to/out_prefix
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASE_BIN="${1:?base.bin}"
OUT_PREFIX="${2:?out prefix e.g. /mnt/disk0/.../mem_R32}"
DISKANN_APP="${DISKANN_APP:-/mnt/disk0/chukexin_motivation/DiskANN_cpp/build/apps}"
R="${R:-32}"
L="${L:-64}"
ALPHA="${ALPHA:-1.2}"
DIST="${DIST:-mips}"   # wikipedia-cohere uses IP/MIPS

echo "base=$BASE_BIN out=$OUT_PREFIX R=$R L=$L dist=$DIST"
# Drop page cache to free CXL node before build/load
sync; echo 3 > /proc/sys/vm/drop_caches || true
numactl -H | sed -n '1,12p'

# Build memory index with pages preferably on node1
mkdir -p "$(dirname "$OUT_PREFIX")"
numactl --membind=1 --preferred=1 \
  "$DISKANN_APP/build_memory_index" \
  --data_type float --dist_fn "$DIST" \
  --data_path "$BASE_BIN" \
  --index_path_prefix "$OUT_PREFIX" \
  --max_degree "$R" --L "$L" --alpha "$ALPHA" \
  2>&1 | tee "${OUT_PREFIX}.build.log"

echo "Built index prefix $OUT_PREFIX"
ls -lh "${OUT_PREFIX}"* | head
