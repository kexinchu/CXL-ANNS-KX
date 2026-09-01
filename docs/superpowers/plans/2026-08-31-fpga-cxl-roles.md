# FPGA CXL-SSD + CXL-DRAM Roles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (or subagent-driven-development) task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把运行时和测量栈改成：FPGA 两块 NVMe = CXL-SSD，FPGA BAR0 32 GiB = CXL-DRAM，host DRAM 只当 host DRAM；并用脚本做身份检查 / 门控探针 / 切栈，按新口径重测 Oracle。

**Architecture:** 三层地址空间在代码和日志里必须分开。CXL-SSD 仍是 `nvmex` 下的 d8+d9。CXL-DRAM 只允许来自 `mem2nvme`+`vmem.ko` 暴露的 BAR 窗口（`/dev/vmem0` 在切栈之后）。Host 固定 2 GiB，T=1 与 T=4 相同。HPS 不是 `0..3` 则数据面停在探针，不装 `vmem.ko`、不报 CXL-DRAM 数。

**Tech Stack:** Linux 6.18.0-rc5（按 `uname -r` 编模块）、`mem2nvme` / `vmem.ko`、`search_beam`、bash bring-up 脚本。

**Spec:** `docs/superpowers/specs/2026-08-31-fpga-cxl-roles-design.md`

**Commit policy:** 只在用户明确要求时 commit。下面各 Task 的 commit 步可跳过。

---

## 0. 结论（先读完）

| 现在 | 改完 |
|------|------|
| `/dev/vmem0` = `vmem_sw`（host 28+4 GiB cache + 双盘条带） | `/dev/vmem0` = `vmem.ko`（BAR0 32 GiB = CXL-DRAM） |
| 打分窗口 = `mbind` node 1 | 打分 = 读 BAR；host 2 GiB 不进 `from_cxl_dram` |
| Oracle 85.8 / 136 | **作废**；新 Oracle 必须打在 BAR 上 |
| `pack_nbr_bundle` 占着 vmem0 | **Task 0 未完成前禁止切栈** |

**不要对的数：** 85.8、136、66.5、50.25（vmem_sw+node1）。  
**不要做的事：** mmap `resource0`；HPS=`0xff` 仍 `insmod vmem.ko`；杀 pack；盘 I/O 中 reload `mem2nvme`。

---

## 文件地图

| 文件 | 职责 |
|------|------|
| `mem2nvme/tools/bringup_fpga_cxl.sh` | **主脚本**：身份、忙锁、编译、HPS 探针、可选切栈、写 env |
| `mem2nvme/tools/hps_probe.sh` | 已有；只 `cat hps_status`，不 mmap BAR |
| `mem2nvme/host/mem2nvme.c` | BAR provider；`allow_hps_mmio`；禁止默认 memremap 32 GiB |
| `mem2nvme/host/vmem.c` | CXL-DRAM 字符设备；`VMEM_IOC_TOUCH_BAR` 内核摸 64 B |
| `CXL-ANNS-KX/serving/search_beam.cpp` | `--cxl-dram` / `--host-bytes` / `--require-cxl-dram` / affinity |
| `CXL-ANNS-KX/serving/tests/test_cxl_roles_cli.sh` | CLI 拒 node1 当 CXL-DRAM |
| `CXL-ANNS-KX/docs/notes/CXL-ANNS-Prefetcher.md` | 只追加新节；不改旧锁定行的数字 |

现场身份（2026-08-31 晚，6.18.0-rc5）：

| 项 | 值 |
|----|-----|
| FPGA | `0000:15:00.0` `1172:0000` BAR0 32 GiB，driver `ntcx`（=已装的 `mem2nvme`） |
| CD8P | d8=`/dev/nvme1n1` SN `7EU0A01P0XK1`；d9=`/dev/nvme2n1` SN `2F50A1360XK3` |
| 现 `/dev/vmem0` | `vmem_sw` 双盘，`pack_nbr_bundle` PID 76497 占着 |
| 树上 `vmem.ko` | vermagic **7.0.0-30-generic**，不能装进 6.18 |

