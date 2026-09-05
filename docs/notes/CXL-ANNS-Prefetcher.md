# CXL-ANNS Prefetcher 攻关笔记

日期：2026-08-30。机器：gpu01，kernel `6.18.0-rc5`。  
论文 claim **不降**：host 打分看起来像数据已在小 CXL-DRAM 窗口里；容量在 CXL-SSD；prefetch 把 NAND 移出 ANNS **query 打分路径**；`from_win=100`，bounce=0。Hide 失败就修 runtime，不改 Intro。

本文件记录思考过程、全部真机数据，以及两道对比问题的结论。

组会汇报（2026-08-31，可独立宣讲）：[`2026-08-31-组会汇报.md`](2026-08-31-组会汇报.md)。  
FPGA 角色（2026-09-02 修订：打分窗留 host，BAR/HPS 停放）：[spec](../superpowers/specs/2026-08-31-fpga-cxl-roles-design.md) · [plan](../superpowers/plans/2026-08-31-fpga-cxl-roles.md)。

---

## 0. 冻结合同与测量定义

### 0.1 产品合同

- 服务：可用的 ANNS，容量在 CXL-SSD，host 只看到小 DramWindow。
- Prefetch 目标：CXL-SSD **不在 query 打分临界路径上**。
- 全精度分数 100% 来自 DramWindow（`from_win_pct=100`，bounce=0）。
- 必须在真设备上测（`vmem_sw` + NAND），不是只跑 host-file smoke。
- 指针追逐上限（写进论文）：一次 walk 在 hop *i* 打完分之前，无法完全隐藏 hop *i+1*。跨 query continuous batching 才是把设备 QD 拉满的手段。

### 0.2 利用率（用户定义，反复强调）

**Page-touch ≠ 利用率。** 4 KiB 里 5×800 B，只用 1 条 = **20%**，不是 100%。

- 正确：`issue_use` = 这次 expand 的 **未访问** 邻居 / 该页上完整装下的 ID 数。
- 禁止：给没用到的 sibling 打分来把利用率刷成 100%（`--score-page`）。
- 要做：page 管理，把一起访问的 neighbors 放进同一页。
- 旧计数 `slot_use`（query 内某 ID 是否曾经被打过分）会把「4 个 sibling 早在别的 expand 打过」算成已使用，**虚高**。

### 0.3 真实带宽占用

不是「有 IO 在飞就标 100%」。

```
occupancy = (timed 窗口 /sys/block/<ns>/stat 读扇区 × 512) / 墙钟 / 孤立随机峰
孤立随机峰（true-cold PREFETCH_BATCH）= 1.560 GB/s
孤立顺序峰 = 1.950 GB/s
```

禁止用 `promote_GBps`（会双计、含 warm）。

### 0.4 协议

- T2I-10M，oneshot-fp，L=400，k=10，seed=42，recall@10 ≥ 0.92。
- True-cold：`rmmod`+`insmod` 直到 `cache_used=0`。`--flush-window` 只清 1 GiB 窗口，**不算** true-cold。
- 图从 **host 文件** 拷进 DRAM，禁止从 `/dev/vmem0` 拷图。
- Pagebin 重映射 ID，禁止把 `CXAN_HOST_LAYOUT` 指到 packed `layout_t2i_10m.bin`。
- 构建：`g++ -O3 -mavx2 -mfma -std=c++17 -pthread -lnuma`。

---

## 1. 硬件身份（测 hide 时 vs 写本文时）

### 1.1 锁定 hide 行当时（2026-08-30 清晨）

| 项 | 值 |
|----|-----|
| FPGA | Altera `0000:15:00.0`（另有 `16:00.0`），BAR0 标称 32 GiB |
| NAND 绑定 | `nvmex` / `vmem_sw` → 当时的 `/dev/nvme4n1` = **`0000:d9:00.0`** Dell CD8P 1.92 TB |
| Userspace | `/dev/vmem0` = `vmem_sw`：28 GiB RAM map + **4 GiB cache** + 2 MiB stripe |
| HPS/BAR | 读出来是 `0xff`，**死**。禁止发 `vmem.ko` 数 |
| DAX / CXL memdev | **不存在**。DramWindow = 1 GiB `mbind` node 1（host DRAM 替身） |
| 对外身份 | 「FPGA 挂的 NVMe + 软件 memory-semantic 代理」，**不是** 产品级 CXL.mem SSD |

`tools/restore_vmem_sw.sh` 仍写死旧 BDF `d8`，**不要用**。当时正确加载：

```bash
rmmod vmem_sw
insmod /root/chukexin/mem2nvme/host/vmem_sw.ko \
  nvme_dev=/dev/nvme4n1 target_bdf=0000:d9:00.0 \
  expected_ssd_size_bytes=1920383410176 \
  ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
```

### 1.2 写本文时（2026-08-30 12:50 以后）现场复核

块设备名已经漂过。**现在**两块 CXL 侧 Kioxia/Dell CD8P 是：

| 现名 | BDF | 序列号 | 容量 | PCIe |
|------|-----|--------|------|------|
| `/dev/nvme1n1` | `0000:d8:00.0` | `7EU0A01P0XK1` | 1.92 TB | 32 GT/s ×4（PCIe 5.0 x4） |
| `/dev/nvme2n1` | `0000:d9:00.0` | `2F50A1360XK3` | 1.92 TB | 32 GT/s ×4 |

现在的 `/dev/nvme4n1` 是 `0000:3f:00.0` Lexar，**不是** CXL-SSD。锁定实验里的 `nvme4n1` = 当时的 d9 CD8P。

FPGA（写本文时）：

| BDF | BAR0 | LinkCap | LinkSta |
|-----|------|---------|---------|
| `15:00.0` | 32 GiB **disabled** | 16 GT/s ×8 | **16 GT/s ×4（降速）** |
| `16:00.0` | 16 GiB **disabled** | 16 GT/s ×8 | **16 GT/s ×4（降速）** |

无 `vmem`/`nvmex` 模块，`/dev/vmem0` **不存在**（degree-bundle 打包写到约 39% 后路径断了）。无 `/dev/dax*`。NUMA：node0/1 各 ~64 GiB。

结论：机箱里**确实有 2 块** CD8P；锁定 hide **只绑了其中 1 块**。CXL.mem / FPGA BAR **没起来**。24 GB/s 的「CXL-SSD→CXL-DRAM」理论值，当前软件路径上**测不到**。

### 1.4 恢复现场（2026-08-30 晚，本轮）

开机后有人加载了**树外**双盘 `vmem_sw`（`srcversion=76BDB408…`，`nvme_devs=nvme1n1,nvme2n1`，逻辑 3.58 TB）。`420 GiB` pagebin magic 读成垃圾——2 MiB 条带把旧单盘偏移打散。树内 `.ko` 是 `7.0.0-30-generic`，不能装进 `6.18.0-rc5`。

已做：

- 用树内源码 `VMEM_SW_ONLY=1` 编出 `vmem_sw.ko.6.18-single`（`vermagic=6.18.0-rc5`）。
- `rmmod` 双盘模块，单盘绑回 **`/dev/nvme2n1` = `0000:d9:00.0`**（锁定 hide 那块）。
- pagebin `450971566080` magic **`0x314e415843`**，`n=10M dim=200 R=32`，`cache_used=0`。
- `vmem_sw` 的 `HPS_SUBMIT` **直接 `-EOPNOTSUPP`**；`vmem.ko` 才走 BAR DMA。
- FPGA `15:00.0` BAR0 **已 enable**（32 GiB，`ntcx`），但前 256 B 仍全 `0xff`；`16:00.0` BAR 仍 disabled；无 `/dev/dax*`。`nvmex` 已绑 d8+d9。

**不要**再把双盘模块装回去读旧 420/460 GiB——映射不同。入度 bundle 必须在**当前单盘**上整份重写。

### 1.3 数据布局（vmem 逻辑地址）

