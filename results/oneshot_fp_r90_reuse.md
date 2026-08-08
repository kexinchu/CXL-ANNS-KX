# One-shot FP @ recall≥0.90 — cross-query reuse + pin + distance prefetch

**Target:** recall@10 ≥ 0.90. Compare **P0 (no prefetch)** vs **P1 (distance-priority vector prefetch)** with the same reuse/pin policy.

## Changes vs flush A/B

1. **Cross-query reuse:** no `--flush-window` (DramWindow residency carries across queries).
2. **Pin hot entry:** pin medoid vec+adj, all entry-subgraph adjacency, and first 256 entry vectors (cap 64 MiB; used ≈29.9 MiB). Pins survive eviction.
3. **Distance-priority prefetch (P1 only):** each expand, prefetch neighbor vectors of the closest unexpanded candidates first (`lookahead_k=8`, `neighbor_k=16`, budget 16 MiB/query).

## Setup

| Item | Value |
|------|-------|
| L / complexity | 300 / fixed-point (`--iters 0`) |
| DRAM window | 1 GiB CXL-DRAM (NUMA node1) |
| Host | ≤32 MiB (used ≈1.33 MiB) |
| nq | 100, `--shuffle-seed 42` |
| Soft-cache | `vmem_sw cache_size_gib=4` still on (affects promote latency, not hit%) |

## Results

| Policy | recall@10 | QPS | mean ms | p50 | p90 | p99 | CXL-DRAM hit% |
|--------|-----------|-----|---------|-----|-----|-----|---------------|
| **P0** no prefetch (reuse+pin) | **0.929** | **1.33** | 750.9 | 831.0 | 1092.0 | 1182.6 | **10.47%** |
| **P1** dist-priority prefetch | **0.929** | 1.11 | 899.5 | 949.8 | 1256.5 | 1484.6 | **37.44%** |

Raw: `results/oneshot_fp_r90_reuse_ab.csv`

### vs prior flush-window A/B (same L=300, nq=100)

| | P0 hit% | P1 hit% | P0 QPS | P1 QPS |
|--|---------|---------|--------|--------|
| Flush (cold window each query) | 4.97% | 6.38% | 6.51 | 16.47 |
| **Reuse + pin (+ dist prefetch on P1)** | **10.47%** | **37.44%** | 1.33 | 1.11 |

## Takeaways

- **Hit%:** reuse+pin roughly **2×** P0 hit vs flush; distance-priority P1 reaches **~37%** (≈**6×** flush P1).
- **Recall unchanged** (0.929) — prefetch/pin do not change the search path.
- **QPS:** this run is slower overall (`avg_promote_ns≈81–88 µs`, closer to cold SSD); sync P1 prefetch adds work and **does not win QPS** here despite higher hit%. Prior flush run’s high QPS was partly soft-cache-warm promotes (~3–15 µs).
- Pin footprint: **31.3 MiB / 64 MiB** cap.
