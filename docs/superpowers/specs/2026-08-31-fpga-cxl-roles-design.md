# FPGA CXL roles — design

**Status:** Accepted 2026-09-02 — 打分窗留在 host；HPS / BAR 不作数据面  

> **2026-09-03：** 回到「图+向量在 CXL-DRAM」的轨见 [新 spec](2026-09-03-cxl-dram-diskann-layout-design.md) · [plan](../plans/2026-09-03-cxl-dram-diskann-layout.md)。本文件仍描述 host-window 历史栈；50.25 / 85.8 不改。  
**Date:** 2026-08-31（修订 2026-09-02）  
**Repos:** `CXL-ANNS-KX` (search / claim) + `mem2nvme` (data plane)

## Goal

三层角色。**打分窗口在 host DRAM**（`search_beam` + `mbind`），容量在 FPGA 侧两块 NVMe。不把窗口下放到 FPGA BAR，因此 **不依赖 HPS**。

| 角色 | 硬件 | 软件入口 | 禁止当成 |
|------|------|----------|----------|
| **CXL-SSD** | FPGA 侧两块 CD8P：`0000:d8:00.0` + `0000:d9:00.0` | `vmem_sw` → `/dev/vmem0`（逻辑地址） | 本机 Lexar / PM1733 |
| **打分窗** | Host DRAM，T=1 与 T=4 **都是 2 GiB 共享一份** | `search_beam --dram-backend numa` | FPGA BAR、`vmem.ko`、`/dev/dax*` |
| **Host 其余** | 插座内存 | 图、beam、cand | CXL-SSD 容量 |

`--require-cxl-dram` 仍表示「窗口在 FPGA BAR」，HPS 未活时 **不要开**。默认路径是 host 窗。

## Why the old BAR plan stopped

把打分窗下放到 FPGA BAR 依赖 HPS。2026-09-01 `hps_status=raw=0xffffffff unsupported`。userspace `mmap resource0` 会挂。7.0 上 `15:00.0` 是 Intel `8086:0ddb` / `cxl_type2_accel`，不是 Altera `1172:0000`。**BAR / `vmem.ko` / `--require-cxl-dram` 停放。**

现在的正确栈：打分窗 = host 2 GiB；容量 = 单盘 `vmem_sw` on d9（pagebin）。双盘条带会打散 420/460 GiB 布局，禁止装回当前镜像。旧 85.8（1 GiB T=1）和 136（4×512 shard）是历史行，不是「同 2 GiB + 加核」的公平对照。

## Data path (target)

```
CPU 打分 ──► Host DramWindow（2 GiB，T=1/T=4 同一份）
                ▲
                │ vmem_sw ioctl / PREFETCH（软件 memory-semantic）
                │
           CXL-SSD (vmem_sw 单盘 d9 pagebin；双盘条带停放)

search_beam 仍是唯一搜索逻辑。图从 host 文件进 DRAM。
FPGA BAR / HPS / vmem.ko 不在这条路上。
```

Oracle = discarded pass 把本轮 eval WS pin 进 **host 2 GiB 窗**，计时 `freeze_fills`，NAND=0。Hide = 边走边从 CXL-SSD 填同一 host 窗。

## Oracle protocol（公平 T=1 vs T=4）

两边 **只有** thread 数不同：

- Host 打分窗 **都是 2 GiB**，共享一份，禁止 `4 × 512 MiB`。
- `--dram-backend numa`，`--cpu-affinity`，work-steal，禁止 `qi % T`。
- 计时 `nvme_read_B=0`，`from_win=100`，bounce=0。
- 旧 85.8（1 GiB T=1）和 136（4×512 MiB shard）仍是历史行；新公平行另记，不自动替换 claim。

## Hard gates (fail closed)

1. `fuser /dev/vmem0` 非空（现在的 `pack_nbr_bundle`）→ 脚本退出，不 `rmmod`。
2. `hps_status` 不是 `0..3` → **不准** `insmod vmem.ko`，不准 mmap BAR，不准报 CXL-DRAM 数。
3. 禁止 userspace `mmap` `/sys/bus/pci/devices/0000:15:00.0/resource0`。
4. 默认 **不要** `--require-cxl-dram`。该 flag 仍拒 numa（BAR 路径）；host 窗 Oracle/hide 走 numa。
5. 不把 `vmem_sw` 的 `ram_size` 写成 CXL-DRAM；它是 CXL-SSD 前面的 host cache。
6. 重载 `mem2nvme` 时若 `vmem_sw` 正在做盘 I/O → 禁止（probe 会写 FPGA `REG_CTRL`）。

## Non-goals

- 不刷 FPGA 比特流（HPS 死了就停，不在本计划里修固件）。
- 不把 Linux `cxl list` / `/dev/dax*` 当作成功条件（class `ff00`，走 `vmem.ko`）。
- 不在本计划里 restage 267 GiB hide bundle。
- 不把 node1 说成 CXL-DRAM；numa 窗记 `from_win`，`from_cxl_dram=0`。

## Measured（2026-09-02，host 2 GiB 公平 Oracle）

nq=20 L=400 seed=42 oneshot-fp P3 ebatch=8 ahead=2 `--no-score-page` `--nbr-bundle` pagebin `CXAN1`。

| T | QPS | mean | recall@10 | NAND | 日志 |
|--:|----:|-----:|----------:|------|------|
| 1 | 96.59 | 10.351 | 0.925 | 0 | `oracle_host2g_T1_nq20.log` |
| 4 | 128–132 | ~27 ms | 0.900–0.925 | 0 | `oracle_host2g_T4_nq20*.log` |
| 8 | 114–131 | ~53–62 ms | 0.900–0.920 | 0 | `oracle_host2g_T8_nq20*.log` |
| 16 | 117–125 | ~111–120 ms | 0.885–0.920 | 0 | `oracle_host2g_T16_nq20*.log` |
| 32 | 103.46 | 163.5 ms | 0.930 | 0 | `oracle_host2g_T32_nq20.log` |

T≥4 不锁。QPS 在 T=4 封顶后回落。不替换 50.25 / 85.8 / 136.23。

## Approval

角色划分已批。执行口径以本修订为准；plan 里 BAR 切栈 Task 停放。
