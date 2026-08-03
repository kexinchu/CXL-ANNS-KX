# CXL + ANNS 学术文献调研

> 整理日期：2026-07-30  
> 范围：明确以 **CXL 为硬件/系统底座**，并直接服务 **ANNS / ENNS / 向量检索 / RAG dense retrieval** 的工作；旁支近存/近盘 ANNS（非 CXL）仅作对照列出。  
> 说明：领域仍在快速增长，下文以目前可公开检索到的核心论文为主；若有遗漏可再补。

---

## 0. 总览：论文在做什么？硬件怎么用 CXL？

可以把现有工作分成三条主线：

| 主线 | 核心想法 | 代表工作 | CXL 角色 |
|------|----------|----------|----------|
| **A. CXL 内存池 + 近端距离计算** | 全量图/向量放 CXL；主机做遍历，设备算距离 | **CXL-ANNS** (ATC’23 / TOCS’24) | Type-3 内存扩展 + EP 内 DSA |
| **B. 全流程卸载到 CXL-PNM** | 遍历 + 距离都在设备侧 | **Cosmos** (IEEE CAL’25)、**CMM-Ax**、**IKS** | Type-2/3 + 控制器内算力 / rank-PU |
| **C. 把 CXL 当「第二层存储」** | 替代 SSD 存索引，利用细粒度随机读 | **Second-Tier Memory** (arXiv’24) | 商用 CXL Memory Expander（软件为主） |
| **D. 稀疏向量 / 专用索引** | 倒排+NMP，面向 sparse ANNS | **SpANNS** (arXiv’26) | **Type-2** + 计算型 DIMM |
| **E. 远内存感知的量化/精炼** | 多级残差量化，减少远存/SSD 精炼代价 | **FaTRQ** (arXiv’26) | 远内存/CXL 作为精炼层假设 |

**与本机关系（gpu01）**：当前是 **Type-3 风格 Montage CXL DRAM（~128 GiB）+ NVMe**，没有公开论文里的 EP-DSA / rank-PU / Type-2 稀疏加速器。最接近可复现的是 **C 线（软件分层）** 与 **A 线的软件部分（图缓存/预取/绑 NUMA）**；硬件加速结论多来自 FPGA/仿真，不可直接外推。

---

## 1. 核心论文逐篇摘要

### 1.1 CXL-ANNS（奠基作）

