# Hide Without CXL-DRAM Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 **CXL-DRAM / HPS / `1172:0000` BAR 都不可用** 的前提下，用已锁定真机数把 hide→oracle 缺口拆开，只做仍可能缩小缺口的软件路径；不假装能单独靠占用率或双盘 restage 把 T=1 送到 85.8。

**Architecture:** 两条独立缺口，不要混对。T=1 **50.25 / 19.8 → 85.8 / 11.4** 是 hop 串行（+8.4 ms，其中 `crit_wait` 只占 1.24）。T=4 **79.02 → 136.23** 是同一 hide 缺口 **加上** 单盘 `cap_qps` 121–127 &lt; 136。CXL-DRAM 下来之后，T=1 只剩跨 query 流水；T=4 只剩「T=4-only QD」和（批准后）双盘抬峰。Device walk 停放。

**Tech Stack:** `serving/search_beam` P3 + `--nbr-bundle`，`vmem_sw` 软件路径（现网 kernel `7.0.0-30-generic`），DramWindow = `mbind` node 1（槽 DRAM 替身）。协议不变。占用分母仍 **1.560 GB/s**。

---

## 0. 结论（先读完）

| 对照 | hide | oracle | 比值 | 缺什么 | CXL-DRAM 没了之后 |
|------|-----:|-------:|-----:|--------|-------------------|
| T=1 nq=20 | **50.25 / 19.8** | **85.8 / 11.4** | **0.59×** | +8.4 ms 串行 hop | **到不了 85.8**，除非跨 query 流水吃掉大部分 7.2 ms |
| T=4 nq=20 512 MiB | **79.02 / 34.4** | **136.23 / 15.8** | **0.58×** | 同一 hide + 字节墙 | 双盘抬峰最多把 **cap** 抬过 136；hide 比仍约 0.6× |

**不要对的数：** `4×85.8=343`；T=8 oracle 106；degree 图 T=1 oracle 95.91；T=4 nq=100 shard oracle 187 / recall 0.895。

**不要当赢再跑：** `--score-page`、`--min-issue-use`、入度 bundle、look=8、`pipe_w=32`、共享 1 GiB×4、`--cont-batch` SIGSEGV 原实现、score thread、`vmem.ko` / BAR 数、占用刷到 100% 当 T=1 赢。

**字节账（已测，不是估）：**

```
有用 ≈ 12 MB/q
T=1 到 85.8：12 MB × 85.8 ≈ 1.02 GB/s  < 1.560     → 不是带宽墙
T=4 占用 100% of 1.56 ≈ 127 QPS                    → 仍 < 136
默认 8/2 nq=20 cap_qps = 121                       → 单盘路径字节顶不到四路 oracle
fio 双盘顺序 14.4 ≠ vmem 峰；双盘软件峰必须重测
```

上一轮计划 Task 1–4、6–7 已跑完。Task 3（`min-issue-use`）失败。Task 5 当时 BLOCKED（无源码）；**源码现已在** `/root/chukexin/mem2nvme` `dual-ssd-vmem-sw`，但 **未 restage、未 insmod**。本计划不重复那些已失败的刀。

---

## 1. 缺口拆解（结合已测）

### 1.1 T=1：19.8 → 11.4 = +8.4 ms

锁定 nq=20：`crit_wait` **1.24 ms/q**（15%），其余 **~7.2 ms**（85%）是 `stall_if_full` / install memcpy / **打完 hop *i* 才发 hop *i+1***。`overlap=0.07`。窗口 hit 13.75%（nq=20）/ 3.31%（nq=100）。

图在 **host 文件** 里。向量在 NAND。hop *i+1* 的邻居 ID 要等 hop *i* 的分数。这是冻结合同里的指针追逐上限。Device walk 是对症；HPS/`1172:0000` 这轮不在。

软件还能动的，只有：

1. **跨 query 流水**（不是跨 hop）：query *q* 后半段打分时，用 host 图预取 *q+1* 的 entry + 1-hop bundle。不违反「同一 walk 藏不住下一跳」。`--cont-batch` 共享窗 SIGSEGV **不要复用**；T=1 仍是一条 query 的窗口，只是 IO 提前。
2. **少搬 install 税**：不能再走「跳过低产 bundle 页改随机 pagebin」（QPS 26/14/15）。只能少 `install_full_page` 次数（已在窗的页、跨 query 13%）。
3. **承认残差**：若流水只吃掉 1–3 ms，论文就写 0.59× 是 hop 串行，不是「search 看见 flash」。

