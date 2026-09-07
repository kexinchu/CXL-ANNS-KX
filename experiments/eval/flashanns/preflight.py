"""Read-only, fail-closed validation of the FlashANNS live VMEM state."""

from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
import random
import struct
import sys
from pathlib import Path
from typing import Any

from experiments.eval.flashanns.config import load_configs
from experiments.eval.flashanns.layout import select_dataset_layout


class PreflightError(ValueError):
    """The live state does not satisfy the frozen evaluation contract."""


INTEGER_FIELDS = (
    "backing_count", "cache_limit", "cache_used", "dirty_bytes", "io_errors",
    "evictions", "ram_size", "ssd_size", "stripe_size",
)
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
            raw = _mmap_page(device, int(staging["offset"]))
            record["image_magic"] = struct.unpack_from("<Q", raw)[0]
        except (OSError, KeyError, TypeError, ValueError):
            record["image_magic"] = None
    return record


def _mmap_page(path: Path, offset: int) -> bytes:
    if offset < 0 or offset % 4096:
        raise PreflightError(f"{Path(path).name}: mmap offset is not page aligned")
    try:
        fd = os.open(Path(path), os.O_RDONLY)
        try:
            with mmap.mmap(
                fd,
                4096,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ,
                offset=offset,
            ) as page:
                return page[:]
        finally:
            os.close(fd)
    except OSError as exc:
        raise PreflightError(f"{Path(path).name}: {exc}") from exc


def sampled_layout_digest(path: Path, base_offset: int, length: int, pages: int, seed: int) -> str:
    if base_offset < 0 or base_offset % 4096 or length < 4096 or pages <= 0:
        raise PreflightError("invalid sampled-layout range")
    complete_pages = length // 4096
    sample_count = min(pages, complete_pages)
    page_ids = random.Random(seed).sample(range(complete_pages), sample_count)
    digest = hashlib.sha256()
    for page_id in page_ids:
        data = _mmap_page(path, base_offset + page_id * 4096)
        digest.update(struct.pack("<Q", page_id))
        digest.update(data)
    return digest.hexdigest()


def full_layout_digest(path: Path, base_offset: int, length: int, block_bytes: int = 64 << 20) -> str:
    if base_offset < 0 or length <= 0 or block_bytes <= 0:
        raise PreflightError("invalid full-layout range")
    page = mmap.PAGESIZE
    digest = hashlib.sha256()
    try:
        fd = os.open(Path(path), os.O_RDONLY)
        try:
            consumed = 0
            while consumed < length:
                logical_offset = base_offset + consumed
                map_base = logical_offset - logical_offset % page
                delta = logical_offset - map_base
                chunk = min(block_bytes, length - consumed)
                with mmap.mmap(
                    fd,
                    delta + chunk,
                    flags=mmap.MAP_SHARED,
                    prot=mmap.PROT_READ,
                    offset=map_base,
                ) as image:
                    digest.update(image[delta : delta + chunk])
                consumed += chunk
        finally:
            os.close(fd)
    except OSError as exc:
        raise PreflightError(f"{Path(path).name}: {exc}") from exc
    return digest.hexdigest()


