#!/usr/bin/env bash
# Wait without load; build YFCC only after the complete T2I campaign is sealed.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
UNIT=flashanns-eval-t2i-68c6.service
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/68c6

source "$ROOT/tools/eval_host_cold_lib.sh"
eval_wait_for_accepted_count "$UNIT" "$ACCEPTED" 270
[[ "$(systemctl show "$UNIT" -p Result --value)" == success ]]
exec /bin/bash "$ROOT/tools/build_eval_dataset_artifacts.sh" yfcc10m
