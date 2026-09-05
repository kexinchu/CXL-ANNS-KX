"""Deterministically widen YFCC uint8 vectors for the L2 serving pipeline."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np


def _header(path: Path) -> tuple[int, int]:
    with Path(path).open("rb") as src:
        raw = src.read(8)
    if len(raw) != 8:
        raise ValueError(f"{path}: truncated header")
    return struct.unpack("<II", raw)


def widen_u8bin(source: Path, destination: Path, expected_rows: int,
                expected_dim: int, chunk_rows: int = 65536) -> dict:
    source, destination = Path(source), Path(destination)
    rows, dim = _header(source)
    if (rows, dim) != (expected_rows, expected_dim):
        raise ValueError(f"{source}: header {(rows, dim)} != {(expected_rows, expected_dim)}")
    if source.stat().st_size != 8 + rows * dim:
        raise ValueError(f"{source}: size does not match uint8 header")
    destination.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with source.open("rb") as src, destination.open("xb") as dst:
        src.seek(8)
        header = struct.pack("<II", rows, dim)
        dst.write(header)
        digest.update(header)
        remaining = rows
        while remaining:
            take = min(remaining, chunk_rows)
            raw = src.read(take * dim)
            if len(raw) != take * dim:
                raise ValueError(f"{source}: truncated payload")
            widened = np.frombuffer(raw, dtype=np.uint8).astype("<f4", copy=True)
            if not np.array_equal(widened, np.frombuffer(raw, dtype=np.uint8)):
                raise ValueError("uint8-to-float32 coordinate mismatch")
            block = widened.tobytes()
            dst.write(block)
            digest.update(block)
            remaining -= take
    return {"source": str(source), "destination": str(destination), "rows": rows,
            "dimension": dim, "normalization": "none", "sha256": digest.hexdigest()}


def extract_gt_topk(source: Path, destination: Path, rows: int, k: int) -> None:
    source, destination = Path(source), Path(destination)
    source_rows, source_k = _header(source)
    if rows > source_rows or k > source_k:
        raise ValueError("requested GT subset exceeds source")
    if source.stat().st_size != 8 + source_rows * source_k * 4:
        raise ValueError("GT size does not match header")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as src, destination.open("xb") as dst:
        dst.write(struct.pack("<II", rows, k))
        src.seek(8)
        for _ in range(rows):
            row = src.read(source_k * 4)
            if len(row) != source_k * 4:
                raise ValueError("truncated GT row")
            dst.write(row[: k * 4])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    manifests = {
        "base": widen_u8bin(args.source_dir / "base.10M.u8bin", args.out_dir / "base.10M.fbin", 10_000_000, 192),
        "queries": widen_u8bin(args.source_dir / "query.public.100K.u8bin", args.out_dir / "query_100k.fbin", 100_000, 192),
    }
    query_100k = args.out_dir / "query_100k.fbin"
    query_10k = args.out_dir / "query_10k.fbin"
    with query_100k.open("rb") as src, query_10k.open("xb") as dst:
        src.seek(8)
        dst.write(struct.pack("<II", 10_000, 192))
        dst.write(src.read(10_000 * 192 * 4))
    extract_gt_topk(args.source_dir / "unfiltered.GT.public.ibin", args.out_dir / "gt_10k_k10.ibin", 10_000, 10)
    (args.out_dir / "conversion-manifest.json").write_text(json.dumps(manifests, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
