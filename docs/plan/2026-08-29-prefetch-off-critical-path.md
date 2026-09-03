# Plan: CXL-SSD ANNS via async prefetch (critical path 无 NAND)

> 对齐用户原设计，**替换** FlashANNS 稿 / `2026-08-28-serving-runtime-completion.md` 的主线
> （驻留平面、共享热集、iso-recall beam 控制器、one-copy 部署税）。
> 那些可以当附录或 ablation，**不是** 论文要卖的系统。

**一句话：** Host / CXL-DRAM 与 CXL-SSD 差两个数量级（~0.6 µs vs ~70 µs）→ 阻塞 `load` 不能出现在 expand/score 上 → 系统是一个 **ANNS-aware 异步 prefetcher**，把 NAND 读提前打进快层，critical path 只 hit。

**成功标准（唯一）：** iso-recall 下，walk 真正碰页时 CXL-SSD 读 ≈ 0（critical-path hit → ~100%）。QPS / 带宽是结果，不是目标本身。

---

## 0. 和现稿差在哪

| | 现稿 FlashANNS | 本 plan（你的设计） |
|--|----------------|---------------------|
| 核心问题 | 第三条部署点 + residency 失控 | **速度差 → 必须 async prefetch 才可用** |
| Challenge | 页 admission / 盲升迁 / beam 是 stall 旋钮 | **怎么 prefetch、prefetch 谁、如何打满带宽** |
| 系统 | 五模块 runtime（放置/promote/热集/控 beam/观测） | **一条 prefetch pipeline**（PipeANN 式 + 更准 + 更满） |
| 一等公民 | 窗里住谁、promote precision、miss depth | **critical path 上还有没有 NAND** |
| 布局 | placement 选项 | **为 prefetch 精度服务的图重建（邻居同页）** |
| 带宽 | 支撑模块 | **continuous batching，为了藏 miss，不是为了打满而打满** |

Opportunity（闪存价冷库 / 将来 one-copy）可以留在 Introduction 一段，**不要再当贡献句**。

---

## 1. 问题（Problem）

CXL-SSD 给出 memory-semantic 冷库，但 **一次 cold 向量/图页 load ≈ 70 µs**。Best-first 每 hop：读 `nbrs(cur)` → 打分邻居向量。这两步若走阻塞 CXL-SSD，整个 ANNS 不可用（T2I-10M 无 prefetch ≈ 1–2 QPS vs pipeline 后十几 QPS）。

对照：

- DiskANN/PipeANN：block `aio` 把 NAND 放在 I/O 队列，不在 CPU load 上。
- CXL-ANNS：全库已在 CXL-DRAM，prefetch 藏的是 ~100 ns。
- **Naive mmap CXL-SSD：** 编程模型像内存，miss 税像闪存，且 QD≈1。

本文解的是第三条上的 **prefetch 问题**，不是再发明一套 OS 页缓存。

---

## 2. Challenges（只保留三问）

### C1 · 如何 prefetch？（机制）

阻塞 `load/store` 无法维持设备队列。必须：

- **打破 best-first 的 compute–I/O 全序**（PipeANN）：I/O 由 candidate 池决定，不必等当前 hop 全部打完。
- **Pipeline：** SSD→快层 与 host 打分重叠。
- **异步 batch：** `PREFETCH_BATCH` / page copy pool，一次发一 hop 的 miss 页，而不是一向量一 fault。

已有：`serving` P3（host score + pipe-w）、PipeANN `pipe_search`。

缺口：expand 时 `nbrs(cur)` 仍常走 demand（图区未进 prefetch 集）。Pipeline 不完整。

### C2 · Prefetch 什么？如何更准？（精度）

带宽用在用不到的页上 = 污染快层 + 挤掉真要用的页（2-hop / install_all 已证：QPS 崩）。

要准，靠两件事，不是更大的 L：

1. **算法准入：** 只发 *walk 下一步会碰的*——未 expand 的 top 候选的 **图页**，落地后再发其未见邻居的 **向量页**。禁止高覆盖 2-hop、禁止「多 fault 抬 QD」。
2. **图重建 / 布局：** 把同一节点的邻居向量打进 **同一 4 KB（或 2-page group）**（pagebin / Starling 同类）。一次准 prefetch 覆盖多个下一跳，减少「读了半页垃圾」。

已有：pagebin（T2I hit 10%→42%）、P3 `install_top=4`（偏保守，准但覆盖不够）。

缺口：

