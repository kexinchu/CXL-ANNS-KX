#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PIPEANN_ROOT=${PIPEANN_ROOT:-/root/chukexin/CXL-ANNS-KX/third_party/PipeANN}
PIPEANN_BUILD=${PIPEANN_BUILD:-$PIPEANN_ROOT/build}

test -f "$PIPEANN_ROOT/include/ssd_index.h"
test -f "$PIPEANN_BUILD/src/libpipeann.a"

g++ -std=c++17 -O3 -DNDEBUG -march=x86-64-v3 -mtune=generic \
  -mavx2 -mfma -msse2 -fopenmp -pthread \
  -DBG_IO_THREAD -DUSE_AVX2 -DUSE_URING -DUSE_TCMALLOC \
  -I"$PIPEANN_ROOT/include" \
  -I"$PIPEANN_ROOT/include/tsl/include" \
  -I"$PIPEANN_ROOT/third_party/liburing/src/include" \
  "$ROOT/tools/pipeann_open_loop.cpp" "$PIPEANN_BUILD/src/libpipeann.a" \
  -L"$PIPEANN_ROOT/third_party/liburing/src" \
  -Wl,-rpath,"$PIPEANN_ROOT/third_party/liburing/src" \
  -Wl,--no-as-needed -ltcmalloc -Wl,--as-needed \
  -luring -lmkl_rt -lgomp -lpthread \
  -o "$ROOT/tools/pipeann_open_loop"
