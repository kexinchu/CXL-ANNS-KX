# Commit-Before-Fetch Prefetcher Paper Design

**Status:** Author-approved architecture boundary on 2026-09-04. This document
defines the truthful paper story for the current implementation; it does not
authorize new performance claims from stale or unmatched logs.

## 1. Reviewer-facing thesis

FlashANNS separates the information needed to navigate a graph from the
full-precision payload needed to return accurate results. The host retains a
compact adjacency graph and 64-byte PQ codes, finishes a bounded approximate
beam without reading full-precision vectors from Flash, and commits the final
candidate set. Only then does the runtime translate those committed IDs into
unique 4-KiB vector pages, coalesce dense short runs, issue one asynchronous
materialization wave through the Flash-backed CXL path, and perform exact
reranking after the required pages are resident.

The paper may continue to call this mechanism the **Wise Prefetcher**, but its
technical name and definition are **commit-before-fetch** or **PQ-guided
committed-candidate materialization**. It is not a predictor of future hops.

## 2. Exact placement contract

| Component | Normal placement | T2I-10M evidence | Paper treatment |
|---|---|---:|---|
| 10K navigation graph | host DRAM | loaded `nav_10k.bin` | entry selection metadata |
| Full adjacency (`R=32`) | host DRAM | 1,280,000,000 B | disclose as a first-class cost |
| PQ codes | host DRAM | 64 B/vector = 640,000,000 B | navigation/ranking metadata |
| ID/slot and ID-remap tables | host DRAM | at least 40 MB per 10M-ID table | disclose separately |
| Query, beam, visited set | host DRAM | per-query | private search state |
| Host scoring window | host DRAM in frozen run | 2 GiB `DramWindow` | not CXL-DRAM and not “small metadata” |
| CXL-side cache role | `vmem_sw` page cache | run logs show a 100 MiB cap | report configured cap, not only 4 GiB hardware maximum |
| Full-precision vectors | Flash-backed `/dev/vmem0` range | 20.48 GB mapped layout; 800-B logical vector in 2-KiB slot | authoritative payload |

The normal configuration therefore does **not** keep the full-precision vector
corpus in host DRAM, but it does keep the complete adjacency and PQ codes there.
The paper must stop saying that the host contains only query state and a small
record cache, or that graph and vector bytes share one residency decision.

## 3. Mechanism contract

For query `q`, beam bound `L`, and return count `k`:

1. Select an entry using the host-resident navigation graph.
2. Build the query-to-PQ lookup table once.
3. Traverse the host adjacency graph and rank candidates from host PQ codes.
   This phase performs no full-precision vector read from Flash.
4. Commit the final candidate set `C_L`; after this point, the vector IDs needed
   for exact reranking are known.
5. Map `C_L` to vector pages, remove pages already resident or already in
   flight, and deduplicate page IDs.
6. Apply extent-run coalescing only when span is at most 32 pages and requested
   density is at least 50 percent.
7. Submit the remaining pages through `HidePipe`; the single-query reference
   path uses one end-batch wave and `stall_if_full=true`.
8. Wait until all pages required by `C_L` are covered, install them into the
   host scoring window, perform full-precision MIPS reranking, and return top-k.

The current path has an explicit `wait_covering` materialization barrier. The
paper may claim **no demand-triggered Flash fault during full-precision
scoring** when `from_win=100%` and `from_bounce=0`; it may not claim zero
critical-path Flash wait or full query hide unless `crit_wait_ns` proves it.

## 4. What is and is not the contribution

### Core contribution

**Semantic request shaping for Flash-backed CXL memory.** Search metadata is
used to postpone full-precision data movement until the candidate set is
committed. The runtime then turns a pointer-dependent sequence of speculative
Flash requests into one precise, page-aware materialization wave.

The novelty claim is the combination of:

- an explicit search-semantic commit point;
- zero full-precision Flash reads during PQ navigation;
- exact candidate-to-page translation after commitment;
- bounded extent formation based on page density;
- a residency gate before exact scoring; and
- explicit accounting of host metadata, CXL-cache state, Flash bytes, useful
  pages, useful vector slots, and materialization wait.

### Prior-art components that must not be claimed as novel

- PQ/ADC navigation: established by DiskANN and related ANNS systems.
- Exact reranking after approximate scoring: established practice.
- Asynchronous SSD reads and cross-query I/O: established by DiskANN/PipeANN.
- Generic remote-memory or CXL prefetch: established by Leap and Cache-in-Hand.
- Page reordering/co-location: established by disk-resident ANNS layouts.

