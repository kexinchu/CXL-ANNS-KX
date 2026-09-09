"""Execute and seal a five-repeat C1 three-tier measurement block."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import statistics
from typing import Iterable

from .observe import block_delta, find_open_users, read_block_snapshot, sha256_file
from .records import RecordValidationError, seal_record
from .run_tier import percentile, run_probe, select_offsets


def partition_offsets(
    region_offset: int,
    region_length: int,
    record_stride: int,
    repeats: int,
    count: int,
    seed: int,
    excluded_offsets: Iterable[int] = (),
) -> list[list[int]]:
    excluded_pages = {int(offset) // 4096 for offset in excluded_offsets}
    candidates = select_offsets(
        region_offset,
        region_length,
        record_stride,
        repeats * count + len(excluded_pages),
        seed,
    )
    selected = [offset for offset in candidates if offset // 4096 not in excluded_pages]
    if len(selected) < repeats * count:
        raise ValueError("not enough pages remain after exclusions")
    return [selected[index * count:(index + 1) * count] for index in range(repeats)]


def latency_metrics(latency_ns: Iterable[int]) -> dict[str, float]:
    values = [float(value) / 1000.0 for value in latency_ns]
    if not values:
        raise ValueError("empty latency vector")
    return {
        "latency_us_mean": statistics.fmean(values),
        "latency_us_p50": percentile(values, 50),
        "latency_us_p95": percentile(values, 95),
        "latency_us_p99": percentile(values, 99),
    }


def offsets_sha256(offsets: Iterable[int]) -> str:
    digest = hashlib.sha256()
    for offset in offsets:
        digest.update(f"{int(offset)}\n".encode("ascii"))
    return digest.hexdigest()


def _faults(sysfs: Path) -> int:
    return int((sysfs / "faults").read_text().strip())


def _cumulative_read_bytes(snapshot: dict[str, dict[str, int]]) -> int:
    return sum(values["read_sectors"] * 512 for values in snapshot.values())


def _write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(f"refusing existing output {path}")
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def execute(args: argparse.Namespace) -> None:
    if find_open_users(args.device):
        raise RuntimeError(f"{args.device} has an open user before campaign")
    binary_hash = sha256_file(args.binary)
    excluded: list[int] = []
    for path in args.exclude_probe:
        probe = json.loads(path.read_text(encoding="utf-8"))
        excluded.extend(int(offset) - args.staging_offset for offset in probe["offsets"])
    groups = partition_offsets(
        args.vector_offset,
        args.region_length,
        args.record_stride,
        args.repeats,
        args.count,
        args.seed,
        excluded,
    )

    for repeat, relative_offsets in enumerate(groups):
        offset_hash = offsets_sha256(relative_offsets)
        device_offsets = [args.staging_offset + offset for offset in relative_offsets]
        for condition in ("flash", "cxl_cache", "host"):
            run_id = f"{args.tag}-{args.dataset}-{condition}-r{repeat}"
            run_root = args.output_root / "raw" / "c1_tier" / run_id
            probe_path = run_root / "probe.json"
            before_block = read_block_snapshot(args.backings)
            before_faults = _faults(args.vmem_sysfs)
            before_users = find_open_users(args.device)
            if before_users:
                raise RuntimeError(f"{args.device} became busy before {run_id}: {before_users}")
            source = args.host_image if condition == "host" else args.device
            offsets = relative_offsets if condition == "host" else device_offsets
            run_probe(
                args.binary,
                source,
                offsets,
                args.record_stride,
                args.payload_bytes,
                condition,
                probe_path,
            )
            after_block = read_block_snapshot(args.backings)
            after_faults = _faults(args.vmem_sysfs)
            after_users = find_open_users(args.device)
            probe = json.loads(probe_path.read_text(encoding="utf-8"))
            delta = block_delta(before_block, after_block)
            record = {
                "schema_version": 1,
                "run_id": run_id,
                "phase": "c1_tier",
                "dataset": args.dataset,
                "condition": condition,
                "repeat": repeat,
                "state": "complete",
                "identities": {
                    "binary_sha256": binary_hash,
                    "image_sha256": args.image_sha256,
                    "query_ids_sha256": offset_hash,
                },
                "geometry": {
                    "cache_bytes": 4 * 1024**3,
                    "page_bytes": 4096,
                    "stripe_bytes": 2 * 1024**2,
                    "record_stride": args.record_stride,
                    "payload_bytes": args.payload_bytes,
                },
                "operations": {
                    "requested": args.count,
                    "completed": len(probe["latency_ns"]),
                },
                "counters": {
                    "before": {
                        "vmem_faults": before_faults,
                        "nvme_read_bytes": _cumulative_read_bytes(before_block),
                    },
                    "after": {
                        "vmem_faults": after_faults,
                        "nvme_read_bytes": _cumulative_read_bytes(after_block),
                    },
                    "block_delta": delta,
                },
                "exclusive": {
                    "overlapping_processes": before_users + after_users,
                    "other_io_detected": False,
                    "backings": list(args.backings),
                },
                "metrics": latency_metrics(probe["latency_ns"]),
                "validation": {"status": "pending"},
            }
            raw_record = run_root / "run.json"
            _write_json(raw_record, record)
            try:
                accepted = seal_record(raw_record, args.output_root / "accepted" / "c1_tier")
            except RecordValidationError as error:
                record["validation"] = {"status": "rejected", "reason": str(error)}
                raw_record.write_text(
                    json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                )
                raise
            print(json.dumps({"run_id": run_id, "accepted": str(accepted), **record["metrics"]}))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--device", type=Path, default=Path("/dev/vmem0"))
    parser.add_argument("--vmem-sysfs", type=Path, default=Path("/sys/class/vmem/vmem0"))
    parser.add_argument("--backings", nargs=2, required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--host-image", type=Path, required=True)
    parser.add_argument("--image-sha256", required=True)
    parser.add_argument("--staging-offset", type=int, required=True)
    parser.add_argument("--vector-offset", type=int, required=True)
    parser.add_argument("--region-length", type=int, required=True)
    parser.add_argument("--record-stride", type=int, required=True)
    parser.add_argument("--payload-bytes", type=int, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--count", type=int, default=2000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--exclude-probe", type=Path, action="append", default=[])
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    execute(parser.parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
