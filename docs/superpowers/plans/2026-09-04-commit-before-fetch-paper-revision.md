# Wise Prefetcher Minimal Manuscript Revision Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` and execute the tasks in order. Preserve the
> current manuscript structure and Continuous Batching design.

**Goal:** Replace only the obsolete frontier/lookahead Wise Prefetcher story
with the implemented when/what/how contract.

**Architecture:** FlashANNS remains Wise Prefetcher + Continuous Batching. The
prefetcher waits for PQ candidate commitment, selects only missing FP-vector
pages, and uses rerank-aware placement plus conservative request formation to
raise useful data per page. Continuous Batching schedules the resulting page
batches across queries.

**Spec:**
`docs/superpowers/specs/2026-09-04-flashanns-prefetcher-evaluation-design.md`

## Global constraints

- Do not change section/subsection order, C1/C2/C3, or the four contributions.
- Do not remove or demote Continuous Batching.
- Do not modify Figure 3 or Figure 4.
- Substantially rewrite only D2 and its Wise Prefetcher figure.
- Keep Evaluation Q1--Q5 and all existing main figure groups.
- Do not claim zero materialization wait or reuse stale performance numbers.

### Task 1: Repair the first-page prefetcher contract

**Files:** `paper/main.tex`, `paper/sections/intro.tex`

- [ ] Replace the Abstract's graph-and-vector/shared-record placement with host
  graph/PQ and Flash FP vectors.
- [ ] Replace soon-to-expand-page prefetch with one sentence answering when,
  what, and how.
- [ ] Update only Figure 1(c)'s FlashANNS placement and caption.
- [ ] Preserve C1/C2/C3, score hide/query hide, and the Continuous Batching
  paragraph.
- [ ] In contribution 3, revise only the Wise Prefetcher half and retain the
  combined contribution title and batching claim.

**Verify:** the Introduction still says “wise prefetcher plus continuous
batching,” but no longer says graph and vectors share Flash records or that the
prefetcher predicts soon-to-expand pages.

### Task 2: Redirect C2 without changing Motivation evidence

**File:** `paper/sections/motivation.tex`

- [ ] Preserve Figure 3/Figure 4 files, values, captions, and negative results.
- [ ] Change the positive takeaway after Figure 4 from frontier prefetch to
  commit-time FP-payload fetch.
- [ ] Change only the C2 mapping sentence and C2 table row.
- [ ] Connect wrong-page amplification to vector-slot utilization in one
  sentence.
- [ ] Leave C3 and its Continuous Batching argument untouched.

**Verify:** C2 points to Wise Prefetcher; C3 still points to Continuous
Batching; no figure asset changed.

### Task 3: Rewrite D2 and the Wise Prefetcher figure

**Files:** `paper/sections/design.tex`,
`paper/sections/fig_design_wise.tex`

- [ ] Keep the D2 position and label.
- [ ] Paragraph 1 answers **when**: after PQ candidate commitment and before FP
  reranking.
- [ ] Paragraph 2 answers **what**: missing, unique FP-vector pages for the
  committed candidates.
- [ ] Paragraph 3 answers **how**: rerank-aware placement, residency/in-flight
  filtering, deduplication, and density-bounded short extents.
- [ ] Define page precision, vector-slot utilization, and extent utilization;
  identify vector-slot utilization as the real-page-use metric.
- [ ] Redraw the existing Wise Prefetcher figure as WHEN → WHAT → HOW with
  `COMMIT C_L` at the center.
- [ ] Remove top-M frontier promotion, optional lookahead, residual lookahead
  budget, and a live precision controller.

**Verify:** D2 and its figure answer all three questions without describing
Continuous Batching or listing runtime parameters.

### Task 4: Synchronize D1, D3, D4, and D5 minimally

**Files:** `paper/sections/design.tex`,
`paper/sections/fig_design_cont.tex`

- [ ] D1: change only the prefetch-target row from frontier neighbors to
  committed rerank vectors.
- [ ] D3: remove stale `install_top` coupling only if it contradicts the new
  committed path; preserve entry/soft-pin content.
- [ ] D4: replace next-hop/lookahead input with committed page batches, while
  preserving the cross-query scheduler, device-depth argument, and figure.
- [ ] In the Continuous Batching figure, rename only the lookahead queue if
  necessary; do not change its topology.
- [ ] D5: add slot and extent utilization to the existing observability list.

**Verify:** the Design still presents both mechanisms, and D4 consumes the page
batches produced by D2.

### Task 5: Synchronize Implementation and Evaluation

**Files:** `paper/sections/impl.tex`, `paper/sections/eval.tex`

- [ ] Replace only Implementation's stale mechanism paragraph with host PQ
  navigation → candidate commitment → page materialization → residency barrier
  → FP rerank.
- [ ] Preserve Implementation's existing disclosure structure.
- [ ] Update Evaluation Setup to name the PQ-guided serving path.
- [ ] Keep Q1--Q5 unchanged.
- [ ] Within existing Q3 only, rename prefetcher ablations to isolate when,
  what, and how; retain Continuous Batching as its own component.
- [ ] Add vector-slot and extent utilization to Q3's current table/figure rather
  than creating a new figure group.
- [ ] Preserve the existing recall, mean latency, tail latency, and throughput
  contracts.

**Verify:** the Evaluation diff is limited to configuration truth, Q3 labels,
and utilization metrics.

### Task 6: Perform a bounded stale-term and layout check

**Files:** `paper/sections/discussion.tex`,
`paper/sections/conclusion.tex`, full generated paper

- [ ] Replace stale prefetcher terms only; do not restructure these sections.
- [ ] Confirm Related Work is unchanged unless one sentence explicitly describes
  the retired frontier/lookahead implementation.
- [ ] Search globally for shared Flash records, soon-to-expand, frontier top-M,
  and optional lookahead; retain occurrences only when describing a rejected
  baseline.
- [ ] Compile the paper and inspect the revised Wise Prefetcher figure at final
  column size.
- [ ] Confirm Figure 3/Figure 4 hashes and Evaluation Q1--Q5 headings remain
  unchanged.

## Completion test

The revision is complete when the Abstract, Introduction, Motivation C2, and D2
give the same answers:

- **When:** after candidate commitment, before FP rerank.
- **What:** missing unique pages of committed FP vectors.
- **How:** rerank-aware placement plus exact filtering and dense short reads,
  measured primarily by vector-slot utilization.

At the same time, the paper must remain structurally and visibly a
**Wise Prefetcher + Continuous Batching** design, with only a small Evaluation
diff.
