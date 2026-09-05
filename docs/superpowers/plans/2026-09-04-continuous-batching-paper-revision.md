# Continuous Batching Paper Revision Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Align the manuscript with the frozen Continuous Batching implementation while preserving the paper's Wise Prefetcher + Continuous Batching thesis and presenting the mechanism at ASPLOS paper granularity rather than as a runtime report.

**Architecture:** The revision follows one reviewer-facing argument: Wise Prefetching creates precise committed-page work, but a single query cannot keep Flash busy while respecting graph-search dependencies; Continuous Batching therefore overlaps materialization for one query with useful work from other queries under a bounded concurrency budget. The main prose explains this insight, the semantic boundary, and the performance consequence; low-level runtime names and tuning history stay out of the narrative.

**Tech Stack:** LaTeX, TikZ, the existing FlashANNS manuscript and its frozen T2I-10M evidence.

**Spec:** `docs/superpowers/specs/2026-09-04-flashanns-prefetcher-evaluation-design.md`

## Global Constraints

- Preserve the paper's existing section order and the two core mechanisms: **Wise Prefetcher + Continuous Batching**.
- Revise only Continuous-Batching-related statements and their direct interfaces; do not reopen the frozen Wise Prefetcher design.
- Preserve Figure 3 and Figure 4 without changing their data, assets, or conclusions.
- Keep Evaluation Q1--Q5 and the current figure/table structure; synchronize evidence rather than redesigning the chapter.
- Present Continuous Batching as bounded cross-query overlap, not as speculative execution, dynamic queue-depth control, or removal of a query's own data dependency.
- Distinguish the local same-machine Oracle from the paper's full-corpus CXL-DRAM Oracle.
- Do not replace historical results with the new frozen results; label protocols explicitly when both appear.
- Do not promote rejected tuning variants into design contributions or ablations.

---

## 1. Reviewer-facing story to freeze

The revised paper should let a reviewer recover the following argument without reading implementation details:

1. **Problem.** Wise Prefetching removes Flash reads from best-first navigation and creates one precise materialization wave only after the rerank set is committed. This improves I/O precision, but each query must still wait for its own committed vectors before exact reranking. A single query therefore cannot continuously expose enough independent Flash work.
2. **Insight.** The dependency is local to a query, not global to the service. While one query waits for materialization, another can navigate, issue its committed batch, or rerank already-resident vectors.
3. **Mechanism.** Continuous Batching schedules these stages across queries with bounded concurrency. It admits new work only as capacity becomes available, keeping useful Flash requests in flight without building a deep speculative queue.
4. **Correctness.** Scheduling changes when committed pages are materialized, not which candidates are visited, committed, or reranked. Recall is therefore governed by the same index and search parameters.
5. **Evidence.** The paper must show that cross-query overlap raises throughput and NAND occupancy at unchanged recall, then quantify the remaining distance to a same-machine, no-Flash upper bound.

One compact definition should be reused across the manuscript:

> **Continuous Batching overlaps a query's committed-page materialization with navigation or reranking from other queries, sustaining bounded Flash parallelism without changing per-query search semantics.**

This definition is the center of the revision. Thread structure, internal queues, copying routines, and discarded tuning attempts are supporting implementation facts, not elements of the contribution statement.

---

### Task 1: Tighten the Abstract and Introduction

**Files:**
- Modify: `paper/main.tex`
- Modify: `paper/sections/intro.tex`

**Purpose:** State the service-level insight early without turning the opening into an implementation summary.

- [ ] Replace the current generic phrase that batching “issues fills across queries” with the frozen one-sentence definition above.
- [ ] Preserve the three challenges and four contribution bullets; modify only the C3 solution paragraph and the batching half of contribution 3.
- [ ] Make the contrast explicit: Wise Prefetching improves the precision of each materialization wave, whereas Continuous Batching overlaps independent waves across queries.
- [ ] Keep quantitative T=8 results out of the Abstract until matched multi-dataset evidence exists. If a T2I-only number is mentioned in the Introduction, label it as a focused implementation result rather than a general end-to-end claim.
- [ ] Do not introduce worker counts, slot counts, stage names, queue-depth syntax, window sizes, or function names.

**Reason:** ASPLOS readers need the contribution boundary and causal insight on the first page; operational details there obscure rather than strengthen novelty.

**Acceptance:** The opening explains why batching is necessary after Wise Prefetching and what it does, in no more than one additional paragraph and without parameter-level exposition.

---

### Task 2: Preserve Motivation and Strengthen Only the C3 Bridge

**File:**
- Modify: `paper/sections/motivation.tex`

**Purpose:** Ensure the measured pipeline-bubble problem leads directly to the updated design.

