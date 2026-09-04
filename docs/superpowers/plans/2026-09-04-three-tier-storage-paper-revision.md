# FlashANNS Three-Tier Storage Paper Revision Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Every task below is tracked with a checkbox.

**Goal:** Align the full manuscript with the approved host-cache / 4 GiB CXL-DRAM-cache / dual-Flash hierarchy without changing section order or touching Figure 3 and Figure 4.

**Architecture:** Complete DiskANN-style graph-and-vector records are authoritative on dual Flash SSDs. `vmem_sw` exposes the hierarchy and manages a 4 GiB CXL-side page cache; host software keeps a smaller record cache and schedules asynchronous fills across queries.

**Tech Stack:** English LaTeX, TikZ, acmart, `latexmk`, `rg`, `git diff`, `sha256sum`, `pdftotext`.

**Spec:** `docs/superpowers/specs/2026-09-04-three-tier-storage-writing-design.md`

## Global Constraints

- Preserve the current section order and existing user edits.
- Do not modify `paper/figs/mot-pathology-admission.pdf`, `paper/figs/mot-pathology-prefetch.pdf`, or `paper/sections/fig_mot_bubbles.tex`.
- Figure 3 data and rendering remain unchanged; Figure 4 is deferred.
- Keep Wise Prefetching and Continuous Batching as the two design mechanisms.
- Never use legacy 50.25/49.25/85.8 QPS rows as evidence for the revised three-tier path.
- Do not commit automatically.

---

### Task 1: Correct Abstract and Introduction

**Files:**
- Modify: `paper/main.tex`
- Modify: `paper/sections/intro.tex`

- [x] Replace host-resident graph claims with Flash-resident graph-and-vector records.
- [x] Define the host cache, 4 GiB CXL-DRAM cache, and Flash backing in the first two pages.
- [x] Redefine score hide as no score-triggered or score-blocking Flash fill.
- [x] Redraw Figure 1(c) without changing the three-column comparison.
- [x] Remove legacy performance numbers from the paper-front claim until remeasurement.

**Reason:** Rapid Review must see the correct system contract before encountering any result.

### Task 2: Correct Background and Motivation Vocabulary

**Files:**
- Modify: `paper/sections/background.tex`
- Modify: `paper/sections/motivation.tex`

- [x] Describe fixed graph-and-vector records rather than host adjacency.
- [x] Define the three access outcomes and the 4 KiB versus 2 MiB granularity distinction.
- [x] Recast the latency cliff as CXL-DRAM-cache hit versus Flash fill.
- [x] Remove the host-neighbor-ID premise from C2 while preserving C1--C3 and all section labels.
- [x] Leave Figure 3 and Figure 4 inputs untouched.

**Reason:** The causal chain for wise prefetch and continuous batching must follow the actual hierarchy.

### Task 3: Align Design and Mechanism Figures

**Files:**
- Modify: `paper/sections/design.tex`
- Modify: `paper/sections/fig_design_wise.tex`
- Modify: `paper/sections/fig_design_cont.tex`

- [x] Introduce `HOST_RESIDENT`, `CXL_RESIDENT`, and `FLASH_ONLY` states.
- [x] Make complete record pages the unit of prefetch, deduplication, and admission.
- [x] Separate Flash-to-CXL fill from CXL-to-host admission.
- [x] Preserve the two mechanism narratives and remove graph-in-host as a design dependency.
- [x] Update both figures to show the correct destination and state transitions.

**Reason:** The previous figures collapsed host and CXL caches and showed only vectors on Flash.

### Task 4: Repair Implementation and Evaluation Contracts

**Files:**
- Modify: `paper/sections/impl.tex`
- Modify: `paper/sections/eval.tex`

- [x] Replace stale single-device identifiers with the dual-device topology and 2 MiB stripe.
- [x] Describe `vmem_sw` as the mapping/cache manager rather than the SSD.
- [x] Replace ambiguous window metrics with the three-level hit/fill accounting.
- [x] Replace `graph-in-host` ablation with host-cache and cross-query-dedup controls.
- [x] Mark legacy rows as non-evidence and leave revised results as measurement placeholders.
- [x] Define the full-corpus CXL-DRAM Oracle as an alternate configuration with zero timed Flash traffic.

**Reason:** Hardware disclosure and evaluation validity must match the architecture claimed in the paper.

### Task 5: Align Related Work, Discussion, and Conclusion

**Files:**
- Modify: `paper/sections/related.tex`
- Modify: `paper/sections/discussion.tex`
- Modify: `paper/sections/conclusion.tex`
- Modify: `paper/main.tex`

- [x] Correct the FlashANNS comparison-table placement row.
- [x] Replace graph-in-host and host-window limitations with the three-tier limitations.
- [x] Summarize the two-stage fill/admission path in the conclusion and remove duplicate prose.
- [x] Remove Appendix inclusion.

**Reason:** Reviewers will cross-check the comparison table, limitations, and conclusion against the system model.

### Task 6: Verify Scope, Terminology, and Build

**Files:**
- Verify all modified paper sources and the compiled PDF.

- [x] Run `git diff --check` on the modified sources.
- [x] Search for stale terms: `Graph and PQ live in host DRAM`, `graph-in-host`, `CXL-DRAM absent`, `/dev/nvme4n1`, and unqualified score-window claims.
- [x] Confirm Figure 3 and Figure 4 hashes are unchanged.
- [x] Run `latexmk -g -pdf -interaction=nonstopmode -halt-on-error main.tex`.
- [x] Check the log for undefined references/citations and inspect extracted PDF text for the three-tier contract.
- [x] Report any remaining placeholders as explicit measurement work, not completed evidence.

**Reason:** A compiling paper is insufficient if stale placement claims or unsupported legacy numbers remain.
