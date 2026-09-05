"""Fail-closed structural validation for evaluation run records."""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
from typing import Any


class RunValidationError(ValueError):
    pass


REQUIRED = (
    "run_id", "dataset", "metric", "phase", "system", "state", "L", "k", "nq", "repeat",
    "command", "git", "binary_sha256", "artifact_manifest_sha256", "preflight_before",
    "preflight_after", "device_before", "device_after", "metrics", "sidecars", "validation",
)
SAME_SEARCH_FIELDS = ("query_ids_sha256", "candidate_offsets_sha256", "candidate_ids_sha256", "result_ids_sha256")


def validate_record(record: dict[str, Any]) -> None:
    errors = [f"missing {key}" for key in REQUIRED if key not in record]
    if errors:
        raise RunValidationError("; ".join(errors))
    if record["validation"].get("status") not in ("pending", "accepted", "rejected"):
        errors.append("invalid validation.status")
    if record["system"] == "oracle":
        errors.append("Oracle system is excluded from the executable evaluation contract")
    external = bool(record.get("external"))
    expected_cache = 4294967296
    if record.get("phase") == "q4_cache":
        expected_cache = int(record.get("cache_gib", 0)) * 1024**3
        if record.get("cache_gib") not in (1, 2, 4, 8):
            errors.append("q4_cache cache_gib is not declared")
    if not external:
        for where in ("preflight_before", "preflight_after"):
            if record[where].get("cache_limit") != expected_cache:
                errors.append(f"{where}.cache_limit is not {expected_cache}")
    metrics = record["metrics"]
    if metrics.get("completed_queries") != record["nq"]:
        errors.append("completed_queries differs from nq")
    if not isinstance(metrics.get("recall@10"), (int, float)):
        errors.append("missing numeric recall@10")
    if record["system"] == "flashanns" and metrics.get("score_bounce", 0) != 0:
        errors.append("score_bounce is nonzero")
    if record["system"] == "flashanns" and metrics.get("score_flash", 0) != 0:
        errors.append("score_flash is nonzero")
    required_sidecars = ("query_ids_sha256", "result_ids_sha256") if external else SAME_SEARCH_FIELDS
    for field in required_sidecars:
        if not record["sidecars"].get(field):
            errors.append(f"missing sidecar {field}")
    if record.get("phase") == "q3_load":
        rate = record.get("arrival_rate")
        if not isinstance(rate, (int, float)) or rate <= 0:
            errors.append("q3_load requires positive arrival_rate")
        for field in ("latency_p99_ms", "queue_wait_ms_mean", "offered_QPS"):
            if not isinstance(metrics.get(field), (int, float)):
                errors.append(f"missing numeric {field}")
        if isinstance(rate, (int, float)) and isinstance(metrics.get("offered_QPS"), (int, float)):
            if abs(float(metrics["offered_QPS"]) - float(rate)) > 1e-6 * max(1.0, float(rate)):
                errors.append("offered_QPS differs from arrival_rate")
        if not record["sidecars"].get("latency_ns_sha256"):
            errors.append("missing sidecar latency_ns_sha256")
    if errors:
        raise RunValidationError("; ".join(errors))


def validate_same_search(records: list[dict[str, Any]]) -> None:
    if len(records) < 2:
        raise RunValidationError("same-search block needs at least two runs")
    for record in records:
        validate_record(record)
    first = records[0]
    for record in records[1:]:
        for field in ("dataset", "metric", "artifact_manifest_sha256", "L", "k", "nq"):
            if record[field] != first[field]:
                raise RunValidationError(f"same-search mismatch: {field}")
        for field in SAME_SEARCH_FIELDS:
            if record["sidecars"][field] != first["sidecars"][field]:
                raise RunValidationError(f"same-search mismatch: {field}")
        if record["metrics"]["recall@10"] != first["metrics"]["recall@10"]:
            raise RunValidationError("same-search mismatch: recall@10")


