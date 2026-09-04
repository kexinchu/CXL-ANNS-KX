# FlashANNS Evaluation Design Specification

**Status:** Approved; placeholder skeleton implemented  
**Date:** 2026-09-03  
**Scope:** ASPLOS-style evaluation of the two-mechanism Design on LAION-10M,
T2I-10M, and YFCC-10M

## Evaluation Standard

The Evaluation must make the paper's causal claim reviewable rather than show
only favorable QPS.  A strong result must establish four facts:

1. FlashANNS improves quality--latency and quality--throughput tradeoffs over a
   demand-filled CXL-SSD baseline on three 10M-scale datasets.
2. It approaches, but is not conflated with, a full-corpus CXL-DRAM Oracle.
3. The improvement comes from Wise Prefetching and Continuous Batching, with
   graph-search semantics held fixed.
4. Every reported FlashANNS score is computed from CXL-DRAM-resident bytes;
   neither a bounce buffer nor an implicit synchronous CXL-SSD read is counted
   as score hide.

The target is approximately 4--4.5 compiled two-column pages.  Results may
replace placeholders only after the validation gates below pass.  A visually
strong curve is not sufficient evidence if its run manifest, recall, or hide
counters fail.

## Claims and Evaluation Questions

The section answers six questions in this order:

- **Q1 Methodology:** Are the three systems, datasets, search parameters, cold
  state, and resource budgets directly comparable?
- **Q2 End to end:** How do recall, mean latency, p99 latency, and saturated
  throughput trade off on all three datasets?
- **Q3 Hide proof:** Are scores actually served from CXL-DRAM, how much NAND
  wait remains, and how much of the Oracle gap is closed?
- **Q4 Wise Prefetcher:** Do mandatory/optional separation and bounded
  lookahead outperform mandatory-only and blind lookahead without changing
  recall?
- **Q5 Continuous Batching:** Does cross-query scheduling sustain useful device
  work while bounding tail latency, compared with blocking and static batches?
- **Q6 Robustness and cost:** Do the conclusions survive true-cold execution,
  window pressure, dataset variation, and resource accounting?

Motivation results are not reproduced as another Evaluation question.  Their
role is to predict the mechanisms; Evaluation tests the resulting system.

## Systems Under Comparison

### Core controlled comparison

All three core systems use the same query selector, graph, complete-entry
layout, distance kernel, termination rule, `L`, hop budget, query order, and
CPU allocation.

- **Oracle:** the full graph and vectors reside in CXL-DRAM.  No CXL-SSD fill is
  permitted in the timed region.  Oracle is the upper bound for the same graph
  walk, not a different index.
- **FlashANNS:** CXL-SSD is authoritative; a bounded CXL-DRAM window is managed
  by Wise Prefetching and Continuous Batching.  Scoring is resident-only.
- **Demand:** the same CXL-SSD corpus and the same bounded CXL-DRAM window, but
  optional lookahead and continuous scheduling are disabled.  A missing
  mandatory entry is synchronously filled before the query resumes.

The Demand baseline is the causal control.  It must not receive a smaller
record, a warmer device cache, a different graph, or a looser recall setting.

### External reference

The strongest reproducible DiskANN/PipeANN configuration is shown as a gray
dashed reference on the main curves for every dataset for which its official
index can be built.  Its PQ-navigation mode, storage interface, host-memory
footprint, and thread count are stated adjacent to the result.  It is not
merged into the controlled Oracle--FlashANNS--Demand speedup calculation.

## Dataset and Layout Preflight

The final manifest records, for each dataset, its source path, number of base
vectors, native dimension, metric, graph degree, query count, ground-truth
depth, and SHA-256 hashes for base/query/ground-truth/index files.

- **T2I-10M:** current evidence identifies 10M 200-dimensional `float32`
  vectors with MIPS, 10k queries, and recall@10 ground truth.
- **LAION-10M:** use the first 10M vectors of the local LAION dump, but verify
  the binary header and metric before staging.  Existing notes indicate a
  512-dimensional representation; that value is not accepted without the
  preflight readback.
