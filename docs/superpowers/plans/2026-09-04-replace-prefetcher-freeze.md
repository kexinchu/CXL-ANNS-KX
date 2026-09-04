# Replace Prefetcher Freeze Implementation Plan

**DONE AND LOCKED (2026-09-04).** Tasks 1–6 landed. Task 7 reconfirm was
blocked by host load and must **not** rewrite freeze rows. Do not reopen
this plan to retune the prefetcher. Source of truth:
`docs/notes/2026-09-04-prefetcher-freeze.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 DiskANN-10M 冻结 prefetcher 从 oneshot-fp e4 a1（hide/oracle 0.20×）换成已测到的 PQ-64 + end-batch hide（nq=20 **0.69×** / nq=100 **0.72×**），恢复缺失源码、删掉无效旗标、用同一套数字重新冻结。

**Architecture:** 唯一搜索合同是 `search_one_pq`：host 图 + host PQ-64 走完 L=400 beam（零 NAND），再对 L 个 cand 发一波向量页，`HidePipe` bounce install 后窗内 FP rerank。`--pipe-drive` / hop 宽度 / score-page 不再是 prefetcher 身份。当前 `search_beam.cpp` 里没有这条路径，必须先从 2026-09-04 跑数实现捞回，再削 CLI。

**Tech Stack:** `serving/search_beam.cpp`, `serving/pq_table.hpp`, `serving/hide_fill.hpp`, `serving/prefetch.hpp`, `serving/metrics.hpp`, `tools/run_10m_pq_nq100.sh`, g++ / numactl / `/dev/vmem0`.

**Spec:** `docs/superpowers/specs/2026-09-04-pq-endbatch-prefetcher-design.md`

**Hard stops:** 不 `insmod vmem.ko`，不 `--switch`，不写 420/460/800/900/930/950/1100。复测前 `fuser /dev/vmem0` 必须空。不替换历史 claim 50.25 / 49.25 / 85.8。

**验收数字（pollute @420，extent @1100，seed-42，PQ-64，pipe off）：**

| nq | QPS 下限 | recall@10 | from_win |
|---:|---------:|----------:|---------:|
| 20 | ≥ 110 | ≥ 0.92 | 100 |
| 100 | ≥ 114 | ≥ 0.92 | 100 |

对照 oracle：nq=20 **161.63 / 0.925**，nq=100 **161.52 / 0.923**。

---

## File map

| file | responsibility |
|------|----------------|
| `serving/pq_table.hpp` | 已有。64B FixedChunk MIPS ADC。不改文件格式。 |
| `serving/search_beam.cpp` | 恢复 `search_one_pq` + CLI `--pq-nav`；DiskANN 默认走 PQ；dispatch。 |
| `serving/prefetch.hpp` | `Prefetch` 加上 `pq` / `pq_nav`；删掉已 DROP 的 hop 字段。 |
| `serving/hide_fill.hpp` | 保留 `HidePipe` / `hide_extent_run`；删 `hide_stripe_fill` 调用。 |
| `serving/metrics.hpp` | end-batch 打分后 `note_pf_used`，修好 `page_use=0`。 |
| `serving/tests/test_pq_table.cpp` | ADC 单测。 |
| `serving/tests/test_extent_run.cpp` | `hide_extent_run` 单测。 |
| `tools/run_10m_pq_nq100.sh` | **唯一** 10M hide/oracle 配方。`PIPE=0`。 |
| `docs/notes/2026-09-04-prefetcher-freeze.md` | 改写成新冻结（替换 e4 a1 正文）。 |
| `docs/notes/2026-09-04-prefetcher-pq-freeze.md` | 标明 superseded，指向上一文件。 |

不在本 plan 拆 `search_beam.cpp`（2300 行）。先恢复可复现，再删旗标。PrefetchHub / T>1 不动。

---

### Task 1: PqTable + extent-run 单测

**Files:**
- Create: `serving/tests/test_pq_table.cpp`
- Create: `serving/tests/test_extent_run.cpp`
- Modify: `serving/tests/Makefile`

- [ ] **Step 1: 写 PqTable 失败单测**

```cpp
#include "serving/pq_table.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
  PqTable pq;
  pq.n = 2;
  pq.dim = 2;
  pq.nchunks = 2;
  pq.chunk_off = {0, 1, 2};
  pq.tables_T.assign((size_t)pq.dim * PqTable::kCentroids, 0.f);
  pq.tables_T[(size_t)0 * PqTable::kCentroids + 1] = 1.f;
  pq.tables_T[(size_t)1 * PqTable::kCentroids + 2] = 1.f;
  pq.codes = {1, 2, 0, 0};
  pq.lut.assign((size_t)pq.nchunks * PqTable::kCentroids, 0.f);
  const float q[2] = {1.f, 1.f};
  pq.begin_query_ip(q);
  assert(pq.dist(0) == -2.f);
  assert(pq.dist(1) == 0.f);
  std::puts("test_pq_table OK");
  return 0;
}
```

- [ ] **Step 2: 写 extent-run 失败单测**

```cpp
#include "serving/hide_fill.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
  const size_t pb = 4096;
  std::vector<uint64_t> far = {0, 64 * pb};
  hide_extent_run(far, pb, 32);
  assert(far.size() == 2);

  std::vector<uint64_t> tight = {0, 2 * pb, 3 * pb};
  hide_extent_run(tight, pb, 32);
  assert(tight.size() == 4);
  assert(tight[1] == pb);

  std::vector<uint64_t> wide = {0, 31 * pb};
  hide_extent_run(wide, pb, 32);
  assert(wide.size() == 2);

  std::puts("test_extent_run OK");
  return 0;
}
```

`wide`：span = 32，density = 2/32 < 50%，不应填洞。

- [ ] **Step 3: 挂进 Makefile**

在 `serving/tests/Makefile` 的 `test:` 目标加上 `test_pq_table test_extent_run`，并加两条编译规则（`test_extent_run` 需要 `-pthread`，因为 `hide_fill.hpp` 拉 `prefetch_hub.hpp`）。

```makefile
test: test_metrics test_placement test_hot_set test_diskann_entry test_nav_graph test_score_cache test_prefetch_hub test_pq_table test_extent_run
	./test_metrics
	./test_placement
	./test_hot_set
	./test_diskann_entry
	./test_nav_graph
	./test_score_cache
	./test_prefetch_hub
	./test_pq_table
	./test_extent_run

