#!/usr/bin/env python3
"""Export DiskANN in-mem vamana graph (mem_R32) to flat N×R uint32 graph.bin."""
from __future__ import annotations

import argparse
import os
import struct
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--diskann-index", required=True, help="DiskANN mem index prefix file (no .data)")
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--R", type=int, default=32)
    ap.add_argument("--out", required=True)
    ap.add_argument("--out-entry-id", default="", help="write start/medoid id to this text file")
    args = ap.parse_args()

    path = args.diskann_index
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        expected_file_size, max_deg, start, frozen = struct.unpack("<QIIQ", f.read(24))
        print(f"header expected={expected_file_size} max_deg={max_deg} start={start} frozen={frozen} filesize={size}")
        assert expected_file_size == size, (expected_file_size, size)

        out = np.memmap(args.out, dtype=np.uint32, mode="w+", shape=(args.n, args.R))
        nodes = 0
        while nodes < args.n:
            k_b = f.read(4)
            if not k_b:
                break
            (k,) = struct.unpack("<I", k_b)
            nbrs = np.frombuffer(f.read(k * 4), dtype=np.uint32)
            row = np.empty(args.R, dtype=np.uint32)
            if k >= args.R:
                row[:] = nbrs[: args.R]
            else:
                row[:k] = nbrs
                # pad with last nbr or self
                pad = int(nbrs[-1]) if k else nodes
                row[k:] = pad
            out[nodes] = row
            nodes += 1
            if nodes % 1_000_000 == 0:
                print(f"  exported {nodes}/{args.n}")
                out.flush()
        out.flush()
        del out
        assert nodes == args.n, (nodes, args.n)
        print(f"wrote {args.out} nodes={nodes} R={args.R} bytes={os.path.getsize(args.out)}")
        if args.out_entry_id:
            open(args.out_entry_id, "w").write(str(start) + "\n")
            print("entry_id", start)


if __name__ == "__main__":
    main()
