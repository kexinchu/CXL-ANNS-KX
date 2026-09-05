# FlashANNS Integrated Evaluation Design

**Status:** Frozen for execution (Oracle excluded)

**Date:** 2026-09-04

**Last updated:** 2026-09-05

**Scope:** Evaluate the frozen Wise Prefetcher plus continuous pipeline on
T2I-10M, YFCC-10M, and LAION-10M, then replace the current paper's Q2--Q4
figure blocks with validated measurements.

## 1. Relationship to Existing Plans

This specification replaces the T=1-only contract in
`docs/superpowers/plans/2026-09-04-flashanns-prefetcher-evaluation.md`.
Continuous scheduling and the pipelined fill path are no longer future work:
they are part of the frozen system under test.

The three-dataset scope is deliberately limited to 10M vectors per dataset:

- T2I-10M;
- YFCC-10M;
- the first verified 10M vectors of the local LAION source.

LAION-100M is not part of this execution. It may be added later as a separate
scalability study without invalidating the three 10M results.

This specification controls the Setup text and Q2--Q4 figures in
`paper/sections/eval.tex`. Q1's motivation figures and Q5's limitations prose
remain outside the measurement rewrite except for terminology or platform facts
that must be synchronized with the validated configuration.

## 2. Frozen Implementation Contract

The source baseline is commit `15e6632`, which freezes the integrated path as:

```text
host PQ-64 navigation
  -> commit the bounded full-precision rerank candidate set C_L
  -> translate candidate IDs to missing vector pages
  -> issue 32-page READ_BATCH waves through one shared PageCopyPool
  -> copy completed pages into per-thread host scoring windows
  -> steal scheduler: issue held work while QD permits, otherwise fill,
     rank a covered query, or pump completions
  -> exact full-precision rerank from resident window bytes
```

The final-system settings are fixed:

```text
threads = 8
per-thread host scoring window = 128 MiB
pipeline depth = 2
issue_qd = 0, meaning effective QD equals threads
stagger = 0 us
shared PageCopyPool = enabled
steal scheduler = enabled
direct install = disabled
score source = host scoring window
PQ code width = 64 bytes
final k = 10
query shuffle seed = 42
```

The page-formation, I/O, completion, pipeline, and scheduling behavior is
frozen. The evaluation may add observation-only tracing, sidecars, counters,
runners, validators, plotting code, and one dataset-semantic distance adapter:

- T2I and LAION use MIPS;
- YFCC uses its official Euclidean/L2 metric;
- the runtime accepts only `mips` or `l2` and applies that choice consistently
  to PQ lookup-table construction and final full-precision reranking.

The metric adapter must be covered by exact synthetic ranking tests and must
not change page formation, request ordering, completion scheduling, scoring
residency, or returned-ID mapping. Removed controls such as early-CL,
lookahead, Blind, admit-gap, pipe-drive, score-page, and speculative beam
expansion must not be reintroduced as evaluation variants.

Any runtime change must pass a source-diff audit against `15e6632`: only
observation hooks and the approved distance dispatch may touch frozen
functions. Candidate-list management and every page/scheduler statement remain
byte-for-byte unchanged.

## 3. Memory and Resource Contract

All measured FlashANNS and demand runs use a CXL-side page-cache limit of
exactly:

```text
4 GiB = 4,294,967,296 bytes
```

This page cache is distinct from the per-thread 128 MiB host scoring windows.
With eight workers, the declared scoring-window budget is 1 GiB in aggregate.
The shared copy pool, host graph, PQ codes, query buffers, and process peak RSS
are recorded separately rather than hidden inside either cache number.

The older freeze note's instruction not to raise the 10M cache to 4 GiB is
superseded for this evaluation by the approved resource contract above. Its
historical 100 MiB performance rows remain historical evidence and cannot be
mixed with or used as repetitions for the new 4 GiB results.

Before each live run, preflight must validate:

- the configured cache limit is exactly 4 GiB;
- cache backend and physical identity;
- backing NVMe device identities and BDFs;
- dataset image offset, length, magic, and sampled content digest;
- zero I/O errors and zero dirty bytes;
- no unexpected process holding the device;
- the required cold or warm cache state;
- CPU affinity, NUMA policy, and available host memory.

Changing the live cache limit, loading or unloading the driver, switching a
backing device, staging an image, or resetting the cache is not performed by
the measurement runner. Each is a separate explicitly approved operation.