---

### Task 0: 忙锁 — pack 不结束就不往下

**Files:**
- Test: `mem2nvme/tests/test_bringup_busy.sh`
- Create: `mem2nvme/tools/bringup_fpga_cxl.sh`（先只做 busy + identity）

- [ ] **Step 1: 写忙锁测试**

```bash
#!/usr/bin/env bash
set -euo pipefail
# 无 /dev/vmem0 → busy 检查应跳过（exit 0 的 identity 前半）
# 有 holder → 脚本 --probe-only 必须 exit 3
```

用假 `fuser` 或跳过：若 `fuser /dev/vmem0` 非空，期望脚本打印 `BUSY` 并以码 3 退出。

- [ ] **Step 2: 实现 busy + FPGA/SSD 身份（脚本骨架）**

`mem2nvme/tools/bringup_fpga_cxl.sh` 必须包含（完整脚本见 Task 1；本步先落地函数）：

```bash
FPGA_BDF="${FPGA_BDF:-0000:15:00.0}"
FPGA_VENDOR=0x1172
FPGA_DEVICE=0x0000
BAR0_BYTES=$((32 * 1024 * 1024 * 1024))
SSD0_BDF=0000:d8:00.0
SSD1_BDF=0000:d9:00.0
SSD0_SN=7EU0A01P0XK1
SSD1_SN=2F50A1360XK3
SSD_BYTES=1920383410176

die_busy() {
  if [[ -e /dev/vmem0 ]] && fuser /dev/vmem0 >/dev/null 2>&1; then
    echo "BUSY: /dev/vmem0 is open; wait for pack_nbr_bundle (do not rmmod)" >&2
    fuser -v /dev/vmem0 >&2 || true
    exit 3
  fi
}

resolve_ns() {
  local bdf="$1" expect="$2"
  local d pci bytes
  for d in /sys/block/nvme*n*; do
    [[ -e "$d/size" ]] || continue
    pci=$(basename "$(readlink -f "$d/device/device" 2>/dev/null || true)")
    [[ "$pci" == "$bdf" ]] || continue
    bytes=$(($(cat "$d/size") * 512))
    [[ "$bytes" == "$expect" ]] || continue
    echo "/dev/$(basename "$d")"
    return 0
  done
  return 1
}
```

- [ ] **Step 3: 在真机跑忙锁（现在应 exit 3）**

```bash
bash /root/chukexin/mem2nvme/tools/bringup_fpga_cxl.sh --probe-only
```

Expected: `BUSY` + exit 3。**不要** `kill` pack。本 Task 在 pack 自行退出前算完成（脚本行为正确）。后续 Task 2+ 必须等 `fuser /dev/vmem0` 为空再开。

---

### Task 1: 主脚本 — identity / build / probe / 写 env（默认不切栈）

**Files:**
- Create: `mem2nvme/tools/bringup_fpga_cxl.sh`
- Create: `mem2nvme/tests/test_bringup_fpga_cxl_static.sh`
- Create: `CXL-ANNS-KX/tools/env_fpga_cxl.sh`（由主脚本生成，不要手写假路径）

- [ ] **Step 1: 静态测试**

`mem2nvme/tests/test_bringup_fpga_cxl_static.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail
s=/root/chukexin/mem2nvme/tools/bringup_fpga_cxl.sh
grep -q 'mmap.*resource0' "$s" && { echo "FAIL: script must not mmap resource0"; exit 1; }
grep -q 'allow_hps_mmio' "$s"
grep -q 'VMEM_ONLY=1' "$s"
grep -q -- '--switch' "$s"
grep -q 'exit 3' "$s"
echo "static bringup: PASS"
```

Run: `bash mem2nvme/tests/test_bringup_fpga_cxl_static.sh`  
Expected: PASS

- [ ] **Step 2: 写完整脚本**

脚本用法：

