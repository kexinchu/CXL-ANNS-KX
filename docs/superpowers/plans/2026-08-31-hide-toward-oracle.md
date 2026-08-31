# Hide Toward Oracle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 hide 单路从 50.25 QPS / 19.8 ms 推向窗口 oracle **85.8 / 11.4**，把四路从 79.02 / 34.4 推向测到的四路 oracle **136.23 / 15.8**；`from_win=100`，bounce=0，recall@10 ≥ 0.92。不改 Intro/claim 行，除非 **nq=20 和 nq=100 同时**在 iso-recall 下超过锁定 hide。

**Architecture:** 两条轨并行，顺序不能反。Track A（现在就能做）修 host 运行时：先恢复可对比布局，再砍脏 IO（`issue_use`），再把 `stop_join` 移出 wall，四路只加 QD/双盘数据面、不加线程。Track B（HPS 门控）拉活 BAR 后才做 device 侧 neighbor walk——这是单路 +8.4 ms 的对症，也是四路去掉 per-hop ioctl 的对症。禁止用 `--score-page`、look=8、共享 1 GiB×4、发 `vmem.ko`/`0xff` 当 CXL.mem。

**Tech Stack:** `serving/search_beam` P3 + `--nbr-bundle`，`/dev/vmem0`=`vmem_sw` 单盘 `nvme2n1`/`0000:d9:00.0`，DramWindow = `mbind` node 1。协议：T2I-10M oneshot-fp L=400 k=10 seed=42，true-cold=`rmmod`+`insmod` 到 `cache_used=0`。占用分母锁定 **1.560 GB/s**（软件随机）。四路 oracle 必须 **shard**（线程 t 只 warm/跑 `qi%T==t`）。

---

## 0. 冻结目标与硬上限（读完再动手）

| | 当前 hide | 对位 oracle | 比值 | 缺口性质 |
|--|----------:|------------:|-----:|----------|
| T=1 nq=20 | **50.25 QPS / 19.8 ms** | **85.8 / 11.4** | 0.59× | **延迟**：+8.4 ms/q，`crit_wait` 1.24 + 其余 ~7.2 |
| T=4 nq=20 512 MiB | **79.02 / 34.4** | **136.23 / 15.8** | 0.58× | **同一 hide 缺口** + 字节墙 |

**不要对的数：** `4×85.8=343`（四路算力测到只有 1.59×）；T=8 oracle 106（再加线程是负优化）；本晚 degree 镜像 T=1 oracle 95.91（不替换 85.8）；T=4 nq=100 shard oracle 187 / recall 0.895（VOID）。

**字节墙（四路必须先承认）：**

```
有用 ≈ 12 MB/q
1.560 GB/s / 12 MB/q ≈ 130 QPS     ← 单盘 vmem_sw 随机峰上的 T=4 硬顶
136 × 12 MB/q ≈ 1.63 GB/s          ← 超过 1.560，纯「藏延迟」到不了 136
```

所以：

- **T=1 到 85.8：** 不需要更高带宽（1.02 GB/s < 1.56）。必须藏 hop 往返。占用刷到 100% **无助于** 85.8。
- **T=4 到 136：** 必须 **少搬字节**（`issue_use` 64%→更高）或 **抬峰**（双盘 / 更大 batch / 将来 BAR）。只把占用从 62% 拉到 100% 最多 ≈ **127 QPS**，仍低于 136。

**现场陷阱：** NAND 上现在是 **入度 bundle** + 必须配 `pagebin_graph_deg.bin`。锁定 50.25 用的是 `pagebin_graph.bin`。要比 claim 行，先重打原图序 bundle（Task 1）。`vmem_sw.ko` 用 `/root/chukexin/mem2nvme/host/vmem_sw.ko.6.18-single`。不要装开机那份双盘模块读旧 420/460 GiB。

