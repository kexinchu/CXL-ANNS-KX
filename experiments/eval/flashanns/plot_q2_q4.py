"""Render the fixed Q2--Q4 one-page vector figure contracts."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Any


COLORS = {
    "demand": "#7f7f7f", "pipeann": "#e69f00", "flashanns": "#0072b2",
    "serial-t1": "#7f7f7f", "batch-t1": "#56b4e9", "extent-t1": "#0072b2",
    "wise-only": "#d55e00", "nosteal-t8": "#d55e00",
}


def _read(path: Path) -> list[dict[str, Any]]:
    with Path(path).open(newline="") as src:
        return list(csv.DictReader(src))


def _line(
    ax: Any,
    rows: list[dict[str, Any]],
    metric: str,
    title: str,
    *,
    xkey: str = "L",
    xlabel: str = "Search list L",
) -> None:
    for system in sorted({r["system"] for r in rows}):
        group = sorted(
            (r for r in rows if r["system"] == system and r.get(metric, "") != ""),
            key=lambda r: float(r[xkey]),
        )
        if not group:
            continue
        x = [float(r[xkey]) for r in group]
        y = [float(r[metric]) for r in group]
        stem = metric.removesuffix("_median")
        low_key, high_key = f"{stem}_ci_low", f"{stem}_ci_high"
        yerr = None
        if all(r.get(low_key, "") != "" and r.get(high_key, "") != "" for r in group):
            yerr = (
                [value - float(r[low_key]) for value, r in zip(y, group)],
                [float(r[high_key]) - value for value, r in zip(y, group)],
            )
        ax.errorbar(x, y, yerr=yerr, marker="o", capsize=2, label=system, color=COLORS.get(system))
    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.grid(True, alpha=0.25)


def _categorical(ax: Any, rows: list[dict[str, Any]], metric: str, title: str) -> None:
    order = ("serial-t1", "batch-t1", "extent-t1")
    selected = [next((r for r in rows if r["system"] == system), None) for system in order]
    pairs = [(system, row) for system, row in zip(order, selected) if row is not None]
    values = [float(row[metric]) for _, row in pairs]
    ax.bar(range(len(pairs)), values, color=[COLORS[system] for system, _ in pairs])
    ax.set_xticks(range(len(pairs)), [system for system, _ in pairs], rotation=18, ha="right")
    ax.set_title(title)
    ax.set_ylabel("Throughput (QPS)")
    ax.grid(True, axis="y", alpha=0.25)


def render_figures(csv_path: Path, out_dir: Path) -> None:
    import matplotlib.pyplot as plt

    rows = _read(csv_path)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    q2 = [r for r in rows if r["phase"] == "q2"]
    fig, axes = plt.subplots(2, 2, figsize=(8.2, 5.8), constrained_layout=True)
    for ax, metric, title in zip(axes.flat, ("qps_median", "latency_p99_ms_median", "critical_wait_ms_median", "vector_slot_use_pct_median"), ("Throughput", "P99 latency", "Critical wait", "Useful vector slots")):
        _line(ax, q2, metric, title)
    handles, labels = axes.flat[0].get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc="upper center", ncol=len(handles))
        fig.get_layout_engine().set(rect=(0, 0, 1, 0.92))
    fig.savefig(out_dir / "q2-main.pdf")
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(8.2, 3.2), constrained_layout=True)
    _categorical(axes[0], [r for r in rows if r["phase"] == "q3_t1"], "qps_median", "T=1 transfer ablation")
    _line(
        axes[1], [r for r in rows if r["phase"] == "q3_t8"],
        "qps_median", "Scheduler scaling", xkey="threads", xlabel="Concurrent queries",
    )
    axes[1].legend(fontsize=7)
    fig.savefig(out_dir / "q3-ablation.pdf")
    plt.close(fig)

    q4 = [r for r in rows if r["phase"] == "q4_cold_warm"]
    fig, ax = plt.subplots(figsize=(6.8, 3.4), constrained_layout=True)
    labels = sorted({(r["dataset"], r["state"]) for r in q4})
    selected = [next(r for r in q4 if (r["dataset"], r["state"]) == label) for label in labels]
    values = [float(r["qps_median"]) for r in selected]
    errors = (
        [value - float(r.get("qps_ci_low", value)) for value, r in zip(values, selected)],
        [float(r.get("qps_ci_high", value)) - value for value, r in zip(values, selected)],
    )
    colors = ["#0072b2" if state == "cold" else "#56b4e9" for _, state in labels]
    ax.bar(range(len(labels)), values, yerr=errors, capsize=3, color=colors)
    ax.set_xticks(range(len(labels)), [f"{d}\n{s}" for d, s in labels])
    ax.set_ylabel("QPS")
    ax.set_title("Paired cold/warm throughput")
    ax.grid(True, axis="y", alpha=0.25)
    fig.savefig(out_dir / "q4-cold-warm.pdf")
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args()
    render_figures(args.csv, args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
