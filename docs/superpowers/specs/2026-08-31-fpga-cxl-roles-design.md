# FPGA CXL roles — design

**Status:** Design for review — do not implement until the plan is approved  
**Date:** 2026-08-31  
**Repos:** `CXL-ANNS-KX` (search / claim) + `mem2nvme` (data plane)

## Goal

把测量栈改成和产品图一致的三层，禁止再用 host DRAM 冒充 CXL-DRAM。

| 角色 | 硬件 | 软件入口 | 禁止当成 |
|------|------|----------|----------|
| **CXL-SSD** | FPGA 侧两块 CD8P：`0000:d8:00.0` + `0000:d9:00.0` | `nvmex` 块设备；容量 backing | 本机 Lexar / PM1733 |
| **CXL-DRAM** | FPGA `1172:0000` @ `15:00.0` **BAR0 32 GiB** | `mem2nvme` + `vmem.ko` → `/dev/vmem0` 的 BAR 窗口 | NUMA node 1、`vmem_sw` 的 28+4 GiB host cache、`/dev/dax*`（现场没有） |
| **Host DRAM** | 插座内存 | 搜索器堆、beam、2 GiB 软件预算 | CXL-DRAM、`from_win` 来源 |

## Why the current stack is wrong

`vmem_sw` 已经打在两块真 CD8P 上，但 **窗口在 host DRAM**。`search_beam` 默认 `mbind` node 1，却把分数记成 `from_win`。`--oracle-window` 85.8 / 136 和 `--oracle-dram` 66.5 都是 host 内存上界，**不能**当「CXL-DRAM Oracle」。

`mem2nvme`（驱动名 `ntcx`）已经 bind 了 `15:00.0`，但 `vmem.ko` 没装；userspace `mmap resource0` 会 PCI timeout；HPS 历史上读 `0xff`。

## Data path (target)

```
CPU load/store ──► CXL-DRAM (BAR0 32 GiB via vmem.ko)
                      ▲
                      │ HPS_PAGE_FETCH（或经证明等价的 device DMA）
                      │
                 CXL-SSD (d8+d9 NAND)

Host DRAM 2 GiB：只放搜索状态（beam / cand / scratch）。图和向量不在这里。
```

T2I-10M 向量 9.6 GiB + 图 1.28 GiB **装得进** 32 GiB BAR。Oracle = 语料已在 BAR，计时冻结 NAND。Hide = 缺页时 HPS 从 CXL-SSD 填进 BAR，打分只读 BAR。

## Oracle protocol (replaces 85.8 / 136)

两边 **只有** thread 数不同：

- Host 本地内存 **都是 2 GiB**，共享一份，禁止 `4 × 512 MiB`。
- 图 + 向量在 CXL-DRAM（BAR）。
- T=4：4 worker，`sched_setaffinity` 一人一核；**work-steal**，禁止 `qi % T`。
- 计时 `nvme_read_B=0`，`from_cxl_dram=100`，bounce=0。
- 旧行 VOID：85.8、136、66.5、node1 窗口 hide 50.25（那是另一套数据面）。

## Hard gates (fail closed)

1. `fuser /dev/vmem0` 非空（现在的 `pack_nbr_bundle`）→ 脚本退出，不 `rmmod`。
2. `hps_status` 不是 `0..3` → **不准** `insmod vmem.ko`，不准 mmap BAR，不准报 CXL-DRAM 数。
3. 禁止 userspace `mmap` `/sys/bus/pci/devices/0000:15:00.0/resource0`。
4. `search_beam --require-cxl-dram` 在 backend=numa / node1 时 `exit 2`。
5. 不把 `vmem_sw` 的 `ram_size` 写成 CXL-DRAM。
6. 重载 `mem2nvme` 时若 `vmem_sw` 正在做盘 I/O → 禁止（probe 会写 FPGA `REG_CTRL`）。

## Non-goals

- 不刷 FPGA 比特流（HPS 死了就停，不在本计划里修固件）。
- 不把 Linux `cxl list` / `/dev/dax*` 当作成功条件（class `ff00`，走 `vmem.ko`）。
- 不在本计划里 restage 267 GiB hide bundle（Oracle 从 host 文件 stage 进 BAR；hide restage 另开）。
- 不恢复「node1 替身」作为 claim 路径。

## Approval

先批这份角色划分，再执行 `docs/superpowers/plans/2026-08-31-fpga-cxl-roles.md`。