- **YFCC-10M:** the official Big ANN Benchmarks artifacts are downloaded at
  `/mnt/disk0/chukexin_motivation/data/yfcc10m`.  Header and length checks
  identify 10M 192-dimensional `uint8` base vectors, 100k public queries, and
  unfiltered top-100 ground truth.  The benchmark defines L2 distance.  The
  final 10k-query subset and its seed remain to be frozen before measurement.

The current Design describes T2I's 800 B vector and 2,048 B record.  Native
LAION or YFCC vectors may not fit that stride.  Before measuring them, retain
the complete-entry invariant but choose an aligned fixed stride per dataset
large enough for `vector + nnbrs + 32 neighbor IDs`.  The paper must generalize
the T2I-specific byte example in Design, Background, and Setup if any evaluated
dataset uses a different stride.  Silently truncating or projecting native
vectors is forbidden.

## Fixed Workload Protocol

### Search-quality sweep

Use the same predeclared search-list sweep for all core systems:

```text
L = 50, 100, 200, 400, 800, 1600
extension, only if the common recall range is not covered: 2400, 3200
k = 10
query order seed = 42
```

A 500-query untimed preflight determines whether the base sweep spans at least
recall@10 0.80--0.97 on each dataset.  It may activate the predeclared extension
but may not delete an unfavorable point.  Final points use a fixed 10k-query
set.  Recall is computed from returned IDs, not parsed from a separately tuned
run.

### Latency mode

Run one admitted query at a time with one search worker.  This isolates the
per-query dependency chain and honestly leaves little opportunity for D2 to
help.  For every query, record end-to-end latency; every final run reports:

```text
mean, p50, p95, p99, maximum, and a bootstrap 95% confidence interval
```

Mean latency and p99 latency are separate aligned subplots.  p50 and p95 remain
in the machine-readable artifact and the iso-recall table.

### Throughput mode

Fix the same CPU-core budget for every core system.  Determine the operating
concurrency from the predeclared set `1, 2, 4, 8, 16, 32` at the primary recall
point, using the Q5 concurrency sweep.  For each system, select the smallest
concurrency whose median throughput is within 95% of its observed maximum;
freeze and disclose that value before the full `L` sweep.  Run at least 60 s
or 10k completed queries per point, whichever is longer.  Report throughput
and the latency distribution from the same run, but do not substitute this
queueing latency for the latency-mode plots.

### Repetitions and state

- Run five independent measured repetitions per final point.
- Randomize system order within each `(dataset, L, repetition)` block.
- Reset the CXL-SSD cache and CXL-DRAM window before every true-cold run and
  validate the reset with device counters.
- Capture an immediate second pass for the warm sensitivity experiment before
  the next reset; this reuses the cold run rather than requiring later restaging.
- Pre-map and page-table-warm the Oracle outside the timed region, then verify
  zero NAND bytes during the timed region.
- Pin CPUs, fix frequency policy, record NUMA placement, and reject a run with
  a device reset, thermal throttle, cache-reset failure, query-count mismatch,
  or missing counter snapshot.

## Main Result Figures

### Figure 7: Recall versus latency

**Layout:** one double-column 2-by-3 grid.  Columns are LAION-10M, T2I-10M,
and YFCC-10M.  The top row is mean latency; the bottom row is p99 latency.

**Axes:** x-axis is measured recall@10; y-axis is end-to-end latency in
milliseconds on a log scale.  The visible x range is shared across datasets
when their achieved ranges overlap; no extrapolation connects missing points.

**Marks:** Oracle, FlashANNS, and Demand use consistent solid line/marker pairs.
DiskANN/PipeANN is gray and dashed.  Each point is the median of five runs;
light bands show 95% bootstrap confidence intervals.  A horizontal or vertical
arrow may mark the predeclared iso-recall anchor, but no speedup is annotated
at a different recall.

