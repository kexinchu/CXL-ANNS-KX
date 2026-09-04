# FlashANNS Prefetcher-First Evaluation Design

**Status:** Approved for implementation-plan drafting

**Date:** 2026-09-04

**Scope:** Evaluate the frozen PQ-64 end-batch prefetcher independently of
Continuous Batching, first on T2I-10M and then on LAION-10M and YFCC-10M.

## Relationship to the Overall Evaluation

This specification is a scoped correction to
`docs/superpowers/specs/2026-09-03-flashanns-evaluation-design.md` and
`docs/superpowers/plans/2026-09-03-flashanns-evaluation.md`.

It preserves their dataset identity, run-record, repetition, cold-state,
recall, and provenance requirements. It replaces only the old Q4 / Figure 9 /
Task 6 definition. That definition assumed live mandatory, optional-lookahead,
and equal-byte Blind policies plus an `M={0,1,2,4,8,16}` sweep. Those policies
do not exist in the frozen PQ-64 path: `--lookahead-k`, `--spec-beam-nbrs`, and
`--score-page` are dropped or no-op controls, and the frozen path issues one
rerank wave only after the host PQ beam terminates.

Tasks 1--3 of the overall plan remain prerequisites, but they may be delivered
as a prefetcher-only vertical slice. The T=1 recall calibration portion of Task
4 is also required. Throughput-concurrency calibration, main continuous-mode
runs, and Task 7 remain outside this specification.

## Frozen Implementation Contract

The evaluated implementation is the committed PQ-64 end-batch path represented
by commit `a9f8447` and the following execution flow:

```text
G0 10k navigation -> entry
  -> host PQ-64 beam with L candidates and host neighbor lists
  -> collect nonresident full-precision vector pages for final candidates
  -> issue one HidePipe rerank wave
  -> copy bounce pages into DramWindow
  -> wait until every required page is resident
  -> full-precision MIPS rerank from DramWindow -> top 10
```

The prefetcher core is frozen. Evaluation work must not change
`search_one_pq`, `hide_fill.hpp`, the PQ-64 codebooks, the ID-to-slot map, or
the staged 1100 GiB extent image. New tests, manifests, runners, validators,
sidecars, and additive metrics are allowed only when they do not alter
candidate selection, I/O scheduling, placement, or returned IDs.

All runs in this specification use one admitted query and one search thread.
They must not pass `--cont-batch`; Continuous Batching is evaluated later from
the same validated run-record format.

## Evaluation Questions

The prefetcher-first evaluation answers five questions:

1. Does the host PQ beam perform zero CXL-SSD reads before the final rerank
   candidate set is fixed?
2. For an identical PQ-64 candidate set, how much do batched page reads improve
   throughput and tail latency over serialized transfer?
3. Does extent-aware issue reduce I/O commands or improve locality without
   changing candidates, returned IDs, or recall?
4. Are all full-precision scores computed from resident window bytes, with no
   score attributed to a bounce buffer or implicit synchronous device read?
5. Do the answers hold at matched recall on T2I-10M, LAION-10M, and YFCC-10M,
   including true-cold and immediate-warm execution?

## Claims and Non-Claims

The resulting evidence may support these claims:

- PQ-guided end-batch issue removes NAND access from the navigation dependency
  chain and concentrates required vector reads into one final wave.
- Batched transfer and extent-aware issue improve performance relative to
  same-candidate serialized and non-extent controls.
- The frozen path preserves returned IDs across transfer controls and scores
  full-precision vectors only after residency.

It must not claim:

- mandatory/optional separation, bounded frontier lookahead, or an equal-byte
  Blind policy;
- a benefit from Continuous Batching;
- physical CXL-DRAM residency when the scoring window uses NUMA host DRAM;
- that the legacy one-shot FP hop path has identical search semantics to the
  PQ-64 path;
- cross-dataset completion before all three 10M datasets pass the final gates.

## Controlled Systems

The core comparison holds the navigation graph, PQ-64 codebook, query order,
candidate limit `L`, final `k=10`, CPU binding, window size, cache size, record
layout, and returned-ID mapping fixed.

