# FlashANNS Evaluation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce an ASPLOS-grade, reproducible Evaluation covering LAION-10M,
T2I-10M, and YFCC-10M, with end-to-end quality/performance curves, direct
score-hide evidence, two mechanism ablations, robustness tests, and resource
costs.

**Architecture:** One declarative experiment manifest drives all runs and emits
self-contained JSON plus per-query sidecars.  Validated JSON is the only input
to plotting and LaTeX-table generation.  Internal comparisons keep graph search
and resources fixed across FlashANNS and Demand; the official external PipeANN
harness remains visibly separate. Oracle is excluded from execution, accepted
records, aggregation, and plots.

**Tech Stack:** C++ runtime, Bash orchestration, JSONL/CSV, Python plotting,
TikZ/LaTeX placeholders, acmart, `latexmk`, sysfs/NVMe counters, `perf`, SHA-256.

**Spec:** `docs/superpowers/specs/2026-09-04-flashanns-prefetcher-evaluation-design.md`

## Global Constraints

- Datasets are LAION-10M, T2I-10M, and YFCC-10M using native dimensions,
  declared source/execution datatypes, and exact recall@10 ground truth.
- Never silently truncate or project vectors to fit T2I's 2,048 B record.
- FlashANNS and Demand use the same graph, query order, distance code,
  `L`, hop budget, CPU allocation, and complete-entry format per dataset.
- Main latency mode admits one query at a time; main throughput mode uses a
  frozen concurrency selected by the predeclared concurrency sweep.
- Collect mean, p50, p95, p99, and maximum latency in every measured run.
- Main results use 10k queries, five repetitions, randomized system order, and
  measured recall on the x-axis.
- A FlashANNS paper point is invalid unless scoring is 100% from CXL-DRAM and
  zero from a bounce path.
- Reject Oracle or `--oracle-dram` in any executable configuration or run
  record. The host-side `oracle_image` artifact is correctness and byte-identity
  evidence only.
- True-cold points require before/after cache and window reset evidence.
- Do not replace missing measurements with historical values or hand-entered
  numbers.
- Do not commit automatically in the current dirty worktree.

---

### Task 1: Lock Dataset Manifests and Per-Dataset Records

**Files:**
- Create: `experiments/eval/datasets.json`
- Create: `experiments/eval/verify_dataset.py`
- Create: `results/eval/manifests/datasets/`
- Read: native dataset headers, queries, ground truth, and graph files

**Interfaces:**
- Consumes: local LAION-10M, T2I-10M, and YFCC-10M artifacts.
- Produces: one immutable dataset manifest and validation report per corpus.

- [ ] Record absolute paths, file sizes, SHA-256 hashes, item count, native
  dimension, distance metric, query count, ground-truth depth, graph degree,
  and graph/index hash.
- [ ] Validate exactly 10,000,000 base vectors and at least 10,000 queries with
  recall@10 ground truth; reject dimensional or metric mismatches.
- [ ] Record both `source_dtype` and `execution_dtype`.  If YFCC `uint8` is
  widened to `float32`, prove value preservation and sampled exact-L2 ranking
  equivalence, and disclose the expanded footprint; never silently cast,
  truncate, or project.
- [ ] Compute `record_stride = max(2048, next_pow2(vector_bytes + 4 + 32*4))`
  from the declared execution datatype, then validate it against the runtime
  layout rather than assuming four bytes per source element.
- [ ] Read back 1,024 deterministic records after packing and verify vector,
  neighbor-count, and neighbor-ID equality against source files.
- [ ] If LAION/YFCC uses a non-2,048 B stride, minimally generalize the
  T2I-specific byte example in Background, Design, and Setup before results are
  inserted; retain the complete-entry residency invariant.

Verification:

```bash
python3 experiments/eval/verify_dataset.py \
  --config experiments/eval/datasets.json \
  --out results/eval/manifests/datasets
```

Expected: three `valid: true` reports with hashes and no inferred field.

