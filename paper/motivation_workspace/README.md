# FlashANNS Motivation figure workspace

This directory is the paper-facing, device-free bundle for the Motivation figures in the
current manuscript build. The historical figure numbers in the experiment
spec are one higher; labels in `paper/sections/motivation.tex` are authoritative.

## Contents

- `data/motivation.csv`: accepted-only medians and deterministic 95% bootstrap
  intervals. The authoritative current CSV is under `results/motivation/flashanns-10m/aggregate/`.
- `data/provenance.json`: the five sealed `run.json` records behind every mark.
- `data/quality-report.json`: 205 accepted runs, five repetitions per mark.
- `data/measured-claims.md`: paper-number to aggregate-row/run-ID mapping.
- `motivation_figures_embedded.py`: compatibility entry point; despite its historical
  name, it embeds no measurements and renders only Figure 5 by default.
- `scripts/plot.py`: thin CLI wrapper around the repository plotter.
- `scripts/aggregate.py`: reference accepted-record aggregator. It needs the
  repository package and sealed records; figure adjustment does not.
- `figures/`: canonical PDFs and inspection PNGs.

## Re-render without the device

Run from the repository root:

```bash
MPLCONFIGDIR=/tmp/matplotlib-motivation \
python3 paper/motivation_workspace/motivation_figures_embedded.py \
  --csv results/motivation/flashanns-10m/aggregate/motivation.csv \
  --output /tmp/flashanns-motivation-rerender
```

Add `--all` only when all Motivation figures should be regenerated. The plotter
contains styling and labels, but no measurement arrays. Figure 3
in the experiment spec is a 5.12 x 2.88 inch PDF. Both halves of its Figure 4
are separate 5.12 x 2.88 inch PDFs with 12 pt plotting text. The measured C3
figure is a 3.35 x 2.45 inch, two-panel, single-column PDF. All four outputs
are vector PDF files.

## Evidence boundary

Only records with `validation.status=accepted` contribute. The bundle does not
contain raw device traces, and does not establish new hardware results by
itself. Raw and sealed evidence remain under
`results/motivation/flashanns-10m/`. The July LAION-200k data and
`paper/figs/plot_motivation.py` are historical and are not inputs.
