"""Run and seal the C3 Wise-Prefetcher queue-depth campaign."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
from typing import Iterable, Sequence

from experiments.eval.flashanns.run_matrix import expand_runs
from experiments.eval.flashanns.run_one import _parse_metrics  # type: ignore[attr-defined]

from .contract import load_contract
from .observe import block_delta, find_open_users, read_block_snapshot, sha256_file
from .records import seal_record, validate_record
from .run_c2_campaign import EXPECTED_DRIVER_SRCVERSION, _assert_cold, _reset
from .run_qd import mean_entire_interval_qd, run_workload


def c3_qd_schedule(
    datasets: Sequence[str], threads: Sequence[int], repeats: int = 5
) -> list[tuple[str, int, int]]:
    if not datasets or not threads or repeats <= 0:
        raise ValueError("C3 QD schedule requires datasets, threads, and repeats")
    schedule: list[tuple[str, int, int]] = []
    for dataset in datasets:
        for repeat in range(repeats):
            rotation = repeat % len(threads)
            ordered = [*threads[rotation:], *threads[:rotation]]
            schedule.extend((dataset, repeat, int(level)) for level in ordered)
    return schedule


def build_wise_spec(
    *, repo: Path, dataset: str, threads: int, repeat: int, tag: str,
    run_root: Path,
) -> dict:
    phase = load_contract().phases["c3_qd"]
    level = int(phase["representative_L"][dataset])
    anchors = {"primary": {"flashanns": {"L": level}}}
    specs = expand_runs(
        repo,
        dataset,
        "q3_t8",
        anchors=anchors,
        out=run_root.parent,
        system_id="wise-only",
        level=level,
        repeat_id=repeat,
        threads_value=threads,
        run_tag=tag,
    )
    if len(specs) != 1:
        raise ValueError("Wise C3 point did not expand to exactly one run")
    spec = specs[0]
    original_root = str(spec["run_dir"])
    spec["run_id"] = run_root.name
    spec["run_dir"] = str(run_root)
    spec["command"] = [
        value.replace(original_root, str(run_root)) for value in spec["command"]
    ]
    return spec


def build_c3_qd_record(
    *, run_id: str, dataset: str, threads: int, repeat: int, level: int,
    identities: dict[str, str], requested_queries: int, completed_queries: int,
    elapsed_seconds: float, before_faults: int, after_faults: int,
    before_read_bytes: int, after_read_bytes: int, block_delta: dict,
    overlapping_processes: list[dict], qd: dict, metrics: dict,
    command: Sequence[str], cold_evidence: str, driver_sha256: str,
) -> dict:
    contract = load_contract()
    normalized_metrics = dict(metrics)
    recall = normalized_metrics.pop("recall@10", normalized_metrics.get("recall_at_10"))
    normalized_metrics.update({
        "recall_at_10": recall,
        "concurrent_queries": threads,
        "mean_aqu_sz": mean_entire_interval_qd(qd),
        "elapsed_seconds": elapsed_seconds,
        "representative_L": level,
    })
    return {
        "schema_version": contract.schema_version,
        "run_id": run_id,
        "phase": "c3_qd",
        "dataset": dataset,
        "condition": f"t{threads}",
        "repeat": repeat,
        "state": "complete",
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
                key for key in block_delta
                if key not in ("total_read_bytes", "total_write_bytes")
            ),
        },
        "metrics": normalized_metrics,
        "qd_samples": qd,
        "sidecars": {
            "query_ids_sha256": identities["query"],
            "candidate_offsets_sha256": identities["candidate_offsets"],
            "candidate_ids_sha256": identities["candidate_ids"],
            "result_ids_sha256": identities["result_ids"],
        },
        "command": list(command),
        "driver": {
            "module_sha256": driver_sha256,
            "cold_evidence": cold_evidence,
        },
        "validation": {"status": "pending"},
    }


def validate_c3_qd_campaign(
    records: Iterable[dict], *, require_complete: bool = True
) -> None:
    items = list(records)
    contract = load_contract().phases["c3_qd"]
    expected = {
        (dataset, threads, repeat)
        for dataset in contract["datasets"]
        for threads in contract["threads"]
        for repeat in range(5)
    }
    observed: set[tuple[str, int, int]] = set()
    for record in items:
        validate_record(record)
        if record["phase"] != "c3_qd":
            raise ValueError("non-C3-QD record in campaign")
        threads = int(record["metrics"].get("concurrent_queries", 0))
        if record["condition"] != f"t{threads}":
            raise ValueError("C3 thread condition mismatch")
        observed.add((record["dataset"], threads, int(record["repeat"])))
        elapsed = float(record["metrics"].get("elapsed_seconds", 0.0))
        completed = int(record["operations"]["completed"])
        if completed < int(contract["min_queries"]) and elapsed < float(contract["min_seconds"]):
            raise ValueError("C3 point must cover 2,000 queries or 30 seconds")
        qd = record.get("qd_samples", {})
        mean = mean_entire_interval_qd(qd)
        if not qd.get("intervals") or not abs(mean - float(record["metrics"]["mean_aqu_sz"])) < 1e-9:
            raise ValueError("C3 QD mean excludes or misstates intervals")
        if qd.get("source") != "linux_block_weighted_io_ticks_100ms":
            raise ValueError("C3 QD source is not the frozen 100-ms sampler")
    if require_complete and (observed != expected or len(items) != len(expected)):
        raise ValueError("C3 QD campaign is not complete")

    binary_hashes = {record["identities"]["binary_sha256"] for record in items}
    if len(binary_hashes) > 1:
        raise ValueError("C3 binary identity drift")
    identity_fields = (
        "image_sha256", "query_ids_sha256", "candidate_offsets_sha256",
        "candidate_ids_sha256", "result_ids_sha256",
    )
    for dataset in {record["dataset"] for record in items}:
        group = [record for record in items if record["dataset"] == dataset]
        for field in identity_fields:
            values = {
                record["identities"][field]
                if field in record["identities"] else record["sidecars"][field]
                for record in group
            }
            if len(values) != 1:
                raise ValueError(f"C3 {field} identity drift")
        recalls = {float(record["metrics"]["recall_at_10"]) for record in group}
        if len(recalls) != 1:
            raise ValueError("C3 recall drift")


def _read_bytes(snapshot: dict[str, dict[str, int]]) -> int:
    return sum(values["read_sectors"] * 512 for values in snapshot.values())


def _faults(sysfs: Path) -> int:
    return int((sysfs / "faults").read_text(encoding="utf-8").strip())


def _device_users(device: Path, backings: Sequence[str]) -> list[dict]:
    users = find_open_users(device)
    for backing in backings:
        users.extend(find_open_users(Path("/dev") / backing))
    return users


def _write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(f"refusing existing output {path}")
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _next_attempt(root: Path, dataset: str) -> tuple[Path, Path, Path]:
    attempt = 0
    while True:
        directory = root / "preflight" / "c3-stage" / f"{dataset}-attempt{attempt}"
        if not directory.exists():
            return directory, directory / "stage.log", directory / "full-identity.json"
        attempt += 1


def _stage_and_verify(repo: Path, dataset: str, output_root: Path) -> dict:
    directory, stage_log, identity_path = _next_attempt(output_root, dataset)
    directory.mkdir(parents=True, exist_ok=False)
    with stage_log.open("w", encoding="utf-8") as stream:
        completed = subprocess.run(
            [
                "python3", "-m", "experiments.eval.flashanns.stage",
                "--dataset", dataset, "--layout", "extent",
                "--device", "/dev/vmem0",
            ],
            cwd=repo,
            stdout=stream,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    if completed.returncode:
        raise subprocess.CalledProcessError(completed.returncode, completed.args)
    subprocess.run(
        [
            "python3", "-m", "experiments.eval.flashanns.preflight",
            "--dataset", dataset, "--layout", "extent", "--state", "post",
            "--full-identity", "--out", str(identity_path),
        ],
        cwd=repo,
        check=True,
    )
    identity = json.loads(identity_path.read_text(encoding="utf-8"))
    if (
        identity.get("accepted") is not True
        or identity.get("full_host_sha256") != identity.get("full_device_sha256")
    ):
        raise RuntimeError(f"{dataset}: full staged-image identity failed")
    identity["evidence_path"] = str(identity_path)
    return identity


def _trace_identities(run_root: Path, binary: Path, image_sha256: str) -> dict[str, str]:
    trace = run_root / "trace"
    paths = {
        "query": trace / "query_ids.u32",
        "candidate_offsets": trace / "candidate_offsets.u64",
        "candidate_ids": trace / "candidate_ids.u32",
        "result_ids": trace / "result_ids.u32",
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise RuntimeError("missing C3 trace sidecars: " + ", ".join(missing))
    return {
        "binary": sha256_file(binary),
        "image": image_sha256,
        **{name: sha256_file(path) for name, path in paths.items()},
    }


def execute(args: argparse.Namespace) -> None:
    contract = load_contract()
    phase = contract.phases["c3_qd"]
    binary_hash = sha256_file(args.binary)
    if binary_hash != args.expected_binary_sha256:
        raise RuntimeError(
            f"frozen search binary changed: {binary_hash} != {args.expected_binary_sha256}"
        )
    driver_hash = sha256_file(args.module)
    accepted_records: list[dict] = []

    schedule = c3_qd_schedule(phase["datasets"], phase["threads"], args.repeats)
    for dataset in phase["datasets"]:
        dataset_schedule = [point for point in schedule if point[0] == dataset]
        pending = []
        for _, repeat, threads in dataset_schedule:
            run_id = f"{args.tag}-{dataset}-c3_qd-t{threads}-r{repeat}"
            accepted = args.output_root / "accepted" / "c3_qd" / run_id / "run.json"
            if accepted.exists():
                record = json.loads(accepted.read_text(encoding="utf-8"))
                validate_record(record)
                accepted_records.append(record)
            else:
                pending.append((repeat, threads, run_id))
        if not pending:
            print(json.dumps({"dataset": dataset, "status": "already-accepted"}), flush=True)
            continue

        if _device_users(args.device, args.backings):
            raise RuntimeError("VMEM or a backing device has an open user before staging")
        identity = _stage_and_verify(args.repo, dataset, args.output_root)
        image_sha256 = str(identity["full_host_sha256"])

        for repeat, threads, run_id in pending:
            raw_root = args.output_root / "raw" / "c3_qd" / run_id
            if raw_root.exists():
                raise FileExistsError(f"preserving incomplete or rejected run {raw_root}")
            cold_evidence = (
                args.output_root / "preflight" / "c3-cold" / f"{run_id}.json"
            )
            cold_evidence.parent.mkdir(parents=True, exist_ok=True)
            _reset(args.repo, dataset, cold_evidence)
            _assert_cold(args.vmem_sysfs, args.device, args.module)

            before_users = _device_users(args.device, args.backings)
            before_block = read_block_snapshot(args.backings)
            before_faults = _faults(args.vmem_sysfs)
            spec = build_wise_spec(
                repo=args.repo,
                dataset=dataset,
                threads=threads,
                repeat=repeat,
                tag=args.tag,
                run_root=raw_root,
            )
            measurement = run_workload(spec["command"], raw_root, args.backings)
            after_block = read_block_snapshot(args.backings)
            after_faults = _faults(args.vmem_sysfs)
            after_users = _device_users(args.device, args.backings)

            metrics = _parse_metrics(
                raw_root / "stdout.log", int(spec["nq"]), int(measurement["returncode"])
            )
            identities = _trace_identities(raw_root, args.binary, image_sha256)
            delta = block_delta(before_block, after_block)
            record = build_c3_qd_record(
                run_id=run_id,
                dataset=dataset,
                threads=threads,
                repeat=repeat,
                level=int(spec["L"]),
                identities=identities,
                requested_queries=int(spec["nq"]),
                completed_queries=int(metrics.get("completed_queries", 0)),
                elapsed_seconds=float(measurement["elapsed_seconds"]),
                before_faults=before_faults,
                after_faults=after_faults,
                before_read_bytes=_read_bytes(before_block),
                after_read_bytes=_read_bytes(after_block),
                block_delta=delta,
                overlapping_processes=before_users + after_users,
                qd=measurement["qd_samples"],
                metrics=metrics,
                command=spec["command"],
                cold_evidence=str(cold_evidence),
                driver_sha256=driver_hash,
            )
            raw_record = raw_root / "run.json"
            _write_json(raw_record, record)
            accepted_path = seal_record(
                raw_record, args.output_root / "accepted" / "c3_qd"
            )
            sealed = json.loads(accepted_path.read_text(encoding="utf-8"))
            accepted_records.append(sealed)
            print(json.dumps({
                "run_id": run_id,
                "status": "accepted",
                "recall_at_10": sealed["metrics"]["recall_at_10"],
                "mean_aqu_sz": sealed["metrics"]["mean_aqu_sz"],
                "elapsed_seconds": sealed["metrics"]["elapsed_seconds"],
            }), flush=True)

        dataset_records = [row for row in accepted_records if row["dataset"] == dataset]
        validate_c3_qd_campaign(dataset_records, require_complete=False)

    validate_c3_qd_campaign(accepted_records)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--expected-binary-sha256", required=True)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--device", type=Path, default=Path("/dev/vmem0"))
    parser.add_argument(
        "--vmem-sysfs", type=Path, default=Path("/sys/class/vmem/vmem0")
    )
    parser.add_argument("--backings", nargs=2, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    execute(parser.parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
