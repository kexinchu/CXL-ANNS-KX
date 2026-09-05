# FlashANNS Paper Plan (storyline lock)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按「flash-backed CXL memory 上 graph ANNS 如何可用」冻结叙事、章节和实验，使论文 claim 与已锁定 hide 数字一致，并在 2026-09-09 截止前交出可审的 ASPLOS Rapid Review 前两页 + 主结果表。

**Architecture:** 机会与系统拆开。机会是第三条部署点（flash 价容量 + 小 memory-semantic 窗口）。系统是 hide runtime：打分只从窗口取数；prefetch / page admission / continuous issue 只为在 hop 打分之前把页装满。Oracle 是窗口已 pin 的搜索，不是「DiskANN 更快」也不是「产品级 CXL.mem」。

**Tech Stack:** `paper/sections/*.tex`，锁定 hide 行见 `docs/notes/2026-08-31-组会汇报.md`，动机 findings 见 `motivation_exps/results/MOTIVATION_FINDINGS.md`，运行时 `serving/search_beam`。

---

## 0. 冻结故事线（不要再和旧 §0 混写）

旧 plan（`docs/plan/2026-07-31-cxl-ssd-anns-asplos-plan.md` §0 旧稿）的主线是 **one-copy 解耦容量与吞吐**。那是部署机会，**不是**本轮系统贡献，也没有 Shared FAM 实验。

新主线（你标出的 58–62 行，这里写死）：

> **论文回答：** 在 flash-backed CXL memory tier 上，graph ANNS 怎样才算可用。
>
> **Oracle（服务合同，不是已达到的结果）：** 对 host 而言，打分像数据已经在一小块 CXL-DRAM 窗口里；容量仍在 CXL-SSD。query 临界路径上不应因为 miss 去读 NAND，也不应阻塞等待 prefetch。
>
> **系统：** 第一个在 SSD-backed CXL 地址空间上直接跑 graph ANNS、并联合管理 residency / promote / search stall 的 serving runtime。不替代 DiskANN 的 block-SSD 路径；不假设全库进入 CXL-DRAM。

### 0.1 Oracle 必须拆成两层

| 层 | 合同 | 锁定证据 | 论文里怎么写 |
|---|---|---|---|
| **Score hide** | 全精度分数 100% 来自窗口，`bounce=0` | `from_win=100` | 已达到。Intro 可以写「打分看不见 flash」。 |
| **Query hide** | 临界路径等待是 DRAM 级；单 query 逼近窗口 oracle | T=1：50.25 QPS / 19.8 ms vs oracle 85.78 / 11.4 ms = **0.59×** | 未达到。写成 pointer-chasing 残差：hop *i* 打完分才知道 hop *i+1* 的向量页。 |

禁止把两层写成已经一起成立。Discussion / Intro 里「host 感知不到 CXL-SSD」只覆盖 **score hide**。

### 0.2 三条部署点（只当 Why，不当 Eval）

| 点 | 代表 | 税 | 本文角色 |
|---|---|---|---|
| Block-SSD ANNS | DiskANN / PipeANN | 扩吞吐时常复制或分片整库 | 对照，不是要打败的 SOTA 头条 |
| Full CXL-DRAM ANNS | CXL-ANNS / Cosmos | 冷尾付 DRAM 价 | 对照：我们不把 *D* 抬进 DRAM |
| Flash-backed CXL memory | 本文 | 闪存价 *D* + 有界窗口 | **机会**。系统贡献是 hide，不是 one-copy 测过 |

One-copy / Shared FAM：只在 Background 写 CXL 3.x 条件。禁止进贡献列表，禁止当 Q4 主图。

### 0.3 一句话 first（不许加宽）

> To our knowledge, this is the first **graph-ANNS serving system** that **directly operates over an SSD-backed CXL address space** and **jointly manages ANNS-specific residency, promotion, and search stalls**.

不要写：第一个把 CXL-SSD 接到向量库；第一个 one-copy disaggregated ANNS；测到了产品 CXL.mem / 160 GB/s。

### 0.4 相对两条已有路线怎么收

- **相对 DiskANN/PipeANN：** 不是更好的 `io_uring` 调度。他们藏的是 **block I/O**；我们藏的是 **memory-semantic flash miss**。同机 PipeANN 是 named residual（oneshot FP vs PQ navigation），不是 hidden bar。
- **相对 CXL-ANNS/Cosmos：** 他们假设图+向量在 DRAM 类 CXL 里，没有 NAND miss cliff。我们承认 flash 仍是冷语料介质。
- **系统贡献收口：** 语义放置、有界升迁、page-bin/expand-bundle、异步 prefetch + 跨 query issue。目标是缩小 **naive CXL-SSD ↔ 窗口 oracle** 的差距，不是「逼近 DiskANN」。

---

## 0.5 ASPLOS 一页纸（核心 / 挑战 / 设计 / 实验）

审稿人听完 90 秒必须能复述这一页。后面每一章只服务这一页，不再加第三条主线。

### 核心（一句话）

> **FlashANNS 让 graph ANNS 在 flash-backed CXL memory 上可用：打分只从一小块窗口取数；wise prefetcher 决定预取谁，continuous batching 在指针追逐留下的空洞里把 NAND fill 发走。**

展开半句（不要写进 title）：score hide 已到（`from_win=100`）；query hide 是 oracle，T=1 仍是 0.59×，因为 hop *i* 打完分才知道 hop *i+1*。

ASPLOS 归属：**OS residency + memory/storage hierarchy**，不是新 ANNS 图算法，也不是「比 DiskANN 更快的 SSD 调度」。

### 挑战（三个，对上两个机制）

| ID | 挑战 | 为什么存在 | 不管会怎样 | 谁来答 |
|---|---|---|---|---|
| **C1 悬崖** | 窗口 hit 与 NAND miss 差 ~140× | memory-semantic 接口改的是 *怎么发 miss*，不是 *miss 有多贵* | 当略慢 DRAM 用，search 被 miss 主导 | 合同：打分不得碰 NAND |
| **C2 预取必须 wise** | 图是 pointer-chasing；盲 2-hop / 按页 admission 污染小窗口 | 下一跳要打完分才知道；错页比漏页更贵 | QPS 掉 3–5×，hit% 不涨 | **Wise prefetcher** |
| **C3 单 query 填不满 QD** | 一条 walk 在 hop *i* 分数出来前发不出 hop *i+1* | best-first + 40µs 级 miss → MLP/QD≈1 | 占用 38% 不是带宽墙，是管线空 | **Continuous batching** |

C2 不是「再预取狠一点」。Motivation 已证 sync 高覆盖预取有害。Wise = 只预取 *即将 expand 的 frontier*，并且用 page-bin/expand-bundle 让一次 fill 带上同跳邻居。

C3 不是「把盘打满当赢」。T=1 有用流量到 oracle 也只要 ~1.02 GB/s < 1.56。Continuous batching 的目的是 **藏延迟 / 保 QD**，跨 query 才拉得满。

### 设计（两个一等公民 + 一个前提）

不要再卖五模块。对外只讲：

```
前提 D0  放置：图/邻居 ID 在 host DRAM，全精度向量在 CXL-SSD。
         否则 prefetcher 连「下一跳是谁」都要先 miss 一次。

D1  Wise prefetcher
    预取谁：frontier top-M / beam 成员的 N(u)，不是 2-hop 全邻域
    预取成页：expand-bundle / page-bin，同跳邻居共页
    预取多少：字节预算 + install_top；禁止 --score-page
    何时：异步 PREFETCH_BATCH，打分线程不得等这次 fill 才能算
         （score hide）；单 walk 仍无法在 hop i 分数前完全发出 i+1
         （query-hide 残差，可选 spec-beam 吃一部分）

D2  Continuous batching
    跨 query、按 fill 粒度维持目标 QD
    某 query 卡在 pointer-chasing 时，把设备时间让给别的 ready query
    不改变 walk 访问哪些点，只改变页何时进窗口
```

