# Phase-Aware Placement Design

**Status:** Approved for specification; implementation has not started.

## 1. Objective and Scope

This change improves FlashANNS's offline vector placement so that vectors likely to be consumed in the same search stage occupy fewer pages and those pages form longer contiguous runs. The goal is to reduce physical NAND commands without increasing transferred bytes or changing search semantics.

The first implementation and validation target is T2I-10M. YFCC-10M and LAION-10M are follow-on confirmations required before the new placement can replace the layout used by the paper's cross-dataset results. This work changes only the offline placement pipeline and the resulting vector image; the frozen online prefetcher and continuous-batching implementation remain unchanged.

## 2. Motivation and Current Baseline

At `L=400`, the current offline diagnostic reports:

| Layout | Pages/query | Runs/query | Useful slot occupancy |
|---|---:|---:|---:|
| Identity | 380.02 | 330.79 | 52.73% |
| Canonical extent | 361.01 | 321.52 | 55.61% |
| Existing co-occurrence | 361.01 | 329.97 | 55.61% |

The canonical layout reduces the number of pages but leaves most accessed pages in short physical runs. The existing co-occurrence pass does not improve page packing and makes run formation worse than the canonical extent layout. The redesign therefore treats vector-to-page packing and page-to-extent ordering as two separate, explicitly optimized levels.

These numbers are diagnostic evidence, not paper results. They establish the optimization target and must not be cited as a performance claim until the measured device A/B protocol in Section 9 passes.

## 3. Query Isolation and Provenance

The public T2I query file contains 100,000 queries. Formal evaluation uses query IDs `[0, 10000)`. Placement construction must use only `[10000, 100000)`.

The placement pool is deterministically split into:

- 8,000 training queries for collecting placement statistics; and
- 2,000 internal-validation queries for choosing the bounded design parameter.

The exact query IDs, source-file hash, split seed, graph hash, PQ hash, executable hash, and command line are recorded in a placement manifest. Before trace collection, a gate verifies that the training and internal-validation IDs are disjoint from the formal evaluation range and from each other. Any overlap, missing hash, or identity mismatch aborts construction.

## 4. Trace Contract

Placement statistics are derived from frozen single-query graph search at `T=1` and `L=400`. The run uses the same graph, PQ representation, entry point, and search semantics as evaluation, with expansion tracing enabled. Page scoring and the online wise-prefetch path are disabled so that the trace represents the search's data demand rather than the current physical layout or prefetch policy.

Each trace records the ordered search stages and the logical vector IDs requested by each stage. Internal validation replays the held-out traces at `L=400`, `L=800`, and `L=1600`. Only `L=400` may select the placement parameter; the larger search lists are guardrails against overfitting.

## 5. Two-Level Placement Construction

### 5.1 Vector-to-page packing

For logical vectors `u` and `v`, define

`Wv(u,v) = number of training search stages that request both u and v`.

The builder constructs a bounded top-`K` partner set for each vector. It first uses deterministic hashing to identify candidate partners, then makes a second pass over the trace to compute exact weights only for those candidates. Candidate edges are sorted by descending weight with logical IDs as deterministic tie-breakers, then consumed by greedy matching. Each vector appears in exactly one output slot; unmatched vectors are appended in canonical logical-ID order.

The only tunable parameter is `K in {4, 8, 16}`. The internal-validation traces select the smallest `K` that achieves the best `L=400` run count within measurement precision while satisfying all guardrails in Section 7. No formal evaluation query participates in this selection.

### 5.2 Page-to-extent ordering

After vectors are packed into pages, define

`Wp(p,q) = number of training search stages that request both page p and page q`.

The builder creates a weighted path cover over pages. Edges are considered by descending weight with page IDs as deterministic tie-breakers. An edge is accepted only if both endpoints have degree below two, it does not form a cycle, the resulting chain contains at most 32 pages, and the chain does not cross a 2 MiB (512-page) device stripe boundary. A disjoint-set structure enforces the cycle and chain constraints. Resulting chains are emitted deterministically; pages not covered by a positive-weight chain retain canonical order.

This separation is deliberate: vector pairing improves useful occupancy within a page, while page ordering coalesces the remaining page demand into larger physical reads. Neither stage changes graph topology, vector values, PQ codes, or query execution.