| 区 | vmem offset | 内容 |
|----|-------------|------|
| Packed T2I-10M | 400 GiB | `layout_t2i_10m.bin`（pagebin 实验不要当 graph 源） |
| Pagebin | 420 GiB = `450971566080`，len `9600069632` | 向量 + 头 |
| Expand-bundle | 紧接 pagebin = `460571635712`，stride 28672 | 10M × 7 × 4 KiB = **267.03 GiB**，每点 N(u) 副本 |
| 图 | host `pagebin_graph.bin` 1.28 GiB | 不走 vmem |

---

## 2. 思考主线（为什么这样攻）

1. **先利用率再带宽。** 利用率低时拉带宽 = 浪费。用户要的是「页装进来之后 5 条都是这次有效结果」，不是 page-touch，也不是冗余打分。
2. Star-BFS pagebin：热 200k expand 理想 ~7 页，实际 23.65 页；expand slot ~26%；`|d|<5` 共页 1.4%。全局 permute / train-trace **搬不到** eval query（T2I working set 不相交）。过拟合 eval→eval 能到 89%，论文不能用。
3. **`--score-page` 把 slot 刷到 100% 但是多算 sibling**，用户禁止当利用率故事。AVX2 score-page 21.30 QPS 是算力通胀。
4. **Expand-bundle**：每个点的 32 邻居占 7 张连续 4 KiB。Expand 只 prefetch 含未见邻居的页，只打这些邻居的分。这是 page 管理，不是冗余计算。
5. Query 级 `slot_use=100%` 锁定后，再看真实 NVMe 占用。单路 0.600/1.560=38.5%。有用流量天花板：~12 MB/q × oracle 85.8 QPS ≈ 1.02 GB/s ≈ **65% of 1.560**。再往 100% 设备峰走，要么灌空页（禁止），要么有用字节/q 再涨。
6. 后来发现 `slot_use=100` **虚高**：`issue_use`（本次 load 的未见 slot / 页内完整 ID）只有 **64.30%**。下一步应是按入度重排 N(u)，让晚 expand 的剩余未见挤在最后 1–2 页。打包写到 ~39% 后 vmem 掉了，**尚未真机复测新 bundle**。

---

## 3. 锁定 Oracle（同 query，nq=20，L=400）

| Row | QPS | mean ms | p99 ms | recall@10 | from_win | SSD |
|-----|----:|--------:|-------:|----------:|---------:|-----|
| Oracle host DRAM `--oracle-dram` | **66.52** | 14.823 | 47.561 | 0.950 | 100 | 0（9.6 GiB MAP_POPULATE） |
| Oracle 1 GiB window `--oracle-window` + `freeze_fills` | **85.78** | 11.446 | 15.484 | 0.925 | 100 | **0**（discarded pass 把 WS pin 进窗口） |

Oracle 85.8 是 **单线程、窗口已热、不再填 NAND** 的 QPS。mean **11.4 ms**。这是「hide 完全成功」的单路延迟/吞吐上界，**不是** 4 路服务 QPS 上界。

L=250 pagebin oracle-window nq=20 = 169.78 QPS / 0.930 — **不要**和 L=400 iso-recall 行比。

---

## 4. 锁定 hide 行（claim 用这两行，不改）

单路、1 GiB 窗口、expand-bundle、ebatch=8、ahead=2、look=0、`--no-score-page`、true-cold。

| nq | QPS | mean ms | p50 | p90 | p99 | recall | from_win | bounce | slot_use† | page_use | nvme GB/s | occ | crit_wait | evicts | log |
|---:|----:|--------:|----:|----:|----:|-------:|---------:|-------:|----------:|---------:|----------:|----:|----------:|-------:|-----|
| 20 | **50.25** | 19.844 | 19.775 | 23.247 | 26.592 | **0.940** | 100 | 0 | 100† | 97.09 | **0.600** | **38.5%** | 24.7 ms 合计 | 0 | `hide_pagebin_bundle_ahead2.log` |
| 100 | **49.25** | 20.298 | 19.145 | 25.314 | 35.551 | **0.933** | 100 | 0 | 100† | 96.31 | **0.561** | **36.0%** | 119 ms 合计 | 21852 | `hide_pagebin_bundle_ahead2_nq100.log` |

† query 级 `slot_use`（见 §6，虚高）。  
nq=20 细项：dist=183628，promote=500064256 B，nvme_read=238850048 B，hit=13.75%（9728/61043），overlap=0.07，`crit_wait`/q = 24.7e6/20 = **1.24 ms**。  
相对 oracle：QPS 50.25/85.8 ≈ **0.59×**；mean 19.8 vs 11.4 = **+8.4 ms/q**。

---

## 5. 利用率与占用：全部真机表

### 5.1 孤立设备峰（PREFETCH_BATCH，nvme stat，true-cold）

| 模式 | nvme GB/s |
|------|----------:|
| 顺序 | **1.950** |
| 随机 | **1.560** ← occupancy 分母 |

早期 probe（issued 口径）seq 曾到 2.07–2.33；占用一律用 **stat 扇区**。

### 5.2 利用率路线（禁止把 VOID 行画进 iso-recall 主图）

| 阶段 | nq | QPS | slot 口径 | 说明 |
|------|---:|----:|-----------|------|
| pagebin 无 sibling | 20 | 26.34 | **25.4%** | 5 槽用 1 |
| `--score-page` scalar | 20 | 14.94 | 100% 冗余算 | 禁止当故事 |
| AVX2 `--score-page` | 20 / 100 | 21.30 / 21.32 | 100% 冗余算 | occ 29.2% / 25.6% |
| 并行 sibling 打分（fill pool） | 20 | 11.34 | — | VOID，NAND 被抢 |
| expand-bundle ebatch=1 | 20 / 100 | 42.17 / 42.93 | query-slot 100 | occ 28.2% / 28.9% |
| ebatch=8 ahead=1 | 20 / 100 | 47.24 / 47.53 | 100 | occ 36.0% / 34.4% |
| **ahead=2 锁定** | 20 / 100 | **50.25 / 49.25** | 100† | occ **38.5% / 36.0%** |
| look=8 未来 bundle | 20 | 31.04 | page 86 / slot 89 | VOID，未提交 expand |
| **issue_use（本次 load）** | 20 | 48.04 | **issue_use=64.30%** | 见 §6 |

### 5.3 单路 QPS 爬坡（同一 iso-recall，节选）

| 行 | nq | QPS | mean ms | 注 |
|----|---:|----:|--------:|----|
| 同步 ioctl 旧 | 20 | 2.83–7.79 | 128–354 | 搜索线程上打 NAND |
| clock alloc | 20 / 100 | 23.43 / 26.94 | 42.6 / 37.1 | 不再扫 256k 帧 |
| look=0 miss-only | 20 / 100 | 22.96 / 26.40 | 43.5 / 37.9 | page_use 100，slot 仍 ~25 |
| stall32 | 20 / 100 | 31.40 / 33.49 | 31.8 / 29.8 | 满管线不再丢 miss |
| bundle 锁定 | 20 / 100 | 50.25 / 49.25 | 19.8 / 20.3 | 见 §4 |

L=200/220 nq=20 recall&lt;0.92 VOID。L=250 nq=20=13.13/0.945 但 nq=100=0.891 VOID。L=300 nq=100=0.913 VOID。地板只在 **L=400**。

### 5.4 多路私有窗口（占用探针，**不能**和 85.8 单路比）

每线程 512 MiB DramWindow，warm 在计时前，`--per-thread-window`。