Placement / page-bin 写进 D1 的「怎样预取才准」，不要单独当第三贡献。LFU 热集、iso-recall 控制器、observability 都不是对外设计主轴。

### 实验必须回答的问题（对应 C/D）

| 问题 | 测什么 | 锁定/已有 | 论文角色 |
|---|---|---|---|
| naive 为何不可用 | cliff；page vs record；盲预取掉 QPS；单 query QD≈1 | Motivation findings | Motivation 图 |
| wise 是否必要 | −prefetch / −pagebin / −bundle；`issue_use`、`from_win`、QPS | hide 50.25 有；ablation 表过期 | Design 可证伪 |
| score hide 到了没有 | `from_win`、bounce | **100% / 0** | 合同，不是 QPS |
| query hide 到了没有 | vs 窗口 oracle | **0.59×**（50.25 / 85.8） | 残差，指针追逐 |
| cont 是否必要 | T=1 vs T>1 或 −cont；占用、crit_wait | T=4 服务行有；勿和 85.8 比 | 证 C3 |
| 公平对照 | Demand；PipeANN T=1 PQ-on | 1.64；57.6 | 下界 + named residual |
| 本轮不测 | Shared FAM、CXL.mem 产品、160 GB/s、占用刷 100% | — | Discussion 一句 |

---

## 0.6 分章写作（ASPLOS 11 页，Rapid Review = 前两页）

原则：每一章开头用一句话接上 §0.5；读者随时能回答「这节在证 C1/C2/C3 的哪一条」。

| 章 | 页 | 读者带走 | 写法（按新 plan） | 图/表合同 |
|---|---|---|---|---|
| **§1 Intro** | 1.8–2.0 | 第三条点 + score/query hide + wise+cont | **已改。** 不要再加 one-copy first。贡献 3 对准 D1+D2 | Fig.1 三点；数字 1.64 / 50.25 / 85.8 |
| **§2 Background** | ~1.0 | 图 walk；窗口；DiskANN 是块路径 | 只够 Design 能读。**不写 §2.5 Sharing**；FAM 两句收在 §2.4 末 | 无 Fig.2 |
| **§3 Motivation** | 1.4–1.6 | naive 放大 C1–C3 | **标题必须是 Motivation。** 按 C 重组，见 §1.7。F1=C1，F2+F3=C2，F5=C3。F4 作 C2 的副作用。F7/LFU 脚注 | Fig.3 cliff；Fig.4 页/盲预取；Fig.5 bubbles |
| **§4 Design** | 2.4–2.6 | D0 前提 + D1 wise + D2 cont | **重排现稿 D1–D5：** D1 placement 并入 D0/D1；D2+D4 前半 = wise prefetcher；D4 后半 = cont；D3 LFU 一段「我们不做」；D5 缩成一段计数器 | Fig.6 架构（fill/score 拆开）；Fig.7 wise 路径；Fig.8 cont 时间线 |
| **§5 Impl** | 0.5–0.7 | 测的是 vmem_sw + host 窗口 | 现稿身份段保留；删「P3 还在 bounce 上等」若与 from_win=100 矛盾，改成：打分从窗口，install 仍可能在临界路径上（0.59×） | 无新图 |
| **§6 Eval** | 2.6–3.0 | money plot 先 hide 再 QPS | Q1 naive；Q2 score hide + vs oracle；Q3 −wise / −cont ablation；Q4 T=1 vs 跨 query；Q5 PipeANN residual。禁止 T=4 证 T=1 hide | Fig.9 money；Table 2 锁定行；Fig.10 ablation |
| **§7 Related** | ~0.8 | 四句切割：数据在哪 / 慢路径 / 延迟量级 / 差在哪 | DiskANN≠更好 io_uring；CXL-ANNS≠可搬预取；SkyByte≠ANNS residency | Table 5 八维 |
| **§8 Discussion** | ~0.5 | proxy；0.59×；PipeANN 不同合同 | 降调「host 感觉不到 SSD」= 仅 score hide | 无图 |
| **§9 Conclusion** | ~0.3 | 复述一句话核心 + 0.59× 未闭 | 不承诺下一篇做 Shared FAM 当贡献 | 无图 |

页合计压在 11 页 body。Fig 不够就砍 Fig.2 细节或把 Fig.4 三面板收成两栏。

### 写作顺序（不要按章节号从 2 写到 9）

1. Intro+Abstract — **已完成**
2. Design 改成 D0/D1/D2 口径（否则后面 Motivation 映射表和 Eval Q 对不上）
3. Discussion 降调（和 Intro 同一声音）
4. Motivation 按 §1.7 重排（标题已是 Motivation；按 C 做小节）
5. Eval 问句改成上表 Q1–Q5；重跑 ablation
6. Background / Related / Impl / Conclusion 收口

---

## 1. 章节合同（每节只做一件事）

| 节 | 文件 | 页预算 | 读者读完必须带走的一句 | 禁止写 |
|---|---|---|---|---|
| Intro | `paper/sections/intro.tex` | 1.8–2.0（含 Fig.1） | 第三条点 + hide 合同 + 有范围的 first | one-copy 已评估；hide = oracle |
| Background | `paper/sections/background.tex` | ~1.2 | 图 walk、窗口、fill 语义 | 独立 Sharing 小节；把 pooling 写成 concurrent share |
| Motivation | `paper/sections/motivation.tex` | 1.4–1.6 | naive 用法会把 140× cliff 放大 | 把负结果当尴尬而不是约束 |
| Design | `paper/sections/design.tex` | 2.5–2.8 | D1/D2/D4 对应 F；D3 只剩 entry pin | LFU 热集当正贡献 |
| Impl | `paper/sections/impl.tex` | ~0.8 | `vmem_sw` + host 窗口替身 | BAR / 24 GB/s / 产品 CXL.mem |
| Eval | `paper/sections/eval.tex` | 2.6–3.0 | money plot = crit_wait + iso-recall QPS | 用 T=4 证明 T=1 hide |
| Related | `paper/sections/related.tex` | ~1.0 | 八维切割，不空比「也做 ANNS」 | first CXL-SSD for vector DB |
| Discussion | `paper/sections/discussion.tex` | ~0.6 | proxy、0.59× 残差、PipeANN 不同合同 | 「host 完全感觉不到 SSD」 |

---

## 1.6 Background 修改 plan（ASPLOS 审稿人口径）

**读者：** 非 ANNS 的 OS / 存储审稿人。读完必须能读懂 P3 的 C1–C3 和 P4 的 D0/wise/cont，而不需要先读 DiskANN 论文。

**这一节的唯一工作：** 定义词汇和两套已有栈。不宣布结果，不卖 first，不重画 Fig.1。

**页预算：** 0.8–1.0 页（现稿偏长且跑题）。Fig.2 只在它比 Fig.1(c) 多出「实测栈」时才留。

### 审稿人现在会打回的点

| 现稿 | 审稿人反应 | 改法 |
|---|---|---|
| 标题 *Background and Deployment Points* | Fig.1 已经是部署点。再写一遍像灌水 | 改成 `Background` |
| §2.1 把 Vamana 和 DiskANN 捆在一起 | 还没讲清 hop，就在骂 sector | 先讲 walk，再讲块路径 |
| 「I/O queue 是错误一等公民」 | 这是 Motivation/Design 的 claim | 改成「对象不同」，对错留给 §3 |
| CXL-AnySSD / P-HNSW 各一段 | Related 已有；Background 变文献综述 | **整段搬 Related** |
| 介质成本公式 + 引用 Fig.eval-scale | 没有价表却写方程；审稿人会要 TCO | **删除**。一句定性即可 |
| Fig.2 caption 谈 one-copy 不复制 IOPS | 把故事拽回未评估的多机 | caption 只画 **单 host 实测栈** |
| FAM 三条件 + 图 | Intro 已降调；这里仍像第三贡献 | 保留 **5 句**，不要为它单独占图 |