### Task 2: Add Structured Measurement Output

**Files:**
- Modify: `serving/metrics.hpp`
- Modify: `serving/search_beam.cpp`
- Create: `serving/tests/test_eval_metrics.cpp`
- Create: `experiments/eval/schema/run.schema.json`

**Interfaces:**
- Consumes: per-query start/end events, residency/fill events, scheduler events,
  and before/after device counters.
- Produces: `run.json`, `latency_ns.u64`, and `result_ids.u32` for each invocation.

- [ ] Add exact p95 calculation while retaining mean, p50, p90, and p99 output.
- [ ] Emit every per-query latency and returned ID so recall and percentiles can
  be recomputed independently.
- [ ] Split page counters into mandatory issued, optional issued, deduplicated,
  useful mandatory, useful optional, evicted, and late completion.
- [ ] Add in-flight-depth and fill-latency histograms plus compute,
  critical-wait, and scheduler nanoseconds.
- [ ] Emit run identity fields required by the spec, including all artifact and
  binary hashes, cold/warm state, record stride, budgets, concurrency, and CPU
  binding.
- [ ] Validate output against `run.schema.json`; fail the process if a required
  field or sidecar is missing.

Verification:

```bash
make -C serving/tests test_eval_metrics
serving/tests/test_eval_metrics
```

Expected: exact synthetic mean/p50/p95/p99, correct mandatory/optional counts,
and schema rejection for a record missing its cold-reset evidence.

### Task 3: Implement One Manifest-Driven Runner

**Files:**
- Create: `experiments/eval/matrix.json`
- Create: `experiments/eval/run_matrix.py`
- Create: `experiments/eval/capture_counters.py`
- Create: `experiments/eval/validate_run.py`

**Interfaces:**
- Consumes: dataset manifests, system/policy/L/concurrency matrices, and runtime
  commands.
- Produces: uniquely addressed run directories under `results/eval/raw/`.

- [ ] Encode the fixed `L={50,100,200,400,800,1600}` sweep and conditional
  extension `{2400,3200}`, `k=10`, seed 42, 10k final queries, and five repeats.
- [ ] Encode FlashANNS, Demand, and the official external PipeANN harness as
  separate adapters with a common identity envelope; fail closed if Oracle is
  configured.
- [ ] Randomize system order within each dataset/L/repetition block using a
  recorded matrix seed.
- [ ] Before every cold run, reset and validate CXL-SSD cache and CXL-DRAM
  window state; capture the immediate warm pass before the next reset.
- [ ] Snapshot NVMe sectors/commands, FPGA counters, CPU/NUMA state, temperature,
  and throttling before and after each timed interval.
- [ ] Reject rather than aggregate any run that violates schema, hash,
  query-count, recall-recompute, reset, score-source, executable-system, or
  anomaly gates.
- [ ] Reuse a run for multiple plots only when every manifest identity field is
  identical.

Verification:

```bash
python3 experiments/eval/run_matrix.py --matrix experiments/eval/matrix.json \
  --phase smoke --dry-run
python3 experiments/eval/validate_run.py results/eval/raw/smoke/*/run.json
```

Expected: complete deterministic command expansion and validator success on a
valid smoke record, followed by deliberate rejection when one hash is removed.

### Task 4: Freeze Recall Coverage and Throughput Concurrency

**Files:**
- Produce: `results/eval/calibration/recall_coverage.json`
- Produce: `results/eval/calibration/concurrency.json`

**Interfaces:**
- Consumes: 500-query preflight and anchor-recall concurrency runs.
- Produces: final `L` coverage and one disclosed throughput concurrency per
  system/dataset.

- [ ] Run all predeclared `L` values on 500 queries and verify each dataset has
  at least five points spanning a common useful recall interval; activate only
  the predeclared extension if 0.80--0.97 is not covered.
- [ ] At the nearest point at or above recall@10 0.90, run active-query
  concurrency `{1,2,4,8,16,32}` for every executable Q2 system.
