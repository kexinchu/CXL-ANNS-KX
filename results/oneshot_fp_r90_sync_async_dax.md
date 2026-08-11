# Sync vs async prefetch @ recall≥0.90 (DAX CXL-DRAM)

**Setup:** L=300 fixed-point, nq=100, shuffle-seed=42, **no flush**, pin-entry, budget 16 MiB/query (P1/P2), DramWindow **1 GiB on `/dev/dax0.0`**, soft-cache still 4 GiB (`avg_promote_ns≈73–79 µs`).

| Policy | recall@10 | QPS | mean ms | CXL-DRAM hit% | prefetch_pages | avg_promote_ns |
|--------|-----------|-----|---------|---------------|----------------|----------------|
| **P0** 无预取 (reuse+pin) | **0.929** | **1.43** | 697.7 | **10.47%** | 0 | 75.1 µs |
| **P1** 同步距离优先预取 | **0.929** | 1.23 | 811.1 | **37.44%** | 395123 | 78.9 µs |
| **P2** 异步距离优先预取 | **0.929** | **1.44** | 694.5 | **10.54%** | 10378 | 73.2 µs |

Raw: `results/oneshot_fp_r90_sync_async_dax_ab.csv`

## Takeaways

1. **Recall 不变**（同一搜索路径）。
2. **P1 sync：** hit% 拉到 ~37%，但搜索线程阻塞在 promote 上 → **QPS 低于 P0**（与此前 anon 窗结论一致）。
3. **P2 async：** QPS ≈ P0（略高），hit% ≈ P0 —— enqueue 后立刻 `get_vec`，worker 几乎总是输给 demand path，异步收益有限。
4. 相对「每 query flush」：P0 hit 4.97%→10.5%；同步 P1 hit 6.4%→37.4%（reuse+pin）。

与截图中旧表（anon+mbind）对比：DAX 重跑后数量级一致（P0≈1.3–1.4 QPS / ~10% hit；P1 sync≈1.1–1.2 QPS / ~37% hit）。