### 审稿人希望按这个顺序读到

**§2.1 Graph walk（约 12 行）**  
必须出现的词，后面 C2/C3/D0 都要用：vector、neighbor list $R$、beam $L$、expand、hop、candidate、pointer-chasing。写清：下一跳的向量页要等本 hop 打完分才知道；图的邻接 ID 可以和向量分开放。不要讲 RAG。不要报 QPS。

**§2.2 块路径（DiskANN / PipeANN，约 10 行）**  
一等公民：sector、PQ 导航、`aio` 队列、per-host node cache。一句：图和向量常在同一 4KB sector。PipeANN：单 query overlap + 可跨 query 抬 QD——这是 **block** 上的 continuous batching，给 §4 D2 当对照，不是竞争对手点评。禁止「所以他们错了」。

**§2.3 CXL-DRAM ANNS（约 8 行）**  
CXL.mem = HDM 映进 PA。CXL-ANNS/Cosmos：图+向量在 DRAM 池，优化的是 ~100 ns，不是 ~40 µs NAND。一句：接口是 load/store，容量假设是全库进 DRAM。他们的预取/缓存 **不能直接搬**——延迟差两个数量级。细节留给 Related。

**§2.4 本文的 CXL-SSD 设备模型（约 15 行，本章核心）**  
审稿人要一张「我们假设的硬件」而不是产品手册。必须定义：

- 容量在 NAND；只有一小块 **score window** 是 DRAM 级
- **hit** = 窗口内拷贝；**miss** = NAND fill 再重试；编程模型像内存，税像闪存
- 窗口有界，admission 单位是页，一向量往往小于 4KB（T2I 800B）→ 给 D1 page-bin 埋伏笔
- 实测身份一句：`vmem_sw` + host DRAM 窗口替身；不是产品 CXL.mem，不是 DAX

**§2.5 Sharing，五句封口（Intro 的债务）**  
(i) direct-attach 够支撑本文单 host 主张；(ii) CXL 2.0 pooling ≠ 同时映射；(iii) Shared FAM 才是多机 one-copy 的标准前提，**未评估**。到此停止。

### Fig.2：可砍，若留就只做 Fig.1(c) 的实测放大

| 选项 | 何时用 |
|---|---|
| **A. 删 Fig.2（推荐若前两页已满）** | Fig.1(c) 已有 window/NAND。Background 用文字定义 hit/miss |
| **B. 单栏栈图** | host DRAM（graph/PQ）→ score window（`mbind` 1 GiB）→ `vmem_sw` → NVMe NAND。标注 hit / miss。**禁止** 160 GB/s、双盘、Shared FAM、one-copy IOPS |

推荐 B 只在 Impl 还没画栈、且 Background 有半栏空时做。否则 A，把诚实身份留给 §5 Impl。

### 禁止写进 Background

- score hide 100%、0.59×、50.25 QPS（那是 Intro/Eval）
- wise prefetcher 算法（那是 Design）
- 「第一个…」（Intro 已 scoped）
- 成本方程、TCO、采购价
- 把 `vmem_sw` 写成 CXL.mem 产品

### 改完后的验收（审稿人 60 秒）

1. 能解释 hop 和为什么会 QD≈1。  
2. 能区分 block I/O hide 和 CXL.mem miss。  
3. 能说出 window hit vs NAND fill。  
4. 不会以为本文评估了 Shared FAM。  
5. 不会在这一节看到一条新的性能 claim。

### 任务拆解（动手时按此改 `paper/sections/background.tex`）

1. 改节名；删介质公式和 `fig:eval-scale` 引用。  
2. 重写 §2.1 walk；DiskANN 拆到 §2.2。  
3. 压缩 §2.3；AnySSD/P-HNSW 移 Related。  
4. 重写 §2.4 设备模型 + 代理一句。  
5. FAM 收成五句；Fig.2 按 A 或 B 二选一。  
6. `make`；Background 不超过 1.0 页。

---

## 1.6b Background 加长到 ≥1 页（2026-09-02）

**现状：** §2 从 p.2 中部开到 p.3 Motivation 之前，正文大约半页。四块骨架对，但审稿人读完仍会觉得「还不能自己推 C3 / page-bin」。加长目标是 **满 1.0–1.2 页**，只补 *Design 要用的机制*，不恢复已删的公式 / Fig.2 / Related。

**原则：** 每一段加完必须能回答一个审稿人问题。答不上来的句子不写。

### 加什么（按性价比）

| 加在 | 审稿人缺的问题 | 加的内容（约） | 不要加 |
|---|---|---|---|
| **§2.1 后半** | hop 时到底碰哪些字节？ | 再一段：expand $u$ 先读 $N(u)$ 的 ID（可在 host），再读 $|N(u)|$ 条向量（T2I 各 800\,B）。best-first 使 hop $i{+}1$ 依赖 hop $i$ 的分数。可加 4 行伪代码/enumerate：`pick u → read N(u) → score → insert`。 | RAG、recall 公式、QPS |
| **§2.2 后半** | DiskANN 的 hide 和我们的 hide 差在哪？ | 再一段：PQ 导航的目的是 *少读* 全精度，不是把 NAND 变快；`aio` 的 hide 是 overlap。图+向量共 sector ⇒ 一次 4KB 读绑在一起。PipeANN 跨 query 抬 QD = block 版 D2，对象仍是 sector。 | LAION-25M 整表、CPU% |
| **§2.3 后半** | 为什么 CXL-ANNS 的预取搬不过来？ | 再 4–6 句：公开 CXL-DRAM 在 140–410\,ns；NAND fill 是 20–80\,µs。可用服务在他们那里 = 没有 flash miss。Cosmos 还假设设备侧算力。 | DSA 细节、Type-2 百科 |
| **§2.4 拆开变厚** | hit/miss 和页到底怎么接？ | 现段拆成两段。(1) 设备：窗口 / NAND / hit / fill。(2) 页：4KB admission、800B 向量、sibling 浪费；load/store miss vs 显式 `PREFETCH` 都是 fill，差别在 *谁等*。(3) 代理：`vmem_sw` 保留 fill 语义，不保留产品 CXL.mem 延迟。 | 160 GB/s、双盘、from_win 数字 |
| **§2.5 Sharing（已删，2026-09-02）** | 独立小节把 Background 拽回 one-copy，且 Intro 已声明未评估 | **不恢复。** FAM 两句收在 §2.4 末：direct-attach = 本文范围；Shared FAM = 未评估。不讲 CXL 2.0 vs 3.x 百科 | 独立 §2.5、IOPS、one-copy 实验 |
| **新 Table：延迟台阶** | 三个量级要钉在一处 | 4 行表：host DRAM；score-window hit；CXL-DRAM（文献）；NAND fill（本路径 ~40–80\,µs）。Caption：cliff 的 *测量* 在 §3，本表只定位量级。 | 精确到 ns 的未测 CXL.mem |

### 仍然不恢复

介质成本方程、Fig.2、CXL-AnySSD/P-HNSW 正文、TCO、first、50.25/0.59×。

### 加完后的页结构（预期）

```
§2.1 walk + hop 字节 + 4 步 enumerate     ~0.28 页
§2.2 块路径 + PQ/aio/共扇区 + PipeANN D2   ~0.22 页
§2.3 CXL-DRAM + 延迟不可搬                 ~0.18 页
§2.4 窗口/页/代理 + FAM 两句                 ~0.30 页
（无 §2.5）
Table 延迟台阶（单栏）                      ~0.12 页
合计                                        ~1.0–1.2 页
```

### 动手顺序

1. §2.1 补 hop 字节 + 四步 expand。  
2. 插入延迟台阶表（引用文献 + 「§3 测 cliff」）。  
3. §2.2 / §2.3 各加一段对照。  
4. §2.4 拆三段；**不写 §2.5**；FAM 两句贴 §2.4 末。  
5. `make`，确认 §2 标题到 §3 标题之间 ≥1 页，且不把 Intro 挤出 Rapid Review 两页过远（Fig.1 已在 p.2）。