| ID | System | Transfer configuration | Role |
|---|---|---|---|
| `pq-oracle` | PQ-64 navigation and FP rerank from a fully resident corpus | No timed NAND access | Same-search upper bound |
| `pq-serial` | PQ-64 end-batch with one copy worker | `--no-vmem-prefetch --pipe-w 1 --no-extent-run` | Serialized transfer control |
| `pq-batch` | PQ-64 end-batch using `READ_BATCH` | `--pipe-w 16 --no-extent-run` | Isolate batched-read contribution |
| `pq-frozen` | PQ-64 end-batch using `READ_BATCH` and extent-aware issue | `--pipe-w 16 --extent-run` with the frozen extent layout | Final prefetcher |

Two diagnostic controls are kept outside the normalized core comparison:

- `legacy-fp-hop`: the superseded one-shot FP e4/a1 path. It demonstrates the
  serialized NAND pathology but is not a same-search speedup denominator.
- `pq32-sensitivity`: the 32-byte code path on T2I-10M only. It tests why the
  frozen contract requires PQ-64 and is not a three-dataset system.

Before `pq-serial` is accepted as the serialized control, a smoke trace must
prove that it emits no `VMEM_IOC_READ_BATCH` or `VMEM_IOC_PREFETCH_BATCH` calls
and uses a single transfer worker. If that cannot be proven without changing
the frozen core, the label becomes `mmap-single-worker`; the paper must not
call it synchronous per-page Demand.

## Dataset Rollout

### Milestone A: T2I-10M vertical slice

T2I-10M is first because its frozen PQ-64 codebook, graph, extent image, slot
map, queries, and recall@10 ground truth already have a documented recipe.
Milestone A completes the runner, schema, correctness checks, recall
calibration, five-repeat measurement, validation, and provisional Figure 9
using only T2I data.

Milestone A is not cross-dataset completion and cannot replace the final paper
figure.

### Milestone B: cross-dataset completion

LAION-10M and YFCC-10M enter the same runner only after each dataset manifest
validates native dimensions, metric, execution datatype, record stride, graph,
queries, ground truth, PQ-64 artifacts, packed layout, and deterministic
readback. No vector may be truncated or projected to fit T2I's record format.

Figure 9 and the prefetcher task are complete only after all three datasets
pass the final gates.

## Experiment Phases

### P0: identity and live-state preflight

Record the commit, dirty-tree hash, binary hash, command, dataset artifacts,
PQ artifacts, graph, ID maps, and staged-image identity. Before a live VMEM
run, fail closed unless the expected device, logical offset, image magic,
backing devices, cache limit, dirty-byte count, open-user state, and I/O-error
counter are known.

The preflight is read-only. It must not restage an image, reformat storage,
reload a module, flush dirty data, or alter the backing-device topology. Such
an operation requires a separate explicitly approved procedure.

### P1: offline contract tests

Offline tests validate:

- PQ-64 pivot/code dimensions and item counts;
- ID permutation and slot-map bounds;
- deterministic candidate IDs for a fixed query, `L`, and binary;
- requested-page deduplication and extent expansion accounting;
- separation of requested pages from extent-added pages;
- rejection of deprecated/no-op controls by the experiment manifest;
- schema rejection when identity, counter, latency, or returned-ID evidence is
  absent.

### P2: T2I 100-query proof run

Run `pq-oracle`, `pq-serial`, `pq-batch`, and `pq-frozen` on the same 100 query
IDs before any large sweep. The proof passes only if:

- PQ candidate-ID sidecars are identical across all four systems;
- final returned-ID sidecars are identical across the three transfer modes;
- recall is identical across transfer modes;
- the PQ navigation interval has zero NAND-byte and device-command deltas;
- every required rerank page becomes resident before FP scoring;
- `pq-frozen` reports zero bounce scores;
- no mandatory rerank page is dropped;
- no live-state or artifact-identity gate fails.

Any failed condition blocks scale-up; it is recorded rather than averaged away.

### P3: recall calibration

For each ready dataset, run 500 fixed queries with
`L={50,100,200,400,800,1600}`. Activate `L={2400,3200}` only when the base
sweep does not cover the common recall interval. Freeze the nearest measured
point at or above recall@10 0.90; additionally freeze T2I's nearest point at or
above 0.92.

