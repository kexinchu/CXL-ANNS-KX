"""Curate and render the two supplemental FlashANNS Evaluation figures."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any

class SupplementalDataError(ValueError):
    """Raised when selected evidence does not satisfy the frozen contract."""


DATASET_LABELS = {
    "t2i10m": "T2I-10M",
    "yfcc10m": "YFCC-10M",
    "laion10m": "LAION-10M",
}
SYSTEM_LABELS = {
    "wise-only": "Wise only",
    "pipeann": "PipeANN",
    "flashanns": "FlashANNS",
}
COLORS = {
    "wise-only": "#d55e00",
    "pipeann": "#e69f00",
    "flashanns": "#0072b2",
}
MARKERS = {"wise-only": "o", "pipeann": "^", "flashanns": "s"}
LINESTYLES = {"wise-only": "--", "pipeann": ":", "flashanns": "-"}
PLOT_STYLE = {
    "font.size": 12,
    "axes.titlesize": 12,
    "axes.labelsize": 11,
    "xtick.labelsize": 9,
    "ytick.labelsize": 9,
    "legend.fontsize": 9,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
    "savefig.facecolor": "white",
}

LOAD_METRICS = (
    "offered_QPS", "throughput_QPS", "mean_latency_ms",
    "latency_p50_ms", "latency_p95_ms", "latency_p99_ms",
    "queue_wait_ms_mean", "queue_wait_ms_p95", "recall@10",
    "completed_queries",
)
IO_METRICS = (
    "throughput_QPS", "recall@10", "requested_pages", "nand_read_bytes",
    "nand_read_commands", "slots_on_pages", "slots_scored", "slot_use_pct",
)


def _load_records(root: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    seen: set[str] = set()
    for path in sorted(Path(root).rglob("run.json")):
        record = json.loads(path.read_text())
        run_id = record.get("run_id")
        if not run_id:
            raise SupplementalDataError(f"{path}: missing run_id")
        if run_id in seen:
            raise SupplementalDataError(f"duplicate run_id {run_id}")
        seen.add(run_id)
        records.append(record)
    return records


def _metric(record: dict[str, Any], key: str) -> float:
    value = record.get("metrics", {}).get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise SupplementalDataError(f"{record.get('run_id')}: missing metric {key}")
    return float(value)


def _validate_group(
    group: list[dict[str, Any]], *, expected_repeats: set[int], expected_nq: int,
    expected_binary: str, require_candidates: bool,
) -> None:
    identity = group[0].get("run_id") if group else "empty selection"
    repeats = {int(record.get("repeat", -1)) for record in group}
    if len(group) != len(expected_repeats) or repeats != expected_repeats:
        raise SupplementalDataError(
            f"{identity}: require exact repeats {sorted(expected_repeats)}, got {sorted(repeats)}"
        )
    for record in group:
        if record.get("validation", {}).get("status") != "accepted":
            raise SupplementalDataError(f"{record.get('run_id')}: not accepted")
        if int(record.get("nq", 0)) != expected_nq:
            raise SupplementalDataError(f"{record.get('run_id')}: nq mismatch")
        if int(_metric(record, "completed_queries")) != expected_nq:
            raise SupplementalDataError(
                f"{record.get('run_id')}: completed_queries mismatch"
            )
        if record.get("binary_sha256") != expected_binary:
            raise SupplementalDataError(f"{record.get('run_id')}: binary_sha256 mismatch")
    for key in ("artifact_manifest_sha256", "binary_sha256"):
        if len({record.get(key) for record in group}) != 1:
            raise SupplementalDataError(f"{identity}: {key} mismatch")
    sidecar_keys = ["query_ids_sha256"]
    if require_candidates:
        sidecar_keys.append("candidate_ids_sha256")
    for key in sidecar_keys:
        values = {record.get("sidecars", {}).get(key) for record in group}
        if None in values or len(values) != 1:
            raise SupplementalDataError(f"{identity}: {key} mismatch")


def _summary(values: list[float]) -> tuple[float, float, float]:
    from experiments.eval.flashanns.aggregate import bootstrap_ci
    low, high = bootstrap_ci(values)
    return statistics.median(values), low, high


def _add_summary(row: dict[str, Any], name: str, values: list[float]) -> None:
    median, low, high = _summary(values)
    row[f"{name}_median"] = median
    row[f"{name}_ci_low"] = low
    row[f"{name}_ci_high"] = high


def curate_records(
    records: list[dict[str, Any]], contract: dict[str, Any]
) -> tuple[list[dict[str, Any]], dict[str, Any], dict[str, Any]]:
    expected_nq = int(contract["expected_nq"])
    expected_repeats = {int(value) for value in contract["repeats"]}
    rows: list[dict[str, Any]] = []
    marks: list[dict[str, Any]] = []
    used_ids: set[str] = set()

    load = contract["load"]
    for dataset, dataset_contract in load["datasets"].items():
        query_hashes: set[str] = set()
        for system in load["systems"]:
            system_L = int(dataset_contract["L"][system])
            for rate_index, rate in enumerate(dataset_contract["rates"]):
                group = [
                    record for record in records
                    if record.get("dataset") == dataset
                    and record.get("phase") == "q3_load"
                    and record.get("system") == system
                    and record.get("campaign_tag") == dataset_contract["campaign_tag"]
                    and int(record.get("L", -1)) == system_L
                    and float(record.get("arrival_rate", -1)) == float(rate)
                ]
                _validate_group(
                    group, expected_repeats=expected_repeats,
                    expected_nq=expected_nq,
                    expected_binary=dataset_contract["binary_sha256"][system],
                    require_candidates=not any(record.get("external") for record in group),
                )
                for metric in LOAD_METRICS:
                    [_metric(record, metric) for record in group]
                query_hashes.update(
                    record["sidecars"]["query_ids_sha256"] for record in group
                )
                ordered = sorted(group, key=lambda record: record["repeat"])
                run_ids = [record["run_id"] for record in ordered]
                if used_ids.intersection(run_ids):
                    raise SupplementalDataError("a run was selected by multiple marks")
                used_ids.update(run_ids)
                row: dict[str, Any] = {
                    "kind": "load", "dataset": dataset, "system": system,
                    "campaign_tag": dataset_contract["campaign_tag"],
                    "L": system_L, "rate_index": rate_index,
                    "rate_fraction": (0.25, 0.50, 0.75, 1.00, 1.25)[rate_index],
                    "offered_qps": float(rate), "run_ids": ";".join(run_ids),
                }
                aliases = {
                    "throughput_QPS": "qps", "recall@10": "recall_at_10",
                }
                for metric in LOAD_METRICS:
                    output = aliases.get(metric, metric)
                    _add_summary(row, output, [_metric(record, metric) for record in group])
                rows.append(row)
                marks.append({"kind": "load", "key": [dataset, system, rate],
                              "run_ids": run_ids})
        if len(query_hashes) != 1:
            raise SupplementalDataError(f"{dataset}: query_ids_sha256 mismatch")

    io = contract["io"]
    io_query_hashes: set[str] = set()
    io_candidate_hashes: set[str] = set()
    useful_bytes_per_vector = int(io["dimension"]) * int(io["element_bytes"])
    for stage_order, stage in enumerate(io["stages"]):
        group = [
            record for record in records
            if record.get("dataset") == io["dataset"]
            and record.get("phase") == stage["phase"]
            and record.get("system") == stage["system"]
            and record.get("campaign_tag") == stage["campaign_tag"]
            and int(record.get("L", -1)) == int(io["L"])
            and int(record.get("threads", record.get("metrics", {}).get("threads", -1)))
            == int(stage["threads"])
        ]
        _validate_group(
            group, expected_repeats=expected_repeats, expected_nq=expected_nq,
            expected_binary=stage["binary_sha256"], require_candidates=True,
        )
        for metric in IO_METRICS:
            [_metric(record, metric) for record in group]
        io_query_hashes.update(record["sidecars"]["query_ids_sha256"] for record in group)
        io_candidate_hashes.update(
            record["sidecars"]["candidate_ids_sha256"] for record in group
        )
        ordered = sorted(group, key=lambda record: record["repeat"])
        run_ids = [record["run_id"] for record in ordered]
        if used_ids.intersection(run_ids):
            raise SupplementalDataError("a run was selected by multiple marks")
        used_ids.update(run_ids)
        row = {
            "kind": "io", "dataset": io["dataset"], "L": io["L"],
            "stage_order": stage_order, "stage_name": stage["name"],
            "stage_label": stage["label"], "phase": stage["phase"],
            "system": stage["system"], "campaign_tag": stage["campaign_tag"],
            "threads": stage["threads"],
            "run_ids": ";".join(run_ids),
        }
        derived: dict[str, list[float]] = defaultdict(list)
        for record in group:
            nq = float(record["nq"])
            nand_bytes = _metric(record, "nand_read_bytes")
            useful_bytes = _metric(record, "slots_scored") * useful_bytes_per_vector
            if useful_bytes <= 0:
                raise SupplementalDataError(f"{record['run_id']}: zero useful payload")
            derived["qps"].append(_metric(record, "throughput_QPS"))
            derived["recall_at_10"].append(_metric(record, "recall@10"))
            derived["requested_pages_per_query"].append(
                _metric(record, "requested_pages") / nq
            )
            derived["nand_mib_per_query"].append(nand_bytes / nq / 1_048_576.0)
            derived["nand_commands_per_query"].append(
                _metric(record, "nand_read_commands") / nq
            )
            derived["vector_slot_use_pct"].append(_metric(record, "slot_use_pct"))
            derived["physical_read_amplification"].append(nand_bytes / useful_bytes)
            commands = _metric(record, "nand_read_commands")
            if commands <= 0:
                raise SupplementalDataError(f"{record['run_id']}: zero NAND commands")
            derived["physical_kib_per_read"].append(nand_bytes / commands / 1024.0)
        for metric, values in derived.items():
            _add_summary(row, metric, values)
        rows.append(row)
        marks.append({"kind": "io", "key": [stage["name"]], "run_ids": run_ids})
    if io["stages"] and len(io_query_hashes) != 1:
        raise SupplementalDataError("I/O stages: query_ids_sha256 mismatch")
    if io["stages"] and len(io_candidate_hashes) != 1:
        raise SupplementalDataError("I/O stages: candidate_ids_sha256 mismatch")

    report = {
        "status": "accepted", "load_marks": sum(row["kind"] == "load" for row in rows),
        "io_marks": sum(row["kind"] == "io" for row in rows),
        "unique_run_count": len(used_ids),
        "metric_notes": {
            "requested_pages_per_query": "host-window requests, not physical NAND misses",
            "nand_mib_per_query": "physical NVMe device read bytes per query",
            "physical_read_amplification": "physical NAND bytes divided by slots_scored times exact-vector payload bytes",
            "physical_kib_per_read": "physical NAND bytes divided by NAND read commands",
            "extent_use_pct": "excluded because the frozen counter is zero and invalid for claims",
        },
    }
    return rows, {"contract_version": 1, "marks": marks}, report


def write_bundle(
    rows: list[dict[str, Any]], provenance: dict[str, Any],
    report: dict[str, Any], out_dir: Path,
) -> None:
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    fields = sorted({key for row in rows for key in row})
    with (out_dir / "validated.csv").open("w", newline="") as dst:
        writer = csv.DictWriter(dst, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    (out_dir / "provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n"
    )
    (out_dir / "quality-report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n"
    )


def _read_csv(path: Path) -> list[dict[str, str]]:
    with Path(path).open(newline="") as src:
        return list(csv.DictReader(src))


def _f(row: dict[str, str], key: str) -> float:
    value = row.get(key, "")
    if value in ("", None):
        raise SupplementalDataError(f"plot row lacks {key}")
    return float(value)


def _style(ax: Any, xlabel: str, ylabel: str, title: str) -> None:
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title, loc="left", fontweight="bold")
    ax.grid(True, axis="y", color="#d9d9d9", linewidth=0.7)
    ax.set_axisbelow(True)


def _render_load(rows: list[dict[str, str]], path: Path) -> None:
    import matplotlib.pyplot as plt

    load_rows = [row for row in rows if row.get("kind") == "load"]
    fig, axes = plt.subplots(1, 3, figsize=(7.4, 2.85))
    for ax, dataset in zip(axes, DATASET_LABELS):
        for system in ("wise-only", "pipeann", "flashanns"):
            group = sorted(
                (row for row in load_rows if row["dataset"] == dataset
                 and row["system"] == system),
                key=lambda row: _f(row, "offered_qps"),
            )
            if not group:
                continue
            x = [_f(row, "qps_median") for row in group]
            y = [_f(row, "latency_p99_ms_median") for row in group]
            low = [_f(row, "latency_p99_ms_ci_low") for row in group]
            high = [_f(row, "latency_p99_ms_ci_high") for row in group]
            ax.errorbar(
                x, y, yerr=([value-lo for value, lo in zip(y, low)],
                             [hi-value for value, hi in zip(y, high)]),
                color=COLORS[system], marker=MARKERS[system],
                linestyle=LINESTYLES[system], capsize=2,
                label=SYSTEM_LABELS[system],
            )
        ax.set_yscale("log")
        _style(ax, "Achieved throughput (QPS)", "P99 latency (ms)",
               DATASET_LABELS[dataset])
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=3, frameon=False,
               bbox_to_anchor=(0.5, 0.995))
    fig.subplots_adjust(left=0.09, right=0.95, bottom=0.20, top=0.73, wspace=0.42)
    fig.savefig(path)
    fig.savefig(path.with_suffix(".png"), dpi=200)
    plt.close(fig)


def _render_io(rows: list[dict[str, str]], path: Path) -> None:
    import matplotlib.pyplot as plt
    import numpy as np

    io_rows = sorted(
        (row for row in rows if row.get("kind") == "io"),
        key=lambda row: _f(row, "stage_order"),
    )
    if len(io_rows) != 5:
        raise SupplementalDataError("I/O figure requires five stages")
    labels = [row["stage_label"] for row in io_rows]
    short = ("Original", "Co-use", "Serialized", "Post-commit", "Extent")
    x = np.arange(len(io_rows))
    fig, axes = plt.subplots(1, 3, figsize=(7.4, 2.95))

    pages = [_f(row, "requested_pages_per_query_median") for row in io_rows]
    nand = [_f(row, "nand_mib_per_query_median") for row in io_rows]
    width = 0.34
    axes[0].bar(x-width/2, [100*value/pages[0] for value in pages], width,
                color="#999999", label="Window pages/q")
    axes[0].bar(x+width/2, [100*value/nand[0] for value in nand], width,
                color="#56b4e9", label="NAND MiB/q")
    axes[0].axhline(100, color="#666666", linestyle=":", linewidth=0.8)
    axes[0].legend(fontsize=7, frameon=False)
    _style(axes[0], "", "Traffic (% of original)", "(a) Fetched traffic")

    slot = [_f(row, "vector_slot_use_pct_median") for row in io_rows]
    amp = [_f(row, "physical_read_amplification_median") for row in io_rows]
    axes[1].plot(x, slot, color="#0072b2", marker="s", label="Useful slots")
    axes[1].set_ylabel("Useful slots (%)", color="#0072b2", labelpad=2)
    axes[1].tick_params(axis="y", labelcolor="#0072b2")
    axes[1].grid(True, axis="y", color="#d9d9d9", linewidth=0.7)
    amp_ax = axes[1].twinx()
    amp_ax.plot(x, amp, color="#d55e00", marker="o", linestyle="--",
                label="Read amplification")
    amp_ax.set_ylabel("Read amp. (x)", color="#d55e00", labelpad=2)
    amp_ax.tick_params(axis="y", labelcolor="#d55e00")
    axes[1].set_title("(b) Useful payload", loc="left", fontweight="bold")

    commands = [_f(row, "nand_commands_per_query_median") for row in io_rows]
    kib_per_read = [_f(row, "physical_kib_per_read_median") for row in io_rows]
    axes[2].bar(x, [100*value/commands[0] for value in commands],
                color="#e69f00", width=0.58)
    axes[2].set_ylabel("NAND cmds (% orig.)", color="#8c6200", labelpad=2)
    axes[2].tick_params(axis="y", labelcolor="#8c6200")
    size_ax = axes[2].twinx()
    size_ax.plot(x, kib_per_read, color="#0072b2", marker="D")
    size_ax.set_ylabel("KiB / device read", color="#0072b2", labelpad=2)
    size_ax.tick_params(axis="y", labelcolor="#0072b2")
    axes[2].set_title("(c) Device work", loc="left", fontweight="bold")
    axes[2].grid(True, axis="y", color="#d9d9d9", linewidth=0.7)

    for ax in axes:
        ax.axvline(1.5, color="#888888", linestyle=":", linewidth=0.8)
        ax.set_xticks(x, short, rotation=27, ha="right")
        transform = ax.get_xaxis_transform()
        ax.text(0.5, 0.98, "T=8", transform=transform,
                ha="center", va="top", fontsize=8)
        ax.text(3.0, 0.98, "T=1", transform=transform,
                ha="center", va="top", fontsize=8)
    fig.subplots_adjust(left=0.07, right=0.93, bottom=0.28, top=0.90, wspace=0.72)
    fig.savefig(path)
    fig.savefig(path.with_suffix(".png"), dpi=200)
    plt.close(fig)


def render_figures(csv_path: Path, out_dir: Path) -> None:
    rows = _read_csv(csv_path)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    import matplotlib.pyplot as plt
    with plt.rc_context(PLOT_STYLE):
        _render_load(rows, out_dir / "load-tail-latency.pdf")
        _render_io(rows, out_dir / "prefetch-io-efficiency.pdf")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--accepted-root", required=True, type=Path)
    parser.add_argument("--contract", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args()
    contract = json.loads(args.contract.read_text())
    rows, provenance, report = curate_records(_load_records(args.accepted_root), contract)
    write_bundle(rows, provenance, report, args.out_dir)
    render_figures(args.out_dir / "validated.csv", args.out_dir / "figures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
