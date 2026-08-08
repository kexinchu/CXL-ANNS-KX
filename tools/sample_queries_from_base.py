#!/usr/bin/env python3
"""Sample N random vectors from a large fbin base without loading the full file."""
from __future__ import annotations

import argparse
import os
import struct

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--nq", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out-q", required=True)
    ap.add_argument("--out-ids", default="", help="optional uint32 id list")
    args = ap.parse_args()

    with open(args.base, "rb") as f:
        n, d = struct.unpack("II", f.read(8))
    print(f"base n={n} dim={d}", flush=True)
    if args.nq > n:
        raise SystemExit(f"nq={args.nq} > n={n}")

    rng = np.random.default_rng(args.seed)
    ids = rng.choice(n, size=args.nq, replace=False).astype(np.uint32)
    ids.sort()  # sequential seeks

    row = d * 4
    q = np.empty((args.nq, d), dtype=np.float32)
    with open(args.base, "rb") as f:
        for i, vid in enumerate(ids):
            f.seek(8 + int(vid) * row)
            q[i] = np.fromfile(f, dtype=np.float32, count=d)
            if (i + 1) % 1000 == 0:
                print(f"  read {i+1}/{args.nq}", flush=True)

    # restore random order (sorted only for IO)
    order = rng.permutation(args.nq)
    q = q[order]
    ids = ids[order]

    os.makedirs(os.path.dirname(args.out_q) or ".", exist_ok=True)
    with open(args.out_q, "wb") as f:
        f.write(struct.pack("II", args.nq, d))
        q.tofile(f)
    print("wrote", args.out_q, q.shape)

    if args.out_ids:
        with open(args.out_ids, "wb") as f:
            f.write(struct.pack("I", args.nq))
            ids.tofile(f)
        print("wrote", args.out_ids)


if __name__ == "__main__":
    main()
