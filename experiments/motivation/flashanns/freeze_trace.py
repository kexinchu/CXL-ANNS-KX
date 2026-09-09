"""Validate and freeze a current-implementation LAION candidate trace."""

from __future__ import annotations

from array import array
import argparse
import json
from pathlib import Path
import shutil
import sys
from typing import Mapping

from .observe import sha256_file


class TraceError(ValueError):
    """The candidate trace cannot support a controlled C2 comparison."""


_TRACE_FILES = (
    "query_ids.u32",
    "candidate_offsets.u64",
    "candidate_ids.u32",
    "result_ids.u32",
)
_ARTIFACTS = ("graph", "slot_map", "binary", "image")


def _read_array(path: Path, typecode: str, item_bytes: int) -> array:
    size = path.stat().st_size
    if size % item_bytes:
        raise TraceError(f"unaligned trace file: {path.name}")
    values = array(typecode)
    with path.open("rb") as stream:
        values.fromfile(stream, size // item_bytes)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def _validate_trace(trace: Path, L: int, k: int) -> dict[str, int]:
    missing = [name for name in _TRACE_FILES if not (trace / name).is_file()]
    if missing:
        raise TraceError("missing trace files: " + ", ".join(missing))
    query_ids = _read_array(trace / "query_ids.u32", "I", 4)
    if len(query_ids) != 10000:
        raise TraceError("trace must contain exactly 10,000 queries")
    if len(set(query_ids)) != len(query_ids):
        raise TraceError("query IDs are not unique")

    offsets = _read_array(trace / "candidate_offsets.u64", "Q", 8)
    candidates = _read_array(trace / "candidate_ids.u32", "I", 4)
    results = _read_array(trace / "result_ids.u32", "I", 4)
    if len(offsets) != len(query_ids) + 1:
        raise TraceError("candidate offset count differs from nq + 1")
    if not offsets or offsets[0] != 0 or offsets[-1] != len(candidates):
        raise TraceError("candidate offsets do not bound candidate IDs")
    widths = [right - left for left, right in zip(offsets, offsets[1:])]
    if any(width <= 0 or width > L for width in widths):
        raise TraceError("candidate width is empty or exceeds L")
    if len(results) != len(query_ids) * k:
        raise TraceError("result ID count differs from nq * k")
    return {"nq": len(query_ids), "candidate_count": len(candidates)}


def freeze_trace(
    trace: Path | str,
    output: Path | str,
    artifacts: Mapping[str, Path | str],
    *,
    L: int,
    k: int,
) -> Path:
    source = Path(trace)
    destination = Path(output)
    counts = _validate_trace(source, L, k)
    missing = [name for name in _ARTIFACTS if name not in artifacts or not Path(artifacts[name]).is_file()]
    if missing:
        raise TraceError("missing identity artifacts: " + ", ".join(missing))
    if destination.exists():
        raise TraceError(f"refusing existing frozen trace {destination}")

    trace_hashes = {name: sha256_file(source / name) for name in _TRACE_FILES}
    artifact_manifest = {
        name: {"path": str(Path(artifacts[name]).resolve()), "sha256": sha256_file(artifacts[name])}
        for name in _ARTIFACTS
    }
    destination.mkdir(parents=True)
    for name in _TRACE_FILES:
        shutil.copyfile(source / name, destination / name)
        if sha256_file(destination / name) != trace_hashes[name]:
            raise TraceError(f"copied trace hash mismatch: {name}")
    manifest = {
        "schema_version": 1,
        "accepted": True,
        "nq": counts["nq"],
        "candidate_count": counts["candidate_count"],
        "L": L,
        "k": k,
        "trace_sha256": trace_hashes,
        "artifacts": artifact_manifest,
        "performance_values_included": False,
    }
    manifest_path = destination / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--slot-map", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--L", type=int, default=400)
    parser.add_argument("--k", type=int, default=10)
    args = parser.parse_args()
    manifest = freeze_trace(
        args.trace,
        args.output,
        {"graph": args.graph, "slot_map": args.slot_map, "binary": args.binary, "image": args.image},
        L=args.L,
        k=args.k,
    )
    print(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
