# FlashANNS Wise Prefetcher and Continuous Batching Design Rewrite Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the five-module placeholder Design with a 3--3.5 page section centered on Wise Prefetching and Continuous Batching, including two real TikZ figures.

**Architecture:** Placement is an opening invariant rather than a numbered module. D1 separates mandatory current-expand pages from bounded optional frontier lookahead and gates scoring on residency; D2 interleaves ready work and fills across independent queries without changing graph-walk semantics.

**Tech Stack:** English LaTeX, TikZ, acmart, `latexmk`, `rg`, `pdftocairo`, `pdftotext`, `git diff`.

**Spec:** `docs/superpowers/specs/2026-09-03-cxl-dram-diskann-layout-design.md`

**Execution status:** Completed and verified on 2026-09-03; no commit created.

## Global Constraints

- Keep `paper/main.tex`, Introduction, Background, and Motivation unchanged.
- Do not modify Motivation figure sources or artifacts.
- Design has exactly two numbered subsections and two mechanism figures.
- Do not use an independent D0 subsection, architecture figure, or placement table.
- Hide uses the full fixed-record corpus on CXL-SSD and a bounded CXL-DRAM resident window.
- Host DRAM contains query state and the exact-scanned 10k selector, not the full graph.
- Mandatory current-expand pages are waved rather than dropped; optional lookahead alone is budget-truncated.
- D2 changes scheduling only; it does not change candidates, distances, visited state, beam width, or hop budget.
- Do not claim full query hide, a fixed target QD, or final-hardware performance.
- Do not commit automatically.

---

### Task 1: Freeze the Existing Boundary

**Files:**
- Read: `paper/sections/design.tex`
- Read: `paper/sections/appendix.tex`
- Read: `paper/main.pdf`

**Interfaces:**
- Consumes: current labels, compiled page boundaries, and dirty-worktree state.
- Produces: a baseline for structure, pagination, and protected-file checks.

- [ ] Record the current Design headings, labels, and starting/ending PDF pages.
- [ ] Record hashes of `paper/main.tex`, `intro.tex`, `background.tex`, `motivation.tex`, and protected Motivation PDFs.
- [ ] Confirm the baseline paper builds before Design edits.

### Task 2: Replace the Design Opening

**Files:**
- Modify: `paper/sections/design.tex`

**Interfaces:**
- Consumes: the spec's System Contract.
- Produces: the placement and score-hide invariants used by both mechanisms.

- [ ] Replace the old host-graph/proxy opening with the host selector, fixed-record CXL-SSD corpus, and bounded CXL-DRAM window.
- [ ] Name the full-corpus CXL-DRAM Oracle only as a separate evaluation mode.
- [ ] Define score hide and query hide without performance numbers.
- [ ] Remove the old architecture placeholder and placement table.

### Task 3: Write D1 and Build Figure 5

**Files:**
- Modify: `paper/sections/design.tex`
- Create: `paper/sections/fig_design_wise.tex`

**Interfaces:**
- Consumes: a resident `entry(u)`, its `N(u)`, page residency, and window headroom.
- Produces: mandatory and optional page requests plus a resident-only score gate.

- [ ] Write `\subsection{Wise Prefetcher}` with paragraph leads for resident objects, mandatory pages, optional lookahead, budgets, residency states, and the score gate.
- [ ] State `mandatory = unique(page(v))` for unseen neighbors and wave overflow rather than dropping it.
- [ ] Limit optional lookahead to top-ranked resident, unexpanded frontier candidates and residual budget.
- [ ] Draw the approved top-to-bottom Figure 5 with red mandatory, dashed amber optional, purple fill, and green resident/compute paths.
- [ ] Keep the CXL-SSD source and bounded CXL-DRAM window at the bottom of the same figure; omit Oracle and rejected policies.

### Task 4: Write D2 and Build Figure 6

**Files:**
- Modify: `paper/sections/design.tex`
- Create: `paper/sections/fig_design_cont.tex`

**Interfaces:**
- Consumes: per-query traversal state and D1 requests/completions.
- Produces: cross-query work-conserving scheduling and completion fan-out.

- [ ] Write `\subsection{Continuous Batching}` around `READY_EXPAND`, `WAIT_FILL`, `READY_SCORE`, and `DONE`.
- [ ] Specify shared mandatory/optional queues, cross-query 4 KiB deduplication, one-time installation, multi-query wakeup, fairness, and window-headroom gating.
- [ ] Bound the claim to useful cross-query issue opportunities.
- [ ] Draw the approved Figure 6 with three query swimlanes and a shared fill engine; show Q1/Q2 work during Q0's wait.
- [ ] Do not redraw Motivation's blocking/static-batch failure cases.

### Task 5: Close the Contract and Repair References

**Files:**
- Modify: `paper/sections/design.tex`
- Modify mechanically: `paper/sections/appendix.tex`

**Interfaces:**
- Consumes: D1 and D2 claims.
- Produces: explicit exclusions, correctness invariants, validation counters, and no dangling figure/table references.

- [ ] Add `What is deliberately absent` without a numbered subsection.
- [ ] Add `Correctness and validation` with the three invariants and required counters.
- [ ] Remove old D3--D5 text and all placeholder/fill-in markers.
- [ ] Replace the Appendix's three Design-figure/placement-table checklist rows with the two implemented mechanism figures.

### Task 6: Verify the Rewritten Section

**Files:**
- Verify: `paper/sections/design.tex`
- Verify: `paper/sections/fig_design_wise.tex`
- Verify: `paper/sections/fig_design_cont.tex`
- Verify: `paper/sections/appendix.tex`

**Interfaces:**
- Consumes: the complete rewrite.
- Produces: an author-reviewable PDF and scoped diff.

- [ ] Run `git diff --check` on all four files.
- [ ] Verify exactly two Design subsections and two Design figures.
- [ ] Reject stale positive terms: host full graph, expand-bundle/pagebin primary layout, LFU module, `score_page`, fixed target QD, and full query hide.
- [ ] Force `latexmk -g -pdf -interaction=nonstopmode -halt-on-error main.tex`.
- [ ] Verify no undefined references and report total paper pagination.
- [ ] Render and inspect the complete Design page range; require 3--3.5 pages, legible text, no overlap, and complementary rather than repetitive figures.
- [ ] Verify protected frontmatter files and Motivation PDFs retain their baseline hashes.
- [ ] Stop without committing and present the scoped diff for author review.
