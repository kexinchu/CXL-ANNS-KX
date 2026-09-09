# FlashANNS Eight-Figure Evaluation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the six current Evaluation figures and add accepted-evidence load-stability and physical-I/O-efficiency figures.

**Architecture:** Keep the existing six-figure aggregate immutable. Add an explicit supplemental evidence contract, a strict curator that resolves only the named accepted runs, and a separate renderer for the two new PDFs. Copy only validated outputs into the manuscript and the user-facing evaluation workspace.

**Tech Stack:** Python 3, JSON/CSV, Matplotlib, unittest, LaTeX/latexmk, pdfinfo/pdftotext/pdftocairo.

**Spec:** `docs/superpowers/specs/2026-09-07-flashanns-evaluation-figures-design.md`

---

### Task 1: Freeze and test the supplemental evidence contract

**Files:**
- Create: `experiments/eval/flashanns/supplemental-contract.json`
- Create: `experiments/eval/flashanns/tests/test_supplemental_figures.py`
- Create: `experiments/eval/flashanns/supplemental_figures.py`

- [x] Encode the three dataset load campaign tags, five rates, three systems,
  and the five T2I I/O-stage selectors from the approved spec.
- [x] Write tests that require exactly repeats 0--4, accepted validation,
  10,000 completed queries, matching query/candidate identities, and required
  physical counters.
- [x] Run the focused tests and confirm they fail before implementation.
- [x] Implement strict selection and derived read-amplification metrics.
- [x] Rerun the focused tests and confirm they pass.

### Task 2: Curate the accepted supplemental dataset

**Files:**
- Produce: `results/eval/flashanns/provisional/supplemental-two-figures/validated.csv`
- Produce: `results/eval/flashanns/provisional/supplemental-two-figures/provenance.json`
- Produce: `results/eval/flashanns/provisional/supplemental-two-figures/quality-report.json`

- [x] Resolve records only below `results/eval/flashanns/accepted/`.
- [x] Fail on duplicate run IDs, unexpected campaign/rate/system, incomplete
  repeats, identity drift, or absent metrics.
- [x] Aggregate medians and bootstrap confidence intervals.
- [x] Verify 45 load marks/225 unique runs and five I/O marks/25 unique runs.

### Task 3: Render and inspect the two figures

**Files:**
- Produce: `results/eval/flashanns/provisional/supplemental-two-figures/figures/load-tail-latency.pdf`
- Produce: `results/eval/flashanns/provisional/supplemental-two-figures/figures/prefetch-io-efficiency.pdf`
- Produce: matching PNG previews.

- [x] Render the three-dataset P99 service curve with achieved throughput on
  the x-axis and preserve offered rate in point annotations/CSV.
- [x] Render the T2I causal-stage I/O panels using physical NAND counters and
  computed useful-payload read amplification.
- [x] Check one page, vector fonts, nonempty text, clipping, and legend/title
  separation with `pdfinfo`, `pdftotext`, and PNG previews.

### Task 4: Update the six retained figures and manuscript

**Files:**
- Modify: `experiments/eval/flashanns/plot_six_figures.py`
- Modify: `experiments/eval/flashanns/tests/test_plots.py`
- Modify: `paper/sections/eval.tex`
- Copy: eight PDFs to `paper/figs/`

- [x] Replace the existing Wise panel's invalid extent-utilization bar with
  supported command/QPS evidence and remove the matching prose claim.
- [x] Insert the physical-I/O figure after the Wise ablation and the load curve
  after the concurrency sweep, without changing Q1--Q5 structure.
- [x] Use measured values only after deriving them from the curated CSV.
- [x] Run the complete FlashANNS unit-test suite.
- [x] Build `paper/main.pdf` and inspect the Evaluation pages.

### Task 5: Refresh the user-facing evaluation workspace

**Files:**
- Modify: `paper/evaluation_workspace/README.md`
- Modify: `paper/evaluation_workspace/regenerate.sh`
- Copy: supplemental CSV, provenance, quality report, renderer, PDFs, and PNGs.

- [x] Keep data, scripts, and figures in separate named directories.
- [x] Make one command regenerate all eight working figures from the copied
  aggregate data without accessing raw devices.
- [x] Document campaign selection, metric semantics, and the retired invalid
  extent counter.
- [x] Run the workspace regeneration command and compare output filenames and
  PDF page counts with the canonical artifacts.