---

## 1.7 Motivation 改稿合同（ASPLOS 审稿人口径，2026-09-02）

**读者：** 非 ANNS 的 OS / 存储审稿人。Intro 已经用 C1–C3 *陈述* 了挑战；本章必须用 *测量* 证明：默认用法会把 140× cliff 放大，而且错的预取比漏的预取更贵。读完必须能自己推出「所以只要 wise + cont，不要第三套模块」。

**这一节的唯一工作：** 把 naive CXL-SSD ANNS 写成三条可证伪的约束。不卖 runtime QPS，不预演 Design 图，不把 LFU / Shared FAM 写成 finding。

**页预算：** 1.4–1.6 页（含 Fig.3–5）。标题必须是 `\section{Motivation}`，禁止 claim-title（现稿 `Why Naive CXL-SSD ANNS Fails` 已改）。

### 为什么现稿会被打回

| 现稿 | 审稿人反应 | 改法 |
|---|---|---|
| 标题 *Why Naive CXL-SSD ANNS Fails* | Intro 贡献 2 已经用这句话。章名再写一遍像 Results 抢戏，也会被问「这为什么不是 §6」 | 标准 ASPLOS 章名 `Motivation`。开篇一句：§2 定义设备，本节测默认用法 |
| 小标题 F1–F5，映射表又写 C1–C5 / F7 | Intro 是 C1–C3。审稿人要对两次编号 | **按 C 做小节标题。** F 只当内部实验 ID，正文可以不出现 |
| F2、F3、F4 三个并列 finding | 三个都是「不 wise」。C2 被拆碎，Design 看起来要三个模块 | F2+F3 合成 §3.2；F4 是 C2 的副作用（加宽 beam = 更大覆盖、更冷窗口），不是独立挑战 |
| 映射表把 C2 写成 placement、C3 写成 bounded promote、还出现 C4/C5 和 LFU 行 | 和锁定 D0/D1/D2 对不上；审稿人会要 D3 热集 ablation | 收成 3 行：C1→score-hide 合同；C2→wise prefetcher；C3→continuous batching |
| F5 引用 `fig:design-pipe` / `\S\ref{sec:d4}` | Motivation 预演 Design，像已经有解 | 只画 Fig.5 bubbles。连续发填是 *约束*，实现留给 §4 |
| 开篇重复「unified VA 不会自动快」 | Intro P3 刚写过。无数字的复述 = 灌水 | 开篇 4–5 句：每条 finding 回答 *exists?* + *hurts how much?* 立刻进 C1 图 |
| 50.25 / `from_win` / 0.59× | 这是 Eval 合同。Motivation 一写，审稿人会当结果提前泄露，并拿 LAION-200k 动机图打 T2I 主表 | **禁止。** 本章只报 naive 病理数字 |
| Fig.3–5 仍是 placeholder；caption 不写语料 | 主协议是 T2I-10M；动机日志是 LAION-200k。审稿人会说换了语料换了故事 | 画图；caption **必须写语料**。换 T2I 或标明 LAION-200k。定性结论（140×、错页更贵、QD≈1）不要改，除非 finding 变了 |
| F7 共享热集进主表 | 会被读成「还要一个 LFU 模块」 | 脚注：入口 pin ≠ LFU 热集当正贡献 |

### 推荐结构（对上 Intro P3，不要发明第四个 C）

```
开篇（一段）     §2 给了窗口/fill；默认当 DRAM 或当 DiskANN SSD 会怎样
§3.1 The miss cliff          = C1     Fig.3
§3.2 Prefetch must be wise   = C2     Fig.4 (a) page  (b) 盲 2-hop  [(c) 加 beam]
§3.3 One query cannot fill   = C3     Fig.5 bubbles
收口表（3 行）               C → 约束 → §4 谁答
```

**否决的两种写法**

- *保留 F1–F5 编号只改标题：* 审稿人仍要对两套 ID，映射表还会把 Design 拽回五模块。
- *叙事散文、不标 C：* Intro 已经用 C1–C3 立了合同；本章不回标，P3 变成口号。

**每小节合同（exists + hurts，4.5/5 分写法）**

| 小节 | 必须留下的一句 | 锁定/已有数（caption 标语料） | 禁止 |
|---|---|---|---|
| **§3.1 C1** | memory-semantic 接口改的是 *怎么发 miss*，不是 *miss 有多贵* | H0/F8：cold ~78 µs vs warm ~0.6–2 µs ≈ 140×；soft-cache ≠ PTE | 精确到 ns 的未测 CXL.mem；把 Table `tab:bg-latency` 再抄一遍 |
| **§3.2 C2** | 错页比漏页更贵；wise = 即将 expand 的 frontier，不是「再预取狠一点」 | page vs record ≈ 3.6× QPS / 3.7× NAND；sync 2-hop 掉 4–5× 且 hit% 几乎不动 | `--score-page`；把 placement 写成第二个 C；把 F4 升成 C4 |
| **§3.3 C3** | 一条 best-first walk 在 hop *i* 分数出来前发不出 hop *i+1*；阻塞 load/store ⇒ QD≈1 | 管线空（占用不是带宽墙）。T=1 有用流量到 oracle 也只要 ~1.02 GB/s < 1.56 | 用 T=4 的 79 QPS 证 C3；和 85.8 比；预演 hide 50.25 |
| **收口** | 三条约束 → D0 前提 + wise + cont | 3 行表 | F7/LFU 行；counters 当 knob；C4/C5 |

F4（加 beam 冷却窗口、recall 饱和）写进 §3.2 末段或 Fig.4(c)：它证明 *覆盖变宽也是一种不 wise*，不是独立的 iso-recall 控制器贡献。F9 miss-chop 控制器不进正文。

### 审稿人会打的七拳（正文里先挡）

1. 「这不就是 DiskANN 的 miss cliff？」→ 块路径藏的是 sector I/O；这里 miss 是 *faulting VA 上的 NAND fill*。新事实是接口变了、量级没变。
2. 「动机图是 LAION-200k，主实验是 T2I-10M」→ caption 写死语料；定性（数量级、错页更贵）跨语料成立，不把 29.4 QPS 写进 Intro。
3. 「那多预取不就好了？」→ Fig.4(b)：覆盖↑、precision↓、QPS↓、hit% 不动。
4. 「那加宽 L 换 recall？」→ Fig.4(c)/F4：recall 饱和后只剩 stall。质量与 stall 必须联合看，但 *控制器不是本章模块*。
5. 「mmap + `io_uring` 不就解决 C3？」→ 编程模型是 load/store + 显式 prefetch ioctl，不是 aio 队列。阻塞 memcpy 把有效 QD 打到 ≈1。
6. 「这章该进 Evaluation」→ Motivation = *默认用法的病理*；Eval = *runtime 是否按 C 修好*（Q1 回指本章，不重复画 Fig.3–5）。
7. 「Intro 已经写了 C1–C3」→ Intro *陈述*；本章 *测量*。开篇不要再论证「统一 VA 没用」，直接给图。

### 图/表合同

| 对象 | 故事功能 | 现在缺什么 |
|---|---|---|
| **Fig.3** cliff | 证 C1 | 换成 T2I 或标明 LAION-200k；可保留 soft-cache vs PTE 三档 |
| **Fig.4** 两或三面板 | 证 C2 | 画出；(a) record vs 16 KB page；(b) demand vs 1/2/8-hop；(c) 可选 beam。每面板看得到 recall |
| **Fig.5** bubbles | 证 C3 | 单 query / static batch / 需要的深度。**不要**画上 hide 的连续发填实现 |
| **Table findings** | C→约束→§4 | **3 行。** 删 F7、C4/C5、counters。`\ph` 换成上表锁定倍数 |

### 动手时改 `paper/sections/motivation.tex`（先改结构，图可后补）

