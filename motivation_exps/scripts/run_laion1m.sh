#!/usr/bin/env bash
# Prepare LAION-1M dataset + run F1/F2/F3 (+ optional H0) on real embeddings.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
make -j

RAW_NPY="${RAW_NPY:-/mnt/disk0/chukexin_motivation/data/laion1m_raw/img_emb/img_emb_000.npy}"
OUT_DIR="${OUT_DIR:-/mnt/disk0/chukexin_motivation/data/laion1m}"
LOG_DIR="$ROOT/results"
mkdir -p "$OUT_DIR" "$LOG_DIR"
OUT="$LOG_DIR/laion1m_$(date +%Y%m%d_%H%M%S).log"

CXL_NODE="${CXL_NODE:-}"
if [[ -z "$CXL_NODE" ]]; then
  CXL_NODE=$(python3 - <<'PY'
import os
for n in sorted(os.listdir("/sys/devices/system/node")):
    if not n.startswith("node"): continue
    path=f"/sys/devices/system/node/{n}"
    cpus=open(f"{path}/cpulist").read().strip()
    if cpus=="":
        print(n.replace("node","")); break
else:
    print("1")
PY
)
fi
echo "CXL_NODE=$CXL_NODE"

# Build dataset once if missing
if [[ ! -f "$OUT_DIR/meta.bin" || ! -f "$OUT_DIR/vectors.bin" || ! -f "$OUT_DIR/graph.bin" ]]; then
  echo "=== prepare LAION-1M ==="
  # Use all ~984k vectors; degree 32
  python3 scripts/prepare_laion1m.py --npy "$RAW_NPY" --out "$OUT_DIR" --degree 32
fi

CACHE_MB="${CACHE_MB:-64}"   # 64MB CXL cache; vectors are 4KB each → ~16k vecs
NQ="${NQ:-80}"
BEAM="${BEAM:-32}"
HOPS="${HOPS:-64}"
# Bound OpenMP for batch_promote: unlimited threads + O_DIRECT random I/O collapses.
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-8}"

{
  echo "=== topology ==="
  numactl -H || true
  echo "=== meta ==="
  python3 - <<PY
import struct
m=open("$OUT_DIR/meta.bin","rb").read()
print(struct.unpack("IIII", m))
import os
print("vectors_GB", os.path.getsize("$OUT_DIR/vectors.bin")/1e9)
print("graph_MB", os.path.getsize("$OUT_DIR/graph.bin")/1e6)
PY

  echo "=== H0 latency ratio (CXL membind=$CXL_NODE) ==="
  numactl --cpunodebind=0 --membind=$CXL_NODE \
    python3 scripts/h0_latency_ratio.py --vectors "$OUT_DIR/vectors.bin" --dim 1024 --iters 3000 --cxl-mb 256 || true

  echo "=== F1 cache_mb=$CACHE_MB ==="
  numactl --cpunodebind=0 --membind=0 \
    ./motivation f1 --dir "$OUT_DIR" --nq "$NQ" --beam "$BEAM" --hops "$HOPS" --cache-mb "$CACHE_MB" --node "$CXL_NODE"

  echo "=== F2 ==="
  numactl --cpunodebind=0 --membind=0 \
    ./motivation f2 --dir "$OUT_DIR" --nq "$NQ" --beam "$BEAM" --hops "$HOPS" --cache-mb "$CACHE_MB" --node "$CXL_NODE"

  echo "=== F3 ==="
  numactl --cpunodebind=0 --membind=0 \
    ./motivation f3 --dir "$OUT_DIR" --nq "$NQ" --hops "$HOPS" --cache-mb "$CACHE_MB" --node "$CXL_NODE"
} 2>&1 | tee "$OUT"

echo "wrote $OUT"
