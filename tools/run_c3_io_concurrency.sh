#!/usr/bin/env bash
# C3 Option A: QD_eff vs concurrent queries on frozen PQ-64 end-batch.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
BIN=$ROOT/serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
NAV=$ROOT/results/paper_figs/nav_10k.bin
GRAPH=$SRV/diskann_t2i_10m.graph.bin
PQ_PIV=${PQ_PIV:-/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin}
PQ_CMP=${PQ_CMP:-/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin}
# This host is a single 32 GiB NUMA node; MAP_SHARED anon 2 GiB fails here.
HOST_BYTES=${HOST_BYTES:-$((512 * 1024 * 1024))}
LEN=20480004096
NQ=${NQ:-20}
SMOKE=0
if [[ "${1:-}" == "--smoke" ]]; then SMOKE=1; shift || true; fi
STAMP=$(date +%Y%m%d_%H%M%S)
OUT=${OUT:-$ROOT/results/motivation/c3_io_concurrency/raw/$STAMP}
mkdir -p "$OUT"

nvme_names() {
  if [[ -r /sys/class/vmem/vmem0/nvme_dev ]]; then
    tr ', \t' '\n' < /sys/class/vmem/vmem0/nvme_dev | while read -r p; do
      [[ -z "$p" ]] && continue
      readlink -f "$p"
    done
  else
    echo /dev/nvme3n1
    echo /dev/nvme4n1
  fi
}

pollute() {
  # cache_limit is 4 GiB and read-only. The freeze host had ~100 MiB used, so a
  # 200 MiB touch at 420 GiB evicted the 1100 GiB working set. Here the cache is
  # full; touch > cache_limit at 420 GiB so timed fills actually miss to Flash.
  python3 - <<'PY'
import os, mmap
fd = os.open("/dev/vmem0", os.O_RDONLY)
off0 = 450971566080  # 420 GiB, 2 MiB aligned
need = 5 << 30
chunk = 512 << 20
align = 2 << 20
acc = 0
off = off0
end = off0 + need
while off < end:
    aoff = off & ~(align - 1)
    delta = off - aoff
    ln = min(chunk, end - off)
    m = mmap.mmap(fd, delta + ln, mmap.MAP_SHARED, mmap.PROT_READ, offset=aoff)
    for i in range(delta, delta + ln, 4096):
        acc += m[i]
    m.close()
    off += ln
os.close(fd)
used = open("/sys/class/vmem/vmem0/cache_used").read().strip()
ev = open("/sys/class/vmem/vmem0/evictions").read().strip()
print(f"pollute checksum={acc} evict={ev} cache_used={used} bytes={need}", flush=True)
PY
}

mapfile -t NVME_DEVS < <(nvme_names)
echo "nvme=${NVME_DEVS[*]}" | tee "$OUT/setup.txt"
{
  echo "git=$(git -C "$ROOT" rev-parse --short HEAD)"
  echo "nq=$NQ smoke=$SMOKE"
  echo "bin=$(sha256sum "$BIN" | awk '{print $1}')"
} | tee -a "$OUT/setup.txt"

# fio ceiling on first backing NVMe (4k randread). QDsat = mean aqu-sz while fio runs.
FIO_DEV=${NVME_DEVS[0]}
if [[ "${SKIP_FIO:-0}" == 1 && -s "${FIO_FROM:-}/fio.iostat.log" ]]; then
  cp -a "$FIO_FROM"/fio.iostat.log "$FIO_FROM"/fio_job.log "$FIO_FROM"/fio.log "$OUT/" 2>/dev/null || true
  echo "==== reused fio from $FIO_FROM ====" | tee "$OUT/fio.log"
else
echo "==== fio ceiling $FIO_DEV ====" | tee "$OUT/fio.log"
stdbuf -oL iostat -x 1 "${FIO_DEV##*/}" > "$OUT/fio.iostat.log" &
IOPID=$!
fio --name=qdsat --filename="$FIO_DEV" --readonly --direct=1 --ioengine=io_uring \
    --rw=randread --bs=4k --iodepth=128 --numjobs=8 --time_based --runtime=20 \
    --group_reporting --output="$OUT/fio_job.log" | tee -a "$OUT/fio.log"