| T | nq | QPS | mean ms | p99 | recall | nvme GB/s | occ | evicts | 总窗口 |
|--:|---:|----:|--------:|----:|-------:|----------:|----:|-------:|--------|
| 2 | 20 | 60.35 | 25.511 | 35.052 | 0.950 | 0.761 | 48.8% | 0 | 1 GiB |
| 3 | 20 | 71.15 | 30.466 | 54.110 | 0.945 | 0.881 | 56.5% | 0 | 1.5 GiB |
| 4 | 20 | **79.02** | **34.372** | 70.153 | 0.950 | 0.969 | 62.1% | 0 | 2 GiB |
| 2 | 100 | 63.85 | 23.489 | 41.563 | 0.928 | 0.748 | 48.0% | 36341 | 1 GiB |
| 4 | 100 | 87.21 | 29.272 | 45.765 | 0.941 | **1.027** | **65.8%** | 0 | 2 GiB |
| 8 | 100 | 69.14 | 75.517 | 153.905 | 0.945 | 0.817 | 52.3% | 0 | 4 GiB，过订阅 |

共享 1 GiB × 4 线程：50.71 QPS，mean **70.3 ms**，occ 36.1%（窗口锁）。VOID 当占用行。

### 5.5 未锁进 claim 的 T=1 税实验

| 实验 | nq=20 QPS | nq=100 QPS | 处理 |
|------|----------:|-----------:|------|
| `with_resident` 锁内打分 | 48.07 | — | 撤回 |
| 等 NAND 时 drain | 52.39 / occ 41.9% | 44.60 | 不锁（nq=100 驱逐升） |
| 页排序 + 一次 56 页 READ_BATCH | 46.25 | 41.48 | 撤回 |
| `insert_cand` 记 worst | 52.71 / occ 40.3% | 47.14 | 撤回（nq=100&lt;49.25） |
| slim warm | 47.54 | — | 撤回 |
| pipe_w=32 | 46.47 | — | 撤回 |
| score thread + mutex | 2.37 | — | 撤回 |
| ebatch 16/32、ahead=3 | ≤46 | — | 无赢 |

留下：clock alloc、`try_copy_resident`、batch `install_full_pages`、bundle、ebatch=8、ahead=2。

完整 CSV：`results/paper_figs/hide_matrix.csv`。过程稿：`results/paper_figs/2026-08-30-hide-engine.md`。

---

## 6. issue_use = 64.30%（利用率的真实口径）

2026-08-30 真冷 nq=20，当前 bundle（图邻接顺序，非入度序）：

```
slot_use_pct=100.00
issue_use_pct=64.30
issue_slots=232442  issue_want=149459
byte_use_pct=60.76
page_use_pct=97.05
QPS=48.04  (计数开销，不替代锁定 50.25)
```

含义：发出的页上，完整装下的 ID 里只有 64.3% 是**这一跳还没访问的邻居**。其余 35.7% 是同页 sibling，在**别的点的 bundle** 里早就打过。`slot_use` 按「本 query 是否打过该 ID」算，所以仍是 100%。

这正是用户举的 5 条用 1 条 = 20% 的同构问题，只是平均到 64% 而不是 20%。

已做：`tools/reorder_graph_degree.cpp` 按入度降序重排 N(u)，98.5% 行有变化，写出 `pagebin_graph_deg.bin`。第一次覆写停在 ~39% 后 vmem 掉线。本轮单盘 d9 上 **整份重打完成**（`pack_nbr_bundle_deg.log`，`DONE`，45 min，`nbr_bundle_deg.json`）。

真冷 nq=20 + `--graph-file pagebin_graph_deg.bin`（禁止 `--score-page`）：

| 次 | QPS | wall s | mean ms | recall | from_win | issue_use | nvme GB/s | 日志 |
|----|----:|-------:|--------:|-------:|---------:|----------:|----------:|------|
| 对照（旧图序 bundle） | 48.04 | 0.416 | 20.756 | 0.950 | 100 | **64.30** | 0.574 | `hide_pagebin_bundle_issueuse_nq20.log` |
| 入度序 r1（刚写完 267 GiB） | 12.71 | 1.573 | 20.550 | 0.945 | 100 | 62.93 | 0.150 | `hide_pagebin_bundle_deg_nq20.log` |
| 入度序 r2（再 reload） | 26.42 | 0.757 | 20.387 | 0.950 | 100 | **62.98** | 0.312 | `hide_pagebin_bundle_deg_nq20_r2.log` |

mean 仍是 ~20.4 ms（和锁定 hide 同量级）。r1/r2 的 QPS 被 `t1` 之前的 `shared_pool.stop_join()` 拉低：写完 267 GiB 后盘在做 GC，管线排空进了 wall，没进 per-query timer。用 query 墙钟估 r1：235 MB / 0.411 s ≈ **0.57 GB/s**，和对照 0.574 一样。

**结论：入度序没有修 issue_use**（64.30 → 62.98）。全局入度 ≠ 本 query 已访问集合；热 slot 被排到页首，未见邻居挤在后面，发出的页更「脏」。NAND 上现在是入度 bundle，必须配 `pagebin_graph_deg.bin`。要回到锁定 50.25 布局，得用原 `pagebin_graph.bin` 再打 267 GiB。claim 行不动。

---

## 6.5 窗口是不是每个 query 都从空开始？（可以 pre-heat）

**不是每个 query 都从空窗口开始。** 默认协议已经在 pre-heat，只是热的不是「这条 eval query 的精确 working set」。

| 机制 | 何时 | 做什么 | 锁定 hide 用了吗 |
|------|------|--------|------------------|
| `hide_warm_entry_ball` | **计时循环之前一次**（不是每条 query） | entry + 1-hop bundle + `eg.nodes` 里约 8192 个 primary，pin ~6.5 MiB | **是**（`--hide-warm-entry` 默认开） |
| 窗口残留 | 默认 `--flush-window` **关** | 上一条 query 没 pin 的页留在 1 GiB 窗口里 | **是** |
| `--flush-window` | 每条 `run_one_q` 开头 | 丢掉未 pin 页，近似「每 query 空窗」 | **否**（void 当 true-cold） |
| `--oracle-window` + `freeze_fills` | 丢弃一轮把 **本 eval WS** pin 进窗口，然后禁止 NAND | 这是 85.8 / 11.4 ms 那一行 | **否**——那是上界，不是 hide runtime |

跨 query 复用很弱（T2I 随机、WS 不相交）：锁定 nq=20 **hit=13.75%**，nq=100 **hit=3.31%**，nq=100 还有 evict。所以「可以 pre-heat」≠「下一条 query 已经在窗口里」。

不能做的 pre-heat：把 seed=42 的 eval WS 预先装进窗口再报 hide QPS——那就是 oracle，claim 作废。

true-cold 指的是 **4 GiB 设备 cache + NAND**（`rmmod`+`insmod` 到 `cache_used=0`），不是「1 GiB DramWindow 每条 query 清空」。

---

## 7. 问题 1：85.8 单路 vs 79.02 四路；临界路径上有没有读 CXL-SSD？

### 7.1 不能比的两行

| | Oracle 1 GiB | Hide T=1 锁定 | Hide T=4 nq=20 |
|--|-------------:|--------------:|---------------:|
| 并发 query | **1** | **1** | **4** |
| 窗口 | 1 GiB 已 pin，`freeze_fills` | 1 GiB，边走边填 | 4×512 MiB |
| NAND | **0** | 有，异步填窗口 | 4 路抢同一块盘 |
| QPS | **85.8** | **50.25** | 79.02 |
| mean latency | **11.4 ms** | **19.8 ms** | **34.4 ms** |
| p99 | 15.5 ms | 26.6 ms | 70.2 ms |

79.02 是 **服务吞吐**（4 个 query 同时跑，墙钟 0.253 s 做完 20 个）。单 query 反而更慢（34 ms vs 19.8 ms）。和 85.8 比对吞吐是把「4 个工人」跟「1 个工人、数据已在窗口」比，无效。

公平单路：

```
50.25 / 85.8 ≈ 0.59× QPS
19.8 − 11.4 = +8.4 ms/q
```

这 8.4 ms 才是「还没 hide 住」的单路残差。

