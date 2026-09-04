#!/usr/bin/env python3
"""Copy the first N DiskANN entries into a version-2 prefix image."""
import argparse
import os
import struct
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=1_000_000)
    args = ap.parse_args()
    with open(args.src, "rb") as f:
        hdr = f.read(96)
    magic, ver, n, dim, R = struct.unpack_from("<QIIII", hdr, 0)
    if magic != 0x314E415843:
        sys.exit(f"bad magic {magic:#x}")
    off_v, len_v = struct.unpack_from("<QQ", hdr, 76)
    stride = len_v // n
    n_out = min(args.n, n)
    out_bytes = off_v + n_out * stride
    print(f"src n={n} stride={stride} -> n={n_out} bytes={out_bytes}", flush=True)
    hdr_out = bytearray(hdr)
    struct.pack_into("<I", hdr_out, 12, n_out)
    struct.pack_into("<I", hdr_out, 8, 2)  # version
    struct.pack_into("<QQ", hdr_out, 76, off_v, n_out * stride)
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.src, "rb") as inf, open(args.out, "wb") as out:
        out.write(hdr_out)
        # pad 96..off_v
        if off_v > 96:
            out.write(b"\x00" * (off_v - 96))
        inf.seek(off_v)
        left = n_out * stride
        chunk = 8 << 20
        while left:
            b = inf.read(min(chunk, left))
            if not b:
                sys.exit("short read")
            out.write(b)
            left -= len(b)
    print(f"OK {args.out}", flush=True)


if __name__ == "__main__":
    main()