test_pq_table: test_pq_table.cpp ../../serving/pq_table.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_pq_table.cpp

test_extent_run: test_extent_run.cpp ../../serving/hide_fill.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ test_extent_run.cpp
```

- [ ] **Step 4: 跑单测确认通过**

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests && make test_pq_table test_extent_run && ./test_pq_table && ./test_extent_run
```

Expected: `test_pq_table OK` 然后 `test_extent_run OK`。

- [ ] **Step 5: Commit**

```bash
git add serving/tests/test_pq_table.cpp serving/tests/test_extent_run.cpp serving/tests/Makefile
git commit -m "$(cat <<'EOF'
Add unit tests for PQ-64 ADC and extent-run hole fill.

EOF
)"
```

---

### Task 2: 恢复 `search_one_pq` 和 `--pq-nav`

当前树里的 `serving/search_beam.cpp` **没有** `search_one_pq` / `--pq-nav`。110.94 / 116.29 是未提交二进制跑的。本 task 只恢复 **pipe-off** 路径（不要恢复 `--pipe-drive`）。

**Files:**
- Modify: `serving/prefetch.hpp`
- Modify: `serving/search_beam.cpp`

- [ ] **Step 1: `Prefetch` 加上 PQ 指针**

在 `serving/prefetch.hpp` 的 `struct Prefetch` 里、`extent_run` 旁加入：

```cpp
  bool pq_nav = false;
  PqTable* pq = nullptr;
```

文件顶部加 `#include "pq_table.hpp"`。

- [ ] **Step 2: 在 `search_one_fp` 之前插入 `search_one_pq`**

插入点：`serving/search_beam.cpp` 里 `static std::vector<uint32_t> search_one_fp(...)` **正上方**。同时加 `#include "serving/pq_table.hpp"`（若只在 prefetch.hpp 里 include，search_beam 已含 prefetch.hpp 即可）。

