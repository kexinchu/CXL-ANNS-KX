#!/usr/bin/env bash
# Run motivation on /dev/vmem0 unified address space (+ Montage CXL identity).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
make -j

DIR="${DIR:-/mnt/disk0/chukexin_motivation/data/laion1m}"
LOG="$ROOT/results/vmem_laion_$(date +%Y%m%d_%H%M%S).log"
MAX_N="${MAX_N:-200000}"   # populate subset first; full = 984133
NQ="${NQ:-40}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-8}"

{
  echo "=== vmem-identity ==="
  ./motivation vmem-identity

  echo "=== vmem-populate max_n=$MAX_N ==="
  ./motivation vmem-populate --dir "$DIR" --max-n "$MAX_N"

  echo "=== vmem-h0 ==="
  ./motivation vmem-h0 --dir "$DIR" --iters 1500

  echo "=== vmem-f1 ==="
  # Graph must match populated n: if subset, rebuild temp graph meta? 
  # For subset populate, header.n=MAX_N but graph still full — require matching.
  # So either full populate or regenerate subset dataset.
  ./motivation vmem-f1 --dir "$DIR" --nq "$NQ" --beam 32 --hops 48
} 2>&1 | tee "$LOG"

echo "wrote $LOG"
