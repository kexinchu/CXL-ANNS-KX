# FlashANNS Six-Figure Evaluation Design

**Status:** Approved

**Updated:** 2026-09-05

**Scope:** Evaluate the frozen Wise Prefetcher plus Continuous Batching on
T2I-10M, YFCC-10M, and LAION-10M using six Evaluation figures, without an
Oracle experiment.

## 1. Reviewer-Facing Evidence Contract

The Evaluation must establish one causal chain:

1. FlashANNS improves the recall--latency and recall--throughput frontier over
   demand access to the same Flash-backed address space.
2. The Wise Prefetcher improves the timing, selection, and physical efficiency
   of committed full-precision page movement.
3. Continuous Batching converts those precise transfers into useful Flash
   parallelism without changing per-query candidates or recall.
4. Exact reranking reads resident bytes and does not trigger a Flash fill.
5. Cold/warm and cache-capacity tests expose the boundary of the result.

No Oracle is measured. Consequently, the Evaluation makes no claim about an
upper bound, percentage of Oracle, or distance to an all-DRAM system. Absolute
performance, matched-recall comparisons, hide counters, and causal controls
carry the argument.

## 2. Frozen System and Storage Contract

The system under test is the integrated PQ-64 path frozen at runtime commit
`15e6632`:

```text
host graph and PQ navigation
  -> commit C_L
  -> select missing unique full-precision vector pages
  -> coalesce dense extent runs
  -> materialize into bounded host scoring windows
  -> exact rerank after this query's residency barrier
```

The final configuration uses eight workers, 128 MiB of host scoring window per
worker, pipeline depth two, effective issue depth equal to the worker count,
one shared copy pool, and completion-driven cross-query scheduling. Candidate
selection, page formation, issue ordering, and scoring residency remain frozen;
the evaluation may add only observation hooks and the approved MIPS/L2 metric
adapter.

All internal runs use the same three-tier placement:

- host DRAM: full graph adjacency, PQ-64 codes, query state, and bounded
  scoring windows;
- CXL side: a 4 GiB page cache, identified as physical CXL-DRAM only when live
  DAX/BAR evidence proves it, otherwise named a software proxy;
- Flash: the full-precision vector corpus on the two CD8P SSDs.

## 3. Dataset Contract

The evaluation uses native dataset semantics:

| Dataset | Size | Dimension/type | Metric | Role |
|---|---:|---|---|---|
| T2I-10M | 10M | 200-d float32 | MIPS | primary analysis and all ablations |
| YFCC-10M | 10M | 192-d uint8 | L2 | cross-metric generality |
| LAION-10M | 10M | 512-d float32 | MIPS | large-payload sensitivity |

Each dataset requires a hashed source, fixed 10,000-query subset, exact
recall@10 ground truth, graph, PQ-64 artifacts, packed Flash image, entry point,
ID map, and ID-to-slot map. Values may be widened losslessly where required but
must not be normalized, projected, truncated, or evaluated under a different
metric.

## 4. Systems and Causal Controls

### End-to-End Systems

| ID | System | Evidence role |
|---|---|---|
| `demand` | same host graph/PQ and committed candidates, demand Flash materialization | memory-semantic Flash baseline |
| `pipeann` | official block-I/O implementation on the same machine | external SSD-ANNS reference |
| `flashanns` | Wise Prefetcher plus Continuous Batching | complete system |

Demand and FlashANNS must share graph/PQ artifacts, query order, candidate set,
CPU budget, cache capacity, and recall parameters. PipeANN uses its native
search controls but is matched by corpus, query IDs, metric, final `k`, CPU
budget, and measured recall.

### Wise Prefetcher Controls

The T2I-10M Wise ablation answers exactly three questions using only supported
frozen paths and counters:

- **When:** serialized materialization versus one batch issued after candidate
  commitment, with the committed candidate set held fixed.
- **What:** the measured funnel from committed candidates to unique pages and
  then cache-missing pages; this is an accounting decomposition, not an
  invented fetch policy.
- **How:** post-commit batch without dense extent formation versus the co-use
  layout with dense extent formation.

Every accepted comparison preserves candidate IDs and returned IDs. Removed or
no-op controls are not revived as pseudo-ablations.

### Continuous Batching Controls

The T2I-10M scheduling ablation compares Wise-only and full Continuous
Batching at the same concurrency, then sweeps `T = 1, 2, 4, 8, 16`. A separate
open-loop load sweep compares Wise-only, FlashANNS, and PipeANN from low load to
saturation. Candidate hashes and recall must remain unchanged between internal
scheduling controls.

## 5. Recall, Repetition, and Service Protocol

For each end-to-end method, measure:

```text
L = 50, 100, 200, 400, 800, 1600
```

Extend to `L = 2400, 3200` only when the base sweep does not reach the declared
recall floor. The common cross-dataset anchor is recall@10 at least 0.90; T2I
also marks the nearest measured point at or above 0.92.

- smoke proof: 100 fixed queries;
- recall calibration: 500 fixed queries per operating point;
- final points: 10,000 fixed queries and five independent repetitions;
- estimates: five-run median with deterministic 95% bootstrap interval;
- system order: randomized within each matched block using seed 20260904;
- latency: retain the complete per-query array and report mean, P50, P95, P99,
  and maximum.