```text
bringup_fpga_cxl.sh --probe-only     # 身份 + 忙锁 + 可选 hps_status（默认）
bringup_fpga_cxl.sh --build          # 按 uname -r 编 mem2nvme.ko + vmem.ko
bringup_fpga_cxl.sh --switch         # 仅当 HPS=0..3 且 vmem0 空闲：卸 vmem_sw、装 vmem.ko
```

完整脚本（实现时按此落地，不要加 `mmap` `/sys/bus/pci/devices/*/resource0`）：

```bash
#!/usr/bin/env bash
# Bring up FPGA roles: d8+d9 = CXL-SSD, BAR0 32GiB = CXL-DRAM.
# Default is probe-only. --switch requires live HPS (0..3).
set -euo pipefail

ROOT="${MEM2NVME_ROOT:-/root/chukexin/mem2nvme}"
KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"
FPGA_BDF="${FPGA_BDF:-0000:15:00.0}"
MODE="probe"
[[ "${1:-}" == --build ]] && MODE=build
[[ "${1:-}" == --switch ]] && MODE=switch
[[ "${1:-}" == --probe-only || -z "${1:-}" ]] && MODE=probe

die() { echo "FAIL: $*" >&2; exit 1; }
sys() { cat "/sys/bus/pci/devices/${FPGA_BDF}/$1"; }

if [[ -e /dev/vmem0 ]] && fuser /dev/vmem0 >/dev/null 2>&1; then
  echo "BUSY: /dev/vmem0 is open; do not rmmod" >&2
  fuser -v /dev/vmem0 >&2 || true
  exit 3
fi

[[ "$(sys vendor)" == "0x1172" ]] || die "FPGA vendor $(sys vendor)"
[[ "$(sys device)" == "0x0000" ]] || die "FPGA device $(sys device)"
bar0=$(awk 'NR==1{print $1,$2}' "/sys/bus/pci/devices/${FPGA_BDF}/resource")
# 32 GiB: 0x000022f000000000 .. 0x000022f7ffffffff
echo "FPGA ${FPGA_BDF} BAR0 ${bar0} enable=$(sys enable) driver=$(basename "$(readlink -f /sys/bus/pci/devices/${FPGA_BDF}/driver 2>/dev/null || echo none)")"

resolve_ns() {
  local bdf="$1"
  local d pci
  for d in /sys/block/nvme*n*; do
    pci=$(basename "$(readlink -f "$d/device/device" 2>/dev/null || true)")
    [[ "$pci" == "$bdf" ]] || continue
    echo "/dev/$(basename "$d")"
    return 0
  done
  return 1
}
NS0=$(resolve_ns 0000:d8:00.0) || die "no ns for d8"
NS1=$(resolve_ns 0000:d9:00.0) || die "no ns for d9"
echo "CXL-SSD $NS0 (d8) $NS1 (d9)"

if [[ "$MODE" == build || "$MODE" == switch ]]; then
  make -C "$KDIR" M="$ROOT/host" VMEM_ONLY=1 modules
  test "$(modinfo -F vermagic "$ROOT/host/mem2nvme.ko" | awk '{print $1}')" = "$(uname -r)"
  test "$(modinfo -F vermagic "$ROOT/host/vmem.ko" | awk '{print $1}')" = "$(uname -r)"
  echo "built mem2nvme.ko vmem.ko for $(uname -r)"
fi

if [[ "$MODE" == probe || "$MODE" == switch ]]; then
  if [[ -e /sys/module/mem2nvme/parameters/allow_hps_mmio ]]; then
    echo -n "hps: "
    bash "$ROOT/tools/hps_probe.sh" "$FPGA_BDF" || true
  else
    echo "hps: loaded mem2nvme has no allow_hps_mmio (old .ko); --build then reload only when vmem_sw is idle"
  fi
fi

ENV=/root/chukexin/CXL-ANNS-KX/tools/env_fpga_cxl.sh
{
  echo "export CXAN_CXL_SSD0=$NS0"
  echo "export CXAN_CXL_SSD1=$NS1"
  echo "export CXAN_FPGA_BDF=$FPGA_BDF"
  echo "export CXAN_HOST_BYTES=$((2 * 1024 * 1024 * 1024))"
  echo "export CXAN_CXL_DRAM_DEV=/dev/vmem0"
  echo "export CXAN_REQUIRE_CXL_DRAM=1"
} > "$ENV"
echo "wrote $ENV"

if [[ "$MODE" != switch ]]; then
  echo "probe-only done. --switch only after hps_status is 0..3 and vmem0 idle."
  exit 0
fi

status_path="/sys/bus/pci/devices/${FPGA_BDF}/hps_status"
[[ -e "$status_path" ]] || die "no hps_status; load gated mem2nvme first"
st=$(cat "$status_path")
echo "hps_status=$st"
[[ "$st" =~ raw=0x[0-3]$ || "$st" == "raw=0x0" || "$st" == "raw=0x1" || "$st" == "raw=0x2" || "$st" == "raw=0x3" ]] \
  || die "HPS not live ($st); refusing vmem.ko"

# I/O in flight on CD8P? refuse reload of mem2nvme
if lsmod | grep -q '^vmem_sw'; then
  echo "rmmod vmem_sw (vmem0 idle already checked)"
  rmmod vmem_sw
fi
if lsmod | grep -q '^vmem '; then
  rmmod vmem
fi
insmod "$ROOT/host/vmem.ko" mode=dax
[[ -e /dev/vmem0 ]] || die "vmem.ko did not create /dev/vmem0"
echo "SWITCHED: /dev/vmem0 is vmem.ko BAR CXL-DRAM"
```

