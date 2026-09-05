#!/usr/bin/env bash
# Build one admitted 10M host artifact set. No device staging is performed here.
set -euo pipefail

if [[ $# -ne 1 || ( "$1" != yfcc10m && "$1" != laion10m ) ]]; then
  echo "usage: $0 yfcc10m|laion10m" >&2
  exit 2
fi

DATASET=$1
ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
DISKANN=/mnt/disk0/chukexin_motivation/DiskANN_cpp/build/apps
PIPEANN_BUILD=/root/chukexin/CXL-ANNS-KX/third_party/PipeANN/build/tests/build_disk_index
N=10000000
R=32
LBUILD=100
THREADS=64

if [[ "$DATASET" == yfcc10m ]]; then
  SRV=/mnt/disk0/chukexin_motivation/serving_yfcc_10m
  DIM=192
  METRIC=l2
  STRIDE=2048
  STEM=yfcc
  MIN_FREE=$((55 << 30))
else
  SRV=/mnt/disk0/chukexin_motivation/serving_laion_10m
  DIM=512
  METRIC=mips
  STRIDE=4096
  STEM=laion
  MIN_FREE=$((100 << 30))
fi

BASE=$SRV/base.10M.fbin
QUERY=$SRV/query_10k.fbin
MEM=$SRV/mem_R32
GRAPH=$SRV/diskann_${STEM}_10m.graph.bin
ENTRY_ID=$SRV/entry_id.txt
IDENTITY=$SRV/new_to_old_pagebin.bin
ORACLE=$SRV/diskann_${STEM}_10m.bin
NEIGHBOR_MAP=$SRV/id_to_slot_10m_neighbor.scratch.bin
SLOT_MAP=$SRV/id_to_slot_10m_extent.bin
EXTENT=$SRV/diskann_${STEM}_10m_extent.bin
NAV=$SRV/nav_10k.bin
ENTRY=$SRV/serving_entry_${STEM}_10m_pagebin.bin
PQ_PREFIX=$SRV/index_pq64
PIPE_PREFIX=$SRV/pipeann
LOG=$SRV/build-host-artifacts.log

cd "$ROOT"
mkdir -p "$SRV"
exec > >(tee -a "$LOG") 2>&1

echo "BUILD_BEGIN dataset=$DATASET metric=$METRIC R=$R L=$LBUILD PQ=64 stride=$STRIDE"
[[ -x "$DISKANN/build_memory_index" && -x "$DISKANN/search_disk_index" ]]
[[ -x "$PIPEANN_BUILD" ]]
python3 - "$BASE" "$N" "$DIM" "$MIN_FREE" <<'PY'
import os, struct, sys
path, n, dim, minimum = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
with open(path, "rb") as source:
    shape = struct.unpack("<II", source.read(8))
assert shape == (n, dim), (shape, (n, dim))
assert os.path.getsize(path) == 8 + n * dim * 4
free = os.statvfs(os.path.dirname(path)).f_bavail * os.statvfs(os.path.dirname(path)).f_frsize
assert free >= minimum, ("insufficient free bytes", free, minimum)
print(f"preflight base={shape} free_bytes={free} minimum={minimum}")
PY

build_cpp() {
  local source=$1 output=$2
  if [[ ! -x "$output" || "$source" -nt "$output" ]]; then
    g++ -O3 -std=c++17 -march=native -pthread -I. "$source" -o "$output"
  fi
}
build_cpp tools/rebuild_diskann_from_base.cpp tools/rebuild_diskann_from_base
build_cpp tools/remap_diskann_pages.cpp tools/remap_diskann_pages
build_cpp tools/apply_diskann_slot_map.cpp tools/apply_diskann_slot_map
build_cpp tools/build_nav_graph.cpp tools/build_nav_graph
if [[ ! -x tools/build_diskann_pq || tools/build_diskann_pq.cpp -nt tools/build_diskann_pq ]]; then
  g++ -O3 -std=c++17 -march=native -mavx2 -fopenmp -I. \
    tools/build_diskann_pq.cpp -o tools/build_diskann_pq
fi

if [[ ! -f "$MEM" || ! -f "$MEM.data" ]]; then
  [[ ! -e "$MEM" && ! -e "$MEM.data" ]]
  "$DISKANN/build_memory_index" --data_type float --dist_fn "$METRIC" \
    --data_path "$BASE" --index_path_prefix "$MEM" -R "$R" -L "$LBUILD" \
    --alpha 1.2 -T "$THREADS"
fi

if [[ ! -f "$GRAPH" || ! -f "$ENTRY_ID" ]]; then
  [[ ! -e "$GRAPH" && ! -e "$ENTRY_ID" ]]
  python3 tools/export_diskann_graph.py --diskann-index "$MEM" --n "$N" --R "$R" \
    --out "$GRAPH" --out-entry-id "$ENTRY_ID"
fi
[[ "$(stat -c %s "$GRAPH")" -eq $((N * R * 4)) ]]
ENTRY_VALUE=$(<"$ENTRY_ID")
[[ "$ENTRY_VALUE" =~ ^[0-9]+$ && "$ENTRY_VALUE" -lt "$N" ]]

if [[ ! -f "$IDENTITY" ]]; then
  python3 -m experiments.eval.flashanns.build_aux identity --out "$IDENTITY" --n "$N"
fi
[[ "$(stat -c %s "$IDENTITY")" -eq $((N * 4)) ]]

if [[ ! -f "${PQ_PREFIX}_pq_pivots.bin" || ! -f "${PQ_PREFIX}_pq_compressed.bin" ]]; then
  [[ ! -e "${PQ_PREFIX}_pq_pivots.bin" && ! -e "${PQ_PREFIX}_pq_compressed.bin" ]]
  tools/build_diskann_pq --data "$BASE" --out-prefix "$PQ_PREFIX" \
    --chunks 64 --train 256000 --seed 42
fi
[[ "$(stat -c %s "${PQ_PREFIX}_pq_compressed.bin")" -eq $((8 + N * 64)) ]]

if [[ ! -f "$ORACLE" ]]; then
  tools/rebuild_diskann_from_base --base "$BASE" --id-map "$IDENTITY" \
    --graph "$GRAPH" --out "$ORACLE" --entry-id "$ENTRY_VALUE" \
    --entry-nodes 8192 --R "$R" --stride "$STRIDE"
fi
[[ "$(stat -c %s "$ORACLE")" -eq $((4096 + N * STRIDE)) ]]

if [[ ! -f "$NAV" ]]; then
  tools/build_nav_graph --src "$ORACLE" --out "$NAV" --n0 10000 --seed 42
fi
if [[ ! -f "$ENTRY" ]]; then
  python3 -m experiments.eval.flashanns.build_aux entry --graph "$GRAPH" --out "$ENTRY" \
    --n "$N" --degree "$R" --entry-id "$ENTRY_VALUE" --limit 8192
fi
[[ "$(stat -c %s "$ENTRY")" -eq $((12 + 8192 * 4 + 8192 * R * 4)) ]]

if [[ ! -f "$NEIGHBOR_MAP" ]]; then
  tools/remap_diskann_pages --src "$ORACLE" --out "$SRV/unused-neighbor-image" \
    --map "$NEIGHBOR_MAP" --mode neighbor --map-only
fi
if [[ ! -f "$SLOT_MAP" ]]; then
  tools/remap_diskann_pages --src "$ORACLE" --out "$SRV/unused-extent-image" \
    --map "$SLOT_MAP" --mode extent --map-in "$NEIGHBOR_MAP" --map-only
fi
[[ "$(stat -c %s "$SLOT_MAP")" -eq $((N * 4)) ]]

# The memory index is reproducible scratch. Remove it only after its exported
# graph, PQ codes, and canonical image have passed exact size gates.
rm -f -- "$MEM" "$MEM.data"

if [[ ! -f "$EXTENT" ]]; then
  tools/apply_diskann_slot_map --src "$ORACLE" --slot-map "$SLOT_MAP" --out "$EXTENT"
fi
[[ "$(stat -c %s "$EXTENT")" -eq $((4096 + N * STRIDE)) ]]
rm -f -- "$NEIGHBOR_MAP"

if [[ "$DATASET" == laion10m && ! -f "$SRV/source_gt_10k_k10.ibin" ]]; then
  "$DISKANN/utils/compute_groundtruth" --data_type float --dist_fn mips \
    --base_file "$BASE" --query_file "$QUERY" \
    --gt_file "$SRV/source_gt_10k_k10.ibin" --K 10
fi
if [[ "$DATASET" == laion10m && ! -f "$SRV/gt_10k_k10.ibin" ]]; then
  python3 - "$SRV/source_gt_10k_k10.ibin" "$SRV/gt_10k_k10.ibin" <<'PY'
import sys
from pathlib import Path
from experiments.eval.flashanns.prepare_yfcc import extract_gt_topk
extract_gt_topk(Path(sys.argv[1]), Path(sys.argv[2]), 10000, 10)
PY
fi

if [[ ! -f "${PIPE_PREFIX}_disk.index" ]]; then
  "$PIPEANN_BUILD" float "$BASE" "$PIPE_PREFIX" 32 100 64 64 "$THREADS" "$METRIC" pq
fi
[[ -f "${PIPE_PREFIX}_disk.index" ]]

python3 - "$DATASET" "$SRV" "$ORACLE" "$EXTENT" "$GRAPH" "$IDENTITY" "$SLOT_MAP" \
  "${PQ_PREFIX}_pq_pivots.bin" "${PQ_PREFIX}_pq_compressed.bin" \
  "${PIPE_PREFIX}_disk.index" <<'PY'
import hashlib, json, os, sys
dataset, root, *paths = sys.argv[1:]
record = {"dataset": dataset, "status": "built-not-yet-admitted", "artifacts": {}}
for value in paths:
    h = hashlib.sha256()
    with open(value, "rb") as source:
        while block := source.read(64 << 20): h.update(block)
    record["artifacts"][os.path.basename(value)] = {
        "bytes": os.path.getsize(value), "sha256": h.hexdigest()
    }
out = os.path.join(root, "host-build-manifest.json")
tmp = out + ".tmp"
with open(tmp, "w") as stream: json.dump(record, stream, indent=2, sort_keys=True); stream.write("\n")
os.replace(tmp, out)
print(json.dumps({"dataset": dataset, "manifest": out, "artifacts": len(paths)}, sort_keys=True))
PY

echo "BUILD_COMPLETE dataset=$DATASET"