- 指标是 mixed hit%（第一次 install 算 miss），**不能**证明「prefetch 之后 critical path 不再读盘」。
- 图页与向量页分裂，lookahead 没绑图页。
- 没有 reserved-until-consume：prefetch 进窗后可被 clock 踢掉，等于白读。

### C3 · 如何最大程度利用带宽？（深度，服务 C1）

单 query 看不远 → pipeline bubble → critical path 再次暴露 NAND。

- **Continuous batching / fetch-level：** 跨 query、按「一次 fill 退休再 admit」，维持目标 inflight \(D\)（PipeANN `cont` 已在 **block** 路径上把 4 KB 推到 ~5.4 GB/s）。
- 目的是 **让预取永远赶在 walk 前面**，不是刷满 7 GB/s / 160 GB/s。
- 不准的合并读（8–16 KB）会放大 BW，但违反 C2 → 只在 pagebin 已保证页内全是邻居时才合并。

已有：PipeANN `cont`（block）；serving 多线程 per-window（有用 promote ~4.7 GB/s 后撞墙）。

缺口：serving 路径上没有与 `cont` 同构的 **全局 fetch 调度**；多线程共享 vmem 仍可能互相污染。

---

## 3. 三个方法（机制 + 必须能答辩的 novelty）

原则：PipeANN / Starling / `cont` / 现有 P3+pagebin **可以当对照或底座**，不能当贡献句。
每个方法必须多出一层 **只有 CXL-SSD（load 语义 + 设备软缓存 + 小 CXL-DRAM 窗）才成立** 的机制。
若实现时只做了底座，这篇就没有创新，不要投稿。

---

### 方法 1 — Prefetch：Two-Sink + 禁 demand walk

#### 复用层（不能写进贡献）

- 用 candidate 池发下一跳 I/O、打破 best-first 全序 → PipeANN。
- 从 host staging 打分、`pipe-w` worker → 现有 P3。
- 按距离 prefetch candidate → CXL-ANNS（而且他们藏的是 ~100 ns）。

#### 独特层（必须做满）

CXL-SSD 有 **两级 miss**（Motivation F8）：NAND ~70 µs → 设备 soft-cache ~3.8 µs → 窗/PTE ~0.6 µs。快层窗口只有 1 GiB 量级，设备缓存却有数 GiB。PipeANN 只有 **一个** host 缓冲；没有「先灌 NAND 前缓存、再精选进窗」。

**M1a · Two-Sink issue（双落点）**

| Sink | 容量 | 延迟若命中 | 谁可以进 | 作用 |
|------|------|------------|----------|------|
| L1 设备 soft-cache | ~4 GiB（vmem cache） | ~4 µs（仍可能 fault） | lookahead 里所有 **SAFE 页**（见方法 2） | 便宜地藏 NAND，允许稍宽的投机 |
| L2 CXL-DRAM 窗 | 256 MiB–1 GiB | ~0.6 µs | 通过 will-use 的页，**reserved** | walk/score **唯一合法读源** |

Issue 顺序：`PREFETCH_BATCH` → L1；仅当页的 `P(use)` 过线或 deadline 临近 → `install` 进 L2 并 reserved。  
L1 投机、L2 精选：**用设备缓存的大容量换精度，用窗口的小容量换 critical-path hit**。这是 CXL-SSD 硬件结构逼出来的，不是把 PipeANN 的 Q 换个名字。

**M1b · Typed EDF（带类型的截止期）**

页不是一种 I/O：

- **G 页**（`off_graph`）：未 expand 的 top-\(M\) 的邻接页，deadline = 「下一次 expand」。
- **V 页**（`off_vectors`）：这些点的 1-hop 向量页，deadline = 「对应 G 落地之后的第一次 score」。

调度键：`(deadline, type)`，G 优先于同 deadline 的 V。PipeANN 的「未读邻居」是单一队列；这里 **图页和向量页是两类对象**（你们布局本身就是分裂的）。DiskANN 一个 sector 里两者在一起，这个问题不存在。

**M1c · 搜索线程禁止 CXL-SSD demand load**

Walk 的 `copy_through` **不得** 在 miss 时 `promote`/`memcpy(ssd)`。三种合法动作：

1. L2 reserved 已 ready → 读窗，记 `crit_hit`。
2. 未 ready → **改 expand 已 ready 的次优候选**（顺序松弛；召回由「仍只访问 beam 内节点」约束，或记录 order-relax 次数）。
3. 没有任何 ready 候选 → **yield 给 prefetcher**，把该页 deadline 打成 NOW，**仍不许** walk 线程去碰 `ssd_base`。

