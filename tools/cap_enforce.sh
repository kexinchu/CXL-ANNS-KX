#!/usr/bin/env bash
# Enforce soft quotas for CXL-ANNS serving experiments.
# Usage: source tools/cap_enforce.sh   OR   tools/cap_enforce.sh env
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CXAN_ROOT="$ROOT"
export CXAN_DRAM_BYTES=$((1 * 1024 * 1024 * 1024))          # 1 GiB CXL-DRAM window
export CXAN_SSD_BYTES=$((100 * 1024 * 1024 * 1024))         # 100 GiB image
export CXAN_SSD_OFFSET=$((200 * 1024 * 1024 * 1024))        # avoid motivation@32GiB
export CXAN_VMEM_DEV="${CXAN_VMEM_DEV:-/dev/vmem0}"
# CXL-DRAM window: prefer devdax (true Type-3). Avoid anon+mbind(node1)=local DRAM.
export CXAN_DRAM_BACKEND="${CXAN_DRAM_BACKEND:-dax}"
export CXAN_DAX_DEV="${CXAN_DAX_DEV:-/dev/dax0.0}"
export CXAN_DAX_OFFSET="${CXAN_DAX_OFFSET:-0}"
export CXAN_DRAM_NODE="${CXAN_DRAM_NODE:-1}"  # only for --dram-backend numa fallback

# vmem soft-cache is a *separate* host DRAM cache (module cache_size_gib, often 4).
# Paper numbers must cite the software 1GiB window; soft-cache is an upper-bound confound.
export CXAN_SOFT_CACHE_NOTE="vmem_sw cache_size_gib=$(cat /sys/module/vmem_sw/parameters/cache_size_gib 2>/dev/null || echo '?')"

if [[ "${1:-}" == "print" || "${1:-}" == "env" ]]; then
  echo "CXAN_ROOT=$CXAN_ROOT"
  echo "CXAN_DRAM_BYTES=$CXAN_DRAM_BYTES"
  echo "CXAN_DRAM_BACKEND=$CXAN_DRAM_BACKEND"
  echo "CXAN_DAX_DEV=$CXAN_DAX_DEV"
  echo "CXAN_DAX_OFFSET=$CXAN_DAX_OFFSET"
  echo "CXAN_DRAM_NODE=$CXAN_DRAM_NODE"
  echo "CXAN_SSD_BYTES=$CXAN_SSD_BYTES"
  echo "CXAN_SSD_OFFSET=$CXAN_SSD_OFFSET"
  echo "CXAN_VMEM_DEV=$CXAN_VMEM_DEV"
  echo "CXAN_SOFT_CACHE_NOTE=$CXAN_SOFT_CACHE_NOTE"
fi