### 1.2 T=4：79.02 → 136.23

同一 0.58× hide 缺口。另：`cap_qps` 默认 **121 &lt; 136**。ahead=3 到过 110 QPS（cap 139）但仍远低于 136，且 **VOID 当 T=1 默认**。

没有 CXL-DRAM 时 T=4 顺序：

1. 恢复单盘数据面，复测 claim 口径（块设备名已漂到 `nvme4n1`/`d9`）。
2. **仅 T=4** 打开 ahead=3 或 ebatch=16（CLI 默认保持 8/2）。
3. 若 `cap_qps` 仍 &lt; 136：**空盘** restage 双盘（`refuse_dual_vmem_sw.sh`，`REFUSE_DUAL_FORCE=1` 仅在放弃单盘镜像之后）。
4. 双盘新随机峰作 occ 分母；T=1 若掉 QPS，双盘只作文 T=4 行。

### 1.3 这轮 boot 现场（2026-08-31 午）

| 项 | 值 |
|----|-----|
| kernel | `7.0.0-30-generic`（不是 6.18） |
| `15:00.0` | BittWare `12ba:0075`，BAR0 256 MiB **disabled** |
| `ntcx` / `1172:0000` | **不存在** |
| CD8P | `d8`=`/dev/nvme3n1`，`d9`=`/dev/nvme4n1` |
| `vmem_sw` | **未加载** |
| 双盘 `.ko` | 已编 `7.0` vermagic；`refuse_dual` 会拒装 |

---

## 文件地图

| 文件 | 职责 |
|------|------|
| `serving/search_beam.cpp` | 计时环、warm、T=4-only flag、跨 query 预取挂钩 |
| `serving/hide_fill.hpp` | `HidePipe`、`hide_warm_entry_ball`、`wait_covering` |
| `serving/page_copy_pool.hpp` | install；`stop_join` 必须在 `t1` 之后（已做） |
| `docs/notes/CXL-ANNS-Prefetcher.md` | 真机数；新节只追加 |
| `/root/chukexin/mem2nvme` `dual-ssd-vmem-sw` | 双盘源码；`tools/refuse_dual_vmem_sw.sh` |
| `results/paper_figs/` | 新日志 |

---

### Task 0: 恢复单盘 hide 数据面（7.0 名）

**Files:**
- Run: `insmod` 单盘 `vmem_sw`（**不要** `nvme_devs`）
- Device: `/dev/nvme4n1` + `0000:d9:00.0`（SN `2F50A1360XK3`）
- Log: `results/paper_figs/hide_restore_7p0_magic.log`

块设备名漂过。上一轮 `nvme2n1` 作废。先确认 pagebin 在 **vmem 逻辑** 420 GiB，不是 raw 盘 420 GiB。

- [ ] **Step 1: 单盘加载（7.0 已编的 `host/vmem_sw.ko`）**

```bash
# 必须没有 backing_count / nvme_devs
/root/chukexin/mem2nvme/tools/refuse_dual_vmem_sw.sh || true
insmod /root/chukexin/mem2nvme/host/vmem_sw.ko \
  nvme_dev=/dev/nvme4n1 target_bdf=0000:d9:00.0 \
  expected_ssd_size_bytes=1920383410176 \
  ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
```

Expected: `/sys/class/vmem/vmem0/nvme_dev` = `/dev/nvme4n1`，无 `backing_count` 或 `=1`。

- [ ] **Step 2: vmem 逻辑偏移读 magic**

```bash
python3 - <<'PY'
import os, mmap, struct
fd = os.open('/dev/vmem0', os.O_RDWR)
m = mmap.mmap(fd, 4096, offset=450971566080)
print(hex(struct.unpack_from('<Q', m, 0)[0]))
PY
```

Expected: `0x314e415843`。若垃圾：单盘镜像已被条带打散，**停**，不要用双盘「修」；要 restage 或找回 6.18 单盘镜像。

- [ ] **Step 3: 真冷 T=1 nq=20 复基线（claim 口径）**

