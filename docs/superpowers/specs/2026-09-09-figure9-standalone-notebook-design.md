# Figure 9 Standalone Notebook Design

**Status:** Approved for implementation on 2026-09-09.

## Goal

Provide one self-contained Jupyter notebook for paper Figure 9. The notebook
must reproduce both the Wise Prefetcher ablation and continuous-batching
scaling plots without reading repository CSV files, accepted-run directories,
or live devices.

## Artifact

Create:

`paper/evaluation_workspace/notebooks/figure9.ipynb`

The executed notebook writes four files below a sibling `figure9-output/`
directory:

- `wise-ablation.pdf`
- `wise-ablation.png`
- `batching-ablation.pdf`
- `batching-ablation.png`

It does not overwrite `paper/figs/` by default.

## Embedded data contract

Embed exactly the 33 aggregate marks selected by the current Figure 9
renderer:

- three T2I-10M `q3_t1` marks for `serial-t1`, `batch-t1`, and `extent-t1`;
- thirty `q3_t8` marks covering T2I-10M, YFCC-10M, and LAION-10M, the
  `wise-only` and `flashanns` systems, and `T={1,2,4,8,16}`.

Each embedded row contains only the fields needed to reproduce or audit the
figure: dataset, phase, system, thread count where applicable, plotted
medians, 95% confidence bounds, supporting ablation metrics, and the five
accepted run IDs. Record the source aggregate path and its SHA-256 as
provenance text, but do not load it at runtime.

## Notebook structure

Use the following top-to-bottom flow:

1. Goal and provenance.
2. Imports and plotting style.
3. Embedded Figure 9 data.
4. Fail-closed checks for row count, matrix completeness, unique keys, finite
   numeric values, and exactly five run IDs per mark.
5. Wise Prefetcher ablation renderer.
6. Continuous-batching renderer.
7. Inline PNG previews and a compact audit summary.

The notebook uses only Python, Matplotlib, NumPy, and IPython display support.

## Plot compatibility

Preserve the current paper semantics and visual organization from
`experiments/eval/flashanns/plot_six_figures.py`:

- Wise Prefetcher panels remain `(a) When`, `(b) What`, and `(c) How`;
- batching remains three dataset panels with Wise-only versus FlashANNS;
- `T=8` remains highlighted as the primary configuration;
- labels, colors, PDF font type, and axis meanings remain consistent.

The notebook may add error bars from already embedded confidence intervals,
provided they do not change the values or panel structure.

## Validation

Completion requires:

- valid notebook structure via `nbformat`;
- successful top-to-bottom execution via `jupyter nbconvert --execute`;
- exactly 33 validated embedded rows and 165 referenced accepted run IDs;
- four nonempty outputs, with both PDFs exactly one page and containing
  embedded fonts;
- plotted T=8 values matching the paper values for all three datasets;
- no runtime file read other than Python package imports and notebook output
  creation.