- [ ] **Step 3: 静态测试再跑一遍**

Expected: PASS。真机 `--probe-only` 在 pack 仍在时 exit 3。

---

### Task 2: 为运行中的 kernel 编 `mem2nvme.ko` + `vmem.ko`

**Files:**
- Generate: `mem2nvme/host/mem2nvme.ko`、`mem2nvme/host/vmem.ko`（vermagic = `uname -r`）
- 不要覆盖正在用的已加载模块的行为；只编，**不 insmod**

- [ ] **Step 1: 确认 pack 已结束**

```bash
fuser /dev/vmem0 && echo STILL_BUSY && exit 1 || echo idle
```

Expected: `idle`。否则停。

- [ ] **Step 2: 编译**

```bash
make -C /lib/modules/$(uname -r)/build \
  M=/root/chukexin/mem2nvme/host VMEM_ONLY=1 modules
modinfo -F vermagic /root/chukexin/mem2nvme/host/vmem.ko
modinfo -F depends /root/chukexin/mem2nvme/host/vmem.ko
```

Expected: vermagic 第一段 = `uname -r`；`depends: mem2nvme`。  
**不要** `insmod vmem.ko`。

- [ ] **Step 3: 确认新 `mem2nvme.ko` 带 `allow_hps_mmio`**

```bash
modinfo /root/chukexin/mem2nvme/host/mem2nvme.ko | grep allow_hps_mmio
```

Expected: 有 parm。当前 **已加载** 的那份没有这个参数，reload 另步。

---

### Task 3: 门控 HPS 探针（一次内核 readl，不 mmap BAR）

**Files:**
- Modify: 仅当需要 reload 已加载的旧 `mem2nvme` 时用新 `.ko`
- Run: `mem2nvme/tools/hps_probe.sh`

- [ ] **Step 1: 确认 vmem_sw 没有进行中的盘 I/O**

```bash
# backing read_ios 两次采样，差为 0 才允许 reload ntcx
s0=$(cat /sys/class/vmem/vmem0/read_ios)
sleep 2
s1=$(cat /sys/class/vmem/vmem0/read_ios)
echo "read_ios $s0 -> $s1"
```

Expected: 相等或 pack 已退出且无 search。否则 **不要** `rmmod mem2nvme`（probe 写 `REG_CTRL`）。

- [ ] **Step 2: 用新模块替换 provider（allow_hps_mmio=0）**