### 7.2 打分读的是窗口，还是 SSD？

P3 合同：**分数只从 `try_copy_resident` / 窗口 memcpy 来**。锁定行：

```
from_win=183628  from_bounce=0  from_win_pct=100.00
```

**每一次距离计算的字节来自 DramWindow（node1 上的 1 GiB），不是当时去 fault `/dev/vmem0`。** 这一点成立。

### 7.3 但 query 临界路径上仍然在等 SSD

窗口不是一开始就有数据。`cxl_dram_hit_pct=13.75`：61043 次 promote 里只有 9728 次页已在窗口。其余必须 `PREFETCH_BATCH`/`READ_BATCH` → `vmem_sw` → **那一块** CD8P → host scratch → `install_full_page`。

搜索线程在这些时候 **阻塞等 NAND**：

1. `pipe.issue(..., stall_if_full=true)`：32 个 in-flight 槽满了，`hide_wait` 直到一槽完成。
2. frontier 上没有可 expand 的已打分候选：`wait_covering`，计入 `crit_wait_ns`。
3. 收尾 `wait_all`。

锁定 nq=20：`crit_wait` **1.24 ms/q**，`device_fill` 1.47 ms/q，`overlap=0.07`（fill 时间 / 墙钟），几乎没有「算力盖住 NAND」。

所以要拆开说：

| 层次 | 状态 | 证据 |
|------|------|------|
| 打分 load | 已 hide | bounce=0，from_win=100 |
| 发现下一跳之前的等待 | **未 hide** | crit_wait 1.24 ms/q，hit 13.75%，overlap 0.07 |
| 服务层「像没 flash」 | **未达到** | 19.8 vs 11.4 ms；50 vs 86 QPS |

指针追逐：hop *i* 的分数出来之前，不知道 hop *i+1* 的 ID，也就不能把那一跳的 NAND 从 **这一条** query 的临界路径上拿掉。四路只是让盘在 **别人的** query 上保持 QD，单 query 延迟不会变成 11.4 ms。

### 7.4 8.4 ms 残差里大概有什么

粗拆（nq=20 锁定，每 query）：

- 有用 NAND ~12 MB。即使在 1.56 GB/s 上纯顺序也要 ~7.7 ms；实际随机+ioctl，且只有一部分与计算重叠（overlap 0.07）。
- `crit_wait` 显式 1.24 ms（frontier 空）。
- 其余：800 B×~9k 次窗口拷、AVX2、O(L) `insert_cand`、图在 DRAM 的邻居扫描。Oracle 同样要算距离，所以 **多出来的 8.4 ms 主要是「页还不在窗口」的填+等**，不是公式本身。

结论：**打分指令不直接读 CXL-SSD；但 query 仍然在临界路径上等 CXL-SSD 把页装进窗口。** 四路 79 QPS 没有否定这件事。

---

## 8. 问题 2：为什么只有 1.56 GB/s？要不要把 prefetch 算力放到 device？

### 8.1 1.56 是什么，不是什么

1.560 GB/s = **true-cold、随机、4 KiB 级、`VMEM_IOC_PREFETCH_BATCH`、经过 `vmem_sw` 软件代理、只打当时那一块 NVMe、用 `stat` 扇区** 的峰。顺序同一路径是 1.950 GB/s。

它 **不是**：

- CXL.mem 窗口带宽；
- FPGA BAR DMA（BAR **disabled**，HPS 读 `0xff`）；
- 两块 CD8P 条带之和；
- 「有 bio 在飞」的假 100%。

Hide 单路真实搬运 0.600 GB/s = 0.600/1.560 = 38.5%（相对这条软件随机峰，不是相对 24）。

### 8.2 24 GB/s 理论值对得上什么

用户侧产品图：CXL-SSD 内部 2×NVMe，CXL-SSD→CXL-DRAM **24 GB/s**。

现场链路：

| 路径 | 能力 | 锁定实验用了吗 |
|------|------|----------------|
| CD8P #1 d8 PCIe 5.0 ×4 | 原始 ~16 GB/s/方向 | **否** |
| CD8P #2 d9 PCIe 5.0 ×4 | 同上 | **只用这一块**，且走内核 bio |
| FPGA 15/16 BAR，PCIe 4.0 ×4（降自 ×8） | 原始 ~8 GB/s/卡；BAR **关** | **否**，没有 CXL.mem |
| `vmem_sw` 4 GiB host cache + stripe | 软件 memcpy + ioctl | **是，唯一数据面** |
| DramWindow | host node1 DRAM 1 GiB | 是（替身，不是 device CXL-DRAM） |

24 GB/s 要的是：**device 内 NVMe→片上/CXL-DRAM，再 CXL.mem 给 host**。我们跑的是：**host 发 ioctl → 内核 → 一块盘的 4K 随机 → 拷回 host DRAM**。分母差的是介质路径，不是「prefetch 算力不够所以只有 1.56」。

PCIe 5.0 ×4 单盘顺序通常远高于 1.56（数 GB/s）。1.56 是 **4K 随机 + vmem 缓存锁/batch/bio 合并** 的软件天花板。两块盘都接上、走大块顺序，host 侧也能到数 GB/s～十数 GB/s，仍然到不了「24 GB/s CXL.mem」，除非 BAR/CXL.mem 活着。

### 8.3 要不要把 prefetcher 计算放到 CXL device？

拆成两件事：

**A. 带宽从 1.56 涨到接近 24 — 首先不是搬计算，是搬数据面。**

缺的是：BAR/HPS/CXL.mem 使能、两块 NVMe 在 device 侧条带、host 只碰窗口。现在 FPGA BAR disabled、只绑一块盘、窗口在 host NUMA。把 beam/prefetch 逻辑搬到一块 **还不能被 host 当内存用的** FPGA 上，不会变出 24 GB/s。

**B. 单路延迟 19.8→11.4 — device 侧 walk/prefetch 是对的，而且正好打在指针追逐上。**

Host 必须：打 hop *i* → 才知道 N(u) → 再 ioctl 拉 hop *i+1*。`overlap=0.07`、`crit_wait` 1.24 ms/q，都是这条往返。若 device 上已经有图+向量，能在 hop *i* 的分数出来时 **本地** 把 N(u) 推进 CXL-DRAM，host 只读窗口，则：

- 单路临界路径上的 NAND 等待可以接近 oracle（claim 的 hide）；
- 设备 QD 不再被 host 一跳一 ioctl 卡住；
- 24 GB/s 那条内部总线才有机会被有用流量用上。

这不是「因为 1.56 太低所以必须上 device 计算」；是「**hide 单路延迟** 被 host 往返和指针追逐锁死，device 侧 prefetch/walk 才是对症。**24 GB/s 要先把 CXL.mem/BAR 和双盘内部通路打开。**」

用户冻结的工程顺序（本轮按此执行，不跳刀）：

1. **恢复 `/dev/vmem0`（单盘 d9，pagebin 可读）** — 已完成，见 §1.4。
2. **修 `issue_use` 64%** — 入度序整份打完并真冷复测：**失败**（62.98%，见 §6）。不是没做，是这刀不对症。
3. **拉活 BAR / 双盘内部通路** — `15:00.0` BAR enable + `ntcx`，前 4 KiB 和 HPS 窗仍全 `0xff`；扫 16 KiB BAR 会 PCI completion 挂死，已停。`vmem_sw` `HPS_SUBMIT` = `-EOPNOTSUPP`。`nvmex` 已绑 d8+d9，但 hide 数据面仍是单盘 d9。开机双盘 `.ko` 不在树里。
4. **device 侧 neighbor walk** — 对 19.8→11.4 仍是对的；HPS 死、无 on-device 核，本轮不能做。禁止假装 24 GB/s。

---

## 9. 当前现场与不要重做的列表

