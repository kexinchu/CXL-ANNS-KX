#!/usr/bin/env python3
"""Sample N queries from a float32 base.bin / vectors.bin and write GT (FAISS Flat)."""
from __future__ import annotations

import argparse
import os
import struct

import numpy as np


def read_fbin(path: str):
    with open(path, "rb") as f:
        n, d = struct.unpack("II", f.read(8))
        x = np.fromfile(f, dtype=np.float32, count=n * d).reshape(n, d)
    return x


def write_fbin(path: str, x: np.ndarray):
    n, d = x.shape
    with open(path, "wb") as f:
        f.write(struct.pack("II", n, d))
        x.astype(np.float32).tofile(f)


def write_ibin(path: str, x: np.ndarray):
    n, k = x.shape
    with open(path, "wb") as f:
        f.write(struct.pack("II", n, k))
        x.astype(np.uint32).tofile(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="fbin float32 vectors OR raw vectors.bin")
    ap.add_argument("--raw-n", type=int, default=0, help="if raw vectors.bin, pass n")
    ap.add_argument("--raw-dim", type=int, default=0)
    ap.add_argument("--nq", type=int, default=10000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out-q", required=True)
    ap.add_argument("--out-gt", required=True)
    ap.add_argument("--max-base", type=int, default=0, help="use only first max-base rows")
    args = ap.parse_args()

    if args.raw_n and args.raw_dim:
        x = np.fromfile(args.base, dtype=np.float32, count=args.raw_n * args.raw_dim).reshape(
            args.raw_n, args.raw_dim
        )
    else:
        x = read_fbin(args.base)
    if args.max_base and args.max_base < x.shape[0]:
        x = x[: args.max_base]
    n, d = x.shape
    print(f"base {n}x{d}")

    rng = np.random.default_rng(args.seed)
    # sample query ids from base (leave-one-out style: GT will include self as top1)
    qids = rng.choice(n, size=min(args.nq, n), replace=False)
    q = x[qids].copy()
    write_fbin(args.out_q, q)
    print("wrote", args.out_q, q.shape)

    import faiss

    index = faiss.IndexFlatL2(d)
    index.add(x)
    D, I = index.search(q, args.k)
    write_ibin(args.out_gt, I.astype(np.uint32))
    print("wrote", args.out_gt, I.shape, "mean_dist0", float(D[:, 0].mean()))


if __name__ == "__main__":
    main()
