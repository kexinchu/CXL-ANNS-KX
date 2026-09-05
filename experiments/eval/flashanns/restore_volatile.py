"""Restore only volatile RAM-tier intersections of a staged VMEM extent."""

from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
import stat
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from experiments.eval.flashanns.config import load_configs
from experiments.eval.flashanns.preflight import atomic_json_write


@dataclass(frozen=True)
class Segment:
    logical_offset: int
    source_offset: int
    length: int


def ram_segments(
    *, offset: int, length: int, ram_size: int, ssd_size: int, stripe_size: int
) -> list[Segment]:
    """Return dataset subranges mapped to RAM by vmem_sw_layout_map()."""
    if min(offset, length, ram_size, ssd_size) < 0 or stripe_size <= 0:
        raise ValueError("invalid VMEM layout or extent")
    logical_size = ram_size + ssd_size
    if offset + length > logical_size:
        raise ValueError("dataset extent exceeds VMEM logical size")
    ram_stripes = ram_size // stripe_size
    ssd_stripes = ssd_size // stripe_size
    full_stripes = ram_stripes + ssd_stripes
    if full_stripes == 0:
        return []
    full_span = full_stripes * stripe_size
    end = min(offset + length, full_span)
    first = offset // stripe_size
    last = (end + stripe_size - 1) // stripe_size
    result: list[Segment] = []
    for stripe in range(first, last):
        ram_before = stripe * ram_stripes // full_stripes
        ram_after = (stripe + 1) * ram_stripes // full_stripes
        if ram_after == ram_before:
            continue
        start = max(offset, stripe * stripe_size)
        stop = min(end, (stripe + 1) * stripe_size)
        if start < stop:
            result.append(Segment(start, start - offset, stop - start))
    return result


def _read_u64(path: Path) -> int:
    return int(path.read_text(encoding="utf-8").strip())


def _mapped_bytes(fd: int, offset: int, length: int, writable: bool) -> tuple[mmap.mmap, int]:
    page = mmap.PAGESIZE
    base = offset - offset % page
    delta = offset - base
    prot = mmap.PROT_READ | (mmap.PROT_WRITE if writable else 0)
    return mmap.mmap(fd, delta + length, mmap.MAP_SHARED, prot, offset=base), delta


def restore_volatile_stripes(
    *, source: Path, device: Path, sysfs: Path, offset: int, length: int
) -> dict[str, Any]:
    if source.stat().st_size != length:
        raise ValueError(f"source size must equal staged length {length}")
    if not stat.S_ISCHR(device.stat().st_mode):
        raise ValueError(f"device is not a character device: {device}")
    busy = subprocess.run(["fuser", str(device)], capture_output=True, text=True, check=False)
    if busy.returncode == 0:
        raise RuntimeError(f"device has open users: {busy.stdout.strip()} {busy.stderr.strip()}")

    layout = {
        "ram_size": _read_u64(sysfs / "ram_size"),
        "ssd_size": _read_u64(sysfs / "ssd_size"),
        "stripe_size": _read_u64(sysfs / "stripe_size"),
    }
    segments = ram_segments(offset=offset, length=length, **layout)
    source_hash = hashlib.sha256()
    device_hash = hashlib.sha256()
    sfd = os.open(source, os.O_RDONLY)
    dfd = os.open(device, os.O_RDWR)
    try:
        for segment in segments:
            payload = os.pread(sfd, segment.length, segment.source_offset)
            if len(payload) != segment.length:
                raise OSError(f"short source read at {segment.source_offset}")
            source_hash.update(payload)
            mapped, delta = _mapped_bytes(dfd, segment.logical_offset, segment.length, True)
            try:
                mapped[delta : delta + segment.length] = payload
                mapped.flush()
            finally:
                mapped.close()
        os.fsync(dfd)
        for segment in segments:
            mapped, delta = _mapped_bytes(dfd, segment.logical_offset, segment.length, False)
            try:
                device_hash.update(mapped[delta : delta + segment.length])
            finally:
                mapped.close()
    finally:
        os.close(dfd)
        os.close(sfd)

    result: dict[str, Any] = {
        "accepted": source_hash.hexdigest() == device_hash.hexdigest(),
        "device": str(device),
        "source": str(source),
        "staging_offset": offset,
        "staging_length": length,
        "layout": layout,
        "segment_count": len(segments),
        "restored_bytes": sum(segment.length for segment in segments),
        "source_ram_sha256": source_hash.hexdigest(),
        "device_ram_sha256": device_hash.hexdigest(),
        "cache_used": _read_u64(sysfs / "cache_used"),
        "dirty_bytes": _read_u64(sysfs / "dirty_bytes"),
        "io_errors": _read_u64(sysfs / "io_errors"),
        "segments": [asdict(segment) for segment in segments],
    }
    if not result["accepted"]:
        raise RuntimeError("RAM-tier restoration digest mismatch")
    if result["cache_used"] != 0 or result["dirty_bytes"] != 0 or result["io_errors"] != 0:
        raise RuntimeError(
            "RAM-tier restoration changed cold/error state: "
            f"cache_used={result['cache_used']} dirty_bytes={result['dirty_bytes']} "
            f"io_errors={result['io_errors']}"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--device", type=Path, default=Path("/dev/vmem0"))
    parser.add_argument("--sysfs", type=Path, default=Path("/sys/class/vmem/vmem0"))
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    datasets, _, _ = load_configs(root)
    dataset = datasets[args.dataset]
    staging = dataset["staging"]
    source = Path(dataset["artifacts"][staging["host_artifact"]])
    layout = {
        "ram_size": _read_u64(args.sysfs / "ram_size"),
        "ssd_size": _read_u64(args.sysfs / "ssd_size"),
        "stripe_size": _read_u64(args.sysfs / "stripe_size"),
    }
    segments = ram_segments(offset=staging["offset"], length=staging["length"], **layout)
    if not args.write:
        print(json.dumps({"dataset": args.dataset, "segment_count": len(segments), "restored_bytes": sum(s.length for s in segments), "write": False}, sort_keys=True))
        return 0
    result = restore_volatile_stripes(
        source=source,
        device=args.device,
        sysfs=args.sysfs,
        offset=staging["offset"],
        length=staging["length"],
    )
    result["dataset"] = args.dataset
    if args.out:
        atomic_json_write(args.out, result)
    print(json.dumps({key: result[key] for key in ("accepted", "dataset", "segment_count", "restored_bytes", "cache_used")}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
