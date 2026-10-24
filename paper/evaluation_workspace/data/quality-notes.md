# Evaluation data-quality notes

## Evidence that passed

- `current-six-figures.csv` has 110 aggregate marks and 504 columns.
- Every mark names exactly five accepted runs: 550 unique run IDs total.
- All 550 run IDs resolve under the accepted-evidence tree.
- The three current frontiers contain complete Demand (optimized layout),
  PipeANN, and FlashANNS sweeps. T2I has one extra PipeANN L=2400 point.
- Continuous-batching scaling contains both Wise-only and FlashANNS at
  T=1,2,4,8,16 on all three datasets.
- Hide/robustness contains six matched Demand/FlashANNS hide marks, six
  cold/warm marks, and four T2I cache-capacity marks.

## Gaps and risks

### High: revised original-layout frontier is incomplete

The later four-system frontier requires 30 original-layout Demand runs per
dataset. T2I is complete (30/30); YFCC has 23/30; LAION has 0/30. Only the T2I
four-system preview is safe to show.

### High: Wise extent-utilization metric is unavailable

All three T2I q3_t1 rows report `extent_use_pct_median = 0`. The current
plotter correctly draws no orange extent bars, but the paper must not interpret
their absence as measured zero utilization. This field is not usable evidence
for the How claim until the counter path is repaired and rerun, or the panel is
redesigned around supported metrics.

### Medium: distinguish host-window requests from NAND misses

`missing_pages_median` is derived from the runtime `requested_pages` counter.
It describes pages absent from the host scoring window when the committed wave
is formed. It is not the same as physical SSD misses after the 4 GiB
`vmem_sw` cache. Use measured backing-device bytes/commands for NAND traffic.

### Medium: open-loop load data is outside the current figure contract

Accepted q3_load records exist, but the accepted tree contains multiple rates
and campaign identities. The approved current plotting path rejects q3_load.
A future load figure must first select one binary/campaign protocol and rerun
the five-repeat aggregation checks; it must not concatenate all accepted rows.

## Visual QA

All seven bundled PDFs are one-page vector outputs with embedded fonts. The
frontiers use log y-axes and redundant line/marker encodings. The Wise figure's
missing extent bars reflect the data gap above. No bundled file was copied over
the manuscript's existing `paper/figs` files.
