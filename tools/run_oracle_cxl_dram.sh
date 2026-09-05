#!/usr/bin/env bash
# CXL-DRAM DiskANN Oracle: corpus on /dev/dax0.0, host keeps only nav_10k.bin.
# Usage: run_oracle_cxl_dram.sh [threads...]   default: 1 4 8 16 32
# Does not replace host-window 85.8. Does not mmap FPGA BAR / vmem.ko.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
DAX=${DAX:-/dev/dax0.0}
NAV=${NAV:-$ROOT/results/paper_figs/nav_10k.bin}
HOST_IMG=${HOST_IMG:-$SRV/diskann_t2i_10m.bin}
# Prefer dax if it has a version-2 header; else host file (MAP_POPULATE).
IMAGE=$HOST_IMG
if python3 -c "import os,mmap,struct,sys
fd=os.open('$DAX',os.O_RDONLY)
m=mmap.mmap(fd,2<<20,mmap.MAP_SHARED,mmap.PROT_READ,offset=0)
magic,ver,n=struct.unpack_from('<QII',m,0)
sys.exit(0 if magic==0x314e415843 and ver>=2 and n==10000000 else 1)" 2>/dev/null; then
  IMAGE=$DAX
fi
echo "oracle image=$IMAGE"
NQ=${NQ:-20}

mkdir -p "$OUT"
g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I"$ROOT" \
  "$ROOT/serving/search_beam.cpp" -o "$BIN" -lnuma

THREADS=("$@")
if [[ ${#THREADS[@]} -eq 0 ]]; then
  THREADS=(1 4 8 16 32)
fi

for T in "${THREADS[@]}"; do
  log=$OUT/oracle_cxl_dram_T${T}_nq${NQ}.log
  echo "==== Oracle CXL-DRAM DiskANN T=$T nq=$NQ ====" | tee "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout --oracle-dram --dram-backend dax \
    --cxl-dram-dev "$DAX" --image "$IMAGE" \
    --nav-graph "$NAV" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" \
    --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --max-q "$NQ" --shuffle-seed 42 --cpu-affinity \
    --threads "$T" --policy P0 \
    2>&1 | tee -a "$log"
done