1. 标题已改 `Motivation`。重写开篇，删「每个 design module 对表」这种五模块口气。  
2. 四小节收成三小节，标题用 C 的自然语言（The miss cliff / Prefetch must be wise / One query cannot fill the device）。保留 `\label{sec:mot-cliff}` 等，避免 eval/design 断引用。  
3. 删对 `fig:design-pipe`、`sec:d4` 的引用。  
4. 映射表改 3 行，对齐 D0/wise/cont。F7 最多脚注。  
5. caption 写语料。不写 50.25 / 0.59× / `from_win`。  
6. `make`，§3 控制在 1.4–1.6 页；不要把 Intro 挤出第 2 页。

**仍然不写：** Sharing/FAM、one-copy、LFU 正贡献、产品 CXL.mem ns、占用刷 100%、query-hide 已达到。

正文已按此写入 `paper/sections/motivation.tex`（2026-09-02）。图仍是 `\phbox`；caption 已是合同。作图按下面 §1.8。

---

## 1.8 Motivation 三图 plan（正文改完后定，2026-09-02）

原则：每张图只证 Intro 的一条 C。caption 已写在 `motivation.tex`，换盒子时**不要改定性句**（140× / 错页更贵 / QD≈1），除非 finding 变了。语料必须写在 caption：现有日志是 **LAION-200k × 1024，HNSW degree=32**；主评测是 T2I-10M。能重跑 T2I 更好，来不及就保留 LAION 并写死，不要把 29.4 QPS 写进 Intro。

禁止画进这三张图：hide 50.25、`from_win`、0.59×、oracle 85.8、PipeANN、连续发填的实现时间线（那是 Design Fig.8）、LFU 热集、`--score-page`。

权威数字：`motivation_exps/results/MOTIVATION_FINDINGS.md`。脚本：`motivation_exps/scripts/run_vmem_f123.sh`、`run_vmem_f4_f9.sh`。设备：`/dev/vmem0`，先 `./motivation vmem-identity`。

---

### Fig.3 `fig:mot-cliff` — 证 C1

**放什么信息（一张图一个句子）：** 窗口 hit 和 NAND miss 差两个数量级；soft-cache 命中 ≠ PTE 命中。

| 系列 | y（每向量） | 锁定量级 | 必须同时报的计数 |
|---|---|---|---|
| Cold NAND（thrash 后） | ns/vector | ~78--83 µs | `Δfaults≈Δread_ios>0` |
| Soft-cache 热 + PTE 冷（只 `MADV_DONTNEED`） | ns/vector | ~3.8 µs | `Δfaults>0` 且 `Δread_ios=0` |
| PTE-warm（窗口已建映射） | ns/vector | ~0.6--2.2 µs | `Δfaults=0`，`Δread_ios=0` |

不要第四根「产品 CXL.mem」棒。不要把 Table `tab:bg-latency` 的文献 140--410 ns 画进来（那是 CXL-DRAM，不是本路径）。

**表现形式：** 单栏柱图，**对数 y 轴**（线性轴会把 warm 压没）。三根柱，从左到右 cold → soft → warm。柱顶标 µs。不要 CDF（审稿人要比的是台阶，不是分布）。不要再叠「整次 search 时间分解」——那是 C1 的推论，正文一句话够；第二张小图会抢 cliff。

**如何测试：**

```bash
cd motivation_exps
./motivation vmem-identity
./motivation vmem-f8 --dir "$DIR" --iters 800
```

`$DIR` 默认 `laion1m_200k`。通过线（与 findings F8/H0 一致，允许 ±20%）：

1. cold / warm ≥ 100×（正文写 ~140×）。
2. soft 介于二者之间，且 `read_ios=0`、`faults>0`。
3. 换语料（可选 T2I-10M 800 个互异向量探针）只改 caption 语料名；若比值掉到 <50× 才改正文。

画图用 findings 表即可，不必等 T2I。源：F8 三行 82650 / 3794 / 2152 ns。

---

### Fig.4 `fig:mot-pathology` — 证 C2（figure*，三面板）

**放什么信息：** 三种默认 admission 都是「不 wise」：按页、盲图预取、加宽 beam。每面板必须能看见质量或精度，禁止只报 QPS。

**(a) Page vs record — 错页**

- 信息：同一 64 MB 预算，record admission 相对 16 KB page 的 QPS 与 NAND。
- 形式：分组柱。两组 x = {record, page16k}；每组两根（QPS、NAND reads，NAND 用右轴或归一化到 record=1）。
- 测试：`./motivation vmem-f2 --dir "$DIR" --nq 30 --beam 32 --hops 32 --cache-mb 64`
- 通过线：QPS record/page ≥ 3×，NAND page/record ≥ 3×（锁定 3.6× / 3.7×）。小维度缩小差距写在正文，不另开面板。

**(b) Blind sync prefetch — 错的预取比漏的贵**

- 信息：demand vs top-1/2/8 的同步 2-hop logical prefetch。QPS 掉、hit% 几乎不动、precision = used/promoted 下降。
- 形式：柱 = QPS（demand、top1、top2、top8）。柱顶或次轴标 hit% 与 precision%。不要画成「预取越狠越好」的折线。
- 测试：`./motivation vmem-f5 --dir "$DIR" --nq 16 --beam 32 --hops 32`（findings 的 F5）。`vmem-f1` 的 graph_2hop 是同故事的单点（29.4 → 6.6 QPS），不要再开第四面板。
- 通过线：最宽档 QPS 相对 demand 掉 ≥ 4×；hit% 变化 < 3 个点；precision 单调下降（锁定 82% → 29%）。

**(c) Beam sweep — 覆盖变宽也是不 wise**

- 信息：加大 $L$ 冷却窗口；recall 饱和后只剩 stall。
- 形式：横轴 beam ∈ {4,8,16,32,64}。四条线：QPS、hit%、recall@10、NAND reads。NAND 与 QPS 用左轴或各自归一化；recall 用右轴 [0,1]。必须能看出 32→64 recall 持平。
- 测试：`./motivation vmem-f4 --dir "$DIR" --nq 16 --hops 32`。`vmem-f3` 是无 recall 的 hit 耦合，只作备份。
- 通过线：存在饱和点（现数据 beam≥32，recall@10=0.244 持平）；不能只报 QPS 最优在 beam=4。F9 miss-chop **不画**。绝对 recall 偏低（hops=32）写进 caption 一句，避免审稿人拿 0.24 打主表 0.92。

三面板同一 caption 协议：LAION-200k，$d{=}1024$，beam 默认 32，hops=32。不要混 T2I 的 50.25。

---

### Fig.5 `fig:mot-bubbles` — 证 C3

**放什么信息：** 一条 walk 填不满设备；阻塞 load/store 把有效 QD 打到 ≈1；static batch 在屏障处排空。**不画** hide 的 continuous batching 实现（那是 Design）。

| 面板 | y | 读者应看到 |
|---|---|---|
| (a) 单 walk | 飞行中 fill 数 vs 时间 | 锯齿 + 尾部排空，长期低于 $D$ |
| (b) 阻塞 load/store | 同上 | 一条贴着 1 的线 |
| (c) static batch | 同上 | 组屏障处掉到接近 0，再拉起 |

$D$ 用水平虚线。$D$ 的校准（正文或 caption 一句，不另开图）：孤立随机 `PREFETCH_BATCH` 分母 **1.560 GB/s**；单 walk 有用流量远低于此。不要把 T=4 的 79 QPS 画成「已经填满」。

**表现形式：** TikZ 示意时间线（现有 `paper/sections/fig_bubbles.tex` 可改，不要原样用）。改动：删掉现稿顶栏 `(c) Continuous fetch batching (proposed)`；改成上表 (b) 阻塞 QD≈1。三栏竖排，红斜线 = 空闲容量，蓝 = 在飞 fill。这是 **示意**，不是示波器截图。

**如何测试（给示意当证据，数字进 caption 或附录，不进 Intro）：**

