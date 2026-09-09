# Motivation Static versus Dynamic Batching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a measured T2I-10M Figure 5 comparing Single, Static, and Dynamic scheduling by physical CXL-SSD bandwidth utilization and QPS.

**Architecture:** Add one admission-only static cohort gate to the frozen scheduler, then extend the isolated Motivation evidence pipeline with a three-condition C3 batching phase. Aggregate five accepted cold repetitions and render the protected Figure 5 asset from CSV only.

**Tech Stack:** C++17, Python 3, unittest, VMEM sysfs/ioctls, Linux block counters, NumPy/Matplotlib, LaTeX.

**Spec:** `docs/superpowers/specs/2026-09-09-motivation-static-dynamic-batching-design.md`

## Global Constraints

- T2I-10M only; `L=400`, `k=10`, PQ-64, identical artifacts and query order.
- Static and Dynamic use T=8 and differ only in cohort admission.
- Exactly five true-cold repetitions per condition.
- Paper bandwidth uses physical dual-backing counters and the accepted empirical device peak.
- Preserve Figures 3 and 4 and the Motivation section structure.

---

### Task 1: Add the admission-only static cohort gate

**Files:**
- Modify: `serving/cont_batch.hpp`
- Modify: `serving/search_beam.cpp`
- Modify: `serving/tests/test_cont_batch.cpp`

**Interfaces:**
- Produces: `StaticCohortGate(total_queries, cohort_size)`, `can_admit(qi)`, and `complete(qi)`.
- Consumes: the existing shared `next_q` admission path in the frozen steal scheduler.

- [ ] Write a unit test proving query 8 is blocked until queries 0--7 all complete, including out-of-order retirement.
- [ ] Run `make -C serving/tests test_cont_batch` and observe the missing-gate failure.
- [ ] Implement the minimal thread-safe gate and `--static-cohort 8`; reject incompatible T=1/non-steal configurations.
- [ ] Rerun the focused serving test and the full serving test suite.

### Task 2: Add fail-closed C3 batching evidence

**Files:**
- Modify: `experiments/motivation/flashanns/contract.json`
- Modify: `experiments/motivation/flashanns/records.py`
- Create: `experiments/motivation/flashanns/run_batching.py`
- Create: `experiments/motivation/flashanns/tests/test_batching.py`

**Interfaces:**
- Produces: one immutable run record for each `(condition, repeat)` and a validator for the complete 3-by-5 block.
- Consumes: `BlockQdSampler`, VMEM cold evidence, block snapshots, and frozen T2I commands.

- [ ] Write tests for the three exact conditions, five repeats, physical-bandwidth calculation, empirical-peak utilization, recall matching, and rejection of writes/incomplete queries.
- [ ] Run the focused test and observe failures for the absent runner/contract.
- [ ] Implement command construction, immutable records, and fail-closed validation.
- [ ] Rerun focused Motivation tests.

### Task 3: Smoke and collect the 15-run campaign

**Files:**
- Produce: `results/motivation/flashanns-10m/raw/c3_batching/`
- Produce: `results/motivation/flashanns-10m/accepted/c3_batching/`

**Interfaces:**
- Consumes: the rebuilt `serving/search_beam`, staged T2I extent image, and C3 empirical peak.
- Produces: five accepted cold records for each of Single, Static, and Dynamic.

- [ ] Perform read-only device, process, image, cache, and backing-identity preflight.
- [ ] Run a 100-query cold smoke for all three conditions and verify scheduler markers and matched recall.
- [ ] Run five randomized-order cold repetitions per condition, resetting only the declared VMEM cache state.
- [ ] Validate exact 3-by-5 coverage and preserve rejected evidence separately.

### Task 4: Aggregate, redraw Figure 5, and minimally revise C3

**Files:**
- Modify: `experiments/motivation/flashanns/aggregate.py`
- Modify: `experiments/motivation/flashanns/plot.py`
- Modify: `experiments/motivation/flashanns/tests/test_aggregate_plot.py`
- Replace: `paper/figs/mot-c3-io-concurrency.pdf`
- Modify: `paper/sections/motivation.tex`

**Interfaces:**
- Consumes: accepted-only C3 batching records.
- Produces: aggregate CSV rows, provenance, the protected vector PDF, and evidence-matched prose.

- [ ] Write failing tests for accepted-only medians/CIs, two zero-based panels, category order, dimensions, and no hard-coded measurements.
- [ ] Implement aggregation and rendering from CSV only.
- [ ] Render and inspect PDF geometry, fonts, text, vectors, and a raster preview.
- [ ] Replace only the C3 claims/caption/table row and build the paper with `latexmk -pdf -g -interaction=nonstopmode main.tex`.
- [ ] Run all Motivation/Evaluation Python tests and serving tests; emit a readiness record resolving every plotted mark to five run IDs.