**禁止重做：** `--score-page`、fill pool 打分、score thread+`work_mu`、slim warm、`pipe_w=32`、look=8 未提交 bundle、共享窗四线程、wait 中打分、sort+单 chunk 56 页、unfaulted READ_BATCH、`restore_vmem_sw.sh`、入度序再打一遍当利用率赢。

---

## 文件地图

| 文件 | 职责 |
|------|------|
| `serving/search_beam.cpp` | P3 循环、oracle-window shard、`stop_join` 相对 `t0/t1`、CLI |
| `serving/hide_fill.hpp` | `hide_collect_bundle_pages`、`HidePipe`、`stall_if_full`、`wait_covering` |
| `serving/placement.hpp` | `for_ids_contained_in_page`、bundle 偏移 |
| `serving/metrics.hpp` | `issue_use`、`nvme_read_bytes` |
| `serving/page_copy_pool.hpp` | NAND memcpy 工人；`stop_join` 必须在 `t1` **之后** |
| `tools/pack_nbr_bundle.cpp` | 267 GiB bundle 重打 |
| `docs/notes/CXL-ANNS-Prefetcher.md` | 真机数与 VOID 列表 |
| `results/paper_figs/` | 每次 hide/oracle 日志 |

---

### Task 1: 恢复图序 bundle（让 hide 能和 50.25 比）

**Files:**
- Run: `tools/pack_nbr_bundle` → offset `460571635712`
- Graph: `/mnt/disk0/chukexin_motivation/serving_t2i_10m/pagebin_graph.bin`（**不是** `_deg`）
- Log: `results/paper_figs/pack_nbr_bundle_restore.log`

入度序没有抬 `issue_use`（64.30→62.98）。后续每一刀的 hide 行若要替换 claim，必须在 **原图序** 上测。

- [ ] **Step 1: 确认单盘 vmem 与 pagebin magic**

```bash
lsmod | grep vmem_sw
# 若不是单盘 nvme2n1/d9：
rmmod vmem_sw
insmod /root/chukexin/mem2nvme/host/vmem_sw.ko \
  nvme_dev=/dev/nvme2n1 target_bdf=0000:d9:00.0 \
  expected_ssd_size_bytes=1920383410176 \
  ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
python3 - <<'PY'
import os,mmap,struct
fd=os.open('/dev/vmem0',os.O_RDWR)
m=mmap.mmap(fd,4096,offset=450971566080)
print(hex(struct.unpack_from('<Q',m,0)[0]))  # 0x314e415843
PY
```

Expected: magic `0x314e415843`。

- [ ] **Step 2: 整份重打原图序 267 GiB（约 45 min）**

```bash
/root/chukexin/CXL-ANNS-KX/tools/pack_nbr_bundle \
  --in-file /mnt/disk0/chukexin_motivation/serving_t2i_10m/layout_t2i_10m.bin \
  --graph-file /mnt/disk0/chukexin_motivation/serving_t2i_10m/pagebin_graph.bin \
  --id-map /mnt/disk0/chukexin_motivation/serving_t2i_10m/new_to_old_pagebin.bin \
  --out-vmem-dev /dev/vmem0 --out-vmem-offset 460571635712 \
  --out-json /root/chukexin/CXL-ANNS-KX/results/paper_figs/nbr_bundle.json \
  --threads 16 \
  2>&1 | tee /root/chukexin/CXL-ANNS-KX/results/paper_figs/pack_nbr_bundle_restore.log
```

Expected: 日志末行 `DONE nbr-bundle bytes=286720000000`。

- [ ] **Step 3: 真冷复基线 T=1 nq=20（`--graph-file pagebin_graph.bin`）**

```bash
rmmod vmem_sw && insmod /root/chukexin/mem2nvme/host/vmem_sw.ko \
  nvme_dev=/dev/nvme2n1 target_bdf=0000:d9:00.0 \
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
  2>&1 | tee /root/chukexin/CXL-ANNS-KX/results/paper_figs/hide_restore_nq20.log
```