### Secondary implementation optimizations

- 64-byte rather than 32-byte PQ codes;
- ID-to-slot mapping and the 2-in-4K physical pairing;
- extent-run hole fill;
- navigation entry pinning; and
- bounce-to-window installation.

These require ablations but should not become independent contribution bullets.

## 5. Relationship to the closest systems

| System | Search information | Full-precision payload path | Primary overlap | FlashANNS distinction |
|---|---|---|---|---|
| DiskANN | DRAM PQ codes; SSD graph records | SSD reads during traversal | batched `aio` beam | FlashANNS keeps adjacency in host and delays vector payload until candidate commitment |
| PipeANN | PQ plus SSD graph pipeline | pipelined SSD records | intra-query and cross-query pipeline | FlashANNS removes FP-vector I/O from traversal and creates one committed payload wave |
| CXL-ANNS/Cosmos | corpus in CXL DRAM | no NAND miss | cache/offload | FlashANNS manages NAND-backed payload under a bounded cache |
| Cache-in-Hand | generic CXL-expander prefetch | device-policy driven | generic access prediction | FlashANNS uses an ANNS candidate-commit boundary |
| FaTRQ/Second-Tier Memory | quantization for far-memory capacity | memory-class far tier | reduce remote bytes | FlashANNS targets a per-miss NAND fill and page-shaped transfer |

The paper must acknowledge that keeping the full adjacency graph in host DRAM
buys part of the speedup. A memory-matched comparison or a host-memory/QPS
tradeoff is required; otherwise the result is a placement trade, not a pure
prefetch result.

## 6. Continuous Batching boundary

The worktree's continuous scheduler is currently entered only for
`oneshot_fp && policy==P3`; the frozen PQ end-batch path takes a different
single-query branch. Therefore the paper must not yet say that commit-before-
fetch and Continuous Batching form one measured end-to-end path.

Two admissible outcomes exist:

1. **Integrated outcome:** add a PQ-aware continuous state machine and evaluate
   `PQ commit + end-batch materialization + continuous admission`. D1 and D2 may
   remain co-equal contributions.
2. **Non-integrated outcome:** present commit-before-fetch as the evaluated core
   mechanism and demote the current continuous scheduler to an independent
   prototype or future extension. Remove “Full = D1 + D2” claims and synergy
   ablations.

No paper wording may sit between these outcomes.

## 7. Required figure redesign

The Wise Prefetcher figure becomes a two-plane dataflow:

```
Host information plane
  G0 entry -> adjacency + PQ-64 ADC -> beam L -> COMMIT C_L
                                                  |
                                                  v
Payload plane
  IDs -> unique 4-KiB vector pages -> dense-run coalescing
      -> async Flash-backed CXL read -> residency barrier
      -> host scoring window -> FP rerank -> top-k
```

The figure must annotate:

- `0 FP Flash reads` before commit;
- full adjacency and PQ codes in host DRAM;
- CXL-cache hit versus Flash-backed miss;
- the explicit materialization barrier;
- `from_win`, page-use, slot-use, Flash B/query, and wait as observables; and
- that `pipe-drive`, speculative neighbors, and optional lookahead are absent.

## 8. Evidence rules

No frozen number is copied into Abstract or Introduction until a matched rerun
produces a complete evidence bundle. The current repository is inconsistent:
the freeze note records 110.94/116.29 QPS, while the latest overwritten logs
record 107.66/105.76 QPS and a different Oracle. The new freeze must use at
least three repetitions of each system under the same code revision, query IDs,
cache state, CPU binding, graph/PQ files, and device state.

Each accepted run must report:

- recall@10, mean, p50, p95, p99, QPS;
- host graph bytes, PQ bytes, map bytes, scoring-window bytes;
- configured and observed CXL-cache capacity;
- Flash bytes/query and unique pages/query;
- page-use and slot-use;
- `from_win`, `from_bounce`, `crit_wait_ns`, and device-fill time;
- code revision and complete command line; and
- both SSD identities and timed read counters.

## 9. Reviewer pass condition

The prefetcher story is ready for an ASPLOS submission only if a reviewer can
answer all five questions from figures or tables:

1. What information stays in host DRAM, and how much does it cost?
2. At what exact point is a vector page justified?
3. How does this differ from DiskANN/PipeANN rather than merely reuse PQ?
4. How much NAND traffic and materialization wait does each design choice save?
5. Are D1 and D2 actually executed together in the reported “Full” system?

