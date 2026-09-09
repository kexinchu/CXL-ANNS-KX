# FlashANNS Eight-Figure Evaluation Design

**Date:** 2026-09-07

## Scope

Retain the six accepted-evidence figures and add exactly two figures that close
the strongest remaining reviewer questions: whether Continuous Batching remains
stable as offered load approaches saturation, and whether the Wise Prefetcher
reduces physical Flash work rather than merely moving it off the critical path.
The change does not alter raw runs, section order, recall targets, or the frozen
implementation.

## Evidence Contract

- Every plotted mark is the median of exactly five sealed `run.json` records
  whose `validation.status` is `accepted`.
- The six existing figures continue to consume
  `results/eval/flashanns/provisional/non-q3-load-all-three/validated.csv`.
- The load figure uses only the frozen load contracts below; similarly named
  older campaigns are rejected rather than merged:
  - T2I-10M: campaign `68c6`, rates 73.835, 147.670, 221.505, 295.340,
    and 369.175 QPS;
  - YFCC-10M: campaign `4ad7-c3ef`, rates 733.303, 1466.605, 2199.908,
    2933.210, and 3666.512 QPS;
  - LAION-10M: campaign `4ad7-c3ef`, rates 101.502, 203.005, 304.507,
    406.010, and 507.512 QPS.
- The I/O-efficiency figure uses T2I-10M at $L=400$: the five accepted
  original-layout Demand runs from `33ba-orc1`, the optimized-layout Demand
  runs, and the three five-run Wise stages from `4ad7-c3ef`. Candidate/query
  identity and recall must match. The layout pair uses T=8; the three
  prefetcher stages use T=1.
- `extent_use_pct` and `scored_vec_B` are not evidence: both are zero in the
  frozen records. Physical read amplification is instead computed from NVMe
  read bytes divided by useful exact-vector payload,
  `slots_scored * dimension * element_bytes`.
- Host-window requests are labeled as such; they are never relabeled NAND
  misses. `nand_read_bytes` and `nand_read_commands` are the physical device
  counters used for Flash claims.

## Added Figure Contracts

### Load stability

`load-tail-latency.pdf` is a three-panel service curve, one panel per dataset.
The x-axis is achieved throughput and the y-axis is P99 end-to-end latency,
including queueing. Curves compare Wise-only, PipeANN, and FlashANNS at the five
frozen offered rates. Points must also retain completion count, offered rate,
mean/P50/P95/P99, queue wait,
and recall in the companion CSV so claims can be audited without rerunning.

The figure answers one question: does completion-driven scheduling sustain
throughput without an uncontrolled tail-latency cliff? It is not folded into
the closed-loop concurrency sweep, because concurrency scaling and service
stability are distinct experiments.

### Physical I/O efficiency

`prefetch-io-efficiency.pdf` is a compact T2I-10M causal-stage figure. The
stages are Original layout, Co-use layout, Serialized, Post-commit batch, and
Extent reads. The plot visibly separates the T=8 layout pair from the T=1
prefetcher stages.
Panel (a) reports host-window page requests/query and physical NAND MiB/query.
Panel (b) reports vector-slot utilization and physical read amplification.
Panel (c) reports NAND read commands/query and average KiB per device read. Mixed units use
separate axes or normalized values with explicit labels; no zero-valued extent
counter is plotted.

The figure answers how Wise Prefetching makes transferred data useful: co-use
placement raises the fraction of fetched slots that exact scoring consumes,
post-commit selection avoids premature work, and extent formation reduces
physical commands while increasing bytes per command. Throughput is excluded
from this causal stage plot because the original-layout evidence uses a
different concurrency setting. It complements, rather than duplicates, the
existing when/what/how ablation.

## Existing Figure Contract

The retained six PDFs are `frontier-t2i.pdf`, `frontier-yfcc.pdf`,
`frontier-laion.pdf`, `wise-ablation.pdf`, `batching-ablation.pdf`, and
`hide-robustness.pdf`. The Wise plot must no longer display or claim the invalid
zero-valued extent-utilization counter.

All eight files are Matplotlib vector PDFs with embedded text, 12-point base
font, white background, a color-blind-safe palette, and redundant marker or
line-style encodings.

## Paper Placement

- Place `prefetch-io-efficiency.pdf` beside the existing Wise Prefetcher
  discussion in Q3.
- Place `load-tail-latency.pdf` after the closed-loop concurrency sweep in Q3.
- Keep Q1--Q5 and every subsection in its current order.
- Add only evidence-supported prose; do not restore Oracle comparisons or
  expand the Evaluation with another research question.

## Completion Gates

1. The curated supplemental CSV contains 45 load marks backed by 225 unique
   accepted runs and five I/O marks backed by 25 accepted runs.
2. Each contract rejects missing repeats, wrong campaign/rate, identity drift,
   incomplete queries, or missing physical counters.
3. All eight PDFs are one-page vector output and have editable companion PNGs.
4. `paper/main.pdf` builds without undefined references; figure captions use
   the exact metric semantics above.
