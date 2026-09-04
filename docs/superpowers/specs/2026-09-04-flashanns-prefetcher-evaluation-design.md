# Wise Prefetcher Minimal Paper Revision Plan

**Status:** Revised against the current manuscript

**Date:** 2026-09-04

**Goal:** Update only the Wise Prefetcher story to match the new PQ-guided
implementation while preserving the paper's existing architecture:
**Wise Prefetcher + Continuous Batching**.

## 1. Scope lock

This is not a new paper outline and not a full Design rewrite.

The revision must preserve:

- the existing section order and subsection structure;
- the C1/C2/C3 problem decomposition;
- the four Introduction contribution bullets;
- the overall two-mechanism design: Wise Prefetcher plus Continuous Batching;
- the Continuous Batching motivation, scheduler, and figure;
- Evaluation Q1--Q5 and the existing main-result/ablation structure;
- Figure 3 and Figure 4.

The revision changes only the obsolete prefetcher contract:

```text
old: frontier top-M -> predict soon-to-expand neighbors -> lookahead fill
new: PQ navigation -> commit rerank candidates -> fetch their FP-vector pages
```

Continuous Batching remains the second mechanism. Its interface to the updated
prefetcher is simply clearer: the Wise Prefetcher produces precise page batches;
Continuous Batching schedules such batches across in-flight queries to sustain
device parallelism.

## 2. The three questions the revised prefetcher must answer

### Q-P1. When does FlashANNS prefetch?

**Answer:** after host-side PQ navigation commits the bounded rerank candidate
set, and before full-precision reranking begins.

This replaces all claims that FlashANNS predicts the next hop or prefetches the
current frontier's likely neighbors. The trigger is a search-semantic event,
not a fixed hop count, queue threshold, or speculative lookahead distance.

The paper should explain the timing tradeoff in one paragraph:

- fetching during traversal is earlier but speculative;
- fetching on the exact-scoring load is precise but too late;
- fetching at candidate commitment is precise and still precedes exact use.

The prefetcher does not claim that the materialization wait disappears. It
claims that exact-scoring loads operate on already materialized pages.

### Q-P2. What does FlashANNS prefetch?

**Answer:** only the missing full-precision vector pages belonging to the
committed rerank candidates.

The graph adjacency and PQ codes remain in host DRAM and are used to decide the
candidate set. After commitment, FlashANNS maps candidate IDs to physical vector
pages, removes resident and in-flight pages, deduplicates repeated page IDs, and
issues the remaining pages. It does not prefetch graph pages, a two-hop
neighborhood, unused siblings, or the entire approximate frontier.

The key invariant is:

> Every requested page is justified by at least one committed vector that will
> participate in exact reranking.

### Q-P3. How does FlashANNS prefetch efficiently?

**Answer:** it increases useful data per Flash read through rerank-aware physical
placement and conservative request formation.

The design has three supporting steps:

1. **Rerank-aware placement.** Training-query traces identify vectors that
   frequently enter the same committed rerank set. Such vectors are placed in
   the same or nearby 4-KiB pages. Training and evaluation queries must be
   disjoint.
2. **Exact online filtering.** Residency filtering, in-flight filtering, and
   page deduplication prevent redundant reads within the committed wave.
3. **Density-bounded coalescing.** Nearby requested pages are combined only
   when the short run is sufficiently dense. The runtime must not fill sparse
   stripe holes merely to increase I/O size.

“Page utilization” must not be represented by one ambiguous percentage. The
paper should distinguish:

- **page precision:** fraction of fetched pages containing at least one scored
  vector;
- **vector-slot utilization:** scored vector slots divided by all vector slots
  carried by fetched pages; this is the primary real-page-use metric;
- **extent utilization:** requested pages divided by all pages actually read
  after coalescing; this exposes hole-fill amplification.

## 3. Mismatches in the current manuscript

