"""Collect fail-closed T2I evidence for single, static, and dynamic batching."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import statistics
from typing import Iterable, Sequence

from experiments.eval.flashanns.run_matrix import expand_runs
from experiments.eval.flashanns.run_one import _parse_metrics  # type: ignore[attr-defined]

from .contract import load_contract
from .observe import block_delta, find_open_users, read_block_snapshot, sha256_file
from .records import seal_record, validate_record
from .run_c2_campaign import _assert_cold, _reset
from .run_c3_qd_campaign import _device_users, _faults, _read_bytes, _trace_identities
from .run_qd import mean_entire_interval_qd, run_workload


CONDITIONS = ("single", "static", "dynamic")


def batching_schedule(repeats: int = 5) -> list[tuple[int, str]]:
    if repeats <= 0:
        raise ValueError("batching schedule requires repeats")
    schedule: list[tuple[int, str]] = []
    for repeat in range(repeats):
        rotation = repeat % len(CONDITIONS)
        ordered = (*CONDITIONS[rotation:], *CONDITIONS[:rotation])
        schedule.extend((repeat, condition) for condition in ordered)
    return schedule


def physical_bandwidth_gib_s(read_bytes: int, wall_s: float) -> float:
    if read_bytes <= 0 or wall_s <= 0:
        raise ValueError("physical bandwidth requires positive bytes and time")
    return read_bytes / (1024**3) / wall_s


def empirical_peak_gib_s(records: Iterable[dict]) -> float:
    groups: dict[int, list[float]] = {}
    ungrouped: list[float] = []
    for record in records:
        value = float(record["metrics"]["bandwidth_gib_s"])
        if value <= 0:
            raise ValueError("invalid capability bandwidth")
        concurrency = record["metrics"].get("issued_concurrency")
        if concurrency is None:
            ungrouped.append(value)
        else:
            groups.setdefault(int(concurrency), []).append(value)
    if groups:
        if any(len(values) != 5 for values in groups.values()):
            raise ValueError("capability levels require five accepted repeats")
        return max(statistics.median(values) for values in groups.values())
    if not ungrouped:
        raise ValueError("no accepted capability records")
    return statistics.median(ungrouped)


def build_batching_spec(
    *, repo: Path, condition: str, repeat: int, tag: str, run_root: Path,
    binary: Path,
) -> dict:
    if condition not in CONDITIONS:
        raise ValueError(f"unknown batching condition {condition}")
    phase = load_contract().phases["c3_batching"]
    threads = int(phase["threads"][condition])
    level = int(phase["representative_L"])
    specs = expand_runs(
        repo, "t2i10m", "q3_t8",
        anchors={"primary": {"flashanns": {"L": level}}},
        out=run_root.parent, system_id="flashanns", level=level,
        repeat_id=repeat, threads_value=threads, run_tag=tag,
    )
    if len(specs) != 1:
        raise ValueError("batching point did not expand to one run")
    spec = specs[0]
    old_root = str(spec["run_dir"])
    spec["run_id"] = run_root.name
    spec["run_dir"] = str(run_root)
    spec["condition"] = condition
    spec["command"] = [
        str(binary) if index == 0 else value.replace(old_root, str(run_root))
        for index, value in enumerate(spec["command"])
    ]
    if condition == "static":
        spec["command"] += ["--static-cohort", str(phase["static_cohort"])]
    return spec


def build_batching_record(
    *, run_id: str, condition: str, repeat: int, identities: dict[str, str],
    requested_queries: int, completed_queries: int, before_faults: int,
    after_faults: int, before_read_bytes: int, after_read_bytes: int,
    block_delta: dict, overlapping_processes: list[dict], metrics: dict,
    command: Sequence[str], cold_evidence: str, driver_sha256: str,
    empirical_peak: float, qd_samples: dict,
) -> dict:
    contract = load_contract()
    wall_s = float(metrics["wall_s"])
    bandwidth = physical_bandwidth_gib_s(int(block_delta["total_read_bytes"]), wall_s)
    recall = metrics.get("recall@10", metrics.get("recall_at_10"))
    normalized = dict(metrics)
    normalized.pop("recall@10", None)
    normalized.update({
        "recall_at_10": float(recall),
        "physical_bandwidth_gib_s": bandwidth,
        "bandwidth_utilization_pct": 100.0 * bandwidth / empirical_peak,
        "empirical_peak_gib_s": empirical_peak,
    })
    return {
        "schema_version": contract.schema_version,
        "run_id": run_id, "phase": "c3_batching", "dataset": "t2i10m",
        "condition": condition, "repeat": repeat, "state": "complete",
        "identities": {
            "binary_sha256": identities["binary"],
            "image_sha256": identities["image"],
            "query_ids_sha256": identities["query"],
        },
        "geometry": {
            "cache_bytes": contract.cache_bytes,
            "page_bytes": contract.page_bytes,
            "stripe_bytes": contract.stripe_bytes,
        },
        "operations": {"requested": requested_queries, "completed": completed_queries},
        "counters": {
            "before": {"vmem_faults": before_faults, "nvme_read_bytes": before_read_bytes},
            "after": {"vmem_faults": after_faults, "nvme_read_bytes": after_read_bytes},
            "block_delta": block_delta,
        },
        "exclusive": {
            "overlapping_processes": overlapping_processes,
            "other_io_detected": block_delta.get("total_write_bytes") != 0,
            "backings": sorted(key for key in block_delta if key not in
                               ("total_read_bytes", "total_write_bytes")),
        },
        "metrics": normalized,
        "qd_samples": qd_samples,
        "sidecars": {
            "query_ids_sha256": identities["query"],
            "candidate_offsets_sha256": identities["candidate_offsets"],
            "candidate_ids_sha256": identities["candidate_ids"],
            "result_ids_sha256": identities["result_ids"],
        },
        "command": list(command),
        "driver": {"module_sha256": driver_sha256, "cold_evidence": cold_evidence},
        "validation": {"status": "pending"},
    }


def validate_batching_campaign(
    records: Iterable[dict], *, require_complete: bool = True,
) -> None:
    items = list(records)
    expected = {(condition, repeat) for condition in CONDITIONS for repeat in range(5)}
    observed = set()
    for record in items:
        validate_record(record)
        if record["phase"] != "c3_batching" or record["dataset"] != "t2i10m":
            raise ValueError("non-batching record in campaign")
        condition = record["condition"]
        observed.add((condition, int(record["repeat"])))
        command = record.get("command", [])
        expected_threads = {"single": "1", "static": "8", "dynamic": "8"}[condition]
        if "--threads" not in command or command[command.index("--threads") + 1] != expected_threads:
            raise ValueError("batching thread contract mismatch")
        has_gate = "--static-cohort" in command
        if has_gate != (condition == "static"):
            raise ValueError("static cohort marker mismatch")
        metrics = record["metrics"]
        if float(metrics.get("physical_bandwidth_gib_s", 0)) <= 0:
            raise ValueError("missing physical bandwidth")
        if not (0 < float(metrics.get("bandwidth_utilization_pct", 0)) <= 125):
            raise ValueError("invalid empirical bandwidth utilization")
        qd = record.get("qd_samples", {})
        if qd.get("source") != "linux_block_weighted_io_ticks_100ms":
            raise ValueError("batching timeline requires the frozen 100-ms sampler")
        if qd.get("active_only") is not False:
            raise ValueError("batching timeline must cover the entire interval")
        intervals = qd.get("intervals", [])
        if not intervals:
            raise ValueError("batching timeline has no intervals")
        mean_entire_interval_qd(qd)
        for interval in intervals:
            if float(interval.get("seconds", 0.0)) <= 0:
                raise ValueError("batching timeline has invalid interval duration")
            if float(interval.get("aqu_sz", -1.0)) < 0:
                raise ValueError("batching timeline has invalid queue depth")
            if float(interval.get("read_kib_s", -1.0)) < 0:
                raise ValueError("batching timeline has invalid read rate")
    if require_complete and (observed != expected or len(items) != len(expected)):
        raise ValueError("batching campaign is not complete")
    for field in ("binary_sha256", "image_sha256", "query_ids_sha256"):
        if len({row["identities"][field] for row in items}) > 1:
            raise ValueError(f"batching {field} drift")
    recalls = [float(row["metrics"]["recall_at_10"]) for row in items]
    if recalls and max(recalls) - min(recalls) > 1e-9:
        raise ValueError("batching recall drift")


def _accepted_capability(root: Path) -> list[dict]:
    records = [json.loads(path.read_text()) for path in sorted(root.glob("*/run.json"))]
    if not records or any(row.get("validation", {}).get("status") != "accepted" for row in records):
        raise ValueError("capability denominator must use accepted records")
    return records


def execute_smoke(args: argparse.Namespace) -> None:
    if sha256_file(args.binary) != args.expected_binary_sha256:
        raise RuntimeError("search binary identity changed")
    summaries = []
    for repeat, condition in batching_schedule(1):
        run_id = f"{args.tag}-t2i10m-c3_batching-smoke-{condition}"
        run_root = args.output_root / "smoke" / "c3_batching" / run_id
        if run_root.exists():
            raise FileExistsError(f"preserving existing smoke {run_root}")
        cold = args.output_root / "preflight" / "c3-batching-smoke-cold" / f"{run_id}.json"
        cold.parent.mkdir(parents=True, exist_ok=True)
        _reset(args.repo, "t2i10m", cold)
        _assert_cold(args.vmem_sysfs, args.device, args.module)
        spec = build_batching_spec(repo=args.repo, condition=condition, repeat=repeat,
                                   tag=args.tag, run_root=run_root, binary=args.binary)
        command = list(spec["command"])
        command[command.index("--max-q") + 1] = str(args.smoke_queries)
        measurement = run_workload(command, run_root, args.backings)
        metrics = _parse_metrics(run_root / "stdout.log", args.smoke_queries,
                                 int(measurement["returncode"]))
        stdout = (run_root / "stdout.log").read_text(encoding="utf-8")
        marker = {
            "single": "threads=1",
            "static": "steal_sched=1 depth=2 threads=8 shared_pool=1 issue_qd=8 admit_serial=1 static_cohort=8",
            "dynamic": "steal_sched=1 depth=2 threads=8 shared_pool=1 issue_qd=8 admit_serial=1 static_cohort=0",
        }[condition]
        read_bytes = int(measurement["block_delta"]["total_read_bytes"])
        if marker not in stdout:
            raise RuntimeError(f"{condition}: scheduler marker missing")
        if int(metrics["completed_queries"]) != args.smoke_queries or read_bytes <= 0:
            raise RuntimeError(f"{condition}: incomplete smoke or no physical reads")
        summary = {
            "condition": condition,
            "completed_queries": int(metrics["completed_queries"]),
            "recall_at_10": float(metrics["recall@10"]),
            "throughput_qps": float(metrics["throughput_QPS"]),
            "wall_s": float(metrics["wall_s"]),
            "physical_read_bytes": read_bytes,
            "scheduler_marker": marker,
        }
        (run_root / "smoke-summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        summaries.append(summary)
        print(json.dumps(summary), flush=True)
    recalls = [row["recall_at_10"] for row in summaries]
    if max(recalls) - min(recalls) > 1e-9:
        raise RuntimeError("smoke recall drift")


def execute(args: argparse.Namespace) -> None:
    binary_hash = sha256_file(args.binary)
    if binary_hash != args.expected_binary_sha256:
        raise RuntimeError("search binary identity changed")
    driver_hash = sha256_file(args.module)
    peak = empirical_peak_gib_s(_accepted_capability(args.capability_root))
    accepted_records: list[dict] = []
    for repeat, condition in batching_schedule(args.repeats):
        run_id = f"{args.tag}-t2i10m-c3_batching-{condition}-r{repeat}"
        accepted = args.output_root / "accepted" / "c3_batching" / run_id / "run.json"
        if accepted.exists():
            accepted_records.append(json.loads(accepted.read_text()))
            continue
        raw_root = args.output_root / "raw" / "c3_batching" / run_id
        if raw_root.exists():
            raise FileExistsError(f"preserving existing raw run {raw_root}")
        cold = args.output_root / "preflight" / "c3-batching-cold" / f"{run_id}.json"
        cold.parent.mkdir(parents=True, exist_ok=True)
        _reset(args.repo, "t2i10m", cold)
        _assert_cold(args.vmem_sysfs, args.device, args.module)
        before_users = _device_users(args.device, args.backings)
        before_block = read_block_snapshot(args.backings)
        before_faults = _faults(args.vmem_sysfs)
        spec = build_batching_spec(repo=args.repo, condition=condition, repeat=repeat,
                                   tag=args.tag, run_root=raw_root, binary=args.binary)
        measured = run_workload(spec["command"], raw_root, args.backings)
        after_block = read_block_snapshot(args.backings)
        after_faults = _faults(args.vmem_sysfs)
        after_users = _device_users(args.device, args.backings)
        metrics = _parse_metrics(raw_root / "stdout.log", int(spec["nq"]), 0)
        identities = _trace_identities(raw_root, args.binary, args.image_sha256)
        delta = block_delta(before_block, after_block)
        record = build_batching_record(
            run_id=run_id, condition=condition, repeat=repeat, identities=identities,
            requested_queries=int(spec["nq"]),
            completed_queries=int(metrics["completed_queries"]),
            before_faults=before_faults, after_faults=after_faults,
            before_read_bytes=_read_bytes(before_block), after_read_bytes=_read_bytes(after_block),
            block_delta=delta, overlapping_processes=before_users + after_users,
            metrics=metrics, command=spec["command"], cold_evidence=str(cold),
            driver_sha256=driver_hash, empirical_peak=peak,
            qd_samples=measured["qd_samples"],
        )
        raw_record = raw_root / "run.json"
        raw_record.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        sealed_path = seal_record(raw_record, args.output_root / "accepted" / "c3_batching")
        sealed = json.loads(sealed_path.read_text())
        accepted_records.append(sealed)
        print(json.dumps({"run_id": run_id, "status": "accepted", **sealed["metrics"]}),
              flush=True)
    validate_batching_campaign(accepted_records)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--expected-binary-sha256", required=True)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--device", type=Path, default=Path("/dev/vmem0"))
    parser.add_argument("--vmem-sysfs", type=Path, default=Path("/sys/class/vmem/vmem0"))
    parser.add_argument("--backings", nargs=2, required=True)
    parser.add_argument("--image-sha256", required=True)
    parser.add_argument("--capability-root", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--smoke-queries", type=int, default=0)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    if args.smoke_queries:
        execute_smoke(args)
    else:
        execute(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
