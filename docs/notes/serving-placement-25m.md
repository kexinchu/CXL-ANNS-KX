# Serving placement — LAION-25M schema lock

**Available now / replacement:** 原 LAION≈1M（1024-d）保留作对照；**规模替换进行中**：Wikipedia-Cohere **25M×768**（公开包，见 `cxl-dram-index-25m.md`）。无公开 LAION-25M embedding 整包。

**Locked target config（容量账仍按 25M@1024 float16 设计；实跑先用 wiki 25M@768 float32≈71.5 GiB）：**

```text
N=25000000 dim=1024 R=32 pq_bytes=32 vec_bytes=2 (float16) metric=L2
```

| Object | Bytes (25M) | Place |
|--------|-------------|-------|
| vectors float16 | ≈51.2 GiB | CXL-SSD |
| graph R=32 | ≈3.0 GiB | CXL-SSD |
| PQ 32 B/vec | ≈0.75 GiB | CXL-SSD → hot in DRAM |
| pivots/meta | ≲20 MiB | DRAM if fits else SSD |
| entry subgraph | ≤128 MiB | host |
| software DRAM window | ≤1 GiB | CXL-DRAM node1 |

**Decision:** float32@1024 would be ≈102 GiB vectors alone → **over 100 GiB SSD cap**. Store corpus vectors as **float16** (or omit full-precision store and fetch shards later). Existing DiskANN 1M build used **512 B/vec PQ** — too fat for 25M; serving target **pq_bytes=32** (rebuild when 25M ready).

```bash
python3 tools/cap_budget.py --n 25000000 --dim 1024 --R 32 --pq-bytes 32 --vec-bytes 2
# SSD sum ≈ 51.4 GiB → OK
```

**软缓存隔离：** `vmem_sw cache_size_gib=4` 与软件 1 GiB 窗口分开计量；缩 soft-cache 需重载模块。论文数字以软件窗口为准。

**已跑通 proxy：** `results/serving_p0_laion200k.md`（N=200k layout）。
