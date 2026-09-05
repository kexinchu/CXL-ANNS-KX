"""Generate deterministic identity and entry artifacts for fixed-stride indexes."""

from __future__ import annotations

import argparse
import array
import mmap
import os
import struct
import sys
from collections import deque
from pathlib import Path


def write_identity_map(path: Path, n: int) -> None:
    path = Path(path)
    if n <= 0 or n > 0xFFFFFFFF:
        raise ValueError("identity-map size must fit uint32")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as output:
        for start in range(0, n, 1 << 20):
            values = array.array("I", range(start, min(start + (1 << 20), n)))
            if sys.byteorder != "little":
                values.byteswap()
            output.write(values.tobytes())
    if path.stat().st_size != n * 4:
        raise OSError("identity-map write was short")


def write_entry_graph(
    graph_path: Path,
    output_path: Path,
    *,
    n: int,
    degree: int,
    entry_id: int,
    limit: int = 8192,
) -> None:
    graph_path, output_path = Path(graph_path), Path(output_path)
    if n <= 0 or degree <= 0 or not 0 <= entry_id < n or limit <= 0:
        raise ValueError("invalid entry-graph dimensions")
    if graph_path.stat().st_size != n * degree * 4:
        raise ValueError("graph size differs from n x degree uint32")
    limit = min(limit, n)
    seen = bytearray(n)
    order: list[int] = []
    adjacency = bytearray()
    pending = deque([entry_id])
    seen[entry_id] = 1
    with graph_path.open("rb") as source:
        graph = mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            while pending and len(order) < limit:
                node = pending.popleft()
                order.append(node)
                neighbors = struct.unpack_from(f"<{degree}I", graph, node * degree * 4)
                for neighbor in neighbors:
                    if neighbor >= n:
                        raise ValueError(f"graph neighbor {neighbor} is out of range")
                    if not seen[neighbor]:
                        seen[neighbor] = 1
                        pending.append(neighbor)
            if len(order) < limit:
                for node in range(n):
                    if not seen[node]:
                        order.append(node)
                        if len(order) == limit:
                            break
            row_bytes = degree * 4
            for node in order:
                start = node * row_bytes
                adjacency.extend(graph[start : start + row_bytes])
        finally:
            graph.close()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("xb") as output:
        output.write(struct.pack("<III", len(order), degree, entry_id))
        output.write(struct.pack(f"<{len(order)}I", *order))
        output.write(adjacency)


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    identity = subparsers.add_parser("identity")
    identity.add_argument("--out", required=True, type=Path)
    identity.add_argument("--n", required=True, type=int)
    entry = subparsers.add_parser("entry")
    entry.add_argument("--graph", required=True, type=Path)
    entry.add_argument("--out", required=True, type=Path)
    entry.add_argument("--n", required=True, type=int)
    entry.add_argument("--degree", required=True, type=int)
    entry.add_argument("--entry-id", required=True, type=int)
    entry.add_argument("--limit", type=int, default=8192)
    args = parser.parse_args()
    if args.command == "identity":
        write_identity_map(args.out, args.n)
    else:
        write_entry_graph(
            args.graph,
            args.out,
            n=args.n,
            degree=args.degree,
            entry_id=args.entry_id,
            limit=args.limit,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