The final plotter may not choose `L`. `pq32-sensitivity` runs only in the T2I
calibration and is retained even when it misses the recall anchor.

### P4: final T=1 measurements

For every dataset/system/selected-`L` block:

- run exactly 10,000 fixed queries;
- collect five independent measured repetitions;
- randomize system order with a recorded seed;
- reset and validate the device cache and DramWindow before the cold run;
- capture one immediate warm pass before the next reset;
- keep CPU allocation, frequency policy, NUMA placement, and resource budgets
  fixed;
- preserve every valid negative or neutral result.

Each point reports sustained QPS; mean, p50, p95, p99, and maximum latency;
recall@10; NAND bytes and commands per query; requested, deduplicated,
extent-added, useful, evicted, and late pages; batch count and pages per batch;
critical wait and copy time; score-source counts; cache/window state; and CPU
and device utilization.

### P5: cross-dataset expansion

Repeat P2--P4 without changing policy definitions when LAION and YFCC pass
their manifests. Dataset-specific record strides and absolute window sizes are
recorded, while the paper reports both GiB and corpus percentage.

### P6: validation and Figure 9

Figure 9 is redefined as:

- **Panel (a):** normalized matched-recall QPS for `pq-serial`, `pq-batch`, and
  `pq-frozen`, grouped by dataset; `pq-serial` is 1.0.
- **Panel (b):** requested pages/query, extent-added pages/query, NAND
  MiB/query, and useful-page percentage. These values are separate aligned
  marks or subpanels and must not use a dual y-axis.
- **Panel (c):** mean and p99 latency plus critical-wait contribution for the
  same points.

`pq-oracle` appears as the upper-bound marker. `legacy-fp-hop` and
`pq32-sensitivity` are reported as negative/sensitivity results in an appendix
or compact companion table, not in the normalized same-search bars.

## Run Records and Data Flow

One declarative matrix expands into immutable run directories. Each invocation
writes:

```text
run.json
latency_ns.u64
result_ids.u32
candidate_ids.u32 plus per-query offsets
device_before.json
device_after.json
stdout.log
```

`run.json` contains artifact hashes, binary identity, full command, policy ID,
dataset identity, query IDs, `L`, resource budgets, cold/warm state, counter
deltas, score sources, page classes, timings, and sidecar hashes. Validation
recomputes recall and latency percentiles from sidecars and rejects disagreement
with stdout summaries.

Validated JSON is the only input to aggregation and plotting. Historical logs
may guide smoke expectations but cannot be inserted into final figures.

## Fairness and Failure Handling

- Core speedups use only `pq-serial`, `pq-batch`, and `pq-frozen` at identical
  candidate IDs and measured recall.
- No extrapolation is allowed when a system misses the recall anchor.
- The `pq-oracle` timed interval must have zero NAND bytes.
- A point is invalid if candidate IDs or returned IDs differ across transfer
  modes, a mandatory page is dropped, a score occurs before residency, bounce
  scoring is nonzero, reset evidence is absent, hashes differ, the query count
  is incomplete, or throttling/reset/I/O errors occur.
- Invalid runs remain in the raw results with a machine-readable rejection
  reason; they are never silently filtered or retried into the same run ID.
- A NUMA-backed window is labeled host DRAM. A physical CXL-DRAM claim requires
  the DAX/BAR backend and its identity evidence.

## Completion Gates

The prefetcher-first evaluation is complete only when all of the following are
true:

1. Offline contract and schema-negative tests pass.
2. T2I, LAION, and YFCC manifests and 100-query proof runs pass.
3. Every core final point has five valid cold repetitions and paired warm runs.
4. Candidate IDs and final returned IDs match across transfer modes at equal
   dataset/query/`L` identity.
5. PQ navigation has zero NAND activity, `pq-oracle` has zero timed NAND
   activity, and `pq-frozen` has zero bounce scoring.
6. Recall and all latency statistics are independently recomputable from
   sidecars.
7. Figure 9 and its provenance map contain no hand-entered measurement.
8. The paper and overall evaluation plan no longer describe an optional
   lookahead or Blind policy that the frozen implementation does not provide.

Passing Milestone A permits T2I implementation feedback and a provisional
figure. It does not satisfy gates 2--8 for the final cross-dataset claim.
