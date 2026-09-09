"""Compatibility entry point for reproducible Motivation figures.

The former version embedded a stale copy of every plotted number. The
accepted-only aggregate CSV is now the sole data source, while the plotting
implementation lives in ``experiments.motivation.flashanns.plot``.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from experiments.motivation.flashanns.plot import plot_all, plot_figure5


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--csv",
        type=Path,
        default=REPO / "results/motivation/flashanns-10m/aggregate/motivation.csv",
        help="accepted-only aggregate CSV",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).with_name("generated"),
        help="output directory",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="render all Motivation figures; default renders only Figure 5",
    )
    args = parser.parse_args()
    if args.all:
        plot_all(args.csv, args.output)
    else:
        plot_figure5(args.csv, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