1. **阻塞 QD≈1。** 单线程 demand：每次 miss 走阻塞 `memcpy`/fault，记录同时飞行 ioctl 数。期望：max in-flight = 1。  
2. **单 walk 空管线。** 一条 query 的 fault/`PREFETCH` 时间戳 → 占用时间分数。期望：远小于 1，且呈 hop 锯齿。  
3. **static batch 排空。** 按 hop 或按 query 组加屏障，画 in-flight。期望：屏障处掉到 0。  
4. **不要** 在这张图上叠加 hide runtime 的 T=1 或 T=4 占用曲线。那是 Eval Q4 / Design Fig.8。

现稿 `fig_bubbles.tex` 的 (a) 标了 `pipe_search`——改成 generic “single walk”，避免审稿人以为在黑 PipeANN。

---

### 作图顺序与通过门

| 步 | 做啥 | 完成定义 |
|---|---|---|
| 1 | 用 findings 现数画 Fig.3 柱 + Fig.4 三面板（Python/matplotlib 或 pgfplots） | caption 语料、倍数与正文一致；log y 在 Fig.3 |
| 2 | 改 `fig_bubbles.tex` 为 Fig.5 三病理面板 | 无 “proposed continuous” 栏 |
| 3 | 换掉 `\phbox`，`make`，看 §3 是否落到 1.4–1.6 页 | 不要把 Intro 挤出第 2 页 |
| 4（可选） | T2I-10M 重跑 f8/f2/f5 | 仅当比值仍过通过线才改 caption 语料；**不改** Intro 锁定行 |

不需要第四张动机图。Table `tab:findings` 已填 3 行，不再用 `\ph`。

---

## 1.8b Fig.3 已改成 CXL-DRAM vs CXL-SSD miss（2026-09-02 晚）

**对比：** Host 从 Montage `/dev/dax0.0` 读一条 item（`clflush` 后 load）vs `vmem_sw` 窗口 miss 读一条。

| 语料 | item | CXL-DRAM $p_{50}$ | CXL-SSD miss $p_{50}$ | 比 |
|---|---|---|---|---|
| LAION-10M（25M dump 前 10M，$d{=}512$） | 2048 B | 666 ns | 88.7 µs | 133× |
| T2I-10M（staged pagebin，$d{=}200$） | 800 B | 281 ns | 88.8 µs | 316× |

Miss 几乎不随 record 变（都是 4 KB fill）。没有独立 LAION-10M 文件；SSD 侧 LAION 是 **同机同路径、按 2048 B 读**，不是把 25M 写进 T2I 盘。日志：`motivation_exps/results/probe_item_cliff_20260902b.log`。

---

## 1.9 Fig.4 pathology 改图 plan（Fig.3 完成后再做，未执行）

**现图为什么读不出来：** 三面板三套轴；(b) hit% 四根柱都是 57.5，视觉零信息；(c) 四条归一化线 + 右轴 recall，读者不知道 1.0 是谁，也看不出「加 beam = 不 wise」。

**这一张图只证 C2 一句：** 错页比漏页更贵。不要在同一张图里再讲 beam 控制器。

**推荐（A，两面板，删 c）：**

| 面板 | 读者 3 秒内应看到 | 形式 | 数 |
|---|---|---|---|
| (a) 按页 admission | record 比 16 KB page 快、少读盘 | 分组柱：QPS + NAND（右轴或归一到 record=1） | F2：33.5 vs 9.3；9.9k vs 36.5k |
| (b) 盲预取 | 覆盖越宽 QPS 越差，precision 掉 | **只要 QPS 柱**；柱顶写 precision。不要画平的 hit% | F5：25.9 / 21.0 / 15.2 / 5.0；prec 82→29 |

F4 beam 退回正文 3 句 + caption 半句：「加宽 $L$ 是同一种不 wise（覆盖变冷，recall 饱和）」。需要图再单独做单栏：横轴 $L$，只留 recall@10 和 NAND 两条，32→64 持平一眼能看懂。

**不推荐：** 继续三面板四轴；(c) 再堆 QPS/hit/NAND/recall。

**caption：** 仍写 LAION-200k（这是 F2/F5 的语料，不是 Fig.3 的 10M）。不要混 T2I 50.25。

**动手顺序（得到同意后再改）：** 重画 (a)(b) → 删 (c) 或拆成小图 → 改正文 §3.2 指向 → `make`。

---

## 1.5 Intro 五段合同（2026-09-02 晚，按作者提纲细化）

作者提纲对，但第 1 段的 first 必须改口：不是「第一个 CXL-DRAM ANNS」（CXL-ANNS/Cosmos 已是），而是「第一个在 *flash-backed* CXL 地址空间上把 scoring 做可用的 graph-ANNS runtime」。

| 段 | 审稿人要听到 | 禁止 |
|---|---|---|
| P1 能力 + 新硬件 + first | graph ANNS 重要 → CXL 可 load/store → CXL-DRAM 已有可用服务 → 本文是 CXL-SSD | first on CXL-DRAM；RAG 开场超过一句 |
| P2 两类工作 | DiskANN 藏 block I/O、复制税；CXL-ANNS 无 flash、DRAM 税；我们做第三条服务 | 在挑战之前就开始卖 QPS 赢了谁 |
| P3 挑战 | C1 140×；C2 预取必须 wise；C3 单 query QD≈1 | 泛泛「硬件很慢」；把带宽当 T=1 主挑战 |
| P4 设计 | D0 放置 + wise prefetcher + continuous batching；score hide 100%；query hide 0.59× | 五模块；hide = oracle |
| P5 贡献 | 四条，与 P3/P4 一一对应 | one-copy first；打败 DiskANN |

正文已按此写入 `paper/sections/intro.tex`。Fig.1 插在 P2 后。Scope 段保留在 enumerate 之后（ASPLOS 诚实，不算第六贡献）。

---

## 1.5b Intro 改稿说明书（先改这一节，归档）

现稿 `paper/sections/intro.tex` **不要推翻重写**。三条部署点、scoped first、proxy scope 都对。要改的是：**开篇问的问题**、**hide 合同的强度**、**Shared FAM 占的篇幅**、**贡献句和 Abstract 与锁定数字对齐**。

Rapid Review 只看前两页。读者读完 Intro 必须能复述：

1. 已有两条线（DiskANN 块路径 / CXL-DRAM 全内存），CXL-SSD 是第三条点。
2. 统一 VA 不会自动可用：140× cliff + 四种 naive 失败。
3. 系统目标是 **score hide**（打分只从窗口）；**query hide** 是 oracle，当前 0.59×。
4. first 只覆盖「SSD-backed CXL VA 上的 graph-ANNS runtime」。

### 段落级改动