**Expected shape:** latency rises with recall for every system.  Oracle is the
lower envelope; Demand is the upper envelope once misses dominate; FlashANNS
lies between them.  FlashANNS need not equal Oracle in single-query mode because
the paper does not claim query hide.  Mean and p99 should tell the same
qualitative story; a mean-only win paired with a p99 regression is a failed
tail-latency result, not a successful main figure.

### Figure 8: Recall versus throughput

**Layout:** one double-column 1-by-3 grid, again ordered LAION, T2I, YFCC.

**Axes:** x-axis is measured recall@10; y-axis is sustained queries/s on a
linear scale starting at zero.  The caption states the frozen concurrency and
core budget for each system.

**Marks:** use the same styles and confidence representation as Figure 7.
Main-text speedups are computed only at matched recall by interpolation between
adjacent measured points; no curve is extrapolated.

**Expected shape:** throughput falls as recall rises.  FlashANNS remains above
Demand on all datasets and closes a material part of the Oracle gap.  The
external reference provides context, not the denominator for the two-mechanism
claim.

### Table 4: Iso-recall anchor and hide proof

For each dataset, include one row each for Demand, FlashANNS, and Oracle, plus
the external reference when available.  Columns are:

```text
recall@10 | mean ms | p50 ms | p95 ms | p99 ms | QPS
score from CXL-DRAM % | score from bounce % | critical wait ms/query
NAND MiB/query | useful-page % | window GiB | host GiB | concurrency
```

This table is the direct evidence for score hide.  Oracle must show zero NAND
bytes; FlashANNS must show 100% scores from CXL-DRAM and zero bounce scores;
Demand is allowed to wait on NAND.  The selected row is interpolated only for
performance presentation; all hide counters come from the nearest measured
point at or above the recall anchor.

The common cross-dataset anchor is recall@10 = 0.90.  T2I additionally reports
the paper's existing recall@10 >= 0.92 point so the Abstract and Introduction
remain auditable.

## Mechanism Evidence

### Figure 9: Wise Prefetcher ablation

Run on all three datasets at the nearest measured point at or above the common
anchor.  Use four policies with identical search semantics:

1. Demand synchronous mandatory fills.
2. Mandatory-only asynchronous fills, no optional lookahead.
3. Full Wise Prefetcher: mandatory first plus bounded frontier lookahead.
4. Blind wider lookahead using the same issued-byte budget as Wise.

**Panel (a):** grouped bars of normalized throughput, one group per dataset;
Demand is 1.0.  Overlaying latency on the same axis is forbidden.

**Panel (b):** x-axis is issued KiB/query while sweeping optional lookahead
`M = 0, 1, 2, 4, 8, 16`; y-axis is useful-page percentage.  Points violating
the recall anchor remain visible with a hollow marker.

**Panel (c):** x-axis is the same `M` sweep; y-axis is p99 latency normalized
to mandatory-only.  This keeps precision and latency on separate axes.

**Expected shape:** mandatory-only improves overlap but leaves next-hop gaps;
Wise improves throughput and/or p99 with higher useful-page precision than
Blind at the same byte budget.  Increasing `M` eventually flattens or hurts,
supporting a bounded rather than maximal policy.

The collector also records separate in-flight issue and resident-retention
budgets.  A small sensitivity table reports the chosen pair and the two nearest
smaller/larger settings so that the result is not attributed to one lucky
budget.

### Figure 10: Continuous Batching ablation

Use the primary T2I anchor and repeat the chosen operating points on LAION and
YFCC as a compact cross-dataset inset.

Compare:

1. Blocking per-query execution.
2. Static batches with a group barrier.
3. Continuous Batching with completion-driven admission.
4. Oracle, as the no-NAND ceiling.

**Panel (a):** x-axis is active-query concurrency `1, 2, 4, 8, 16, 32`; y-axis
is throughput.  One line per scheduling policy.

**Panel (b):** same x-axis; y-axis is p99 latency.  This prevents a throughput
win obtained only by unbounded queueing from passing.

