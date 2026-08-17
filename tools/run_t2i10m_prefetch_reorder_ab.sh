#!/usr/bin/env bash
# T2I-10M: P0 vs P3 (prefetch) vs P3+pagebin A (prefetch+reorder). No page-group B.
set -euo pipefail
ROOT=/root/chukexin/CXL-ANNS-KX
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
RES="$ROOT/results"
source "$ROOT/tools/cap_enforce.sh"
BIN="$ROOT/serving/search_beam"
CSV="$RES/oneshot_fp_t2i10m_prefetch_reorder.csv"
LOG="$RES/oneshot_fp_t2i10m_prefetch_reorder.log"
mkdir -p "$RES"
echo "tag,pass,policy,qps,mean_ms,recall,hit_pct,dram_hits,ssd_misses,prefetch_pages,dist,vec_stride" > "$CSV"
: > "$LOG"

L=${L:-200}
MAX_Q=${MAX_Q:-100}
BUDGET=$((64<<20))

run() {
  local tag=$1 envf=$2 policy=$3 pass=$4
  set +u; source "$envf"; set -u
  local extra=()
  if [[ -n "${CXAN_ID_MAP:-}" ]]; then extra+=(--id-map "$CXAN_ID_MAP"); fi
  local pol_args=(--policy "$policy")
  if [[ "$policy" == P3 ]]; then
    pol_args+=(--budget "$BUDGET" --pipe-w 4 --install-top 4 --fetch-top 0 --no-page-group)
  else
    pol_args+=(--budget 0)
  fi
  echo "=== $(date -Is) $tag $pass ===" | tee -a "$LOG"
  set +e
  out=$(numactl --cpunodebind=0 --membind=0 "$BIN" \
    --vmem-dev "$CXAN_VMEM_DEV" --vmem-offset "$CXAN_SSD_OFFSET" --vmem-len "$CXAN_LAYOUT_LEN" \
    --entry "$CXAN_ENTRY" --queries "$CXAN_QUERIES" --gt "$CXAN_GT" \
    "${extra[@]}" "${pol_args[@]}" \
    --dram-bytes $((1<<30)) --host-cap $((32<<20)) \
    --dram-backend dax --dax-dev /dev/dax0.0 --dax-offset 0 \
    --beam "$L" --k 10 --iters 0 --oneshot-fp --pin-entry \
    --shuffle-seed 42 --max-q "$MAX_Q" 2>&1)
  set -e
  echo "$out" | tee -a "$LOG" | grep -E 'vec_stride|page_group|throughput|hit_pct|recall|prefetch_pages| dist='
  qps=$(echo "$out" | grep -oP 'throughput_QPS=\K[0-9.]+' | tail -1)
  mean=$(echo "$out" | grep -oP 'latency_ms mean=\K[0-9.]+' | tail -1)
  rec=$(echo "$out" | grep -oP 'recall@10=\K[0-9.]+' | tail -1)
  hit=$(echo "$out" | grep -oP 'cxl_dram_hit_pct=\K[0-9.]+' | tail -1)
  dh=$(echo "$out" | grep -oP 'dram_hits=\K[0-9]+' | tail -1)
  sm=$(echo "$out" | grep -oP 'ssd_misses=\K[0-9]+' | tail -1)
  pp=$(echo "$out" | grep -oP 'prefetch_pages=\K[0-9]+' | tail -1)
  dist=$(echo "$out" | grep -oP ' dist=\K[0-9]+' | tail -1)
  vs=$(echo "$out" | grep -oP 'vec_stride=\K[0-9]+' | head -1)
  echo "$tag,$pass,$policy,$qps,$mean,$rec,$hit,$dh,$sm,$pp,$dist,$vs" | tee -a "$CSV"
}

# Find L for packed P0 recall>=0.9 if needed — fixed L for now; adjust after smoke
echo "==== packed P0 x3 ====" | tee -a "$LOG"
unset CXAN_ID_MAP || true
for p in warm1 warm2 warm3; do run packed_P0 "$SRV/serve_vmem.env" P0 "$p"; done
echo "==== packed P3 prefetch x3 ====" | tee -a "$LOG"
for p in warm1 warm2 warm3; do run packed_P3 "$SRV/serve_vmem.env" P3 "$p"; done
echo "==== pagebin A + P3 x3 ====" | tee -a "$LOG"
for p in warm1 warm2 warm3; do run pagebin_A_P3 "$SRV/serve_vmem_pagebin.env" P3 "$p"; done
echo "DONE $(date -Is)" | tee -a "$LOG"
column -t -s, "$CSV"
