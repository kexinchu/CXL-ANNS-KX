"""Prepare the declared LAION prefix and held-out query subset without normalization."""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import struct
from pathlib import Path

import numpy as np


def _header(path: Path) -> tuple[int, int]:
    with Path(path).open("rb") as src:
        raw = src.read(8)
    if len(raw) != 8:
        raise ValueError(f"{path}: truncated fbin header")
    return struct.unpack("<II", raw)


def prepare_prefix_and_queries(base_path: Path, query_path: Path, out_dir: Path,
                               corpus_rows: int, query_rows: int, seed: int) -> dict:
    base_path, query_path, out_dir = Path(base_path), Path(query_path), Path(out_dir)
    base_n, dim = _header(base_path)
    query_n, query_dim = _header(query_path)
    if query_dim != dim or corpus_rows > base_n or query_rows > query_n:
        raise ValueError("LAION subset dimensions or row counts are invalid")
    if base_path.stat().st_size != 8 + base_n * dim * 4 or query_path.stat().st_size != 8 + query_n * dim * 4:
        raise ValueError("LAION fbin size does not match header")
    out_dir.mkdir(parents=True, exist_ok=True)
    prefix = out_dir / "base.10M.fbin"
    digest = hashlib.sha256()
    with base_path.open("rb") as src, prefix.open("xb") as dst:
        src.seek(8)
        header = struct.pack("<II", corpus_rows, dim)
        dst.write(header)
        digest.update(header)
        remaining = corpus_rows * dim * 4
        while remaining:
            block = src.read(min(64 << 20, remaining))
            if not block:
                raise ValueError("truncated LAION base prefix")
            dst.write(block)
            digest.update(block)
            remaining -= len(block)
    query_ids = random.Random(seed).sample(range(query_n), query_rows)
    source_queries = np.memmap(query_path, dtype="<f4", mode="r", offset=8, shape=(query_n, dim))
    selected = np.asarray(source_queries[query_ids], dtype="<f4")
    with (out_dir / "query_10k.fbin").open("xb") as dst:
        dst.write(struct.pack("<II", query_rows, dim))
        dst.write(selected.tobytes())
    manifest = {
        "base_source": str(base_path), "query_source": str(query_path),
        "corpus_ids": [0, corpus_rows], "query_ids": query_ids, "seed": seed,
        "dimension": dim, "base_prefix_sha256": digest.hexdigest(),
        "normalization": "none", "ground_truth_status": "required",
    }
    (out_dir / "subset-manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--queries", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    prepare_prefix_and_queries(args.base, args.queries, args.out_dir, 10_000_000, 10_000, 42)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
