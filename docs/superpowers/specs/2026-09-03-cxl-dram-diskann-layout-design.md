# CXL-DRAM + DiskANN packed entry — design

**Status:** Plan locked 2026-09-03（运行时用 `--diskann-layout`；旧 `--graph-file` 路径保留以复现 50.25）  
**Date:** 2026-09-03  
**Supersedes for this track:** host-window D0（图在 host 文件、向量在 SSD、窗在 `mbind`）  
**Does not replace:** hide claim 50.25 / 49.25 / oracle 85.8（那些是 host 替身行）

## Goal

回到最初合同：**Oracle = 图+向量都在 CXL-DRAM**。两条调整：

1. **DiskANN 式定长条目**：每个 node 一条记录，**向量和 neighbor id 存在一起**，固定 `STRIDE`。一次 miss / 一次 load 同时拿到「这个点的向量 + 它的出边」。
2. **Host 只留导引图**：从 10M 里随机抽 **10k** 个 node，在 host 上建一张很小的导航图，用来找进入全图的入口。完整图不进 host。

## Roles

```
Host CPU memory（≪ 2 GiB）
  └─ 导引图 G0：10k 个 node 的 (vec + nbrs)
      搜索：先在 G0 上 greedy/beam，得到进入全图的 start

CXL-DRAM（/dev/dax0.0 优先；FPGA BAR / HPS 仍停放）
  └─ 全库 G：10M × STRIDE 的 DiskANN 条目
      Oracle：G 已全部在此，计时 NAND=0
      Hide 热层：从 CXL-SSD promote 上来的扇区

CXL-SSD（vmem_sw 单盘 d9，pagebin 盘不双条带）
  └─ 同一份 10M × STRIDE 布局（冷容量）
```

| 谁 | 存什么 | 不存什么 |
|----|--------|----------|
| Host | 10k 导引图 + beam/cand | 10M 全图、全库向量 |
| CXL-DRAM | 全库定长条目（Oracle）或热扇区（Hide） | NAND 容量本身 |
| CXL-SSD | 全库定长条目 | 不当 host 图文件源 |

## Record (DiskANN)

T2I-10M：`dim=200`，`vec_bytes=4`，`R=32`。

```
offset 0      float32 vec[200]     800 B
offset 800    uint32  nnbrs        4 B
offset 804    uint32  nbrs[R]      128 B
offset 932    pad                  STRIDE - 932
```

`id → byte = HEADER + id * STRIDE`。`nbrs[j]` 是 **全图 ID**（0..n-1），不是导引图局部编号。

**STRIDE 锁定：`/dev/dax0.0` = 34359738368（32 GiB）→ `kDiskannStride = 2048`。**

| STRIDE | 10M 体积 | 一次 4K I/O | 能否整库进 32 GiB dax |
|--------|----------|-------------|------------------------|
| 4096（经典 DiskANN 扇区） | **38.15 GiB** | 正好 1 个 node | **否**（32 GiB 装不下） |
| 2048 | 19.07 GiB | 2 个 node | 是 |
| 1024 | 9.54 GiB | 4 个 node | 是 |

优先：**能整库进 CXL-DRAM 的最大 2 的幂**。32 GiB dax → **2048**。若 `dax` ≥ 40 GiB → **4096**。Hide 与 Oracle **同一份布局**，禁止再拆 `pagebin_graph.bin` + 向量 + 267 GiB expand-bundle。

## 导引图 G0（host）

- `n0 = 10000`，从 `0..n-1` **均匀随机**（seed=42，可复现）。
- 每个样本带上全图里该点的 **向量 + 出边**（出边仍是全图 ID）。
- 在这 10k 个点上再跑一遍短 beam（或诱导子图：边的两端都在 10k 内才留下，缺边用全图出边里落在 10k 的那些）。
- Host 体积：10k × 932 B ≈ **9.3 MiB**（或 10k × STRIDE，仍 ≪ 2 GiB）。
- **不是** 把 1.28 GiB `pagebin_graph.bin` 拷进 host。

搜索顺序：

1. 在 G0 上 oneshot-fp beam（小 L，如 64），得到最好的若干全图 ID。
2. 用这些 ID 当 entry，在 CXL-DRAM/SSD 的全库 G 上跑 L=400。

## Oracle vs Hide

| | Oracle | Hide |
|--|--------|------|
| G 在哪 | 整库已在 CXL-DRAM | 在 CXL-SSD；窗/热层在 CXL-DRAM |
| G0 | host，两边都有 | 同左 |
| 打分 | 读 CXL-DRAM 条目里的 vec | 只读已 promote 的条目 |
| 邻居 ID | 同一条目里的 `nbrs[]` | 同左；**不再**单独 miss 一次图 |
| NAND | 0 | prefetch 升扇区；打分不得 bounce |
| 协议 | T2I-10M，L=400，k=10，seed=42，recall@10 ≥ 0.92 | 同左 |

## 废弃（本轨）

- 默认 `--graph-file` → host 1.28 GiB 全图。
- 向量 pagebin 与图分离 + 267 GiB expand-bundle 当主布局。
- host `mbind` 当「CXL-DRAM 窗」。
- FPGA BAR / `vmem.ko` / `--require-cxl-dram` 在 HPS 活之前。
- 用 50.25 / 85.8 当本轨新 Oracle。

## Open (Task 0)

`/dev/dax0.0` 实际字节数。不够 40 GiB 就锁 STRIDE=2048；够就锁 4096。
