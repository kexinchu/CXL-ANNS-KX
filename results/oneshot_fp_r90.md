# One-shot FP MIPS + neighbor vector-page prefetch (wiki-cohere 25M)

**Target:** recall@10 ≥ 0.90, no re-rank. Report recall / latency / QPS / CXL-DRAM hit%.

## Setup

| Item | Value |
|------|-------|
| Corpus | Wikipedia-Cohere 25M × 768 float32, DiskANN R=32 (MIPS) |
| Placement | CXL-SSD `/dev/vmem0` @ 200 GiB; CXL-DRAM window **1 GiB**; host ≤**32 MiB** |
| Search | One-shot FP MIPS greedy (DiskANN fixed-point); `--iters 0` |
| GT | DiskANN MIPS L=800 top-10 (`gt_10k_diskann_k10.ibin`), held-out 10k queries |
| Fairness | `--flush-window --shuffle-seed 42`, nq=100 |
| P0 | Demand promote only |
| P1 | Sync prefetch of neighbor **vector** pages (budget 16 MiB/query, neighbor_k=16) |

**Bugfix:** DramWindow previously mapped only one 4 KB page; 3072‑byte vectors often span two pages → corrupt scores / recall ~0.45. Fixed with multi-page promote + spill copy.

## L ladder (P0, no flush, nq=50) — pick operating point

| L | recall@10 | QPS | mean ms |
|---|-----------|-----|---------|
| 100 | 0.838 | 3.40 | 294 |
| 150 | 0.880 | 3.51 | 285 |
| **200** | **0.906** | 2.45 | 409 |
| **300** | **0.924** | 1.36 | 737 |
| 400 | 0.952 | 1.03 | 971 |

Operating point for ≥0.90 under flush: **L=300** (L=200 is borderline ~0.897 with flush).

## A/B @ L=300 (flush-window, nq=100, DRAM=1 GiB)

| Policy | recall@10 | QPS | mean ms | p50 | p90 | p99 | CXL-DRAM hit% |
|--------|-----------|-----|---------|-----|-----|-----|---------------|
| **P0** no prefetch | **0.929** | 6.51 | 153.2 | 66.4 | 314.3 | 354.4 | **4.97%** |
| **P1** vec-page prefetch | **0.929** | 16.47 | 60.4 | 62.5 | 76.6 | 87.3 | **6.38%** |

Raw CSV: `results/oneshot_fp_r90_ab.csv`

### L=200 (same setup; recall just under target)

| Policy | recall@10 | QPS | mean ms | hit% |
|--------|-----------|-----|---------|------|
| P0 | 0.897 | 3.40 | 293.8 | 4.62% |
| P1 | 0.897 | 24.30 | 40.9 | 5.70% |

## Notes

- Prefetch does **not** change recall (same expansions / distance comps); it only changes residency / promote path.
- Hit% gain is modest (~+1.4 pp). Large QPS/latency wins partly reflect `vmem_sw` soft-cache (4 GiB) warming from extra prefetch promotes (`avg_promote_ns` drops from ~15 µs → ~3.5 µs).
- Host used ≈1.4 MiB (entry + 100 queries + GT) ≪ 32 MiB.