- [ ] Select the smallest concurrency within 95% of median maximum throughput;
  write it to `concurrency.json` before the main sweep.
- [ ] Preserve every calibration point because the same runs feed Figure 10.

Verification: calibration JSON contains a frozen value and justification for
all dataset/system pairs; no final plotter is allowed to choose concurrency.

### Task 5: Collect Main Recall--Latency and Recall--Throughput Curves

**Files:**
- Produce: `results/eval/raw/main-latency/`
- Produce: `results/eval/raw/main-throughput/`

**Interfaces:**
- Consumes: Tasks 1--4 manifests and frozen concurrency.
- Produces: all inputs for Figures 7--8 and Table 4.

- [ ] Run latency mode with one admitted query, one worker, 10k queries, every
  dataset/system/L, and five randomized repetitions.
- [ ] Run throughput mode at frozen concurrency for at least 60 seconds or 10k
  completions, whichever is longer, using the same matrix.
- [ ] Recompute recall and mean/p50/p95/p99/max from sidecars and verify stdout
  summaries agree within rounding tolerance.
- [ ] At recall@10 0.90, and additionally T2I recall@10 0.92, compute matched
  interpolation without extrapolation.
- [ ] Compute and report matched FlashANNS speedup over Demand and the separate
  comparison with official PipeANN, without an Oracle-derived gap-closure
  metric.

Verification: each plotted core curve has at least five valid points and every
point has five repetitions with confidence bounds.

### Task 6: Collect Wise Prefetcher Ablations

**Files:**
- Produce: `results/eval/raw/wise/`

**Interfaces:**
- Consumes: the nearest measured anchor point and D1 policy switches.
- Produces: Figure 9 and its budget-sensitivity summary.

- [ ] Run Demand, Mandatory-only, Wise, and equal-issued-byte Blind policies on
  all datasets without changing search parameters.
- [ ] Sweep optional `M={0,1,2,4,8,16}` and record issued KiB/query,
  useful-page percentage, p99, QPS, NAND bytes, and recall.
- [ ] Sweep the chosen issue/retention budget pair at one smaller and one larger
  value independently.
- [ ] Verify mandatory pages are never dropped and returned IDs match the core
  search at equal `L`.

Verification: Wise's equal-byte comparison is computable on all datasets and
every point that misses the recall anchor is retained and marked invalid.

### Task 7: Collect Continuous Batching Ablations

**Files:**
- Produce: `results/eval/raw/continuous/`

**Interfaces:**
- Consumes: Task 4's concurrency records and Blocking/Static/Continuous modes.
- Produces: Figure 10 without rerunning the calibration matrix.

- [ ] Validate that the saved concurrency sweep contains QPS, p99, mean
  in-flight depth, useful device GiB/s, critical wait, and recall.
- [ ] Add only missing cross-dataset anchor records; do not rerun matching T2I
  calibration points.
- [ ] Compare policies at identical active-query concurrency and CPU budget.
- [ ] Verify Continuous changes scheduling only by comparing returned-ID
  sidecars and search counters.

Verification: Continuous has a throughput curve and p99 curve for all six
concurrency values, plus a device-utilization point for every dataset.

### Task 8: Collect Window, Cold/Warm, and Resource Evidence

**Files:**
- Produce: `results/eval/raw/robustness/`
- Produce: `results/eval/raw/resources/`

**Interfaces:**
- Consumes: default main runs and paired cold/warm records.
- Produces: Figure 11 and Table 5.

- [ ] Sweep CXL-DRAM window fractions `{0.25,0.5,1,2,4}%` at iso-recall on all
  datasets and record absolute GiB values.
- [ ] Reuse the immediate warm pass paired with each default cold main run.
- [ ] Record host/CXL-DRAM bytes, CPU utilization, cores, device and useful
  GiB/s, peak window bytes, index/staging time, and trustworthy energy/query if
  an end-to-end power counter exists.
