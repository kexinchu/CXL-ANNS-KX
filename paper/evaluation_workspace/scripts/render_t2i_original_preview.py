"""Render the T2I frontier with the completed original-layout Demand series."""

from pathlib import Path
import csv

import matplotlib.pyplot as plt

from plot_six_figures import PLOT_STYLE, _frontier


ROOT = Path(__file__).resolve().parents[1]


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def main() -> None:
    rows = read_rows(ROOT / "data/current-six-figures.csv")
    rows += read_rows(ROOT / "data/t2i-original-layout.csv")
    out = ROOT / "figures/t2i-original-preview/frontier-t2i.pdf"
    out.parent.mkdir(parents=True, exist_ok=True)
    with plt.rc_context(PLOT_STYLE):
        _frontier(rows, "t2i10m", out)


if __name__ == "__main__":
    main()
