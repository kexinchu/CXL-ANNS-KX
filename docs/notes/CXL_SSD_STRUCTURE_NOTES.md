# CXL SSD 存储结构学习笔记

> 整理日期：2026-07-30  
> 目的：弄清 CXL SSD「长什么样、数据怎么走」；并对照本机（gpu01）实际有什么。

---

## 0. 先分清：本机有什么 / 没有什么

| 东西 | 本机 gpu01 | 说明 |
|------|------------|------|
| **CXL Type-3 内存（DRAM）** | ✅ Montage `64:00.0` → `mem0` / `region0` ≈ **128 GiB**，NUMA **node1** | 易失内存扩展，**不是** SSD |
| **`/dev/vmem0`（CXL-SSD 用户态入口）** | ✅ **在线** | **size=1,954,743,148,544 B（≈1.778 TiB）**；**4 GiB cache**（used=0）；`ram_size=32 GiB`；`backend=software`（`vmem_sw` → `/dev/nvme3n1`） |
| **FPGA CXL SSD 端点** | ✅ 在线 | `15:00.0` Altera/Agilex；**BAR0=32 GiB**；驱动 `ntcx` / `nvmex` / `mem2nvme` |
| **普通 PCIe NVMe** | ✅ ~13 TB | 其中 `nvme3n1` @ `d8:00.0` 为当前 vmem backing |

学习时请把三者分开：

1. **Montage** = 纯 CXL DRAM expander（node1 System RAM）  
2. **`/dev/vmem0`** = CXL-SSD 逻辑地址空间（32 GiB 热层窗口语义 + TB 级容量）；**当前**由 `vmem_sw` 提供  
3. **`15:00.0` BAR** = 硬件 32 GiB 热层；与当前 software `/dev/vmem0` 并存但路径不同  

### 0.1 `/dev/vmem0` 最新在线快照（2026-07-30 UTC）

| 字段 | 值 |
|------|-----|
| `size` | **1,954,743,148,544** |
| `ssd_size` | 1,920,383,410,176（≈1.747 TiB） |
| `ram_size` | 34,359,738,368（**32 GiB**） |
| `cache_limit` | 4,294,967,296（**4 GiB**） |
| `cache_used` / `dirty_bytes` / `io_errors` | **0 / 0 / 0** |
| `backend` | `software` |
| `nvme_dev` / `target_bdf` | `/dev/nvme3n1` / `0000:d8:00.0` |
| stripe / prefetch | 2 MiB / off |

本机相关：`/root/CXLSSDEval`、`/root/CapCXL`、[`CXL_MEMORY_NOTES.md`](./CXL_MEMORY_NOTES.md) §3.3。

---

## 1. CXL SSD 是什么（一句话）

**CXL SSD = 带 NAND 的 Type-3 设备**：对外同时（或可配置地）提供

1. **CXL.mem**：把设备侧 DRAM（及经 DRAM 缓存后的闪存视图）映射进主机统一地址空间 → CPU **load/store**；  
2. **CXL.io / NVMe**：传统 **块 I/O**（DMA、SQ/CQ），扛大吞吐顺序读写。

所以它是 **「内存语义 + 块语义」双通路存储**，不是单纯更快的 NVMe，也不是纯 CXL DRAM。

典型产品叙事：Samsung CMM-H 等；学术系统：RomeFS、SkyByte、AgileStore/WIO、From-Block-to-Byte 等。

---

## 2. 内部存储结构（逻辑分层）

```
┌─────────────────────────────────────────────────────────────┐
│                        Host CPU                             │
│   load/store (cacheable)          submit NVMe / DMA         │
└───────────────┬─────────────────────────────┬───────────────┘
                │ CXL.mem                     │ CXL.io / NVMe
                ▼                             ▼
┌──────────────────────────────┐   ┌──────────────────────────┐
│     CXL Controller           │   │  NVMe / Flash Controller │
│  (flit ↔ mem req, HDM 映射)  │◄─►│  (FTL, wear, GC, DMA)    │
└───────────────┬──────────────┘   └────────────┬─────────────┘
                │                               │
                ▼                               ▼
        ┌───────────────┐              ┌────────────────┐
        │ Device DRAM   │  ◄─tiering─► │ NAND Flash     │
        │ (热层/缓存/PMR)│              │ (容量层, TB级) │
        │ 数 GB～数十 GB │              │                │
        └───────────────┘              └────────────────┘
```

