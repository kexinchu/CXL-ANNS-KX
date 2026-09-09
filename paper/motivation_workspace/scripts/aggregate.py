"""Aggregate accepted Motivation evidence into a reproducible CSV bundle."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import random
import statistics
from typing import Iterable

from .records import validate_block, validate_record


METRICS = (
    "latency_us_mean", "latency_us_p50", "latency_us_p95", "latency_us_p99",
    "recall_at_10", "useful_page_pct", "physical_amplification",
    "logical_pages_per_query", "nvme_read_bytes", "bandwidth_gib_s",
    "mean_aqu_sz", "working_set_bytes", "concurrent_queries",
    "elapsed_seconds", "throughput_QPS", "mean_latency_ms",
)


def _percentile(values: list[float], percentile: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * percentile / 100.0
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _bootstrap(values: list[float], seed_text: str, samples: int = 2000) -> tuple[float, float]:
    rng = random.Random(int(hashlib.sha256(seed_text.encode()).hexdigest()[:16], 16))
    medians = [
        statistics.median(rng.choices(values, k=len(values))) for _ in range(samples)
    ]
    return _percentile(medians, 2.5), _percentile(medians, 97.5)


def aggregate_records(accepted_root: Path | str) -> tuple[list[dict], dict, dict]:
    root = Path(accepted_root)
    paths = sorted(root.glob("*/*/run.json"))
    records = []
    path_by_id = {}
    for path in paths:
        record = json.loads(path.read_text(encoding="utf-8"))
        validate_record(record)
        if record.get("validation", {}).get("status") != "accepted":
            raise ValueError(f"non-accepted record under accepted root: {path}")
        records.append(record)
        path_by_id[record["run_id"]] = str(path)

    groups: dict[tuple[str, str, str], list[dict]] = {}
    for record in records:
        key = (record["phase"], record["dataset"], record["condition"])
        groups.setdefault(key, []).append(record)

    rows: list[dict] = []
    marks: list[dict] = []
    for key in sorted(groups):
        group = sorted(groups[key], key=lambda item: int(item["repeat"]))
        validate_block(group)
        phase, dataset, condition = key
        row: dict[str, object] = {
            "phase": phase,
            "dataset": dataset,
            "condition": condition,
            "n": len(group),
        }
        for metric in METRICS:
            values = [record.get("metrics", {}).get(metric) for record in group]
            if not all(isinstance(value, (int, float)) for value in values):
                continue
            numbers = [float(value) for value in values]
            low, high = _bootstrap(numbers, ":".join((*key, metric)))
            row[f"{metric}_median"] = statistics.median(numbers)
            row[f"{metric}_ci_low"] = low
            row[f"{metric}_ci_high"] = high
        per_query = [
            float(record["counters"]["block_delta"]["total_read_bytes"])
            / int(record["operations"]["completed"])
            / (1024.0**2)
            for record in group
        ]
        low, high = _bootstrap(per_query, ":".join((*key, "nvme_read_mib_per_query")))
        row["nvme_read_mib_per_query_median"] = statistics.median(per_query)
        row["nvme_read_mib_per_query_ci_low"] = low
        row["nvme_read_mib_per_query_ci_high"] = high
        rows.append(row)
        run_ids = [record["run_id"] for record in group]
        marks.append({
            "phase": phase,
            "dataset": dataset,
            "condition": condition,
            "run_ids": run_ids,
            "source_records": [path_by_id[run_id] for run_id in run_ids],
        })

    provenance = {
        "schema_version": 1,
        "accepted_root": str(root),
        "marks": marks,
    }
    quality = {
        "schema_version": 1,
        "accepted_runs": len(records),
        "marks": len(rows),
        "rejected_marks": [],
        "all_marks_have_five_repeats": all(row["n"] == 5 for row in rows),
    }
    return rows, provenance, quality


def write_bundle(
    rows: Iterable[dict], provenance: dict, quality: dict, output: Path | str
) -> None:
    items = list(rows)
    out = Path(output)
    out.mkdir(parents=True, exist_ok=True)
    fields = ["phase", "dataset", "condition", "n"]
    fields.extend(sorted({key for row in items for key in row} - set(fields)))
    with (out / "motivation.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(items)
    for name, value in (("provenance.json", provenance), ("quality-report.json", quality)):
        (out / name).write_text(
            json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--accepted", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    write_bundle(*aggregate_records(args.accepted), args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
