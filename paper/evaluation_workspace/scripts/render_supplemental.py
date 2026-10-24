#!/usr/bin/env python3
"""Regenerate the two supplemental figures from the bundled aggregate CSV."""

from pathlib import Path

from supplemental_figures import render_figures


ROOT = Path(__file__).resolve().parents[1]
render_figures(
    ROOT / "data" / "supplemental-two-figures.csv",
    ROOT / "figures" / "supplemental",
)
