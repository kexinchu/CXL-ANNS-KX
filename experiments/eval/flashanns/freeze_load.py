"""Freeze the measured open-loop load contract."""

from __future__ import annotations

import argparse
import copy
import json
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any


def freeze_load_contract(
    records: list[dict[str, Any]],
    recall_anchors: dict[str, Any],
    target_recall: float,
) -> dict[str, Any]:
    if not 0.0 < target_recall <= 1.0:
        raise ValueError("target_recall must be in (0, 1]")
    accepted = [
        record for record in records
        if record.get("validation", {}).get("status") == "accepted"
    ]
    datasets = {record.get("dataset") for record in accepted}
    if len(datasets) != 1:
        raise ValueError("load calibration must contain one dataset")

    pipe_groups: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for record in accepted:
        if record.get("phase") == "q2" and record.get("system") == "pipeann":
            pipe_groups[int(record["L"])].append(record)
    eligible: list[tuple[float, int, list[dict[str, Any]]]] = []
    for level, group in pipe_groups.items():
        if len(group) != 5 or {item["repeat"] for item in group} != set(range(5)):
            raise ValueError(f"pipeann L={level} requires exactly five repeats")
        recall = statistics.median(float(item["metrics"]["recall@10"]) for item in group)
        if recall >= target_recall:
            eligible.append((recall, level, group))
    if not eligible:
        raise ValueError("pipeann does not cover the load-curve recall target")
    pipe_recall, pipe_level, pipe_group = min(
        eligible, key=lambda item: (item[0] - target_recall, item[1])
    )

    saturation: dict[str, float] = {}
    provenance: dict[str, list[str]] = {}
    for system in ("wise-only", "flashanns"):
        group = [
            record for record in accepted
            if record.get("phase") == "q3_t8" and record.get("system") == system
            and record.get("threads") == 8
        ]
        if len(group) != 5 or {item["repeat"] for item in group} != set(range(5)):
            raise ValueError(f"{system} T=8 requires exactly five repeats")
        saturation[system] = statistics.median(
            float(item["metrics"]["throughput_QPS"]) for item in group
        )
        provenance[system] = [
            item["run_id"] for item in sorted(group, key=lambda item: item["repeat"])
        ]
    saturation["pipeann"] = statistics.median(
        float(item["metrics"]["throughput_QPS"]) for item in pipe_group
    )
    provenance["pipeann"] = [
        item["run_id"] for item in sorted(pipe_group, key=lambda item: item["repeat"])
    ]

    reference = min(saturation.values())
    fractions = (0.25, 0.50, 0.75, 1.00, 1.25)
    rates = [round(reference * fraction, 3) for fraction in fractions]
    if len(set(rates)) != len(rates) or rates[0] <= 0:
        raise ValueError("measured saturation cannot produce unique positive rates")

    result = copy.deepcopy(recall_anchors)
    result["accepted"] = True
    result["dataset"] = next(iter(datasets))
    result.setdefault("primary", {})["pipeann"] = {
        "L": pipe_level,
        "recall@10": pipe_recall,
        "run_ids": provenance["pipeann"],
    }
    result["saturation_qps"] = saturation
    result["saturation_reference_qps"] = reference
    result["arrival_rate_fractions"] = list(fractions)
    result["arrival_rates"] = rates
    result["load_provenance"] = provenance
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--recall-anchors", required=True, type=Path)
    parser.add_argument("--target-recall", required=True, type=float)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    from experiments.eval.flashanns.validate_run import load_records

    frozen = freeze_load_contract(
        load_records(args.paths),
        json.loads(args.recall_anchors.read_text()),
        args.target_recall,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(frozen, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