kill "$IOPID" 2>/dev/null || true
wait "$IOPID" 2>/dev/null || true
fi

pq_flags() { echo --pq-nav --pq-pivots "$PQ_PIV" --pq-compressed "$PQ_CMP"; }

run_one() {
  local tag=$1
  shift
  local log=$OUT/${tag}.log
  echo "==== $tag nq=$NQ $(date -Is) ====" | tee "$log"
  echo "cache_used=$(cat /sys/class/vmem/vmem0/cache_used) evictions=$(cat /sys/class/vmem/vmem0/evictions)" | tee -a "$log"
  pollute | tee -a "$log"
  local iodevs=()
  local d
  for d in "${NVME_DEVS[@]}"; do iodevs+=("${d##*/}"); done
  # sysstat 12.7 here ignores a 0.1s interval (one since-boot line only).
  python3 - "${iodevs[@]}" > "$OUT/${tag}.iostat.log" <<'PY' &
import sys, time
devs = sys.argv[1:]
interval = 0.1
def rd(dev):
    f = open(f"/sys/block/{dev}/stat")
    p = f.read().split(); f.close()
    return int(p[2]), int(p[10])  # rsect, time_in_queue_ms
prev = {d: rd(d) for d in devs}
t0 = time.monotonic()
print("Device r/s rkB/s aqu-sz %util", flush=True)
while True:
    time.sleep(interval)
    t1 = time.monotonic()
    dt = t1 - t0
    if dt <= 0:
        continue
    for d in devs:
        rsect, tiq = rd(d)
        pr, ptiq = prev[d]
        rkbs = (rsect - pr) * 512.0 / 1024.0 / dt
        aqu = (tiq - ptiq) / 1000.0 / dt
        print(f"{d} 0 {rkbs:.2f} 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 {aqu:.2f} 0.00", flush=True)
        prev[d] = (rsect, tiq)
    t0 = t1
PY
  local ip=$!
  local rc=1
  local _try
  for _try in 1 2; do
    set +e
    # Single-node 32 GiB host: process-wide membind races CommitLimit.
    # Window is still mbind(node 0) inside map_dram_numa.
    numactl --cpunodebind=0 "$BIN" \
      --diskann-layout --nav-graph "$NAV" --graph-file "$GRAPH" \
      --vmem-dev /dev/vmem0 --vmem-len "$LEN" \
      --vmem-offset 1181116006400 --id-slot-map "$SRV/id_to_slot_10m_extent.bin" \
      --entry "$SRV/serving_entry_t2i_10m_pagebin.bin" \
      --queries "$SRV/query_10k.fbin" --gt "$SRV/gt_10k_k10.ibin" \
      --id-map "$SRV/new_to_old_pagebin.bin" \
      --dram-backend numa --dram-numa "${DRAM_NUMA:-0}" \
      --dram-bytes "$HOST_BYTES" --host-bytes "$HOST_BYTES" \
      --cpu-affinity --policy P3 --budget $((64 << 20)) \
      --beam 400 --k 10 --iters 0 \
      --max-q "$NQ" --shuffle-seed 42 --pin-entry \
      --shared-window --pipe-w 16 --no-sync-hop \
      --no-score-cache --no-hide-warm-entry --no-direct-install \
      --expand-batch 8 --issue-ahead 1 --extent-run --no-pipe-drive \
      --io-trace "$OUT/${tag}.io" \
      $(pq_flags) \
      "$@" \
      2>&1 | tee -a "$log"
    rc=${PIPESTATUS[0]}
    set -e
    if [[ "$rc" -eq 0 ]]; then
      break
    fi
    echo "WARN $tag attempt $_try rc=$rc" | tee -a "$log"
    sleep 2
  done
  sleep 0.4
  kill "$ip" 2>/dev/null || true
  wait "$ip" 2>/dev/null || true
  [[ "$rc" -eq 0 ]]
}