```bash
rmmod vmem_sw && insmod /root/chukexin/mem2nvme/host/vmem_sw.ko \
  nvme_dev=/dev/nvme4n1 target_bdf=0000:d9:00.0 \
  expected_ssd_size_bytes=1920383410176 ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
source /mnt/disk0/chukexin_motivation/serving_t2i_10m/serve_vmem_pagebin.env
unset CXAN_HOST_LAYOUT
numactl --cpunodebind=0 --membind=0 /root/chukexin/CXL-ANNS-KX/serving/search_beam \
  --vmem-dev /dev/vmem0 --vmem-offset 450971566080 --vmem-len 9600069632 \
  --entry "$CXAN_ENTRY" --queries "$CXAN_QUERIES" --gt "$CXAN_GT" --id-map "$CXAN_ID_MAP" \
  --graph-file /mnt/disk0/chukexin_motivation/serving_t2i_10m/pagebin_graph.bin \
  --dram-backend numa --dram-bytes 1073741824 \
  --oneshot-fp --k 10 --iters 0 --beam 400 --shuffle-seed 42 \
  --policy P3 --budget $((64<<20)) --pipe-w 16 \
  --lookahead-k 0 --nbr-bundle --no-score-page --max-q 20 \
  2>&1 | tee /root/chukexin/CXL-ANNS-KX/results/paper_figs/hide_restore_7p0_nq20.log
```

Expected: `from_win=100`，recall≥0.92，QPS **~48–51**，mean **~20 ms**。不替换 50.25。若 magic 坏或 QPS≪40：先修 layout，不要开始 Task 1。

---

### Task 1: 跨 query 预取（T=1 唯一还没锁死的软件刀）

**Files:**
- Modify: `serving/search_beam.cpp` 计时环（`run_one_q` 之前）
- Modify: `serving/hide_fill.hpp` — 抽出 `hide_warm_entry_ball` 的「只发行、不等待」变体
- Test: 先单测「下一 query 的 entry id 来自 host 图 / queries 文件」，不发 NAND
- Log: `hide_qpipe_nq20.log`、`hide_qpipe_nq100.log`

**禁止：** 共享窗 `--cont-batch`、在 `hide_wait` 里打分、look=8 未提交 expand。

语义：T=1 仍串行完成 query *q* 的 top-k。在 *q* 的 **第一次** `hide_wait` 返回之后（窗口已有可打分向量），对 *q+1* 调一次 `hide_warm_entry_ball`（或只发行 entry+1hop bundle 页）。*q+1* 自己的 timed 段仍从自己的 `t0` 算；预取字节计入 *q* 还是 *q+1* 必须在日志里写清（建议：计入被预取的那条 query，避免虚高 *q* 的 occ）。

- [ ] **Step 1: 写失败测试 — 下一 query 入口可在 host 侧解析**

在 `serving/tests/` 加一个小测试（或 `search_beam` 的 dry 路径）：给定 `queries` + `entry`，`next_qi = qi+1` 的 entry id 与 1-hop 邻居列表非空，且 **不** 打开 `/dev/vmem0`。

Expected: 测试在无 vmem 时 PASS。

- [ ] **Step 2: CLI `--prefetch-next-q` 默认关**

`search_beam.cpp` 增加 flag，默认 0。打开时 timed 环内：

```cpp
// after run_one_q(qi) returns, before qi+1 starts its own timer
if (prefetch_next_q && qi + 1 < nq)
  hide_issue_entry_ball(pipe, /*query=*/qi + 1);  // issue only
```

更好（多藏 7.2 ms）：在 *q* **中途**（例如 beam 已半满）发行 *q+1*。先做「query 结束后、下一条开始前」的弱版本；若 nq=20 QPS 不升，再把发行点前移到第一次 `wait_covering` 之后。

- [ ] **Step 3: 编译**

```bash
g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I/root/chukexin/CXL-ANNS-KX \
  serving/search_beam.cpp -o serving/search_beam -lnuma
```

- [ ] **Step 4: 真冷 T=1 nq=20 / nq=100**

同一基线命令 + `--prefetch-next-q`。留下同时满足的才继续：

- recall@10 ≥ 0.92，`from_win=100`，bounce=0
- T=1 nq=20 **且** nq=100 QPS ≥ 复基线（允许 ±2%）
- mean 下降才算吃到 7.2 ms；只涨 QPS、mean 不降 → 查是不是把下一 query 的 IO 计出了 wall

否则 revert flag。 **不替换 50.25**，除非 nq=20 与 nq=100 **都**高于锁定 hide。

---

### Task 2: T=4-only QD（不改 T=1 默认）

**Files:**
- Modify: `serving/search_beam.cpp` — 仅当 `nthreads>=4 && per_thread_window` 才采用 `--t4-issue-ahead` / `--t4-expand-batch`
- Log: `hide_T4only_ahead3_nq20.log`

已测：ahead=3 → 110.23，ebatch=16 → 113.71。VOID 当 **全局** 默认。本任务只把它们变成 **T=4 专用**，T=1 仍 8/2。

