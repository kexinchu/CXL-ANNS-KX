# gpu01 CXL Memory 环境记录

> 记录日期：2026-07-30（UTC）  
> 主机名：`gpu01`  
> 用途：本机 CXL 内存/加速器速查，方便后续实验与排障。  
> **以本次 boot 实测为准**；硬件热插拔、cmdline、内核模块变化后请重跑第 5 节并改本文。

---

## 1. 一句话结论

本机有两块与 CXL/存储相关的主设备：

1. **Montage CXL Type 3 内存（128 GiB）**：`region0` commit，**System RAM → NUMA node 1**（本地 DRAM 在 node 0）。  
2. **CXL SSD / `/dev/vmem0`（在线）**：逻辑容量 **1,954,743,148,544 B（≈1.778 TiB）**，**4 GiB cache**（`cache_used=0`）；`ram_size=32 GiB` 对应 BAR/热层窗口。当前 `backend=software`（`vmem_sw`，backing=`/dev/nvme3n1` @ `d8:00.0`）。硬件端点仍在：`0000:15:00.0` Altera/Agilex，BAR0=32 GiB，驱动 `ntcx`。

当前 boot **未见 Intel Type 2 加速器**（无 `3b:00.x` / `cxl_type2_accel` / `cache0`）。

关键启动参数：`efi=nosoftreserve`（CXL soft-reserved 直接进 buddy allocator，而不是留在 DAX/hmem）。

绑 Montage CXL **内存**请用 **`--membind=1`**。访问 CXL SSD 热层走 **`/dev/vmem0`**（mmap/DAX 语义），不要和 node1 System RAM 混淆。
---

## 2. 平台与内核

| 项 | 值 |
|----|-----|
| CPU | Intel(R) Xeon(R) **6787P**，1 socket，86 cores / 172 threads |
| 内核 | `6.18.0-rc5` |
| cmdline | `intel_iommu=on iommu=pt nohibernate efi=nosoftreserve` |
| ACPI 相关表 | `CEDT` / `HMAT` / `SRAT` / `SLIT` 均存在 |
| 用户态工具 | `cxl` / `ndctl` / `daxctl` / `numactl` / `fio` / `perf` |

已加载主要模块（当前 boot）：

```
# Montage Type3 / 内核 CXL 子系统
cxl_core, cxl_pci, cxl_acpi, cxl_port, cxl_mem, cxl_pmem,
cxl_pmu, dax_hmem

# CXL SSD（FPGA / nvmex 栈 + 当前用户态设备）
ntcx（绑定 15:00.0）, nvmex, mem2nvme, vmem_sw（当前提供 /dev/vmem0）
```

未加载：`cxl_type2_accel` / `type2_common` / `cxl_cache`（与 Type2 设备当前不在线一致）。
---

## 3. 设备清单

### 3.1 Type 3 内存扩展（主用，已就位）

| 项 | 值 |
|----|-----|
| PCI BDF | `0000:64:00.0` |
| 厂商/设备 | Montage Technology `1b00:c002` (rev 03) |
| 类型 | CXL Memory Device（prog-if 10，CXL 2.0+） |
| Physical Slot | 4043 |
| 链路 | **32 GT/s ×8**（LnkCap/LnkSta 一致） |
| CXL memdev | `mem0` |
| 用户态节点 | `/dev/cxl/mem0` |
| 容量 | **128 GiB** volatile RAM（无 pmem 分区） |
| Firmware | `20.00.0.0609.00` |
| Serial | `0x8a0af738c2820407` |
| Region | `region0`，mode=`ram`，decode_state=`commit` |
| SPA 起始 | `0x2080000000`（`/proc/iomem`：CXL Window 0 → `region0` → System RAM） |
| Region 大小 | `0x2000000000`（128 GiB） |
| Interleave | ways=1，granularity=256 |
| QoS | `ram_qos_class=1`；`cxl list` 显示 `qos_class_mismatch=true` |
| sysfs | `/sys/bus/cxl/devices/mem0` |
| PMU | `cxl_pmu_mem0.0`（`/sys/bus/event_source/devices/cxl_pmu_mem0.0`） |

拓扑路径（简化）：

