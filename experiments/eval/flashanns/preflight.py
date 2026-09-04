"""Read-only, fail-closed validation of the FlashANNS live VMEM state."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import struct
import sys
from pathlib import Path
from typing import Any

from experiments.eval.flashanns.config import load_configs


class PreflightError(ValueError):
    """The live state does not satisfy the frozen evaluation contract."""


INTEGER_FIELDS = ("backing_count", "cache_limit", "cache_used", "dirty_bytes", "io_errors", "evictions")
TEXT_FIELDS = ("backend", "nvme_dev", "target_bdf")


def _read_field(sysfs: Path, name: str) -> Any:
    path = sysfs / name
    try:
        text = path.read_text().strip()
    except OSError:
        return None
    if name in INTEGER_FIELDS:
        try:
            return int(text)
        except ValueError:
            return None
    if name in ("nvme_dev", "target_bdf"):
        return [part.strip() for part in text.split(",") if part.strip()]
    return text


def _open_users(device: Path) -> list[int]:
    if not device.exists():
        return []
    target = str(device.resolve())
    users: set[int] = set()
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            for fd in (proc / "fd").iterdir():
                try:
                    if str(fd.resolve()) == target:
                        users.add(int(proc.name))
                except OSError:
                    continue
        except (OSError, ValueError):
            continue
    return sorted(users)


def snapshot(sysfs: Path, device: Path, dataset: dict[str, Any], read_device: bool = True) -> dict[str, Any]:
    sysfs, device = Path(sysfs), Path(device)
    record = {name: _read_field(sysfs, name) for name in (*TEXT_FIELDS, *INTEGER_FIELDS)}
    record["device_exists"] = device.exists()
    record["open_users"] = _open_users(device)
    record["image_magic"] = None
    staging = dataset.get("staging", {})
    if read_device and record["device_exists"]:
        try:
            fd = os.open(device, os.O_RDONLY)
            try:
                raw = os.pread(fd, 8, int(staging["offset"]))
            finally:
                os.close(fd)
            if len(raw) == 8:
                record["image_magic"] = struct.unpack("<Q", raw)[0]
        except (OSError, KeyError, TypeError, ValueError):
            record["image_magic"] = None
    return record


def sampled_layout_digest(path: Path, base_offset: int, length: int, pages: int, seed: int) -> str:
    if base_offset < 0 or length < 4096 or pages <= 0:
        raise PreflightError("invalid sampled-layout range")
    complete_pages = length // 4096
    sample_count = min(pages, complete_pages)
    page_ids = random.Random(seed).sample(range(complete_pages), sample_count)
    digest = hashlib.sha256()
    try:
        fd = os.open(Path(path), os.O_RDONLY)
        try:
            for page_id in page_ids:
                data = os.pread(fd, 4096, base_offset + page_id * 4096)
                if len(data) != 4096:
                    raise PreflightError(f"{Path(path).name}: short sampled page {page_id}")
                digest.update(struct.pack("<Q", page_id))
                digest.update(data)
        finally:
            os.close(fd)
    except OSError as exc:
        raise PreflightError(f"{Path(path).name}: {exc}") from exc
    return digest.hexdigest()


def validate(record: dict[str, Any], contract: dict[str, Any], dataset: dict[str, Any], state: str) -> None:
    errors: list[str] = []
    if not record.get("device_exists"):
        errors.append("device is missing")
    for field in ("backend", "backing_count", "nvme_dev", "target_bdf", "cache_limit"):
        if record.get(field) != contract.get(field):
            errors.append(f"{field} is {record.get(field)!r}, expected {contract.get(field)!r}")
    if record.get("dirty_bytes") != contract.get("required_dirty_bytes"):
        errors.append(f"dirty_bytes is {record.get('dirty_bytes')!r}")
    if record.get("io_errors") != contract.get("required_io_errors"):
        errors.append(f"io_errors is {record.get('io_errors')!r}")
    if record.get("open_users"):
        errors.append(f"open_users is nonempty: {record['open_users']!r}")
    expected_magic = dataset.get("staging", {}).get("magic")
    if record.get("image_magic") != expected_magic:
        errors.append(f"image_magic is {record.get('image_magic')!r}, expected {expected_magic!r}")
    if record.get("host_digest") != record.get("device_digest"):
        errors.append("sampled staged-image digest mismatch")
    if state == "cold" and record.get("cache_used") != 0:
        errors.append(f"cache_used is {record.get('cache_used')!r}, expected 0 for cold")
    if state == "warm" and not record.get("cold_parent_accepted"):
        errors.append("warm state lacks accepted cold-parent evidence")
    if state not in ("cold", "warm", "post"):
        errors.append(f"unknown state {state!r}")
    if errors:
        raise PreflightError("; ".join(errors))


def snapshot_and_validate(
    contract: dict[str, Any],
    dataset: dict[str, Any],
    state: str,
    identity_evidence: dict[str, Any] | None = None,
) -> dict[str, Any]:
    staging = dataset.get("staging", {})
    record = snapshot(Path(contract["sysfs"]), Path(contract["device"]), dataset)
    host_image = Path(dataset["artifacts"][staging["host_artifact"]])
    record["host_digest"] = sampled_layout_digest(
        host_image, 0, int(staging["length"]), int(contract["sample_pages"]), int(contract["sample_seed"])
    )
    record["device_digest"] = None
    if record["device_exists"]:
        record["device_digest"] = sampled_layout_digest(
            Path(contract["device"]),
            int(staging["offset"]),
            int(staging["length"]),
            int(contract["sample_pages"]),
            int(contract["sample_seed"]),
        )
    record["cold_parent_accepted"] = bool(identity_evidence and identity_evidence.get("accepted"))
    validate(record, contract, dataset, state)
    return record


def atomic_json_write(path: Path, value: dict[str, Any]) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(str(path) + ".tmp")
    payload = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()
    try:
        with tmp.open("wb") as dst:
            dst.write(payload)
            dst.flush()
            os.fsync(dst.fileno())
        os.replace(tmp, path)
        directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except OSError:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--state", choices=("cold", "warm", "post"), required=True)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--identity-evidence", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    datasets, _, _ = load_configs(root)
    contract = json.loads((Path(__file__).with_name("live-contract.json")).read_text())
    evidence = json.loads(args.identity_evidence.read_text()) if args.identity_evidence else None
    record = snapshot_and_validate(contract, datasets[args.dataset], args.state, evidence)
    atomic_json_write(args.out, record)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (PreflightError, KeyError, OSError, ValueError) as exc:
        print(f"preflight failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
