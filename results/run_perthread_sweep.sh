#!/usr/bin/env bash
set -euo pipefail
source /mnt/disk0/chukexin_motivation/serving_t2i_10m/serve_vmem_pagebin.env
BIN=/root/chukexin/CXL-ANNS-KX/serving/search_beam
OUT=/root/chukexin/CXL-ANNS-KX/results/t2i10m_perthread_sweep.log
MON=/tmp/diskmon_perthread_sweep.csv
: > "$OUT"

python3 /mnt/disk0/chukexin_motivation/pipeann_t2i10m/diskmon.py nvme1n1 300 0.1 "$MON" \
  > /tmp/diskmon_perthread_sweep.summary &
MONPID=$!
echo "LOG=$OUT MONPID=$MONPID" | tee -a "$OUT"

run_one() {
  local tag="$1"; shift
  echo "========== $tag $(date -Iseconds) ==========" | tee -a "$OUT"
  echo "CMD: $*" | tee -a "$OUT"
  /usr/bin/time -f 'elapsed_s=%e maxrss_kb=%M' "$BIN" "$@" 2>&1 | tee -a "$OUT"
  echo | tee -a "$OUT"
}

COMMON=(
  --policy P3 --pipe-w 4 --install-top 4 --budget $((64<<20))
  --dram-backend numa --oneshot-fp --beam 300 --k 10 --iters 0
  --shuffle-seed 42 --host-cap $((64<<20)) --pin-entry
  --vmem-dev "$CXAN_VMEM_DEV" --vmem-offset "$CXAN_SSD_OFFSET" --vmem-len "$CXAN_LAYOUT_LEN"
  --entry "$CXAN_ENTRY" --queries "$CXAN_QUERIES" --gt "$CXAN_GT" --id-map "$CXAN_ID_MAP"
)

run_one "T1_1GiB_nq50" "${COMMON[@]}" --dram-bytes 1073741824 --max-q 50 --threads 1

for t in 1 2 4 8 16 32; do
  run_one "T${t}_256MiB_nq200" "${COMMON[@]}" --dram-bytes 268435456 --max-q 200 --threads "$t" --per-thread-window
done

wait "$MONPID" || true
echo "========== diskmon ==========" | tee -a "$OUT"
cat /tmp/diskmon_perthread_sweep.summary | tee -a "$OUT"
echo "DONE $OUT"