Expected: `from_win_pct=100`，`recall@10≥0.92`，`issue_use` 约 64%。QPS 应回到 **~48–50**（mean ~20 ms）。若 wall 远大于 `nq×mean`（`stop_join` 税），仍以 **mean** 对 19.8，QPS 以 Task 2 修正后再锁。

---

### Task 2: 把 `stop_join` 移出计时（测量刀，两路都要）

**Files:**
- Modify: `serving/search_beam.cpp` 里 `t1` 与 `shared_pool.stop_join()` / `c->pool.stop_join()` 的顺序

现状：`t1` 在 `stop_join` **之后**（约 L1616–1631）。写完 267 GiB 后 r1 QPS 12.71、r2 26.42，mean 仍 20.4 ms——虚低 QPS。

- [ ] **Step 1: 改顺序**

`nthreads>1` 分支：先 `th.join()`，再 `t1 = now()`，**然后** 各 `pool.stop_join()` + `munmap`。  
`nthreads==1`：`t1` 在 `shared_pool.stop_join()` **之前**。`nvme_sect1` 仍在 `t1` 之后采一次（排空的扇区不算进 occ）。

```cpp
  // after worker join / last run_one_q
  auto t1 = std::chrono::steady_clock::now();
  if (nthreads > 1) {
    for (auto& c : thr_ctx) {
      metrics.add_from(c->m);
      if (pref.policy == PrefetchPolicy::P3) c->pool.stop_join();
      if (c->dram) munmap(c->dram, dram_bytes);
    }
  }
  if (use_shared_pool) shared_pool.stop_join();
  const uint64_t nvme_sect1 = nvme_read_sectors();
```

注意：`nthreads>1` 里现在 `add_from` 在 join 后、`t1` 前。改完后 `add_from` 可留在 `t1` 后（不影响 QPS）。

- [ ] **Step 2: 编译**

```bash
g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I/root/chukexin/CXL-ANNS-KX \
  serving/search_beam.cpp -o serving/search_beam -lnuma
```

- [ ] **Step 3: 真冷 T=1 nq=20 复测**

同一命令如 Task 1 Step 3。Expected: `wall_s ≈ nq×mean/1000`（20×20.5 ms ≈ 0.41 s → ~48–50 QPS），不再出现 wall=1.57、QPS=12。

---

### Task 3: `issue_use` 直方图 + 低产页策略（T=1 减税，T=4 破 130 QPS 顶）

**Files:**
- Modify: `serving/hide_fill.hpp` `hide_collect_bundle_pages`
- Modify: `serving/metrics.hpp`（可选：`issue_use` 分桶）
- Modify: `serving/search_beam.cpp` 增加 `--min-issue-use`（0–100，默认 0=关）

入度序已失败。剩下可做的是 **运行时：低产 bundle 页不发整页**。禁止改成 `--score-page`。

定义：页的 `u = want / contained`。`contained` 用已有 `for_ids_contained_in_page`。`want` = 这次 expand 未见邻居。

策略（`--min-issue-use 80` 表示 u&lt;0.8 的页不进 `PREFETCH_BATCH`）：

- 不发该 bundle 页。
- 对该页上每个 `want` id，改走 `hide_collect_vec_pages`（pagebin 向量页）。
- **先测再留。** pagebin 页可能更脏；若 `issue_use` 升但 QPS 降或 recall 掉，**整刀撤回**。

- [ ] **Step 1: 只加直方图，不改发页（nq=20 真冷）**

在 `hide_collect_bundle_pages` 里对每个将发的页记 `u` 到 5 个桶：`[0,.2) [.2,.4) [.4,.6) [.6,.8) [.8,1]`。打印 `issue_use_hist=...`。

Expected: 多数脏页落在 0.4–0.8（与 64.3% 均值一致）。若几乎全在 ≥0.8，Task 3 策略无意义，跳到 Task 4。

- [ ] **Step 2: 实现 `--min-issue-use`**