```
root0 (ACPI.CXL)
  └─ port3 (host=pci0000:63)
       └─ endpoint7 (host=mem0)
            └─ decoder7.0 ── region0 (128 GiB RAM)
```

### 3.2 Type 2 加速器（当前 boot：不在线）

| 项 | 值 |
|----|-----|
| 状态 | **未枚举**：`lspci` 无 `3b:00.x` / `8086:0ddb`；无 `/dev/cxl/cache*` |
| 历史记录 | 曾见过 Intel Type2（RCH，`cxl_type2_accel`，CSR-only，`enable_memdev=0`）及约 4 GiB 相关窗口 |

若再次插上/驱动加载后，需重新确认 PCI BDF、是否出现新 NUMA node，以及 CXL 内存节点编号是否变化。

### 3.3 CXL SSD / `/dev/vmem0`（在线快照 2026-07-30 UTC）

#### 3.3.1 用户态设备（当前）

| 项 | 值 |
|----|-----|
| 设备节点 | **`/dev/vmem0`**（`493:0`，在线） |
| 驱动 / backend | **`vmem_sw`**，`backend=software` |
| **逻辑容量 `size`** | **1,954,743,148,544 B**（≈1820.5 GiB / **≈1.778 TiB**） |
| SSD 容量 `ssd_size` | 1,920,383,410,176 B（≈1.747 TiB） |
| RAM / BAR 窗口 `ram_size` | **32 GiB**（34,359,738,368 B） |
| **Cache** | **`cache_limit=4 GiB`**（4,294,967,296 B） |
| `cache_used` | **0** |
| `dirty_bytes` | **0** |
| `io_errors` | **0** |
| `faults` / `evictions` / `read_ios` / `write_ios` | 均为 0（尚未有业务流量） |
| `ram_allocated` | 0 |
| stripe | 2 MiB（2,097,152 B） |
| prefetch | `off 0` |
| backing NVMe | `/dev/nvme3n1` |
| `target_bdf` | `0000:d8:00.0`（Dell DC NVMe CD8P E3.S） |

模块参数（与上表一致）：

```
vmem_sw: ram_size_gib=32 cache_size_gib=4 stripe_size_mib=2
         nvme_dev=/dev/nvme3n1 target_bdf=0000:d8:00.0
         expected_ssd_size_bytes=1920383410176
```

心智模型（**当前 software 路径**）：

```
应用 mmap(/dev/vmem0)   size ≈ 1.778 TiB
        │
        ▼
   vmem_sw
   ├── 主机侧 cache ≤ 4 GiB（当前 used=0）
   ├── ram_size 窗口 = 32 GiB（对齐 CXL SSD BAR/热层语义）
   └── backing SSD = /dev/nvme3n1 @ d8:00.0（≈1.747 TiB）
```

快速健康检查：

```bash
cat /sys/class/vmem/vmem0/{size,cache_limit,cache_used,dirty_bytes,io_errors,backend}
# 期望：size=1954743148544  cache_limit=4294967296  cache_used=0  dirty=0  io_errors=0
```

#### 3.3.2 硬件端点（FPGA，仍在线）

| 项 | 值 |
|----|-----|
| 角色 | Agilex FPGA CXL SSD 端点；**BAR0 = 32 GiB** 热层窗口 |
| PCI BDF | `0000:15:00.0`（父桥 `pci0000:14` → `14:02.0`） |
| 厂商/设备 | Altera / Agilex `1172:0000` (rev 01)，class `ff00` |
| Physical Slot | 4063 |
| BAR0 | **`0x22f000000000`–`0x22f7ffffffff`，size=32 GiB** |
| 链路 | Cap 16 GT/s ×8；Sta **16 GT/s ×4（downgraded）** |
| 驱动 | `ntcx`；相关 `nvmex`、`mem2nvme` |
| NUMA（PCI） | `numa_node=0` |
| 姊妹卡 | `0000:16:00.0`（Slot 4057，BAR0 16 GiB **disabled**） |

历史上 `ntcx` 也曾把 BAR 以 DAX 模式直接暴露为 `/dev/vmem0`。  
**当前 `/dev/vmem0` 由 `vmem_sw` 占用**（`backend=software`）；硬件 BAR 仍在 `15:00.0`，但用户态逻辑盘是上表的 software 配置。若要切回硬件 BAR 路径，需处理模块叠加并以 `backend`/dmesg 为准。

