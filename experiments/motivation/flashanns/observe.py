"""Read-only platform observation helpers for Motivation experiments."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any, Iterable


_VMEM_FIELDS = ("cache_limit", "cache_used", "dirty_bytes", "io_errors")
_BLOCK_FIELDS = (
    "read_ios",
    "read_merges",
    "read_sectors",
    "read_ticks_ms",
    "write_ios",
    "write_merges",
    "write_sectors",
    "write_ticks_ms",
    "in_flight",
    "io_ticks_ms",
    "weighted_io_ticks_ms",
)


def sha256_file(path: Path | str, chunk_bytes: int = 8 * 1024**2) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while chunk := stream.read(chunk_bytes):
            digest.update(chunk)
    return digest.hexdigest()


def read_vmem_snapshot(sysfs_root: Path | str) -> dict[str, int]:
    root = Path(sysfs_root)
    return {field: int((root / field).read_text().strip()) for field in _VMEM_FIELDS}


def read_block_snapshot(
    devices: Iterable[str], sys_block_root: Path | str = "/sys/class/block"
) -> dict[str, dict[str, int]]:
    root = Path(sys_block_root)
    snapshot: dict[str, dict[str, int]] = {}
    for device in devices:
        values = [int(value) for value in (root / device / "stat").read_text().split()]
        if len(values) < len(_BLOCK_FIELDS):
            raise ValueError(f"short block stat for {device}")
        snapshot[device] = dict(zip(_BLOCK_FIELDS, values))
    return snapshot


def block_delta(
    before: dict[str, dict[str, int]], after: dict[str, dict[str, int]]
) -> dict[str, Any]:
    if set(before) != set(after):
        raise ValueError("block snapshot device set changed")
    result: dict[str, Any] = {}
    for device in sorted(before):
        if set(before[device]) != set(after[device]):
            raise ValueError(f"block counter set changed for {device}")
        delta = {field: after[device][field] - before[device][field] for field in before[device]}
        if any(value < 0 for field, value in delta.items() if field != "in_flight"):
            raise ValueError(f"negative block counter delta for {device}")
        result[device] = delta
    result["total_read_bytes"] = sum(
        result[device]["read_sectors"] * 512 for device in before
    )
    result["total_write_bytes"] = sum(
        result[device]["write_sectors"] * 512 for device in before
    )
    return result


def verify_dual_backing(identities: Iterable[dict[str, str]]) -> None:
    items = list(identities)
    if len(items) != 2:
        raise ValueError("exactly two backing devices are required")
    for field in ("name", "serial", "bdf"):
        values = [item.get(field, "") for item in items]
        if not all(values) or len(set(values)) != 2:
            raise ValueError(f"backing {field} values must be distinct")


def find_open_users(device: Path | str, proc_root: Path | str = "/proc") -> list[dict[str, Any]]:
    target = Path(device).resolve()
    users: list[dict[str, Any]] = []
    for process in sorted(Path(proc_root).iterdir(), key=lambda path: path.name):
        if not process.name.isdigit() or not (process / "fd").is_dir():
            continue
        for descriptor in (process / "fd").iterdir():
            try:
                matches = descriptor.resolve() == target
            except (FileNotFoundError, PermissionError):
                continue
            if not matches:
                continue
            try:
                command = (process / "cmdline").read_bytes().rstrip(b"\0").replace(b"\0", b" ").decode(
                    "utf-8", errors="replace"
                )
            except (FileNotFoundError, PermissionError):
                command = ""
            users.append({"pid": int(process.name), "fd": int(descriptor.name), "command": command})
    return users


def parse_iostat(text: str, devices: Iterable[str]) -> dict[str, Any]:
    selected = set(devices)
    header: list[str] | None = None
    current: list[dict[str, float]] = []
    intervals: list[dict[str, float]] = []

    def finish_interval() -> None:
        if not current:
            return
        present = {str(row["device"]) for row in current}
        if present != selected:
            raise ValueError("iostat interval is missing a backing device")
        intervals.append(
            {
                "aqu_sz": sum(float(row["aqu_sz"]) for row in current),
                "read_iops": sum(float(row["r/s"]) for row in current),
                "read_kib_s": sum(float(row["rkb/s"]) for row in current),
            }
        )
        current.clear()

    for raw_line in text.splitlines():
        fields = raw_line.split()
        if not fields:
            continue
        if fields[0].lower() == "device":
            finish_interval()
            header = [field.lower().replace("-", "_") for field in fields]
            continue
        if header is None or fields[0] not in selected:
            continue
        if len(fields) != len(header):
            raise ValueError("iostat row does not match header")
        row = dict(zip(header, fields))
        aqu_key = "aqu_sz" if "aqu_sz" in row else "avgqu_sz"
        read_kib_key = "rkb/s" if "rkb/s" in row else "rkb_s"
        current.append(
            {
                "device": fields[0],
                "aqu_sz": float(row[aqu_key]),
                "r/s": float(row["r/s"]),
                "rkb/s": float(row[read_kib_key]),
            }
        )
    finish_interval()
    if not intervals:
        raise ValueError("no complete iostat intervals")
    return {
        "intervals": intervals,
        "mean_aqu_sz": sum(row["aqu_sz"] for row in intervals) / len(intervals),
        "active_only": False,
    }