这和 PipeANN「等到 aio 完成再 explore」不同：他们的等待发生在 I/O 线程模型里，CPU 不发阻塞 load。我们的失败模式是 **一次 walk 侧 `load` 把设备 QD 打成 1**。禁止 demand 是为了保 pipeline，不是风格选择。

**Walk 伪代码**

```
while beam not done:
  cur = best unexpanded among {id | G(id) and V(needed) reserved-ready in L2}
  if cur is empty:
        bump deadline of best unexpanded; yield; continue
  expand cur from L2          # never ssd_base
  issue G of new top-M into L1
  when G completes: issue V of unseen nbrs into L1
  promote L1→L2 only if will-use or deadline < τ
```

**Novelty 一句话：** 双落点（L1 投机 / L2 契约）+ 类型化截止期 + walk 禁 NAND。少做任何一条，就退回 PipeANN-on-mmap。

**Ablation：** 单 sink（只 L2 / 只 L1）；允许 demand promote；无类型（G/V 混队列）。缺一条 crit-ssd 应明显回升。

---

### 方法 2 — 图重建：Expand-Bundle + Prefetch-Closed Page

#### 复用层（不能写进贡献）

- 点与邻居放近、一块读多个点 → Starling block shuffle、DiskANN sector、**现有 pagebin**（star-BFS 邻居块 + 密排）。
- PageANN 的「图节点 = 一页」是另一种索引，不是我们的 Vamana 重建。

现有 `pack_layout_pagebin.cpp` 只做 **ID 重排 + 密排**，图和向量仍分 `off_graph` / `off_vectors`，**不保证**「prefetch 这一页里没有未来用不到的向量」，也不形成「一次 expand 的工作集」。

#### 独特层（必须做满）

Prefetch 的 issue 粒度是 **「expand v 接下来要碰的字节」**，不是「读 v 自己的 sector」。DiskANN sector = `{vec(v), nbrs(v)}`（读当前点）。我们要的 bundle = **`{nbrs(v), vec(nbrs(v))}`**（读下一跳）。布局必须按 **prefetcher 的 issue** 重建，不是按「减少 I/O 次数」泛泛做 locality。

**M2a · Expand-Bundle（按下一跳打包）**

对每个 \(v\)，定义

\[
\mathrm{Bundle}(v) = \mathrm{page}(\mathrm{nbrs}(v)) \;\cup\; \mathrm{pages}(\{\mathrm{vec}(u): u \in N(v)\})
\]

重建目标：\(\lvert \mathrm{Bundle}(v) \rvert\) 尽量小，且页在地址上 **可一批发**（连续或固定 stride，便于 `PREFETCH_BATCH`）。

做法（build-time，不改搜索语义，只改 ID 与物理布局）：

1. 以 \(v\) 为种子，把 \(N(v)\) 能装进同一 4 KB / 2-page group 的先装满（1-hop closure pack）。
2. 装不下的邻居进 **overflow 页**（见 M2b）。
3. 把 \(\mathrm{nbrs}(v)\) 所在图页与该 group **对齐或相邻**，使 `prefetch(expand v)` = 一小段连续 offset 列表，而不是 32 个随机 4 KB。

和 Starling 的差：他们优化「读到的块里有用点比例」（vertex utilization）。我们优化 **一次 lookahead issue 覆盖整个下一跳**。和 DiskANN 的差：他们打包的是 **当前点**，我们打包的是 **当前点的出边终点**。

**M2b · Prefetch-Closed Page（可投机页 vs 仅按需页）**

每个向量页打标签：

- **SAFE：** 页上每个「非种子」向量都属于页内某一种子的 1-hop。投机 prefetch 该页 **不会** 引入与本次 expand 无关的点。
- **OVFL：** 装不下的边；**禁止** 因「顺便」被 L1 投机；只有当 lookahead 集合 **显式包含** 其上的 id 才发。

构造：贪心 1-hop 闭包包页；剩余边进 OVFL。  
Prefetcher 规则：L1 投机 **只许 SAFE**；OVFL 进 L1 必须带显式 id（仍走异步，不走 walk demand）。

这是 pagebin 没有的：**页分成两类，和 Two-Sink 的「谁可以进 L1」对齐**。Starling 所有块一视同仁。

**M2c · Frontier-aligned 图页（可选，第二刀）**

