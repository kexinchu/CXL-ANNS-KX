# FlashANNS Motivation Refresh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace all paper-facing Motivation measurements with current, five-repeat evidence while preserving the existing C1--C3 paper structure.

**Architecture:** Add an isolated `experiments.motivation.flashanns` evidence pipeline that freezes the paper contract, validates immutable per-run JSON, aggregates accepted runs, and renders the protected four figure assets. Figure 3 and both Figure 4 panels replace existing PDFs; Figure 5 keeps its location and C3 role but replaces the current TikZ schematic with a measured PDF. Reuse the frozen Evaluation artifact identities and VMEM ABI, but do not reuse old Motivation measurements or place numbers in plotting code.

**Tech Stack:** Python 3 standard library, C++17, `/dev/vmem0` VMEM ioctls, sysfs and block counters, `iostat`, NumPy/Matplotlib, unittest, LaTeX/latexmk.

**Spec:** `docs/superpowers/specs/2026-09-08-flashanns-motivation-refresh-design.md`

---

### Task 1: Freeze the paper and experiment contract

**Files:**
- Create: `experiments/motivation/__init__.py`
- Create: `experiments/motivation/flashanns/__init__.py`
- Create: `experiments/motivation/flashanns/contract.json`
- Create: `experiments/motivation/flashanns/tests/__init__.py`
- Create: `experiments/motivation/flashanns/tests/test_contract.py`

- [x] **Step 1: Write the contract test.** Require the five phases
  `c1_tier`, `c2_admission`, `c2_coverage`, `c3_capability`, and `c3_qd`;
  exactly five repeats;
  the three datasets for C1/C3; LAION-10M for C2; the declared tier, admission,
  coverage, and concurrency levels; 4 GiB cache; 4 KiB pages; and 2 MiB stripe.
- [x] **Step 2: Run**
  `python3 -m unittest experiments.motivation.flashanns.tests.test_contract -v`
  **Expected:** fail because the package and contract do not exist.
- [x] **Step 3: Add the minimal JSON contract and loader** with immutable
  tuples for phase dimensions and expected repetition IDs `0..4`.
- [x] **Step 4: Rerun the focused test.** **Expected:** pass.
- [x] **Step 5: Record SHA-256 hashes** of the current Motivation TeX, the three
  existing PDF assets, and `paper/sections/fig_mot_bubbles.tex` in
  `results/motivation/flashanns-10m/preflight/paper-before.json`. Record that
  Figure 5 will become `paper/figs/mot-c3-io-concurrency.pdf` without changing
  its paper position, number, or C3 role.

### Task 2: Implement fail-closed Motivation records

**Files:**
- Create: `experiments/motivation/flashanns/records.py`
- Create: `experiments/motivation/flashanns/tests/test_records.py`

- [x] **Step 1: Write failing unit tests** for missing binary/image/query
  hashes, wrong cache or stripe, incomplete operations, negative counter
  deltas, overlapping-device evidence, tier/counter disagreement, and a
  non-five-repeat aggregate block.
- [x] **Step 2: Run the focused tests.** **Expected:** fail because
  `validate_record`, `validate_block`, and `seal_record` are absent.
- [x] **Step 3: Implement** `validate_record(record)`,
  `validate_block(records)`, and atomic `seal_record(raw, accepted_root)`.
  `seal_record` must copy rather than mutate raw evidence and refuse overwrite.
- [x] **Step 4: Add C2 same-trace validation** for query IDs, candidate offsets,
  candidate IDs, result IDs, useful page set, and recall.
- [x] **Step 5: Rerun the focused and existing Evaluation validator tests.**
  **Expected:** all pass.

### Task 3: Add common observation helpers

**Files:**
- Create: `experiments/motivation/flashanns/observe.py`
- Create: `experiments/motivation/flashanns/tests/test_observe.py`

- [x] **Step 1: Write failing tests** using temporary sysfs/block-stat fixtures
  for counter snapshots, deltas, dual-backing identity, process-conflict
  detection, and `iostat` interval parsing that preserves idle samples.
- [x] **Step 2: Run the test.** **Expected:** fail on missing helpers.
- [x] **Step 3: Implement read-only helpers** for VMEM attributes, Linux block
  counters, executable/file SHA-256, open-user checks, and `iostat -x` parsing.
- [x] **Step 4: Rerun the tests.** **Expected:** pass without opening
  `/dev/vmem0`.

### Task 4: Build the C1 three-tier probe

**Files:**
- Create: `experiments/motivation/flashanns/tier_probe.cpp`
- Create: `experiments/motivation/flashanns/run_tier.py`
- Create: `experiments/motivation/flashanns/tests/test_tier.py`

- [x] **Step 1: Write failing tests** for deterministic distinct-page
  selection, percentile calculation, and classification gates:
  host=`faults=0,nand=0`; CXL-cache=`faults>0,nand=0`;
  Flash=`faults>0,nand>0`.
