"""Deterministic selection and orchestration for the C1 tier probe."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import random
import subprocess
import tempfile
from typing import Iterable


def select_offsets(
    region_offset: int,
    region_length: int,
    record_stride: int,
    count: int,
    seed: int,
    page_bytes: int = 4096,
) -> list[int]:
    if min(region_offset, region_length, record_stride, count) < 0 or record_stride == 0:
        raise ValueError("offset, length, stride, and count must be positive")
    vector_count = region_length // record_stride
    if vector_count == 0:
        raise ValueError("region contains no complete vector")
    if record_stride >= page_bytes:
        max_distinct = vector_count
    else:
        first_page = region_offset // page_bytes
        last_start = region_offset + (vector_count - 1) * record_stride
        max_distinct = last_start // page_bytes - first_page + 1
    if count > max_distinct:
        raise ValueError("not enough distinct vector pages")

    rng = random.Random(seed)
    selected: list[int] = []
    pages: set[int] = set()
    attempts = 0
    limit = max(10000, count * 100)
    while len(selected) < count and attempts < limit:
        offset = region_offset + rng.randrange(vector_count) * record_stride
        page = offset // page_bytes
        if page not in pages:
            pages.add(page)
            selected.append(offset)
        attempts += 1
    if len(selected) < count:
        for vector_id in range(vector_count):
            offset = region_offset + vector_id * record_stride
            page = offset // page_bytes
            if page not in pages:
                pages.add(page)
                selected.append(offset)
                if len(selected) == count:
                    break
    if len(selected) != count:
        raise ValueError("failed to select requested distinct pages")
    return selected


def validate_probe_geometry(record_stride: int, payload_bytes: int) -> None:
    if record_stride <= 0 or payload_bytes <= 0:
        raise ValueError("record stride and payload bytes must be positive")
    if payload_bytes > record_stride:
        raise ValueError("payload bytes cannot exceed record stride")


def percentile(values: Iterable[float], percent: float) -> float:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        raise ValueError("percentile of empty data")
    if not 0 <= percent <= 100:
        raise ValueError("percent must be in [0, 100]")
    position = (len(ordered) - 1) * percent / 100.0
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def classify_tier(vmem_faults: int, nvme_read_bytes: int) -> str:
    if vmem_faults < 0 or nvme_read_bytes < 0:
        raise ValueError("negative counters")
    if vmem_faults == 0 and nvme_read_bytes == 0:
        return "host"
    if vmem_faults > 0 and nvme_read_bytes == 0:
        return "cxl_cache"
    if vmem_faults > 0 and nvme_read_bytes > 0:
        return "flash"
    raise ValueError("inconsistent tier counters")


def run_probe(
    binary: Path,
    source: Path,
    offsets: list[int],
    record_stride: int,
    payload_bytes: int,
    tier: str,
    output: Path,
) -> None:
    if tier not in ("host", "cxl_cache", "flash"):
        raise ValueError(f"unknown tier {tier}")
    validate_probe_geometry(record_stride, payload_bytes)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="flashanns-tier-") as temporary:
        offsets_path = Path(temporary) / "offsets.txt"
        offsets_path.write_text("".join(f"{offset}\n" for offset in offsets), encoding="ascii")
        base = [
            str(binary), "--path", str(source), "--offsets", str(offsets_path),
            "--record-stride", str(record_stride), "--payload-bytes", str(payload_bytes),
        ]
        if tier == "cxl_cache":
            subprocess.run(base + ["--mode", "prepare-cache"], check=True)
        subprocess.run(base + ["--mode", tier, "--output", str(output)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--path", type=Path, required=True)
    parser.add_argument("--region-offset", type=int, required=True)
    parser.add_argument("--region-length", type=int, required=True)
    parser.add_argument("--record-stride", type=int, required=True)
    parser.add_argument("--payload-bytes", type=int, required=True)
    parser.add_argument("--tier", choices=("host", "cxl_cache", "flash"), required=True)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--count", type=int, default=2000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    offsets = select_offsets(
        args.region_offset, args.region_length, args.record_stride, args.count, args.seed
    )
    run_probe(
        args.binary, args.path, offsets, args.record_stride, args.payload_bytes,
        args.tier, args.output
    )
    result = json.loads(args.output.read_text(encoding="utf-8"))
    if result.get("offsets") != offsets:
        raise RuntimeError("probe output offsets differ from requested offsets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