| Current location | Obsolete statement | Minimal correction | Reason |
|---|---|---|---|
| `paper/main.tex` Abstract | complete graph-and-vector records stay on Flash; soon-to-expand pages are prefetched | graph/PQ guide host-side navigation; committed FP-vector pages are prefetched | current placement and trigger changed |
| `paper/sections/intro.tex` system paragraph and contribution 3 | fixed records share one residency decision; frontier pages are promoted | separate search information from FP payload; preserve “Wise Prefetcher + Continuous Batching” | restore implementation truth without changing thesis |
| Intro Figure 1(c) | graph and vectors both reside on dual Flash | host graph/PQ, Flash FP vectors, bounded CXL-side cache | figure currently contradicts D1 and code |
| `paper/sections/motivation.tex` C2 takeaway | useful policy fetches soon-to-expand nodes | useful policy waits for candidate commitment and fetches only rerank payload | motivation must lead to the new trigger |
| `paper/sections/design.tex` D2 | frontier top-M, byte budget, `install_top` | when/what/how contract above | D2 is the core rewrite |
| Design D4 opening | next-hop lookahead plus continuous batching | precise committed-page batches plus continuous cross-query issue | preserve Continuous Batching, change only its input |
| `paper/sections/impl.tex` opening | hop-ranked miss pages and bounce scoring | PQ navigation, committed page wave, residency barrier, FP rerank | implementation paragraph is stale |
| `paper/sections/eval.tex` Setup/Q3 | serving path is oneshot FP; ablation uses old promote/pagebin labels | identify PQ path and rename only the Wise Prefetcher ablation rows/metrics | Evaluation needs synchronization, not redesign |

## 4. Section-by-section modification plan

### Task 1: Correct Abstract and Introduction

**Files:** `paper/main.tex`, `paper/sections/intro.tex`

**Modify only:** the storage-placement sentence, the Wise Prefetcher definition,
Figure 1(c), the C2 solution paragraph, and contribution 3.

- Keep the problem statement, C1/C2/C3 ordering, score-hide/query-hide terms,
  Continuous Batching paragraph, and four-bullet contribution structure.
- Define Wise Prefetcher in one compact sentence answering when, what, and how.
- Replace “graph and vectors share fixed records on Flash” with the actual split:
  host graph/PQ information and Flash full-precision vector payload.
- Preserve the combined contribution title, **A wise prefetcher plus continuous
  batching**. Revise only its prefetcher half; retain the batching half about
  sustaining device depth across queries.
- Redraw only panel (c) of Figure 1 if textual edits cannot express the corrected
  placement. Panels (a) and (b) remain unchanged.

**Reason:** the first two pages currently define a different prefetcher and a
different storage placement from the implementation. This is a contract repair,
not a new narrative.

**Acceptance:** a reviewer can answer when/what/how from the Introduction while
still identifying Continuous Batching as the second core design.

### Task 2: Change only the C2 conclusion in Motivation

**File:** `paper/sections/motivation.tex`

**Modify only:** lines corresponding to the conclusion after Figure 4, the C2
sentence in “From measurements to constraints,” and the C2 table row.

- Preserve all measured Figure 3/Figure 4 data, captions, files, and the negative
  result that wider two-hop prefetch lowers precision.
- Replace “prefetch the soon-to-expand frontier” with “delay FP-vector fetch
  until approximate search commits the rerank candidates.”
- Add one sentence connecting wrong-page amplification to low vector-slot
  utilization; do not introduce implementation parameters here.
- Leave the C3 subsection and Continuous Batching motivation untouched.

**Reason:** existing measurements still support the problem. Only the positive
design conclusion derived from them has changed.

**Acceptance:** C2 leads directly to D2's new trigger and target; C3 still leads
directly to Continuous Batching.

### Task 3: Rewrite D2 around when, what, and how

**Files:** `paper/sections/design.tex`,
`paper/sections/fig_design_wise.tex`

Keep D1--D5 and their order. D2 remains the Wise Prefetcher subsection and is
the only subsection receiving a substantial rewrite.

D2 should contain three short paragraphs:

1. **When---candidate commitment.** PQ navigation finishes before FP payload
   movement; prefetch begins immediately before reranking.
2. **What---committed vector payload.** Translate committed IDs into missing,
   unique physical pages; exclude graph/PQ and speculative neighbors.
3. **How---useful page formation.** Use rerank-aware placement, online
   deduplication, and density-bounded short extents to raise real slot use while
   bounding read amplification.

Replace the D2 figure with a left-to-right diagram labeled **WHEN / WHAT / HOW**.
The center should be `COMMIT C_L`; the right side should show the scoring window
and exact rerank. Remove optional lookahead, frontier top-M, residual lookahead
budget, and live precision-controller elements.

**Reason:** D2 currently describes the retired implementation. A localized D2
rewrite is smaller and clearer than redistributing the prefetcher across D1--D4.

**Acceptance:** D2 explains the three decisions without function names or a
parameter inventory, and its figure can be understood independently.

### Task 4: Make only interface-level edits outside D2