### 2.1 三层介质（设备内部）

| 层 | 介质 | 典型角色 | 延迟量级（经验） |
|----|------|----------|------------------|
| **热层** | 设备 DRAM | 服务 cacheline 级 CXL.mem；写缓冲；元数据；队列 | ~亚 µs～数 µs（命中时） |
| **容量层** | NAND | 持久化主体；后台从 DRAM drain | 数十～数百 µs+ |
| （可选）**近存算力** | ARM / FPGA | 压缩、校验、谓词、actor 执行 | 视 offload |

**关键点**：主机以为在「访存」，设备固件/控制器往往在 **DRAM 页缓存** 上满足 64B 请求；miss 才触 NAND。这与「主机 OS 页缓存」是两套缓存。

### 2.2 双接口（主机视角）

| 通路 | 协议 | 粒度 | 适合 |
|------|------|------|------|
| **Memory path** | CXL.mem → HDM | 64B cacheline（可缓存、可一致） | 元数据、小随机、索引、日志头、doorbell |
| **Block path** | CXL.io ≈ NVMe | 4KB+ DMA | 大块顺序、批量向量/文件 |

RomeFS 等文件系统的核心主张：按访问模式 **协同使用两条路**，而不是只走一条。

### 2.3 和「NVMe PMR/CMB」的关系（进化路径）

| | 传统 NVMe PMR/CMB | CXL SSD / CXL 增强 PMR |
|--|-------------------|------------------------|
| 映射 | PCIe BAR，多为 **UC** | CXL.mem，多为 **可缓存 + 一致** |
| 延迟 | 常 8–15× 差于本机 DRAM | 明显改善（论文常报数量级） |
| 一致性 | 无，需手动同步 | 硬件 cache coherency |
| 容量 | 往往几百 MB～2GB | DRAM 热层更大；背后还有 NAND |

PMR ≈ 「块设备上抠一块可 mmap 的内存」；CXL SSD ≈ 「用 CXL.mem 把这块做成真正的内存语义，并与 NAND 自动分层」。

---

## 3. 一次访问怎么走（两种路径）

### 3.1 CXL.mem 读（小对象）

```
CPU load VA
  → 过主机 cache
  → Root Complex 把 miss 编成 CXL.mem flit
  → CXL SSD controller 解析地址
  → 查设备 DRAM cache（页或 cacheline/写日志）
       hit  → 返回 data flit
       miss → FTL 读 NAND → 填 DRAM → 返回
  → 填回 CPU cache
```

软件上可像访 DRAM：`mmap` DAX / 内核把 HDM 做成 System RAM / `devdax`。

### 3.2 NVMe 块读（大对象）

```
io_uring / pread
  → NVMe SQ（可放在主机内存或设备 CMB/PMR）
  → 设备 DMA 填 buffer
  → CQ 完成
```

与普通 SSD 相同；CXL SSD 仍保留这条路径，因为 **大顺序用块通路更划算**。

### 3.3 写与持久性

常见模型（AgileStore/SkyByte 等叙述）：

- **写应答**：先进入设备 DRAM（或写日志）即可返回（亚 µs 叙事）；  
- **持久化**：后台刷 NAND；  
- **耐久屏障**：Global Persistent Flush（GPF）等显式边界。

做系统时必须分清：**CXL load/store 完成 ≠ 已落 NAND**。

---

## 4. AgileStore / 本机文档里的「参考结构」（学习用）

`/root/CXLSSDEval` 论文中的多核结构（设计目标，非本机当前挂载设备）：

```
Host x86 Linux                    Device ARM Linux
  POSIX / io_uring                  WASM actors / I/O 处理
         \                         /
          \      共享 32GB PMR     /
           \   (CXL.mem 一致)     /
            ─────── 门铃/队列 ────
                      │
              FPGA: LZ4 / AES / CRC
                      │
                 8TB 级 NAND
```

