# CXL–SSD ANNS 研究计划（ASPLOS 叙事）

**推荐标题方向：** *Stall-Aware Residency for Graph ANNS on Unified CXL–SSD Memory*

---

## 0. 论文故事线（总览）

```
机会：为何把 CXL–SSD 引入 ANNS？
      → 相对 replicated SSD 副本降低数据冗余；相对 full-in-memory CXL-DRAM 把冷库压到闪存价
问题：引入之后多出了什么新问题？（本文要解决的）
      → 统一 VA 下的 residency/stall 失控（双台阶、页错配、盲目升迁、efSearch↔miss）
Findings → Challenges → Design → Evaluation
```

**审稿人 30 秒版（必须能背）：**

| # | 问题 | 一句话回答 |
|---|------|------------|
| 1 | 为何引入 CXL–SSD？ | 常见 replicated DiskANN 扩吞吐要付整库复制/分片税；CXL-ANNS/Cosmos 类全库进 CXL-DRAM 又让冷数据付 DRAM 价；CXL–SSD 在「共享一份库」与「闪存价容量」之间给出折中部署点（机会）。 |
| 2 | 引入后有何问题？ | 冷读 ~80 µs（~100×）；按页缓存 QPS÷3.6；图乱预取 QPS÷5；加大 efSearch 命中率↓——**三种「默认用法」都会把 stall 放大**。 |
| 3 | 本文解什么？ | 在 SSD-backed CXL 地址空间上做 graph-ANNS 的语义 residency / 有界升迁 / stall-aware search（系统贡献）。 |

---

## 1. 为什么把 CXL–SSD 引入 ANNS？引入后又多出什么问题？

### 1.1 为何需要把 CXL–SSD 引入 ANNS？（机会 / Opportunity）

Billion-scale ANNS 必须同时扩展两个**基本独立**的维度：**语料容量**与**查询吞吐**。一个核心部署问题是：

> **加查询算力时，能不能不再按 \(R\) 倍复制整库，也不把冷数据全体抬到 DRAM 价？**

CXL–SSD 进入 ANNS 的价值，正是针对下面两个对照部署点给出折中；**它不是「让 NAND 变得和 DRAM 一样快」**。

#### （1）存储冗余问题：相对 replicated SSD-based ANNS

DiskANN 等单机外存图 ANNS 的常见服务单元是：

```text
one query replica ≈ CPU + DRAM working set (PQ / node cache) + local SSD index
```

当查询量上升、需要 \(R\) 个查询 worker 时，**常见**只有两条路：

1. **整库复制：** 每台挂一份 SSD 索引 → provisioned 容量 \(\approx R \times D\)（\(D\) 为全精度图+向量体积）；TB 级时复制税不可接受。  
2. **分片：** 每台只持有一部分 → 跨分片 fan-out / 路由 / 负载不均，抬高尾延迟。

即便换 **NVMe-oF**，也只是把本地盘 I/O 换成远端块 I/O；**每台主机仍各自维护 page/buffer cache**，并没有取消「加算力 ≈ 再铺一份（或再切碎）存储」的税。

**必须降调的边界：**

