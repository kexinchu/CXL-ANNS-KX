#!/usr/bin/env python3
"""Capacity budget for CXL-ANNS serving layout (Task 1)."""
from __future__ import annotations

import argparse
import json
import sys

GiB = 1024**3
MiB = 1024**2


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--n", type=int, required=True)
    p.add_argument("--dim", type=int, required=True)
    p.add_argument("--R", type=int, default=32)
    p.add_argument("--pq-bytes", type=int, default=32)
    p.add_argument(
        "--vec-bytes",
        type=int,
        default=2,
        help="bytes per dimension (2=float16, 4=float32)",
    )
    p.add_argument("--entry-mb", type=float, default=128.0)
    p.add_argument("--dram-gb", type=float, default=1.0)
    p.add_argument("--ssd-gb", type=float, default=100.0)
    p.add_argument("--pivots-mb", type=float, default=16.0)
    p.add_argument("--json", action="store_true")
    args = p.parse_args()

    vectors = args.n * args.dim * args.vec_bytes
    graph = args.n * args.R * 4
    pq = args.n * args.pq_bytes
    pivots = int(args.pivots_mb * MiB)
    meta = 4 * MiB
    entry = int(args.entry_mb * MiB)
    ssd_total = vectors + graph + pq + pivots + meta
    dram_cap = int(args.dram_gb * GiB)
    ssd_cap = int(args.ssd_gb * GiB)

    rows = {
        "n": args.n,
        "dim": args.dim,
        "R": args.R,
        "pq_bytes": args.pq_bytes,
        "vec_bytes": args.vec_bytes,
        "vectors_bytes": vectors,
        "graph_bytes": graph,
        "pq_bytes_total": pq,
        "pivots_bytes": pivots,
        "meta_bytes": meta,
        "ssd_total_bytes": ssd_total,
        "entry_budget_bytes": entry,
        "dram_cap_bytes": dram_cap,
        "ssd_cap_bytes": ssd_cap,
        "ssd_ok": ssd_total <= ssd_cap,
        "entry_ok": entry <= 128 * MiB,
        "dram_ok": dram_cap <= GiB + 1024,  # allow exact 1GiB
        "headroom_ssd_bytes": ssd_cap - ssd_total,
    }

    if args.json:
        print(json.dumps(rows, indent=2))
        return 0 if rows["ssd_ok"] else 1

    def fmt(b: int) -> str:
        return f"{b / GiB:.3f} GiB ({b})"

    print(f"config: N={args.n} dim={args.dim} R={args.R} pq_bytes={args.pq_bytes} "
          f"vec_bytes={args.vec_bytes}")
    print(f"  vectors:  {fmt(vectors)}")
    print(f"  graph:    {fmt(graph)}")
    print(f"  pq:       {fmt(pq)}")
    print(f"  pivots:   {fmt(pivots)}")
    print(f"  meta:     {fmt(meta)}")
    print(f"  SSD sum:  {fmt(ssd_total)}  cap={args.ssd_gb} GiB  OK={rows['ssd_ok']}")
    print(f"  entry bud:{entry / MiB:.1f} MiB  OK={rows['entry_ok']}")
    print(f"  DRAM cap: {args.dram_gb} GiB  OK={rows['dram_ok']}")
    print(f"  SSD headroom: {fmt(rows['headroom_ssd_bytes'])}")
    return 0 if rows["ssd_ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