**Panel (c), compact inset:** per dataset, show device useful-bandwidth
utilization and mean in-flight fill depth at the frozen throughput point.

**Expected shape:** Blocking leaves device depth low; Static improves until its
barrier drains; Continuous reaches a higher or earlier plateau while avoiding
an uncontrolled p99 rise.  At concurrency one, Continuous is not expected to
erase the Oracle gap.

## Robustness, Cost, and Negative Results

### Figure 11: Window pressure and cold/warm behavior

**Panel (a):** x-axis is CXL-DRAM window size as a percentage of the complete
corpus (`0.25%, 0.5%, 1%, 2%, 4%`); y-axis is normalized iso-recall throughput.
Use one line per dataset.  The absolute GiB value is included on a secondary
top label, not a second y-axis.

**Panel (b):** two vertically aligned mini-panels compare the true-cold first
pass with the immediate warm second pass.  The upper grouped bars report
throughput; the lower grouped bars report critical wait ms/query.  If only the
warm pass hides NAND, the paper calls the result service hide and removes any
true-cold claim.

### Table 5: Resource cost and scope

Report host DRAM, CXL-DRAM window, full-corpus bytes, CPU utilization, cores,
device read GiB/s, useful GiB/s, energy/query if a reliable board or wall-power
counter exists, and index/staging time.  An unavailable trustworthy energy
counter is disclosed rather than replaced with a CPU-only estimate.

Also state negative or neutral cases: small windows, low concurrency, warm
working sets already resident, and any dataset on which Wise or Continuous
does not materially outperform its ablation.  These cases belong in the main
Evaluation or limitations paragraph, not only in an appendix.

## One-Pass Measurement Record

Every invocation writes one self-contained JSON record plus per-query latency
and returned-ID sidecars.  CSV is generated from JSON after validation; stdout
is not the primary database.

### Run identity

```text
run_id, UTC start/end, git SHA, dirty-tree hash, binary hash, hostname
kernel, firmware/bitstream hash, device BDF/serial, CPU list, NUMA policy
dataset/index/query/GT hashes, record stride, metric, graph degree
system, policy, cold/warm, repetition, seed, L, k, hop budget
window bytes, issue bytes, retention bytes, lookahead M
threads, client concurrency, query count
```

### Outcomes collected in the same run

```text
recall@1, recall@10, returned-ID checksum
wall time, QPS, per-query mean/p50/p95/p99/max latency
score_from_cxl_dram, score_from_window, score_from_bounce
critical_wait_ns, device_fill_ns, compute_ns, scheduler_ns
mandatory/optional pages issued, pages deduplicated, pages useful
lookahead useful pages, fetched pages, evictions, late completions
NAND commands, sectors, bytes, useful bytes, read-amplification
fill-latency histogram, in-flight-depth histogram, device busy time
CPU utilization/cycles/instructions, peak host bytes, peak window bytes
reset-before and reset-after cache/window/device counter snapshots
```

The current runtime already emits mean, p50, p90, p99, QPS, recall, score
source, critical wait, prefetch-use, and NAND-byte fields.  The execution plan
must add p95, per-query latency output, mandatory versus optional issue counts,
dedup counts, queue-depth histograms, scheduler time, and a structured run
manifest before the final matrix begins.

## Run Matrix and Reuse Strategy

The matrix is generated from one declarative manifest rather than separate
handwritten scripts.

1. **Preflight:** validate all files/hashes/layouts and run 500 queries over the
   full predeclared `L` set.  No paper number comes from this phase.
2. **Q5 concurrency sweep:** at the anchor `L`, collect all four scheduling
   policies and all six concurrency values.  These records directly produce
   Figure 10 and freeze the throughput concurrency.
3. **Main latency sweep:** three datasets x core systems x `L` x five
   repetitions in latency mode.  The same record produces mean, p50, p95, p99,
   recall, hide counters, and the latency half of Table 4.
4. **Main throughput sweep:** use the frozen concurrency for the same datasets,
   systems, `L`, and repetitions.  The same record produces Figure 8, resource
   counters, and the throughput half of Table 4.