- 这是对 **“per-worker replicated local-SSD”** 部署的批评，**不是**「DiskANN 算法必然复制」。  
- [DistributedANN](https://arxiv.org/html/2509.06046) 已把单个逻辑 DiskANN 图放到分布式 KV 上服务（Bing 生产经验），说明「每个 worker 必须拥有本地全库」不是算法必然条件；但其共享介质仍是 **块/KV 语义**，每跳仍可付网络往返，且仍可能为 HA / head index / 压缩码本保留额外副本。  
- 因此 CXL–SSD 相对这条线的机会，应写成：**有机会减少 provisioned replication，并以 memory-semantic 共享同一逻辑索引**；不要写成「DiskANN 只能复制」。

#### （2）存储成本问题：相对 full-in-memory CXL-DRAM ANNS

以 CXL 做 disaggregated ANNS 的已发表主线，后端几乎都是 **DRAM 类介质**：

| 工作 | 介质与部署 | 与「成本折中」的关系 |
|------|------------|----------------------|
| **CXL-ANNS** (ATC’23 / TOCS’24) | Type-3 **CXL DRAM 池**；essential graph + embedding 放入 HDM | 解决可扩展全精度库，但冷尾也付 DRAM 价；多 host 实验是 **分区后聚合 top-k**，不是多 host 同区 coherent 读同一份 HDM |
| **Cosmos** (CAL’25) | **Full in-memory** CXL DRAM + 设备侧 GPC/rank-PU | 进一步卸载计算，仍是 DRAM 容量定价 |
| **IKS / SpANNS / CMM-Ax / FaTRQ** 等 | CXL DRAM / Type-2 far-memory / PNM | 加速或分层残差，**不是**把全库图+向量放进 NAND-backed HDM |
| **Second-Tier Memory** (arXiv’24) | CXL/RDMA/NVM 作第二层 | 解决 SSD 4KB 粒度 vs 细粒度随机读，但 second-tier 仍是 **DRAM/NVM 价**，非 flash-priced CXL-SSD |

记 \(D\) = 全精度语料，\(H\) = 有效热点工作集；图 ANNS 通常 \(H \ll D\)。对 **CXL-ANNS / Cosmos 这类 full-in-memory baseline**：

$$
\mathrm{MediaCost}_{\text{CXL-DRAM}} \;\sim\; D \cdot c_{\mathrm{DRAM}}
$$

共享内存池可以做到逻辑 **one-copy**，却**没有**把容量定价压到闪存。大量几乎不碰的冷向量仍占用昂贵 DRAM，造成 **stranded memory** / 功耗与采购浪费（Bauhaus 等对 CXL DRAM 分层的动机也指向同一问题）。


#### （3）CXL–SSD 的折中部署点

CXL–SSD（Type-3 memory-semantic SSD：NAND 容量层 + 设备/主机 DRAM 热窗口 + `load/store`）把「闪存价容量」与「可缓存内存语义」放到同一地址空间层级（硬件叙事见 From Block to Byte、SkyByte、CMM-H 等）。

**定性对照（不做具体采购价断言；需后续 provisioning 曲线验证）：**

| 部署点 | 扩吞吐时的容量税 | 冷库介质价 | 访问语义 |
|--------|------------------|------------|----------|
| Replicated local-SSD DiskANN | 常 \(\propto R\) 复制，或分片 fan-out | flash | 块 I/O + 每 host 独立 cache |
| Full-in-memory CXL-DRAM ANNS | 可为 one-copy（池化/共享） | **DRAM** | CXL.mem |
| **CXL–SSD ANNS（目标）** | 单 host：一份库；**Shared FAM**：多 host 同区一份库 | **flash + 有限 cache** | CXL.mem（miss → NAND） |

对 CXL–SSD，更合理的**介质容量**叙事是：

$$
\mathrm{MediaCost}_{\text{CXL-SSD}} \;\sim\; n_{\mathrm{HA}} D \cdot c_{\mathrm{flash}} + H \cdot c_{\mathrm{cache}} + C_{\mathrm{fabric/controller}}
$$

其中 \(n_{\mathrm{HA}}\) 是高可用副本数（通常 \(\ll R\)），**不是**简单的 \(D\cdot c_{\mathrm{flash}}\)。相对 replicated DiskANN，机会在于 **减少随 \(R\) 增长的 provisioned 冗余**；相对 CXL-DRAM full-in-memory，机会在于 **冷库按 flash 计价**。两者合在一起，才是「折中」。

<!-- **必须分清三种 CXL 条件（写 Intro 时禁止混写）：**

1. **单 host direct-attach CXL–SSD：** 已能做「闪存价 + memory-semantic」；相对「NVMe + record cache」的差异主要在接口与 residency，**不能**单独支撑 multi-host one-copy。  
2. **CXL 2.0 pooling / MLD：** 多个 host 可共享**物理设备**，但同一 HDM 区通常**同一时刻只属于一个 host**——这是 pooling，不是 concurrent sharing。  
3. **CXL 3.x Shared FAM / GFAM：** 多个 host 才能**同时**映射同一区域；这才是严格 **multi-host one-copy** 的标准前提（还依赖 MHD/GFAM、Fabric Manager、可选 HDM-DB / Back-Invalidate，以及只读索引发布协议）。 -->

#### 与最接近工作的划界（机会成立 ≠ 已有同构系统）

公开检索（至 2026-08）**未发现**同时满足下列全部条件的 peer-reviewed 系统：  
**(i)** NAND-backed memory-semantic CXL 地址空间上直接服务 graph ANNS；**(ii)** 针对图遍历的 residency / promote / stall 控制；**(iii)**（若主张 multi-host）Shared FAM 上的共享只读索引。

相邻但**不是同构**的工作：

| 工作 | 做了什么 | 为何不是本文机会点的完成态 |
|------|----------|----------------------------|
| CXL-ANNS / Cosmos | CXL + ANNS | 后端是 **DRAM**，不是 SSD-backed HDM |
| Second-Tier / FaTRQ | 远层 / 分层残差 | 远层仍是 CXL DRAM 或 NVM，不是 flash-priced CXL-SSD 全库 |
| DistributedANN / NVMe-oF DiskANN | 共享或远端 SSD 索引 | **块/KV** 语义，非统一 CXL.mem VA |
| SkyByte / From Block to Byte / CMM-H | CXL-SSD 硬件与通用栈 | **无** graph-ANNS residency 设计 |
| **CXL-AnySSD** (UIUC thesis’25) | 真实 Type-2 CXL-SSD；vector-DB workload 上相对 OS swap 提速 | 主线是 **composable SSD + 去 swap**，不是 graph-ANNS 语义缓存/升迁/stall |
| **P-HNSW** | 持久内存上 crash-consistent HNSW；文中提及 CXL-SSD | 重点是 **一致性与恢复**；实验用 DRAM 模拟 PM，不是 CXL-SSD 上的 serving residency |

<!-- **受限的 “first” 表述（推荐用于 Intro，禁止更宽）：**

> To our knowledge, this is the first **graph-ANNS serving system** that **directly operates over an SSD-backed CXL address space** and **jointly manages ANNS-specific residency, promotion, and search stalls**.

不要写成「第一个把 CXL-SSD 用于向量数据库 / ANNS 的工作」。 -->

**一句话（Why introduce）：**  
常见 replicated DiskANN 扩吞吐要付 **存储冗余税**；CXL-ANNS/Cosmos 类方案用 CXL 做 disaggregation，却让冷库付 **DRAM 容量税**；CXL–SSD 提供折中：**闪存价承载全精度库 + memory-semantic 热路径**，并在 Shared FAM 条件下让多查询节点共享**一份**逻辑索引——这是**部署机会**，不是本文系统贡献。系统贡献是：在该介质上控制 graph ANNS 的 residency / stall。

<!-- #### 论据成立的前提（写 Intro / Evaluation 时必须交代）

1. **分条件陈述部署模型：** 单 host 论证「闪存价 + 内存语义」；multi-host one-copy **仅**在 CXL 3.x Shared FAM/GFAM（或等价共享区域）下主张；禁止把 CXL 2.0 pooling 写成同区共享。  
2. **成本只做定性 + provisioning 曲线：** 展示随 \(R\)、\(H/D\)、\(n_{\mathrm{HA}}\) 相对「replicated DiskANN / full-in-memory CXL-DRAM」的交叉点；检查共享 CXL–SSD 带宽是否成为中心化瓶颈；**不要**直接宣称端到端 TCO 更低。  
3. **只读查询与更新/HA 分开：** one-copy 首先针对只读 serving；索引发布、版本切换、metadata ownership、HA 副本仍可能 \(n_{\mathrm{HA}}>1\)。  
4. **实验平台对齐：** 本机 `/dev/vmem0`（host-NVMe software 路径）用于 **residency/stall 机制研究**；§1.1 的 fabric one-copy 是最终部署叙事，论文须显式声明当前平台为 proxy。 -->

### 1.2 引入之后有什么问题？

**事实 1 — 冷读很贵：对照 CPU 直读 SSD + 整次 search**

| 路径 | ns/向量 |
|------|---------|
| vmem cold | ~82 µs |
| vmem warm | ~2.2 µs |

完整 ANNS Query vs. DiskANN(LAION-1M)：

| 系统 | 每 query 延迟（约） | 每 query 盘 IO | 机制 / 驻留 |
|------|---------------------|----------------|-------------|
| DiskANN SSD（有 PQ） | **1.2–4.5 ms**（随 L） | **~16–62** | PQ 导航；aio beam；node cache |
| DiskANN SSD（无 PQ 导航） | **4.9–15.7 ms**（随 L） | **~472–1275** | 盘上 FP 排序；仍 aio/cache |
| **vmem cold SSD** | **~44 ms**（0.703 s / 16 q） | **~490**（7876/16） | 无 PQ；串行 fault；**几乎全 soft-miss→NVMe** |
| **vmem soft（PTE 冷）** | **~4.0 ms**（0.065 s / 16 q） | **~0** | 数据在 soft-cache；fault 装 PTE，几乎不读盘 |
| **vmem warm（PTE 热）** | **~3.4 ms**（0.055 s / 16 q） | **0** | **全 path_hit**；无 fault / 无 SSD |

**事实 2 — 按页缓存会系统性做错（F2，暂不改）**

| 策略 | QPS | SSD `read_ios` |
|------|-----|----------------|
| 按向量 record 缓存 | **33.5** | **9.9k** |
| 按 16 KB「页」 | 9.3 | 36.5k |

即 OS/SSD 习惯的「按页装」在图 ANNS 上会多读 ~3.7× 盘、QPS 掉到约 1/3.6。

**事实 3 — 照搬图预取更慢 (page 预取)**

| L | QPS | Δread_ios | sec_pref | sec_touch | precision | leftover 未用 |
|---|-----|-----------|----------|-----------|-----------|---------------|
| 0 demand | **21.7** | 8177 | 0 | 0.72 | — | 0 |
| 1 | 20.0 | 8760 | **0.40** | 0.38 | 88% | 622 |
| 2 | 17.0 | 10817 | **0.73** | 0.19 | 70% | 2990 |
| 8 | **5.2** | **37397** | **3.03** | 0.05 | **22%** | **31254** |

**原因：**

1. **`PREFETCH_LOGICAL` 是同步的**：L=8 时 pref 占墙钟 ~3.0 s / 3.1 s，搜索 touch 反而不到 0.05 s。  
2. **大量预取从未被用到**（leftover 3.1 万页），挤占 4 GiB soft-cache → 更多真实 SSD IO。  
3. **PTE hit% 不变（~49%）**：预取没变成更高「已建映射」命中，只是多付 IO。

**事实 4 — 加大 efSearch - 没什么特别的变化**

| efSearch | QPS | hit% | recall@10 | unique_ids | fetches/q | ssd_reads |
|----------|-----|------|-----------|------------|-----------|-----------|
| **100** | **7.82** | 56.8 | **0.663** | **22125** | **3201** | **21723** |
| 200 | 4.59 | 62.1 | 0.800 | 38816 | 6401 | 38118 |
| 400 | 2.67 | 68.6 | 0.869 | 64315 | 12801 | 63196 |
| 800 | 1.65 | 75.6 | 0.900 | 100113 | 25601 | 98359 |
| **1600** | **1.10** | 82.9 | **0.931** | **140182** | **51201** | **137691** |

**事实 5 — 查询热点：同 efSearch 对照 cache vs nocache**
- 32MB cache (8192个vector)

| efSearch | mode | QPS | hit% | ssd_reads |
|----------|------|-----|------|-----------|
| **200** | no-cache | 4.58 | 62.2 | 38016 |
| **200** | cache | **5.78** | **69.9** | **30309** |
| 400 | no-cache | 2.62 | 68.1 | 64194 |
| 400 | cache | **3.06** | **72.1** | **56148** |
| 800 | no-cache | 1.64 | 75.1 | 100260 |
| 800 | cache | **1.79** | **77.1** | **92214** |
| **1600** | no-cache | 1.08 | 82.4 | 141344 |
| **1600** | cache | **1.16** | **83.4** | **133298** |

---

### 详细信息

**（B）官方 DiskANN 整次 search（同 200k×1024，索引在 `/mnt/disk0` NVMe，`search_disk_index`，T=1，W=4，cache=10k）：**

| L | Mean Latency | Mean IOs | QPS |
|---|--------------|----------|-----|
| 10 | **1.38 ms** | 16.1 | 683 |
| 20 | **1.80 ms** | 24.1 | 531 |
| 32 | **2.48 ms** | 34.3 | 389 |
| 50 | **2.61 ms** | 50.0 | 370 |
| 64 | **4.49 ms** | 62.5 | 219 |

**（B1）SSD DiskANN 去掉 PQ 导航（改写版，同索引同 W=4 / cache=10k / T=1）：**

> 官方 SSD 路径关不掉内存 PQ。我们在 `PQFlashIndex` 加了 `--no_pq_nav`：  
> **邻居排序改为盘上全精度距离**（coord cache 命中则免 IO；否则按 sector 批量读），仍保留 aio beam 展开与 node cache。  
> PQ 码表仍加载但不参与导航。导航读与后续 expand 可能对同一节点各读一次（未做跨阶段 sector 复用）→ IO 偏上界。

| L | Mean Latency | Mean IOs | QPS |
|---|--------------|----------|-----|
| 10 | **4.87 ms** | **472** | 202 |
| 20 | **7.01 ms** | **636** | 141 |
| 32 | **9.98 ms** | **818** | 99 |
| 50 | **13.2 ms** | **1079** | 75 |
| 64 | **15.7 ms** | **1275** | 63 |

日志：`/mnt/disk0/chukexin_motivation/diskann_data/search_T1_W4_nopq.log`  
相对有 PQ：延迟约 **3.5–4×**，IO 约 **20–30×**。



#### 给审稿人的一句话

> **CXL–SSD 冷 miss ≈ CPU 直读 SSD（~80 µs）；整次 cold search 相对 warm 慢约 13×，且 ~94% 时间在 SSD path。页式缓存、同步图预取（同步 ioctl + 低 precision 污染）、以及在未饱和前加大 efsearch，都会继续放大 stall。本文要解决的是：在该层级上如何选缓存对象与搜索旋钮，避免这些用法。**

---


### 1.3 问题清单 = 「存在 + 重要」双栏（写作用）

每个问题必须同时回答：**（A）数上存在吗？（B）错了有多痛？**

| ID | 问题（一句话） | 存在性数据（A） | 重要性数据（B） | 若不管会怎样 | 还缺的加硬实验 |
|----|----------------|-----------------|-----------------|--------------|----------------|
| **P1** | 冷向量 load 比热向量慢两个数量级 | 78–83 µs vs 0.6–2 µs | 搜索每 hop×degree 次 load；冷比例稍升则延迟被 miss 主导 | 系统当「略慢的 DRAM」用会全错 | 扫 miss 比例→查询延迟分解 |
| **P2** | 页式缓存与图邻接不匹配 | 同预算下 page 与 record 行为分裂 | QPS **3.6×**、IO **3.7×**（1024-d） | 默认 OS/页思维在高维上直接不可用 | M-E2：小 dim 下差距缩小（record≈page）→ claim 限定「高维/近页大小」 |
| **P3** | 图预取/过度升迁污染热窗口 | hit% 不变，IO×5，precision→29% | QPS **÷4–5** | 有 prefetch 接口反而更慢 | 画「promote 字节 vs QPS」帕累托；标出有界甜蜜点（M-E3） |

### 1.3.1 机会 / 问题 / 贡献（防混写）

```
机会：CXL–SSD = one-copy + 闪存价容量 + load/store 热路径（解耦算力扩展与整库复制）
        ↓
问题：同一 VA 上冷读极贵 + 页缓存/乱预取/乱加大 beam 会再糟数倍
        ↓
贡献：语义驻留（record/hub/cache）+ 有界升迁 + stall-aware 搜索，打在 iso-recall 上
```

### 1.3.2 Motivation 实验设计



### 1.4 Related Work（细对比）

比较维度固定为 8 列，避免「他们做 ANNS、我们也做 ANNS」式空话：

| 维度 | DiskANN / FreshDiskANN | Second-Tier Memory | CXL-ANNS | Cosmos / CMM-Ax 类 | FaTRQ | **本文** |
|------|------------------------|--------------------|----------|-------------------|-------|----------|
| **全精度放哪** | NVMe SSD（块） | 倾向 CXL/RDMA/NVM 第二层 | **全部 CXL DRAM** | CXL 设备内存 | 远层精炼，可分层量化 | **默认 CXL–SSD/闪存 VA**；热集 cache |
| **图放哪** | 常与向量一起在盘上 sector | 第二层内存 | CXL；热 hop 可在本地 DRAM | 设备侧 | 视系统 | **主机 DRAM**（不与向量抢 soft-cache） |
| **慢路径接口** | `pread` / async 块 I/O | load/store 远内存 | CXL.mem load + DSA | 设备核 + 近存 | 精炼读远层 | **fault / soft-cache / 可选 prefetch ioctl** |
| **假设容量** | 闪存够大 | second-tier 装得下索引 | CXL 池装得下全库 | 多设备拼容量 | 远层存在 | **闪存必须**；不假设全库进 CXL DRAM |
| **核心优化对象** | 减少 round-trip、吃满 IOPS、PQ 导航 | 打破「4K↔小随机读」迫使的索引放大 | 掩盖 CXL 百 ns + 近存距离 | 卸载遍历/距离 | 减少精炼字节 | **residency 精度、共享 cache、beam↔stall** |
| **缓存/升迁单位** | 4K sector / 静态入口 cache | 细粒度远存访问 | 关系感知图缓存 + candidate 预取 | 设备内放置 | 码本层级 | **向量 record / hub / frontier-top**（反页、反盲 2-hop） |
| **质量旋钮含义** | beam≈I/O 批宽 W | 较少谈 stall×beam | 调度 urgent/defer | 主机只发查询 | 量化层选择 | **beam = 工作集温度；iso-recall 控制** |
| **本文是否重复** | 否：我们不是「更好的块 I/O 调度」 | 否：我们不主张换掉闪存 | 否：我们不主张全进 CXL | 否：无 Type-2/PNM 依赖 | 正交：可叠加量化 | — |

**逐条「审稿人会问 / 我们答」：**

1. **“你们不就是 DiskANN 换成 mmap 吗？”**  
   DiskANN 的一等公民是 **I/O 队列与 sector 布局**；我们的一等公民是 **cache/promote/分层 miss 与 iso-recall**。同机对照应显示：在统一 VA 上套页缓存或盲预取会变差，而语义驻留变好（F2/F5/F7）。

2. **“Second-Tier 已经用 CXL 做向量了？”**  
   Second-Tier 的解药是 **用 CXL 内存替代 SSD 以匹配小随机读**；我们承认闪存仍必要，研究 **闪存挂成 CXL–SSD 之后** 的控制平面。介质假设相反。

3. **“CXL-ANNS 不也有图缓存和预取吗？”**  
   他们优化的是 **CXL DRAM 远置延迟（~百 ns）** 与 DSA；我们优化的是 **闪存 miss（~十~百 µs）+ soft-cache 污染**。延迟差两个数量级，预取/缓存策略不可照搬（F1 已表明照搬会亏）。

4. **“Cosmos 全卸载不是更彻底？”**  
   需要设备侧 GPC/rank-PU；我们面向 **Type-3 式内存扩展 + CXL–SSD 软件/设备栈**，贡献在主机侧 residency 运行时，硬件假设不同。

5. **“FaTRQ 呢？”**  
   正交：他们减精炼字节；我们减 **错误驻留与失控 beam** 带来的 stall。可组合，不互相替代。

**Related Work 写作模板（每篇 4 句）：**  
（1）他们假设数据在哪；（2）慢路径是什么接口；（3）优化的延迟量级/目标函数；（4）我们因何不同（一句话差异）。

---

## 2. CXL–SSD ANNS 有什么问题？（展开：即 §1.2 的详细版）

> 接上 CXL–SSD **并不自动变快**；统一 VA 暴露出新的系统问题（这才是论文机会）。  
> 下面 P1–P4 是对 §1.2 问题清单的展开，每条直接喂 Design。

### 2.1 问题 P1：深分层悬崖（双台阶）

- Soft-cache / 设备热层命中 ≠ 主机 PTE 命中（F8：~83 µs / ~3.8 µs / ~2.2 µs）。  
- 只看 mincore/PTE hit% 会 **掩盖** soft-cache 层的得失（解释 F1）。

### 2.2 问题 P2：页语义与图语义错配

- 邻接在 ID 空间不连续；按页/连续块 admission 放大 SSD 流量（F2/F6）。  
- 沿用 OS/SSD 页思维会系统性做错。

### 2.3 问题 P3：升迁精度 vs 带宽

- Sync / 高覆盖图预取：hit% 不变，`read_ios` 暴涨，QPS 下降（F1/F5）。  
- **错误升迁代价 > 漏升迁**；需要有界高精 promote，不是「多预取」。

### 2.4 问题 P4：检索旋钮与 stall 强耦合

- beam↑ → 工作集变冷 → hit↓、SSD↑、QPS↓（F3）；recall 效率非单调（F4）。  
- 跨 query 共享热集远比单 query 加大 beam 有效（F7）。  
- 朴素「miss 高就降 beam」不够（F9）→ 需要 iso-recall / 预算型控制器。

### 2.5 本节产出

- [ ] Problem 小节按 P1–P4 组织，每条挂一条已测 finding  
- [ ] Challenges 列表（见下一节）与 Design 模块一一对应

---

## 3. Findings / Challenges（Motivation → 设计约束）

### 3.1 已验证 Findings（可写进 Motivation）

| ID | 结论 | 对设计的约束 |
|----|------|--------------|
| **H0** | hit/miss ~140× | 系统必须显式管理 miss；不能当均匀内存 |
| **F8** | 双台阶：SSD / soft+PTE冷 / PTE热 | 代价模型分三级；promote 目标要说清到哪一级 |
| **F2/F6** | record/hub ≻ page；盲 graph_expand 有害 | Admission 按向量/图语义，禁止纯页策略 |
| **F5**（+F1 负） | 升迁精度 ≫ 体积 | Promote 有界、可计 precision；禁止高覆盖 2-hop |
| **F3/F4** | beam↔SSD/recall 耦合；效率需联合度量 | 主指标用 iso-recall，不用裸 QPS |
| **F7** | 共享热集 > 加深 beam | 一等组件：跨 query cache / hub set |
| **F9 负** | 朴素自适应 beam 不赢 | 控制器要 iso-recall 或 IO/时间预算，不能只看瞬时 miss |

### 3.2 Challenges（设计必须回答）

| Challenge | 表述 | 对应 Design 模块 |
|-----------|------|------------------|
| **C1 可见性** | 如何区分并暴露 L3 miss / soft hit / PTE hit？ | 计数与代价模型 |
| **C2 放置** | 图 / PQ / 全精度 / hub 各放哪一层？ | Placement |
| **C3 升迁** | 升谁、升多少、升到哪一级，且保持高 precision？ | Bounded Promote |
| **C4 共享** | 多查询下如何维护 cachened 热集而不炸 cache？ | Shared cache |
| **C5 质量** | 如何在 iso-recall 或固定 IO 预算下调 beam/早停？ | Stall-aware Search |
| **C6 诚实对照** | 与 DiskANN 块路径比时，后端与数据集如何公平？ | Evaluation 协议 |

### 3.3 本节产出

- [ ] Motivation 2–3 页 + 表：Finding → Challenge → Design knob  
- [ ] 补强实验（可选，投稿前）：  
  - [ ] F4 加大 hops / 固定 SSD budget 截断，强化「假 recall」  
  - [ ] F8 在真 CXL BAR / 非 soft 路径复测（若硬件就绪）  
  - [ ] 与 DiskANN 同库同 recall 的小规模对照原型

---

## 4. Design

### 4.1 架构总览

```
┌─────────────────────────────────────────────────────────┐
│  ANNS Runtime（用户态）                                  │
│  ┌──────────────┐ ┌──────────────┐ ┌─────────────────┐ │
│  │ Placement    │ │ Promote Ctrl │ │ Search Ctrl     │ │
│  │ 图/PQ/向量   │ │ 有界精确升迁 │ │ beam / 早停     │ │
│  │ hub 初始 cache │ │ precision 预算│ │ iso-recall 环   │ │
│  └──────┬───────┘ └──────┬───────┘ └────────┬────────┘ │
│         │                │                   │          │
│  ┌──────▼────────────────▼───────────────────▼────────┐ │
│  │ Residency Plane：cache / uncache / promote / stats      │ │
│  └──────────────────────┬─────────────────────────────┘ │
└─────────────────────────┼───────────────────────────────┘
                          │
     ┌────────────────────┼────────────────────┐
     ▼                    ▼                    ▼
 本地 DRAM            Montage CXL           /dev/vmem0
 (图, PQ, 状态)       (可选热全精度)         (全精度默认;
                                            soft-cache+闪存)
```

### 4.2 模块 D1 — Placement（数据怎么放）

| 数据 | 默认位置 | 理由 |
|------|----------|------|
| 图邻接 | 本地 DRAM | 每 hop 必碰；勿与向量抢 soft-cache |
| PQ / 码本 | 本地 DRAM | 极热、小 |
| 查询/候选/visited | 本地 DRAM | 工作集 |
| 全精度向量 | vmem/闪存（默认） | 容量 |
| Hub / 跨 query 热集 | 显式 cache → soft-cache 或 Montage | F7 |
| Frontier 顶部候选邻接 | 有界 promote（非全 2-hop） | F5 |

- [ ] 写清与 DiskANN「图也在盘上 sector」的差异：本设计 **图在内存**  
- [ ] 可选：热全精度进 Montage 的阈值与迁移 API

### 4.3 模块 D2 — Bounded Precise Promote（C3）

**策略草图：**

1. 仅对 **即将扩展的 frontier top-M** 的邻接做 ensure/promote。  
2. 全局 **promote 字节预算** / 每查询预算；超额则降级为 demand。  
3. 目标层级可选：soft-cache only vs PTE install（对应 F8 两级）。  
4. 在线估计 precision：`used / promoted`；低于阈值则收缩 M。

**禁止：** 高覆盖 sync 2-hop、OpenMP 乱 fault 充 QD（F1 已证明有害）。

- [ ] API：`promote(ids[], level, budget)` / `cache(ids[])` / `stats()`  
- [ ] 实现路径：先用户态 + `VMEM_IOC_PREFETCH_LOGICAL` + touch；再评估是否需内核扩展

### 4.4 模块 D3 — Shared cache Set（C4）

1. 短窗口统计跨 query 访问频率或入度 hub。  
2. 固定字节预算 cache（如 soft-cache 的 10–50%）。  
3. 与单查询 beam 解耦：默认 **较小 beam + 共享 cache**（F7）。

- [ ] 替换策略：LFU / 老化；避免 cache 集合僵死  
- [ ] 多租户/批查询负载生成器

### 4.5 模块 D4 — Stall-aware Search Controller（C5）

**目标：** iso-recall@k 下最大化 QPS（或固定时间/IO 预算下最大化 recall）。

**不是 F9 那种瞬时 miss 砍 beam**，而是例如：

1. 离线/在线标定：beam → recall、IO 曲线（利用 F3/F4）。  
2. 运行时：在 recall 下界约束下选最小 IO 的 beam；或  
3. 早停：边际 recall 增益 / 边际 IO 低于阈值则停。  
4. 与 D3 联动：cache 命中率高时允许略增 beam。

- [ ] 控制器接口与可复现配置文件  
- [ ] 对照：static beam、F9 式自适应、本控制器

### 4.6 模块 D5 — 可观测性（C1）

- 暴露：`Δread_ios`、`Δfaults`、soft-cache 近似命中、promote precision、cache 命中率。  
- 论文图统一用这些计数，避免只报 QPS。

### 4.7 Design 里程碑

| 里程碑 | 内容 | 完成标准 |
|--------|------|----------|
| M1 | Placement + 基础 vmem 搜索可跑 | 与现 motivation 对齐 |
| M2 | Bounded promote + precision 日志 | 优于 demand 或至少不差于 demand，且 ≫ top8 prefetch |
| M3 | Shared cache | 复现/超过 F7 量级收益 |
| M4 | iso-recall 控制器 | 同 recall 下 QPS/IO 优于 static 与 F9 |
| M5 | 对 DiskANN 公平小对照 | 同数据集、同 recall 带 |

---

## 5. Evaluation

### 5.1 环境与数据

| 项 | 计划 |
|----|------|
| 平台 | gpu01：Montage + `/dev/vmem0`（注明 software / 真 CXL 窗口若可用） |
| 数据 | LAION 子集 → 扩到 1M+；标准集 SIFT1M / DEEP1M（若可）作对照 |
| 索引 | HNSW/Vamana 类图，degree 固定报告 |
| 主指标 | **iso-recall@10/100** 下的 QPS、p99 延迟、`read_ios`、promote 字节 |
| 辅指标 | hit 分层（F8）、precision@promote、cache 命中率 |

### 5.2 Baselines

| Baseline | 目的 |
|----------|------|
| Demand fault（无 promote） | 下界 |
| 页/16K admission | 打 F2 |
| Sync 图 2-hop prefetch | 打 F1/F5 |
| Static 大 beam | 打 F3/F7 |
| F9 式 miss 自适应 | 打「朴素控制」 |
| DiskANN 风格（PQ+O_DIRECT/块读，同机同盘） | 外部 SOTA 对照 |
| （可选）全进 Montage 的 oracle 热集 | 上界 |

### 5.3 实验矩阵（论文图规划）

| 实验 | 问题 | 期望 |
|------|------|------|
| E0 Motivation | H0/F2/F5/F7/F8 复现 | 与 FINDINGS 一致 |
| E1 Placement | 图在 DRAM vs 图也在 vmem | 证明图必须快层 |
| E2 Promote | 扫 M 与预算 | 存在最优 precision/预算 |
| E3 Shared cache | cache 比例 × beam | 复现 F7 并画帕累托 |
| E4 Controller | iso-recall 曲线 | 主导 claim |
| E5 vs DiskANN | 同 recall 吞吐/IO | 系统收益成立 |
| E6 Scale | n、dim、并发查询 | 趋势不塌 |
| E7 Ablation | 去掉 cache / promote / 控制器 | 各模块必要 |

### 5.4 威胁与诚实声明

- [ ] soft-vmem 与真 CXL.mem BAR 差异写进 Limitations  
- [ ] 当前 dim=1024 时 1 向量=4KB，布局税实验需含 **更小 dim** 或子向量布局  
- [ ] recall 绝对值受 hops 限制时，强调相对/iso 比较

### 5.5 Evaluation 产出

- [ ] 可一键复现脚本（现有 `run_vmem_f*.sh` 扩展）  
- [ ] 结果表/图草稿目录：`motivation_exps/results/paper_figs/`  
- [ ] Artifact：二进制 + 数据准备说明 + 身份门 `vmem-identity`

---

## 6. 工作计划（按阶段）

### 阶段 A — 叙事冻结（约 1 周）

- [ ] 定稿 Intro「为什么 CXL–SSD」三段式（容量 → DiskANN 错位 → 控制平面）  
- [ ] Related Work 切割表定稿  
- [ ] Problem P1–P4 ↔ Finding 映射表定稿（本文档 §2–§3）

### 阶段 B — 系统 MVP（约 2–3 周）

- [ ] M1 Placement 固化  
- [ ] M2 Bounded promote  
- [ ] M3 Shared cache  
- [ ] 单元级：precision、cache 命中、计数器正确性

### 阶段 C — 控制器与对照（约 2 周）

- [ ] M4 iso-recall 控制器  
- [ ] M5 DiskANN 同机对照（允许简化版）  
- [ ] E0–E4 主图出齐

### 阶段 D — 规模与论文（约 2–3 周）

- [ ] 更大 n / 并发 / 小 dim 布局税  
- [ ] Ablation + Limitations  
- [ ] 初稿：Abstract / Intro / Motivation / Design / Eval / Related / Conclusion

---

## 7. 成功标准（什么叫「这篇能投」）

1. **Why 站得住：** 审稿人无法用「那用 DiskANN / 那用 CXL 内存」一句话打发（切割表 + 容量前提清晰）。  
2. **Problem 站得住：** P1–P4 均有本机 finding，且负结果（F1/F9）变成设计约束而非尴尬。  
3. **Design 可指认：** cache / promote / controller 去掉任一则主指标明显变差（ablation）。  
4. **Eval 主表一张：** iso-recall 下相对最强 baseline（含 DiskANN 路径）有稳定增益，并报告 IO/分层命中。  
5. **不夸大：** 不把 soft-vmem 写成已交付的产品级 CXL.mem SSD；Limitation 诚实。

---

## 8. 风险与备选

| 风险 | 应对 |
|------|------|
| 真 CXL BAR 不可用，只剩 soft-vmem | 论文定位「统一 VA CXL–SSD 软件栈 + 商用 CXL DRAM」；强调控制平面可迁移 |
| vs DiskANN 打不赢绝对吞吐 | 改强调 IO 效率、主机 DRAM 占用、布局灵活性；或限定「无 PQ / 全精度遍历」场景 |
| Promote 始终不如 demand | 收缩 claim：主贡献改为 shared cache + iso-recall 调度；promote 作负结果驱动的「有界」 |
| 布局税在 dim=1024 上不明显 | 换 96/128 维或拆分存储邻接与向量 |

---

## 9. 关键文档与代码入口

| 资源 | 路径 |
|------|------|
| Findings | `motivation_exps/results/MOTIVATION_FINDINGS.md` |
| 文献 | `CXL_ANNS_LITERATURE_SURVEY.md` |
| 硬件笔记 | `CXL_MEMORY_NOTES.md`, `CXL_SSD_STRUCTURE_NOTES.md` |
| 实验框架 | `motivation_exps/`（`vmem-f1`…`vmem-f9`） |
| 复现脚本 | `motivation_exps/scripts/run_vmem_f123.sh`, `run_vmem_f4_f9.sh` |

---

## 10. 下一步（立即执行的 3 件事）

1. **先跑 M-E1 + M-E4**（延迟分解 + iso-recall beam），把 §1.2「重要」补硬。  
2. **Intro 只保留 §1.2 口语三行 + 一张数图表**，删掉抽象「控制平面」开场。  
3. **开 Design M2/M3 接口草图**（`promote` / `cache` / `stats`），接到现有 `vmem_findings.hpp`。

---

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-07-31 | 初版：Why → Problem → Findings/Challenges → Design → Eval 全链路计划 |
| 2026-07-31 | 重写 §0–§1：拆开「为何引入」与「引入后的问题」；Related Work 八维细表 + 审稿人问答 |
| 2026-07-31 | §1.2/1.3 改为「事实墙+人话+重要性」；新增 M-E1…M-E6 Motivation 实验设计 |
| 2026-07-31 | 新增 `vmem-mot12`：O_DIRECT 对照、整次 search 分解、预取原因、beam 全扫、同 beam cache；写入 §1.2.3 实测 |
| 2026-07-31 | **更正：** 不以自制 O_DIRECT 冒充 DiskANN；接入官方 `search_disk_index`（同数据 ~1.4–4.5 ms/q） |
| 2026-07-31 | 补 **无 PQ** DiskANN：`search_memory_index`（同 R=32，L=10…64 → **0.25–0.57 ms/q**）；说明 SSD 路径无法关内存 PQ |
| 2026-07-31 | 实现 SSD `--no_pq_nav`（盘上 FP 邻居排序）：L=10…64 → **4.9–15.7 ms/q，472–1275 IO**；写入 §1.2.3 B3 |
| 2026-07-31 | 补齐 M-E1…M-E5：`vmem-mot-me`；M-E2 小 dim 为限定结论（record≈page） |
| 2026-07-31 | 新增 `vmem-verify`：cold/warm/soft + cold search 对 `Δvmem_read_ios`/`Δnvme_reads` 门禁 **ALL PASS**；复测 H0/F2/F8 走 CXL–SSD |
| 2026-07-31 | 重做搜索宽度实验：改名 **efSearch**，扫 **100/200/400/800/1600**（hops=ef）；召回 0.66→0.93，SSD IO ×6.3 |
| 2026-07-31 | 重做 cache vs no-cache：同 ef∈{200,400,800,1600}；固定 8k cache 仍有收益但随 ef 相对变弱 |
| 2026-07-31 | **重写 §1.1**：从「统一 VA 能力表」改为 DiskANN 复制/分片税 vs CXL-DRAM 冷数据 DRAM 价 vs CXL–SSD one-copy 部署点；补英文定稿与 fabric 前提 |
