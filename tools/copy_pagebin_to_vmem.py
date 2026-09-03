#!/usr/bin/env python3
"""Copy host pagebin_image.bin onto /dev/vmem0 at the logical pagebin offset."""
import mmap
import os
import struct
import sys
import time

SRC = "/mnt/disk0/chukexin_motivation/serving_t2i_10m/pagebin_image.bin"
DST = "/dev/vmem0"
OFF = 450971566080
MAGIC = 0x314e415843
CHUNK = 8 << 20

def main():
    size = os.path.getsize(SRC)
    if size != 9600069632:
        print(f"FAIL unexpected pagebin size {size}", file=sys.stderr)
        return 2
    sfd = os.open(SRC, os.O_RDONLY)
    src = mmap.mmap(sfd, size, mmap.MAP_SHARED, mmap.PROT_READ)
    mag = struct.unpack_from("<Q", src, 0)[0]
    if mag != MAGIC:
        print(f"FAIL src magic {hex(mag)}", file=sys.stderr)
        return 2
    dfd = os.open(DST, os.O_RDWR)
    dst = mmap.mmap(dfd, size, mmap.MAP_SHARED, mmap.PROT_WRITE, offset=OFF)
    t0 = time.time()
    copied = 0
    while copied < size:
        n = min(CHUNK, size - copied)
        dst[copied : copied + n] = src[copied : copied + n]
        copied += n
        if copied == size or copied % (256 << 20) < CHUNK:
            dt = time.time() - t0
            print(
                f"  pagebin {copied}/{size} ({100.0 * copied / size:.1f}%) "
                f"{copied / 1e9 / dt:.2f} GB/s",
                flush=True,
            )
    print("msync pagebin...", flush=True)
    dst.flush()
    os.fsync(dfd)
    dst.close()
    src.close()
    os.close(dfd)
    os.close(sfd)
    # verify magic via fresh map
    vfd = os.open(DST, os.O_RDONLY)
    vm = mmap.mmap(vfd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=OFF)
    got = struct.unpack_from("<Q", vm, 0)[0]
    vm.close()
    os.close(vfd)
    print(f"DONE pagebin bytes={size} off={OFF} magic={hex(got)}")
    return 0 if got == MAGIC else 2

if __name__ == "__main__":
    sys.exit(main())
