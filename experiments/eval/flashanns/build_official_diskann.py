"""Build official DiskANN-compatible artifacts from the frozen flat graph."""

from __future__ import annotations

import argparse
import os
import struct
from pathlib import Path

import numpy as np


_ROWS_PER_CHUNK = 65536


def _matrix_shape(path: Path, item_bytes: int) -> tuple[int, int]:
    with path.open("rb") as stream:
        header = stream.read(8)
    if len(header) != 8:
        raise ValueError(f"{path}: missing matrix header")
    rows, columns = struct.unpack("<II", header)
    if not rows or not columns or path.stat().st_size != 8 + rows * columns * item_bytes:
        raise ValueError(f"{path}: invalid matrix shape")
    return rows, columns


def write_vamana_from_flat(
    graph_path: Path, output_path: Path, *, n: int, degree: int, entry_id: int
) -> None:
    graph_path, output_path = Path(graph_path), Path(output_path)
    if n <= 0 or degree <= 0 or not 0 <= entry_id < n:
        raise ValueError("invalid Vamana dimensions or entry ID")
    expected_graph_bytes = n * degree * 4
    if graph_path.stat().st_size != expected_graph_bytes:
        raise ValueError("flat graph size differs from n * degree * 4")

    graph = np.memmap(graph_path, dtype="<u4", mode="r", shape=(n, degree))
    for first in range(0, n, _ROWS_PER_CHUNK):
        block = graph[first : min(n, first + _ROWS_PER_CHUNK)]
        if block.size and int(block.max()) >= n:
            raise ValueError("graph neighbor lies outside dataset")

    expected_size = 24 + n * (4 + degree * 4)
    with output_path.open("xb") as stream:
        stream.write(struct.pack("<QIIQ", expected_size, degree, entry_id, 0))
        for first in range(0, n, _ROWS_PER_CHUNK):
            block = graph[first : min(n, first + _ROWS_PER_CHUNK)]
            encoded = np.empty((len(block), degree + 1), dtype="<u4")
            encoded[:, 0] = degree
            encoded[:, 1:] = block
            stream.write(encoded.tobytes(order="C"))
        stream.flush()
        os.fsync(stream.fileno())
    del graph


def prepare_mips_base(
    source_path: Path, output_path: Path, norm_path: Path
) -> float:
    source_path, output_path, norm_path = map(
        Path, (source_path, output_path, norm_path)
    )
    n, dim = _matrix_shape(source_path, 4)
    source = np.memmap(
        source_path, dtype="<f4", mode="r", offset=8, shape=(n, dim)
    )
    max_squared_norm = 0.0
    for first in range(0, n, _ROWS_PER_CHUNK):
        block = np.asarray(source[first : min(n, first + _ROWS_PER_CHUNK)])
        squared = np.einsum("ij,ij->i", block, block, dtype=np.float32)
        max_squared_norm = max(max_squared_norm, float(squared.max()))
    maximum = float(np.sqrt(np.float32(max_squared_norm)))
    if not np.isfinite(maximum) or maximum <= 0:
        raise ValueError("MIPS base must have a finite positive maximum norm")

    with output_path.open("xb") as stream:
        stream.write(struct.pack("<II", n, dim + 1))
        for first in range(0, n, _ROWS_PER_CHUNK):
            block = np.asarray(
                source[first : min(n, first + _ROWS_PER_CHUNK)], dtype=np.float32
            )
            encoded = np.empty((len(block), dim + 1), dtype="<f4")
            encoded[:, :dim] = block / np.float32(maximum)
            squared = np.einsum(
                "ij,ij->i", encoded[:, :dim], encoded[:, :dim], dtype=np.float32
            )
            encoded[:, dim] = np.sqrt(np.maximum(np.float32(0), np.float32(1) - squared))
            stream.write(encoded.tobytes(order="C"))
        stream.flush()
        os.fsync(stream.fileno())
    with norm_path.open("xb") as stream:
        stream.write(struct.pack("<IIf", 1, 1, maximum))
        stream.flush()
        os.fsync(stream.fileno())
    del source
    return maximum


def main() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    wrap = commands.add_parser("wrap-graph")
    wrap.add_argument("--graph", type=Path, required=True)
    wrap.add_argument("--output", type=Path, required=True)
    wrap.add_argument("--n", type=int, required=True)
    wrap.add_argument("--degree", type=int, required=True)
    wrap.add_argument("--entry-id", type=int, required=True)
    mips = commands.add_parser("prepare-mips")
    mips.add_argument("--source", type=Path, required=True)
    mips.add_argument("--output", type=Path, required=True)
    mips.add_argument("--norm-output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "wrap-graph":
        write_vamana_from_flat(
            args.graph,
            args.output,
            n=args.n,
            degree=args.degree,
            entry_id=args.entry_id,
        )
    else:
        maximum = prepare_mips_base(args.source, args.output, args.norm_output)
        print(f"max_base_norm={maximum:.9g}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
