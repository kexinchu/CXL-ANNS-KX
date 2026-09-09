"""Run the cold, same-trace C2 replay campaign and seal accepted evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import statistics
import subprocess
from typing import Iterable, Sequence

from .observe import block_delta, find_open_users, read_block_snapshot, sha256_file
from .records import seal_record, validate_block, validate_c2_same_trace, validate_record
from .run_tier import percentile


ADMISSION = ("selective_4k", "blind_16k")
COVERAGE = ("demand", "top1", "top2", "top8")
EXPECTED_DRIVER_SRCVERSION = "C3EFE5700757D87BABCC26C"


def phase_for_policy(policy: str) -> str:
    if policy in ADMISSION:
        return "c2_admission"
    if policy in COVERAGE:
        return "c2_coverage"
    raise ValueError(f"unknown policy {policy}")


def build_schedule(policies: Sequence[str], repeats: int) -> list[tuple[int, str]]:
    if not policies or repeats <= 0:
        raise ValueError("schedule requires policies and repeats")
    result: list[tuple[int, str]] = []
    for repeat in range(repeats):
        rotation = repeat % len(policies)
        ordered = [*policies[rotation:], *policies[:rotation]]
        result.extend((repeat, policy) for policy in ordered)
    return result


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


def _faults(sysfs: Path) -> int:
    return int((sysfs / "faults").read_text().strip())


def _read_bytes(snapshot: dict[str, dict[str, int]]) -> int:
    return sum(values["read_sectors"] * 512 for values in snapshot.values())


def _assert_cold(sysfs: Path, device: Path, module: Path) -> None:
    if (Path("/sys/module/vmem_sw/srcversion").read_text().strip()
            != EXPECTED_DRIVER_SRCVERSION):
        raise RuntimeError("loaded VMEM module is not the frozen C3EF implementation")
    if sha256_file(module) != "5b7d7b2efc04fae182e41d5fcf29dd26ebef32341e41b6f369050f3ce5cf945f":
        raise RuntimeError("frozen VMEM module hash changed")
    for name in ("cache_used", "dirty_bytes", "io_errors"):
        value = int((sysfs / name).read_text().strip())
        if value != 0:
            raise RuntimeError(f"cold-state gate failed: {name}={value}")
    users = find_open_users(device)
    if users:
        raise RuntimeError(f"device has open users after reset: {users}")


def _reset(repo: Path, dataset: str, evidence: Path) -> None:
    script = (
        'source "$1/tools/eval_host_cold_lib.sh"; '
        'eval_reset_and_restore "$1" "$2" "$3" 4 extent'
    )
    subprocess.run(
        ["bash", "-c", script, "bash", str(repo), dataset, str(evidence)],
        check=True,
    )


def _write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(f"refusing existing output {path}")
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def execute(args: argparse.Namespace) -> None:
    trace = json.loads(args.trace_manifest.read_text(encoding="utf-8"))
    trace_hash = trace["trace_sha256"]
    binary_hash = sha256_file(args.binary)
    policies = {policy: json.loads(
        (args.policy_root.parent / f"{args.policy_root.name}{policy}" / "manifest.json")
        .read_text(encoding="utf-8")
    ) for policy in (*ADMISSION, *COVERAGE)}

    schedules = [build_schedule(ADMISSION, args.repeats), build_schedule(COVERAGE, args.repeats)]
    for schedule in schedules:
        for repeat, policy in schedule:
            phase = phase_for_policy(policy)
            run_id = f"{args.tag}-laion10m-{phase}-{policy}-r{repeat}"
            accepted_path = args.output_root / "accepted" / phase / run_id / "run.json"
            if accepted_path.exists():
                record = json.loads(accepted_path.read_text(encoding="utf-8"))
                validate_record(record)
                print(json.dumps({"run_id": run_id, "status": "already-accepted"}), flush=True)
                continue
            run_root = args.output_root / "raw" / phase / run_id
            if run_root.exists():
                raise FileExistsError(f"preserving incomplete or rejected run {run_root}")
            cold_evidence = args.output_root / "preflight" / "c2-cold" / f"{run_id}.json"
            cold_evidence.parent.mkdir(parents=True, exist_ok=True)
            _reset(args.repo, "laion10m", cold_evidence)
            _assert_cold(args.vmem_sysfs, args.device, args.module)

            policy_dir = args.policy_root.parent / f"{args.policy_root.name}{policy}"
            timing_path = run_root / "timing.json"
            timing_path.parent.mkdir(parents=True, exist_ok=False)
            before_block = read_block_snapshot(args.backings)
            before_faults = _faults(args.vmem_sysfs)
            before_users = find_open_users(args.device)
            subprocess.run([
                str(args.binary), "--device", str(args.device),
                "--image-offset", str(args.image_offset),
                "--image-bytes", str(args.image_bytes),
                "--query-offsets", str(policy_dir / "query_offsets.u64"),
                "--pages", str(policy_dir / "pages.u64"),
                "--output", str(timing_path),
            ], check=True)
            after_block = read_block_snapshot(args.backings)
            after_faults = _faults(args.vmem_sysfs)
            after_users = find_open_users(args.device)
            timing = json.loads(timing_path.read_text(encoding="utf-8"))
            delta = block_delta(before_block, after_block)
            policy_manifest = policies[policy]
            metrics = {
                **latency_metrics(timing["latency_ns"]),
                "recall_at_10": args.recall_at_10,
                "useful_page_pct": policy_manifest["useful_page_pct"],
                "physical_amplification": policy_manifest["physical_amplification"],
                "logical_pages": policy_manifest["logical_pages"],
                "logical_pages_per_query": policy_manifest["logical_pages"] / policy_manifest["nq"],
                "nvme_read_bytes": delta["total_read_bytes"],
            }
            record = {
                "schema_version": 1,
                "run_id": run_id,
                "phase": phase,
                "dataset": "laion10m",
                "condition": policy,
                "repeat": repeat,
                "state": "complete",
                "identities": {
                    "binary_sha256": binary_hash,
                    "image_sha256": args.image_sha256,
                    "query_ids_sha256": trace_hash["query_ids.u32"],
                },
                "geometry": {
                    "cache_bytes": 4 * 1024**3,
                    "page_bytes": 4096,
                    "stripe_bytes": 2 * 1024**2,
                },
                "operations": {
                    "requested": policy_manifest["nq"],
                    "completed": timing["completed_queries"],
                },
                "counters": {
                    "before": {
                        "vmem_faults": before_faults,
                        "nvme_read_bytes": _read_bytes(before_block),
                    },
                    "after": {
                        "vmem_faults": after_faults,
                        "nvme_read_bytes": _read_bytes(after_block),
                    },
                    "block_delta": delta,
                },
                "exclusive": {
                    "overlapping_processes": before_users + after_users,
                    "other_io_detected": False,
                    "backings": list(args.backings),
                },
                "metrics": metrics,
                "sidecars": {
                    "query_ids_sha256": trace_hash["query_ids.u32"],
                    "candidate_offsets_sha256": trace_hash["candidate_offsets.u64"],
                    "candidate_ids_sha256": trace_hash["candidate_ids.u32"],
                    "result_ids_sha256": trace_hash["result_ids.u32"],
                    "useful_pages_sha256": policy_manifest["sidecars"]["useful_pages.u64"],
                },
                "driver": {
                    "srcversion": EXPECTED_DRIVER_SRCVERSION,
                    "module_sha256": sha256_file(args.module),
                    "cold_evidence": str(cold_evidence),
                },
                "validation": {"status": "pending"},
            }
            raw_record = run_root / "run.json"
            _write_json(raw_record, record)
            accepted = seal_record(raw_record, args.output_root / "accepted" / phase)
            print(json.dumps({"run_id": run_id, "accepted": str(accepted), **metrics}), flush=True)

    all_records = []
    for policy in (*ADMISSION, *COVERAGE):
        phase = phase_for_policy(policy)
        block = [json.loads((
            args.output_root / "accepted" / phase /
            f"{args.tag}-laion10m-{phase}-{policy}-r{repeat}" / "run.json"
        ).read_text(encoding="utf-8")) for repeat in range(args.repeats)]
        validate_block(block)
        all_records.extend(block)
    validate_c2_same_trace(all_records)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--device", type=Path, default=Path("/dev/vmem0"))
    parser.add_argument("--vmem-sysfs", type=Path, default=Path("/sys/class/vmem/vmem0"))
    parser.add_argument("--backings", nargs=2, required=True)
    parser.add_argument("--trace-manifest", type=Path, required=True)
    parser.add_argument("--policy-root", type=Path, required=True)
    parser.add_argument("--image-offset", type=int, required=True)
    parser.add_argument("--image-bytes", type=int, required=True)
    parser.add_argument("--image-sha256", required=True)
    parser.add_argument("--recall-at-10", type=float, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    execute(parser.parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
