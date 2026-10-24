"""Render the YFCC frontier with only the complete original-layout marks."""

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
    rows += read_rows(ROOT / "data/original-layout-complete-marks.csv")
    out = ROOT / "figures/yfcc-original-partial/frontier-yfcc.pdf"
    out.parent.mkdir(parents=True, exist_ok=True)
    with plt.rc_context(PLOT_STYLE):
        _frontier(rows, "yfcc10m", out)


if __name__ == "__main__":
    main()