**Files:** `paper/sections/design.tex`,
`paper/sections/fig_design_cont.tex`

- In the Design opening, replace the lookahead sentence with the committed-wave
  definition; keep the following inter-query Continuous Batching sentence.
- In D1 and its placement table, replace “frontier-top neighbors” with
  “committed rerank vectors.” Do not restructure the table.
- In D3, remove only stale references to `install_top` if they no longer describe
  the committed path; retain entry/soft-pin treatment.
- In D4, rename its prefetch input from “next-hop/lookahead pages” to
  “committed page batches.” Preserve the fetch-level cross-query scheduling,
  target device depth, and Continuous Batching figure structure.
- In D5, retain score-hide observability and add vector-slot and extent
  utilization alongside page precision.

**Reason:** these are interface references to D2. Leaving them unchanged would
make the Design internally inconsistent; rewriting the subsections would violate
the minimal-change constraint.

**Acceptance:** D1--D5 remain recognizable and Continuous Batching is neither
removed nor demoted.

### Task 5: Synchronize Implementation in one compact replacement

**File:** `paper/sections/impl.tex`

- Replace the obsolete opening description with the actual path: host graph/PQ
  navigation, committed candidate-to-page translation, one materialization
  wave, residency barrier, then FP rerank.
- Correct the placement/capacity facts required to interpret D2.
- Keep the existing “what we measure / what is implemented / what is not
  implemented” paragraph structure.
- Do not expand Implementation into a second Design section.

**Reason:** a reviewer will check D2 against Implementation immediately. One
paragraph must substantiate the revised mechanism.

### Task 6: Apply minimal Evaluation synchronization

**File:** `paper/sections/eval.tex`

Do not add or remove evaluation questions, subsections, or main figures. Do not
change Q1, Q2, Q4, or Q5 except where an obsolete prefetcher label appears.

Only make these changes:

1. Update the Setup index row from oneshot FP/no-PQ navigation to the evaluated
   PQ-guided path.
2. Keep Q3 as the existing module-ablation section. Within its current figure
   or table, replace obsolete `-promote/-pagebin` variants with three compact
   Wise Prefetcher comparisons corresponding to when, what, and how.
3. Add vector-slot utilization and extent utilization to the existing Q3
   evidence; do not create a new utilization figure.
4. Preserve the Continuous Batching ablation as a separate row/component in Q3.
5. Replace stale numbers or labels only after matched reruns; do not alter the
   recall/latency/throughput plotting contract.

**Reason:** Evaluation already asks whether each module is necessary. The new
prefetcher implementation changes the ablation labels and explanatory metrics,
not the evaluation architecture.

**Acceptance:** the Evaluation diff is small, Q1--Q5 remain intact, and Q3 can
validate all three prefetcher decisions without obscuring Continuous Batching.

### Task 7: Bounded terminology and consistency pass

**Files:** `paper/sections/discussion.tex`,
`paper/sections/conclusion.tex`, plus a global stale-term scan

- Replace only stale prefetcher terms such as next-hop lookahead,
  soon-to-expand pages, frontier top-M promotion, and graph/vector shared Flash
  records.
- Do not restructure Related Work, Discussion, or Conclusion.
- Preserve every Continuous Batching claim not logically dependent on the old
  lookahead interface.
- Compile and inspect page count, references, and the revised Wise Prefetcher
  figure at final column size.

## 5. Evaluation evidence for the three decisions

Reuse the current Q3 ablation rather than designing a new evaluation chapter:

| Decision | Minimal comparison | Primary evidence |
|---|---|---|
| When | early/frontier issue vs. commit-time issue | wasted NAND bytes and residual materialization wait |
| What | broad frontier payload vs. committed missing FP pages | unique pages/query and page precision |
| How | default layout vs. rerank-aware layout, with/without dense coalescing | vector-slot utilization, extent utilization, QPS/p99 |

All comparisons must hold candidate IDs, search budget, and recall constant.
The current provisional T2I result may motivate the experiment but cannot yet
support a general throughput claim for the layout.

## 6. Final reviewer test

The revision succeeds only if all four statements are simultaneously true:

1. The paper is still visibly about **Wise Prefetcher + Continuous Batching**.
2. The revised Wise Prefetcher clearly answers when, what, and how.
3. The substantial prose/figure rewrite is confined to D2, with only necessary
   contract edits before and after it.
4. Evaluation retains its current structure and changes only the prefetcher
   configuration, ablation labels, and utilization metrics.
