#!/usr/bin/env bash
# T=1 hide on DiskANN image staged at a NEW vmem offset (never 420/460 GiB).
# Corpus must already be on /dev/dax0.0; this copies it to CXL-SSD then searches.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
DAX=${DAX:-/dev/dax0.0}
NAV=${NAV:-$OUT/nav_10k.bin}
NQ=${NQ:-20}
# 800 GiB — past pagebin (420) and expand-bundle (~429+267).
HIDE_OFF=${HIDE_OFF:-858993459200}
HIDE_LEN=$((4096 + 10000000 * 2048))
HOST_BYTES=$((2 * 1024 * 1024 * 1024))

if [[ "$HIDE_OFF" -eq 450971566080 || "$HIDE_OFF" -eq 460571635712 ]]; then
  echo "FAIL: refuse pagebin/bundle offsets"
  exit 1
fi

mkdir -p "$OUT"
echo "copy dax corpus -> /dev/vmem0 off=$HIDE_OFF len=$HIDE_LEN"
python3 - "$DAX" /dev/vmem0 "$HIDE_OFF" "$HIDE_LEN" <<'PY'
import os, sys, mmap
src, dst, off, ln = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
chunk = 8 << 20
sfd = os.open(src, os.O_RDONLY)
dfd = os.open(dst, os.O_RDWR)
sm = mmap.mmap(sfd, ln, mmap.MAP_SHARED, mmap.PROT_READ, offset=0)
written = 0
while written < ln:
    n = min(chunk, ln - written)
    os.pwrite(dfd, sm[written:written+n], off + written)
    written += n
    if written % (1 << 30) == 0:
        print(f"  copied {written >> 30} GiB", flush=True)
sm.close()
os.close(sfd)
os.close(dfd)
print("copy done")
PY

log=$OUT/hide_diskann_T1_nq${NQ}.log
echo "==== Hide DiskANN T=1 nq=$NQ off=$HIDE_OFF ====" | tee "$log"
numactl --cpunodebind=0 --membind=0 "$BIN" \
  --diskann-layout --dram-backend numa --dram-numa 1 \
  --nav-graph "$NAV" \
  --vmem-dev /dev/vmem0 --vmem-offset "$HIDE_OFF" --vmem-len "$HIDE_LEN" \
  --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
  --queries "$SRV/query_10k.fbin" \
  --gt "$SRV/gt_10k_k10.ibin" \
  --id-map "$SRV/new_to_old_pagebin.bin" \
  --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
  --shared-window --cpu-affinity --threads 1 \
  --policy P3 --no-score-page --expand-batch 8 --issue-ahead 2 \
  --oneshot-fp --beam 400 --k 10 --iters 0 \
  --max-q "$NQ" --shuffle-seed 42 --pin-entry \
  2>&1 | tee -a "$log"