完整函数（与 2026-09-04 跑数实现一致，**去掉** `pipe_drive` / `issue_top_unresident`）：

```cpp
static std::vector<uint32_t> search_one_pq(Placement& pl, DramWindow& win, Prefetch& pref,
                                           const EntryGraph& eg, const float* qf, uint32_t beam,
                                           uint32_t k, uint32_t iters, PageCopyPool* ext_pool,
                                           VmemIo* vio) {
  PqTable* pq = pref.pq;
  if (!pq || !pq->loaded()) {
    fprintf(stderr, "pq-nav needs loaded --pq-pivots/--pq-compressed\n");
    std::exit(2);
  }
  pq->begin_query_ip(qf);
  pref.on_query_begin(pl, win, eg.nodes, eg.entry_id);

  const size_t vb = (size_t)pl.hdr->dim * pl.hdr->vec_bytes;
  const size_t pb = win.page_bytes;
  const uint32_t L = beam ? beam : k;
  uint32_t Rlim = pl.hdr->R;
  if (Rlim > 64) Rlim = 64;

  std::vector<Cand> cand;
  cand.reserve(L + pl.hdr->R + 8);
  std::unordered_set<uint32_t> seen;
  std::unordered_set<uint32_t> expanded;

  auto insert_cand = [&](uint32_t id, float d) {
    if (cand.size() >= L) {
      auto worst = std::max_element(cand.begin(), cand.end(),
                                    [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
      if (d >= worst->dist) return;
      *worst = {d, id};
    } else {
      cand.push_back({d, id});
    }
  };
  auto pq_insert = [&](uint32_t id) {
    if (id >= pq->n) return;
    float d = pq->dist(id);
    if (cur_met(win)) cur_met(win)->distance_comps++;
    insert_cand(id, d);
  };

  seen.insert(eg.entry_id);
  pq_insert(eg.entry_id);

  auto pick_best = [&]() -> int {
    int bi = -1;
    float bd = 0;
    for (size_t i = 0; i < cand.size(); ++i) {
      if (expanded.count(cand[i].id)) continue;
      if (bi < 0 || cand[i].dist < bd) {
        bi = (int)i;
        bd = cand[i].dist;
      }
    }
    return bi;
  };
  auto expand_one = [&](uint32_t cur) {
    uint32_t nbrs_local[64];
    hide_read_nbrs(pl, cur, nbrs_local, Rlim);
    for (uint32_t j = 0; j < Rlim; ++j) {
      uint32_t nb = nbrs_local[j];
      if (nb >= pl.hdr->n || seen.count(nb)) continue;
      seen.insert(nb);
      pq_insert(nb);
    }
  };

  auto fp_rerank_dram = [&]() {
    for (Cand& c : cand) {
      c.dist = vec_mips_neg(pl.vec(c.id), qf, pl.hdr->dim, pl.hdr->vec_bytes);
      if (cur_met(win)) {
        cur_met(win)->distance_comps++;
        cur_met(win)->note_score_from_window(1);
        cur_met(win)->note_pf_scored_id(c.id);
      }
    }
  };

  if (pref.oracle_dram) {
    uint32_t expands = 0;
    while (true) {
      int bi = pick_best();
      if (bi < 0) break;
      if (iters != 0 && expands >= iters) break;
      uint32_t cur = cand[(size_t)bi].id;
      expanded.insert(cur);
      expands++;
      expand_one(cur);
    }
    fp_rerank_dram();
    std::sort(cand.begin(), cand.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    std::vector<uint32_t> out;
    for (size_t i = 0; i < cand.size() && out.size() < k; ++i) out.push_back(cand[i].id);
    return out;
  }

  const uint32_t W = pref.pipe_w ? pref.pipe_w : 8;
  PageCopyPool local_pool;
  PageCopyPool* pool = ext_pool ? ext_pool : &local_pool;
  if (pool->nworkers == 0) pool->start(W);
  HidePipe hpipe;
  hpipe.pool = pool;
  hpipe.win = &win;
  hpipe.pl = &pl;
  hpipe.vio = vio;
  hpipe.m = cur_met(win);
  hpipe.direct_install = false;
  hpipe.stripe_fill = false;
  hpipe.extent_run = pref.extent_run;

  auto issue_ids = [&](const std::vector<uint32_t>& ids) {
    std::vector<uint64_t> pages;
    std::unordered_set<uint64_t> ps;
    pages.reserve(ids.size() * 2);
    for (uint32_t id : ids) hide_collect_vec_pages(pl, vb, pb, id, pages, &ps);
    if (!pages.empty()) hpipe.issue(pages, /*ttl=*/128, /*stall_if_full=*/true);
    return pages;
  };

  uint32_t expands = 0;
  while (true) {
    hpipe.pump();
    int bi = pick_best();
    if (bi < 0) break;
    if (iters != 0 && expands >= iters) break;
    uint32_t cur = cand[(size_t)bi].id;
    expanded.insert(cur);
    expands++;
    expand_one(cur);
  }

  std::vector<uint32_t> rerank_ids;
  rerank_ids.reserve(cand.size());
  for (const Cand& c : cand) rerank_ids.push_back(c.id);
  auto need = issue_ids(rerank_ids);
  uint64_t wns = hpipe.wait_covering(need);
  if (!wns) wns = hpipe.wait_all();
  if (cur_met(win)) {
    cur_met(win)->device_fill_ns += wns;
    cur_met(win)->crit_wait_ns += wns;
  }
  hpipe.pump();

  alignas(64) uint8_t buf[4096];
  if (vb > sizeof(buf)) {
    fprintf(stderr, "vec too large for score buf\n");
    std::exit(2);
  }
  std::vector<uint64_t> scored_pages;
  std::unordered_set<uint64_t> sps;
  for (Cand& c : cand) {
    bool hit = win.try_copy_resident(pl.ssd_base, pl.vec(c.id), vb, buf);
    if (!hit) win.copy_through(pl.ssd_base, pl.vec(c.id), vb, buf);
    c.dist = vec_mips_neg(buf, qf, pl.hdr->dim, pl.hdr->vec_bytes);
    if (cur_met(win)) {
      cur_met(win)->distance_comps++;
      if (hit) cur_met(win)->note_score_from_window(1);
      else cur_met(win)->note_score_from_bounce(1);
      cur_met(win)->note_pf_scored_id(c.id);
    }
    hide_collect_vec_pages(pl, vb, pb, c.id, scored_pages, &sps);
  }
  if (cur_met(win)) cur_met(win)->note_pf_used(scored_pages);
  if (!ext_pool) pool->stop_join();

  std::sort(cand.begin(), cand.end(),
            [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
  std::vector<uint32_t> out;
  for (size_t i = 0; i < cand.size() && out.size() < k; ++i) out.push_back(cand[i].id);
  return out;
}
```

