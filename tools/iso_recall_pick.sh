#!/usr/bin/env bash
# Usage: iso_recall_pick.sh --floor 0.92 --beams 150,200,300,400 -- <search_beam args...>
set -euo pipefail
FLOOR=0.92
BEAMS=150,200,300,400
while [[ $# -gt 0 ]]; do
  case $1 in
    --floor) FLOOR=$2; shift 2 ;;
    --beams) BEAMS=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "unknown $1"; exit 2 ;;
  esac
done
if [[ $# -eq 0 ]]; then
  echo "usage: iso_recall_pick.sh --floor 0.92 --beams 150,200,300 -- <search_beam> [args]" >&2
  exit 2
fi
BEST=""
IFS=',' read -ra LS <<< "$BEAMS"
for L in "${LS[@]}"; do
  echo "=== try L=$L floor=$FLOOR ==="
  set +e
  out=$("$@" --beam "$L" --iters 0 2>&1)
  ec=$?
  set -e
  echo "$out" | tail -n 20
  [[ $ec -eq 0 ]] || exit $ec
  rec=$(echo "$out" | awk -F= '/^recall@/{print $2; exit}')
  if awk -v r="$rec" -v t="$FLOOR" 'BEGIN{exit !(r+0>=t)}'; then
    BEST=$L
    echo "PICKED L=$L recall=$rec"
    echo "$out" | grep '^CSV,'
    exit 0
  fi
done
echo "NO_BEAM_MET_FLOOR $FLOOR" >&2
exit 1
