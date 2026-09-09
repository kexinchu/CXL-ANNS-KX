"""Thin CLI wrapper for the authoritative Motivation plotter."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


REPO = Path(__file__).resolve().parents[3]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from experiments.motivation.flashanns.plot import plot_all, plot_figure5


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--all", action="store_true")
    args = parser.parse_args()
    if args.all:
        plot_all(args.csv, args.output)
    else:
        plot_figure5(args.csv, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
