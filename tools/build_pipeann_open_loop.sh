#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DISKANN=${DISKANN_ROOT:-/mnt/disk0/chukexin_motivation/DiskANN_cpp}
BUILD=$DISKANN/build
TMP=$(mktemp -d /tmp/flashanns-diskann-src.XXXXXX)
trap 'rm -rf -- "$TMP"' EXIT

git -C "$DISKANN" archive HEAD | tar -x -C "$TMP"

g++ -std=c++17 -fopenmp -mavx2 -mfma -msse2 -ftree-vectorize \
  -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free \
  -fopenmp-simd -funroll-loops -DUSE_AVX2 -O3 -DNDEBUG -march=native -mtune=native \
  -I"$TMP/include" -I"$TMP/apps" "$ROOT/tools/pipeann_open_loop.cpp" \
  "$BUILD/src/libdiskann.a" -o "$ROOT/tools/pipeann_open_loop" \
  -lmkl_intel_ilp64 -lmkl_intel_thread -lmkl_core -liomp5 -lpthread -lm -ldl \
  -laio -ltcmalloc -lboost_container
