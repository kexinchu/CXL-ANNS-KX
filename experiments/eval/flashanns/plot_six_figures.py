"""Render the six fixed Evaluation figures from validated aggregate CSV."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Any, Iterable


class FigureDataError(ValueError):
    """Raised when aggregate evidence is incomplete or unsafe to plot."""


DATASET_LABELS = {
    "t2i10m": "T2I-10M",
    "yfcc10m": "YFCC-10M",
    "laion10m": "LAION-10M",
}

PLOT_STYLE = {
    "font.size": 12,
    "axes.titlesize": 12,
    "axes.labelsize": 12,
    "xtick.labelsize": 10,
    "ytick.labelsize": 10,
    "legend.fontsize": 9,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
    "savefig.bbox": "tight",
    "savefig.facecolor": "white",
}


COLORS = {
    "demand-orc": "#303030",
    "demand": "#7f7f7f", "pipeann": "#e69f00", "flashanns": "#0072b2",
    "serial-t1": "#7f7f7f", "batch-t1": "#56b4e9", "extent-t1": "#0072b2",
    "wise-only": "#d55e00",
}
LABELS = {
    "demand-orc": "Demand (original layout)",
    "demand": "Demand (optimized layout)",
    "pipeann": "PipeANN", "flashanns": "FlashANNS",
    "serial-t1": "Serialized", "batch-t1": "Post-commit batch",
    "extent-t1": "Co-use + extent", "wise-only": "Wise only",
}


def _read(path: Path) -> list[dict[str, str]]:
    with Path(path).open(newline="") as src:
        return list(csv.DictReader(src))


def _replace_phase_rows(
    base: list[dict[str, str]], replacement: list[dict[str, str]], phase: str
) -> list[dict[str, str]]:
    if not replacement or any(row.get("phase") != phase for row in replacement):
        raise FigureDataError(f"replacement CSV must contain only {phase} rows")
    return [row for row in base if row.get("phase") != phase] + replacement


def _number(row: dict[str, str], key: str, default: float = 0.0) -> float:
    raw = row.get(key, "")
    return default if raw in ("", None) else float(raw)


def _required_number(row: dict[str, str], key: str) -> float:
    raw = row.get(key, "")
    if raw in ("", None):
        identity = "/".join(
            str(row.get(name, "?")) for name in ("dataset", "phase", "system")
        )
        raise FigureDataError(f"missing {key} for {identity}")
    return float(raw)


def _physical_nand_mib_per_query(row: dict[str, str]) -> float:
    return (
        _required_number(row, "nand_read_bytes_median")
        / _required_number(row, "nq_median")
        / 1_048_576.0
    )


def validate_figure_rows(
    rows: list[dict[str, str]], *, require_complete: bool = True
) -> None:
    if any(row.get("phase") == "q3_load" for row in rows):
        raise FigureDataError("q3_load evidence is incomplete and must not be plotted")
    for row in rows:
        run_ids = [item for item in row.get("run_ids", "").split(";") if item]
        if len(run_ids) != 5:
            raise FigureDataError("every plotted mark must name five accepted runs")

    t8 = {
        (row.get("dataset"), row.get("system"))
        for row in rows
        if row.get("phase") == "q3_t8" and _number(row, "threads") == 8
    }
    for dataset in DATASET_LABELS:
        for system in ("wise-only", "flashanns"):
            if (dataset, system) not in t8:
                raise FigureDataError(
                    f"{DATASET_LABELS[dataset]} lacks {LABELS[system]} T=8 evidence"
                )

    if not require_complete:
        return
    required = {
        "q2": ("recall_at_10_median", "mean_latency_ms_median", "qps_median"),
        "q2_original": ("recall_at_10_median", "mean_latency_ms_median", "qps_median"),
        "q3_t1": ("qps_median", "committed_candidates_median",
                  "missing_pages_median", "issued_pages_per_query_median",
                  "nand_read_bytes_median", "nq_median",
                  "nand_commands_per_query_median"),
        "q3_t8": ("threads", "qps_median"),
        "q4_hide": ("coverage_wait_ms_median", "slot_backpressure_wait_ms_median",
                    "host_data_stall_ms_median", "qps_median",
                    "score_triggered_flash_fills_median",
                    "score_prematerialized_pct_median"),
        "q4_cold_warm": ("qps_median", "latency_p99_ms_median"),
        "q4_cache": ("cache_gib", "qps_median", "latency_p99_ms_median"),
    }
    for row in rows:
        for key in required.get(row.get("phase", ""), ()):
            _required_number(row, key)


def _t2i_phase(rows: list[dict[str, str]], phase: str) -> list[dict[str, str]]:
    """Select the explicitly declared representative-dataset ablation rows."""
    return [
        row for row in rows
        if row.get("dataset") == "t2i10m" and row.get("phase") == phase
    ]


def _error(row: dict[str, str], key: str) -> tuple[float, float]:
    value = _number(row, key)
    stem = key.removesuffix("_median")
    low = _number(row, f"{stem}_ci_low", value)
    high = _number(row, f"{stem}_ci_high", value)
    return max(0.0, value - low), max(0.0, high - value)


def _style(ax: Any, xlabel: str, ylabel: str, title: str) -> None:
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title, loc="left", fontweight="bold")
    ax.grid(True, axis="y", color="#d9d9d9", linewidth=0.7)
    ax.set_axisbelow(True)


def _frontier(rows: list[dict[str, str]], dataset: str, path: Path) -> None:
    import matplotlib.pyplot as plt

    selected = [
        row for row in rows
        if row.get("phase") in ("q2", "q2_original")
        and row.get("dataset") == dataset
    ]
    fig, axes = plt.subplots(1, 2, figsize=(6.4, 2.85))
    styles = {
        "demand-orc": ("--", "o", "none"),
        "demand": ("-", "s", COLORS["demand"]),
        "pipeann": ("--", "^", COLORS["pipeann"]),
        "flashanns": ("-", "D", COLORS["flashanns"]),
    }
    for system in ("demand-orc", "demand", "pipeann", "flashanns"):
        group = sorted((r for r in selected if r.get("system") == system),
                       key=lambda r: _number(r, "recall_at_10_median"))
        if not group:
            continue
        recall = [_required_number(r, "recall_at_10_median") for r in group]
        for ax, metric in zip(axes, ("mean_latency_ms_median", "qps_median")):
            values = [_required_number(r, metric) for r in group]
            errors = [_error(r, metric) for r in group]
            ax.errorbar(recall, values,
                        yerr=([e[0] for e in errors], [e[1] for e in errors]),
                        linestyle=styles[system][0], marker=styles[system][1],
                        markerfacecolor=styles[system][2], markeredgecolor=COLORS[system],
                        capsize=2, color=COLORS[system], label=LABELS[system])
    _style(axes[0], "Recall@10", "Mean latency (ms)", "(a) Latency frontier")
    _style(axes[1], "Recall@10", "Throughput (QPS)", "(b) Throughput frontier")
    for ax in axes:
        ax.set_yscale("log")
        ax.axvline(0.90, color="#999999", linestyle="--", linewidth=0.8)
    if dataset == "t2i10m":
        for ax in axes:
            ax.axvline(0.92, color="#555555", linestyle=":", linewidth=0.8)
    handles, labels = axes[0].get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 1.0),
                   ncol=2, frameon=False)
    fig.subplots_adjust(left=0.10, right=0.98, bottom=0.18, top=0.68, wspace=0.34)
    fig.savefig(path)
    plt.close(fig)


def _wise(rows: list[dict[str, str]], path: Path) -> None:
    import matplotlib.pyplot as plt
    import numpy as np

    selected = _t2i_phase(rows, "q3_t1")
    by_system = {r["system"]: r for r in selected}
    systems = [s for s in ("serial-t1", "batch-t1", "extent-t1") if s in by_system]
    fig, axes = plt.subplots(1, 3, figsize=(7.4, 2.85), constrained_layout=True)

    values = [_number(by_system[s], "qps_median") for s in systems]
    axes[0].bar(range(len(systems)), values, color=[COLORS[s] for s in systems])
    axes[0].set_xticks(range(len(systems)), [LABELS[s] for s in systems], rotation=18, ha="right")
    _style(axes[0], "", "Throughput (QPS)", "(a) When")

    source = by_system.get("batch-t1") or by_system.get("extent-t1")
    funnel_keys = (
        "committed_candidates_median",
        "missing_pages_median",
        "issued_pages_per_query_median",
    )
    funnel = [_number(source, key) if source else 0 for key in funnel_keys]
    axes[1].bar(range(3), funnel, color=("#999999", "#56b4e9", "#0072b2"))
    axes[1].set_xticks(
        range(3), ("Committed", "Missing pages", "Issued pages"), rotation=18, ha="right"
    )
    _style(axes[1], "", "Items/query", "(b) What")
    if source:
        axes[1].text(0.98, 0.98,
                     f"{_physical_nand_mib_per_query(source):.2g} MiB/q\n"
                     f"{_number(source, 'nand_commands_per_query_median'):.2g} cmds/q",
                     transform=axes[1].transAxes, va="top", ha="right", fontsize=7)

    compare = [s for s in ("batch-t1", "extent-t1") if s in by_system]
    x = np.arange(len(compare))
    width = 0.26
    metrics = (
        (lambda row: _required_number(row, "nand_commands_per_query_median"), "NAND commands"),
        (_physical_nand_mib_per_query, "NAND MiB"),
        (lambda row: _required_number(row, "qps_median"), "QPS"),
    )
    for index, (value_of, label) in enumerate(metrics):
        baseline = value_of(by_system["batch-t1"])
        axes[2].bar(
            x + (index - 1) * width,
            [100 * value_of(by_system[s]) / baseline for s in compare],
            width, label=label,
        )
    axes[2].set_xticks(x, [LABELS[s] for s in compare], rotation=18, ha="right")
    axes[2].set_ylim(bottom=0)
    axes[2].legend(fontsize=6, frameon=False)
    _style(axes[2], "", "Metric / post-commit batch (%)", "(c) How")
    fig.savefig(path)
    plt.close(fig)


def _group_lines(ax: Any, rows: Iterable[dict[str, str]], xkey: str, ykey: str) -> None:
    materialized = list(rows)
    for system in sorted({r["system"] for r in materialized}):
        group = sorted((r for r in materialized if r["system"] == system), key=lambda r: _number(r, xkey))
        ax.plot([_number(r, xkey) for r in group], [_number(r, ykey) for r in group],
                marker="o", color=COLORS.get(system), label=LABELS.get(system, system))


def _batching(rows: list[dict[str, str]], path: Path) -> None:
    import matplotlib.pyplot as plt

    scaling = [row for row in rows if row.get("phase") == "q3_t8"]
    fig, axes = plt.subplots(1, 3, figsize=(7.4, 2.9), sharex=True)
    styles = {"wise-only": ("--", "o"), "flashanns": ("-", "s")}
    for ax, dataset in zip(axes, ("t2i10m", "yfcc10m", "laion10m")):
        selected = [row for row in scaling if row.get("dataset") == dataset]
        for system in ("wise-only", "flashanns"):
            group = sorted(
                (row for row in selected if row.get("system") == system),
                key=lambda row: _required_number(row, "threads"),
            )
            ax.plot(
                [_required_number(row, "threads") for row in group],
                [_required_number(row, "qps_median") for row in group],
                color=COLORS[system], linestyle=styles[system][0],
                marker=styles[system][1], label=LABELS[system],
            )
            t8 = next(row for row in group if _required_number(row, "threads") == 8)
            ax.scatter([8], [_required_number(t8, "qps_median")], s=52,
                       facecolor=COLORS[system], edgecolor="black", linewidth=0.7,
                       zorder=4)
        ax.axvline(8, color="#777777", linestyle=":", linewidth=0.9)
        ax.set_xticks((1, 2, 4, 8, 16))
        _style(ax, "Concurrent queries (T)", "Throughput (QPS)", DATASET_LABELS[dataset])
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, frameon=False,
               bbox_to_anchor=(0.5, 0.99))
    fig.subplots_adjust(left=0.09, right=0.99, bottom=0.20, top=0.72, wspace=0.38)
    fig.savefig(path)
    plt.close(fig)


def _hide(rows: list[dict[str, str]], path: Path) -> None:
    import matplotlib.pyplot as plt
    import numpy as np
    from matplotlib.patches import Patch

    hide = [row for row in rows if row.get("phase") == "q4_hide"]
    cold_warm = [r for r in rows if r.get("phase") == "q4_cold_warm"]
    cache = sorted(_t2i_phase(rows, "q4_cache"),
                   key=lambda r: _number(r, "cache_gib"))
    fig, axes = plt.subplots(2, 2, figsize=(6.8, 5.0), constrained_layout=True)

    datasets = ("t2i10m", "yfcc10m", "laion10m")
    x = np.arange(len(datasets))
    width = 0.36
    flash_contract = [
        row for row in hide if row.get("system") == "flashanns"
    ]
    if any(
        _required_number(row, "score_triggered_flash_fills_median") != 0
        or abs(_required_number(row, "score_prematerialized_pct_median") - 100) > 1e-9
        for row in flash_contract
    ):
        raise FigureDataError("FlashANNS score-source contract is not satisfied")
    for index, system in enumerate(("demand", "flashanns")):
        selected = [next(row for row in hide
                         if row.get("dataset") == dataset and row.get("system") == system)
                    for dataset in datasets]
        xpos = x + (index - 0.5) * width
        coverage = [_required_number(row, "coverage_wait_ms_median") for row in selected]
        backpressure = [
            _required_number(row, "slot_backpressure_wait_ms_median") for row in selected
        ]
        axes[0, 0].bar(xpos, coverage, width, color=COLORS[system],
                       edgecolor="black", linewidth=0.45)
        axes[0, 0].bar(xpos, backpressure, width, bottom=coverage,
                       color=COLORS[system], edgecolor="black", linewidth=0.55,
                       hatch="////")
        axes[0, 1].bar(x + (index - 0.5) * width,
                       [_required_number(row, "qps_median") for row in selected],
                       width, color=COLORS[system], label=LABELS[system])
    short_labels = ("T2I", "YFCC", "LAION")
    max_stall = max(_required_number(row, "host_data_stall_ms_median") for row in hide)
    axes[0, 0].set_ylim(0, max_stall * 1.35)
    for ax in (axes[0, 0], axes[0, 1]):
        ax.set_xticks(x, short_labels)
    axes[0, 0].legend(
        handles=[
            Patch(facecolor="white", edgecolor="black", label="Waiting for required pages"),
            Patch(facecolor="white", edgecolor="black", hatch="////",
                  label="I/O admission backpressure"),
        ],
        fontsize=5.5, frameon=False, ncol=2, columnspacing=0.8,
        handlelength=1.4, loc="upper center", bbox_to_anchor=(0.5, 0.99),
    )
    axes[0, 1].legend(fontsize=8, frameon=False)
    axes[0, 0].text(
        0.50, 0.48,
        "Score-source audit\nFlash fills: 0/query; pre-materialized: 100%",
        transform=axes[0, 0].transAxes, va="center", ha="center", fontsize=5.5,
        bbox={"boxstyle": "round,pad=0.22", "facecolor": "white", "edgecolor": "#777777",
              "linewidth": 0.45, "alpha": 0.92},
    )
    _style(axes[0, 0], "", "Host data-dependency stall\n(ms/query)",
           "(a) Host data-dependency stall")
    _style(axes[0, 1], "", "Throughput (QPS)", "(b) End-to-end throughput")

    markers = {"t2i10m": "o", "yfcc10m": "s", "laion10m": "^"}
    state_colors = {"cold": "#d55e00", "warm": "#0072b2"}
    for row in cold_warm:
        axes[1, 0].scatter(_number(row, "qps_median"), _number(row, "latency_p99_ms_median"),
                           s=25 + 12 * _number(row, "flash_fills_per_query_median"),
                           marker=markers.get(row["dataset"], "o"),
                           color=state_colors.get(row["state"], "#777777"),
                           label=f"{row['dataset']} {row['state']}")
    _style(axes[1, 0], "Throughput (QPS)", "P99 latency (ms)", "(c) Cold / warm")
    axes[1, 0].legend(fontsize=5.5, frameon=False, ncol=2)

    if cache:
        cache_x = [_number(r, "cache_gib") for r in cache]
        qps4 = next((_number(r, "qps_median") for r in cache if _number(r, "cache_gib") == 4), 1.0) or 1.0
        p994 = next((_number(r, "latency_p99_ms_median") for r in cache if _number(r, "cache_gib") == 4), 1.0) or 1.0
        axes[1, 1].plot(cache_x, [100 * _number(r, "qps_median") / qps4 for r in cache], marker="o", label="QPS / 4 GiB")
        axes[1, 1].plot(cache_x, [100 * _number(r, "latency_p99_ms_median") / p994 for r in cache], marker="s", label="P99 / 4 GiB")
    axes[1, 1].axvline(4, color="#555555", linestyle=":", linewidth=0.8)
    axes[1, 1].legend(fontsize=6, frameon=False)
    _style(axes[1, 1], "CXL-side cache (GiB)", "Normalized metric (%)", "(d) Cache sensitivity")
    fig.savefig(path)
    plt.close(fig)


def render_figures(
    csv_path: Path, out_dir: Path, replace_q4_hide_csv: Path | None = None
) -> None:
    rows = _read(csv_path)
    if replace_q4_hide_csv is not None:
        rows = _replace_phase_rows(rows, _read(replace_q4_hide_csv), "q4_hide")
    validate_figure_rows(rows)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    import matplotlib.pyplot as plt
    with plt.rc_context(PLOT_STYLE):
        for dataset, name in (("t2i10m", "frontier-t2i.pdf"),
                              ("yfcc10m", "frontier-yfcc.pdf"),
                              ("laion10m", "frontier-laion.pdf")):
            _frontier(rows, dataset, out_dir / name)
        _wise(rows, out_dir / "wise-ablation.pdf")
        _batching(rows, out_dir / "batching-ablation.pdf")
        _hide(rows, out_dir / "hide-robustness.pdf")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--replace-q4-hide-csv", type=Path)
    parser.add_argument("--datasets", default="t2i10m,yfcc10m,laion10m")
    args = parser.parse_args()
    render_figures(args.csv, args.out_dir, args.replace_q4_hide_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
