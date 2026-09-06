#!/usr/bin/env bash
# Shared fail-closed host helpers for detached FlashANNS evaluation services.

EVAL_VMEM_KO=/root/chukexin/mem2nvme/host/vmem_sw.ko
EVAL_VMEM_SRCVERSION=76B6E44368BF753535CD293
EVAL_VMEM_D8=/dev/disk/by-id/nvme-Dell_DC_NVMe_CD8P_E3.S_1.92TB_7EU0A01P0XK1
EVAL_VMEM_D9=/dev/disk/by-id/nvme-Dell_DC_NVMe_CD8P_E3.S_1.92TB_2F50A1360XK3

eval_reset_and_restore() {
  local repo_root=$1
  local dataset=$2
  local evidence=$3
  local cache_gib=${4:-4}

  [[ ! -e "$evidence" ]]
  [[ -z "$(fuser /dev/vmem0 2>/dev/null || true)" ]]
  if lsmod | awk '{print $1}' | grep -qx vmem_sw; then
    [[ "$(cat /sys/module/vmem_sw/srcversion)" == "$EVAL_VMEM_SRCVERSION" ]]
    rmmod vmem_sw
  fi
  insmod "$EVAL_VMEM_KO" \
    nvme_devs="$EVAL_VMEM_D8,$EVAL_VMEM_D9" \
    target_bdfs=0000:d8:00.0,0000:d9:00.0 \
    expected_ssd_sizes_bytes=1920383410176,1920383410176 \
    ram_size_gib=28 cache_size_gib="$cache_gib" stripe_size_mib=2
  python3 -m experiments.eval.flashanns.restore_volatile \
    --dataset "$dataset" --write --out "$evidence"
}

eval_wait_for_accepted_count() {
  local prerequisite_unit=$1
  local accepted_root=$2
  local expected=$3
  local observed state
  mkdir -p "$accepted_root"
  while true; do
    observed=$(find "$accepted_root" -name run.json -type f 2>/dev/null | wc -l)
    if [[ "$observed" == "$expected" ]]; then
      return
    fi
    state=$(systemctl is-active "$prerequisite_unit" 2>/dev/null || true)
    if [[ "$state" != "active" && "$state" != "activating" && "$state" != "deactivating" ]]; then
      echo "REFUSE ended prerequisite $prerequisite_unit state=$state: accepted=$observed expected=$expected" >&2
      return 2
    fi
    sleep 30
  done
}
