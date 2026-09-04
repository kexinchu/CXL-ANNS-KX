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


def aggregate_records(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    if not records:
        raise AggregationError("no records")
    for record in records:
        if record.get("validation", {}).get("status") != "accepted":
            raise AggregationError(f"run {record.get('run_id')} is not accepted")
    cold_ids = {r["run_id"] for r in records if r["phase"] == "q4" and r["state"] == "cold"}
    for record in records:
        if record["phase"] == "q4" and record["state"] == "warm" and record.get("cold_parent_run_id") not in cold_ids:
            raise AggregationError(f"run {record['run_id']} has missing cold-parent link")

    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        key = (record["dataset"], record["phase"], record["system"], record["state"], record["L"])
        groups[key].append(record)
    output: list[dict[str, Any]] = []
    for key, group in sorted(groups.items()):
        if len(group) != 5 or {r["repeat"] for r in group} != set(range(5)):
            raise AggregationError(f"{key}: require exactly five repeats")
        for identity in ("artifact_manifest_sha256",):
            if len({r[identity] for r in group}) != 1:
                raise AggregationError(f"{key}: {identity} mismatch")
        for identity in ("query_ids_sha256", "candidate_ids_sha256"):
            if len({r["sidecars"].get(identity) for r in group}) != 1:
                raise AggregationError(f"{key}: {identity} mismatch")
        row: dict[str, Any] = dict(zip(("dataset", "phase", "system", "state", "L"), key))
        row["run_ids"] = ";".join(r["run_id"] for r in sorted(group, key=lambda x: x["repeat"]))
        numeric = sorted(set.intersection(*({k for k, v in r["metrics"].items() if isinstance(v, (int, float)) and not isinstance(v, bool)} for r in group)))
        for metric in numeric:
            values = [float(r["metrics"][metric]) for r in group]
            low, high = bootstrap_ci(values)
            row[f"{metric}_median"] = statistics.median(values)
            row[f"{metric}_ci_low"] = low
            row[f"{metric}_ci_high"] = high
        output.append(row)
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--datasets", required=True)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--provenance", required=True, type=Path)
    args = parser.parse_args()
    records = [json.loads(path.read_text()) for path in sorted(args.input.rglob("run.json"))]
    wanted = set(args.datasets.split(","))
    records = [r for r in records if r["dataset"] in wanted]
    rows = aggregate_records(records)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fields = sorted({key for row in rows for key in row})
    with args.out.open("w", newline="") as dst:
        writer = csv.DictWriter(dst, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    args.provenance.write_text(json.dumps({"datasets": sorted(wanted), "marks": [{"key": [r["dataset"], r["phase"], r["system"], r["state"], r["L"]], "run_ids": r["run_ids"].split(";")} for r in rows]}, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
