#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
make -j

DIR="${DIR:-$ROOT/data/motivation_v2}"
mkdir -p "$DIR" "$ROOT/results"
OUT="$ROOT/results/motivation_$(date +%Y%m%d_%H%M%S).log"

# Detect CXL-like memory node: first memory-only NUMA node, else 1.
CXL_NODE="${CXL_NODE:-}"
if [[ -z "$CXL_NODE" ]]; then
  CXL_NODE=$(python3 - <<'PY'
import os
for n in sorted(os.listdir("/sys/devices/system/node")):
    if not n.startswith("node"): continue
    path=f"/sys/devices/system/node/{n}"
    cpus=open(f"{path}/cpulist").read().strip()
    mem=open(f"{path}/meminfo").read()
    if cpus=="" and "MemTotal" in mem:
        print(n.replace("node",""))
        break
else:
    print("1")
PY
)
fi
echo "Using CXL_NODE=$CXL_NODE"

# Small cache to force misses (Motivation regime).
CACHE_MB="${CACHE_MB:-8}"
N="${N:-400000}"
NQ="${NQ:-120}"

export OMP_NUM_THREADS="${OMP_NUM_THREADS:-8}"

{
  echo "=== topology ==="
  numactl -H || true
  echo "=== gen ==="
  ./motivation gen --dir "$DIR" --n "$N" --dim 128 --degree 32

  echo "=== F1 cache_mb=$CACHE_MB ==="
  numactl --cpunodebind=0 --membind=0 \
    ./motivation f1 --dir "$DIR" --nq "$NQ" --beam 32 --hops 64 --cache-mb "$CACHE_MB" --node "$CXL_NODE"

  echo "=== F2 ==="
  numactl --cpunodebind=0 --membind=0 \
    ./motivation f2 --dir "$DIR" --nq "$NQ" --beam 32 --hops 64 --cache-mb "$CACHE_MB" --node "$CXL_NODE"

  echo "=== F3 ==="
  numactl --cpunodebind=0 --membind=0 \
    ./motivation f3 --dir "$DIR" --nq "$NQ" --hops 64 --cache-mb "$CACHE_MB" --node "$CXL_NODE"
} 2>&1 | tee "$OUT"

echo "wrote $OUT"
