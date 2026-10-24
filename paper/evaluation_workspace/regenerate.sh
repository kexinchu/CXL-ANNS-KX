#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
export OPENBLAS_NUM_THREADS=1
export OMP_NUM_THREADS=1
export MKL_NUM_THREADS=1
export MPLCONFIGDIR=${MPLCONFIGDIR:-/tmp/mpl-flashanns-paper}

python3 "$ROOT/scripts/plot_six_figures.py" \
  --csv "$ROOT/data/current-six-figures.csv" \
  --out-dir "$ROOT/figures/current"

python3 "$ROOT/scripts/render_supplemental.py"

for pdf in "$ROOT"/figures/current/*.pdf \
           "$ROOT"/figures/supplemental/*.pdf; do
  pdftocairo -png -singlefile -r 160 "$pdf" "${pdf%.pdf}"
done