\(R \times 4 = 128\) B，一页图可装 32 个邻接表。按「常一起出现在 top-\(M\)」或「共享邻居」聚类，使 **一次 G prefetch 覆盖多个未 expand 点的 adj**。服务 M1 的 G 类型，不是再做一个向量 shuffle。

**Novelty 一句话：** 布局单元 = prefetch 的 expand-bundle；页带 SAFE/OVFL 契约。只做 star-BFS 密排 = 复用 Starling/pagebin，没有创新。

**Ablation：** packed 乱序；仅 pagebin A；Bundle+SAFE。主指标：`|Bundle(v)|`、SAFE 页上的 used/fetched、crit-hit。召回必须不变（只 remap id）。

---

### 方法 3 — 带宽：有用填充 + 双队列 + 跨 query 合并

#### 复用层（不能写进贡献）

- 目标 inflight \(D\)、retire 再 admit → PipeANN `cont` / 通用 QD 控制。
- 多 query 交错 I/O → CoroSearch。
- 为刷满而合并 8–16 KB → 违反精度，已证有害。

#### 独特层（必须做满）

最大化的不是 NAND GB/s，而是 **能在 L2 reserved 上变成 crit-hit 的字节/秒**（useful fill）。设备侧有 **两级队列**（L1 ioctl vs L2 install），且多 query 共享同一 CXL-SSD 地址空间（同一 `page_off` 不该读两遍）。

**M3a · Useful-fill 准入（不是满 QD 就发）**

每个候选 fill 的分：

\[
s(p) = \hat{P}(\mathrm{use}\mid p) \cdot \frac{\mathrm{bytes}_{\mathrm{SAFE}}(p)}{\max(\varepsilon,\, t_{\mathrm{deadline}}-t)}
\]

- \(\hat{P}(\mathrm{use})\)：在 lookahead-2 里则高；仅因同页顺便则 0（OVFL 或非闭包）。
- 若该 fill 会 **evict 一条仍 reserved 的 L2 页** → **拒发**（契约优先于带宽）。
- 目标：在 crit-ssd≈0 的约束下拉高 useful GB/s。满队列但 precision 低 = 失败。

**M3b · 双深度 \(D_{L1}, D_{L2}\)**

- \(D_{L1}\)：设备 cache 上的 in-flight ioctl，可以大（藏 70 µs，灌 SAFE）。
- \(D_{L2}\)：窗上 reserved 槽位数，**≤ 窗容量 − pin**，只收 will-use。

PipeANN 一个 \(W\)。我们把「管道有多深」和「快层能留多少」拆开，否则 `install_all` 会再次污染 L2。这是 Two-Sink 在带宽上的对应物。

**M3c · 跨 query 页合并（page multicast）**

全局 `in_flight[page_off] → waiters[]`。Query B 要的页已在飞或已在别人的 L1 → **挂 waiter，禁止第二下 NAND**。  
`cont` 只是交错不同读；这里是 **同一 CXL-SSD 页的多 query 共享一次 fill**。one-copy / 多线程 serving 下这是结构问题，不是调度微调。

**M3d · 紧急时仍不 demand**

crit 路径要饿死时：只把该页 \(s(p)\) 提到最高、deadline=NOW，必要时从 L1 提前 promote 到 L2。**禁止** walk 侧 `load`（与 M1c 同一条不变量）。

**Novelty 一句话：** 优化 useful fill；L1/L2 两个深度；跨 query 合并同一 `page_off`。只搬 `cont` 的目标 \(D\) = 复用。

**Ablation：** 固定单 \(D\)；无 multicast；无 useful 分（见页就发）。看 crit-ssd、precision、NAND 次数/query，不只看 GB/s。

---

### 三者怎么咬合（系统，不是三篇短文）

```
图重建 (M2)     →  每个 expand 的 issue 列表短且全是 SAFE
     ↓
Prefetch (M1)   →  列表进 L1，精选进 L2，walk 只读 L2
     ↓
带宽 (M3)       →  在「不破坏 reserved、不读 OVFL」下把 L1 灌满、合并重复页
```

少 M2：M1 只能散读，L1 灌的是垃圾页。  
少 M1：M3 的 QD 会变成 walk 侧阻塞 load。  
少 M3：单 query 看不远，L2 赶不上，crit-ssd 回升。

---

**从现稿降级、不要写进贡献句：** 共享 LFU 热集、miss 反馈砍 beam、图永久放 host 当「第一个模块」、Shared FAM one-copy、160 GB/s 设备内部总线。热集最多当 L2 的一点 warmup。P3 / pagebin / PipeANN `cont` 只作底座或对照。

