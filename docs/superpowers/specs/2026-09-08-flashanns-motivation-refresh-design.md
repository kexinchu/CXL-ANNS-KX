# FlashANNS Motivation Refresh Design

**Status:** Approved for implementation on 2026-09-08.  
**Scope:** Replace the Motivation measurements and figures without changing the
section's C1--C3 structure or the paper's Wise Prefetcher plus continuous
batching storyline.

## 1. Goal and reviewer contract

The refreshed Motivation must let an ASPLOS reviewer independently infer three
constraints before seeing the design:

1. **C1 -- The miss cliff.** A load served from the host scoring window or the
   CXL-side DRAM cache is fundamentally different from a 4 KiB fill from the
   Flash backing.
2. **C2 -- Prefetch must be wise.** Address-contiguous admission and wider
   pre-commit coverage increase physical Flash work faster than useful exact
   reranking payload.
3. **C3 -- One query cannot fill the device.** A query exposes a finite
   committed fill wave whose time-averaged device queue depth is below the
   concurrency knee of the same CXL-SSD path.

The evidence must explain why the two designs are necessary; complete-system
speedup remains in Evaluation.

## 2. Protected paper structure

The following topology is frozen:

- `The miss cliff` (`sec:mot-cliff`), Figure 3 (`fig:mot-cliff`);
- `Prefetch must be wise` (`sec:mot-page`), the two separately rendered halves
  of Figure 4 (`fig:mot-pathology`);
- `One query cannot fill the device` (`sec:mot-qd`), Figure 5
  (`fig:mot-bubbles`);
- `From measurements to constraints` (`sec:mot-map`) and the C1--C3 findings
  table (`tab:findings`).

No new subsection or paper figure is added. Figure 4 remains two one-page
vector PDFs, each 5.12 by 2.88 inches with 12 pt text. Figure 5 keeps its
current position, number, single-column footprint, and C3 argument, but its
implementation changes from `paper/sections/fig_mot_bubbles.tex` to the
measured `paper/figs/mot-c3-io-concurrency.pdf`; the TikZ source is retained as
pre-refresh provenance. The target Motivation length is 1.8--2.1 pages and
must not exceed 2.2 pages.

## 3. Platform wording and claim boundary

The Motivation begins with one compact path description:

- host DRAM holds the graph, PQ-64 codes, and a bounded scoring window;
- full-precision vectors reside in the `/dev/vmem0` logical address space;
- a fixed 4 GiB CXL-side DRAM cache fronts two 1.92 TB NVMe backings;
- page residency, faults, and fills are 4 KiB; 2 MiB is only the backing stripe;
- an ordinary load blocks on an absent page, whereas the asynchronous batch
  interface installs requested pages before scoring consumes them.

The prototype instantiates CXL-SSD semantics in `vmem_sw`; the paper does not
claim protocol-level latency or NAND-channel occupancy that the platform does
not measure. `aqu-sz` is called effective device queue depth. Host-window page
requests are never relabeled as NAND reads.

## 4. Common evidence protocol

New Motivation values must not use the July LAION-200k logs or constants
embedded in `paper/figs/plot_motivation.py`. Those files remain historical.

Every plotted mark and every capability-curve point requires exactly five
accepted repetitions. Each run binds:

- benchmark Git revision and executable SHA-256;
- dataset, image, query, and candidate-trace identities;
- `/dev/vmem0` offset, length, layout, cache limit, stripe size, and backing
  device identities;
- declared cache state and pre/post VMEM plus physical block counters;
- raw latency or I/O trace, completed operation count, and rejection reason;
- no overlapping evaluation or unrelated backing-device workload.

Aggregation reads accepted `run.json` records only and emits medians with 95%
bootstrap intervals. Plotters read only the aggregate CSV and contain no paper
numbers.

## 5. C1 experiment and Figure 3

Figure 3 remains a single-column log-scale grouped plot, now covering
T2I-10M, YFCC-10M, and LAION-10M. For each dataset it compares:

1. **Host window:** the vector page is already installed in a prefaulted host
   buffer; no VMEM fault and no physical read are permitted.