`note_pf_used` 一并修掉 `page_use=0`（Task 5 若已含于此，Task 5 只做回归）。

- [ ] **Step 3: `search_one` dispatch**

把 `search_one` 开头改成：

```cpp
  if (!oneshot_fp && pref.pq_nav && pref.pq)
    return search_one_pq(pl, win, pref, eg, qf, beam, k, iters, ext_pool, vio);
  if (oneshot_fp)
    return search_one_fp(pl, win, pref, pipe, eg, qf, beam, k, iters, ext_pool, vio, ext_hub);
```

禁止 hybrid：`--oneshot-fp` 时即使加载了 PQ 也走 FP hop，不走 `search_one_pq`。

- [ ] **Step 4: CLI**

在 `main` 的 flag 区加入：

```cpp
  bool pq_nav = false;
  const char* pq_pivots_path = nullptr;
  const char* pq_compressed_path = nullptr;
```

```cpp
    else if (a == "--pq-nav") pq_nav = true;
    else if (a == "--no-pq-nav") pq_nav = false;
    else if (a == "--pq-pivots") pq_pivots_path = need(a.c_str());
    else if (a == "--pq-compressed") pq_compressed_path = need(a.c_str());
```

加载（在 `Prefetch pref;` 赋值之后、query 循环之前）：