def load_records(paths: list[Path]) -> list[dict[str, Any]]:
    from experiments.eval.flashanns.run_one import _parse_metrics

    records: list[dict[str, Any]] = []
    for path in paths:
        path = Path(path)
        candidates = sorted(path.rglob("run.json")) if path.is_dir() else [path]
        if not candidates:
            raise RunValidationError(f"no run.json records under {path}")
        for candidate in candidates:
            record = json.loads(candidate.read_text())
            log = candidate.with_name("stdout.log")
            command = record.get("command") or []
            if log.is_file() and command and Path(command[0]).name == "search_beam":
                record["metrics"] = {
                    **record.get("metrics", {}),
                    **_parse_metrics(
                        log, int(record["nq"]), int(record["validation"].get("returncode", 0))
                    ),
                }
            records.append(record)
    return records


def seal_records(records: list[dict[str, Any]], out_dir: Path) -> list[Path]:
    """Write validated copies; raw run directories remain immutable evidence."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    for record in records:
        validate_record(record)
        sealed = copy.deepcopy(record)
        source_status = sealed["validation"].get("status")
        sealed["validation"] = {
            **sealed["validation"],
            "source_status": source_status,
            "status": "accepted",
        }
        path = out_dir / record["run_id"] / "run.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists():
            raise RunValidationError(f"refusing existing sealed run {record['run_id']}")
        path.write_text(json.dumps(sealed, indent=2, sort_keys=True) + "\n")
        paths.append(path)
    return paths


def _select_anchor(records: list[dict[str, Any]], target: float) -> dict[str, Any]:
    selected: dict[str, Any] = {}
    systems = sorted({record["system"] for record in records})
    for system in systems:
        eligible = [
            record
            for record in records
            if record["system"] == system and record["metrics"]["recall@10"] >= target
        ]
        if not eligible:
            raise RunValidationError(f"{system} does not cover recall target {target}")
        chosen = min(
            eligible,
            key=lambda record: (record["metrics"]["recall@10"] - target, record["L"]),
        )
        selected[system] = {
            "L": chosen["L"],
            "recall@10": chosen["metrics"]["recall@10"],
            "run_id": chosen["run_id"],
        }
    return selected


def freeze_anchors(
    records: list[dict[str, Any]],
    primary_target: float,
    extra_target: float | None = None,
) -> dict[str, Any]:
    if not records:
        raise RunValidationError("cannot freeze anchors from no records")
    seen: set[tuple[str, int]] = set()
    datasets = set()
    for record in records:
        validate_record(record)
        if record["phase"] != "calibration":
            raise RunValidationError("anchor input contains a non-calibration record")
        key = (record["system"], record["L"])
        if key in seen:
            raise RunValidationError(f"duplicate calibration point: {key[0]} L={key[1]}")
        seen.add(key)
        datasets.add(record["dataset"])
    if len(datasets) != 1:
        raise RunValidationError("anchor input spans multiple datasets")
    result: dict[str, Any] = {
        "accepted": True,
        "dataset": next(iter(datasets)),
        "primary_target": primary_target,
        "primary": _select_anchor(records, primary_target),
        "runs": [record["run_id"] for record in records],
    }
    if extra_target is not None:
        result["extra_target"] = extra_target
        result["extra"] = _select_anchor(records, extra_target)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--compare-same-search", action="store_true")
    parser.add_argument("--freeze-anchor", type=float)
    parser.add_argument("--extra-anchor", type=float)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--seal-dir", type=Path)
    args = parser.parse_args()
    records = load_records(args.paths)
    output: dict[str, Any] | None = None
    if args.freeze_anchor is not None:
        output = freeze_anchors(records, args.freeze_anchor, args.extra_anchor)
    elif args.compare_same_search:
        validate_same_search(records)
    else:
        for record in records:
            validate_record(record)
    if args.seal_dir:
        seal_records(records, args.seal_dir)
    if args.out:
        output = output or {"accepted": True, "runs": [r["run_id"] for r in records]}
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