- [x] **Step 2: Run the focused tests.** **Expected:** fail because `run_tier`
  does not exist.
- [x] **Step 3: Implement the probe.** It accepts dataset image offset/length,
  vector stride, tier, seed, count, and JSON output. It prefaults an aligned
  host buffer; uses `madvise(MADV_DONTNEED)` for the CXL-cache condition; emits
  every access latency; and never writes the mapping.
- [x] **Step 4: Compile with**
  `g++ -O3 -std=c++17 -pthread -I. experiments/motivation/flashanns/tier_probe.cpp -o tools/motivation-tier-probe`.
  **Expected:** exit 0.
- [x] **Step 5: Rerun the unit tests and a regular-file smoke.** **Expected:**
  deterministic offsets and valid JSON; no hardware claim from the smoke.

### Task 5: Freeze the current LAION candidate trace

**Files:**
- Create: `experiments/motivation/flashanns/freeze_trace.py`
- Create: `experiments/motivation/flashanns/tests/test_freeze_trace.py`
- Produce: `results/motivation/flashanns-10m/manifests/laion-trace/manifest.json`

- [x] **Step 1: Write failing tests** requiring aligned query/candidate offset
  files, exactly 10,000 queries, `L=400`, stable IDs, and recomputed SHA-256.
- [x] **Step 2: Implement the trace freezer** to copy a freshly generated
  serial PQ-navigation trace and its identity manifest without performance
  values.
- [x] **Step 3: Run one host-only trace-generation command** using the frozen
  LAION graph/PQ artifacts and `--eval-trace-dir`; do not access or stage VMEM.
- [x] **Step 4: Recompute and freeze query, candidate, result, graph, slot-map,
  and executable identities.** **Expected:** manifest accepted.

### Task 6: Build the C2 replay harness

**Files:**
- Create: `experiments/motivation/flashanns/replay_probe.cpp`
- Create: `experiments/motivation/flashanns/run_replay.py`
- Create: `experiments/motivation/flashanns/tests/test_replay.py`

- [x] **Step 1: Write failing tests** for useful-page derivation, selective
  4 KiB pages, blind 16 KiB expansion, Top-1/2/8 two-hop expansion, duplicate
  removal, page bounds, useful-page percentage, and physical amplification.
- [x] **Step 2: Run focused tests.** **Expected:** fail because replay policy
  functions are missing.
- [x] **Step 3: Implement pure policy construction first** so it can be tested
  without a device. Policy output contains one ordered page list per query and
  one frozen useful-page set.
- [x] **Step 4: Implement the read-only replay binary** using
  `VMEM_IOC_READ_BATCH` into prefaulted host buffers. Emit per-query completion
  time, logical pages, useful pages, and pre/post physical counters.
- [x] **Step 5: Compile and run regular-file/pure-policy tests.** **Expected:**
  all pass; no paper result is accepted from these smokes.

### Task 7: Build the C3 capability and QD runner

**Files:**
- Create: `experiments/motivation/flashanns/run_qd.py`
- Create: `experiments/motivation/flashanns/tests/test_qd.py`

- [x] **Step 1: Write failing tests** for the smallest-90%-bandwidth knee,
  complete concurrency levels, entire-interval `aqu-sz` means including zero,
  and rejection of active-only averages.
- [x] **Step 2: Run focused tests.** **Expected:** fail because knee and parser
  functions are absent.
- [x] **Step 3: Implement two modes:** a read-only unique-page VMEM batch sweep
  for `{1,2,4,8,16,32,64,128}`, and a wrapper around the frozen Wise
  Prefetcher at `T={1,2,4,8,16}` with 100-ms Linux block-stat sidecars. The
  latter use the same weighted-I/O-time definition as `iostat aqu-sz` because
  the installed sysstat ignores sub-second intervals.
- [x] **Step 4: Rerun tests.** **Expected:** pass and preserve idle samples.

### Task 8: Preflight the live device and execute C1

**Files:**
- Produce: `results/motivation/flashanns-10m/preflight/live.json`
- Produce: raw and accepted `c1_tier` records

- [x] **Step 1: Read-only preflight** `/dev/vmem0`, VMEM sysfs, backing-device
  identities, open users, dirty bytes, I/O errors, cache limit, staged image
  identity, and available host memory.
- [x] **Step 2: Stop for approval** before the first cache reset, module reload,
  or image overwrite. Name the exact staged source, destination range, and
  restoration target.
- [x] **Step 3: For T2I, YFCC, then LAION, stage or verify the image once and
  run five host, five CXL-cache, and five Flash repetitions.** Each run uses
  2,000 distinct pages and captures all latency and counters in one pass.
  All three datasets are complete with 45/45 accepted records under tag
  `mot-c1-20260908a`.
- [x] **Step 4: Seal only records whose tier classification passes.** Preserve
  all rejected and negative results.
- [ ] **Step 5: Restore the declared default image and prove full identity.**

