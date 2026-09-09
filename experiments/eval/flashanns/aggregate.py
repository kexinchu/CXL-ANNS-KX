"""Deterministic five-repeat aggregation for accepted FlashANNS evidence."""

from __future__ import annotations

import argparse
import csv
import json
import random
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any


class AggregationError(ValueError):
    pass


def _canonical_metrics(record: dict[str, Any]) -> dict[str, float]:
    """Expose stable figure names while retaining every numeric runtime metric."""
    metrics = {
        key: float(value)
        for key, value in record["metrics"].items()
        if isinstance(value, (int, float)) and not isinstance(value, bool)
    }
    aliases = {
        "recall_at_10": "recall@10",
        "qps": "throughput_QPS",
        "mean_latency_ms": "mean",
        "latency_p50_ms": "p50",
        "latency_p95_ms": "p95",
        "latency_p99_ms": "p99",
        "vector_slot_use_pct": "slot_use_pct",
        "extent_use_pct": "issue_use_pct",
        "mean_inflight": "inflight_depth_mean",
    }
    for canonical, raw in aliases.items():
        if canonical not in metrics and raw in metrics:
            metrics[canonical] = metrics[raw]
    nq = float(record.get("nq") or metrics.get("nq") or 1)
    if "crit_wait_ns" in metrics:
        metrics["critical_wait_ms"] = metrics["crit_wait_ns"] / nq / 1_000_000.0
    stall_aliases = {
        "coverage_wait_ms": "coverage_wait_ns",
        "slot_backpressure_wait_ms": "slot_backpressure_wait_ns",
        "host_data_stall_ms": "host_data_stall_ns",
    }
    for canonical, raw in stall_aliases.items():
        if raw in metrics:
            metrics[canonical] = metrics[raw] / nq / 1_000_000.0
    if "nand_read_bytes" in metrics:
        metrics["nand_mib_per_query"] = metrics["nand_read_bytes"] / nq / 1_048_576.0
    elif "nvme_read_B" in metrics:
        # Compatibility fallback for legacy records that predate device-delta
        # counters. New physical-NAND claims must use nand_read_bytes.
        metrics["nand_mib_per_query"] = metrics["nvme_read_B"] / nq / 1_048_576.0
    # The committed full-precision set is the frozen C_L. ``dist`` counts all
    # PQ navigation distance evaluations and is not a candidate-set size.
    metrics["committed_candidates"] = float(record["L"])
    if "requested_pages" in metrics:
        metrics["missing_pages"] = metrics["requested_pages"] / nq
    if "issued_pages" in metrics:
        metrics["issued_pages_per_query"] = metrics["issued_pages"] / nq
    if "issue_commands" in metrics:
        metrics["read_batches_per_query"] = metrics["issue_commands"] / nq
    if "nand_read_commands" in metrics:
        metrics["nand_commands_per_query"] = metrics["nand_read_commands"] / nq
    if "nvme_real_GBps" in metrics:
        metrics["useful_bandwidth_mib_s"] = metrics["nvme_real_GBps"] * 1024.0
    return metrics


def _dimension(record: dict[str, Any], name: str) -> Any:
    return record.get(name, record.get("metrics", {}).get(name))


def _record_layout(record: dict[str, Any]) -> str:
    layout = record.get("layout")
    if layout in ("extent", "original", "external"):
        return str(layout)
    if record.get("external"):
        return "external"
    if "--id-slot-map" in record.get("command", []):
        return "extent"
    raise AggregationError(
        f"run {record.get('run_id')} lacks physical layout identity"
    )


def bootstrap_ci(values: list[float], seed: int = 20260904, samples: int = 10000) -> tuple[float, float]:
    if not values:
        raise AggregationError("cannot bootstrap empty values")
    rng = random.Random(seed)
    medians = []
    for _ in range(samples):
        draw = [values[rng.randrange(len(values))] for _ in values]
        medians.append(statistics.median(draw))
    medians.sort()
    return medians[250], medians[9750]