---

## 4. 指标（先改指标，再改故事）

| 指标 | 含义 | 目标 |
|------|------|------|
| **crit_hit%** | walk 的 `copy_through`/`score` 时页已在快层 | → ~100% |
| **crit_ssd_frac** | critical path 上发生的 NAND fill / 全部 touch | → ~0 |
| promote_bytes / GB/s | prefetch 流量（允许在非 crit 路径） | 够用即可 |
| iso-recall QPS / p99 | 系统结果 | 相对 Demand / 无 prefetch 数量级提升 |
| inflight \(D_{L1}/D_{L2}\) / bubble | M3 | L1 深、L2 不溢、bubble→0 |
| precision / useful GB/s | M2+M3 | SAFE 高、OVFL 无顺便读 |
| \(\lvert\mathrm{Bundle}(v)\rvert\) | M2 | 相对 packed / pagebin 下降 |

废弃作为主表：现在的 `cxl_dram_hit_pct`（第一次 install 算 miss）。可留作附录。

---

## 5. 评价矩阵（只服务三问）

| 实验 | 证明 |
|------|------|
| Demand / 阻塞 mmap | 无 prefetch 不可用（C1 动机） |
| P3 only（单 sink + 允许 demand） | 底座；crit-ssd 仍高 |
| + M1（Two-Sink + 禁 demand + G/V EDF） | crit-ssd 大降；缺 M2 则 L1 浪费 |
| + M2 Bundle+SAFE（相对 pagebin A） | \(\lvert Bundle\rvert\)↓、precision↑ |
| + M3 useful-fill + 双 D + multicast | 高并发下 crit-ssd 仍≈0，NAND/q 不翻倍 |
| 负例：2-hop / install_all / 盲目加 W | 不准的 prefetch 有害 |
| 同机 DiskANN/PipeANN block | 对照「block 路径已经会藏 I/O」；我们藏的是 **load 语义** |
| Oracle 全在 DRAM | 上界：crit-ssd=0 时的 QPS |

不把「打满 12/160 GB/s」当 pass/fail。

---

## 6. 已有 vs 还要做（执行顺序）

**已有、应对进主线：** P3 pipeline；pagebin；per-thread window；PipeANN `cont`（block 对照）；2-hop/install_all 负例。

**按顺序做（论文能闭环的最小集）：**

1. **指标：** `crit_hit` / `crit_ssd_frac` / `useful_GBps`；prefetch 不计 miss。
2. **M1：** walk 禁 `ssd_base`；L1 ioctl vs L2 reserved；G 页进 issue；未 ready 则换候选或 yield。
3. **M2：** 新 packer：1-hop Bundle + SAFE/OVFL 标签（不要只重跑 star-BFS pagebin）。
4. **M3：** \(D_{L1}/D_{L2}\) + `in_flight[page]` waiter；拒发会踢 reserved 的 fill。
5. **重跑：** Demand / P3 / +M1 / +M1+M2 / 全开；负例 2-hop、单 D、关 multicast。
6. **改 intro/design：** 贡献句 = 上述三条独特层，不是「参考 PipeANN 做了 pipeline」。

**不要做（会把主线再漂走）：** 实现 `hot_set.hpp`、iso-recall 在线砍 beam、把「图在 DRAM」写成第一贡献。

---

## 7. 审稿人会说「复用」时的标准答

| 攻击 | 答 |
|------|----|
| 就是 PipeANN | 他们一个 host 缓冲、一种 I/O、walk 等 aio。我们 **L1/L2 两落点**、**G/V 两类截止期**、**walk 禁止 CXL-SSD load**（否则 QD→1）。 |
| 就是 Starling/pagebin | 他们打包「读当前点更赚」。我们打包 **expand 的下一跳 Bundle**，且页有 **SAFE/OVFL**，和 L1 投机权绑定。 |
| 就是 cont | 他们最大化设备 QD。我们最大化 **useful fill**，**双深度**，**跨 query 同一 page_off 只读一次**，且永不 evict reserved。 |

---

## 8. 对旧文档的处置

- 本文件 = **现行研究/实现主 plan**。
- `2026-07-31-cxl-ssd-anns-asplos-plan.md`：保留实测表；§0「新-降级」和后文 residency 叙事以本文为准。
- `paper/sections/{intro,design}.tex`：按 §1–3 改 Challenge/Design（另开写作任务）。
- `2026-08-28-serving-runtime-completion.md`：**暂停**，与本主线冲突。