`hide_collect_bundle_pages` 增加参数 `float min_use`。伪代码：

```cpp
// after computing on, w for page p (on>0)
if (min_use > 0 && (float)w / (float)on < min_use) {
  // do not push p; for each want id whose vector lies in p:
  hide_collect_vec_pages(pl, vb, pb, id, pages, seen);
  continue;
}
pages.push_back(p);
```

默认 `min_use=0` 行为与现在完全相同。

- [ ] **Step 3: 扫描 min_use ∈ {0, 0.6, 0.8, 1.0}，真冷 T=1 nq=20**

每档一条日志 `hide_minuse_{0,60,80,100}_nq20.log`。留下同时满足的档：

- `recall@10 ≥ 0.92`
- `from_win=100`
- `issue_use` 高于 64.3
- T=1 QPS **不低于** 复基线（允许 ±2%）

否则 revert 该档。`min_use=1.0` 几乎等于「只发满页未见」，预期 QPS 掉，作对照不要锁。

- [ ] **Step 4: 胜出档再跑 T=1 nq=100 与 T=4 nq=20**

T=4 命令（与锁定 79.02 对齐）：

```bash
numactl --cpunodebind=0 --membind=0 serving/search_beam \
  ... --graph-file pagebin_graph.bin --nbr-bundle --no-score-page \
  --dram-bytes 536870912 --max-q 20 --threads 4 --per-thread-window \
  --min-issue-use <WIN>
```

Expected（T=4）：`nvme_real_GBps` 下降或持平，QPS 上升。有用字节/q 降到 ≤11.5 MB 才有希望在 1.56 峰上靠近 136。

---

### Task 4: 四路 QD / 数据面（只服务 T=4，不碰 T=1 claim）

T=4 占用 62% of 1.56。拉满软件峰 ≈ 127 QPS，仍 &lt; 136。本任务只做 **不重打 267 GiB** 的 QD 刀；双盘条带另开 Task 5（因为要 restage）。

- [x] **Step 1: 每线程独立 `PageCopyPool` 已存在。确认 T=4 没有误走 `use_shared_pool`**

`search_beam.cpp`：`use_shared_pool = P3 && !(per_thread_window && nthreads>1)`。Expected: T=4 `--per-thread-window` 时日志 `per-thread windows=4`，不要 `cont_batch`。

- [x] **Step 2: T=4 只加 issue-ahead / expand-batch 的对照（在复基线布局上）**

锁定是 ebatch=8 ahead=2。只测：

| 档 | flags | 预期 |
|----|-------|------|
| 锁定 | 默认 bundle | ~79 QPS，occ ~62% |
| ahead=3 | `--issue-ahead 3` | 曾 T=1 无赢；T=4 可能吃 QD。QPS 不升则撤 |
| ebatch=16 | `--expand-batch 16` | 同上 |

禁止 `pipe_w=32`、`--cont-batch`（共享窗）。nq=20 真冷一条 + nq=100 一条。nq=100 T=4 对 87.21，不是对 136。

- [x] **Step 3: 记录 occ 与有用字节/q**

```
bytes_per_q = nvme_read_B / nq
cap_qps = 1.560e9 / bytes_per_q
```

若 `cap_qps < 136`，书面写明：**本路径到不了四路 oracle，必须 Task 3 或 Task 5。**

---

### Task 5: 双盘软件条带（可选，重，先算再打）

**只在 Task 3/4 之后、且 `cap_qps<136` 仍成立时做。** 树内 `vmem_sw` 是单盘；开机双盘 `.ko` **不在树里**，且会打散 420/460 GiB。

- [ ] **Step 1: 找到或移植双盘 `vmem_sw` 源码**（`backing_count` / `nvme_devs`）。没有源码就 **停**，不要装不明 `.ko`。

- [ ] **Step 2: 新逻辑地址上重 stage pagebin（9.6 GiB）+ bundle（267 GiB）**。旧 d9 镜像作废。