```bash
# vmem.ko 此时必须未加载
lsmod | grep -w vmem && exit 1
rmmod mem2nvme
insmod /root/chukexin/mem2nvme/host/mem2nvme.ko \
  target_bdf=0000:15:00.0 allow_hps_mmio=0
test "$(cat /sys/bus/pci/devices/0000:15:00.0/driver/module/name)" = mem2nvme \
  -o -e /sys/bus/pci/drivers/ntcx/0000:15:00.0
bash /root/chukexin/mem2nvme/tools/hps_probe.sh
```

Expected: `allow_hps_mmio=N`，`hps_status=gated`。没有 `resource0` mmap。

- [ ] **Step 3: 开闸只读 status（可挂则立即关闸并停）**

```bash
echo 1 > /sys/module/mem2nvme/parameters/allow_hps_mmio
timeout 5 cat /sys/bus/pci/devices/0000:15:00.0/hps_status
echo 0 > /sys/module/mem2nvme/parameters/allow_hps_mmio
```

Expected:
- `raw=0x0` .. `raw=0x3` → Task 4 可以做
- `gated` / `raw=0xffffffff` / `raw=0xff` / timeout → **HARD STOP**。写进笔记。不做 Task 4–6 的真机切栈。Task 5 的 CLI 改动仍可做（代码先拒假 CXL-DRAM）。

**禁止：** `echo 1 > hps_nop`、`HPS_PAGE_FETCH`、`insmod vmem.ko`、mmap BAR。

---

### Task 4: 内核摸 BAR 数据窗（64 B），通过才切 `vmem.ko`

**Files:**
- Modify: `mem2nvme/host/vmem.c`（加 ioctl）
- Modify: `mem2nvme/host/shell.h`（`VMEM_IOC_TOUCH_BAR`）
- Test: `mem2nvme/tests/test_vmem_touch_bar_static.sh`

HPS 寄存器活了 ≠ 32 GiB 数据窗能 CPU load。`vmem.ko` 的 mmap 走 PFN，和挂死的 `resource0` mmap 是同一类访问。先加 **内核** 拷 64 字节，timeout 包一层。

- [ ] **Step 1: 静态测试**

```bash
#!/usr/bin/env bash
set -euo pipefail
grep -q VMEM_IOC_TOUCH_BAR /root/chukexin/mem2nvme/host/shell.h
grep -q VMEM_IOC_TOUCH_BAR /root/chukexin/mem2nvme/host/vmem.c
grep -q 'copy_to_user' /root/chukexin/mem2nvme/host/vmem.c
echo "touch-bar static: PASS"
```

- [ ] **Step 2: ioctl 语义**

`shell.h` 增加：

```c
struct vmem_touch_bar {
	uint64_t offset; /* must be 0 for first bring-up */
	uint32_t len;    /* 1..64 */
	uint8_t  data[64];
};
#define VMEM_IOC_TOUCH_BAR _IOWR(VMEM_IOC_MAGIC, 20, struct vmem_touch_bar)
```

`vmem.c`：仅当 `allow_hps_mmio` 已在装 `vmem.ko` **之前** 用 Task 3 证明 HPS=0..3；ioctl 用已有 `bar_map` 或单页 `memremap(bar_phys+off, PAGE_SIZE)`，`memcpy_fromio` 最多 64 B，然后立刻 `memunmap`。`offset` 第一轮必须是 0。全 `0xff` 返回 `-ENXIO`。

- [ ] **Step 3: 切栈（仅 HPS 活且 touch 能编进将要装的 `.ko`）**

```bash
bash /root/chukexin/mem2nvme/tools/bringup_fpga_cxl.sh --switch
# 然后用最小 C 调 TOUCH_BAR offset=0 len=64
```

Expected: ioctl 返回 0 且不全是 `0xff`。否则 `rmmod vmem`，**恢复 vmem_sw 不要自动做**（双盘镜像可能已乱；只报告停）。

- [ ] **Step 4: 禁止 search 在 touch 失败时 mmap `/dev/vmem0`**

`search_beam` 打开 CXL-DRAM 后先发 `TOUCH_BAR`，失败 `exit 2`。