- **标题**：*CXL-ANNS: Software-Hardware Collaborative Memory Disaggregation and Computation for Billion-Scale Approximate Nearest Neighbor Search*  
- **会议/期刊**：USENIX ATC 2023；扩展版 *Bridging Software-Hardware…* ACM TOCS 2024  
- **作者单位**：KAIST / Panmnesia（Junhyeok Jang, Myoungsoo Jung 等）  
- **链接**：[ATC PDF](https://www.usenix.org/system/files/atc23-jang.pdf) · [ATC 页面](https://www.usenix.org/conference/atc23/presentation/jang)

**做了什么**

1. 把 billion-scale 图与向量全部放入 **CXL 内存池**，避免 DiskANN/HM-ANN 式「压缩搜 + SSD 精炼」带来的延迟与精度损失。  
2. 观察到裸用 CXL（主机 load/store 远程 HDM）比本地 DRAM oracle **慢约 3.9×**（flit 转换开销）。  
3. 提出软硬协同：  
   - **关系感知图缓存**：按距 entry 的 hop 把热邻接放本地 DRAM；  
   - **ANNS 感知预取**：用 candidate array 推测下一跳，掩盖 CXL 延迟；  
   - **EP 侧 DSA 距离计算** + **向量按列分片（sharding）**；  
   - **子任务依赖放松与调度**（urgent vs deferrable）。  
4. 报告相对 DiskANN/HM-ANN 等可达约 **111× QPS**、延迟降 **93%**；甚至超过「无限本地 DRAM」oracle（延迟/吞吐约 **0.32× / 3.8×** 量级，论文数字）。

**CXL 硬件怎么设计**

```
Host (RISC-V CXL CPU 原型)
  └─ CXL Switch
       └─ 多个 Type-3 EP（默认 4×，大库更多）
            ├─ PHY + CXL engine（flit ↔ 内存请求）
            ├─ DSA（距离加速）：多 PE，加减乘树，L2/IP
            └─ 4× memory controller × DIMM（原型每 EP ~1TB 量级叙述）
```

- **为何 Type-3**：易通过 switch 规模扩展；HDM 映射进主机物理地址，软件仍用 load/store。  
- **计算接口**：距离命令不走“大包搬向量”，而用 **CXL.io 映射的 doorbell + 本地 DRAM command buffer**；EP 主动拉取邻居列表，结果推回主机。  
- **验证**：16 nm FPGA 原型 + Linux 5.15 驱动 + FAISS；大规模用 **gem5 周期级仿真**（与原型交叉验证）。  
- **索引**：图侧偏向 **NSG**；数据来自 BigANN billion 集。

**一句话**：CXL 当可扩展内存池；**主机管图遍历/候选，设备管距离**。

---

### 1.2 Cosmos

- **标题**：*Cosmos: A CXL-Based Full In-Memory System for Approximate Nearest Neighbor Search*  
- **出处**：IEEE Computer Architecture Letters, 2025；[arXiv:2505.16096](https://arxiv.org/abs/2505.16096)  
- **动机**：RAG 需要 billion-scale 低延迟 ANNS；相对 CXL-ANNS，进一步减少主机干预与带宽瓶颈。

**做了什么**

1. **完整 ANNS 卸载**到 CXL 设备上的 **GPC（通用核）**：图遍历 + 候选管理都在设备侧。  
2. **Rank-level Processing Unit**：向量按维拆到 DRAM rank，并行算部分距离（L2/IP，约 64B 子向量）。  
3. **邻接感知的 cluster 放置**：相邻 cluster 打散到不同 CXL 设备，降低负载不均。  
4. 相对“主机算 + CXL 存”基线，SIFT1B/DEEP1B trace 上最高约 **6.72×** 吞吐；相对复现的 CXL-ANNS 类方案约 **2.35×**。

**CXL 硬件怎么设计**

```
Host ── CXL Switch ── N× CXL memory devices
                         ├─ CXL controller + CXL-PNM（含 GPC）
                         └─ DRAM + per-rank PU
```

- 主机只发查询、聚合各设备 local top-k。
- HDM 静态映射进 HPA；`mmap` + `mlock` 固定映射。  
- **评估**：Ramulator 仿真（如 4×256 GB 设备，DDR5-4800）；非大规模实机。

**相对 CXL-ANNS**：CXL-ANNS 只把 **距离** 卸到 DSA；Cosmos 把 **遍历也卸到 GPC**，并用 rank-PU 吃满内部带宽。

---

### 1.3 Second-Tier Memory 向量索引（SJTU 等）

- **标题**：*Characterizing the Dilemma of Performance and Index Size in Billion-Scale Vector Search and Breaking It with Second-Tier Memory*  
- **出处**：[arXiv:2405.03267](https://arxiv.org/abs/2405.03267)（Xingda Wei 通讯等）  
- **定位**：**系统/软件为主**，CXL 是第二层介质之一（另有 RDMA、NVM）。

**做了什么**

1. 刻画 SSD 上图索引/聚类索引的 **吞吐 ↔ 索引放大** 困境：为提速往往要把索引做大（DiskANN/SPFresh 可达数倍数据集大小）。  
2. 根因：ANNS 需要 **小粒度随机读（~100–256 B）**，与 SSD **4 KB** 友好粒度不匹配。  
3. 论证 **second-tier memory（CXL/RDMA/NVM）** 支持更细粒度随机读，可在 **更小索引放大** 下达到近优吞吐。  
4. 重新设计：  
   - **图索引**：改造执行流水，掩盖远存延迟，避免当纯 DRAM 用；  
   - **聚类索引**：解耦复制 + grouping，减少小随机读。  
5. 发现与 SSD 常识相反：在第二层内存上，**图索引可以比聚类索引更快/更省空间**。

**CXL 硬件怎么设计**

- **不新做 CXL ASIC**；使用商用 **CXL Memory Expander**（文中提及 MXC 等，PCIe 5.0 ×8 量级）。  
- 把 CXL 当 **存储语义的细粒度介质**，而不是近存 DSA。  
- 对比表（论文 Table 量级）：CXL 延迟约 **0.3 µs**，介于 DRAM 与 RDMA/SSD 之间。

**一句话**：最贴近「向量在慢层、利用 CXL 细粒度」的软件路线；**没有 hit/miss 加速器，但有索引布局/流水线设计**。

---

### 1.4 SpANNS（稀疏向量）

- **标题**：*SpANNS: Optimizing Approximate Nearest Neighbor Search for Sparse Vectors Using Near Memory Processing*  
- **出处**：[arXiv:2601.03229](https://arxiv.org/abs/2601.03229)

**做了什么**

1. 面向 **sparse ANNS**（倒排/IR），弥补 dense 加速器已很多、sparse 仍偏 CPU 的缺口。  
2. **混合倒排索引**：L1 维倒排 + L2 聚类 silhouette + Forward Index 精排。  
3. 相对 Seismic 等 CPU 基线约 **15–22×**。

**CXL 硬件怎么设计**

```
Host CPU（解析稀疏 query）
  └─ CXL Type-2 Controller
       ├─ L1 inverted（控制器内 ~1MB）
       ├─ L2Inv DIMMs（silhouette / 候选指针，Ellpack）
       └─ F-Index DIMMs（完整记录 + rank 级距离/匹配单元）
```

- Type-2：共享内存、减少主机拷贝；DIMM 间可直传候选指针。  
- 评估：Ramulator2 + SystemVerilog + ASAP7 综合（面积/功耗开销报告较小）。

---

### 1.5 IKS（Intelligent Knowledge Store）— 偏 ENNS / RAG

- **标题**：*Compute-Enabled CXL Memory Expansion for Efficient Retrieval Augmented Generation*  
- **链接**：[PDF](https://derrickquinn.github.io/cxl.pdf)（Quinn 等；文献中常被称作 IKS / Quinn 路线）

**做了什么**

1. 主张 RAG 在高精度下可用 **ENNS（精确 kNN）**，用 CXL 近存加速 similarity。  
2. **IKS**：CXL 内存扩展器 + 多颗轻量 **NMA**，各绑一包 LPDDR；总容量叙事约 **512 GB**、内部带宽约 **1 TB/s**。  
3. 解决 NMP 常见问题：**内存搁浅（stranding）**——设备 DRAM 也可当主机内存用。  
4. FAISS 增加类似 GPU 的 `IKSIndexFlatIP`；报告相对 CPU ANNS 基线可达很高吞吐增益（论文称最高约 **37×** 量级，需按场景理解）。

**CXL 硬件怎么设计**

- 同时用 **CXL.mem + CXL.cache**（控制/scratch 一致性；向量更新少，软件刷缓存）。  
- 每 NMA：多 PE 做点积/相似度 + top-k；列主布局批量复用。  
- 低 offload tax：共享地址空间，细粒度启动搜索。

**注意**：主打 **exact / dense retrieval kernel**，不是 DiskANN 式外存图 ANNS；但与 CXL+检索强相关。

---

### 1.6 CMM-Ax（可部署 CXL-PNM + 向量库栈）

- **标题**：*Toward Deployable CXL-PNM: The CMM-Ax Prototype and Software Stack*  
- **出处**：IEEE Transactions on Computers, 2026  

**做了什么**

1. 强调现有 CXL-PNM 多为仿真/难落地；做 **可部署原型 + 完整软件栈**。  
2. 与 **HMSDK** 集成：`malloc`/`mmap` 路径暴露异构内存；**FAISS backend**、领域编译器、**K8s device plugin** 多租户切分。  
3. FPGA **CXL Type-3** 原型：exact kNN 相对纯 CXL 内存基线约 **4.54×** 吞吐、**5.56×** 更低每查询能耗；IVF ANN 在小 batch 下带宽效率优于 CPU/GPU 叙述。

**硬件**：Type-3 + streaming near-memory pipeline + 设备侧可编程性（混合）。

---

### 1.7 FaTRQ（远内存感知量化）

- **标题**：*FaTRQ: Tiered Residual Quantization for LLM Vector Search in Far-Memory-Aware ANNS Systems*  
- **出处**：[arXiv:2601.09985](https://arxiv.org/abs/2601.09985)

**做了什么**

1. 观察：CXL-ANNS 等把全精度向量放 CXL，**容量仍可能不够**；DiskANN 式精炼仍易打到慢存储。  
2. 提出 **分层残差量化**：在快层/远层之间做渐进精炼，减少 SSD 访问。  
3. 可与 NDP/CXL 加速 **正交组合**（论文定位偏算法+系统分层，而非新 CXL ASIC）。

**硬件**：不绑定专用 CXL 芯片；假设存在 far-memory（含 CXL）层级。

---

## 2. 相关但「非 CXL 主线」的对照工作

读 CXL+ANNS 时常被引用，但硬件不是 CXL：

| 工作 | 关系 |
|------|------|
| **DiskANN / FreshDiskANN / SPFresh** | SSD 外存 ANNS 基线；CXL-ANNS / Second-Tier 的主要对比对象 |
| **HM-ANN** | DRAM–NVM 分层图；思想类似「热层/冷层」，介质是 PMEM 非 CXL |
| **ANSMET** (ISCA’25) | DIMM-NDP + early termination；文中对比 CXL-ANNS，但本身是 DDR DIMM-NMP |
| **ANNA / NDSearch / SmartANNS / Proxima / REIS** | FPGA/近盘/近存 ANNS；非 CXL 池化 |
| **MemANNS** 等 PIM | PIM 加速 billion ANNS |

做 lit review 时建议：**核心 CXL 论文精读 + 这些作 related work**，避免混成「所有近存 ANNS」。

---

## 3. CXL 硬件设计对比表

| 工作 | CXL Type | 设备侧算力 | 主机职责 | 数据放哪 | 验证方式 |
|------|----------|------------|----------|----------|----------|
| CXL-ANNS | Type-3 + Switch | DSA（距离） | 遍历、候选、调度、热图缓存 | 全图+向量在 CXL；热 hop 在 DRAM | FPGA + gem5 |
| Cosmos | Type-3/PNM | GPC + rank-PU | 分发查询、聚合 top-k | 分片在多 CXL 设备 | Ramulator 仿真 |
| Second-Tier | 商用 Type-3 expander | 无（纯内存） | 全部算法；改布局/流水 | 索引在 CXL/RDMA/NVM | 实机多介质 |
| SpANNS | **Type-2** | 控制器 + DIMM 内计算 | 稀疏 query 预处理 | 倒排/正排在专用 DIMM | 仿真+综合 |
| IKS | mem+cache | 多 NMA（ENNS kernel） | 向量库逻辑、聚合 | embedding 在 IKS DRAM（可共享） | 设计/评估叙述 |
| CMM-Ax | Type-3 PNM | streaming + 可编程 | 标准分配路径 / FAISS | HDM | FPGA 原型 |
| FaTRQ | （假设 far-mem） | 可选 NDP | 分层量化精炼 | 多级码本/残差 | 算法系统评估 |

### 设计选择题（读论文时可对照）

1. **算力放哪**：只加速距离（CXL-ANNS） vs 全流程卸载（Cosmos） vs 纯软件用 CXL（Second-Tier）。  
2. **Type-2 vs Type-3**：要设备主动算且复杂控制 → Type-2（SpANNS/IKS）；要大规模内存池 → Type-3（CXL-ANNS）。  
3. **一致性**：向量只读为主时，多用软件刷缓存 + 小范围 CXL.cache（IKS）。  
4. **分片策略**：按维切分并行距离（CXL-ANNS/Cosmos） vs 按 cluster 打散负载（Cosmos placement） vs 按 hop 热冷（CXL-ANNS cache）。

---

## 4. 时间线（简）

```
2023  CXL-ANNS (ATC) —— 领域标志性软硬协同
2024  CXL-ANNS TOCS 扩展；Second-Tier Memory (arXiv) —— 软件+商用 CXL
2025  Cosmos (CAL)；ANSMET 等近存对照；CMM-Ax 路线成熟叙述
2025–26  IKS/Quinn RAG-ENNS；SpANNS 稀疏；FaTRQ 远存量化；CMM-Ax TC
```

---

## 5. 对「CXL SSD 层级 + 向量全在 SSD + CXL hit/miss」的启示

现有 **CXL+ANNS 论文几乎都不走「真 CXL SSD」**：

- 主流是 **CXL DRAM 池装下全库**（或第二层内存装索引），用近存算力或软件流水补延迟。  
- **DiskANN 式「向量在块设备」** 仍是外存基线；CXL 论文多宣称要 **摆脱** 这条路径的延迟。  
- 与你设想最接近的文献支点：  
  - **Second-Tier**：CXL 细粒度随机读 vs SSD 4K；  
  - **CXL-ANNS 的 cache/prefetch**：本质是 **hit（本地/近端）vs miss（CXL 远）**；  
  - **FaTRQ**：远层精炼代价建模。  

若做 **SSD 全量向量 + CXL 热缓存**：这是 **DiskANN ∪ CXL-ANNS 缓存思想 ∪ Second-Tier 粒度分析** 的交叉空白，公开 CXL 专用论文覆盖仍少——适合作为研究切口，但需明确声明与 CXL-ANNS「全进 CXL」主张的差异。

---

## 6. 建议精读顺序

1. **CXL-ANNS ATC’23**（必读，建立问题与 Type-3+DSA 模板）  
2. **Second-Tier Memory arXiv’24**（必读，若关心 SSD↔CXL 分层与索引放大）  
3. **Cosmos**（看全卸载与 rank-PU 如何超越 CXL-ANNS）  
4. 按兴趣：**SpANNS**（稀疏） / **IKS·CMM-Ax**（可部署 PNM + FAISS） / **FaTRQ**（量化分层）

---

## 7. 参考文献速查

1. Jang et al., CXL-ANNS, USENIX ATC 2023.  
2. Jang et al., Bridging Software-Hardware…, ACM TOCS 2024.  
3. Cosmos, IEEE CAL 2025 / arXiv:2505.16096.  
4. Wei et al., Second-Tier Memory for Vector Search, arXiv:2405.03267.  
5. SpANNS, arXiv:2601.03229.  
6. Quinn et al., IKS / Compute-Enabled CXL for RAG, derrickquinn.github.io/cxl.pdf.  
7. Shin et al., CMM-Ax, IEEE TC 2026.  
8. FaTRQ, arXiv:2601.09985.  

DiskANN / HM-ANN / SPFresh / ANSMET 等见各文 related work。

---

## 8. 变更记录

| 日期 | 说明 |
|------|------|
| 2026-07-30 | 初版文献调研，面向 gpu01 上 CXL DRAM + NVMe 的研究语境 |
