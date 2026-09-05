#!/usr/bin/env python3
"""Rank pages by expand-trace frequency and emit IDs that fit a byte budget."""
import argparse
import struct
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--bytes", type=int, default=80 << 20)
    ap.add_argument("--page-bytes", type=int, default=4096)
    args = ap.parse_args()

    slot = open(args.map, "rb").read()
    n = len(slot) // 4
    id_to_slot = struct.unpack_from(f"<{n}I", slot, 0)
    freq = {}
    expands = 0
    with open(args.trace, "rb") as f:
        while True:
            hdr = f.read(4)
            if len(hdr) < 4:
                break
            (cnt,) = struct.unpack("<I", hdr)
            if cnt == 0 or cnt > 256:
                break
            raw = f.read(4 * cnt)
            if len(raw) < 4 * cnt:
                break
            ids = struct.unpack(f"<{cnt}I", raw)
            pages = {id_to_slot[i] // 2 for i in ids if i < n}
            for p in pages:
                freq[p] = freq.get(p, 0) + 1
            expands += 1

    ranked = sorted(freq.items(), key=lambda kv: (-kv[1], kv[0]))
    cap_pages = args.bytes // args.page_bytes
    chosen = [p for p, _ in ranked[:cap_pages]]
    slot_to_ids = [[] for _ in range(n)]
    for i, s in enumerate(id_to_slot):
        if s < n:
            slot_to_ids[s].append(i)
    out_ids = []
    for p in chosen:
        for s in (p * 2, p * 2 + 1):
            if s < n:
                out_ids.extend(slot_to_ids[s])
    with open(args.out, "wb") as f:
        f.write(struct.pack(f"<{len(out_ids)}I", *out_ids) if out_ids else b"")
    print(
        f"trace_expands={expands} unique_pages={len(freq)} "
        f"pin_pages={len(chosen)} pin_ids={len(out_ids)} "
        f"pin_bytes={len(chosen) * args.page_bytes}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