---

### Task 5: `search_beam` 三层角色（代码先改，真机数后补）

**Files:**
- Modify: `CXL-ANNS-KX/serving/search_beam.cpp`
- Create: `CXL-ANNS-KX/serving/tests/test_cxl_roles_cli.sh`

- [ ] **Step 1: 失败 CLI 测试**

```bash
#!/usr/bin/env bash
set -euo pipefail
bin=/root/chukexin/CXL-ANNS-KX/serving/search_beam
# 最小参数集，要求 --require-cxl-dram + --dram-backend numa 立刻失败
set +e
"$bin" --require-cxl-dram --dram-backend numa --host-bytes $((2*1024*1024*1024)) \
  --entry /dev/null --queries /dev/null
rc=$?
set -e
test "$rc" -eq 2
echo "PASS refuse numa as CXL-DRAM"
```

先跑应 FAIL（flag 还不存在）。

- [ ] **Step 2: 实现角色 CLI**

在 `search_beam.cpp` 增加并强制：

```text
--cxl-dram-dev DEV     默认 $CXAN_CXL_DRAM_DEV（切栈后 /dev/vmem0）
--host-bytes N         默认 2147483648；T=1 与 T=4 必须相同
--require-cxl-dram     若实际 mmap 的是 anon+mbind / 非 vmem BAR → exit 2
--cpu-affinity         T 个线程绑 T 个核（从 /proc/cpuinfo 顺序取，跳过 node 混绑可选）
```

删除或改写：

```c
if (dram_bytes > (1ull << 30)) {
  fprintf(stderr, "CXL-DRAM window capped at 1GiB (got %zu)\n", dram_bytes);
  return 2;
}
```

改成：**host-bytes** 上限 2 GiB；CXL-DRAM 设备大小可以是 32 GiB。  
`map_dram_numa` 的 printf 必须是 `mapped HOST DRAM`，禁止出现 `CXL-DRAM`。  
`from_win` 日志改名或并列 `from_cxl_dram`；numa 路径该计数必须为 0。

`--per-thread-window` 在 `--require-cxl-dram` 或 Oracle 下 **默认关**。T=4 共享同一 2 GiB host + 同一 BAR。

- [ ] **Step 3: affinity**

```c
cpu_set_t set;
CPU_ZERO(&set);
CPU_SET(cpu_id, &set);
if (sched_setaffinity(0, sizeof(set), &set) != 0) perror("affinity");
```

T=4 用 work-steal（已有 `next_q.fetch_add`），**不要**走 `freeze_fills && per_thread_window` 的 `qi += nthreads` 分片。

- [ ] **Step 4: 再跑 CLI 测试**

Expected: PASS。

---

### Task 6: FPGA CXL-DRAM Oracle（T=1 / T=4，host 都是 2 GiB）

**Files:**
- Create: `CXL-ANNS-KX/tools/run_oracle_fpga_cxl.sh`
- Log: `CXL-ANNS-KX/results/paper_figs/oracle_cxl_dram_T1_nq20.log`
- Log: `CXL-ANNS-KX/results/paper_figs/oracle_cxl_dram_T4_nq20.log`

**前置：** Task 4 touch 成功，`/dev/vmem0` 是 `vmem.ko`。

- [ ] **Step 1: stage 语料到 BAR（计时外）**

从 **host 文件** 把 pagebin 向量 + `pagebin_graph.bin` 拷进 BAR（ioctl 或已证明可 mmap 的 `/dev/vmem0`）。不要从双盘 420 GiB 读（条带可能已乱）。

Host 文件（已有）：

```text
/mnt/disk0/chukexin_motivation/serving_t2i_10m/layout_t2i_10m.bin
/mnt/disk0/chukexin_motivation/serving_t2i_10m/pagebin_graph.bin
```

- [ ] **Step 2: 同一二进制两条**

协议：oneshot-fp，L=400，k=10，seed=42，nq=20，`--require-cxl-dram`，`--host-bytes 2147483648`，`--shared-window`，`--cpu-affinity`，`freeze_fills`。