If the active backend is the software `vmem_sw` proxy backed by host memory,
the paper calls it a software CXL-side page-cache proxy. A physical CXL-DRAM
claim requires a separate DAX/BAR/device identity record proving that physical
backend for the measured run.

## 4. Dataset Contract and Admission Order

Each dataset retains its native dimension, dtype, distance metric, and record
stride. No vector may be truncated, projected, or reinterpreted to fit the
T2I layout.

### T2I-10M

T2I is admitted first using the existing 10M MIPS corpus, graph, PQ-64
artifacts, query set, recall@10 ground truth, packed extent image, entry point,
ID map, and ID-to-slot map. The existing artifacts are still rehashed and
sample-read before use; historical logs do not substitute for admission.

### YFCC-10M

YFCC uses the local 10M x 192 `uint8` base set, official public queries, and
ground truth under its native L2 contract. It is not ready for live execution
until the exact 10k query subset, graph, PQ-64 files, packed image, entry point,
ID map, and slot map have been built and independently verified.

The packed representation may widen `uint8` coordinates to exactly
value-preserving `float32` if required by the index builder. Admission must
prove coordinate equality on deterministic samples and must prove that exact
top-k ordering under the runtime L2 kernel matches direct L2 on the native
bytes. It must not normalize, project, or use a MIPS reduction.

### LAION-10M

LAION uses a deterministic first-10M subset of the local 25M x 512 `float32`
source. It is not ready for live execution until the subset identity, metric,
10k query IDs, recall@10 ground truth, graph, PQ-64 files, packed image, entry
point, ID map, and slot map have been built and independently verified.

For every dataset, admission requires:

1. exact header, count, dimension, dtype, metric, and file-size checks;
2. SHA-256 identity for every immutable input;
3. 10,000 unique query IDs and aligned ground-truth rows;
4. slot-map permutation and ID-map bounds checks;
5. deterministic readback of 1,024 packed vector and neighbor records;
6. a full host-versus-device image identity proof immediately after staging;
7. after every cold module reload, restoration and digest verification of only
   the volatile RAM-tier stripes from the admitted host extent image;
8. proof that RAM-tier restoration leaves the SSD-backed page cache empty
   (`cache_used=0`) and does not change the SSD-tier image;
9. a dataset-specific live preflight with the 4 GiB cache contract.

The accepted full-image identity record may be reused at a measured-run
preflight only when the device identity, module layout, dataset extent, and
immutable host-image hash still match. A full device scan immediately before a
cold measurement is forbidden because it populates the SSD-backed page cache.
The cold proof instead combines that accepted identity record with the fresh
RAM-stripe restoration digest and current `cache_used=0`, `dirty_bytes=0`, and
device-identity evidence. The artifact named `oracle_image` is retained only as
a host-side vector/record correctness reference; `extent_image` is the staged
byte-identity reference. Neither makes Oracle an executable system.

Missing upstream artifacts block only that dataset. They are reported as a
readiness failure and are never replaced with invented paths or synthetic
measurements.

## 5. Systems and Causal Comparisons

### Q2 core systems

The main result compares these systems at the same dataset, queries, final
`k`, CPU-core budget, and declared memory budget:

| ID | System | Purpose |
|---|---|---|
| `demand` | same PQ search and committed candidate set, with blocking CXL-SSD materialization | NAND-critical-path baseline |
| `pipeann` | official block-I/O path on the same machine and recall floor | external block-SSD reference |
| `flashanns` | frozen T=8 Wise Prefetcher plus steal pipeline | proposed complete system |

The two internal systems (`demand` and `flashanns`) share the graph, PQ
artifacts, `L`, and candidate set and must prove query IDs, candidate offsets,
candidate IDs, returned IDs, and recomputed recall are identical. PipeANN is
matched by query IDs, corpus, metric, `k`, CPU budget, and measured recall, but
uses its native index/search controls and is not claimed to have identical
internal candidates.

Throughput comparisons use the same eight-core budget. Demand may block on NAND
but must not receive a larger cache or a different query order. Oracle is
excluded from executable configurations, smoke proof, calibration, Q2,
acceptance, aggregation, and plots. `--oracle-dram` is a removed flag and any
Oracle run record is rejected.

### Q3 frozen ablations

Q3 has two causal groups so thread count does not confound the mechanism:

1. **T=1 transfer formation:** serialized committed transfer, batched
   committed transfer without extent expansion, and batched plus extent-aware
   transfer. Candidate IDs and returned IDs must be byte-identical.