- [ ] **Step 1: 默认仍 8/2；T=4 覆盖**

```cpp
if (nthreads >= 4 && per_thread_window) {
  if (t4_issue_ahead) issue_ahead = t4_issue_ahead;
  if (t4_expand_batch) expand_batch = t4_expand_batch;
}
```

- [ ] **Step 2: 真冷 T=4 nq=20**（`--dram-bytes 536870912 --threads 4 --per-thread-window`）+ T=1 nq=20 回归（必须仍 ~50）

Expected T=4：QPS ≥ 本轮 8/2，mean **不高于 34.4+10%**（避免再锁 45 ms）。Expected T=1：与 Task 0 复基线 iso。  
对 **136.23**，不对 85.8。`cap_qps` 仍 &lt; 136 则书面写明必须 Task 3。

---

### Task 3: 双盘抬峰（只服务 T=4 cap，批准后才 restage）

**Files:**
- `/root/chukexin/mem2nvme/host/vmem_sw_*.c`（已有 `nvme_devs`）
- `/root/chukexin/mem2nvme/tools/refuse_dual_vmem_sw.sh`
- Restage: `tools/pack_layout_pagebin` + `tools/pack_nbr_bundle` 到 **新** 双盘逻辑地址

**停条件：** controller 未批准放弃 420/460 GiB 单盘镜像。

- [ ] **Step 1: 算账，先不写盘**

```
新随机峰 P 必须实测（PREFETCH_BATCH 真冷）
T=4 要 136：P >= 12 MB/q × 136 ≈ 1.63 GB/s   （若仍 12 MB/q）
若 P < 1.63：双盘也到不了 136，除非同时少字节
```

- [ ] **Step 2: 空逻辑 restage**（仅 `REFUSE_DUAL_FORCE=1`）

```text
nvme_devs=/dev/nvme3n1,/dev/nvme4n1
target_bdfs=0000:d8:00.0,0000:d9:00.0
expected_ssd_sizes_bytes=1920383410176,1920383410176
ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
```

然后重打 pagebin 9.6 GiB + bundle 267 GiB。旧单盘偏移作废。

- [ ] **Step 3: 新峰 + T=1 nq=20 + T=4 nq=20**

T=1 若掉：双盘只作文 T=4，单路 claim 仍报单盘 50.25。T=4 对 136.23。occ 分母换成 **新** 真冷随机峰，不要继续用 1.560。

---

### Task 4: Device walk — PARKED

`15:00.0` 现为 `12ba:0075`。禁止：改 `ntcx` PCI ID、mmap `resource0`、`insmod vmem.ko` 发数、写 walk 代码当完成。

恢复条件（以后 boot）：`lspci -n -s 15:00.0` 再出现 `1172:0000`，且 `allow_hps_mmio=1` 后 `hps_status` ∈ {0,1,2,3}。那时才回到 `2026-08-31-hide-toward-oracle.md` Task 6。

---

### Task 5: 论文口径（不锁新 hide 行除非 both-beaten）

任何想改 50.25 / 49.25 / 79.02 的二进制，同一套跑齐：T=1 nq=20、T=1 nq=100、T=1 oracle 85.8 对照、T=4 nq=20 vs **136.23**。追加到 `docs/notes/CXL-ANNS-Prefetcher.md` 新节。VOID 标 VOID。

可写的残差句（不改 Intro 合同）：

- 单路 hide / 窗 oracle = **0.59×**，主因 hop 串行，不是 1.56 带宽。
- 四路 hide / shard oracle = **0.58×**；`4×85.8` 不是分母。
- CXL-DRAM 通路未实例化；24 GB/s N/A。

---

## 执行顺序

```
Task 0  7.0 单盘恢复 + magic + T=1 复基线
  → Task 1  --prefetch-next-q（失败即撤）
  → Task 2  T=4-only QD + T=1 回归
  → Task 3  仅当 cap_qps<136 且批准 restage
  → Task 4  停放
  → Task 5  锁行 / 写残差
```

---

## Self-review

- 85.8 与 136.23 都有「能不能到」的书面答案：无 CXL-DRAM 时 T=1 默认不能；T=4 要抬峰或少字节。
- 无占用刷 100%、无 343、无 `--score-page` / `min-issue-use` 重做。
- 双盘源码位置与 `refuse_dual` 和现网 `nvme3n1`/`nvme4n1` 一致。
- HPS 停放条件可检查（PCI ID），不是「以后再说」。
