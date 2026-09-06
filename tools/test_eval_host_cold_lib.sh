#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$ROOT/tools/eval_host_cold_lib.sh"

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

cat >"$tmp" <<'EOF'
CommitLimit:    70960824 kB
Committed_AS:   60959080 kB
EOF

actual=$(eval_commit_headroom_kib "$tmp")
[[ "$actual" == 10001744 ]]

EVAL_MEMINFO_PATH=$tmp EVAL_COMMIT_POLL_SECONDS=0 \
  eval_wait_for_commit_headroom 8388608

echo "test_eval_host_cold_lib OK"