**不要**把 `/dev/vmem0` 和 Montage `mem0`/`region0`（node1 System RAM）当成同一块设备。

相关工程：`/root/CXLSSDEval`、`/root/CapCXL`；结构说明见 [`CXL_SSD_STRUCTURE_NOTES.md`](./CXL_SSD_STRUCTURE_NOTES.md)。

### 3.4 普通 NVMe（主机侧块设备）

| 设备 | 型号（摘要） | 容量 | 父 host | 备注 |
|------|--------------|------|---------|------|
| `nvme0n1` | Crucial CT4000P3SSD8 | ~4.0 TB | `pci0000:3d` → `3e:00.0` | 普通 NVMe |
| `nvme1n1` | Lexar SSD ARES 4TB | ~4.1 TB | `pci0000:3d` → `3f:00.0` | 普通 NVMe |
| `nvme2n1` | Dell Ent NVMe PM1733a | ~3.8 TB | `pci0000:d5` → `d7:00.0` | 普通 NVMe |
| `nvme3n1` | Dell DC NVMe CD8P E3.S | ~1.9 TB | `pci0000:d5` → `d8:00.0` | **当前 `vmem_sw` backing**（`/dev/vmem0`） |

这些盘 class 为标准 NVMe `[0108]`。当前 **`/dev/vmem0` 逻辑盘** 建在 `nvme3n1` 上（software）；FPGA `15:00.0` 是硬件 CXL SSD 端点，勿与「随便一块 NVMe」混为一谈。

### 3.5 CXL Host / Window 概况

Root `root0` 下可见多个 CXL-capable host（`pci0000:14/3d/63/89/af/d5`）。  
`/proc/iomem` 中有 **CXL Window 0–11**；当前真正映射为 System RAM 的主要是：

| Window | 地址范围（摘要） | 状态 |
|--------|------------------|------|
| Window 0 | `0x2080000000`–`0x407fffffff` | **System RAM（Montage 128 GiB → node1）** |
| Window 1–11 | 已声明 | 未见对应大块 System RAM（本次无旧版 Window6 ~4 GiB） |

---

## 4. NUMA 与内存分层

### 4.1 节点角色（当前）

| Node | 角色 | 容量（约） | CPU | Memory Tier |
|------|------|------------|-----|-------------|
| **0** | 本地 DRAM | ~128 GiB（`128312 MB`） | 0–171 | `memory_tier4` |
| **1** | **CXL Type3 内存** | ~129 GiB（`128973 MB`） | 无 | `memory_tier108` |

> 注意：`cxl list` 里 `mem0.numa_node` 显示为 `0`（设备挂在靠近 socket0 的 host 上），但 **在线后的内存页落在 node 1**。绑 CXL 内存请用 **`--membind=1`**，不要被 memdev 的 `numa_node` 字段误导。

> 旧版文档曾写 node2=CXL、node1≈4 GiB（Type2 相关）。Type2 不在线后，NUMA 收缩为 0/1，**CXL 现为 node1**。

### 4.2 SLIT 距离矩阵

```
node     0    1
   0:   10   14
   1:   14   10
```

### 4.3 HMAT（sysfs，单位为内核导出值）

| Node | Access | read_lat | write_lat | read_bw | write_bw | 备注 |
|------|--------|----------|-----------|---------|----------|------|
| 0 | access0/1 | 91 | 91 | 262144 | 176128 | DRAM |
| 1 | access0/1 | 1510 | 390 | 98304 | 92160 | CXL Type3 |

解读建议：把 **node0 ≈ DRAM、node1 ≈ CXL DRAM 扩展** 作为默认心智模型。

### 4.4 Demotion

```
/sys/kernel/mm/numa/demotion_enabled = false
```

当前未开启自动 demotion（冷页下沉到 CXL）。

---

## 5. 常用查阅命令

### 5.1 设备与 region

```bash
cxl list
cxl list -vvvv
ls -l /sys/bus/cxl/devices/
ls -l /dev/cxl/
lspci -nn | grep -iE 'cxl|0502|0ddb'
lspci -vvv -s 64:00.0 | less
```

### 5.2 内存拓扑

