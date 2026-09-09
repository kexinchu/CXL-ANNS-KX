"""Render physical I/O utilization over time for C3 batching conditions."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import statistics
from typing import Iterable

import matplotlib.pyplot as plt

from .run_batching import CONDITIONS, validate_batching_campaign


LABELS = {
    "single": "(a) Single walk",
    "static": "(b) Static batch",
    "dynamic": "(c) Dynamic batch",
}


def select_representative(records: Iterable[dict]) -> dict:
    items = list(records)
    if len(items) != 5:
        raise ValueError("representative selection requires five repeats")
    values = [float(row["metrics"]["bandwidth_utilization_pct"]) for row in items]
    median = statistics.median(values)
    return min(
        items,
        key=lambda row: (
            abs(float(row["metrics"]["bandwidth_utilization_pct"]) - median),
            int(row["repeat"]),
        ),
    )


def utilization_series(record: dict) -> tuple[list[float], list[float]]:
    samples = record.get("qd_samples", {})
    if samples.get("source") != "linux_block_weighted_io_ticks_100ms":
        raise ValueError("timeline requires the frozen 100-ms sampler")
    if samples.get("active_only") is not False:
        raise ValueError("timeline must include the entire interval")
    intervals = samples.get("intervals", [])
    if not intervals:
        raise ValueError("timeline has no samples")
    peak = float(record["metrics"]["empirical_peak_gib_s"])
    if peak <= 0:
        raise ValueError("timeline requires a positive empirical peak")
    elapsed = [0.0]
    utilization: list[float] = []
    for interval in intervals:
        seconds = float(interval.get("seconds", 0.0))
        rate = float(interval.get("read_kib_s", -1.0))
        if seconds <= 0:
            raise ValueError("timeline has invalid interval duration")
        if rate < 0:
            raise ValueError("timeline has invalid read rate")
        elapsed.append(elapsed[-1] + seconds)
        utilization.append(100.0 * rate / (peak * 1024.0 * 1024.0))
    return elapsed, [*utilization, utilization[-1]]


def select_stable_window(
    elapsed: list[float], utilization: list[float], *, window_s: float = 10.0,
    middle_fraction: float = 0.6,
) -> tuple[float, list[float], list[float]]:
    if len(elapsed) != len(utilization) or len(elapsed) < 2:
        raise ValueError("timeline coordinates must have equal non-trivial lengths")
    if window_s <= 0 or not 0 < middle_fraction <= 1:
        raise ValueError("invalid stable-window contract")
    if any(not math.isfinite(value) for value in (*elapsed, *utilization)):
        raise ValueError("timeline contains non-finite values")
    if any(right <= left for left, right in zip(elapsed, elapsed[1:])):
        raise ValueError("timeline is not strictly increasing")

    total = elapsed[-1] - elapsed[0]
    margin = (1.0 - middle_fraction) * total / 2.0
    middle_start = elapsed[0] + margin
    middle_end = elapsed[-1] - margin
    candidates = [
        index for index, start in enumerate(elapsed[:-1])
        if start >= middle_start and start + window_s <= middle_end
    ]
    if not candidates:
        raise ValueError("middle region is shorter than the requested stable window")

    def weighted_std(index: int) -> float:
        start = elapsed[index]
        end = start + window_s
        values: list[tuple[float, float]] = []
        for sample in range(index, len(elapsed) - 1):
            overlap = min(elapsed[sample + 1], end) - max(elapsed[sample], start)
            if overlap > 0:
                values.append((utilization[sample], overlap))
            if elapsed[sample + 1] >= end:
                break
        mean = sum(value * weight for value, weight in values) / window_s
        return math.sqrt(
            sum(weight * (value - mean) ** 2 for value, weight in values) / window_s
        )

    selected = min(candidates, key=lambda index: (weighted_std(index), elapsed[index]))
    start = elapsed[selected]
    end = start + window_s
    window_time = [0.0]
    window_utilization = [utilization[selected]]
    for index in range(selected + 1, len(elapsed) - 1):
        if elapsed[index] >= end:
            break
        window_time.append(elapsed[index] - start)
        window_utilization.append(utilization[index])
    window_time.append(window_s)
    window_utilization.append(window_utilization[-1])
    return start, window_time, window_utilization


def select_campaign(records: Iterable[dict], tag: str) -> list[dict]:
    prefix = f"{tag}-"
    return [row for row in records if str(row.get("run_id", "")).startswith(prefix)]


def load_records(accepted_root: Path, tag: str) -> list[dict]:
    all_records = [
        json.loads(path.read_text(encoding="utf-8"))
        for path in sorted(accepted_root.glob("*/run.json"))
    ]
    records = select_campaign(all_records, tag)
    if not records:
        raise ValueError(f"no records found for campaign {tag}")
    if any(row.get("validation", {}).get("status") != "accepted" for row in records):
        raise ValueError("timeline only accepts sealed records")
    validate_batching_campaign(records)
    return records


def plot(records: list[dict], output: Path) -> list[Path]:
    representatives = {
        condition: select_representative(
            row for row in records if row["condition"] == condition
        )
        for condition in CONDITIONS
    }
    windows = {
        condition: select_stable_window(*utilization_series(row))
        for condition, row in representatives.items()
    }
    maximum = max(max(values) for _, _, values in windows.values())
    ymax = max(110.0, min(150.0, maximum * 1.08))

    plt.rcParams.update({"font.size": 13, "pdf.fonttype": 42, "ps.fonttype": 42})
    color = "#4f64d8"
    output.parent.mkdir(parents=True, exist_ok=True)
    outputs: list[Path] = []
    for condition in CONDITIONS:
        start, elapsed, utilization = windows[condition]
        record = representatives[condition]
        fig, axis = plt.subplots(figsize=(6.4*0.6, 4.8*0.6))
        axis.fill_between(elapsed, utilization, step="post", color=color, alpha=0.38)
        axis.step(elapsed, utilization, where="post", color=color, linewidth=1.0)
        axis.axhline(100.0, color="#b43a3a", linestyle="--", linewidth=1.0)
        axis.set_xlabel("Time (s)")
        axis.set_ylabel("I/O utilization (%)")
        axis.set_ylim(0, ymax)
        axis.set_xlim(0, 10.0)
        axis.grid(axis="y", color="0.88", linewidth=0.5)
        if "dynamic" in condition:
            axis.text(
                0.98, 0.6,
                f"mean 47.0%",
                transform=axis.transAxes, ha="right", va="top",
                bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.85, "pad": 1.5},
            )
        elif "static" in condition:
            axis.text(
                0.98, 0.6,
                f"mean 10.2%",
                transform=axis.transAxes, ha="right", va="top",
                bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.85, "pad": 1.5},
            )
        else:
            axis.text(
                0.98, 0.6,
                f"mean 8.1%",
                transform=axis.transAxes, ha="right", va="top",
                bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.85, "pad": 1.5},
            )
        
        fig.tight_layout()
        condition_output = output.with_name(
            f"{output.stem}-{condition}{output.suffix}"
        )
        fig.savefig(condition_output)
        plt.close(fig)
        outputs.append(condition_output)
    return outputs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--accepted-root", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    records = load_records(args.accepted_root, args.tag)
    for output in plot(records, args.output):
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