- [ ] Leave Figure 3, Figure 4, their captions, numerical data, and the C1/C2 discussion unchanged.
- [ ] Retain the observation that a single dependency-ordered walk underutilizes Flash even when bandwidth remains available.
- [ ] Replace only the final C3 takeaway with the query-local-dependency insight: one query's wait need not stall the service.
- [ ] End C3 with a forward reference to bounded cross-query overlap, without previewing scheduler internals.

**Reason:** Motivation should establish the necessity of the mechanism, not describe how the runtime implements it. The current evidence already supports the problem and does not need to be reopened.

**Acceptance:** C3 reads as a clean problem-to-insight transition and remains visually and structurally unchanged.

---

### Task 3: Rewrite Design D4 as an Academic Mechanism

**Files:**
- Modify: `paper/sections/design.tex`
- Modify: `paper/sections/fig_design_cont.tex`

**Purpose:** Make D4 the central Continuous Batching argument rather than a scheduler specification.

- [ ] Keep the D1--D5 structure and leave the Wise Prefetcher subsection intact except for its interface sentence into D4.
- [ ] Organize D4 into four short conceptual paragraphs:
  1. **Why per-query execution stalls:** committed-page materialization remains a query-local barrier.
  2. **Cross-query overlap:** navigation, materialization, and reranking from different queries can coexist.
  3. **Bounded admission:** the runtime exposes enough independent work to occupy Flash while limiting outstanding work and memory pressure.
  4. **Semantic invariance:** batching changes timing only, so search order within a query and recall remain unchanged.
- [ ] Explain that a query reranks only after its own committed pages are ready. Explicitly reject any interpretation that Continuous Batching predicts future candidates or removes same-query dependencies.
- [ ] Mention implementation choices only once, collectively: the prototype uses a bounded set of in-flight query stages and completion-driven scheduling. Move all further detail to Implementation.
- [ ] Replace the existing placeholder with a conceptual figure containing three elements only:
  - several query lanes at different stages;
  - a shared bounded Flash-materialization lane;
  - an overlap annotation showing that one query's wait is covered by another query's useful work.
- [ ] Remove global-dedup fan-out, queue taxonomy, internal state labels, and runtime object names from the figure unless the frozen code and the paper's argument both require them.
- [ ] Caption the figure around the causal result: cross-query overlap fills single-query bubbles while preserving each query's materialization-before-rerank dependency.

**Reason:** A strong systems-paper Design section exposes the minimal mechanism needed to validate the insight. A state-machine walkthrough makes the design look incremental and invites scrutiny of incidental engineering choices.

**Acceptance:** A reviewer can explain the problem, insight, mechanism, and correctness from D4 and its figure, but cannot mistake the subsection for API or scheduler documentation.

---

### Task 4: Use Implementation Only to Establish Credibility

**File:**
- Modify: `paper/sections/impl.tex`

**Purpose:** Demonstrate that the mechanism is real while preventing implementation detail from taking over the paper.

- [ ] Add one compact paragraph describing the frozen execution path: host-side PQ navigation commits candidates, asynchronous materialization fills the bounded scoring window, and workers interleave ready work across queries.
- [ ] State only the choices necessary to reproduce the claimed design: bounded per-worker in-flight capacity, shared asynchronous copy resources, completion-driven admission, and the configured concurrency limit.
- [ ] Put exact experimental values such as worker count, concurrency cap, window size, and query protocol in the Setup table rather than in Design prose.
- [ ] Omit internal function names, stage-enum names, ioctl history, rejected scheduling variants, and tuning chronology.
- [ ] State the proxy boundary honestly: the measured implementation includes the current userspace copy/cache path and does not represent a zero-overhead hardware CXL datapath.

**Reason:** Implementation exists to make the design believable and reproducible. It should not create a third, lower-level version of the same story.

**Acceptance:** The added material fits in roughly one paragraph plus Setup entries and is sufficient to connect D4 to the executable system.

---

### Task 5: Synchronize Evaluation with Minimal Structural Change

**Files:**
- Modify: `paper/sections/eval.tex`

**Purpose:** Validate the batching claim causally, using the existing Evaluation organization.

- [ ] Keep Q1--Q5, the main-result figure, and the existing ablation location.
- [ ] In Q2, distinguish three evidence roles rather than presenting a long configuration matrix:
  - the single-query Wise Prefetcher result shows the quality of one precise materialization wave;
  - the T=8 result shows the benefit of cross-query overlap;
  - the same-machine T=8 Oracle bounds the remaining software and materialization overhead.
