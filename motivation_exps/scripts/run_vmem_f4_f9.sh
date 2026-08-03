#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
make -j
DIR="${DIR:-/mnt/disk0/chukexin_motivation/data/laion1m_200k}"
LOG="$ROOT/results/vmem_f4_f9_$(date +%Y%m%d_%H%M%S).log"
NQ="${NQ:-16}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-8}"
{
  echo "=== vmem-identity ==="
  ./motivation vmem-identity
  echo "=== vmem-f4 ==="
  ./motivation vmem-f4 --dir "$DIR" --nq "$NQ" --hops 32
  echo "=== vmem-f5 ==="
  ./motivation vmem-f5 --dir "$DIR" --nq "$NQ" --beam 32 --hops 32
  echo "=== vmem-f6 ==="
  ./motivation vmem-f6 --dir "$DIR" --nq "$NQ" --beam 32 --hops 32 --cache-mb 64
  echo "=== vmem-f7 ==="
  ./motivation vmem-f7 --dir "$DIR" --nq "$NQ" --hops 32 --cache-mb 64
  echo "=== vmem-f8 ==="
  ./motivation vmem-f8 --dir "$DIR" --iters 800
  echo "=== vmem-f9 ==="
  ./motivation vmem-f9 --dir "$DIR" --nq "$NQ" --hops 32
} 2>&1 | tee "$LOG"
echo "wrote $LOG"
