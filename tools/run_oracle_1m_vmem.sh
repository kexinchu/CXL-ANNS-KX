#!/usr/bin/env bash
# 1M DiskANN Oracle on /dev/vmem0 after the image is in the 4 GiB page cache.
# Host keeps only nav_1m_10k.bin. Does not replace 10M claim rows.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=${NAV:-$OUT/nav_1m_10k.bin}
VMEM_OFF=${VMEM_OFF:-966367641600}
NQ=${NQ:-20}
# 1_000_000 * 2048 + 4K, rounded up to 2 MiB
VMEM_LEN=${VMEM_LEN:-2048917504}

mkdir -p "$OUT"
g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I"$ROOT" \
  "$ROOT/serving/search_beam.cpp" -o "$BIN" -lnuma

echo -n "cache_used_before="; cat /sys/class/vmem/vmem0/cache_used
echo -n "nvme_devs="; cat /sys/class/vmem/vmem0/nvme_devs

THREADS=("$@")
if [[ ${#THREADS[@]} -eq 0 ]]; then
  THREADS=(1 4 8 16 32)
fi

for T in "${THREADS[@]}"; do
  log=$OUT/oracle_1m_vmem_T${T}_nq${NQ}.log
  echo "==== Oracle 1M vmem-cache T=$T nq=$NQ $(date -Is) ====" | tee "$log"
  echo -n "cache_used="; cat /sys/class/vmem/vmem0/cache_used | tee -a "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout --oracle-dram --dram-backend dax \
    --vmem-dev /dev/vmem0 --vmem-offset "$VMEM_OFF" --vmem-len "$VMEM_LEN" \
    --nav-graph "$NAV" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" \
    --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --max-q "$NQ" --shuffle-seed 42 --cpu-affinity \
    --threads "$T" --policy P0 \
    2>&1 | tee -a "$log"
  echo -n "cache_used_after_T$T="; cat /sys/class/vmem/vmem0/cache_used
done
