#!/usr/bin/env bash
set -euo pipefail
bin=/root/chukexin/CXL-ANNS-KX/serving/search_beam
set +e
err=$("$bin" --require-cxl-dram --dram-backend numa --host-bytes $((2*1024*1024*1024)) \
  --entry /dev/null --queries /dev/null 2>&1)
rc=$?
set -e
test "$rc" -eq 2
# Unknown-flag exit 2 is not the role check. Refuse must mention numa / HOST.
case "$err" in
  unknown*) echo "FAIL flags missing: $err"; exit 1 ;;
esac
echo "$err" | grep -qiE 'numa|HOST DRAM|require-cxl'
echo "PASS refuse numa as CXL-DRAM"

# /dev/dax* and default DAX are not the vmem BAR.
unset CXAN_CXL_DRAM_DEV || true
set +e
err2=$("$bin" --require-cxl-dram --dram-backend dax --host-bytes $((2*1024*1024*1024)) \
  --entry /dev/null --queries /dev/null 2>&1)
rc2=$?
set -e
test "$rc2" -eq 2
case "$err2" in
  unknown*) echo "FAIL flags missing: $err2"; exit 1 ;;
esac
echo "$err2" | grep -q '^need (' && { echo "FAIL missing vmem-BAR check: $err2"; exit 1; }
echo "$err2" | grep -qiE 'vmem|/dev/dax|cxl-dram-dev|CXAN_CXL_DRAM'
echo "PASS refuse dax default as CXL-DRAM"

unset CXAN_CXL_DRAM_DEV || true
set +e
err3=$("$bin" --require-cxl-dram --dram-backend dax --cxl-dram-dev /dev/dax0.0 \
  --host-bytes 2147483648 --entry /dev/null --queries /dev/null 2>&1)
rc3=$?
set -e
test "$rc3" -eq 2
case "$err3" in
  unknown*) echo "FAIL flags missing: $err3"; exit 1 ;;
esac
echo "$err3" | grep -qiE 'dax|refuse'
echo "PASS refuse --cxl-dram-dev /dev/dax0.0"

set +e
err4=$("$bin" --diskann-layout --oracle-dram --dram-backend numa \
  --entry /dev/null --queries /dev/null --nav-graph /dev/null 2>&1)
rc4=$?
set -e
test "$rc4" -eq 2
echo "$err4" | grep -qiE 'numa|refuse'
echo "PASS refuse diskann+numa oracle"

set +e
err5=$("$bin" --diskann-layout --entry /dev/null --queries /dev/null 2>&1)
rc5=$?
set -e
test "$rc5" -eq 2
echo "$err5" | grep -qiE 'nav-graph'
echo "PASS refuse diskann without nav-graph"
