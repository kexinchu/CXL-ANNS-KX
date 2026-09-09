"""Run and seal the cold, SSD-only C3 capability sweep."""

from __future__ import annotations

import argparse
from array import array
import json
from pathlib import Path
import subprocess
from typing import Iterable, Sequence

from .contract import load_contract
from .observe import block_delta, find_open_users, read_block_snapshot, sha256_file
from .records import seal_record, validate_block, validate_record
from .run_c2_campaign import EXPECTED_DRIVER_SRCVERSION
from .run_qd import (
    BlockQdSampler,
    capability_probe_command,
    capability_schedule,
    mean_entire_interval_qd,
    select_ssd_pages,
    validate_capability_block,
)


def relative_ram_ranges(
    image_offset: int, image_bytes: int, segments: Iterable[dict]
) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    image_stop = image_offset + image_bytes
    for segment in segments:
        start = int(segment["logical_offset"])
        stop = start + int(segment["length"])
        if start < image_offset or stop > image_stop or start >= stop:
            raise ValueError("RAM segment is outside the staged image")
        result.append((start - image_offset, stop - image_offset))
    return result


def build_capability_record(
    *, run_id: str, repeat: int, concurrency: int, binary_sha256: str,
    image_sha256: str, pages_sha256: str, requested_queries: int,
    completed_queries: int, before_faults: int, after_faults: int,
    before_read_bytes: int, after_read_bytes: int, block_delta: dict,
    overlapping_processes: list[dict], bandwidth_gib_s: float,
    mean_aqu_sz: float, working_set_bytes: int, cold_evidence: str,
    driver_sha256: str,
) -> dict:
    contract = load_contract()
    return {
        "schema_version": contract.schema_version,
        "run_id": run_id,
        "phase": "c3_capability",
        "dataset": "laion10m",
        "condition": f"c{concurrency}",
        "repeat": repeat,
        "state": "complete",
        "identities": {
            "binary_sha256": binary_sha256,
            "image_sha256": image_sha256,
            "query_ids_sha256": pages_sha256,
        },
        "geometry": {
            "cache_bytes": contract.cache_bytes,
            "page_bytes": contract.page_bytes,
            "stripe_bytes": contract.stripe_bytes,
        },
        "operations": {
            "requested": requested_queries,
            "completed": completed_queries,
        },
        "counters": {
            "before": {
                "vmem_faults": before_faults,
                "nvme_read_bytes": before_read_bytes,
            },
            "after": {
                "vmem_faults": after_faults,
                "nvme_read_bytes": after_read_bytes,
            },
            "block_delta": block_delta,
        },
        "exclusive": {
            "overlapping_processes": overlapping_processes,
            "other_io_detected": block_delta.get("total_write_bytes") != 0,
            "backings": sorted(
                key for key in block_delta if key not in ("total_read_bytes", "total_write_bytes")
            ),
        },
        "metrics": {
            "issued_concurrency": concurrency,
            "bandwidth_gib_s": bandwidth_gib_s,
            "mean_aqu_sz": mean_aqu_sz,
            "working_set_bytes": working_set_bytes,
        },
        "sidecars": {"pages_sha256": pages_sha256},
        "driver": {
            "srcversion": EXPECTED_DRIVER_SRCVERSION,
            "module_sha256": driver_sha256,
            "cold_evidence": cold_evidence,
        },
        "validation": {"status": "pending"},
    }


def _read_bytes(snapshot: dict[str, dict[str, int]]) -> int:
    return sum(values["read_sectors"] * 512 for values in snapshot.values())


def _read_faults(sysfs: Path) -> int:
    return int((sysfs / "faults").read_text(encoding="utf-8").strip())


def _write_u64(path: Path, values: Iterable[int]) -> None:
    payload = array("Q", values)
    path.write_bytes(payload.tobytes())


def _write_json(path: Path, value: dict | list) -> None:
    if path.exists():
        raise FileExistsError(f"refusing existing output {path}")
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _reset(repo: Path, evidence: Path) -> None:
    script = (
        'source "$1/tools/eval_host_cold_lib.sh"; '
        'eval_reset_and_restore "$1" laion10m "$2" 4 extent'
    )
    subprocess.run(["bash", "-c", script, "bash", str(repo), str(evidence)], check=True)


def _assert_cold(sysfs: Path, device: Path, module: Path) -> None:
    if Path("/sys/module/vmem_sw/srcversion").read_text(encoding="utf-8").strip() != EXPECTED_DRIVER_SRCVERSION:
        raise RuntimeError("loaded VMEM module is not the frozen C3EF implementation")
    if sha256_file(module) != "5b7d7b2efc04fae182e41d5fcf29dd26ebef32341e41b6f369050f3ce5cf945f":
        raise RuntimeError("frozen VMEM module hash changed")
    for name in ("cache_used", "dirty_bytes", "io_errors"):
        value = int((sysfs / name).read_text(encoding="utf-8").strip())
        if value != 0:
            raise RuntimeError(f"cold-state gate failed: {name}={value}")
    users = find_open_users(device)
    if users:
        raise RuntimeError(f"device has open users after reset: {users}")


