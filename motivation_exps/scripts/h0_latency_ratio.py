#!/usr/bin/env python3
"""H0: CXL vs SSD latency for ANNS-relevant access sizes (LAION vector = 1024*4B)."""
from __future__ import annotations

import argparse
import os
import time

import numpy as np


def pick_cxl_node() -> int:
    import glob
    for path in sorted(glob.glob("/sys/devices/system/node/node*")):
        n = int(os.path.basename(path).replace("node", ""))
        cpus = open(f"{path}/cpulist").read().strip()
        if cpus == "":
            return n
    return 1


def alloc_on_node(n_bytes: int, node: int) -> np.ndarray:
    # Prefer numactl-bound process; here we just allocate and hope mbind via
    # running under numactl. Touch pages.
    arr = np.empty(n_bytes // 4, dtype=np.float32)
    arr.fill(1.0)
    return arr


def bench_ptr_chase(buf: np.ndarray, access: int, iters: int, rng) -> float:
    """Random access `access` floats; return ns/access."""
    n = buf.size
    stride = max(1, access)
    idx = rng.integers(0, max(1, n - stride), size=iters, dtype=np.int64)
    # warmup
    s = 0.0
    for i in idx[: min(1000, iters)]:
        s += float(buf[i : i + stride].sum())
    t0 = time.perf_counter_ns()
    for i in idx:
        s += float(buf[i : i + stride].sum())
    t1 = time.perf_counter_ns()
    if s == 0:
        print("unlikely")
    return (t1 - t0) / iters


def bench_ssd_pread(path: str, vec_bytes: int, n_vecs: int, iters: int, rng) -> float:
    # Use O_DIRECT via os.open if possible
    flags = os.O_RDONLY
    if hasattr(os, "O_DIRECT"):
        flags |= os.O_DIRECT
    try:
        fd = os.open(path, flags)
        odirect = True
    except OSError:
        fd = os.open(path, os.O_RDONLY)
        odirect = False
    # aligned buffer
    buf = np.empty(max(4096, (vec_bytes + 4095) & ~4095) // 4, dtype=np.float32)
    ids = rng.integers(0, n_vecs, size=iters, dtype=np.int64)
    # warmup
    for i in ids[:100]:
        off = int(i) * vec_bytes
        if odirect:
            aligned = off & ~4095
            pad = off - aligned
            need = pad + vec_bytes
            alloc = (need + 4095) & ~4095
            raw = os.pread(fd, alloc, aligned)
        else:
            raw = os.pread(fd, vec_bytes, off)
    t0 = time.perf_counter_ns()
    total = 0
    for i in ids:
        off = int(i) * vec_bytes
        if odirect:
            aligned = off & ~4095
            pad = off - aligned
            need = pad + vec_bytes
            alloc = (need + 4095) & ~4095
            raw = os.pread(fd, alloc, aligned)
            total += len(raw)
        else:
            raw = os.pread(fd, vec_bytes, off)
            total += len(raw)
    t1 = time.perf_counter_ns()
    os.close(fd)
    return (t1 - t0) / iters


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vectors", required=True)
    ap.add_argument("--dim", type=int, default=1024)
    ap.add_argument("--iters", type=int, default=5000)
    ap.add_argument("--cxl-mb", type=int, default=512)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    rng = np.random.default_rng(args.seed)
    vec_bytes = args.dim * 4
    n_vecs = os.path.getsize(args.vectors) // vec_bytes
    print(f"vectors={args.vectors} n={n_vecs} dim={args.dim} vec_bytes={vec_bytes}")

    # CXL buffer (caller should numactl --membind=CXL_NODE)
    cxl = alloc_on_node(args.cxl_mb * 1024 * 1024, 0)
    print(f"cxl_buf_mb={args.cxl_mb} floats={cxl.size}")

    sizes = [16, 64, 256, args.dim, 1024, 4096 // 4]  # in floats
    sizes = sorted(set(sizes))
    print("access_floats,cxl_ns,ssd_ns,ratio_ssd_over_cxl")
    for nf in sizes:
        if nf > cxl.size:
            continue
        cxl_ns = bench_ptr_chase(cxl, nf, args.iters, rng)
        # SSD always reads one full vector (ANNS miss unit)
        ssd_ns = bench_ssd_pread(args.vectors, vec_bytes, n_vecs, args.iters, rng)
        # Also time SSD for same byte size when smaller — approximate by still reading vector
        ratio = ssd_ns / max(cxl_ns, 1.0)
        print(f"{nf},{cxl_ns:.1f},{ssd_ns:.1f},{ratio:.1f}")


if __name__ == "__main__":
    main()
