"""Fail-closed structural validation for evaluation run records."""

from __future__ import annotations

import argparse
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
    for where in ("preflight_before", "preflight_after"):
        if record[where].get("cache_limit") != 4294967296:
            errors.append(f"{where}.cache_limit is not 4294967296")
    metrics = record["metrics"]
    if metrics.get("completed_queries") != record["nq"]:
        errors.append("completed_queries differs from nq")
    if record["system"] == "oracle" and metrics.get("nvme_read_B", 0) != 0:
        errors.append("Oracle NAND bytes are nonzero")
    if record["system"] == "flashanns" and metrics.get("score_bounce", 0) != 0:
        errors.append("score_bounce is nonzero")
    if record["system"] == "flashanns" and metrics.get("score_flash", 0) != 0:
        errors.append("score_flash is nonzero")
    for field in SAME_SEARCH_FIELDS:
        if not record["sidecars"].get(field):
            errors.append(f"missing sidecar {field}")
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--compare-same-search", action="store_true")
    parser.add_argument("--out", type=Path)
    args, _ = parser.parse_known_args()
    records = [json.loads(path.read_text()) for path in args.paths]
    if args.compare_same_search:
        validate_same_search(records)
    else:
        for record in records:
            validate_record(record)
    if args.out:
        args.out.write_text(json.dumps({"accepted": True, "runs": [r["run_id"] for r in records]}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