5. **Wise sweep:** run the four policies and six `M` settings at the anchor.
   Reuse `M=0` as mandatory-only and the full-policy point from the main matrix
   when every manifest field matches.
6. **Window sweep:** run the five window fractions.  Capture cold and immediate
   warm passes together; reuse the configured-default point from the main
   matrix when hashes and state match.
7. **External reference:** run under its native official harness with the same
   queries, ground truth, CPU budget, cold declaration, and `L` coverage.  Never
   copy its counters into core-system fields it cannot measure.

The validator rejects duplicate logical points with conflicting manifests and
permits reuse only when every identity and configuration field matches.  This
prevents rerunning the same point merely because a later plot needs another
metric.

## Statistical and Plotting Rules

- Plot medians across five repetitions and 95% bootstrap confidence intervals.
- Preserve all individual repetitions in released CSV/JSON.
- Use measured recall on the x-axis; never label a point with its target recall.
- Use log scale only for latency, and state it visibly.
- Start throughput and normalized-bar axes at zero.
- Do not use dual y-axes.  Figure 9's policy/validity markers and Figure 11's
  cold/warm encoding must include explicit legends and remain readable in
  grayscale.
- Keep system colors and markers identical across all figures.
- Show missing or rejected data as missing; do not connect across it.
- Perform matched comparisons within the same randomized repetition block.
- Report both effect size and confidence interval; do not rely only on a
  significance test.

## Fail-Closed Validation

A run is invalid if any of the following holds:

- query, index, ground-truth, binary, or bitstream hash is missing;
- achieved recall or returned-ID checksum cannot be recomputed;
- FlashANNS reports a bounce score or a non-CXL-DRAM scoring source;
- Oracle reports nonzero NAND traffic in the timed interval;
- mandatory work was dropped, search parameters differ, or returned IDs differ
  between core systems at the same query and `L` beyond documented distance ties;
- cache/window reset is unproven for a run labeled true-cold;
- NAND byte deltas are negative, wrap, or include another process;
- fewer than 10k valid query latencies exist for a final p99 point;
- a thermal, device, or scheduler anomaly changes the fixed resource budget.

Invalid runs are rerun under the same manifest; they are never averaged into a
paper point.

## Paper-Readiness Exit Criteria

The Evaluation is ready for a strong ASPLOS review only when all of these hold:

1. Figures 7 and 8 contain at least five measured points across a common useful
   recall range for all three core systems and all three datasets.
2. FlashANNS improves both mean and p99 latency over Demand at matched recall on
   every dataset, with positive confidence bounds; it has no hidden tail
   regression.
3. FlashANNS improves saturated throughput over Demand on every dataset and
   closes at least half of the Demand-to-Oracle throughput gap on the median
   dataset.  The exact gap-closure fraction is reported, not implied.
4. Every FlashANNS main point has 100% CXL-DRAM scoring, zero bounce scoring,
   unchanged search semantics, and auditable critical-wait/NAND counters.
5. Wise beats the equal-byte Blind policy in useful-page precision and improves
   throughput or p99 over mandatory-only on at least two datasets, without a
   greater-than-5% regression on the third.
6. Continuous beats Blocking and Static at their best observed concurrency on
   throughput, while its p99 is no more than 20% above Static at the same
   active-query concurrency selected for Continuous.
7. Window and cold/warm experiments show where the claim holds.  If true-cold
   hide fails, Abstract and Introduction are weakened before submission.
8. The strongest reproducible external baseline, resource costs, native record
   dimensions, and all fairness differences are visible in the paper.
9. All plotted points trace to validated JSON records and can be regenerated by
   one plotting command without manual numeric transcription.
10. Captions state dataset, recall definition, cold/warm state, concurrency,
    repetition count, and confidence interval.  No placeholder, unsupported
    speedup, or stale T2I-only layout claim remains.

These criteria cannot guarantee a score, but failing one identifies the exact
review objection that would keep the evidence below the intended 4.5/5 bar.