| 现稿位置 | 现在在说什么 | 问题 | 改成 |
|---|---|---|---|
| L11–21 开篇 + quote | 加机器能不能不再复制冷库、不抬进 DRAM | 问的是 **one-copy 部署**，本轮没有这个结果 | 问改成：**在 flash-backed CXL 上，graph ANNS 怎样才算可用？** 容量/吞吐独立扩展只当一句背景，不要当 quote |
| L23–35 DiskANN | 复制/分片税 | 保留，但降为「他们已经解决块路径」 | 强调 DiskANN/PipeANN **藏的是 block I/O**；我们不替代这条路径 |
| L37–44 CXL-DRAM | 冷尾付 DRAM 价 | 保留 | 一句收：他们没有 NAND miss 要藏 |
| L46–58 第三条点 + Shared FAM 三条件 | 机会 + 很长的 CXL 2.0/3.x | FAM 三条件抢 Rapid Review 篇幅，把故事拽回 one-copy | **Intro 只留一句**：one-copy 需要 Shared FAM，本文未评估。三条件整段搬去 Background |
| Fig.1 caption L68–74 | opportunity 含 one-copy under Shared FAM | 和图注并列成第三贡献 | caption 改成：机会 = flash 价 *D* + 小窗口；系统 = hide。Shared FAM 用脚注或小字 |
| L80–92 四种 naive 失败 | F1–F4 | 好，保留 | 第 (4) 后面加半句：这就是 pointer-chasing，也是 0.59× 的来源 |
| L94–109 系统段 | 「invisible on the search critical path」+ five modules | **过强**：把 query hide 写成已追求且像已达到；五模块含作废 LFU 叙事 | 拆成：score hide 已作为合同达到；query hide 是 oracle，T=1 = 0.59×；模块写成 **三个**（placement / bounded promote / hide engine），entry pin 一笔带过 |
| L111–115 first | scoped first | 好，几乎不动 | 保持。不要加 one-copy |
| 贡献 1 L120–125 | 第三条点 + one-copy 何时成立 | 贡献 1 仍在卖 FAM 条件 | 改成：第三条点 + 机会/系统拆开。one-copy 不进贡献句 |
| 贡献 3 L130–133 | runtime that *tries* to hide | 方向对，缺数字 | 写明：`from_win=100`；单 query 相对窗口 oracle **0.59×**，残差是 hop 串行 |
| 贡献 4 L134–137 | iso-recall hide eval | 好 | 点名 Demand 1.64 → hide 50.25 → oracle 85.8；PipeANN 是残差 |
| Scope L140–147 | proxy / 无 DAX / 无 160 GB/s | 好，保留 | 加半句：1 GiB 窗口是 host DRAM 替身，不称 CXL-DRAM |
| Abstract `main.tex` L36–65 | 合同写成 DRAM-class wait；主数字仍是 P3 5.35 | 和 Intro、锁定 hide 行冲突 | Abstract 与 Intro 同步改；主数字改 50.25 / 0.59× / `from_win=100` |

### Intro 目标叙事（改完后的骨架）

```
P1  背景：billion-scale ANNS 要同时有 flash 价容量和可服务延迟。
    问：flash-backed CXL memory 上，graph search 怎样才可用？
P2  DiskANN/PipeANN 已经把 block-SSD 做好；扩吞吐仍常复制或分片。
P3  CXL-ANNS/Cosmos 把图+向量放进 CXL-DRAM，没有 NAND cliff，但冷尾付 DRAM 价。
P4  CXL-SSD 是第三条点（Fig.1）。机会 ≠ 系统。Shared FAM = 未评估的部署路径。
P5  统一 VA 仍会失败（四种 naive）。
P6  FlashANNS：打分只从窗口（score hide，已到）；
    query 临界路径不减到 DRAM（query hide，0.59×，指针追逐）。
P7  scoped first + 四条贡献 + proxy scope。
```

### Intro 改完才动的后续（顺序不要反）

1. **Abstract**（`paper/main.tex`）与 Intro 同一天改，否则 Rapid Review 自相矛盾。
2. **Discussion** 降调「host 感觉不到 SSD」。
3. **Design** 五模块改为三模块口径，与 Intro P6 一致。
4. **Eval** money plot / Table 2 用 Intro 已经预告的三个数（1.64 / 50.25 / 85.8）。
5. **Motivation / Fig.1** 可以后补画图，但不改 Intro 已经定下的 caption 合同。

---

## 2. Design 模块（按故事线裁剪，不再用旧 M1–M5）

旧 plan §4 的 D3 Shared LFU hot set **已作废**（hit 升、QPS 崩）。本轮只保留四个能指认到 hide 的模块：

| 模块 | 回答的 finding | 现状 | 论文里的地位 |
|---|---|---|---|
| **D1 Placement** | F2：页与图错配 | 图在 host DRAM（1.28 GiB `pagebin_graph.bin`）；向量+expand-bundle 在 NAND | 正贡献 |
| **D2 Bounded promote** | F3/F5：盲预取有害 | `install_top` + expand-bundle；禁止 `--score-page` | 正贡献 |
| **D3 Entry / soft-pin** | F7 的瘦身版 | 只 pin 入口子图；LFU 热集 void | 负结果驱动的「我们不做什么」 |
| **D4 Hide engine** | 指针追逐 / QD≈1 | async fill + ebatch=8/ahead=2；`from_win=100`；overlap=0.07 | 正贡献，残差写进同一节 |
| D5 计数器 | 可观测性 | `from_win` / `crit_wait` / `issue_use` / occupancy | 测量合同，不当贡献第 5 条 |

可选工程刀（**不改 Intro**，除非 nq=20 与 nq=100 同时赢）：`--spec-beam-nbrs M`。设计在 `docs/superpowers/specs/2026-08-31-break-best-first-prefetch-design.md`。期望吃 2–4 ms，不是到 11.4 ms。

---

## 3. 实验安排（图为故事服务）

协议冻结（改协议 = 新论文行，不是补点）：

- 语料：T2I-10M，oneshot FP，L=400，k=10，seed=42，recall@10 ≥ 0.92
- True-cold：`rmmod`+`insmod` 到 `cache_used=0`。`--flush-window` 不算
- 窗口：1 GiB host DRAM（`mbind` node 1），**不说成 CXL-DRAM**
- 占用分母：孤立随机 `PREFETCH_BATCH` **1.560 GB/s**
- 利用率：`issue_use`，禁止 page-touch 或 `--score-page`

### 3.1 必须出的图（投稿最小集）

| 图 | 故事功能 | 数据从哪来 | 现在缺什么 |
|---|---|---|---|
| **Fig.1** 三点部署 + runtime overlay | Rapid Review 第一页 | 画图，不需新跑 | placeholder |
| **Fig.3** miss cliff | Motivation F1 | H0/F8：~78 µs vs ~0.6–2 µs | 换成 T2I 或标明 LAION-200k |
| **Fig.4** page / 盲预取 / beam | F2 F3 F4 | findings 表已有 | 画出来；caption 写语料 |
| **Fig.6** 架构：score 与 fill 拆开 | Design | 现有系统图 | placeholder |
| **Fig.9 money plot** | 主结果 | Demand 1.64；hide packed 24.67；hide pagebin 50.25/49.25；oracle 85.8；PipeANN 57.6 | 盒子未画；补 crit_wait 轴 |
| **Table 2** 主表 | 锁定行 | `paper/sections/eval.tex` 已有关键 QPS | NAND/promote 列仍是 `\ph` |
| **Fig.10 + Table 3** ablation | 每个模块必须动 hide | **过期**：表还写 Full=5.35 / 34% hit | **必须用 hide runtime 重跑** |

### 3.2 有时间再做

| 实验 | 目的 | 通过线 |
|---|---|---|
| `--spec-beam-nbrs` ∈ {8,16,32} | 缩小 0.59× 残差 | mean 在 nq=20 与 nq=100 都低于复基线；`from_win=100`；QPS ≥ 基线−2% |
| T=4 服务图 | 跨 query 填 QD | caption 写清：79–87 QPS **不是** 单路 hide 成功 |
| True-cold vs 丢掉的 warmup | Q4 | 若只有 warm 才 hide，这句话不准进 Intro |

### 3.3 本轮不做（会带偏故事）

- 双盘 restage / HPS / BAR / CXL-DRAM DAX
- Shared FAM / 多 host one-copy QPS
- `--score-page`、占用刷到 100%、lookahead-k=8、T=1 加宽 ebatch
- 把 LFU 热集画成正 ablation
- 用 `4×85.8` 或历史 T=4 shard 136 当公平上界（2026-09-02 公平对照已改同一份 2 GiB）

### 3.4 Baseline 在故事里的位置

| Baseline | 回答 | 不要用来回答 |
|---|---|---|
| Demand / page / sync-2hop / miss-chop | naive 为何不可用 | 主 claim「我们更快」 |
| 窗口 oracle（WS pin，无 NAND） | hide 的上界 | 产品 CXL-DRAM |
| PipeANN T=1，PQ on | 块路径 residual | iso-protocol SOTA 胜利 |
| 全 host DRAM populate | 算力上界，可选 | 和 1 GiB 窗口比成本 |

---

## 4. 文件地图