- [ ] Record negative cases rather than filtering small-window, low-concurrency,
  or warm-neutral results.

Verification: every Figure 11 bar/point links to a cold reset snapshot and Table
5 contains no inferred energy value.

### Task 9: Validate, Aggregate, and Plot

**Files:**
- Create: `experiments/eval/aggregate.py`
- Create: `experiments/eval/plot_evaluation.py`
- Create: `experiments/eval/render_tables.py`
- Create: `paper/figs/eval-latency.pdf`
- Create: `paper/figs/eval-throughput.pdf`
- Create: `paper/figs/eval-wise.pdf`
- Create: `paper/figs/eval-continuous.pdf`
- Create: `paper/figs/eval-robustness.pdf`
- Create: `results/eval/validated.csv`

**Interfaces:**
- Consumes: validated run JSON and sidecars only.
- Produces: camera-ready figures, generated LaTeX table bodies, and provenance.

- [ ] Aggregate matched five-run blocks using medians and bootstrap 95%
  confidence intervals while preserving individual runs.
- [ ] Render Figure 7 as mean/p99 rows by three dataset columns with log-latency
  axes and Figure 8 as three recall--QPS panels starting at zero.
- [ ] Render Figure 9 as normalized-QPS bars, useful-page precision sweep, and
  normalized-p99 sweep.
- [ ] Render Figure 10 as concurrency--QPS, concurrency--p99, and compact
  utilization inset; render Figure 11 as window sweep and aligned cold/warm
  throughput/wait mini-panels.
- [ ] Use identical system colors/markers, measured recall x values, visible
  confidence intervals, grayscale-safe encodings, and no dual y-axis.
- [ ] Generate Table 4/5 bodies directly from `validated.csv` and emit a
  provenance map from every PDF mark/table cell to run IDs.

Verification:

```bash
python3 experiments/eval/aggregate.py --raw results/eval/raw \
  --out results/eval/validated.csv
python3 experiments/eval/plot_evaluation.py --csv results/eval/validated.csv \
  --out paper/figs
python3 experiments/eval/render_tables.py --csv results/eval/validated.csv \
  --out paper/generated
```

Expected: five PDFs, two generated table bodies, and no manual numeric input.

### Task 10: Replace Evaluation Placeholders and Audit the Paper

**Files:**
- Modify: `paper/sections/eval.tex`
- Modify mechanically: `paper/sections/appendix.tex`
- Verify: Abstract, Introduction, Background, Design, Discussion, Conclusion

**Interfaces:**
- Consumes: Task 9 figures/tables and validated claims.
- Produces: the final Evaluation and consistent paper-wide claims.

- [ ] Replace placeholders only with generated PDFs and table bodies.
- [ ] State setup, fairness differences, cold/warm protocol, repetitions,
  confidence intervals, and native record layouts before interpreting results.
- [ ] Lead with Figures 7--8 and Table 4, then explain hide proof, D1, D2,
  robustness, cost, and negative cases in claim order.
- [ ] Change Abstract/Introduction numbers only from validated Table 4; weaken
  true-cold or cross-dataset claims if the corresponding exit gate fails.
- [ ] Update Appendix figure rows and artifact commands to exact generated names.
- [ ] Run the ten paper-readiness exit checks from the spec and record pass/fail
  evidence in `results/eval/paper_readiness.md`.
- [ ] Force-build and visually inspect every Evaluation page.

Verification:

```bash
git diff --check -- paper/sections/eval.tex paper/sections/appendix.tex \
  experiments/eval results/eval/paper_readiness.md
latexmk -g -pdf -interaction=nonstopmode -halt-on-error paper/main.tex
pdfinfo paper/main.pdf | rg 'Pages|Page size'
rg -n 'undefined references|Reference .* undefined|Citation .* undefined' \
  paper/main.log
```

Expected: successful build, no undefined references, no placeholders, and a
paper-readiness report whose ten gates all pass or whose failed gate has caused
the corresponding paper claim to be weakened.
