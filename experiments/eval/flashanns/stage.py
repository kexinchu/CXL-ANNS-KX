"""Stage one admitted fixed-stride image into the shared VMEM aperture."""

from __future__ import annotations

import argparse
import json
import mmap
import os
import random
import stat
import struct
import sys
from pathlib import Path
from typing import Any, Callable

from experiments.eval.flashanns.config import load_configs
from experiments.eval.flashanns.preflight import _open_users


class StageError(ValueError):
    """The image or target does not satisfy the staging contract."""


def stage_image(
    dataset: dict[str, Any],
    target: Path,
    *,
    require_char_device: bool = True,
    chunk_bytes: int = 2 << 20,
    progress: Callable[[int, int], None] | None = None,
) -> dict[str, int]:
    staging = dataset["staging"]
    source = Path(dataset["artifacts"][staging["host_artifact"]])
    target = Path(target)
    offset, length = int(staging["offset"]), int(staging["length"])
    if offset < 0 or offset % mmap.PAGESIZE or length < 4096 or chunk_bytes <= 0:
        raise StageError("invalid staging range or chunk size")
    if not source.is_file() or source.stat().st_size != length:
        actual = source.stat().st_size if source.exists() else None
        raise StageError(f"source length is {actual}, expected {length}")
    try:
        target_mode = target.stat().st_mode
    except OSError as exc:
        raise StageError(f"target is unavailable: {exc}") from exc
    if require_char_device and not stat.S_ISCHR(target_mode):
        raise StageError("target must be a character device")
    users = _open_users(target)
    if users:
        raise StageError(f"target has open users: {users!r}")

    expected_header = (
        int(staging["magic"]),
        1,
        int(dataset["count"]),
        int(dataset["dimension"]),
        32,
    )
    with source.open("rb") as source_stream:
        header = source_stream.read(24)
    if len(header) != 24 or struct.unpack("<QIIII", header) != expected_header:
        raise StageError("source layout header differs from the dataset contract")

    source_fd = -1
    target_fd = -1
    try:
        source_fd = os.open(source, os.O_RDONLY)
        target_fd = os.open(target, os.O_RDWR)
        with mmap.mmap(source_fd, length, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ) as src:
            with mmap.mmap(
                target_fd,
                length,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
                offset=offset,
            ) as dst:
                for start in range(0, length, chunk_bytes):
                    end = min(start + chunk_bytes, length)
                    dst[start:end] = src[start:end]
                    if progress:
                        progress(end, length)
                if dst[:24] != src[:24]:
                    raise StageError("staged header verification failed")
                n = int(dataset["count"])
                stride = (length - 4096) // n
                slot_ids = random.Random(1).sample(range(n), min(4, n))
                for slot_id in slot_ids:
                    start = 4096 + slot_id * stride
                    if dst[start : start + stride] != src[start : start + stride]:
                        raise StageError(f"staged slot verification failed at {slot_id}")
    except OSError as exc:
        raise StageError(f"staging I/O failed: {exc}") from exc
    finally:
        if target_fd >= 0:
            os.close(target_fd)
        if source_fd >= 0:
            os.close(source_fd)
    return {"copied_bytes": length, "staging_offset": offset, "verified_slots": min(4, int(dataset["count"]))}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--device", default="/dev/vmem0", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    datasets, _, _ = load_configs(root)
    if args.dataset not in datasets:
        raise StageError(f"unknown dataset {args.dataset}")

    last_report = -1
    def report(done: int, total: int) -> None:
        nonlocal last_report
        gib = done >> 30
        if done == total or gib != last_report:
            last_report = gib
            print(f"stage_progress bytes={done}/{total}", flush=True)

    record = stage_image(datasets[args.dataset], args.device, progress=report)
    record.update({"dataset": args.dataset, "device": str(args.device)})
    print(json.dumps(record, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (KeyError, OSError, StageError, ValueError) as exc:
        print(f"stage failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