def full_identity_record(contract: dict[str, Any], dataset: dict[str, Any]) -> dict[str, Any]:
    device = Path(contract["device"])
    sysfs = Path(contract["sysfs"])
    staging = dataset["staging"]
    host_image = Path(dataset["artifacts"][staging["host_artifact"]])
    before = snapshot(sysfs, device, dataset, read_device=False)
    if not before.get("device_exists"):
        raise PreflightError("device is missing")
    if before.get("open_users"):
        raise PreflightError(f"open_users is nonempty: {before['open_users']!r}")
    host_full = full_layout_digest(host_image, 0, int(staging["length"]))
    device_full = full_layout_digest(device, int(staging["offset"]), int(staging["length"]))
    record = snapshot(sysfs, device, dataset, read_device=True)
    record["host_digest"] = sampled_layout_digest(
        host_image, 0, int(staging["length"]), int(contract["sample_pages"]), int(contract["sample_seed"])
    )
    record["device_digest"] = sampled_layout_digest(
        device, int(staging["offset"]), int(staging["length"]), int(contract["sample_pages"]), int(contract["sample_seed"])
    )
    record["identity_scope"] = "full"
    record["layout"] = dataset.get("selected_layout", "extent")
    record["host_artifact"] = staging["host_artifact"]
    record["full_host_sha256"] = host_full
    record["full_device_sha256"] = device_full
    record["cold_parent_accepted"] = False
    record["identity_evidence_reused"] = False
    record["volatile_evidence_reused"] = False
    validate(record, contract, dataset, "post")
    if host_full != device_full:
        raise PreflightError("full staged-image digest mismatch")
    return record


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
    volatile_evidence: dict[str, Any] | None = None,
) -> dict[str, Any]:
    staging = dataset.get("staging", {})
    reuse_identity = bool(identity_evidence and identity_evidence.get("accepted"))
    selected_layout = dataset.get("selected_layout")
    selected_artifact = staging.get("host_artifact")
    if reuse_identity and selected_layout is not None:
        assert identity_evidence is not None
        if identity_evidence.get("layout") != selected_layout:
            raise PreflightError("identity evidence layout is stale")
        if identity_evidence.get("host_artifact") != selected_artifact:
            raise PreflightError("identity evidence host artifact is stale")
    record = snapshot(
        Path(contract["sysfs"]),
        Path(contract["device"]),
        dataset,
        read_device=not reuse_identity,
    )
    host_image = Path(dataset["artifacts"][staging["host_artifact"]])
    record["host_digest"] = sampled_layout_digest(
        host_image, 0, int(staging["length"]), int(contract["sample_pages"]), int(contract["sample_seed"])
    )
    record["device_digest"] = None
    record["identity_evidence_reused"] = reuse_identity
    record["volatile_evidence_reused"] = False
    record["layout"] = selected_layout or "extent"
    record["host_artifact"] = selected_artifact
    if state == "cold" and not reuse_identity:
        raise PreflightError("cold state requires accepted full-stage identity evidence")
    if reuse_identity:
        assert identity_evidence is not None
        mismatches = [
            field
            for field in ("backend", "backing_count", "nvme_dev", "target_bdf")
            if identity_evidence.get(field) != record.get(field)
        ]
        if mismatches:
            raise PreflightError(
                "identity evidence differs from current state: " + ", ".join(mismatches)
            )
        if identity_evidence.get("host_digest") != record["host_digest"]:
            raise PreflightError("identity evidence host digest is stale")
        if state == "cold" and (
            identity_evidence.get("identity_scope") != "full"
            or identity_evidence.get("full_host_sha256") != identity_evidence.get("full_device_sha256")
            or len(identity_evidence.get("full_host_sha256", "")) != 64
        ):
            raise PreflightError("cold state requires accepted full-stage identity evidence")
        record["image_magic"] = identity_evidence.get("image_magic")
        record["device_digest"] = identity_evidence.get("device_digest")
        if state == "cold":
            if not volatile_evidence or not volatile_evidence.get("accepted"):
                raise PreflightError("cold state requires accepted volatile RAM-stripe evidence")
            expected_layout = {
                name: record.get(name) for name in ("ram_size", "ssd_size", "stripe_size")
            }
            volatile_errors = []
            if volatile_evidence.get("layout") != expected_layout:
                volatile_errors.append("layout")
            if volatile_evidence.get("source") != str(host_image):
                volatile_errors.append("source")
            if selected_layout is not None and volatile_evidence.get("physical_layout") != selected_layout:
                volatile_errors.append("physical_layout")
            if selected_layout is not None and volatile_evidence.get("host_artifact") != selected_artifact:
                volatile_errors.append("host_artifact")
            if volatile_evidence.get("staging_offset") != int(staging["offset"]):
                volatile_errors.append("staging_offset")
            if volatile_evidence.get("staging_length") != int(staging["length"]):
                volatile_errors.append("staging_length")
            if volatile_evidence.get("source_ram_sha256") != volatile_evidence.get("device_ram_sha256"):
                volatile_errors.append("RAM digest")
            for name in ("cache_used", "dirty_bytes", "io_errors"):
                if volatile_evidence.get(name) != 0:
                    volatile_errors.append(name)
            if volatile_errors:
                raise PreflightError(
                    "volatile RAM-stripe evidence mismatch: " + ", ".join(volatile_errors)
                )
            record["volatile_evidence_reused"] = True
    elif record["device_exists"]:
        record["device_digest"] = sampled_layout_digest(
            Path(contract["device"]),
            int(staging["offset"]),
            int(staging["length"]),
            int(contract["sample_pages"]),
            int(contract["sample_seed"]),
        )
    record["cold_parent_accepted"] = bool(
        identity_evidence and identity_evidence.get("cold_parent_accepted")
    )
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
    parser.add_argument("--layout", choices=("extent", "original"), default="extent")
    parser.add_argument("--state", choices=("cold", "warm", "post"), required=True)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--identity-evidence", type=Path)
    parser.add_argument("--volatile-evidence", type=Path)
    parser.add_argument("--full-identity", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    datasets, _, _ = load_configs(root)
    contract = json.loads((Path(__file__).with_name("live-contract.json")).read_text())
    dataset = select_dataset_layout(datasets[args.dataset], args.layout)
    if args.full_identity:
        if args.state != "post" or args.identity_evidence or args.volatile_evidence:
            raise PreflightError("--full-identity requires --state post and no reused evidence")
        record = full_identity_record(contract, dataset)
    else:
        evidence = json.loads(args.identity_evidence.read_text()) if args.identity_evidence else None
        volatile = json.loads(args.volatile_evidence.read_text()) if args.volatile_evidence else None
        record = snapshot_and_validate(contract, dataset, args.state, evidence, volatile)
    record["accepted"] = True
    record["dataset"] = args.dataset
    record["state"] = args.state
    atomic_json_write(args.out, record)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (PreflightError, KeyError, OSError, ValueError) as exc:
        print(f"preflight failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
