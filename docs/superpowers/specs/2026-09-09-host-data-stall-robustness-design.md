# Host Data-Dependency Stall Robustness Design

## Objective

Replace Figure 10(a)'s incomplete `critical_wait_ms` proxy with a directly
instrumented host-side stall metric, while preserving the matched T=8
Demand-versus-FlashANNS comparison and the remaining robustness panels.

## Metric contract

`host_data_stall_ns` is wall-clock time during which a host worker cannot make
progress on the next search step because required vector data are unavailable
and no other query is runnable. It is the sum of two non-overlapping counters:

- `coverage_wait_ns`: required pages have been submitted but are not yet
  available for reranking or result handling.
- `slot_backpressure_wait_ns`: the next required I/O cannot be submitted
  because the fill/admission capacity is full.

Background fill lifetime, useful compute/I/O overlap, arrival pacing, static
cohort barriers, and post-result cleanup are excluded. `device_fill_ns` and
`crit_wait_ns` remain compatibility diagnostics and are not figure inputs.

For blocking execution, `wait_covering()` records its wall time once and
subtracts any nested slot-backpressure interval. A required fallback
`wait_all()` is coverage wait; final cleanup is not. For continuous scheduling,
the interval begins only when the scheduler has no Fill, Rank, or Issue work;
it ends at the next scheduling decision that can make progress.

## Score-source contract

Each run must emit, rather than infer, `score_triggered_flash_fills` and
`score_prematerialized_pct`. FlashANNS is admissible only when the former is
zero and the latter is 100%. These fields audit the implementation contract;
they are not presented as a speedup source.

## Experiment contract

Run T2I-10M, YFCC-10M, and LAION-10M with their frozen recall anchors, identical
query order (`seed=42`), cold reset, T=8, and matched Demand/FlashANNS settings.
Collect five repetitions per pair. Every accepted record must contain the two
stall components, their exact sum, QPS, P99, recall, NAND bytes/commands,
score-source counters, command line, binary SHA-256, and dataset identity.

Figure (a) is a paired stacked bar chart: dataset groups on x, host data-
dependency stall (ms/query) on y, system by bar color and stall component by
fill/hatch. Figure (b) remains end-to-end QPS from the same new runs. Existing
panels (c) and (d) remain unchanged. Negative or weak stall reductions are
retained and investigated, not filtered.

## Acceptance gates

1. Unit tests prove non-overlapping accounting and parser/aggregation behavior.
2. A paired T2I smoke run completes with matched candidates/recall.
3. Every formal run satisfies `host_data_stall_ns = coverage_wait_ns +
   slot_backpressure_wait_ns` and the score-source contract.
4. Only five-run accepted medians enter the PDF; old q4 records stay immutable.