```cpp
  PqTable pq_store;
  if (pq_nav) {
    if (!pq_pivots_path || !pq_compressed_path) {
      fprintf(stderr, "--pq-nav needs --pq-pivots and --pq-compressed\n");
      return 2;
    }
    if (!pq_store.load_pivots(pq_pivots_path)) {
      fprintf(stderr, "failed to load --pq-pivots %s\n", pq_pivots_path);
      return 2;
    }
    const uint32_t* n2o = id_map.empty() ? nullptr : id_map.data();
    const uint32_t n2o_n = (uint32_t)id_map.size();
    if (!pq_store.load_codes(pq_compressed_path, n2o, n2o_n)) {
      fprintf(stderr, "failed to load --pq-compressed %s\n", pq_compressed_path);
      return 2;
    }
    pref.pq = &pq_store;
    pref.pq_nav = true;
    printf("pq-nav pivots=%s codes=%s n=%u dim=%u chunks=%u permuted=%d\n",
           pq_pivots_path, pq_compressed_path, pq_store.n, pq_store.dim,
           pq_store.nchunks, (int)(n2o != nullptr));
  }
```

实现时把 `id_map` 换成该文件里真正的 `new_to_old` 变量名（现码 `loaded id-map new→old` 那一段）。

- [ ] **Step 5: 编译**

```bash
g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I/root/chukexin/CXL-ANNS-KX \
  /root/chukexin/CXL-ANNS-KX/serving/search_beam.cpp \
  -o /root/chukexin/CXL-ANNS-KX/serving/search_beam -lnuma
```

Expected: 无 error。`strings serving/search_beam | grep pq-nav` 有输出。

- [ ] **Step 6: Commit**

```bash
git add serving/prefetch.hpp serving/search_beam.cpp
git commit -m "$(cat <<'EOF'
Restore PQ-64 beam plus end-batch hide as the search path.

EOF
)"
```

---

### Task 3: DiskANN 默认改成新 prefetcher

**Files:**
- Modify: `serving/search_beam.cpp`（`diskann_cli` 默认块，约 1906–1913 行一带）

- [ ] **Step 1: 改 DiskANN 默认**

现码：

```cpp
  if (pl.diskann_layout && !use_nbr_bundle) {
    if (!expand_batch_explicit) expand_batch = 4;
    if (!issue_ahead_explicit) issue_ahead = 1;
    if (!score_page_explicit) pref_score_page = false;
    if (!extent_run_explicit) extent_run = true;
  }
```

改成：

```cpp
  if (pl.diskann_layout && !use_nbr_bundle) {
    if (!score_page_explicit) pref_score_page = false;
    if (!extent_run_explicit) extent_run = true;
    if (!oneshot_fp && !pq_nav) {
      fprintf(stderr,
              "diskann hide needs --pq-nav (codebook) or --oneshot-fp (ablation)\n");
      return 2;
    }
  }
```

不要再默认 `expand_batch=4`。PQ 路径不读这个值。

- [ ] **Step 2: 编译 + 无 codebook 应立刻退出**

```bash
g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I/root/chukexin/CXL-ANNS-KX \
  serving/search_beam.cpp -o serving/search_beam -lnuma
./serving/search_beam --diskann-layout --nav-graph /dev/null --help 2>&1 | head
```

更干净的检查：对一个最小假 image 跑 `--diskann-layout` 不带 `--pq-nav`，期望 exit 2 且 stderr 含 `needs --pq-nav`。

- [ ] **Step 3: Commit**

```bash
git add serving/search_beam.cpp
git commit -m "$(cat <<'EOF'
Require PQ-nav for DiskANN hide unless oneshot ablation is set.

EOF
)"
```

---

### Task 4: 删无效旗标和死代码

目标：默认路径再也看不到已 DROP 的 hop 身份。`--oneshot-fp` 仍可编译，但不再带 spec-beam / score-page / stripe / pipe-drive。

**Files:**
- Modify: `serving/prefetch.hpp`
- Modify: `serving/search_beam.cpp`
- Modify: `serving/hide_fill.hpp`

- [ ] **Step 1: 从 `Prefetch` 删除这些字段及所有读写**

删除（或停止赋值，并删 CLI）：

