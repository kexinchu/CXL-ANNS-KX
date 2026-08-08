# Prefetch A/B — 10k held-out @ CXL-SSD（完成）

**Setup**
- Corpus: Wikipedia-Cohere **25M** on `/dev/vmem0` @ 200 GiB（`ram_size_gib=28`）
- Queries: `query_10k_heldout.bin`（[25M,35M) held-out，**非** indexed base）
- GT: DiskANN MIPS **L=800** top-10
- DRAM window **64 MiB**；`--shuffle-seed 42 --flush-window`（消跨 query 虚高）
- beam=32, iters=48, k=10；nq=**10000**
- Wall: ~1.5 h（10 cells，~9–11 min/cell）

Raw: `results/prefetch_ab_25m_vmem.csv`，log: `serving_25m/prefetch_ab_10k_heldout_runner.log`

## With rerank

| policy | budget | QPS | mean ms | p50 | p90 | p99 | hit% | recall@10 |
|--------|-------:|----:|--------:|----:|----:|----:|-----:|----------:|
| P0 | 0 | 15.39 | 64.5 | 58.8 | 102.6 | 154.4 | 57.9 | 0.0016 |
| P1 | 0 | **16.36** | **60.7** | 55.8 | 95.3 | 139.0 | 57.9 | 0.0016 |
| P1 | 256 KB | 16.25 | 61.0 | 56.3 | 95.8 | 138.3 | 58.0 | 0.0016 |
| P1 | 1 MB | 16.07 | 61.7 | 56.8 | 97.2 | 141.0 | 58.0 | 0.0015 |
| P1 | 4 MB | 15.59 | 63.7 | 58.9 | 99.2 | 142.5 | 57.8 | 0.0016 |
| P3 | 256 KB | 16.15 | 61.5 | 56.5 | 96.8 | 140.8 | 58.0 | 0.0016 |

## Without rerank

| policy | budget | QPS | mean ms | p50 | p90 | p99 | hit% | recall@10 |
|--------|-------:|----:|--------:|----:|----:|----:|-----:|----------:|
| P0 | 0 | 18.54 | 53.6 | 48.9 | 86.6 | 123.7 | 58.0 | 0.0014 |
| P1 | 0 | **19.82** | **50.1** | 45.3 | 81.9 | 118.9 | 58.0 | 0.0014 |
| P1 | 256 KB | 19.71 | 50.4 | 45.6 | 82.7 | 119.9 | 58.0 | 0.0014 |
| P1 | 1 MB | 19.32 | 51.4 | 46.5 | 84.0 | 121.4 | 58.0 | 0.0014 |

## 结论

1. **flush+held-out 后 hit≈58%**（不再是 200k/~78% 虚高），miss 可见。
2. **轻量 P1@0 略赢 P0**（有 rerank +6%；无 rerank +7%）；加大预算无收益甚至略慢。
3. Recall 仍≈0：占位 PQ vs DiskANN GT；吞吐对比有效。
4. `avg_promote_ns≈3 µs`：多数 promote 仍走 soft-cache，非冷 NAND 78 µs。
