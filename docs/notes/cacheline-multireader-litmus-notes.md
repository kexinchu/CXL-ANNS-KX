# 同 cache line 多读者（单机近似多 host）

**目的：** 没有真多机时，判断「多 host 并发读 CXL 同一 cache line」会不会成为瓶颈。工具：`cl_conflict_litmus`（`MAP_SHARED` + CXL-DRAM `mbind(node1)`，same vs private，扫 N）。

**做法与有效性：** 普通 load 只测到 CPU Shared 命中；每 op `clflush` 的 `*-miss` 会让读者互相全局失效（单机一致性乒乓），**不能**当多 host 证据。`same-nt`/`private-nt`（仅 `movntdqa`、无 flush）更接近多 host 热读：各方保留本地副本、互不拆对方 cache；WB 上 NT 填线后仍走 L1，对应稳态热读。测不到多 root 冷 fill / 多 CXL 口。

| N | same-nt | private-nt | same-ro-miss | private-ro-miss |
|--:|--------:|-----------:|-------------:|----------------:|
| 1 | 1548 | 1561 | 1.94 | 2.28 |
| 4 | 1544 | 1558 | 1.12 | 2.06 |
| 16 | 1452 | 1427 | 0.21 | 2.31 |

**结果（per_proc_Mops，CXL-DRAM）：** NT 下 same≈private、~0.6 ns/op、近似线性 → 热路径同 line 多读不是主矛盾；miss 塌缩来自 flush 假象。论文勿用 miss 曲线论证多 host 读冲突；矛盾应放在 miss/驻留、写协议、SSD 冷路径。日志：`results/cl_conflict_nt_cxl_dram_20260805_004651.log`。