def execute(args: argparse.Namespace) -> None:
    contract = load_contract()
    levels = tuple(contract.phases["c3_capability"]["concurrency"])
    binary_sha256 = sha256_file(args.binary)
    driver_sha256 = sha256_file(args.module)
    minimum_pages = contract.cache_bytes // contract.page_bytes + 1
    accepted_records: list[dict] = []

    for repeat, concurrency in capability_schedule(levels, args.repeats):
        run_id = f"{args.tag}-laion10m-c3_capability-c{concurrency}-r{repeat}"
        accepted_path = args.output_root / "accepted" / "c3_capability" / run_id / "run.json"
        if accepted_path.exists():
            record = json.loads(accepted_path.read_text(encoding="utf-8"))
            validate_record(record)
            accepted_records.append(record)
            print(json.dumps({"run_id": run_id, "status": "already-accepted"}), flush=True)
            continue

        run_root = args.output_root / "raw" / "c3_capability" / run_id
        if run_root.exists():
            raise FileExistsError(f"preserving incomplete or rejected run {run_root}")
        cold_evidence = args.output_root / "preflight" / "c3-cold" / f"{run_id}.json"
        cold_evidence.parent.mkdir(parents=True, exist_ok=True)
        _reset(args.repo, cold_evidence)
        _assert_cold(args.vmem_sysfs, args.device, args.module)

        cold = json.loads(cold_evidence.read_text(encoding="utf-8"))
        ram_ranges = relative_ram_ranges(args.image_offset, args.image_bytes, cold["segments"])
        page_count = ((minimum_pages + concurrency - 1) // concurrency) * concurrency
        pages = select_ssd_pages(
            image_bytes=args.image_bytes,
            ram_ranges=ram_ranges,
            page_count=page_count,
            seed=repeat * 1000 + concurrency,
            cache_bytes=contract.cache_bytes,
            page_bytes=contract.page_bytes,
        )
        query_count = page_count // concurrency

        run_root.mkdir(parents=True, exist_ok=False)
        pages_path = run_root / "pages.u64"
        offsets_path = run_root / "query_offsets.u64"
        probe_path = run_root / "probe.json"
        _write_u64(pages_path, pages)
        _write_u64(offsets_path, range(0, page_count + 1, concurrency))
        pages_sha256 = sha256_file(pages_path)

        before_users = find_open_users(args.device)
        for backing in args.backings:
            before_users.extend(find_open_users(Path("/dev") / backing))
        before_block = read_block_snapshot(args.backings)
        before_faults = _read_faults(args.vmem_sysfs)
        sampler = BlockQdSampler(args.backings)
        sampler.start()
        try:
            subprocess.run(capability_probe_command(
                binary=args.binary,
                device=args.device,
                image_offset=args.image_offset,
                image_bytes=args.image_bytes,
                query_offsets=offsets_path,
                pages=pages_path,
                output=probe_path,
                wave_pages=concurrency,
            ), check=True)
        finally:
            qd = sampler.stop()
            _write_json(run_root / "qd.json", qd)
        after_block = read_block_snapshot(args.backings)
        after_faults = _read_faults(args.vmem_sysfs)
        after_users = find_open_users(args.device)
        for backing in args.backings:
            after_users.extend(find_open_users(Path("/dev") / backing))

        probe = json.loads(probe_path.read_text(encoding="utf-8"))
        if probe.get("wave_pages") != concurrency:
            raise RuntimeError("probe did not preserve issued concurrency")
        if probe.get("fallback_waves") != 0:
            raise RuntimeError("SSD-only capability run unexpectedly used mmap fallback")
        if probe.get("completed_queries") != query_count:
            raise RuntimeError("capability probe did not complete every query")
        elapsed_ns = sum(int(value) for value in probe["latency_ns"])
        if elapsed_ns <= 0:
            raise RuntimeError("capability probe has invalid elapsed time")

        delta = block_delta(before_block, after_block)
        record = build_capability_record(
            run_id=run_id,
            repeat=repeat,
            concurrency=concurrency,
            binary_sha256=binary_sha256,
            image_sha256=args.image_sha256,
            pages_sha256=pages_sha256,
            requested_queries=query_count,
            completed_queries=probe["completed_queries"],
            before_faults=before_faults,
            after_faults=after_faults,
            before_read_bytes=_read_bytes(before_block),
            after_read_bytes=_read_bytes(after_block),
            block_delta=delta,
            overlapping_processes=before_users + after_users,
            bandwidth_gib_s=page_count * contract.page_bytes / (1024**3) / (elapsed_ns / 1e9),
            mean_aqu_sz=mean_entire_interval_qd(qd),
            working_set_bytes=page_count * contract.page_bytes,
            cold_evidence=str(cold_evidence),
            driver_sha256=driver_sha256,
        )
        raw_record = run_root / "run.json"
        _write_json(raw_record, record)
        sealed = seal_record(raw_record, args.output_root / "accepted" / "c3_capability")
        accepted = json.loads(sealed.read_text(encoding="utf-8"))
        accepted_records.append(accepted)
        print(json.dumps({
            "run_id": run_id,
            "accepted": str(sealed),
            **accepted["metrics"],
            "nvme_read_bytes": delta["total_read_bytes"],
        }), flush=True)

    for concurrency in levels:
        validate_block([
            record for record in accepted_records
            if record["metrics"]["issued_concurrency"] == concurrency
        ])
    validate_capability_block([
        {
            "concurrency": record["metrics"]["issued_concurrency"],
            "repeat": record["repeat"],
            "bandwidth_gib_s": record["metrics"]["bandwidth_gib_s"],
        }
        for record in accepted_records
    ], levels)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--device", type=Path, default=Path("/dev/vmem0"))
    parser.add_argument("--vmem-sysfs", type=Path, default=Path("/sys/class/vmem/vmem0"))
    parser.add_argument("--backings", nargs=2, required=True)
    parser.add_argument("--image-offset", type=int, required=True)
    parser.add_argument("--image-bytes", type=int, required=True)
    parser.add_argument("--image-sha256", required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    execute(parser.parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