if [[ "$SMOKE" == 1 ]]; then
  run_one T1 --threads 1
  run_one T16 --cont-batch 16 --cont-workers 16
else
  run_one T1 --threads 1
  run_one T2 --cont-batch 2 --cont-workers 2
  run_one T4 --cont-batch 4 --cont-workers 4
  run_one T8 --cont-batch 8 --cont-workers 8
  run_one T16 --cont-batch 16 --cont-workers 16
fi

python3 - <<PY
import json, os, re, statistics
from pathlib import Path
out = Path("$OUT")
devs = [p.split("/")[-1] for p in """${NVME_DEVS[*]}""".split() if p]
primary = devs[0] if devs else None

def parse_iostat(path, want=None):
    # sysstat prints a since-boot block first (has avg-cpu). The python
    # 0.1s sidecar does not.
    txt = path.read_text(errors="ignore")
    skip_boot = "avg-cpu" in txt
    rows = []
    block = -1
    for line in txt.splitlines():
        if line.startswith("Device"):
            block += 1
            continue
        if skip_boot and block < 1:
            continue
        p = line.split()
        if len(p) < 2:
            continue
        if want and p[0] != want:
            continue
        try:
            rkbs = float(p[2]); aqu = float(p[-2]); util = float(p[-1])
        except Exception:
            continue
        rows.append((rkbs/1e6, aqu, util))
    return rows

def active(rows, min_gbs=0.1):
    hit = [r for r in rows if r[0] > min_gbs]
    if len(hit) >= 8:
        return hit, min_gbs
    # This host's 4 GiB vmem cache keeps NVMe well under 100 MB/s.
    hit = [r for r in rows if r[0] > 0.001]
    return hit, 0.001

fio_rows, _fio_thr = active(parse_iostat(out/"fio.iostat.log", want=primary), min_gbs=0.1)
qdsat = statistics.mean([r[1] for r in fio_rows]) if fio_rows else None
qdsat_gbs = statistics.mean([r[0] for r in fio_rows]) if fio_rows else None

summary = []
for tag in ["T1","T2","T4","T8","T16"]:
    log = out/f"{tag}.log"
    if not log.exists():
        continue
    txt = log.read_text(errors="ignore")
    rec = re.search(r"recall@10=([0-9.]+)", txt)
    qps = re.search(r"throughput_QPS=([0-9.]+)", txt)
    fills_p = out/f"{tag}.io.csv"
    miss = issue = cand = None
    nfill = 0
    if fills_p.exists():
        ms, iss, cs = [], [], []
        for line in fills_p.read_text().splitlines()[1:]:
            if not line.startswith("fill,"):
                continue
            f = line.split(",")
            cs.append(int(f[3])); ms.append(int(f[5])); iss.append(int(f[6]))
        nfill = len(ms)
        if ms:
            cand = statistics.mean(cs); miss = statistics.mean(ms); issue = statistics.mean(iss)
    ior, ath = active(parse_iostat(out/f"{tag}.iostat.log", want=primary))
    qd = statistics.mean([r[1] for r in ior]) if ior else None
    peak = max([r[1] for r in ior]) if ior else None
    gbs = statistics.mean([r[0] for r in ior]) if ior else None
    row = {
        "tag": tag, "nq": int("$NQ"),
        "recall": float(rec.group(1)) if rec else None,
        "qps": float(qps.group(1)) if qps else None,
        "nfill": nfill, "mean_cand": cand, "mean_miss": miss, "mean_issue": issue,
        "QD_eff": qd, "peak_aqu": peak, "mean_GBs": gbs,
        "QDsat": qdsat, "QDsat_gbs": qdsat_gbs, "active_iostat": len(ior),
        "active_min_GBs": ath,
    }
    summary.append(row)
    print(row)

(out/"summary.jsonl").write_text("".join(json.dumps(r)+"\n" for r in summary))
print("wrote", out/"summary.jsonl")
PY

echo "OUT=$OUT"