### Task 9: Execute C2 in one LAION staging window

**Files:**
- Produce: raw and accepted `c2_admission` and `c2_coverage` records

- [x] **Step 1: Stage/verify LAION once and claim exclusive backing-device
  ownership.** Reject the campaign if another workload appears.
- [x] **Step 2: Run five cold repetitions of selective-4K and blind-16K.**
- [x] **Step 3: Run five cold repetitions of Demand, Top-1, Top-2, and Top-8.**
- [x] **Step 4: Validate identical trace/useful-set identities and physical
  counters across each comparison block.** Do not require a favorable trend.
- [ ] **Step 5: Restore the default image with full identity evidence.**

### Task 10: Execute C3 capability and workload sweeps

**Files:**
- Produce: raw and accepted `c3_capability` and `c3_qd` records

- [x] **Step 1: In one read-only device window, run five complete capability
  sweeps and compute `QD_knee` only after all repetitions are accepted.**
- [x] **Step 2: For each dataset, run five repetitions at each
  `T={1,2,4,8,16}`, with at least 2,000 queries or 30 seconds per mark.**
- [x] **Step 3: Validate recall, query/candidate identities, entire-interval QD,
  dual-backing counters, and absence of other I/O.**
- [x] **Step 4: Restore the default image and record clean VMEM state.**

### Task 11: Aggregate and render the protected figures

**Files:**
- Create: `experiments/motivation/flashanns/aggregate.py`
- Create: `experiments/motivation/flashanns/plot.py`
- Create: `experiments/motivation/flashanns/tests/test_aggregate_plot.py`
- Produce: `results/motivation/flashanns-10m/aggregate/motivation.csv`
- Produce: `results/motivation/flashanns-10m/aggregate/provenance.json`
- Produce: `results/motivation/flashanns-10m/aggregate/quality-report.json`
- Produce: four PDFs under `results/motivation/flashanns-10m/figures/`; the
  fourth supersedes the current Figure 5 TikZ schematic.

- [x] **Step 1: Write failing tests** for exactly five runs per mark, bootstrap
  determinism, no old run IDs, no hard-coded plot arrays, Figure 4 dimensions
  and 12 pt minimum text, vector output, and the protected four filenames.
- [x] **Step 2: Implement accepted-only aggregation** with medians and 95%
  bootstrap intervals plus complete run-ID provenance.
- [x] **Step 3: Implement Figure 3, both Figure 4 PDFs, and Figure 5 from CSV.**
- [x] **Step 4: Run tests, render PDFs/PNGs, inspect with `pdfinfo`,
  `pdftotext`, `pdffonts`, and page images.**

### Task 12: Revise the paper without changing its framework

**Files:**
- Modify: `paper/sections/motivation.tex`
- Modify only if needed: `paper/sections/intro.tex`
- Modify only if needed: `paper/sections/background.tex`
- Replace: `paper/figs/mot-cliff.pdf`
- Replace: `paper/figs/mot-pathology-admission.pdf`
- Replace: `paper/figs/mot-pathology-prefetch.pdf`
- Replace: `paper/figs/mot-c3-io-concurrency.pdf`

- [x] **Step 1: Generate a measured-claim table** mapping every proposed number
  to an aggregate row and five run IDs.
- [x] **Step 2: Add the compact hardware-path paragraph and rewrite each
  existing subsection as mechanism, measurement, observation, constraint.**
- [x] **Step 3: Update the findings table from the measured range.** Do not add
  subsections, figures, Oracle, or Evaluation speedup.
- [x] **Step 4: Synchronize only changed C1--C3 numbers in Introduction and the
  4 KiB/2 MiB distinction in Background.**
- [x] **Step 5: Build with** `latexmk -pdf -g -interaction=nonstopmode main.tex`.
  **Expected:** no undefined references and Motivation 1.8--2.1 pages, never
  above 2.2 pages.

### Task 13: Final audit and handoff

**Files:**
- Produce: `results/motivation/flashanns-10m/readiness/final.json`
- Create: `paper/motivation_workspace/README.md`
- Copy: aggregate data, provenance, quality report, plotting scripts, PDFs,
  and PNGs into `paper/motivation_workspace/`

- [x] **Step 1: Run all Motivation and Evaluation Python tests plus serving
  tests.**
- [x] **Step 2: Regenerate all four Motivation PDFs from the copied CSV without
  raw-device access and compare semantic PDF text, dimensions, and plotted row
  counts with canonical outputs.**
- [x] **Step 3: Verify no old values or LAION-200k claims remain in paper-facing
  TeX/scripts and every plotted mark resolves to five accepted runs.**
- [x] **Step 4: Record figure hashes, paper hash, test counts, full device
  restoration evidence, and any preserved counterexample in `final.json`.**
- [x] **Step 5: Preserve the dirty worktree; do not commit, merge, delete, or
  clean unrelated user changes without a separate integration choice.**