## 6. Scalability and Determinism

The builder must stream traces and vector data; it must not materialize a dense vector-pair or page-pair matrix. Memory use is bounded by the configured top-`K` candidate table, page metadata, and the current construction buffers. All ordering decisions have explicit logical-ID tie-breakers. Repeating construction with the same inputs and manifest must produce a byte-identical slot map and vector image.

## 7. Offline Validation Gate

Before any image is staged to the device, the candidate must pass all of the following on internal-validation traces:

- at `L=400`, physical runs per query decrease by at least 5% relative to the current canonical extent layout;
- at `L=400`, pages per query do not increase and useful slot occupancy does not decrease;
- at `L=800` and `L=1600`, neither pages per query nor runs per query regresses by more than 1%;
- the slot map is a complete permutation of all 10 million logical vector IDs, with no duplicates, omissions, or out-of-range IDs; and
- the output manifest contains complete input, executable, configuration, split, slot-map, and image hashes.

Failure of any condition stops the workflow before device mutation. Offline counters may diagnose locality, but they do not establish NAND or serving improvements.

## 8. Image Materialization and Integrity

Only a placement that passes the offline gate is materialized as a host-side candidate image. Image generation streams every logical record through the approved permutation and verifies record count, record size, source-to-destination uniqueness, and end-to-end hashes. The source graph and vector payload remain immutable.

Before staging, the workflow records the current canonical device image identity and verifies that a restorable canonical host image exists. Staging `/dev/vmem0` is a separate destructive step and requires explicit user authorization. The workflow records candidate host and device hashes, runs the requested experiment, restores the canonical image in an exit path, and verifies the restored device hash. A failed restore blocks further experiments and paper use.

## 9. Measured Device A/B Protocol

The candidate and canonical layouts are compared under identical cold-cache conditions, binary identity, query order, and search parameters:

1. Smoke check: `T=1`, `L=400`, 100 queries, one repetition.
2. Mechanism check: `T=1`, `L in {400, 800, 1600}`, 1,000 queries, three repetitions.
3. Serving check: `T=8`, `L=400`, 10,000 queries, five repetitions.
4. Robustness check: `T=8`, `L in {800, 1600}`, three repetitions.

Every run records recall, throughput, mean and P99 latency, host-window misses, NAND bytes and commands, mean physical read size, cache state, image and binary hashes, and run acceptance status. Comparisons use paired repetitions and retain all accepted raw artifacts.

## 10. Adoption Criteria

The T2I candidate is adopted only if all of the following hold:

- at `T=1, L=400`, median NAND commands per query decreases by at least 5%;
- NAND bytes per query increases by no more than 1%;
- recall is identical within the existing evaluation tolerance;
- at `T=8, L=400`, median throughput improves by at least 3%;
- mean and P99 latency show no statistically meaningful regression;
- the direction is consistent across accepted repetitions; and
- the canonical image is successfully restored and hash-verified.

If command coalescing improves but serving performance does not, the result may be retained only as mechanism evidence and must not be described as an end-to-end speedup. A failed T2I candidate is rejected without changing the paper layout.

After T2I adoption, the same frozen procedure is repeated independently for YFCC-10M and LAION-10M. Cross-dataset paper claims require all three datasets to pass their provenance and correctness gates. Any accepted paper result that depends on the optimized extent image must be rerun; old- and new-layout results must never be mixed. PipeANN and original-layout Demand results are unaffected unless their physical image is explicitly changed.

## 11. Paper Evidence Boundary

The intended paper claim is narrow: phase-aware offline placement increases useful data per page and coalesces page demand into fewer physical NAND commands, complementing rather than replacing the wise prefetcher and continuous batching. The paper may report only metrics produced by accepted device runs. Training-trace locality, replay estimates, and offline gates are supporting diagnostics and must be labeled as such.

## 12. Failure Handling

Construction, validation, staging, and restoration are fail-closed. Invalid permutations, identity mismatches, incomplete traces, rejected runs, interrupted staging, or failed restoration produce no accepted artifact. Raw evidence is preserved for diagnosis. The workflow does not silently fall back to an old map, substitute missing values, or mark an incomplete run as zero.
