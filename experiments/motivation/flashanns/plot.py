"""Render the four protected Motivation PDFs from aggregate CSV only."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib as mpl
import matplotlib.pyplot as plt
import numpy as np


FIGURE_NAMES = (
    "mot-cliff.pdf",
    "mot-pathology-admission.pdf",
    "mot-pathology-prefetch.pdf",
    "mot-c3-io-concurrency.pdf",
)
COLORS = ("#d5dbe5", "#7891b8", "#284f82")


def _rows(path: Path) -> list[dict]:
    result = []
    with path.open(encoding="utf-8", newline="") as stream:
        for raw in csv.DictReader(stream):
            row = dict(raw)
            for key, value in list(row.items()):
                if key not in ("phase", "dataset", "condition") and value != "":
                    row[key] = float(value)
            result.append(row)
    return result


def _select(rows: list[dict], phase: str, dataset: str, condition: str) -> dict:
    matches = [
        row for row in rows
        if row["phase"] == phase and row["dataset"] == dataset
        and row["condition"] == condition
    ]
    if len(matches) != 1:
        raise ValueError(f"expected one aggregate row for {phase}/{dataset}/{condition}")
    return matches[0]


def _style(ax) -> None:
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.tick_params(direction="out", length=3)
    ax.grid(axis="y", color="#dddddd", linewidth=0.6, zorder=0)


def _save(fig, path: Path) -> None:
    fig.savefig(path, format="pdf", metadata={"Creator": "matplotlib vector"})
    plt.close(fig)


def _plot_cliff(rows: list[dict], output: Path) -> None:
    datasets = ("t2i10m", "yfcc10m", "laion10m")
    labels = ("T2I-10M", "YFCC-10M", "LAION-10M")
    conditions = ("host", "cxl_cache", "flash")
    condition_labels = ("Host DRAM", "CXL-DRAM hit", "CXL-SSD fill")
    x = np.arange(len(datasets))
    width = 0.24
    with mpl.rc_context({"font.size": 10, "axes.labelsize": 10,
                         "xtick.labelsize": 9, "ytick.labelsize": 9,
                         "legend.fontsize": 9, "pdf.fonttype": 42}):
        fig, ax = plt.subplots(figsize=(5.12, 2.88), layout="constrained")
        for index, (condition, label) in enumerate(zip(conditions, condition_labels)):
            values = [
                _select(rows, "c1_tier", dataset, condition)["latency_us_p50_median"]
                for dataset in datasets
            ]
            bars = ax.bar(
                x + (index - 1) * width, values, width, label=label,
                color=COLORS[index], edgecolor="black", linewidth=0.7, zorder=3,
            )
            for bar, value in zip(bars, values):
                ax.text(bar.get_x() + bar.get_width() / 2, value * 1.22,
                        f"{value:.2g}", ha="center", va="bottom", fontsize=8)
        ax.set_yscale("log")
        ax.set_ylabel(r"Median latency ($\mu$s/item)")
        ax.set_xticks(x, labels)
        ax.legend(frameon=False, ncol=3, loc="upper center",
                  bbox_to_anchor=(0.5, 1.16))
        _style(ax)
        _save(fig, output / "mot-cliff.pdf")


def _plot_c2(rows: list[dict], output: Path, phase: str,
             conditions: tuple[str, ...], labels: tuple[str, ...], filename: str) -> None:
    values = [_select(rows, phase, "laion10m", condition) for condition in conditions]
    nand = [row["nvme_read_mib_per_query_median"] for row in values]
    useful = [row["useful_page_pct_median"] for row in values]
    latency_ms = [row["latency_us_mean_median"] / 1000.0 for row in values]
    x = np.arange(len(values))
    rc = {
        "font.size": 12, "axes.labelsize": 12, "xtick.labelsize": 12,
        "ytick.labelsize": 12, "legend.fontsize": 12, "pdf.fonttype": 42,
    }
    with mpl.rc_context(rc):
        fig, ax = plt.subplots(figsize=(5.12, 2.88), layout="constrained")
        bars = ax.bar(x, nand, width=0.58, color="#7891b8", edgecolor="black",
                      linewidth=0.8, zorder=3, label="NAND MiB/query")
        ax2 = ax.twinx()
        line = ax2.plot(x, useful, color="#a34832", marker="o", linewidth=2,
                        markersize=6, label="Useful pages")[0]
        ax.set_xticks(x, labels)
        ax.set_ylabel("NAND MiB/query")
        ax2.set_ylabel("Useful pages (%)")
        ax2.set_ylim(0, 112)
        ax.set_ylim(0, max(nand) * 1.35)
        for bar, latency in zip(bars, latency_ms):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + max(nand) * 0.035,
                    f"{latency:.1f} ms", ha="center", va="bottom", fontsize=12)
        ax.legend([bars, line], ["NAND MiB/query", "Useful pages"],
                  frameon=False, ncol=2, loc="upper left")
        _style(ax)
        ax2.spines["top"].set_visible(False)
        ax2.tick_params(direction="out", length=3)
        _save(fig, output / filename)


def _plot_c3(rows: list[dict], output: Path) -> None:
    conditions = ("single", "static", "dynamic")
    labels = ("Single", "Static", "Dynamic")
    selected = [
        _select(rows, "c3_batching", "t2i10m", condition)
        for condition in conditions
    ]
    x = np.arange(len(conditions))
    colors = ("#d5dbe5", "#7891b8", "#284f82")
    hatches = ("", "///", "")

    def values(metric: str) -> tuple[list[float], np.ndarray]:
        center = [row[f"{metric}_median"] for row in selected]
        low = [row[f"{metric}_ci_low"] for row in selected]
        high = [row[f"{metric}_ci_high"] for row in selected]
        errors = np.array([
            [value - lower for value, lower in zip(center, low)],
            [upper - value for value, upper in zip(center, high)],
        ])
        return center, errors

    with mpl.rc_context({
        "font.size": 7.5,
        "axes.labelsize": 7.5,
        "axes.titlesize": 7.5,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "pdf.fonttype": 42,
    }):
        fig, axes = plt.subplots(1, 2, figsize=(3.35, 2.45), layout="constrained")
        panels = (
            (axes[0], "bandwidth_utilization_pct", "(a) Bandwidth utilization", "%"),
            (axes[1], "throughput_QPS", "(b) Throughput", "QPS"),
        )
        for ax, metric, title, ylabel in panels:
            center, errors = values(metric)
            bars = ax.bar(
                x,
                center,
                width=0.64,
                yerr=errors,
                capsize=2.5,
                color=colors,
                edgecolor="black",
                linewidth=0.7,
                error_kw={"elinewidth": 0.8, "capthick": 0.8},
                zorder=3,
            )
            for bar, hatch in zip(bars, hatches):
                bar.set_hatch(hatch)
            ax.set_xticks(x, labels, rotation=20, ha="right", rotation_mode="anchor")
            ax.set_ylabel(ylabel)
            ax.set_title(title, pad=4)
            ax.set_ylim(bottom=0, top=max(errors[1] + np.asarray(center)) * 1.18)
            _style(ax)
        _save(fig, output / "mot-c3-io-concurrency.pdf")


def plot_figure5(csv_path: Path | str, output: Path | str) -> None:
    rows = _rows(Path(csv_path))
    out = Path(output)
    out.mkdir(parents=True, exist_ok=True)
    _plot_c3(rows, out)


def plot_all(csv_path: Path | str, output: Path | str) -> None:
    rows = _rows(Path(csv_path))
    out = Path(output)
    out.mkdir(parents=True, exist_ok=True)
    _plot_cliff(rows, out)
    _plot_c2(rows, out, "c2_admission", ("selective_4k", "blind_16k"),
             ("Selective 4 KiB", "Blind 16 KiB"), "mot-pathology-admission.pdf")
    _plot_c2(rows, out, "c2_coverage", ("demand", "top1", "top2", "top8"),
             ("Demand", "Top-1", "Top-2", "Top-8"), "mot-pathology-prefetch.pdf")
    _plot_c3(rows, out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--figure5-only", action="store_true")
    args = parser.parse_args()
    if args.figure5_only:
        plot_figure5(args.csv, args.output)
    else:
        plot_all(args.csv, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