- `lookahead_k`（DiskANN 默认 0，P3 hop 里的 `look_k` 死）
- `spec_beam_nbrs`
- `expand_sib`
- `score_cache`
- `direct_install`
- `stripe_fill`
- `min_issue_use`（bundle-only）

保留：`expand_batch` / `issue_ahead` / `sync_hop` **仅**给 `--oneshot-fp` 用；PQ 路径禁止读它们。`score_page` 默认 false，CLI 可留 `--no-score-page` 作 no-op，或直接删。

- [ ] **Step 2: 从 `search_one_fp` P3 块删除这些 lambda / 调用**

用 ripgrep 确认删干净：

```bash
rg -n "maybe_spec_beam|score_page_occupants|try_cache_score|expand_sib|stripe_fill|direct_install" \
  serving/search_beam.cpp serving/prefetch.hpp serving/hide_fill.hpp
```

Expected: PQ 路径零命中；oneshot 路径不再调用它们。`hide_stripe_fill` 函数可留着但 `hide_issue` 不再调用。

- [ ] **Step 3: `hide_issue` 去掉 stripe 参数**

```cpp
inline void hide_issue(..., bool lookahead = false, bool extent_run = false) {
  ...
  if (extent_run) hide_extent_run(miss, pb, 32);
```

`HidePipe` 删 `direct_install` / `stripe_fill` 成员。

- [ ] **Step 4: 跑 `serving/tests` + 重编 search_beam**

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests && make test
g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I/root/chukexin/CXL-ANNS-KX \
  ../search_beam.cpp -o ../search_beam -lnuma
```

Expected: 全部 `OK`，search_beam 链过。

- [ ] **Step 5: Commit**

```bash
git add serving/prefetch.hpp serving/search_beam.cpp serving/hide_fill.hpp
git commit -m "$(cat <<'EOF'
Drop dead hop flags from the frozen DiskANN prefetcher.

EOF
)"
```

---

### Task 5: `page_use` 回归

Task 2 已在 rerank 后调用 `note_pf_used`。本 task 只确认。

**Files:**
- Modify: `serving/search_beam.cpp`（若 Task 2 漏了 `note_pf_used`）
- Test: 复测 log 的 `page_use_pct` 不再是 0.00

- [ ] **Step 1: 确认 `search_one_pq` 在 FP 循环后有**

```cpp
  if (cur_met(win)) cur_met(win)->note_pf_used(scored_pages);
```

- [ ] **Step 2: 不单独跑 10M。** 等 Task 7。若 nq=20 log 仍 `page_use_pct=0.00`，检查 `hide_issue` 的 `note_pf_issue` 与 `pl.vec` 页号是否同一坐标系（`hide_collect_vec_pages` 必须两边共用）。

---

### Task 6: 配方和冻结文档换成一份

**Files:**
- Modify: `tools/run_10m_pq_nq100.sh`（已是 `PIPE=0`；删 `hide_e8` oneshot 分支或标 ABLATION）
- Modify: `docs/notes/2026-09-04-prefetcher-freeze.md`
- Modify: `docs/notes/2026-09-04-prefetcher-pq-freeze.md`
- Modify: `docs/notes/2026-09-04-prefetcher-10m-t1.md`（顶部加 superseded 指向新冻结）

- [ ] **Step 1: 脚本头改成唯一配方**

文件头改成：

```bash
# Frozen 10M prefetcher (2026-09-04 replace):
# PQ-64 beam + end-batch FP rerank. PIPE must stay 0.
# Does not write 420/460/800/900/930/950/1100.
# Does not replace 50.25 / 49.25 / 85.8.
```

`hide_e8` case 改成打印 `obsolete: use hide_pqbeam` 并 `exit 2`。

- [ ] **Step 2: 重写 `docs/notes/2026-09-04-prefetcher-freeze.md`**

正文只保留新合同。旧 e4 a1 / 25.90 移到文末 “Superseded hide-copy (2026-09-04 morning)” 一节，并写 **replaced by 110.94 / 116.29**。核心 7 条改成 spec 里的 6 条 invariants。Runtime flags 换成 Task 3 的 DiskANN 默认。证据表用 nopipe 行，不要 72.48。

- [ ] **Step 3: pq-freeze 笔记第一段改成**

```markdown
**Superseded as the dual-contract note.** The single freeze is
`docs/notes/2026-09-04-prefetcher-freeze.md` (PQ-64 end-batch, 110.94 / 116.29).
This file remains only as the keep/drop diary that closed nq=20 and pipe-drive.
```

- [ ] **Step 4: Commit**

```bash
git add tools/run_10m_pq_nq100.sh docs/notes/2026-09-04-prefetcher-freeze.md \
  docs/notes/2026-09-04-prefetcher-pq-freeze.md docs/notes/2026-09-04-prefetcher-10m-t1.md
