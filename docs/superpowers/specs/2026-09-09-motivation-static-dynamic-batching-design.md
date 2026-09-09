# Motivation Figure 5: Static versus Dynamic Batching Design

## Objective

Replace the current C3 queue-depth-versus-thread plot with a T2I-10M-only,
measured comparison of a single request, fixed static batches, and dynamic
continuous batching. The figure must show both physical CXL-SSD bandwidth
utilization and delivered QPS without changing the Motivation section's C3
position or role.

## Controlled comparison

All conditions use the same T2I-10M image, `L=400`, `k=10`, PQ-64 navigation,
extent layout, Wise Prefetcher, 4 GiB VMEM page cache, 128 MiB host window per
worker where applicable, query order, graph, executable, and dual CXL-SSD
backings. Each condition has five accepted true-cold repetitions of 10,000
queries.

- **Single request:** one worker and at most one admitted query.
- **Static batching:** eight workers and the frozen pipeline, but query `8j+8`
  cannot be admitted until all queries in cohort `8j..8j+7` retire.
- **Dynamic batching:** eight workers and the same pipeline; each freed slot is
  refilled immediately from the pending stream.

The static gate changes admission only. It must not change candidate selection,
prefetch policy, graph layout, issue depth, or scoring.

## Metrics and evidence

Physical read bandwidth is `dual-backing block read bytes / timed search wall
seconds`. Utilization divides this value by the independently measured median
maximum of the accepted C3 device-capability sweep. Runtime-estimated
`nvme_real_GBps` is diagnostic only and cannot feed the figure. QPS and
Recall@10 come from the runtime log. Every accepted run must prove the same
query IDs, artifacts, binary, cache geometry, zero dirty bytes before the run,
complete 10,000-query execution, no overlapping workload, and no backing
writes or I/O errors.

The three conditions must remain within 0.001 absolute Recall@10. A run block
with fewer or more than five repetitions is not paper-facing.

## Figure contract

`paper/figs/mot-c3-io-concurrency.pdf` remains a single-column vector PDF.
It contains two zero-based bar panels with the shared order Single, Static,
Dynamic:

1. measured CXL-SSD bandwidth utilization (%);
2. throughput (QPS).

Bars show medians; whiskers show 95% bootstrap intervals. Exact median labels
appear above bars. Static is visually distinguished from Dynamic by both tone
and hatch, so the comparison survives grayscale. The caption states T2I-10M,
T=8 for the two batched modes, five cold runs, and the empirical bandwidth
denominator.

## Paper boundary

Only the C3 paragraphs, Figure 5 caption/description, and C3 row of the findings
table may change. The result motivates continuous batching; implementation
details remain in Design. No Figure 3 or Figure 4 asset or prose changes.

## Exit gates

- static cohort behavior has a deterministic unit test;
- a smoke run proves Single, Static, and Dynamic complete with matched recall;
- fifteen cold runs are accepted with five runs per condition;
- physical counters and timed wall duration resolve for every run;
- the PDF passes dimensions, embedded-font, vector-content, and visual checks;
- the paper builds without undefined references.
