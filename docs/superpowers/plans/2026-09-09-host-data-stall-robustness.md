# Host Data-Dependency Stall Robustness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-instrument and retest Figure 10(a) using actual host data-dependency stall while preserving matched Figure 10(b-d) evidence.

**Architecture:** Add explicit, non-overlapping counters at blocking and continuous-scheduler stall boundaries; carry them through parsing, validation, aggregation, and plotting. Seal a new 30-run q4-only campaign under a new binary tag without modifying old accepted records.

**Tech Stack:** C++17, Python 3 unittest, JSON evidence records, Matplotlib, Bash campaign wrappers.

**Spec:** `docs/superpowers/specs/2026-09-09-host-data-stall-robustness-design.md`

## Global Constraints

- T=8, query seed 42, cold reset, frozen per-dataset recall anchors.
- Demand and FlashANNS use the same dataset image, query order, and L anchor policy.
- Five repetitions per system and dataset; accepted-only aggregation.
- Old accepted records and the current PDF are not overwritten before all gates pass.
- No `device_fill_ns` substitution and no inferred/default-zero score contract.

---

### Task 1: Runtime metric contract

**Files:**
- Modify: `serving/metrics.hpp`
- Modify: `serving/hide_fill.hpp`
- Modify: `serving/cont_batch.hpp`
- Modify: `serving/search_beam.cpp`
- Test: `serving/tests/test_metrics.cpp`
- Test: `serving/tests/test_extent_run.cpp`
- Test: `serving/tests/test_cont_batch.cpp`

**Interfaces:**
- Produces: `coverage_wait_ns`, `slot_backpressure_wait_ns`, `host_data_stall_ns()`, `score_triggered_flash_fills`, and `score_prematerialized_pct()`.
- Preserves: `crit_wait_ns` and `device_fill_ns` as compatibility diagnostics.

- [ ] Add failing tests for exact sum, merge/reset behavior, blocking split accounting, and scheduler stall classification.
- [ ] Run the focused C++ tests and confirm failure because the new contract is absent.
- [ ] Implement the minimum counters and instrumentation described by the spec.
- [ ] Run the focused tests, then the complete `serving/tests` suite.

### Task 2: Evidence pipeline

**Files:**
- Modify: `experiments/eval/flashanns/run_one.py`
- Modify: `experiments/eval/flashanns/validate_run.py`
- Modify: `experiments/eval/flashanns/aggregate.py`
- Test: `experiments/eval/flashanns/tests/test_validate_run.py`
- Test: `experiments/eval/flashanns/tests/test_aggregate.py`

**Interfaces:**
- Consumes: explicit runtime fields from Task 1.
- Produces: accepted records and median columns for both stall components and their sum.

- [ ] Add failing tests that reject missing/inconsistent stall fields and missing score-contract counters.
- [ ] Confirm focused tests fail for the intended missing validation/aggregation behavior.
- [ ] Parse, validate, and aggregate the explicit fields without fallback to `crit_wait_ns`.
- [ ] Run the focused and full evaluation Python tests.

### Task 3: Paired smoke gate

**Files:**
- Create: `tools/run_eval_host_stall_q4.sh`
- Create under results: a new binary-tagged smoke evidence directory.

**Interfaces:**
- Consumes: frozen anchors and full dataset-identity evidence.
- Produces: one cold Demand/FlashANNS T2I pair suitable only for semantic validation.

- [ ] Build and hash a dedicated search binary.
- [ ] Verify `/dev/vmem0`, backing-device identity, staged T2I identity, and absence of conflicting experiment processes.
- [ ] Run one cold 100-query pair with identical candidate traces.
- [ ] Gate completion, recall/candidate equality, stall sum, score contract, and nonnegative NAND deltas.

### Task 4: Three-dataset q4 campaign

**Files:**
- Modify: `tools/run_eval_host_stall_q4.sh`
- Create under results: new raw, accepted, preflight, and readiness evidence keyed by binary tag.

**Interfaces:**
- Produces: 3 datasets x 2 systems x 5 repetitions = 30 accepted q4 records.

- [ ] Stage and fully verify one dataset at a time.
- [ ] Run five interleaved cold Demand/FlashANNS pairs at its frozen anchors.
- [ ] Validate and seal each record immediately; stop on the first identity, metric, recall, or score-contract failure.
- [ ] Write a manifest containing every accepted run ID, binary hash, dataset hash, and gate result.

### Task 5: Figure and manuscript integration

**Files:**
- Modify: `experiments/eval/flashanns/plot_six_figures.py`
- Modify: `experiments/eval/flashanns/tests/test_plots.py`
- Modify: `paper/sections/eval.tex`
- Replace after gates pass: `paper/figs/hide-robustness.pdf`

**Interfaces:**
- Consumes: accepted-only component medians from Task 4.
- Produces: stacked panel (a), matched QPS panel (b), unchanged panels (c-d), and a compact score-contract audit.

- [ ] Add a failing plot-validation test requiring both component medians and the score audit.
- [ ] Render panel (a) as paired stacked bars and preserve panel (b-d) layout.
- [ ] Update only the Figure 10 caption and its directly dependent paragraph.
- [ ] Regenerate the PDF, inspect it visually, build the paper, and verify page/label integrity.