git commit -m "$(cat <<'EOF'
Point the 10M freeze and recipe at PQ end-batch hide.

EOF
)"
```

---

### Task 7: 10M 复测（唯一允许碰 vmem 的 task）

**Files:** 只读 log。不写保护 offset。

- [ ] **Step 1: 独占检查**

```bash
fuser /dev/vmem0 2>/dev/null && echo BUSY && exit 1 || echo vmem_free
cat /sys/class/vmem/vmem0/cache_limit
```

Expected: `vmem_free`，`cache_limit=104857600`。

- [ ] **Step 2: oracle nq=20（不碰 vmem 镜像）**

```bash
PQ_BYTES=64 NQ=20 /root/chukexin/CXL-ANNS-KX/tools/run_10m_pq_nq100.sh oracle
```

Expected: `recall@10≥0.92`，QPS 约 161。

- [ ] **Step 3: hide nq=20**

```bash
PQ_BYTES=64 NQ=20 PIPE=0 /root/chukexin/CXL-ANNS-KX/tools/run_10m_pq_nq100.sh hide_pqbeam
```

Expected: `throughput_QPS≥110`，`recall@10≥0.92`，`from_win_pct=100`，`pipe_drive=0`，`pq_nav=1`，`page_use_pct>0`。

- [ ] **Step 4: hide nq=100**

```bash
PQ_BYTES=64 NQ=100 PIPE=0 /root/chukexin/CXL-ANNS-KX/tools/run_10m_pq_nq100.sh hide_pqbeam
```

Expected: `throughput_QPS≥114`，`recall@10≥0.92`，`from_win_pct=100`。

- [ ] **Step 5: 把 CSV 行写进冻结笔记证据表。** 若任一刀失败：先对照 `search_one_pq` 是否被 `--oneshot-fp` 误入、codebook 是否仍是 `idx_t2i64_*`、offset 是否仍是 `1181116006400`。禁止用改 cache / 改 layout 来“救”QPS。

- [ ] **Step 6: Commit 笔记数字（若有更新）**

```bash
git add docs/notes/2026-09-04-prefetcher-freeze.md results/paper_figs/hide_10m_pq64beam_nopipe_nq20.log \
  results/paper_figs/hide_10m_pq64beam_nopipe_nq100.log
git commit -m "$(cat <<'EOF'
Reconfirm the PQ end-batch 10M freeze rows.

EOF
)"
```

---

## 自检

| spec 要求 | task |
|-----------|------|
| 恢复缺失的 `search_one_pq` | Task 2 |
| DiskANN 默认 = 新路径 | Task 3 |
| 删 pipe-drive / hop 身份 / 已 DROP 旗标 | Task 4 |
| `page_use` 不再假 0 | Task 2 + 5 |
| 一份冻结笔记 + 唯一配方 | Task 6 |
| nq=20/100 复测 ≥ 0.69×/0.72× 门 | Task 7 |
| 单测 ADC / extent-run | Task 1 |
| 不写保护 vmem / 不替换 50.25 | Task 7 hard stops + 全文 |

无 TBD。类型名全程是 `search_one_pq` / `PqTable` / `HidePipe` / `hide_extent_run`。

---

## 不做（避免范围膨胀）

- 不把 `search_beam.cpp` 拆文件（可在复测通过后另开）。
- 不删 `--oneshot-fp` 整条函数（论文 iso-FP 对照还要用）；只剥夺它当默认 / 当冻结的资格。
- 不动 PrefetchHub、T>1、FPGA、重训 codebook。
- 不把 50.25 / 85.8 改成 116.29。