**现场（本轮结束后）：** 单盘 `/dev/vmem0` → `nvme2n1`/`d9`，pagebin magic 好；NAND 上已恢复**图序 bundle**（配 `pagebin_graph.bin`，见 §11）；issue_use **未修好**；BAR enable 但 HPS `0xff`；`nvmex` 绑着两块 CD8P。hide 锁定行仍是 50.25 / 49.25。

**不要当赢再跑：** `--score-page`、fill pool 上打分、score thread+`work_mu`、slim warm、`pipe_w=32`、look=8 未提交 bundle、共享 1 GiB 四线程刷占用、wait 中打分、sort+单 chunk 56 页、unfaulted READ_BATCH 进窗口、`restore_vmem_sw.sh`（BDF `d8` 且现网名已变）、发 `vmem.ko`/BAR 数、把 79.02 或 87.21 写成「已经达到 oracle 85.8」。

**Claim 行保持：** hide pagebin nq=20 **50.25**、nq=100 **49.25**；oracle window **85.8**；`from_win=100`。利用率对外应改口到 `issue_use`（64.3% 未修），不要再用虚高的 query-`slot_use=100` 假装「5 槽全是这次结果」。

---

## 10. NVMe→CXL-DRAM 带宽、单路 50.25→85.8、四路 79.02→oracle（2026-08-31 晚）

### 10.1 通路 1：device 内 NVMe→CXL-DRAM

**这条通路现在不存在，带宽和占用都测不到。**

| 检查 | 结果 |
|------|------|
| `/dev/dax*` | 无 |
| `cxl list -M` | `[]` |
| NUMA node 1 | **有 CPU**（43–85,129–171），node 距离 10/12，是对槽 DRAM，不是 CXL-DRAM |
| FPGA `15:00.0` BAR0 | enable 32 GiB，`ntcx`，读数仍 `0xff` |
| `vmem_sw` HPS_SUBMIT | `-EOPNOTSUPP` |

DramWindow 的 `mbind node 1` 是 host DRAM 替身。禁止把下面任何一行写成「CXL.mem 24 GB/s」。

### 10.2 能测到的真实带宽（host 侧）

| 路径 | seq | 4K 随机 | 日志 |
|------|----:|--------:|------|
| fio 单盘 CD8P `nvme1n1`（d8，twin，不经 vmem） | **6.465 GB/s**（128k QD64） | **7.569 GB/s**（QD128×4，1.85M IOPS） | `fio_cd8p_d8_*.json` |
| fio 双盘同时顺序 | **14.436 GB/s**（7.14+7.29） | — | `fio_cd8p_dual_seq.json` |
| `vmem_sw` PREFETCH_BATCH 真冷（本晚，16k 页） | 1.736 GB/s | 1.263 GB/s | `vmem_sw_bw_probe_20260831.log` |
| 锁定占用分母（此前真冷） | **1.950** | **1.560** | 仍用这个当 occ 分母 |

产品图 24 GB/s 是 device 内 NVMe→片上/CXL-DRAM。host 双盘顺序 14.4 已经是 PCIe/bio 之和，还是到不了 24，也不是 CXL.mem。

### 10.3 占用率（hide 真实搬运 ÷ 哪条峰）

锁定 hide T=1 nq=20：**0.600 GB/s**（`stat` 扇区）。

| 分母 | 占用 |
|------|-----:|
| 软件随机峰 1.560（锁定口径） | **38.5%** |
| 软件顺序峰 1.950 | 30.8% |
| 单盘 raw 4K 7.569 | **7.9%** |
| 双盘顺序 14.436 | 4.2% |
| device 内 CXL.mem 24 | **N/A（通路没了）** |

T=4 hide nq=20：**0.969 GB/s / 1.560 = 62.1%** 软件随机峰；相对 raw 4K 是 **12.8%**。

单路要到 oracle 85.8，有用字节约 12 MB/q × 85.8 ≈ **1.02 GB/s**，低于 1.560。所以 **T=1 不是带宽墙，是指针追逐延迟墙**。四路已经用到软件峰的 62%，再往 raw 7.6 走必须换数据面（BAR/HPS 或绕开 vmem_sw 4K ioctl）。

### 10.4 单路 50.25 / 19.8 → 85.8 / 11.4

锁定差：**0.59× QPS，+8.4 ms/q**。本晚 degree 镜像上 T=1 `--oracle-window` nq=20 跑到 95.91 / 10.36 / recall 0.925（`oracle_window_T1_nq20.log`）——**不替换** claim 85.8（布局不同，且 `pin_bytes` 仍只有 warm 的 6.5 MiB）。

+8.4 ms 拆开（锁定 nq=20）：

| 块 | ms/q | 占缺口 |
|----|-----:|------:|
| `crit_wait`（frontier 上硬等 NAND） | 1.24 | 15% |
| 其余（`stall_if_full` / install memcpy / hop *i* 打完才发 hop *i+1*） | ~7.2 | 85% |
| `overlap` | 0.07 | fill 几乎没盖住计算 |

还有空间、按优先级：

1. **Device 侧 neighbor walk**（HPS 活了才能做）：对症 19.8→11.4。host「打完 hop *i* 再 ioctl」锁死 overlap。
2. **不要在 T=1 上刷占用到 100%**：1.02/1.56 已经够 oracle 字节；再灌页是浪费。
3. **`issue_use` 64%**：少搬 36% 脏 sibling，减 install 税；入度序已证明无效。
4. **不要再试的**：`--score-page`、look=8、ebatch 16/32、score thread、共享窗四线程。

### 10.5 Continuous batching：oracle 吞吐 vs 79.02

`4 × 85.8 = 343` **不是**四路 oracle。本晚修好「每线程只 warm/跑自己的 query」之后，iso-recall（≥0.92）测到：

| 行 | QPS | mean ms | recall | NAND | 日志 |
|----|----:|--------:|-------:|------|------|
| T=1 oracle 锁定 | **85.8** | 11.4 | 0.925 | 0 | claim |
| T=4 shard oracle 512 MiB nq=20 | **136.23** | 15.8 | **0.930** | 0 | `oracle_window_T4_shard_nq20.log` |
| T=8 shard oracle 256 MiB nq=20 | 106.16 | 28.2 | 0.920 | 0 | `oracle_window_T8_shard_nq20.log` |
| Hide T=4 512 MiB nq=20 | **79.02** | 34.4 | 0.950 | 0.969 GB/s | 锁定 |
| Hide T=4 nq=100 | 87.21 | 29.3 | 0.941 | 1.027 | 锁定 |

VOID：把 100 条 WS 塞进同一 512 MiB（recall 0.28 / 0.40）；T=4 shard nq=100 187.41 但 recall **0.895**（低于 0.92）。

四路缩放：136.23 / 85.8 ≈ **1.59×**，不是 4×（CPU + 512 MiB + 跨 query 缓存）。

79.02 / 136.23 ≈ **0.58×**，和单路 50.25/85.8 ≈ 0.59× **同一缺口**：hide 没把 NAND 移出 query 路径。T=8 oracle 已经低于 T=4，再加线程不是优化。

下一步计划：`docs/superpowers/plans/2026-08-31-hide-toward-oracle.md`（恢复图序 bundle → `stop_join` 出 wall → `min-issue-use` → 四路 QD → 双盘门控 → HPS 活了再 device walk）。

四路还能挤的：

- 目标先是 **136 QPS / ~16 ms**，不是 343。
- 手段：跨 query 把 QD 拉满（软件峰还剩 38%）、修 `issue_use` 少搬脏页、device walk 去掉 per-hop ioctl。
- 共享 1 GiB ×4 已证明是锁车（50.71 QPS / 70 ms），不要重做。
- T=4 nq=100 hide 87.21 相对 79.02 已经好一点（更长稳态）；没有合法 nq=100 四路 oracle 可对（25 q/窗仍挤）。

---

## 11. Task 1：图序 bundle 已恢复（2026-08-31）