2. **T=8 scheduling:** the frozen transfer path with the supported non-steal
   scheduling control versus the final steal scheduler with pipeline depth 2
   and QD=8. Both receive the same eight workers, query stream, cache, and
   candidate sets.

The plan must map these semantic controls to flags that still exist at
`15e6632` and add a dry-run test rejecting removed flags. If the non-steal path
cannot execute the same candidates and page formation, it is labeled a
different implementation and is excluded from the causal speedup.

### Q4 cold and warm states

Q4 uses only the full `flashanns` configuration. A true-cold run starts from a
validated empty 4 GiB page cache and empty scoring windows. Its immediate warm
partner follows without reset, uses the identical query order, and cites the
cold run ID. A warm run without such a parent is invalid.

## 6. Recall Calibration and Repetition

The common cross-dataset anchor is recall@10 >= 0.90. T2I additionally reports
the nearest measured point at or above recall@10 0.92.

For each dataset, use fixed query IDs and sweep:

```text
L = 50, 100, 200, 400, 800, 1600
```

For both internal systems, the per-query expansion budget is explicitly
bounded as `iters=L`. Demand and FlashANNS therefore receive the same beam and
expansion limits at every matched point. Historical `iters=0` calibration rows
are diagnostic only and must not be combined with the bounded sweep.

The predeclared extension `L = 2400, 3200` is allowed only if the base sweep
does not reach an anchor. The selected `L` is frozen by the validator before
formal runs. Plotting code may not choose or interpolate a different operating
point.

Run sizes are:

- offline and command-expansion tests: no device execution;
- correctness smoke: 100 fixed queries, one run per system/ablation;
- recall calibration: 500 fixed queries per `L`;
- final Q2, Q3, and Q4 points: 10,000 fixed queries and five independent
  repetitions;
- each Q4 repetition is one cold run followed immediately by its warm partner.

System order is randomized within matched blocks using recorded seed 20260904.
Negative or neutral results remain in the sealed records.

## 7. Sequential Execution

Work proceeds in this order:

```text
shared schemas, instrumentation, runner, validator, and plotting tests
  -> T2I admission and full Q2/Q3/Q4 vertical slice
  -> T2I provisional figures and review gate
  -> YFCC admission and full Q2/Q3/Q4 vertical slice
  -> YFCC provisional figures and review gate
  -> LAION admission and full Q2/Q3/Q4 vertical slice
  -> LAION provisional figures and review gate
  -> three-dataset aggregation, paper replacement, and final audit
```

Within each dataset, the sequence is:

```text
artifact admission
  -> offline identity and format tests
  -> 100-query candidate/result proof
  -> recall calibration
  -> Q2 five-repeat main matrix
  -> Q3 five-repeat ablations
  -> Q4 five paired cold/warm repetitions
  -> validation and sealing
  -> provisional plot
```

A failed stage stops that dataset before the next stage. Valid sealed results
from an earlier dataset are not rerun after a later dataset is unblocked unless
the binary, schema, cache contract, system flags, or aggregation semantics
change.

## 8. Measurement Record

Every invocation writes an immutable `run.json` plus binary sidecars for query
IDs, per-query latency, candidate IDs with offsets, and returned IDs. The run
record includes:

- git commit, dirty patch hash, binary hash, command, and environment;
- dataset and artifact-manifest hashes;
- system/ablation ID, `L`, `k`, query count, repeat, and seeds;
- cache/backend/device preflight before and after;
- cold-parent identity for warm runs;
- QPS and mean, p50, p95, p99, and maximum latency;
- recall recomputed from sidecars;
- NAND reads, commands, bytes, useful bandwidth, and busy time;
- requested, deduplicated, extent-added, useful, evicted, and late pages;
- page precision, vector-slot utilization, and extent utilization;
- materialization critical wait, copy time, and in-flight-depth histogram;
- score-source counts from host window, cache, bounce, and direct Flash;
- CPU utilization, peak RSS, graph/PQ bytes, shared-pool bytes, host scoring
  windows, and 4 GiB CXL-side page cache.

Stdout is diagnostic evidence only. Validators recompute sidecar lengths,
hashes, recall, percentiles, and matched-block identity before accepting a run.

## 9. Figure Contracts

### Q2: end-to-end iso-recall and hide proof

Replace `fig:eval-main` with one double-column 2-by-2 figure:

- **(a) QPS:** grouped absolute-QPS marks for demand, PipeANN, and FlashANNS
  across T2I, YFCC, and LAION.
- **(b) Latency:** aligned mean and p99 markers in milliseconds for the same
  validated runs.
- **(c) Critical wait:** materialization critical-wait milliseconds per query;
  no QPS axis is overlaid.
- **(d) Score source:** stacked percentages from host window, CXL-side cache,
  bounce, and score-triggered Flash. Non-applicable PipeANN categories are
  explicitly marked rather than treated as zero.

All performance bars use the frozen recall anchors and five-run medians with
95% bootstrap confidence intervals. The figure uses no twin y-axis. Table
`tab:eval-main` is regenerated from the same validated CSV and contains run IDs
through its provenance record.

### Q3: Wise Prefetcher and scheduling ablation

Replace `fig:eval-ablate` with two aligned panels:

- **(a) T=1 transfer formation:** serialized, batched, and extent-aware
  committed transfer. Show normalized QPS plus separate traffic/utilization
  marks for NAND MiB/query, commands/query, vector-slot utilization, and extent
  utilization.
- **(b) T=8 scheduling:** supported non-steal control versus final steal
  pipeline. Show absolute QPS and p99; a compact inset reports useful device
  bandwidth and mean in-flight fill depth.

The figure never labels removed speculative policies as live ablations. A
variant that changes candidates or recall is visibly rejected and cannot enter
the normalized bars.

### Q4: true-cold versus immediate-warm

Replace `fig:eval-scale` with three dataset groups. Each group contains paired
cold/warm marks for:

- QPS;
- p99 latency;
- critical wait milliseconds per query;
- cache hits and Flash fills per query.

The top-level page-cache limit remains 4 GiB for both states. The figure tests
state, not cache capacity. If only the warm pass hides NAND, the caption and
paper call the result service hide and remove any true-cold claim.

All three figures are generated from accepted JSON records through one
validated CSV and one provenance map. No historical console value or manually
entered performance number may appear in a final plot.

## 10. Fail-Closed Gates

A run is rejected if any of the following holds:

- artifact, binary, command, dataset, query, or cache identity differs within
  a matched block;
- the live cache limit is not exactly 4 GiB;
- dirty bytes, I/O errors, reset errors, open users, or topology drift appear;
- a cold run lacks empty-cache evidence or a warm run lacks its cold parent;
- more than one internal cold run is expanded in one live invocation, a cold
  run lacks a fresh UUID-tagged RAM-restoration capture, or that capture has
  already been atomically claimed by another run;
- fewer than the declared queries complete;
- sidecar sizes, hashes, result IDs, candidate IDs, recall, or latency
  recomputation disagree;
- an executable system, command, accepted run, aggregate row, or plot series
  contains Oracle or `--oracle-dram`;
- FlashANNS scores from bounce or triggers Flash from an exact-scoring load;
- the candidate set changes between same-search transfer or scheduling
  controls;
- the configured metric differs from the dataset manifest or PQ/final scoring
  use different metrics;
- CPU affinity, frequency, memory availability, or throttling violates the
  resource contract.

Rejected runs remain immutable with machine-readable reasons and are never
silently overwritten or retried under the same run ID.

Calibration is executed one `(system, L)` point at a time. Every internal
point consumes a separately reloaded cold state and a newly generated volatile
RAM-restoration `capture_id`; phase-wide evidence reuse is forbidden. Anchor
freezing reads validated `run.json` records recursively and selects the nearest
measured point at or above each declared recall target without extrapolation.

## 11. Completion Criteria

The work is complete only when:

1. shared offline tests, exact MIPS/L2 ranking tests, and negative
   schema/preflight tests pass;
2. T2I, YFCC, and LAION artifact manifests and live-image identities pass;
3. every dataset passes its 100-query candidate/result proof;
4. all recall anchors are selected from measured calibration points;
5. every Q2 and Q3 block has five accepted 10k-query repetitions;
6. every Q4 block has five accepted cold/warm pairs;
7. FlashANNS has zero bounce scores and no score-triggered Flash fills;
8. Oracle is absent from executable configs, accepted records, aggregates, and
   final figures;
9. all three final figures and tables regenerate byte-for-byte from validated
   records and expose their run-ID provenance;
10. `paper/sections/eval.tex` declares the 4 GiB cache, three 10M datasets,
    physical-versus-proxy backend truth, frozen T=8 system, negative results,
    and the exact limits of the claims.
