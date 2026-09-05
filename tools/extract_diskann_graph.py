#!/usr/bin/env python3
"""Extract packed DiskANN N(u) into a host graph-file (n * R * 4 bytes)."""
import argparse
import mmap
import os
import struct
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    fd = os.open(args.src, os.O_RDONLY)
    size = os.fstat(fd).st_size
    mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
    magic, ver, n, dim, R = struct.unpack_from("<QIIII", mm, 0)
    if magic != 0x314E415843:
        sys.exit(f"bad magic {magic:#x}")
    off_v, len_v = struct.unpack_from("<QQ", mm, 76)
    stride = len_v // n
    nbr_off = dim * 4 + 4
    need = n * R * 4
    print(f"n={n} R={R} stride={stride} -> {args.out} bytes={need}", flush=True)
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    out = open(args.out, "wb")
    rec = R * 4
    chunk = bytearray()
    flush_at = 32 << 20
    for i in range(n):
        o = off_v + i * stride + nbr_off
        chunk.extend(mm[o : o + rec])
        if len(chunk) >= flush_at:
            out.write(chunk)
            chunk.clear()
            if i and i % 2_000_000 == 0:
                print(f"  {i} ids", flush=True)
    if chunk:
        out.write(chunk)
    out.close()
    mm.close()
    os.close(fd)
    got = os.path.getsize(args.out)
    if got != need:
        sys.exit(f"size {got} != {need}")
    print(f"OK {args.out} {got}", flush=True)


if __name__ == "__main__":
    main()