开机后又是双盘 `vmem_sw`（`nvme_devs=nvme1n1,nvme2n1`）。已 `rmmod` 后 `insmod` 树内单盘 `vmem_sw.ko`：`nvme2n1` / `0000:d9:00.0` / `expected_ssd_size_bytes=1920383410176`。pagebin `450971566080` magic **`0x314e415843`**。

`pack_nbr_bundle` 用 **`pagebin_graph.bin`**（不是 `_deg`）整份覆写 offset `460571635712`：`DONE nbr-bundle bytes=286720000000`（`pack_nbr_bundle_restore.log`，`nbr_bundle.json`）。约 33 min。

真冷 reload 到 `cache_used=0` 后 T=1 nq=20（`--graph-file pagebin_graph.bin`，`--no-score-page`，seed=42）：

| QPS | wall s | mean ms | recall@10 | from_win | issue_use | nvme GB/s | 日志 |
|----:|-------:|--------:|----------:|---------:|----------:|----------:|------|
| **49.92** | 0.401 | 19.990 | **0.940** | **100** | **63.33** | 0.570 | `hide_restore_nq20.log` |

`wall ≈ nq × mean`（无 `stop_join` 税）。issue_use 对照旧图序 64.30，本轮 63.33（同量级，未修利用率）。**claim 50.25 不改。** 后续 hide 替换行必须在这份图序 bundle 上测。

---

## 12. Task 3：`issue_use` 直方图 + `--min-issue-use`（未锁）

真冷 T=1 nq=20，图序 bundle，`--no-score-page`。直方图（不改发页，`hide_issue_hist_nq20.log`）：

`issue_use_hist=[0,.2):4183 [.2,.4):9372 [.4,.6):10655 [.6,.8):12279 [.8,1]:22094`

不是几乎全在 `[.8,1]`（约 38%），所以试了运行时跳过低 `u` 的 bundle 页、改走 `hide_collect_vec_pages`。`--min-issue-use N` = 阈值 N/100，默认 0。

| min_use | QPS | mean ms | recall@10 | from_win | issue_use | 日志 |
|--------:|----:|--------:|----------:|---------:|----------:|------|
| 0 | 48.77 | 20.502 | 0.945 | 100 | 63.30 | `hide_minuse_0_nq20.log` |
| 0.6 | 25.66 | 38.967 | 0.955 | 100 | 87.27 | `hide_minuse_60_nq20.log` |
| 0.8 | 14.43 | 69.309 | 0.960 | 100 | 97.89 | `hide_minuse_80_nq20.log` |
| 1.0 | 15.48 | 64.596 | 0.960 | 100 | 100.00 | `hide_minuse_100_nq20.log` |

胜出条件：recall≥0.92、from_win=100、issue_use>64.3、QPS≥48.3。0.6/0.8/1.0 的 issue_use 上去了，QPS 掉到 15–26（pagebin 随机页比顺序 bundle 贵）。**无胜出档；默认保持 0，不锁。** 未跑 nq=100 / T=4。claim 50.25 不改。

---

## 13. Task 4：四路 QD / 数据面（只服务 T=4，不改 claim）

现场：单盘 `vmem_sw` → `nvme2n1` / `0000:d9:00.0`，图序 bundle + `pagebin_graph.bin`。`use_shared_pool = P3 && !(per_thread_window && nthreads>1)`：T=4 四份日志都是 `per-thread windows=4`、`cont_batch=0`（没有误走共享池）。CLI 默认仍是 **ebatch=8 / ahead=2**。

真冷 `rmmod`+`insmod` 到 `cache_used=0` 后 mmap 了 pagebin 头 4 KiB 做 magic 核对，search_beam 启动行是 `cache_used=4096`（一页，不在 timed 窗口里当有用流量）。禁止 `--score-page` / `pipe_w=32` / `--cont-batch` / 共享 1 GiB。

| 行 | nq | QPS | mean ms | recall | from_win | nvme GB/s | occ | bytes/q | cap_qps | 日志 |
|----|---:|----:|--------:|-------:|---------:|----------:|----:|--------:|--------:|------|
| 锁定 T=4（claim，不改） | 20 | **79.02** | **34.4** | 0.950 | 100 | 0.969 | 62.1% | 12.26 MB | 127.2 | `hide_pagebin_bundle_thr4_512m_nq20.log` |
| 本轮默认 8/2 | 20 | 86.52 | 45.263 | 0.955 | 100 | 1.115 | 71.5% | 12.890 MB | **121.02** | `hide_T4_base_nq20.log` |
| `--issue-ahead 3` | 20 | 110.23 | 32.981 | 0.945 | 100 | 1.241 | 79.6% | 11.259 MB | 138.55 | `hide_T4_ahead3_nq20.log` |
| `--expand-batch 16` | 20 | 113.71 | 32.189 | 0.945 | 100 | 1.393 | 89.3% | 12.250 MB | **127.35** | `hide_T4_ebatch16_nq20.log` |
| 锁定 T=4 nq=100（对照，不是 136） | 100 | 87.21 | 29.3 | 0.941 | 100 | 1.027 | 65.8% | 11.77 MB | 132.5 | `hide_pagebin_bundle_thr4_512m_nq100.log` |
| 本轮默认 8/2 | 100 | 133.83 | 29.691 | 0.939 | 100 | 1.459 | 93.5% | 10.903 MB | 143.09 | `hide_T4_base_nq100.log` |

`bytes_per_q = nvme_read_B / nq`，`cap_qps = 1.560e9 / bytes_per_q`。

**cap_qps < 136：** 默认 8/2 nq=20 是 **121.02 < 136**。ebatch=16 是 **127.35 < 136**。ahead=3 字节顶 138.55，略高于 136，但测到只有 110.23（occ 79.6%），没有 nq=100 复测。**默认单盘路径到不了四路 oracle 136；Task 3 已失败，必须 Task 5（双盘抬峰）或再少搬字节。**

ahead=3 / ebatch=16 相对本轮 T=4 基线都抬了 QPS，但 **不改 CLI 默认**（全局 8/2 也服务 T=1；T=1 上这两档曾经无赢；本任务不碰 50.25）。不把 86.52 / 133.83 写成新 claim，也不把 133.83 当成 136 oracle。nq=100 墙钟 0.747 s vs 锁定 1.147 s、mean 几乎一样（29.7 vs 29.3），更像 Task 2 把 `stop_join` 移出 wall 的吞吐，不是 136。

---

## 14. Task 5：双盘软件条带 — BLOCKED-as-specified（2026-08-31）

按计划 Step 1 搜 `/root/chukexin/mem2nvme`：`backing_count` / `nvme_devs` / `validated 2 backing` → **none**。树内 `vmem_sw` 只有单盘 `nvme_dev`（`host/vmem_sw_main.c`），`open_backing` 开一块盘。开机双盘 `.ko` 仍不在树里。

**停。** 不 `insmod` 不明 `.ko`，不 `rmmod` 当前单盘，不 restage 9.6+267 GiB。`/dev/vmem0` 仍是单盘 `nvme2n1` / `0000:d9:00.0`。claim 不改。要做双盘必须先有可打开的源码，再经 controller 批准 restage。

---

## 15. Task 6：Track B BAR/HPS 门控 — SKIPPED（HPS dead，2026-08-31）

按计划只做 Step 1：`mmap` **4096 B** of `/sys/bus/pci/devices/0000:15:00.0/resource0`（`PROT_READ`），读前 16 B hex + 前 256 B 是否全 `0xff`。禁止扫 16 KiB。

**BAR peek：** 进程在第一次页故障上挂住（`python3` R 态 >100 s，无 stdout）。前 16 B **没读出来**，因此没有 hex、也没有「前 256 B 是否全 ff」的本轮实测。未重试、未放大 map、未 `HPS_PAGE_FETCH`、未写 device walk、未 `insmod vmem.ko`、未 `rmmod vmem_sw`。dmesg 无 AER 新行。