2. **CXL-DRAM cache:** the VMEM page is resident but its process PTE is dropped;
   faults are expected and physical reads must remain zero.
3. **CXL-SSD fill:** the page is absent from both upper tiers; faults and
   physical read bytes must both be positive.

Each repetition samples at least 2,000 deterministic, distinct vector pages
and records per-access latency. The plotted point is P50 and the whisker spans
P95--P99. The figure makes the three-tier ordering visible; it does not require
the former 89 microsecond result.

## 6. C2 experiments and Figure 4

Both panels use a freshly frozen LAION-10M PQ-navigation candidate trace at the
paper's representative recall point. The query IDs, committed candidates, and
recall remain identical across policies.

### 6.1 Figure 4(a): coarse admission

Compare selective 4 KiB admission with blind 16 KiB contiguous read-ahead.
The useful set is the 4 KiB pages containing committed exact-rerank vectors.
The 16 KiB policy adds the three following address-contiguous pages without
using graph or candidate semantics.

Plot materialization latency and physical NAND MiB/query; report useful-vector
byte fraction in the caption. This panel may say that coarse admission hurts
only if physical bytes increase and useful fraction decreases on the same
candidate trace.

### 6.2 Figure 4(b): wider speculative coverage

Compare Demand, Top-1, Top-2, and Top-8 pre-commit two-hop coverage. The roots
are ordered candidates from the frozen trace; speculative page sets are
derived from the frozen host graph and unioned with the committed useful set.

Plot physical NAND MiB/query as bars and useful-page percentage as a line;
annotate median materialization latency. Useful means that at least one exact
vector on a fetched page is consumed by committed reranking. No monotonic
shape is assumed: the paper reports the measured range and preserves any
counterexample.

## 7. C3 experiment and Figure 5

First measure a read-only `/dev/vmem0` asynchronous batch capability curve at
issued concurrency `{1,2,4,8,16,32,64,128}` using deterministic, nonrepeating
pages larger than the 4 GiB cache. Define:

`QD_knee = smallest issued concurrency reaching 90% of maximum stable physical read bandwidth`.

The capability sweep is calibration evidence; only `QD_knee` appears in the
paper.

Then run the frozen Wise Prefetcher at `T={1,2,4,8,16}` on all three datasets
at their representative recall point. Every point covers at least 30 seconds
or 2,000 completed queries and records the entire timed interval, including
idle device samples. On this host the installed `iostat` ignores sub-second
intervals, so the runner samples the same Linux block `weighted_io_ticks`
counter every 100 ms, sums both backings, and retains zero-valued intervals.
Figure 5 remains one single-column scatter: concurrent
queries on x, effective queue depth on y, one measured line per dataset, and a
horizontal `QD_knee` reference. Throughput stays in Evaluation.

## 8. Acceptance and claim gates

A condition is rejected if any of the following holds:

- fewer or more than five repetitions back a plotted mark;
- a tier classification disagrees with VMEM faults or physical read bytes;
- query, candidate, result, dataset, binary, or image identity drifts within a
  comparison block;
- a C2 condition changes recall or the committed useful set;
- a counter delta is negative, an I/O trace is missing, or another process uses
  either backing device;
- C3 excludes idle samples, calls `aqu-sz` NAND occupancy, or derives the
  reference from one extreme fio point;
- a plotter contains hard-coded measurement values.

If a predicted trend is absent, the data remain accepted and the prose claim
is weakened. Results are never reshaped or selectively omitted.

## 9. Paper revision boundary

Only measured values from the new aggregate may update
`paper/sections/motivation.tex`. Introduction receives numerical synchronization
only. Background may receive one clarification that 4 KiB is the fill unit and
2 MiB the stripe unit. Design and Evaluation keep their structure and measured
claims; only inconsistent terminology may change.

## 10. Exit condition

The refresh is complete when Figure 3, both Figure 4 PDFs, and Figure 5 can be
regenerated from accepted evidence; all points have five repetitions; all
numbers in Motivation resolve to the aggregate and provenance; the paper
builds without undefined references; and visual inspection confirms the
protected structure and page budget.
