#!/usr/bin/env bash
# Run motivation F1/F2/F3 on /dev/vmem0 (assumes already populated).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
make -j

DIR="${DIR:-/mnt/disk0/chukexin_motivation/data/laion1m_200k}"
LOG="$ROOT/results/vmem_f123_$(date +%Y%m%d_%H%M%S).log"
NQ="${NQ:-30}"
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-8}"

{
  echo "=== vmem-identity ==="
  ./motivation vmem-identity

  echo "=== vmem-f1 (graph-aware) nq=$NQ ==="
  ./motivation vmem-f1 --dir "$DIR" --nq "$NQ" --beam 32 --hops 32

  echo "=== vmem-f2 record vs page16k ==="
  ./motivation vmem-f2 --dir "$DIR" --nq "$NQ" --beam 32 --hops 32 --cache-mb 64

  echo "=== vmem-f3 beam x hit ==="
  ./motivation vmem-f3 --dir "$DIR" --nq 20 --hops 32
} 2>&1 | tee "$LOG"

echo "wrote $LOG"
