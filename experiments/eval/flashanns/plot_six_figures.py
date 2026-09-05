"""Render the six fixed Evaluation figures from validated aggregate CSV."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Any, Iterable


COLORS = {
    "demand": "#7f7f7f", "pipeann": "#e69f00", "flashanns": "#0072b2",
    "serial-t1": "#7f7f7f", "batch-t1": "#56b4e9", "extent-t1": "#0072b2",
    "wise-only": "#d55e00",
}
LABELS = {
    "demand": "Demand", "pipeann": "PipeANN", "flashanns": "FlashANNS",
    "serial-t1": "Serialized", "batch-t1": "Post-commit batch",
    "extent-t1": "Co-use + extent", "wise-only": "Wise only",
}


def _read(path: Path) -> list[dict[str, str]]:
    with Path(path).open(newline="") as src:
        return list(csv.DictReader(src))


def _number(row: dict[str, str], key: str, default: float = 0.0) -> float:
    raw = row.get(key, "")
    return default if raw in ("", None) else float(raw)


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
    ax.grid(True, alpha=0.22)


def _frontier(rows: list[dict[str, str]], dataset: str, path: Path) -> None:
    import matplotlib.pyplot as plt

    selected = [r for r in rows if r.get("phase") == "q2" and r.get("dataset") == dataset]
    fig, axes = plt.subplots(1, 2, figsize=(6.4, 2.55), constrained_layout=True)
    for system in ("demand", "pipeann", "flashanns"):
        group = sorted((r for r in selected if r.get("system") == system),
                       key=lambda r: _number(r, "recall_at_10_median"))
        if not group:
            continue
        recall = [_number(r, "recall_at_10_median") for r in group]
        for ax, metric in zip(axes, ("mean_latency_ms_median", "qps_median")):
            values = [_number(r, metric) for r in group]
            errors = [_error(r, metric) for r in group]
            ax.errorbar(recall, values,
                        yerr=([e[0] for e in errors], [e[1] for e in errors]),
                        marker="o", capsize=2, color=COLORS[system], label=LABELS[system])
    _style(axes[0], "Recall@10", "Mean latency (ms)", "(a) Latency frontier")
    _style(axes[1], "Recall@10", "Throughput (QPS)", "(b) Throughput frontier")
    for ax in axes:
        ax.axvline(0.90, color="#999999", linestyle="--", linewidth=0.8)
    if dataset == "t2i10m":
        for ax in axes:
            ax.axvline(0.92, color="#555555", linestyle=":", linewidth=0.8)
    handles, labels = axes[0].get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc="upper center", ncol=3, frameon=False)
    fig.savefig(path)
    plt.close(fig)


def _wise(rows: list[dict[str, str]], path: Path) -> None:
    import matplotlib.pyplot as plt
    import numpy as np

    selected = _t2i_phase(rows, "q3_t1")
    by_system = {r["system"]: r for r in selected}
    systems = [s for s in ("serial-t1", "batch-t1", "extent-t1") if s in by_system]
    fig, axes = plt.subplots(1, 3, figsize=(7.4, 2.65), constrained_layout=True)

    values = [_number(by_system[s], "qps_median") for s in systems]
    axes[0].bar(range(len(systems)), values, color=[COLORS[s] for s in systems])
    axes[0].set_xticks(range(len(systems)), [LABELS[s] for s in systems], rotation=18, ha="right")
    _style(axes[0], "", "Throughput (QPS)", "(a) When")

    source = by_system.get("batch-t1") or by_system.get("extent-t1")
    funnel_keys = ("committed_candidates_median", "unique_pages_median", "missing_pages_median")
    funnel = [_number(source, key) if source else 0 for key in funnel_keys]
    axes[1].bar(range(3), funnel, color=("#999999", "#56b4e9", "#0072b2"))
    axes[1].set_xticks(range(3), ("Committed", "Unique", "Missing"), rotation=18, ha="right")
    _style(axes[1], "", "Items/query", "(b) What")
    if source:
        axes[1].text(0.98, 0.98,
                     f"{_number(source, 'nand_mib_per_query_median'):.2g} MiB/q\n"
                     f"{_number(source, 'nand_commands_per_query_median'):.2g} cmds/q",
                     transform=axes[1].transAxes, va="top", ha="right", fontsize=7)

    compare = [s for s in ("batch-t1", "extent-t1") if s in by_system]
    x = np.arange(len(compare))
    width = 0.26
    metrics = (("vector_slot_use_pct_median", "Vector slots"),
               ("extent_use_pct_median", "Extent"))
    for index, (metric, label) in enumerate(metrics):
        axes[2].bar(x + (index - 0.5) * width,
                    [_number(by_system[s], metric) for s in compare], width, label=label)
    baseline = _number(by_system.get("batch-t1", {}), "qps_median", 1.0) or 1.0
    axes[2].bar(x + 1.5 * width,
                [100 * _number(by_system[s], "qps_median") / baseline for s in compare],
                width, label="QPS vs. batch")
    axes[2].set_xticks(x + 0.5 * width, [LABELS[s] for s in compare], rotation=18, ha="right")
    axes[2].set_ylim(bottom=0)
    axes[2].legend(fontsize=6, frameon=False)
    _style(axes[2], "", "Utilization / normalized QPS (%)", "(c) How")
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

    scaling = _t2i_phase(rows, "q3_t8")
    load = _t2i_phase(rows, "q3_load")
    fig, axes = plt.subplots(1, 3, figsize=(7.4, 2.65), constrained_layout=True)
    _group_lines(axes[0], scaling, "threads", "qps_median")
    _style(axes[0], "Concurrent queries", "Throughput (QPS)", "(a) Scaling")
    axes[0].legend(fontsize=6, frameon=False)

    for system in sorted({r["system"] for r in scaling}):
        group = sorted((r for r in scaling if r["system"] == system), key=lambda r: _number(r, "threads"))
        bandwidth = [_number(r, "useful_bandwidth_mib_s_median") for r in group]
        peak = max(bandwidth, default=1.0) or 1.0
        axes[1].plot([_number(r, "threads") for r in group],
                     [_number(r, "nand_occupancy_pct_median") for r in group], marker="o",
                     color=COLORS.get(system), label=f"{LABELS.get(system, system)} occ.")
        axes[1].plot([_number(r, "threads") for r in group], [100 * v / peak for v in bandwidth],
                     marker="x", linestyle="--", color=COLORS.get(system),
                     label=f"{LABELS.get(system, system)} useful BW")
    _style(axes[1], "Concurrent queries", "Occupancy / normalized BW (%)", "(b) Device use")
    axes[1].legend(fontsize=5.5, frameon=False)

    _group_lines(axes[2], load, "qps_median", "latency_p99_ms_median")
    _style(axes[2], "Achieved throughput (QPS)", "P99 latency (ms)", "(c) Load curve")
    axes[2].legend(fontsize=5.5, frameon=False)
    fig.savefig(path)
    plt.close(fig)


def _hide(rows: list[dict[str, str]], path: Path) -> None:
    import matplotlib.pyplot as plt
    import numpy as np

    hide = _t2i_phase(rows, "q4_hide")
    cold_warm = [r for r in rows if r.get("phase") == "q4_cold_warm"]
    cache = sorted(_t2i_phase(rows, "q4_cache"),
                   key=lambda r: _number(r, "cache_gib"))
    fig, axes = plt.subplots(2, 2, figsize=(6.8, 5.0), constrained_layout=True)

    systems = [s for s in ("demand", "flashanns") if any(r["system"] == s for r in hide)]
    x = np.arange(len(systems))
    bottom = np.zeros(len(systems))
    for metric, label in (("pq_nav_ms_median", "PQ nav"),
                          ("critical_wait_ms_median", "Materialize wait"),
                          ("exact_rerank_ms_median", "Rerank"),
                          ("queue_wait_ms_median", "Queue")):
        values = [_number(next(r for r in hide if r["system"] == s), metric) for s in systems]
        axes[0, 0].bar(x, values, bottom=bottom, label=label)
        bottom += np.asarray(values)
    axes[0, 0].set_xticks(x, [LABELS[s] for s in systems])
    axes[0, 0].legend(fontsize=6, frameon=False)
    _style(axes[0, 0], "", "Mean latency (ms)", "(a) Latency breakdown")

    bottom = np.zeros(len(systems))
    for metric, label in (("score_host_pct_median", "Host"),
                          ("score_cxl_pct_median", "CXL cache"),
                          ("score_bounce_pct_median", "Bounce"),
                          ("score_flash_pct_median", "Flash")):
        values = [_number(next(r for r in hide if r["system"] == s), metric) for s in systems]
        axes[0, 1].bar(x, values, bottom=bottom, label=label)
        bottom += np.asarray(values)
    axes[0, 1].set_xticks(x, [LABELS[s] for s in systems])
    axes[0, 1].set_ylim(0, 100)
    axes[0, 1].legend(fontsize=6, frameon=False)
    _style(axes[0, 1], "", "Exact-score source (%)", "(b) Score source")

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


def render_figures(csv_path: Path, out_dir: Path) -> None:
    rows = _read(csv_path)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
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
    parser.add_argument("--datasets", default="t2i10m,yfcc10m,laion10m")
    args = parser.parse_args()
    render_figures(args.csv, args.out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
