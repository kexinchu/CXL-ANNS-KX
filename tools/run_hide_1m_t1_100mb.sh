#!/usr/bin/env bash
# T=1 e1a1 at 100 MiB vmem cache (≈1/20 of 1.91 GiB 1M image).
# Reloads vmem_sw only (not vmem.ko). Does not write 420/460/900.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
MEM2=/root/chukexin/mem2nvme
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
OUT=$ROOT/results/paper_figs
NAV=$OUT/nav_1m_10k.bin
MAP=$SRV/id_to_slot_1m_cooccur.bin
GRAPH=$SRV/diskann_t2i_1m.graph.bin
OFF=998579896320
LEN=2048917504
NQ=${NQ:-20}
HOST_BYTES=$((2 * 1024 * 1024 * 1024))
WANT_LIMIT=$((100 * 1024 * 1024))

reload_100mb() {
  if fuser /dev/vmem0 >/dev/null 2>&1; then
    echo "FAIL: /dev/vmem0 is open; refuse rmmod" >&2
    fuser -v /dev/vmem0 >&2 || true
    exit 1
  fi
  lsmod | awk '{print $1}' | grep -qx vmem_sw && rmmod vmem_sw
  insmod "$MEM2/host/vmem_sw.ko" \
    nvme_devs=/dev/nvme1n1,/dev/nvme2n1 \
    target_bdfs=0000:d8:00.0,0000:d9:00.0 \
    expected_ssd_sizes_bytes=1920383410176,1920383410176 \
    ram_size_gib=28 stripe_size_mib=2 cache_size_mib=100
  sleep 0.4
  local lim used nback
  lim=$(cat /sys/class/vmem/vmem0/cache_limit)
  used=$(cat /sys/class/vmem/vmem0/cache_used)
  nback=$(cat /sys/class/vmem/vmem0/backing_count)
  echo "reloaded cache_limit=$lim cache_used=$used backing=$nback"
  [[ "$lim" == "$WANT_LIMIT" ]] || { echo "FAIL: want cache_limit=$WANT_LIMIT"; exit 1; }
  [[ "$nback" == "2" ]] || { echo "FAIL: want dual backing"; exit 1; }
  python3 - <<'PY'
import os, mmap, struct
fd = os.open("/dev/vmem0", os.O_RDONLY)
for name, off in (("pagebin420", 450971566080), ("cooccur930", 998579896320)):
    m = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=off)
    mag = struct.unpack_from("<Q", m, 0)[0]
    m.close()
    print(f"magic {name} {hex(mag)} {'ok' if mag==0x314e415843 else 'BAD'}")
    if mag != 0x314e415843:
        raise SystemExit(2)
os.close(fd)
PY
}

run() {
  local tag=$1; shift
  local log=$OUT/hide_1m_t1_100mb_${tag}_nq${NQ}.log
  echo "==== T=1 100MiB $tag $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions) limit=$(cat /sys/class/vmem/vmem0/cache_limit)" | tee -a "$log"
  numactl --cpunodebind=0 --membind=0 "$BIN" \
    --diskann-layout --nav-graph "$NAV" --graph-file "$GRAPH" \
    --vmem-dev /dev/vmem0 --vmem-offset "$OFF" --vmem-len "$LEN" \
    --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
    --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
    --id-map "$SRV/new_to_old_pagebin.bin" --id-slot-map "$MAP" \
    --dram-backend numa --dram-numa 1 \
    --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
    --cpu-affinity --policy P3 --budget $((64 << 20)) \
    --oneshot-fp --beam 400 --k 10 --iters 0 \
    --max-q "$NQ" --shuffle-seed 42 --pin-entry \
    --threads 1 --shared-window --no-score-page \
    --expand-batch 1 --issue-ahead 1 --pipe-w 16 --no-sync-hop \
    "$@" 2>&1 | tee -a "$log"
}

case "${1:-all}" in
  reload) reload_100mb ;;
  *)
    reload_100mb
    echo "==== warmup (discarded) ===="
    run warm --no-score-cache
    echo "==== A0 baseline hide-copy ===="
    run a0_hide --no-score-cache
    echo "==== A1 score-cache ===="
    run a1_cache --score-cache
    ;;
esac