- [ ] **Step 3: 真冷重测软件随机峰（双盘）**。新 occ 分母。再跑 T=1 nq=20（**不应**为了双盘牺牲单路；若 T=1 QPS 掉，双盘只用于 T=4 论文行，单路仍报单盘）。

- [ ] **Step 4: T=4 nq=20**。目标：`nvme_real` 能到 ~1.6 GB/s 且 recall≥0.92，QPS 逼近 136。

---

### Task 6: Track B — BAR/HPS 门控（单路对症，四路也受益）

没有 HPS **不要**写 device walk 代码当完成。

- [x] **Step 1: 存活检查（只读 4 KiB，禁止扫 16 KiB BAR）** — 2026-08-31：4 KiB mmap 在第一次页故障挂死，无 hex。按 HPS 死跳过其余。见 `docs/notes/CXL-ANNS-Prefetcher.md` §15。

```bash
python3 - <<'PY'
import os,mmap
fd=os.open('/sys/bus/pci/devices/0000:15:00.0/resource0',os.O_RDONLY)
m=mmap.mmap(fd,4096,mmap.MAP_SHARED,mmap.PROT_READ)
print(m[:16].hex(), 'all_ff', set(m[:256])=={255})
PY
```

仍全 `ff` → 本 Task 其余步骤 **跳过**，回去 Track A。

- [x] **Step 2: HPS 寄存器可读且 `status≤3` 之后** — SKIPPED（HPS dead；未发 `HPS_PAGE_FETCH` / `vmem.ko`）。

- [x] **Step 3: Device 侧 neighbor walk** — SKIPPED（HPS dead；未写 walk 代码）。

Host 只提交 query / 收 top-k；hop *i* 的 N(u) 在 device 上推进窗口。验收：

- T=1：mean 从 ~19.8 降向 **11.4**，`crit_wait→0`，`from_win=100`
- T=4：mean 从 ~34 降向 **16**，QPS 向 136（若字节墙仍在，walk 不能单独破 1.56）

---

### Task 7: 锁定新行的验收清单

任何想改论文 hide 数字的改动，**同一二进制**跑齐：

| 行 | 必须 |
|----|------|
| T=1 nq=20 | QPS、mean、recall、from_win、issue_use、nvme GB/s、occ vs **1.560** |
| T=1 nq=100 | 同上；recall≥0.92 |
| T=1 oracle-window nq=20 | 对照仍约 85.8（原布局） |
| T=4 nq=20 512 MiB `--per-thread-window` | 对 **136.23**，不是对 85.8 |
| T=4 shard oracle nq=20 | 复测仍 ≥0.92；QPS 作分母 |

替换 50.25 的条件：T=1 **nq=20 与 nq=100** 都更高，且 iso-recall。  
替换 79.02 的条件：T=4 nq=20 更高，mean 不炸，recall≥0.92。

把数字写入 `docs/notes/CXL-ANNS-Prefetcher.md` 新节，VOID 行标 VOID。

---

## 执行顺序（不要跳）

```
Task 1 恢复图序 bundle
  → Task 2 stop_join 出 wall
  → Task 3 issue_use 直方图 + min-issue-use（T=1 与 T=4 都测）
  → Task 4 四路 ahead/ebatch（无赢即撤）
  → 若 T=4 仍 cap_qps<136：Task 5 双盘（有源码才做）
  → Task 6 仅当 BAR 不再是 0xff
  → Task 7 锁行
```

单路和四路 **共享** Task 1–3。Task 4–5 主要是四路。Task 6 是单路第一对症、四路第二对症。

---

## Self-review

- 85.8 与 136.23 都有对应任务和验收行。
- 无「把占用刷到 100% 当 T=1 赢」。
- 无入度序重打、无 `--score-page`、无 343 分母。
- `min-issue-use` / `stop_join` / 双盘门控名称前后一致。
- HPS 死则 Task 6 跳过，Track A 仍可单独交付。
