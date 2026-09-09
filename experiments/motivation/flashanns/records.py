"""Fail-closed records for paper-facing Motivation measurements."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import re
import tempfile
from typing import Any, Iterable

from .contract import load_contract


class RecordValidationError(ValueError):
    """A record is incomplete, inconsistent, or unsafe to accept."""


_HASH_RE = re.compile(r"^[0-9a-f]{64}$")
_REQUIRED = (
    "schema_version",
    "run_id",
    "phase",
    "dataset",
    "condition",
    "repeat",
    "state",
    "identities",
    "geometry",
    "operations",
    "counters",
    "exclusive",
    "metrics",
    "validation",
)
_IDENTITIES = ("binary_sha256", "image_sha256", "query_ids_sha256")
_TRACE_FIELDS = (
    "query_ids_sha256",
    "candidate_offsets_sha256",
    "candidate_ids_sha256",
    "result_ids_sha256",
    "useful_pages_sha256",
)


def _counter_deltas(record: dict[str, Any]) -> dict[str, int]:
    counters = record.get("counters", {})
    before = counters.get("before", {})
    after = counters.get("after", {})
    required = ("vmem_faults", "nvme_read_bytes")
    missing = [name for name in required if name not in before or name not in after]
    if missing:
        raise RecordValidationError("missing counters: " + ", ".join(missing))
    deltas = {name: int(after[name]) - int(before[name]) for name in required}
    if any(value < 0 for value in deltas.values()):
        raise RecordValidationError("negative counter delta")
    return deltas


def validate_record(record: dict[str, Any]) -> None:
    errors = [f"missing {field}" for field in _REQUIRED if field not in record]
    if errors:
        raise RecordValidationError("; ".join(errors))

    contract = load_contract()
    if record["schema_version"] != contract.schema_version:
        errors.append("wrong schema_version")
    if record["phase"] not in contract.phases:
        errors.append("undeclared phase")
    if record["repeat"] not in contract.repeats:
        errors.append("undeclared repeat")
    if record["state"] != "complete":
        errors.append("record state is not complete")

    identities = record.get("identities", {})
    for field in _IDENTITIES:
        value = identities.get(field)
        if not isinstance(value, str) or not _HASH_RE.fullmatch(value):
            errors.append(f"missing or invalid {field}")

    expected_geometry = {
        "cache_bytes": contract.cache_bytes,
        "page_bytes": contract.page_bytes,
        "stripe_bytes": contract.stripe_bytes,
    }
    geometry = record.get("geometry", {})
    for field, expected in expected_geometry.items():
        if geometry.get(field) != expected:
            errors.append(f"wrong {field}")

    operations = record.get("operations", {})
    requested = operations.get("requested")
    completed = operations.get("completed")
    if not isinstance(requested, int) or requested <= 0 or completed != requested:
        errors.append("operations are incomplete")

    exclusive = record.get("exclusive", {})
    if exclusive.get("overlapping_processes"):
        errors.append("overlapping device process evidence")
    if exclusive.get("other_io_detected") is not False:
        errors.append("other I/O detected or not ruled out")

    try:
        deltas = _counter_deltas(record)
    except RecordValidationError as error:
        errors.append(str(error))
        deltas = {}

    if record["phase"] == "c1_tier" and deltas:
        tier = record["condition"]
        observed = {
            "host": deltas["vmem_faults"] == 0 and deltas["nvme_read_bytes"] == 0,
            "cxl_cache": deltas["vmem_faults"] > 0 and deltas["nvme_read_bytes"] == 0,
            "flash": deltas["vmem_faults"] > 0 and deltas["nvme_read_bytes"] > 0,
        }
        if tier not in observed or not observed[tier]:
            errors.append("tier classification disagrees with counters")

    status = record.get("validation", {}).get("status")
    if status not in ("pending", "accepted", "rejected"):
        errors.append("invalid validation status")
    if errors:
        raise RecordValidationError("; ".join(errors))


def validate_block(records: Iterable[dict[str, Any]]) -> None:
    items = list(records)
    if len(items) != 5:
        raise RecordValidationError("comparison block requires exactly five records")
    for record in items:
        validate_record(record)
    keys = {(item["phase"], item["dataset"], item["condition"]) for item in items}
    if len(keys) != 1:
        raise RecordValidationError("five-record block spans multiple marks")
    repeats = tuple(sorted(item["repeat"] for item in items))
    if repeats != load_contract().repeats:
        raise RecordValidationError("repeat IDs must be exactly 0..4")


def validate_c2_same_trace(records: Iterable[dict[str, Any]]) -> None:
    items = list(records)
    if len(items) < 2:
        raise RecordValidationError("C2 comparison needs at least two records")
    for record in items:
        validate_record(record)
        if record["phase"] not in ("c2_admission", "c2_coverage"):
            raise RecordValidationError("non-C2 record in same-trace comparison")
        sidecars = record.get("sidecars", {})
        for field in _TRACE_FIELDS:
            if not _HASH_RE.fullmatch(str(sidecars.get(field, ""))):
                raise RecordValidationError(f"missing or invalid {field}")
        if not isinstance(record["metrics"].get("recall_at_10"), (int, float)):
            raise RecordValidationError("missing recall_at_10")
    first = items[0]
    for record in items[1:]:
        if record["dataset"] != first["dataset"]:
            raise RecordValidationError("dataset mismatch")
        for field in _TRACE_FIELDS:
            if record["sidecars"][field] != first["sidecars"][field]:
                raise RecordValidationError(f"same-trace mismatch: {field}")
        if record["metrics"]["recall_at_10"] != first["metrics"]["recall_at_10"]:
            raise RecordValidationError("same-trace mismatch: recall_at_10")


def seal_record(raw: Path | str, accepted_root: Path | str) -> Path:
    """Validate a raw JSON record and atomically write an accepted copy."""
    raw_path = Path(raw)
    record = json.loads(raw_path.read_text(encoding="utf-8"))
    validate_record(record)
    sealed = copy.deepcopy(record)
    sealed["validation"] = {
        **sealed["validation"],
        "source_status": sealed["validation"]["status"],
        "status": "accepted",
    }
    destination = Path(accepted_root) / record["run_id"] / "run.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise RecordValidationError(f"refusing existing sealed record {record['run_id']}")
    temporary: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=destination.parent,
            prefix=f".{destination.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary = stream.name
            json.dump(sealed, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
        temporary = None
    finally:
        if temporary is not None:
            Path(temporary).unlink(missing_ok=True)
    return destination