def load_records(inputs: list[Path]) -> list[dict[str, Any]]:
    """Load only the explicitly selected evidence roots, without silent duplication."""
    paths: list[Path] = []
    for source in inputs:
        source = Path(source)
        if source.is_file():
            paths.append(source)
        else:
            paths.extend(source.rglob("run.json"))
    records: list[dict[str, Any]] = []
    seen: set[str] = set()
    for path in sorted(paths):
        record = json.loads(path.read_text())
        run_id = record.get("run_id")
        if not run_id:
            raise AggregationError(f"{path}: missing run_id")
        if run_id in seen:
            raise AggregationError(f"duplicate run_id {run_id}")
        seen.add(run_id)
        records.append(record)
    return records


def aggregate_records(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    if not records:
        raise AggregationError("no records")
    for record in records:
        if record.get("system") == "oracle":
            raise AggregationError("Oracle system is excluded from aggregation")
        if record.get("validation", {}).get("status") != "accepted":
            raise AggregationError(f"run {record.get('run_id')} is not accepted")
    cold_ids = {r["run_id"] for r in records if r["phase"] in ("q4", "q4_cold_warm") and r["state"] == "cold"}
    for record in records:
        if record["phase"] in ("q4", "q4_cold_warm") and record["state"] == "warm" and record.get("cold_parent_run_id") not in cold_ids:
            raise AggregationError(f"run {record['run_id']} has missing cold-parent link")

    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        key = (
            record["dataset"], record["phase"], record["system"], _record_layout(record), record["state"], record["L"],
            _dimension(record, "threads"), _dimension(record, "arrival_rate"),
            _dimension(record, "cache_gib"),
        )
        groups[key].append(record)
    output: list[dict[str, Any]] = []
    for key, group in sorted(groups.items()):
        if len(group) != 5 or {r["repeat"] for r in group} != set(range(5)):
            raise AggregationError(f"{key}: require exactly five repeats")
        for identity in ("artifact_manifest_sha256", "binary_sha256"):
            if len({r[identity] for r in group}) != 1:
                raise AggregationError(f"{key}: {identity} mismatch")
        trace_identities = (
            ("query_ids_sha256",)
            if group[0].get("external")
            else ("query_ids_sha256", "candidate_ids_sha256")
        )
        for identity in trace_identities:
            if len({r["sidecars"].get(identity) for r in group}) != 1:
                raise AggregationError(f"{key}: {identity} mismatch")
        row: dict[str, Any] = dict(zip(
            ("dataset", "phase", "system", "layout", "state", "L", "threads", "arrival_rate", "cache_gib"),
            key,
        ))
        row["run_ids"] = ";".join(r["run_id"] for r in sorted(group, key=lambda x: x["repeat"]))
        normalized = [_canonical_metrics(record) for record in group]
        numeric = sorted(set.intersection(*(set(metrics) for metrics in normalized)))
        for metric in numeric:
            values = [metrics[metric] for metrics in normalized]
            low, high = bootstrap_ci(values)
            row[f"{metric}_median"] = statistics.median(values)
            row[f"{metric}_ci_low"] = low
            row[f"{metric}_ci_high"] = high
        output.append(row)
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--datasets", required=True)
    parser.add_argument(
        "--input", required=True, type=Path, action="append",
        help="accepted evidence directory or run.json; repeat for curated non-overlapping roots",
    )
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--provenance", required=True, type=Path)
    args = parser.parse_args()
    records = load_records(args.input)
    wanted = set(args.datasets.split(","))
    records = [r for r in records if r["dataset"] in wanted]
    rows = aggregate_records(records)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fields = sorted({key for row in rows for key in row})
    with args.out.open("w", newline="") as dst:
        writer = csv.DictWriter(dst, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    args.provenance.write_text(json.dumps({"datasets": sorted(wanted), "marks": [{"key": [r["dataset"], r["phase"], r["system"], r["layout"], r["state"], r["L"]], "run_ids": r["run_ids"].split(";")} for r in rows]}, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
