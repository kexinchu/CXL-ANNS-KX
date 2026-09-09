#!/usr/bin/env python3
"""Render the three independent physical-data-movement diagnostics.

The input is the accepted-only supplemental aggregate.  The renderer selects
only the two matched controls needed by each panel; it deliberately does not
draw a five-stage trend across different thread counts or mechanisms.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CSV = (
    ROOT
    / "results/eval/flashanns/provisional/supplemental-two-figures/validated.csv"
)
DEFAULT_OUTPUT = Path(__file__).resolve().parent / "figure10-output"

GREY = "#8c8c8c"
BLUE = "#0072b2"
EDGE = "#202020"
GRID = "#d9d9d9"
FIGSIZE = (2.45, 2.35)

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Liberation Serif"],
    "font.size": 12,
    "axes.titlesize": 12,
    "axes.labelsize": 12,
    "xtick.labelsize": 10,
    "ytick.labelsize": 9,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
    "savefig.facecolor": "white",
})


class Figure10DataError(ValueError):
    """Raised when the aggregate cannot support the frozen figure contract."""


@dataclass(frozen=True)
class Control:
    labels: tuple[str, str]
    values: tuple[float, float]
    lows: tuple[float, float]
    highs: tuple[float, float]
    auxiliary: tuple[float, float] | None = None

    @property
    def reduction_pct(self) -> float:
        return 100.0 * (self.values[0] - self.values[1]) / self.values[0]


def _number(row: dict[str, str], key: str) -> float:
    try:
        value = float(row[key])
    except (KeyError, TypeError, ValueError) as exc:
        raise Figure10DataError(f"{row.get('stage_name', '<unknown>')}: missing {key}") from exc
    return value


def _pair(
    rows: dict[str, dict[str, str]], stages: tuple[str, str], metric: str,
    labels: tuple[str, str], auxiliary: str | None = None,
) -> Control:
    try:
        selected = tuple(rows[stage] for stage in stages)
    except KeyError as exc:
        raise Figure10DataError(f"missing required I/O stage {exc.args[0]}") from exc
    return Control(
        labels=labels,
        values=tuple(_number(row, f"{metric}_median") for row in selected),
        lows=tuple(_number(row, f"{metric}_ci_low") for row in selected),
        highs=tuple(_number(row, f"{metric}_ci_high") for row in selected),
        auxiliary=(
            tuple(_number(row, f"{auxiliary}_median") for row in selected)
            if auxiliary else None
        ),
    )


def load_controls(path: Path) -> dict[str, Control]:
    with Path(path).open(newline="") as src:
        io_rows = [row for row in csv.DictReader(src) if row.get("kind") == "io"]
    by_stage = {row.get("stage_name", ""): row for row in io_rows}
    if len(by_stage) != len(io_rows):
        raise Figure10DataError("duplicate I/O stage in aggregate")
    return {
        "placement": _pair(
            by_stage, ("original-layout", "co-use-layout"),
            "requested_pages_per_query", ("Original", "Co-use"),
        ),
        "traffic": _pair(
            by_stage, ("original-layout", "co-use-layout"),
            "nand_mib_per_query", ("Original", "Co-use"),
            auxiliary="vector_slot_use_pct",
        ),
        "extent": _pair(
            by_stage, ("post-commit", "extent-reads"),
            "nand_commands_per_query", ("Post-commit", "Extent"),
            auxiliary="physical_kib_per_read",
        ),
    }


def _render_panel(
    control: Control, path: Path, *, title: str, ylabel: str,
    decimals: int, note: str | None = None,
) -> None:
    fig, ax = plt.subplots(figsize=FIGSIZE)
    x = (0, 1)
    errors = (
        [value - low for value, low in zip(control.values, control.lows)],
        [high - value for value, high in zip(control.values, control.highs)],
    )
    bars = ax.bar(
        x, control.values, width=0.58, color=(GREY, BLUE), edgecolor=EDGE,
        linewidth=0.8, yerr=errors, capsize=2,
        error_kw={"elinewidth": 0.8, "capthick": 0.8, "ecolor": EDGE},
    )
    ymax = max(control.highs) * (1.72 if note else 1.28)
    ax.set_ylim(0, ymax)
    ax.set_xticks(x, control.labels)
    ax.set_ylabel(ylabel)
    ax.set_title(title, loc="left", fontweight="bold", pad=5)
    ax.grid(axis="y", color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.spines[["top", "right"]].set_visible(False)
    ax.tick_params(direction="out", length=3, width=0.7)

    fmt = f"{{:.{decimals}f}}"
    offset = ymax * 0.018
    for bar, value in zip(bars, control.values):
        ax.text(
            bar.get_x() + bar.get_width() / 2, bar.get_height() + offset,
            fmt.format(value), ha="center", va="bottom", fontsize=9,
        )
    ax.text(
        0.5, 0.94, f"−{control.reduction_pct:.1f}%",
        transform=ax.transAxes, ha="center", va="top", fontsize=10,
    )
    if note:
        ax.text(
            0.5, 0.79, note, transform=ax.transAxes, ha="center", va="top",
            fontsize=8.2, bbox={"boxstyle": "round,pad=0.18", "facecolor": "white",
                              "edgecolor": "#bdbdbd", "linewidth": 0.5},
        )
    fig.subplots_adjust(
        left=0.27, right=0.97, bottom=0.23,
        top=0.78 if "\n" in title else 0.88,
    )
    fig.savefig(path, format="pdf")
    plt.close(fig)


def render(csv_path: Path = DEFAULT_CSV, output_dir: Path = DEFAULT_OUTPUT) -> None:
    controls = load_controls(csv_path)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    _render_panel(
        controls["placement"], output_dir / "figure10a-placement.pdf",
        title="(a) Placement", ylabel="Host-window requests\n(pages/query)",
        decimals=2,
    )
    slot = controls["traffic"].auxiliary
    assert slot is not None
    _render_panel(
        controls["traffic"], output_dir / "figure10b-traffic.pdf",
        title="(b) Physical NAND\ntraffic", ylabel="NAND traffic\n(MiB/query)",
        decimals=3,
        note=f"Useful vector slots: {slot[0]:.2f}% → {slot[1]:.2f}%",
    )
    size = controls["extent"].auxiliary
    assert size is not None
    _render_panel(
        controls["extent"], output_dir / "figure10c-extents.pdf",
        title="(c) Extent formation", ylabel="NAND read commands\n(per query)",
        decimals=2,
        note=f"Mean physical read: {size[0]:.2f} → {size[1]:.2f} KiB",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, default=DEFAULT_CSV)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    render(args.csv, args.out_dir)


if __name__ == "__main__":
    main()
