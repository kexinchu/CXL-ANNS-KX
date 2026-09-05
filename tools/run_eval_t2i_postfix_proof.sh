#!/usr/bin/env bash
# Fail-closed 100-query proof for the committed-page barrier runtime fix.
set -euo pipefail

ROOT=/root/chukexin/CXL-ANNS-KX/.worktrees/eval-flashanns-10m
TAG=7a04
EXPECTED_BINARY=7a04678165a2
IDENTITY=$ROOT/results/eval/flashanns/preflight/t2i-full-identity.json
EVIDENCE=$ROOT/results/eval/flashanns/preflight/t2i-postfix-${TAG}-volatile.json
RAW=$ROOT/results/eval/flashanns/raw/t2i10m/postfix-proof-${TAG}
ACCEPTED=$ROOT/results/eval/flashanns/accepted/t2i10m/postfix-proof-${TAG}
RUN_ID=t2i10m-smoke-L400-r0-proof-flashanns-${TAG}
RUN_DIR=$RAW/$RUN_ID

source "$ROOT/tools/eval_host_cold_lib.sh"
cd "$ROOT"

[[ "$(sha256sum serving/search_beam | awk '{print $1}')" == ${EXPECTED_BINARY}* ]]
[[ ! -e "$EVIDENCE" && ! -e "$RUN_DIR" && ! -e "$ACCEPTED/$RUN_ID" ]]
eval_reset_and_restore "$ROOT" t2i10m "$EVIDENCE" 4
python3 -m experiments.eval.flashanns.run_matrix \
  --dataset t2i10m --phase smoke --system flashanns --L 400 --repeat 0 \
  --run-tag "$TAG" --out "$RAW" \
  --identity-evidence "$IDENTITY" --volatile-evidence "$EVIDENCE"
jq -e --arg prefix "$EXPECTED_BINARY" \
  '.binary_sha256 | startswith($prefix)' "$RUN_DIR/run.json" >/dev/null
jq -e '.metrics.score_bounce == 0 and .metrics.score_flash == 0 and
       .metrics.completed_queries == 100' "$RUN_DIR/run.json" >/dev/null
python3 -m experiments.eval.flashanns.validate_run "$RUN_DIR" --seal-dir "$ACCEPTED"
echo "POSTFIX_PROOF_ACCEPTED $RUN_ID"