| 文件 | 职责 |
|---|---|
| `paper/sections/intro.tex` | 故事线正文；改贡献句必须同步本 plan §0 |
| `paper/sections/motivation.tex` | F1–F4；换 Fig.3–5 |
| `paper/sections/design.tex` | D1/D2/D4；删 LFU 正贡献语气 |
| `paper/sections/eval.tex` | Fig.9 / Table 2 / 重做 Table 3 |
| `paper/sections/discussion.tex` | 把 hide 合同降到 score hide + 0.59× |
| `docs/notes/2026-08-31-组会汇报.md` | 锁定数字的唯一权威 |
| `docs/plan/2026-07-31-cxl-ssd-anns-asplos-plan.md` | 历史材料库，不再当执行清单 |
| `docs/superpowers/plans/2026-08-31-hide-toward-oracle.md` | 工程轨；只有 spec-beam 通过才回写论文行 |

---

## 5. 任务

### Task 1: 冻结 Intro 故事线与贡献句

**Files:**
- Modify: `paper/sections/intro.tex`
- Modify: `paper/sections/discussion.tex`
- Modify: `docs/plan/2026-07-31-cxl-ssd-anns-asplos-plan.md` §0（指向本 plan）

- [x] **Step 1: 对照 §0.1 改 Intro 里所有「host unaware / cannot see flash」**

只保留 score-path 表述。在 pointer-chasing 句后加一句残差：single walk cannot hide hop *i+1* until hop *i* scores；measured T=1 is 0.59× the in-window oracle.

- [x] **Step 2: 贡献列表保持四条，但第 3 条改成 hide 尝试 + 残差诚实**

不要把 one-copy、Shared FAM、逼近 DiskANN 写进 enumerate。 Abstract 已与 Intro 同步。

- [ ] **Step 3: Discussion「Hide is the service contract」降调**

改成：score hide 已达到；query hide 是 oracle，当前 0.59×；若 crit_wait 永远停在 NAND 级，贡献 3 收缩为 overlap vs demand。

- [ ] **Step 4: 编译**

Run: `make` in `paper/`
Expected: `main.pdf` 生成；Intro 不再出现未限定的 one-copy first。

---

### Task 2: Motivation 三图按 F1–F4 填实

**Files:**
- Modify: `paper/sections/motivation.tex`
- Data: `motivation_exps/results/MOTIVATION_FINDINGS.md`

- [ ] **Step 1: Fig.3 用已有 cliff 数**

H0：warm ~0.6 µs vs cold ~78 µs。Caption 写语料（LAION-200k 或复测 T2I）。不要等新硬件。

- [ ] **Step 2: Fig.4 三面板：page vs record、sync prefetch、efSearch**

F2：33.5 vs 9.3 QPS。F5：top8_2hop 5.0 QPS。F3/ef：beam 变冷。负结果写成设计约束。

- [ ] **Step 3: 若来不及复测 T2I，caption 明确「motivation corpus ≠ eval corpus」**

禁止在 Intro 把 200k 上的 3.6× 写成 T2I-10M 的主结果。

---

### Task 3: 主结果表与 money plot

**Files:**
- Modify: `paper/sections/eval.tex`
- Data: `docs/notes/2026-08-31-组会汇报.md` §4

- [ ] **Step 1: Table 2 锁定行不许改**

`hide pagebin nq=20 = 50.25 / 0.940`；`nq=100 = 49.25 / 0.933`；`oracle 1 GiB = 85.78 / 0.925`。从锁定 log 补 NAND/promote，不要重跑换 QPS。

- [ ] **Step 2: 画 Fig.9**

轴：iso-recall 下 crit_wait（主）+ QPS（次）。线：Demand、hide packed、hide pagebin、oracle、PipeANN T=1。Caption：`50.25/85.8≈0.59×` 是残差，不是失败藏起来。

- [ ] **Step 3: 删掉用 T=4 83.5% 证明 hide 成功的句子**

旧 plan 2026-08-31 节的 T=4 / 136.23 只可进 appendix 或「服务吞吐」，并注明 2026-09-02 公平对照已改同一份 2 GiB。

---

### Task 4: 用 hide runtime 重跑 ablation（唯一必跑实验）

**Files:**
- Modify: `paper/sections/eval.tex` Table 3 / Fig.10
- Run: `serving/search_beam` hide 路径（pagebin + expand-bundle）

- [ ] **Step 1: 在复基线 ≈50 QPS / 20 ms 的机器上跑 leave-one-out**

变体：Full pagebin；−pagebin（packed）；−PREFETCH_BATCH；−graph-in-host（若可）；Demand。协议与锁定行相同。

- [ ] **Step 2: 主指标是 `from_win` 和 `crit_wait`，QPS 是辅**

去掉 prefetch 必须让 hide 失败（`from_win` 掉或 crit_wait 升）。旧表 Full=5.35 / 34% **整表替换**。

- [ ] **Step 3: 不把 −LFU 画成损失**

LFU 行保持 void。

---

### Task 5: 架构图与 Rapid Review 自检

**Files:**
- Modify: `paper/sections/intro.tex` Fig.1
- Modify: `paper/sections/design.tex` Fig.6

- [ ] **Step 1: Fig.1 三列**

(a) replicated local-SSD；(b) full CXL-DRAM；(c) 图/PQ 在 host，窗口打分，语料在 CXL-SSD。不要画 160 GB/s。

- [ ] **Step 2: Fig.6 画 fill / score 拆分**

上：Placement | Admit | Prefetch+cont | L ctrl。中：residency plane。下：host DRAM | 窗口替身 | CXL-SSD。标注 `vmem_sw`。

- [ ] **Step 3: 只读前两页**

检查 Rapid Review 能否在不翻 Eval 的情况下复述：第三条点、score hide、0.59× 残差、scoped first。

---

### Task 6（可选）: spec-beam 只作为缩小残差的工程，不改故事

**Files:**
- Spec: `docs/superpowers/specs/2026-08-31-break-best-first-prefetch-design.md`
- Do not modify Intro unless both nq=20 and nq=100 win

- [ ] 默认 `M=0`，claim 路径不变
- [ ] 扫 M∈{8,16,32}；只看 mean 和 `crit_wait`
- [ ] 失败整刀撤回，不改论文故事线

---

## 6. 成功标准（这篇能投）

1. **Why 站得住：** 审稿人不能用「那用 DiskANN / 那用 CXL-DRAM」打发。切割表在 Related，机会在 Intro，系统在 Design。
2. **Oracle 不撒谎：** 正文清楚区分 score hide（已到）和 query hide（0.59×）。
3. **Problem 站得住：** F1–F4 有数；负结果是设计约束。
4. **Design 可指认：** ablation 在 **hide runtime** 上，去掉 prefetch/pagebin/graph-in-host 会动 hide 计数。
5. **Eval 主表一张：** iso-recall 下 Demand → hide → oracle，PipeANN 是残差。
6. **不夸大：** proxy 设备、host 窗口替身、无 Shared FAM、无 160 GB/s。

---

## 7. 和旧 plan 的关系

`docs/plan/2026-07-31-cxl-ssd-anns-asplos-plan.md` 仍保留：§1.1 部署点论述、§1.2 事实墙、Related 八维表、早期 findings。那些是材料。

不再执行旧 plan 的：

- §0 旧 one-copy 主线
- §1.3.1「贡献 = 接近 DiskANN + 不复制 cold corpus」
- §4.4 D3 Shared LFU 作为正模块
- §5.3 E3/E4 以 shared cache / iso-recall 控制器当主导 claim
- §6 阶段 B「再做 2–3 周 MVP」（系统已经是 hide runtime）
- §10「先跑 M-E1 + 开 M2/M3 接口」

---

## 变更记录

| 日期 | 说明 |
|---|---|
| 2026-09-02 | 按 plan.md:58–62 冻结 hide 故事线；裁掉 one-copy 执行项；实验最小集对齐锁定 hide 行 |