挂住之后只读了 config/sysfs 元数据（不碰 `resource0` 内容）：

| 项 | 值 |
|----|-----|
| `enable` | `1` |
| BAR0 range | `0x22f000000000`–`0x22f7ffffffff`（32 GiB） |
| driver | `ntcx` |
| ID | `1172:0000` |
| `setpci COMMAND` / `STATUS` | 都是 `ffff`（BAR hang 之后才读到；config 已不响应） |

结论：HPS 仍死。按计划「仍全 ff → 其余跳过」处理（挂死比全 `0xff` 更糟，不是活寄存器）。Step 2/3 不做。回去 Track A。禁止把 `0xff` / 挂死 peek 写成 GB/s。

---

## 16. Task 7：锁定新行的验收清单（2026-08-31，只读已有日志）

**总判：没有新 claim 行。** `eval.tex` 数字未动。  
**未替换：** T=1 hide **50.25** / **49.25**；T=4 hide **79.02**；T=1 oracle-window **85.8**。

替换门槛（计划 Task 7）：同一二进制、iso-recall（recall@10 ≥ 0.92）、`from_win=100`。T=1 必须 **nq=20 与 nq=100 都高于** 锁定 hide。T=4 nq=20 必须更高且 **mean 不炸**，对照是四路 shard oracle **136.23**，不是 85.8。

为什么本轮锁不上：

| 原因 | 证据 |
|------|------|
| T=1 nq=100 **没有 both-beaten** | nq=100 hide **本轮没跑**（SKIP）；nq=20 本轮 49.92 / 49.32 **都低于** 50.25 |
| T=4 默认 `cap_qps` **121 &lt; 136** | `hide_T4_base_nq20.log`：`nvme_read_B=257802240` → 12.890 MB/q → `1.560e9 / bytes_per_q` = **121.02** |
| `--min-issue-use` 失败 | §12：0.6/0.8/1.0 的 issue_use 上去、QPS 掉到 15–26；默认保持 0 |
| 双盘 BLOCKED | §14：树内无 `nvme_devs` 源码，未 restage |
| HPS 死 | §15：BAR peek 挂死；Task 6 其余跳过 |

T=4 `--issue-ahead 3` / `--expand-batch 16`：**VOID 当新默认**（CLI 仍 8/2）。T=1 历史上这两档无赢（§5.5）；本任务不锁、不改 50.25。

状态口径：**PASS** = 该对照行已锁、作分母；**FAIL** = 已测、达不到锁新行 / 达不到对照；**SKIP** = 本轮没跑，不编造。

### 16.1 必测五行

| # | 行 | 状态 | 日志 |
|---|----|------|------|
| 1 | T=1 nq=20 hide | **FAIL**（不锁；QPS &lt; 50.25） | `results/paper_figs/hide_restore_nq20.log`；复核 `hide_stopjoin_nq20.log` |
| 2 | T=1 nq=100 hide | **SKIP** | 本轮无 `hide_restore_nq100.log` / `hide_stopjoin_nq100.log`。不发明。旧锁定仍是 `hide_pagebin_bundle_ahead2_nq100.log` = 49.25 |
| 3 | T=1 oracle-window nq=20 | **PASS**（分母仍是锁定 **85.8**） | 锁定：`results/paper_figs/hide_matrix.csv` 行 `oracle_window_1GiB`（85.78 / 11.446 / 0.925，论文写 85.8）。本轮 `oracle_window_T1_nq20.log` = **95.91**（入度图 `pagebin_graph_deg.bin`）**不是**替换 |
| 4 | T=4 nq=20 512 MiB hide | **FAIL**（对 **136.23**，不是对 85.8） | `results/paper_figs/hide_T4_base_nq20.log` |
| 5 | T=4 shard oracle nq=20 | **PASS**（本轮已锁，不重跑） | `results/paper_figs/oracle_window_T4_shard_nq20.log` = **136.23** / recall **0.930** |

### 16.2 行 1 — T=1 nq=20 hide（FAIL）

图序 bundle + `pagebin_graph.bin`，ebatch=8 / ahead=2，`--no-score-page`，seed=42。占用分母 **1.560**。

| 源 | QPS | mean ms | recall@10 | from_win | issue_use | nvme GB/s | occ vs 1.560 |
|----|----:|--------:|----------:|---------:|----------:|----------:|-------------:|
| `hide_restore_nq20.log` | 49.92 | 19.990 | 0.940 | **100** | 63.33 | 0.570 | **36.5%** |
| `hide_stopjoin_nq20.log` | 49.32 | 20.274 | 0.945 | **100** | 63.41 | 0.562 | **36.0%** |
| 锁定 claim（不改） | **50.25** | 19.844 | 0.940 | 100 | —† | **0.600** | **38.5%** |

† 锁定行当时还没有 `issue_use` 字段（`hide_pagebin_bundle_ahead2.log`）。对照 issue_use 64.30 见 §6。  
49.92 / 49.32 都 **低于** 50.25。即使 nq=20 赢了，nq=100 本轮 SKIP，也不能 both-beaten。

### 16.3 行 2 — T=1 nq=100 hide（SKIP）

本轮没有 T=1 nq=100 hide。`hide_T4_base_nq100.log` 是四路，不算。不编 QPS / mean / recall。替换 50.25 **自动失败**（计划：nq=20 与 nq=100 必须同时更高）。

### 16.4 行 3 — T=1 oracle-window nq=20（PASS = 85.8 仍是分母）

| 源 | QPS | mean ms | recall | 处理 |
|----|----:|--------:|-------:|------|
| 锁定（原布局） | **85.8**（日志 85.78） | 11.446 | 0.925 | **claim，不改**。`hide_matrix.csv` / 笔记 §3 |
| 本轮 `oracle_window_T1_nq20.log` | 95.91 | 10.361 | 0.925 | 入度图；**不是** 85.8 的替换 |

本轮没有原布局 T=1 oracle 复测 `.log`。按计划「对照仍约 85.8」，用锁定行。

### 16.5 行 4 — T=4 nq=20 512 MiB hide vs 136.23（FAIL）

`hide_T4_base_nq20.log`（默认 8/2，`--per-thread-window`，4×512 MiB）：

| | QPS | mean ms | recall | from_win | nvme GB/s | occ vs 1.560 | cap_qps |
|--|----:|--------:|-------:|---------:|----------:|-------------:|--------:|
| 本轮默认 | 86.52 | **45.263** | 0.955 | 100 | 1.115 | 71.5% | **121.02** |
| 锁定 T=4 claim | **79.02** | **34.372** | 0.950 | 100 | 0.969 | 62.1% | 127.2 |
| T=4 shard oracle | **136.23** | 15.835 | 0.930 | 100 | 0 | 0 | — |

86.52 / 136.23 ≈ **0.64×**。`cap_qps=121.02 < 136`：单盘默认路径字节顶不到四路 oracle。QPS 虽高于 79.02，mean **45.3 vs 34.4（炸了）**，不满足「更高且 mean 不炸」，**79.02 不换**。不要拿 86.52 去跟 85.8 比。

### 16.6 行 5 — T=4 shard oracle nq=20（PASS，不重跑）

`oracle_window_T4_shard_nq20.log`：`throughput_QPS=136.23`，mean=15.835，recall@10=**0.9300**，`from_win_pct=100.00`，`nvme_read_B=0`，`freeze_fills=1`。本轮锁定分母，禁止再跑。

### 16.7 VOID：T=4 ahead=3 / ebatch=16 不当新默认

| 档 | QPS | mean ms | recall | nvme | occ | cap_qps | 日志 |
|----|----:|--------:|-------:|-----:|----:|--------:|------|
| `--issue-ahead 3` | 110.23 | 32.981 | 0.945 | 1.241 | 79.6% | 138.55 | `hide_T4_ahead3_nq20.log` |
| `--expand-batch 16` | 113.71 | 32.189 | 0.945 | 1.393 | 89.3% | **127.35** | `hide_T4_ebatch16_nq20.log` |

