# Extent Locality Diagnostic Sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine whether the small extent benefit is bounded by T2I-10M page locality or lost in the extent issue path.

**Architecture:** Run a matched T=1 diagnostic matrix at `L={400,800,1600}` with extent formation disabled and enabled. For each point, combine device-counter deltas with a trace-derived upper bound based on contiguous physical pages; classify the result from the gap between theoretical and realized command reduction.

**Tech Stack:** Existing FlashANNS cold-reset/preflight helpers, `search_beam`, Python run records, NumPy trace analysis, unittest.

---

### Task 1: Freeze the diagnostic contract

**Files:**
- Create: `experiments/eval/flashanns/extent_diagnostic.py`
- Test: `experiments/eval/flashanns/tests/test_extent_diagnostic.py`

- [x] Define the fixed matrix: T2I-10M, extent layout, `T=1`, `L={400,800,1600}`, systems `batch-t1` and `extent-t1`, three cold repeats, 1,000 queries.
- [x] Require one binary SHA-256, full host/device identity equality, zero dirty bytes before reset, and identical query/candidate hashes within every pair.
- [x] Reject missing repeats, incomplete runs, nonzero `score_flash`, or nonzero `score_bounce`.

### Task 2: Add trace-locality analysis

**Files:**
- Modify: `experiments/eval/flashanns/extent_diagnostic.py`
- Test: `experiments/eval/flashanns/tests/test_extent_diagnostic.py`

- [x] Map committed logical IDs through `id_to_slot`, then convert two 2-KiB slots into each 4-KiB physical page.
- [x] For each query, compute unique pages, contiguous runs, pages in runs longer than one, maximum run length, and the ideal command reduction if each run becomes one command.
- [x] Aggregate medians and retain per-run values rather than averaging pre-aggregated marks.

### Task 3: Add a resumable cold-run driver

**Files:**
- Create: `tools/run_eval_extent_diagnostic.sh`

- [x] Bind the campaign tag to the current `search_beam` SHA-256.
- [x] Before every run, call `eval_reset_and_restore` for the existing T2I extent image and require accepted volatile evidence.
- [x] Execute only missing run IDs and preserve all raw logs/traces; never overwrite an existing run.
- [x] Write the final CSV, provenance JSON, and diagnosis JSON under `results/eval/flashanns/provisional/<tag>/extent-diagnostic/`.

### Task 4: Execute and classify

**Files:**
- Produce: `results/eval/flashanns/raw/t2i10m/<tag>/extent_diagnostic/`
- Produce: `results/eval/flashanns/provisional/<tag>/extent-diagnostic/validated.csv`
- Produce: `results/eval/flashanns/provisional/<tag>/extent-diagnostic/diagnosis.json`

- [x] Run all 18 points only after `/dev/vmem0` identity and cold-reset checks pass.
- [x] Compare actual `nand_read_commands/query` reduction with the trace-derived ideal reduction at every L.
- [x] Classify locality-limited when the theoretical ceiling remains small and realized reduction captures most of it; classify implementation-limited when the ceiling grows but realized reduction does not.
- [x] Report physical bytes, KiB/read, added extent pages, QPS, and recall as guardrails, not as selection criteria.

### Task 5: Verify the evidence bundle

**Files:**
- Test: `experiments/eval/flashanns/tests/test_extent_diagnostic.py`

- [x] Run the focused unittest and `git diff --check`.
- [x] Verify 18 unique run IDs, three repeats per mark, matched hashes, and exact binary identity.
- [x] Confirm the final diagnosis is reproducible from raw `run.json` and trace sidecars.

The worktree is intentionally dirty, so this diagnostic plan does not commit, clean, or modify unrelated paper artifacts.