Maximum-throughput experiments use a closed-loop stream. The serving-load panel
uses an open-loop arrival process and plots achieved throughput against P99
latency so queueing cannot masquerade as useful throughput.

## 6. Six-Figure Contract

### Figure 1: T2I-10M Frontier

Two panels: recall@10 versus mean latency, and recall@10 versus throughput.
Show Demand, PipeANN, and FlashANNS with 95% intervals and mark the 0.90 and
0.92 recall anchors.

### Figure 2: YFCC-10M Frontier

The same two panels and visual encoding as Figure 1. This figure establishes
generality under native L2 distance; it does not repeat the mechanism analysis.

### Figure 3: LAION-10M Frontier

The same two panels and visual encoding as Figure 1. Interpretation may refer
to the larger full-precision payload only when NAND bytes per query support it.

### Figure 4: Wise Prefetcher Ablation

Three panels on T2I-10M:

1. **When:** matched-recall performance for serialized materialization versus
   the post-commit batch.
2. **What:** committed-candidate, unique-page, and cache-missing-page counts,
   alongside NAND MiB/query and commands/query.
3. **How:** vector-slot and extent utilization without and with co-use-plus-
   extent formation, with one aligned performance marker.

### Figure 5: Continuous Batching Ablation

Three panels on T2I-10M:

1. QPS versus concurrency for Wise-only and Continuous Batching.
2. NAND occupancy/useful bandwidth versus concurrency, with mean in-flight
   fills.
3. P99 latency versus achieved QPS under open-loop load for Wise-only,
   FlashANNS, and PipeANN.

The causal batching claim uses the same-`T` Wise-only control, never a T=8/T=1
division.

### Figure 6: Hide Proof and Robustness

One double-column 2-by-2 figure:

1. latency breakdown into PQ navigation, materialization critical wait, exact
   rerank, and scheduling/queue wait;
2. exact-score source percentages from host window, CXL-side cache, bounce,
   and score-triggered Flash;
3. paired true-cold and immediate-warm QPS/P99/Flash-fills results for all three
   datasets;
4. T2I cache-capacity sensitivity at 1, 2, 4, and 8 GiB.

The default 4 GiB result remains part of the formal matrix. The 1/2/8 GiB
capacity sweep is an explicitly approved sensitivity phase rather than a silent
change to the main cache contract.

## 7. Tables

The figures are accompanied by two tables only:

- **Setup:** hardware/backend identity, datasets, metrics, placement, cache and
  scoring-window budgets, CPU/NUMA allocation, query count, repetitions, and
  cold/warm definition.
- **Selected operating points:** Recall@10, QPS, mean/P50/P95/P99, NAND
  reads and MiB per query, CPU utilization, host/CXL memory footprint, and
  score-triggered Flash fills for Demand, PipeANN, and FlashANNS.

Tail latency and resource cost do not receive separate figures.

## 8. One-Pass Measurement Record

Every run seals its command and identities plus:

- query IDs, returned IDs, candidate IDs, and per-query latency;
- recall, QPS, mean/P50/P95/P99/max;
- requested, deduplicated, extent-added, useful, evicted, and late pages;
- NAND commands, bytes, busy time, and useful bandwidth;
- page precision, vector-slot utilization, and extent utilization;
- materialization critical wait, copy/queue time, and in-flight histogram;
- score-source counts from host window, CXL cache, bounce, and Flash;
- CPU utilization, peak RSS, graph/PQ bytes, host-window bytes, and CXL-cache
  bytes;
- git/binary hashes, dataset manifest, query seed, cache state, backend, and
  device identities.

Plotting and LaTeX tables consume only validated records, never console values.

## 9. Fail-Closed Gates

A run is rejected when artifacts, query IDs, metric, CPU/NUMA placement,
candidate hashes, recall protocol, cache state, or binary identity do not match
its comparison block; when fewer than the declared queries complete; or when
sidecar-derived recall/latency disagrees with the summary.

A FlashANNS hide point is additionally rejected if exact scoring uses bounce
bytes or triggers a Flash fill. Cold runs require empty-cache evidence; warm
runs require an immediate named cold parent. Negative and rejected runs remain
recorded and are never overwritten under the same run ID.

## 10. Completion Criteria

The Evaluation is complete only when:

1. all three datasets pass artifact, metric, and 100-query correctness gates;
2. Figures 1--3 contain validated recall--mean-latency and recall--throughput
   frontiers for Demand, PipeANN, and FlashANNS;
3. Wise when/what/how controls preserve candidates and support their claimed
   traffic or utilization effect;
4. the same-concurrency scheduling control shows whether batching changes QPS,
   occupancy, and P99 without changing recall;
5. exact scoring records zero bounce and zero score-triggered Flash fills;
6. five accepted cold/warm pairs exist for every dataset and the T2I cache
   sensitivity is complete;
7. both paper tables and all six vector-PDF figures regenerate from one
   validated CSV and provenance map;
8. the paper contains no Oracle result, ratio, or upper-bound claim, and
   `paper/sections/eval.tex` states the physical-versus-proxy backend truth.