```bash
source /root/chukexin/CXL-ANNS-KX/tools/env_fpga_cxl.sh
# T=1
# T=4 --threads 4
```

验收：

| 必须 | 值 |
|------|----|
| `from_cxl_dram` | 100 |
| bounce | 0 |
| `nvme_read_B` | 0 |
| host-bytes | 两边都是 2147483648 |
| recall@10 | ≥ 0.92 |
| T=4 vs T=1 | 只多 thread/core，不许多窗 |

不要和 85.8 / 136 比。新数写入笔记新节，**不覆盖**旧锁定表。

- [ ] **Step 3: T=4 若 QPS < 2× T=1**

记原因（CXL-DRAM 带宽 / 缓存），不要改 host-bytes 或改回 node1。

---

### Task 7: Hide 数据面改走 HPS→BAR（Oracle 通了再做）

**Files:**
- Modify: `CXL-ANNS-KX/serving/hide_fill.hpp`（fill 目标 = BAR slot，不是 node1 窗）
- Modify: `mem2nvme/host/vmem.c`（miss → `HPS_PAGE_FETCH`）

- [ ] **Step 1:** hide promote 只安装进 CXL-DRAM（`/dev/vmem0` BAR），host 2 GiB 不收向量页。
- [ ] **Step 2:** `from_cxl_dram=100` 才算 hide 打分合同。
- [ ] **Step 3:** 真冷定义改为：BAR 冷 + 设备 cache 冷，不再用 `vmem_sw cache_used=0` 当唯一口径。
- [ ] **Step 4:** 50.25 **不自动替换**。替换门槛：nq=20 与 nq=100 同 iso-recall、`from_cxl_dram=100`。

双盘 420/460 布局若已条带打散：hide 需要 **另开 restage 计划**，不要塞进本文件当「顺手做」。

---

### Task 8: 文档与拒假口径

**Files:**
- Modify: `CXL-ANNS-KX/docs/notes/CXL-ANNS-Prefetcher.md`（只追加）
- Modify: `CXL-ANNS-KX/docs/notes/2026-08-31-组会汇报.md`（Oracle 节加「旧行 VOID」）
- Modify: `CXL-ANNS-KX/tools/restore_vmem_sw.sh` 文件头：写明「恢复的是软件代理，不是 CXL-DRAM」

追加固定段落：

```markdown
## FPGA 三角色（2026-08-31）
- CXL-SSD = d8+d9 CD8P
- CXL-DRAM = 15:00.0 BAR0 32 GiB via vmem.ko
- Host DRAM = 2 GiB searcher only
- 85.8 / 136 / node1 hide 不是本架构的 Oracle
```

---

## 执行顺序与停点

```
Task 0 忙锁（现在就该 exit 3）
  → Task 1 脚本落地
  → pack 退出
  → Task 2 编译 6.18 模块
  → Task 3 HPS readl
       ├─ 0..3 → Task 4 touch → Task 5/6 Oracle
       └─ 0xff / timeout → 停切栈；Task 5 CLI 仍做；不报 FPGA Oracle
  → Task 6 通过后才 Task 7 hide
  → Task 8 文档
```

---

## Self-review

| Spec 要求 | 对应 Task |
|-----------|-----------|
| 两块 FPGA NVMe = CXL-SSD | Task 1 identity + env `CXAN_CXL_SSD*` |
| BAR0 32 GiB = CXL-DRAM | Task 4–6 `vmem.ko` + `--cxl-dram-dev` |
| host 就是 host，2 GiB 两边相同 | Task 5 `--host-bytes` + Task 6 |
| 主脚本 | Task 0–1 `bringup_fpga_cxl.sh` |
| 不 mmap resource0 | Task 1 静态 grep + Task 3 |
| 不杀 pack | Task 0 exit 3 |
| 旧 Oracle 作废 | Task 6 / 8 |
| HPS 死则停 | Task 3 HARD STOP |

无 TBD。无「类似 Task N」。切栈命令写在 Task 1 / 4。
