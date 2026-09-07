# Demand Original-Layout Baseline Design

**Date:** 2026-09-07

## Purpose

Add a paper-grade baseline that removes every FlashANNS optimization from the
Demand path, including the physical graph/vector layout. This baseline closes
an attribution gap in Figure 8: the existing `demand` system disables software
prefetching, extent-run coalescing, pipelining, and cross-query scheduling, but
still reads the reordered extent image through an ID-to-slot map.

The internal system identifier is `demand-orc`. In the paper and figure legend,
it is named **Demand (original layout)**. Here, `orc` means original logical-ID
order; it does not mean that the index resides in DRAM or that an Oracle system
is being evaluated.

## Baseline Contract

`demand-orc` uses the same host graph, PQ-64 navigation, queries, ground truth,
search parameters, host-memory window, CXL-DRAM cache, CXL-SSD devices, and
measurement protocol as the existing Figure 8 systems. It differs only by
removing FlashANNS mechanisms:

- Stage the dataset's `oracle_image`, whose records remain in logical-ID order,
  into the CXL-SSD-backed `/dev/vmem0` aperture.
- Do not load or apply an ID-to-slot map.
- Disable VMEM prefetching and extent-run request formation.
- Use one in-flight query slot per worker and disable continuous-batching
  pipeline overlap.
- Disable cross-query work stealing and admission optimizations.

Vectors and graph records are therefore demand-fetched from CXL-SSD through the
same CXL-DRAM cache hierarchy as the other systems. Host-side PQ navigation is
unchanged, so layout and serving mechanisms—not the search algorithm—are the
controlled variables.

## Runner and Evidence Design

Layout selection is an explicit system property, not an implicit file rename.
For an original-layout system, staging and preflight select `oracle_image`, the
search command omits `--id-slot-map`, and the sealed record carries the selected
layout. Existing systems continue to select `extent_image`; their commands and
evidence identities remain unchanged.

The runner must fail closed when any of the following occurs:

- an original-layout run contains an ID-to-slot map;
- the staged image identity does not match `oracle_image`;
- an optimized-layout run is accidentally pointed at `oracle_image`;
- the record omits layout, image, binary, cache, or device identity;
- the run fails correctness, completeness, metric, or cold-cache validation.

Only records with `validation.status=accepted` may enter aggregation or Figure
8. Rejected, interrupted, raw, or stale records remain visible for diagnosis
but cannot be promoted to paper evidence.

## Measurement Matrix

Run `demand-orc` on T2I-10M, YFCC-10M, and LAION-10M with:

- `T=8` workers;
- `L={50,100,200,400,800,1600}`;
- 10,000 queries per run;
- five independent cold-cache repetitions per point.

This produces 90 accepted measurements: three datasets by six search widths by
five repetitions. Each run records Recall@10, mean/P50/P95/P99 latency,
throughput, NAND traffic, cache traffic, page/extent utilization, completion
count, and the full run identity. Recall is expected to match the corresponding
search configuration; performance values are not assumed in advance.

Before the full matrix, one smoke point per dataset verifies that original-order
records return valid candidates and that no slot-map path is active. The smoke
records do not count toward the 90 paper measurements.

## Device-State Protocol

All datasets share the configured `/dev/vmem0` staging range. For each dataset:

1. Verify the live VMEM device, backing-device identity, cache geometry, source
   image size, destination range, and absence of outstanding writes.
2. Stage the original-order image and verify its content identity before runs.
3. Apply the same cold-cache reset and validation protocol before every repeat.
4. Seal accepted evidence before moving to the next dataset.

Because staging overwrites the shared range, the campaign records the image
present before it begins. After all measurements—or after a controlled abort—it
restores the appropriate extent image and verifies the restored identity. A
failed restore is reported explicitly and is never treated as experimental
completion.

## Figure 8 Integration

Each dataset frontier in Figure 8 contains four systems:

1. **Demand (original layout)** — new `demand-orc` evidence;
2. **Demand (optimized layout)** — existing `demand` evidence;
3. **PipeANN**;
4. **FlashANNS**.

The two Demand variants use distinct neutral colors and redundant marker/line
styles so the layout contribution is readable in print. The caption states
that both Demand variants disable FlashANNS's runtime mechanisms, while only
the optimized-layout variant retains extent placement. Existing accepted
curves are not rerun or relabeled.

## Verification and Exit Criteria

- Unit tests cover layout selection, command construction, staging/preflight
  identity, fail-closed validation, aggregation, and four-series plotting.
- All three smoke points pass before the full campaign starts.
- Exactly 90 new paper records are accepted, with five run IDs behind every
  plotted `demand-orc` point.
- Aggregation rejects mixed layouts, missing repetitions, and non-accepted
  records.
- The regenerated vector PDFs and manuscript build contain no clipping,
  undefined references, or stale three-series Figure 8 caption.
- `/dev/vmem0` is restored and verified after the campaign.