相对本轮 T=4 基线 QPS 更高，但 **VOID 当新默认**（全局 8/2 也服务 T=1；T=1 上 ebatch 16/32、ahead=3 曾经 ≤46、无赢，§5.5）。ebatch=16 的 cap_qps 仍 **127.35 &lt; 136**。不锁进 CLI，不写进 `eval.tex`。

### 16.8 Claim 对照（全部不改）

| Claim | 值 | 本轮 |
|-------|----|------|
| T=1 hide nq=20 | **50.25** | 未替换 |
| T=1 hide nq=100 | **49.25** | 未替换（本轮 SKIP） |
| T=4 hide nq=20 | **79.02** | 未替换 |
| T=1 oracle-window | **85.8** | 未替换（95.91 不是） |
| T=4 shard oracle（笔记分母，非 eval.tex） | **136.23** / 0.930 | 保持 |

---

## 17. 双盘 restage + T=4 vs 136（2026-09-01）

现场：kernel `6.18.0-rc5`。单盘 layout 先恢复（`vmem_sw.ko.6.18-single` → `nvme2n1`/`d9`）：pagebin magic `0x314e415843`，host `pagebin_image.bin` header+vec0 对齐。真冷 T=1 nq=20 = **47.77 / 20.9 ms / recall 0.945 / from_win=100**（`hide_layout_restore_T1_nq20.log`）。不替换 50.25。

然后 `REFUSE_DUAL_FORCE=1` restage：pagebin 9.6 GiB + 图序 bundle 267 GiB 经 `/dev/vmem0` 写入双盘 2 MiB 条带（`hide_dual_restage.log`，`DUAL_RESTAGE_OK`）。Victoryang 的 6.18 双盘 `.ko` **没有** `PREFETCH_BATCH`（ioctl `-ENOTTY`）。测量改用树内 `vmem_sw.ko.6.18-dual`（`srcversion=66204DB7…`，`nvme_devs` + batch）。

**新随机峰（真冷 PREFETCH_BATCH，两盘 rios 约 1:1）：**

| pages | nvme GB/s | 日志 |
|------:|----------:|------|
| 16k | 1.462–1.473 | 对单盘 1.560 的同口径 |
| 64k | 1.671 | |
| 128k | 1.711 | |
| **256k** | **1.744** | T=4 量级 QD 分母 |
| seq 16k | 2.037 | |

fio 14.4 **仍不是** occ 分母。256k 峰 1.744 → 12 MB/q 的 cap ≈ 145，刚过 136；16k 同口径双盘 **低于** 单盘 1.560。软件条带没有把随机峰抬到接近 2×。

**Hide（双盘，from_win=100，recall≥0.96，对照 136.23）：**

| 行 | QPS | mean | nvme | occ vs 1.744 | 日志 |
|----|----:|-----:|-----:|-------------:|------|
| T=1 8/2 | 44.93 | 22.3 | 0.528 | 30.3% | `hide_dual_T1_nq20.log` |
| T=4 8/2 | 96.36 | 39.5 | 1.264 | 72.5% | `hide_dual_T4_base_nq20.log` |
| T=4 ahead=3 | 98.97 | 37.7 | 1.301 | 74.6% | `hide_dual_T4_ahead3_nq20.log` |
| T=4 ebatch=16 | 100.54 | 37.4 | 1.399 | 80.2% | `hide_dual_T4_ebatch16_nq20.log` |
| T=4 ebatch16+spec16 | **107.92** | 34.7 | 1.398 | 80.2% | `hide_dual_T4_eb16_spec16_nq20.log` |

T=1 双盘低于单盘 47.77 / claim 50.25 → **双盘只作文 T=4 行**。单盘 T=4 ebatch=16 曾到 113.71，双盘宽管没有超过它。最好双盘 107.92 / 136.23 = **0.79×**。不替换 79.02 / 50.25。

`--spec-beam-nbrs M`（默认 0）已接到 `finish_score`：进 beam 且距序前 M 则异步发 N(u) bundle（`stall_if_full=false`）。T=4 单独 M=8/16/32 = 91/85/82（更差）。ebatch16+M=16 相对双盘 ebatch16 到 107.92，仍远低于 136；page_use 掉、字节涨。**默认保持 0。** CLI 默认仍 ebatch=8 / ahead=2。

---

## FPGA HPS probe (2026-09-01)

`hps_status` = `raw=0xffffffff unsupported`. `vmem.ko` was **not** loaded. `allow_hps_mmio` written back to 0. **HARD STOP**: Task 4–6 live switch is stopped.

## FPGA 三角色（2026-08-31）
- 原计划：CXL-SSD = d8+d9；CXL-DRAM = BAR0 via `vmem.ko`；Host = 2 GiB searcher
- **2026-09-02 修订：** 打分窗继续在 host；逻辑继续在 `search_beam`。不下放 FPGA，因此不依赖 HPS。

HPS probe 2026-09-01 was `raw=0xffffffff unsupported`；`vmem.ko` / `--switch` / mmap `resource0` 仍停放。

## Host-window 公平 Oracle（2026-09-02）

规则：T=1 与 T=4 **同一份** host 2 GiB DramWindow（`--dram-backend numa`，`--shared-window`，`--cpu-affinity`，work-steal）。CXL-SSD = 单盘 `vmem_sw` on d9（pagebin `CXAN1` @ 420 GiB）。不计 BAR。脚本：`tools/run_oracle_host_window.sh`。

| T | 窗 | QPS | mean | recall@10 | from_win | NAND | 日志 |
|--:|----|----:|-----:|----------:|---------:|------|------|
| 1 | 2 GiB host 共享 | **96.59** | **10.351** | **0.925** | 100 | 0 | `oracle_host2g_T1_nq20.log` |
| 4 | 2 GiB host 共享 | 132.16 | 26.689 | 0.900 | 100 | 0 | `oracle_host2g_T4_nq20.log` |
| 4 rerun | 同上 | 128.41 | 27.893 | 0.925 | 100 | 0 | `oracle_host2g_T4_nq20_rerun.log` |
| 8 | 同上 | 128.22 | 54.943 | 0.900 | 100 | 0 | 首跑（后被复跑覆盖） |
| 8 复跑 | 同上 | 114–131 | 53–62 | 0.910–0.920 | 100 | 0 | `oracle_host2g_T8_nq20.log` / `_rerun.log` |
| 16 | 同上 | 125.46 | 110.715 | 0.885 | 100 | 0 | 首跑（后被复跑覆盖） |
| 16 复跑 | 同上 | 117 | 120 | 0.910–0.920 | 100 | 0 | `oracle_host2g_T16_nq20.log` / `_rerun.log` |
| 32 | 同上 | 103.46 | 163.500 | 0.930 | 100 | 0 | `oracle_host2g_T32_nq20.log` |

T≥4 共享窗 recall 会抖（0.885–0.930），**T=8/16/32 都不锁成 claim**。QPS 在 T=4 附近封顶（~128–132），再加核 mean 近似线性涨、吞吐掉：T=8 ~114–131 / ~55 ms；T=16 ~117–125 / ~111–120 ms；T=32 **103.46 / 164 ms**。nq=20 时 T=32 有空闲线程，墙钟仍被共享窗锁拉长。`4×` / `8×` / `32×` 96.59 都不是上界。

**不替换：** T=1 hide **50.25** / **49.25**；T=1 oracle 1 GiB **85.8**；T=4 shard 4×512 MiB **136.23**。新行是「同 2 GiB + 加核」的附加对照，不是 85.8 的替代。`4×96.59` 也不是四路上界。

`cxl_dram_hit_pct` 打印名是旧字段（窗口 hit），不是 FPGA CXL-DRAM。真计数：`from_cxl_dram=0`，`mapped HOST DRAM`。