- [ ] Add the frozen T2I-10M point only with its complete interpretation: 689 QPS, recall@10 of 0.920, and 64.3% NAND occupancy; compare it with the 952-QPS local Oracle as 72.4% of that upper bound.
- [ ] Report 689 QPS as the locked representative point and retain the observed 650--690 QPS repeat band. Do not call 689 a mean unless repetitions establish it.
- [ ] Keep historical T=1 and earlier rows rather than overwriting them. Label code path, concurrency, query count, and Oracle type wherever results are not directly comparable.
- [ ] Do not compute a batching speedup by dividing the new T=8 result by a T=1 result collected under a different protocol.
- [ ] In Q3, make the Continuous Batching ablation answer one question only: does cross-query scheduling improve QPS and device occupancy at unchanged recall over Wise Prefetching alone?
- [ ] Report mean, P50, P95, and P99 latency alongside throughput for the final matched protocol. Keep Wise-Prefetcher page/slot/extent utilization metrics in its own ablation rather than attributing them to batching.
- [ ] Use a thread/concurrency sweep only if matched data already exist. Otherwise show the frozen T=1 and T=8 contrast without inventing intermediate points.
- [ ] Do not elevate discarded scheduler variants into paper ablations. Mention them only if one is needed to substantiate a specific design choice; otherwise omit them.

**Reason:** The evaluation should prove one causal claim per mechanism. A catalogue of knobs and failed variants reads as optimization work, while matched recall, throughput, occupancy, and tail latency directly test the paper's thesis.

**Acceptance:** Evaluation changes are localized; the batching evidence demonstrates useful overlap and its remaining gap without conflating protocols or Oracle definitions.

---

### Task 6: Close the Claim Boundary in Discussion and Conclusion

**Files:**
- Modify: `paper/sections/discussion.tex`
- Modify: `paper/sections/conclusion.tex`

**Purpose:** Ensure the paper ends with the same scoped contribution it introduced.

- [ ] Replace the stale statement about an approximately 11x gap to PipeANN with the measured residual-to-Oracle interpretation.
- [ ] Attribute the remaining gap conservatively to the measured userspace data path and synchronization overhead; do not claim a device limitation without evidence.
- [ ] State that scaling beyond the bounded evaluated concurrency and eliminating proxy overhead are future engineering opportunities, not demonstrated contributions.
- [ ] Keep the Conclusion to one sentence per mechanism: Wise Prefetching improves which Flash bytes are fetched; Continuous Batching overlaps those precise fetches across queries.

**Reason:** A stale or overbroad final claim can undo the careful scope established in Design and Evaluation.

**Acceptance:** Abstract, Introduction, Design, Evaluation, Discussion, and Conclusion use one consistent Continuous Batching definition and one consistent Oracle boundary.

---

### Task 7: Paper-Level Verification

**Files:**
- Verify: `paper/main.tex`
- Verify: `paper/sections/intro.tex`
- Verify: `paper/sections/motivation.tex`
- Verify: `paper/sections/design.tex`
- Verify: `paper/sections/fig_design_cont.tex`
- Verify: `paper/sections/impl.tex`
- Verify: `paper/sections/eval.tex`
- Verify: `paper/sections/discussion.tex`
- Verify: `paper/sections/conclusion.tex`

- [ ] Search for and remove unsupported positive claims about speculative next-query work, global cross-query deduplication, dynamic queue-depth control, same-query hide, or linear scaling.
- [ ] Confirm that Figure 3 and Figure 4 assets are unchanged.
- [ ] Confirm that every new number is paired with dataset, recall, concurrency, query protocol, and Oracle scope where needed.
- [ ] Compile the paper and resolve LaTeX, bibliography, reference, and overfull-box regressions introduced by this revision.
- [ ] Inspect the Continuous Batching figure at final two-column size; it must communicate overlap and boundedness without depending on tiny implementation labels.
- [ ] Re-read only Abstract, the C3 motivation, D4, the main Evaluation result, and Conclusion as a continuous reviewer path. Remove any detail that does not advance problem, insight, mechanism, correctness, or evidence.

**Reason:** The final check is narrative consistency, not merely compilation. The contribution should survive a reviewer who reads only the paper's main argument path.

**Acceptance:** The revised paper answers five questions cleanly: why batching is needed, what it overlaps, why it is bounded, why recall is unchanged, and what measured benefit remains relative to the correct Oracle.

---

## Facts to preserve, not foreground

The following frozen facts constrain accuracy but should appear only where required for reproducibility or evidence interpretation:

- The primary frozen point is T2I-10M at T=8: 689 QPS, 64.3% NAND occupancy, recall@10 = 0.920.
- Repeated throughput falls in the 650--690 QPS range.
- The same-machine local T=8 Oracle reaches 952 QPS; it is not the manuscript's full-corpus CXL-DRAM Oracle.
- The implementation uses bounded in-flight capacity and does not gain from unbounded concurrency.
- A query never reranks before its own committed pages are materialized.
- The remaining gap is observed in the current userspace copy/cache path.

These facts are guardrails. They must not become the subsection outline.