```bash
numactl -H
free -h
grep -E 'CXL Window|System RAM|region0' /proc/iomem
cat /sys/devices/virtual/memory_tiering/memory_tier*/nodelist
cat /sys/kernel/mm/numa/demotion_enabled
```

### 5.3 HMAT / 延迟元数据

```bash
for n in 0 1; do
  echo "=== node$n ==="
  for a in access0 access1; do
    echo "$a"
    cat /sys/devices/system/node/node$n/$a/initiators/read_latency
    cat /sys/devices/system/node/node$n/$a/initiators/write_latency
    cat /sys/devices/system/node/node$n/$a/initiators/read_bandwidth
    cat /sys/devices/system/node/node$n/$a/initiators/write_bandwidth
  done
done
```

### 5.4 健康与固件

```bash
cxl list -vvvv   # 含 health / alert_config / partition_info / firmware
cat /sys/bus/cxl/devices/mem0/firmware_version
cat /sys/bus/cxl/devices/region0/{resource,size,mode,commit}
```

### 5.5 CXL SSD / `/dev/vmem0`

```bash
ls -l /dev/vmem0
# 容量 / cache / 健康
cat /sys/class/vmem/vmem0/{backend,size,ssd_size,ram_size,cache_limit,cache_used,dirty_bytes,io_errors,nvme_dev,target_bdf,stripe_size}
# 硬件端点
lspci -nn -s 15:00.0
lspci -vvv -s 15:00.0 | grep -E 'Region 0|LnkSta|Kernel driver'
ls -l /sys/bus/pci/devices/0000:15:00.0/driver   # → ntcx
lsmod | grep -iE 'vmem_sw|ntcx|nvmex|mem2nvme'
dmesg | grep -iE 'vmem_sw|ntcx|nvmex BAR' | tail
```

---

## 6. 使用 CXL 内存（应用侧）

CXL 已是普通 System RAM，**最直接方式是 NUMA bind 到 node 1**。

```bash
# CPU 留在 node0，内存只用 CXL（node1）
numactl --cpunodebind=0 --membind=1 ./your_app

# 仅绑定内存到 CXL
numactl --membind=1 ./your_app

# 对比：只用本地 DRAM
numactl --membind=0 ./your_app

# 优先 CXL，不足再回退（按策略需要）
numactl --preferred=1 ./your_app
```

C / 库侧也可用 `mbind` / `numa_alloc_onnode` / `numa_set_membind` 等。

**不要默认假设** CXL 与 DRAM 带宽/延迟相同；做实验时建议固定：

1. CPU 绑在 node0  
2. 分别 `membind=0` vs `membind=1`  
3. 用同一工作集与同一线程数对比  

---

## 7. 性能计数器（PMU）

### 7.1 设备侧 CXL PMU

```bash
perf list | grep cxl_pmu_mem0
```

常见事件类别（名称以 `perf list` 为准）：

- 协议侧：`m2s_req_memrd` / `m2s_rwd_memwr` / `s2m_drs_memdata` 等  
- DRAM 侧：`ddr_casrd` / `ddr_caswr` / `ddr_act` / `ddr_pre` 等  
- 时钟：`clock_ticks`

示例：

```bash
perf stat -e cxl_pmu_mem0.0/m2s_req_memrd/,cxl_pmu_mem0.0/m2s_rwd_memwr/,cxl_pmu_mem0.0/ddr_casrd/,cxl_pmu_mem0.0/ddr_caswr/ -- \
  numactl --membind=1 ./your_app
```

### 7.2 CPU Uncore（CXL 相关）

机器上还可见：

- `uncore_b2cxl_*`
- `uncore_cxlcm_*`
- `uncore_cxldp_*`

```bash
ls /sys/bus/event_source/devices/ | grep -iE 'cxl|b2cxl'
```

---

## 8. 已知注意点 / 坑

1. **`efi=nosoftreserve` 很关键**  
   没有它时，CXL soft-reserved 可能不进普通 `malloc`/buddy，而要以 DAX/devdax 等方式使用。改 cmdline 后需确认 node1 是否仍在。

2. **`mem0.numa_node=0` ≠ 内存在 node0**  
   真正可绑的 CXL RAM 是 **node 1**（当前 boot）。