要点：

- **PMR**：主机与设备核共享的一致内存（队列、元数据、actor 共享状态）；  
- **Actor 迁移**：控制状态进 PMR，共享状态原地换主人；  
- **异步耐久**：写先 DRAM，再 drain NAND。

驱动示例（`driver/README.md`）里的 BAR 切分思路（具体数值随板卡而变）：

- 大 **VMEM** 窗口：字节寻址孔径；  
- 小 BAR：配置、doorbell、初始化/DMA 相关区；  
- 桥：把 mem 语义转到后端 NVMe/闪存。

---

## 5. 软件栈怎么「看见」CXL SSD（本机）

```
应用
 ├─ mmap(/dev/vmem0)          ← 逻辑空间 ≈ 1.778 TiB（当前）
 ├─（可选）Montage：numactl --membind=1
 └─ 块语义：主机 NVMe / 历史上的 /dev/nvmex0n1

当前路径（backend=software）
  vmem_sw
   ├── host cache ≤ 4 GiB
   ├── ram_size 窗口 = 32 GiB（对齐 BAR 热层语义）
   └── /dev/nvme3n1 @ d8:00.0

硬件端点（并存）
  ntcx @ 0000:15:00.0  BAR0=32G + nvmex/mem2nvme
```

本机核对命令：

```bash
ls -l /dev/vmem0
cat /sys/class/vmem/vmem0/{backend,size,ssd_size,ram_size,cache_limit,cache_used,dirty_bytes,io_errors}
lspci -s 15:00.0 -vv | grep -E 'Region 0|Kernel driver'
```

配置文件线索（`CXLSSDEval/scripts/config.yaml`）：曾用 `/dev/nvmex0n1`、`/mnt/dax`。

---

## 6. 和本机 Montage「CXL Memory」对比（避免概念混淆）

| | Montage CXL Memory | `/dev/vmem0` CXL-SSD（当前） | FPGA 端点 `15:00.0` |
|--|--------------------|------------------------------|---------------------|
| 后端 | DRAM only | `vmem_sw`：4 GiB cache + `nvme3n1` | BAR0 32 GiB + 设备侧 NAND 路径 |
| 容量 | ~128 GiB | **逻辑 1,954,743,148,544 B** | BAR 窗口 32 GiB |
| 主机入口 | NUMA **node1** | **`/dev/vmem0`** | PCI BAR / `ntcx` |
| PCI | `64:00.0` | （逻辑设备；backing `d8:00.0`） | `15:00.0` `1172:0000` |

ANNS「向量全在 SSD + 热缓存」：应用层入口优先 **`/dev/vmem0`**（注意当前 software）；Montage node1 可作主机侧另一层缓存。

---

## 7. 建议学习路径（动手）

1. **读结构**：本文 §2–3 + `CXLSSDEval/paper/bg.tex`（PMR/CMB vs CXL）+ `design.tex` 架构段。  
2. **看图**：`CXLSSDEval/paper/img/Topology.pdf`、`Design.png`、`cxl ssd.drawio.pdf`。  
3. **对照本机**：`/dev/vmem0`、`lspci -s 15:00.0`、`backend`；并与 `cxl list`（Montage）对照。  
4. **读一篇系统纸（选一）**：RomeFS / SkyByte / From Block to Byte。  
5. **编程模型**：`CXLSSDEval/paper/other/model.md`（doorbell + UMWAIT / io_uring-like）。

---

## 8. 速记卡片

- CXL SSD = **热层（32 GiB 窗口语义）+ 容量层（TB）**。  
- **本机入口**：`/dev/vmem0` **在线**，size=**1,954,743,148,544**，**4 GiB cache**，used/dirty/io_errors=**0**。  
- **当前路径**：`backend=software`（`vmem_sw` → `nvme3n1`）；硬件 BAR 仍在 `15:00.0`/`ntcx`。  
- 勿与 Montage `mem0`（node1）混淆。