3. **NUMA 编号会随设备在线情况变化**  
   Type2 / 额外 CXL 窗口出现时，CXL 大内存可能不再是 node1。以 `numactl -H` 为准，不要死记编号。

4. **`qos_class_mismatch=true`**  
   region 与 decoder QoS class 不一致；当前仍可当 RAM 用，做 QoS/带宽分级实验前需再核对。

5. **Type2 当前不在线**  
   旧记录中的 CSR-only / `/dev/cxl_tmatmul*` / `cache0` 路径本次不可用。

6. **CXL SSD / `/dev/vmem0` ≠ Montage `mem0` ≠ 裸 NVMe**  
   - 当前逻辑入口：`/dev/vmem0`（size≈1.778 TiB，4 GiB cache，`backend=software`）  
   - 硬件端点：`15:00.0` BAR0=32 GiB（`ntcx`）  
   - 纯内存扩展：`mem0`/`region0` → NUMA node1  
   - backing 盘：当前为 `nvme3n1`

7. **当前是 `vmem_sw` software 路径**  
   流量走主机 cache + `nvme3n1`，不是直接打 FPGA BAR。切硬件路径前看 `backend` 与模块。

8. **Demotion 默认关闭**  
   不会自动把冷页迁到 Montage CXL；分层策略需用户态显式 bind，或自行打开 demotion 并验证。

---

## 9. 建议上手 checklist

- [ ] `cxl list` 看到 `mem0` + `region0` commit  
- [ ] `numactl -H` 看到 node1 ≈ 128 GiB 且几乎空闲  
- [ ] `numactl --membind=1` 跑通 hello/微基准  
- [ ] 对比 `membind=0` vs `membind=1` 延迟与带宽  
- [ ] `perf` 采样 `cxl_pmu_mem0.0` 确认流量打到 Montage  
- [ ] `ls -l /dev/vmem0`；`size=1954743148544`；`cache_limit=4GiB`；`cache_used/dirty/io_errors=0`  
- [ ] `backend` 与实验意图一致（当前多为 `software`；硬件 BAR 另看 `15:00.0`/`ntcx`）  
- [ ] （可选）确认 Type2 是否重新出现；有则更新 NUMA 表  
- [ ] （可选）demotion / 多 region / CXL SSD hit-miss 分层实验  

---

## 10. 本机相关代码/资料路径（供延伸）

探测时机器上可见（未必都与当前在线配置一致）：

| 路径 | 备注 |
|------|------|
| `/root/cxl-u-boot/linux-cxl-type2` | 含 `drivers/cxl`、`cxl_type2_accel` 等 |
| `/root/CXLAgent` | CXL 相关工程 |
| `/root/SimCXL` | 仿真/相关 |
| `/root/CXLSSDEval` / `/root/CapCXL` | CXL SSD（AgileStore / FPGA）评估与 RTL；对应本机 `15:00.0` + `/dev/vmem0` |
| `/root/chukexin/CXL_SSD_STRUCTURE_NOTES.md` | CXL SSD 结构学习笔记（已对照本机） |
| `/root/CXL_Memory_Expansion_0.3.1.tar.gz` | 扩展相关包 |

内核文档（若使用上述 tree）：

- `Documentation/driver-api/cxl/`
- `Documentation/ABI/testing/sysfs-bus-cxl`
- `Documentation/admin-guide/perf/cxl.rst`

---

## 11. 变更记录

| 日期 | 说明 |
|------|------|
| 2026-07-30 | 初版：Montage 128 GiB → node2；Type2 CSR-only；node1≈4 GiB |
| 2026-07-30 | **对齐当前 boot**：CXL → **node1**；Type2 不在线；更新 HMAT/SLIT/模块/拓扑（endpoint7）；应用侧改为 `membind=1` |
| 2026-07-30 | **补上 CXL SSD**：`15:00.0` Altera/Agilex，BAR0=32 GiB → **`/dev/vmem0`**；纠正「本机无 CXL SSD」；注明 `vmem_sw` software backend 坑 |
| 2026-07-30 | **同步 `/dev/vmem0` 在线快照**：size=1,954,743,148,544；4 GiB cache；cache_used/dirty/io_errors=0；backend=software；backing=`nvme3n1`@`d8:00.0` |

若硬件、cmdline、region 或内核变更，请重新跑第 5 节命令并更新本文对应表格。
