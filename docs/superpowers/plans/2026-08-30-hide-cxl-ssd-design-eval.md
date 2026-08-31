# Hide CXL-SSD: Design + Eval Revision Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retarget Design and experiments so the system claim is a *usable ANNS service on flash-priced CXL*: prefetch + page/bandwidth policy + continuous batching keep CXL-SSD **off the ANNS critical path**; from the host, scored data looks like CXL-DRAM while capacity stays on CXL-SSD.

**Architecture:** Do not add a sixth module. Re-order D1–D5 around one falsifiable hide objective (critical-path SSD stall → 0 in steady state). Split **fill-to-window** (off path) from **score-from-window** (critical path). A window miss while scoring is a hide failure, not a successful promote.

**Tech Stack:** `serving/search_beam` P3, `vmem_sw` + `PREFETCH_BATCH`, T2I-10M protocol in `docs/superpowers/plans/2026-08-28-serving-eval-test-plan.md`. Paper under `paper/sections/`.

**Out of scope:** Product CXL.mem SSD; 160 GB/s; Shared FAM as a result; LFU hot-set as a win; rewriting motivation F1–F9 unless a finding no longer holds; merging the PipeANN binary; `vmem.ko` numbers while HPS/BAR are dead.

---

## Locked environment (do not relabel)

| Item | Fact (2026-08-30, `gpu01`, kernel `6.18.0-rc5`) |
|------|--------------------------------------------------|
| FPGA | Altera `0000:15:00.0`, `ntcx`/`mem2nvme`, BAR0 32 GiB |
| NAND path | `nvmex` on `0000:d9:00.0` → `/dev/nvme4n1` (CD8P 1.92 TB) |
| Userspace device | `/dev/vmem0` = **`vmem_sw`** (28 GiB RAM map + 4 GiB cache + 2 MiB stripe) |
| HPS / CPU→BAR | **Dead.** HPS `-EOPNOTSUPP`; BAR reads `0xff`. `vmem.ko` exists; **no paper numbers**. |
| CXL-DRAM DAX | **Absent.** DramWindow = 1 GiB `mbind` node 1. |
| Identity sentence | “FPGA-attached NVMe + software memory-semantic proxy.” |

True-cold T2I-10M, L=400, nq=100, seed=42, oneshot-fp (`results/paper_figs/2026-08-30-hw-cxl-ssd.md`):

| Config | QPS | recall | window hit% | avg_promote_ns |
|--------|----:|-------:|------------:|---------------:|
| P0 demand | 1.64 | 0.944 | 1.0 | 72 µs |
| P3 packed | 4.08 | 0.944 | 3.1 | 40 µs |
| P3 pagebin | 5.35 | 0.932 | 34.1 | 24 µs |
| PipeANN pipe T=1 PQ-on | 57.6 | 0.925 | — | block path |

**Gate:** hide is **not** achieved. 24–72 µs promote is NAND-class. Window hit 3–34% means most scores still miss. P3/P0 ≈ 2.5–3.3× is “less bad,” not “SSD invisible.”

Packed image: vmem offset 400 GiB (`serve_vmem.env`). Pagebin: 420 GiB (`serve_vmem_pagebin.env`). True-cold: `rmmod`+`insmod` until `cache_used=0`. `--flush-window` alone is **void**.

---

## Why current Design / P3 miss the goal

User sentence:

> From the host, data *looks like* it lives in CXL-DRAM. Capacity is CXL-SSD. Software must hide the difference.

Current P3 (`serving/search_beam.cpp` ~260–460) still:

1. Issues `PREFETCH_BATCH` for this hop’s miss pages.
2. `PageCopyPool` copies SSD → host bounce.
3. **Scores from the bounce** (`assemble_from_host` / `try_score_ready`).
4. Then `install_top` into DramWindow for *later* hops.

That is PipeANN-style overlap. The hop is still coupled to NAND completion. Prefetch reduces wait; it does not take SSD off the critical path.

Pointer-chasing limit (keep in the paper): **one query cannot fully hide hop i+1 until hop i scores.** Intra-query hide = lookahead of the current frontier. Inter-query **continuous batching** is the only way to keep device QD up while one walk is stuck.

---

## Success metrics (lock before rewriting figures)

Extend CSV **without breaking the first 15 columns**. Add a tail:

| Name | Meaning | Hide **pass** (steady-state, nq≥100) |
|------|---------|--------------------------------------|
| `score_from_window` / `score_from_window_pct` | FP distances whose bytes were in DramWindow *before* this hop’s ioctl | **100%** (packed nq=5/20 and pagebin nq=5 already) |
| `score_from_bounce` | FP distances assembled from `PageCopyPool` | should → 0 on hide pass |
| `crit_wait_ns` | Scoring thread blocked on NAND/ioctl/pool wait | ≈ window memcpy, **not** 20–80 µs |
| `overlap_ratio` | `device_fill_ns / wall_ns` | ≥ 1 means hiding; ≪ 1 critical-path bound |
| `window_hit_pct` | Existing `cxl_dram_hit_pct` | Report; do not call it hide if `crit_wait_ns` is NAND |
| QPS @ recall≥0.92 | Secondary | Compare to **oracle-in-window**, not only DiskANN |

**Oracle-in-window (required new row):** pin/prefault the measured working set into the 1 GiB window, then `--no-vmem-prefetch` with no SSD fills. That is “what ANNS would see if hide succeeded.” PipeANN stays *block-path* contrast.

**No claim downgrade.** Target is **100%** `score_from_window` and DRAM-class `crit_wait_ns`. If a row misses, fix the runtime (lookahead, warm, install, budget, cont)—do not shrink §1.

---

## Design remap (paper §4) — no sixth module

| ID | Old name | New job | Code today | Change after approval |
|----|----------|---------|------------|------------------------|
| D1 | Placement | Graph/PQ/entry in **host DRAM** so the window is 100% vectors | `Placement::set_graph_host` exists; **`--graph-in-dram` not parsed** in `search_beam.cpp` | Wire it. Default **on** for hide rows. Copy graph from the **host file**, not from `/dev/vmem0` (do not warm the 4 GiB cache). |
| D2 | Bounded promote | `install_top` = who *stays*. Score **only** from window. | Scores from bounce then installs | Split score-from-window vs fill-to-window. Miss on score increments hide-fail. |
| D3 | Hot set | Entry pin + soft-pin only | `hot_set.hpp` unused in binary; E3 LFU failed | Do not claim D3 QPS. Paper already demoted. |
| D4 | Prefetch + cont | **Hide engine.** Lookahead hop i+1 / next query *before* score of hop i; target QD across queries | Hop-barrier P3 + ioctl; no global `cont` | Add lookahead + cross-query admit into `PageCopyPool`. Do not merge PipeANN. |
| D5 | Iso-recall + stats | Min L at floor 0.92; hide counters | Precision stuck at 100% (every fetch later scored from bounce) | `precision = score_from_window / fetched` |

Fig.6: Host DRAM (graph/PQ) | Window = virtual CXL-DRAM | CXL-SSD corpus. Arrows: “prefetch/cont (off crit path)” vs “score (window only).”

Fig.7–8: *time* diagrams (hop score overlapping next fill + next query), not promote-API boxes.

Forbidden: page-fault a neighbor “to be helpful”; `install_all` as a win; `--threads>1 --shared-window` as a win; 160 GB/s; Shared FAM results.

---

## File map

| File | Responsibility |
|------|----------------|
| `paper/sections/{intro,impl,eval,discussion,conclusion,background,design,motivation,related}.tex` | Environment + hide goal (Task 1 landed 2026-08-30) |
| `paper/main.tex` | Title/abstract retargeted to hide |
| `serving/metrics.hpp` | New counters + `print` tail |
| `serving/tests/test_metrics.cpp` | Precision no longer 100% when fetch > window-scored |
| `serving/search_beam.cpp` | `--graph-in-dram` default; hide split; CSV tail |
| `serving/dram_window.hpp` | `copy_if_resident` (no promote) |
| `serving/prefetch.hpp` | Optional lookahead queue (Task 3b) |
| `results/paper_figs/hide_matrix.csv` | Task 4 numbers |
| `paper/sections/design.tex` | Task 5: rewrite Fig.6–8 only after hide numbers exist |

---

## Task 1: Land environment + goal in the paper

**Status:** done in this session (re-check `cd paper && make` ≤ 11 body pages).

- [x] Intro: service goal is hide; one-copy is not the Rapid Review claim.
- [x] Abstract/title: hide + honest 3.3× / not-hide / FPGA+`vmem_sw`.
- [x] Impl: BAR 32 GiB, HPS down, numbers from `vmem_sw`, DAX absent.
- [x] Eval Table 3: P0 1.64 / P3 packed 4.08 / P3 pagebin 5.35 / PipeANN 57.6; Q2/Q4 retargeted.
- [x] Design lead + D3 demote + D4 hide-engine wording (Fig.6–8 still placeholders).
- [x] Discussion/conclusion: hide not met; remaining debt.
- [x] `cd paper && make` — 12 PDF pages; body+figures end ~p.9 (conclusion); refs/appendix free. ≤ 11 body pages.

Do **not** claim hide success in captions.

---

## Task 2: Human approval (stop here until yes)

- [ ] Rapid Review lead is **hide**, not one-copy.
- [ ] No `vmem.ko` numbers until HPS/BAR writes work.
- [ ] Next code is score-from-window split + hide CSV, **not** more LFU.
- [x] No shrink: 100% `score_from_window` is the bar; keep iterating components.

---

## Task 3: Runtime hide counters + `--graph-in-dram` (TDD)

**Files:**
- Modify: `serving/metrics.hpp`
- Modify: `serving/tests/test_metrics.cpp`
- Modify: `serving/dram_window.hpp`
- Modify: `serving/search_beam.cpp` (flag parse ~915; P3 loop ~292–460; setup ~1007)
- Test: `make -C serving/tests test`

### Task 3a: Metrics API

- [ ] **Step 1: Extend the unit test** in `serving/tests/test_metrics.cpp`:

```cpp
#include "serving/metrics.hpp"
#include <cassert>
#include <cstdio>

int main() {
  Metrics m;
  m.note_promote_pages(10);
  m.note_promote_used(7);
  m.note_promote_pages(5);
  m.note_promote_used(1);
  assert(m.promote_pages == 15);
  assert(m.promote_used == 8);
  double p = m.precision_pct();
  assert(p > 53.3 && p < 53.4);

  m.note_score_from_window(3);
  m.note_score_from_bounce(7);
  m.note_fetched_pages(10);
  assert(m.score_from_window == 3);
  assert(m.score_from_bounce == 7);
  double hide = m.score_from_window_pct();
  assert(hide > 29.9 && hide < 30.1);
  double prec = m.hide_precision_pct();
  assert(prec > 29.9 && prec < 30.1);  // 3 window scores / 10 fetched
  m.crit_wait_ns = 1234;
  m.device_fill_ns = 5000;
  m.wall_ns = 2000;
  assert(m.overlap_ratio() > 2.4 && m.overlap_ratio() < 2.6);

  Metrics a, b;
  a.note_score_from_window(1);
  b.note_score_from_window(2);
  b.note_score_from_bounce(4);
  a.add_from(b);
  assert(a.score_from_window == 3);
  assert(a.score_from_bounce == 4);
  std::puts("test_metrics OK");
  return 0;
}
```

- [ ] **Step 2: Run the test and confirm it fails** to compile (`note_score_from_window` missing).

```bash
make -C /root/chukexin/CXL-ANNS-KX/serving/tests test_metrics
```

Expected: compile error on `note_score_from_window`.

- [ ] **Step 3: Add fields** to `serving/metrics.hpp` (keep existing fields; extend `add_from` / `print` / `reset`):

```cpp
  uint64_t score_from_window = 0;
  uint64_t score_from_bounce = 0;
  uint64_t fetched_pages = 0;
  uint64_t crit_wait_ns = 0;
  uint64_t device_fill_ns = 0;
  uint64_t wall_ns = 0;

  void note_score_from_window(uint64_t n = 1) { score_from_window += n; }
  void note_score_from_bounce(uint64_t n = 1) { score_from_bounce += n; }
  void note_fetched_pages(uint64_t n) { fetched_pages += n; }

  double score_from_window_pct() const {
    uint64_t tot = score_from_window + score_from_bounce;
    return tot ? 100.0 * (double)score_from_window / (double)tot : 0.0;
  }
  double hide_precision_pct() const {
    return fetched_pages ? 100.0 * (double)score_from_window / (double)fetched_pages : 0.0;
  }
  double overlap_ratio() const {
    return wall_ns ? (double)device_fill_ns / (double)wall_ns : 0.0;
  }
```

`print()` must append:

```
score_from_window=%llu score_from_bounce=%llu from_win_pct=%.2f
hide_prec=%.2f crit_wait_ns=%llu overlap=%.2f
```

Do not reorder the existing `metrics queries=...` tokens (scripts parse them).

- [ ] **Step 4: Re-run tests**

```bash
make -C /root/chukexin/CXL-ANNS-KX/serving/tests test
```

Expected: `test_metrics OK` and the other two tests still pass.

### Task 3b: `copy_if_resident` (no promote)

- [ ] Add to `serving/dram_window.hpp` next to `is_resident`:

```cpp
  // Copy [ssd, ssd+n) into dst only if every overlapping page is already
  // in the window. Returns false without touching NAND / copy_through.
  bool copy_if_resident(const uint8_t* ssd_base, const uint8_t* ssd, size_t n,
                        void* dst) {
    if (!is_resident(ssd_base, ssd, n)) return false;
    copy_through(ssd_base, ssd, n, dst);  // hits the window path only
    return true;
  }
```

If `copy_through` still demand-fills on miss, **do not** call it unless `is_resident` is true. Add a comment at the call site in `search_beam` that a `false` return is a hide miss.

### Task 3c: Wire `--graph-in-dram` (default on)

- [ ] In `search_beam.cpp` flag parse (~915), add:

```cpp
  bool graph_in_dram = true;  // hide default
  // ...
  else if (a == "--graph-in-dram") graph_in_dram = true;
  else if (a == "--no-graph-in-dram") graph_in_dram = false;
```

- [ ] After the layout header is valid and `pl.ssd_base` is set, copy graph from a **host file** when `--image` is not used:

```cpp
  std::vector<uint8_t> graph_host_buf;
  if (graph_in_dram && pl.hdr && pl.hdr->len_graph) {
    const char* host_layout = getenv("CXAN_HOST_LAYOUT");
    // prefer host file so we do not pull graph through vmem_sw cache
    FILE* hf = host_layout ? fopen(host_layout, "rb") : nullptr;
    graph_host_buf.resize(pl.hdr->len_graph);
    bool ok = false;
    if (hf) {
      if (fseeko(hf, (off_t)pl.hdr->off_graph, SEEK_SET) == 0 &&
          fread(graph_host_buf.data(), 1, graph_host_buf.size(), hf) ==
              graph_host_buf.size())
        ok = true;
      fclose(hf);
    }
    if (!ok) {
      // fallback: memcpy from mapped image (warms cache; log it)
      memcpy(graph_host_buf.data(), pl.ssd_base + pl.hdr->off_graph,
             graph_host_buf.size());
      fprintf(stderr, "WARN graph-in-dram copied from mmap (cache warm)\n");
    }
    pl.set_graph_host(graph_host_buf.data(), graph_host_buf.size());
  }
```

Set `CXAN_HOST_LAYOUT=/mnt/disk0/chukexin_motivation/serving_t2i_10m/layout_t2i_10m.bin` in both serve env files.

- [ ] Rebuild:

```bash
g++ -O2 -std=c++17 -pthread -I/root/chukexin/CXL-ANNS-KX \
  /root/chukexin/CXL-ANNS-KX/serving/search_beam.cpp \
  -o /root/chukexin/CXL-ANNS-KX/serving/search_beam -lnuma
```

Unknown `--graph-in-dram` must no longer exit 2. `--no-hotset` stays unknown (flag was never shipped; do not add it).

### Task 3d: Count hide vs bounce in the P3 hop (minimal split)

Do **not** yet refuse to score from bounce (that would break recall). First **measure** the lie.

In `hop_parallel_score` (`search_beam.cpp` ~422–467):

- When the id is already fully in the window (the loop at ~422 that calls `score_id` because `id_pages` is empty): `metrics->note_score_from_window(1)`.
- When `assemble_from_host` / leftover `score_id` after `wait_idle` scores from bounce: `metrics->note_score_from_bounce(1)`.
- After submitting `fetch`: `metrics->note_fetched_pages(fetch.size())`.
- Around `while (ready_n < fetch.size())` … `pool->wait_idle()`: accumulate elapsed ns into `crit_wait_ns`.

```cpp
      auto t0 = std::chrono::steady_clock::now();
      while (ready_n < fetch.size()) { /* existing wait loop */ }
      pool->wait_idle();
      auto t1 = std::chrono::steady_clock::now();
      if (win.metrics) {
        win.metrics->crit_wait_ns += (uint64_t)
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      }
```

CSV printer (wherever the 15-column row is emitted): append

`from_win_pct,hide_prec,crit_wait_ns,overlap`

without inserting columns in the middle.

- [ ] Rebuild `search_beam`. Smoke 2 queries on a host file (`--image`, no vmem) to confirm the new tokens print and recall is unchanged.

### Task 3e: Optional lookahead (only if 3d lands)

Issue `PREFETCH_BATCH` for **unexpanded candidate neighbors** that are not yet resident, *before* expanding the current node, then score current from window. Keep `PageCopyPool` as the fill engine. Do not import PipeANN.

If this is more than ~80 lines, land 3d first and measure; do not let lookahead block counters.

---

## Task 4: Hide matrix (true-cold + service warm)

**Protocol (every cold row):**

```bash
rmmod vmem_sw
insmod /root/chukexin/mem2nvme/host/vmem_sw.ko \
  nvme_dev=/dev/nvme4n1 target_bdf=0000:d9:00.0 \
  expected_ssd_size_bytes=1920383410176 \
  ram_size_gib=28 cache_size_gib=4 stripe_size_mib=2
# wait until GET_INFO (or sysfs) shows cache_used=0
source /mnt/disk0/chukexin_motivation/serving_t2i_10m/serve_vmem.env   # packed
# or serve_vmem_pagebin.env + --id-map new_to_old_pagebin.bin
```

Identity header on every log: kernel, nvmex BDF, `vmem_sw`, DAX absent, `cache_used`.

Common flags: `--oneshot-fp --k 10 --shuffle-seed 42 --max-q 100 --beam 400 --policy P3 --dram-backend numa`

Rows (write `results/paper_figs/hide_matrix.csv`):

1. P0 packed cold
2. P3 packed cold
3. P3 pagebin cold
4. P3 pagebin `--graph-in-dram` (default) cold
5. P3 pagebin `--no-vmem-prefetch` cold (must **fail** hide: high `crit_wait`)
6. P3 pagebin **service warm**: discard 20q, then 100q, no reload, no `--flush-window`
7. Oracle-in-window: best-effort pin/prefault of the 100-query working set (label pin budget), then `--no-vmem-prefetch`

Do **not** re-run E3 LFU as a candidate win.

Pass/fail on the CSV, not on vibes:

- Hide pass: `from_win_pct ≥ 90` on pagebin **and** mean `crit_wait` per score ≪ 20 µs.
- If only row 6 passes: paper says **service hide**, not cold hide.
- If no row passes: shrink intro; money plot stays “overlap vs demand.”

---

## Task 5: Design + Fig.6–8 after numbers

- [ ] Rewrite D2 only if score-from-window is actually enforced (or keep the “measured loop still scores from bounce” sentence).
- [ ] Replace Fig.6–9 placeholders only where `hide_matrix.csv` has a number.
- [ ] Q3 ablation uses hide columns, not “−LFU as a loss.”
- [ ] `cd paper && make` — still ≤ 11 body pages.

---

## Experiment rewrite (paper §6) — question map

| Q | Ask | Figure | Pass |
|---|-----|--------|------|
| Q1 | Does naive load/store put NAND on the critical path? | F1 cliff (keep) | Miss ~70 µs, QD≈1 |
| Q2 | Does full \sys{} move score off NAND at iso-recall? | Fig.9: `crit_wait` vs compute; QPS twin axis | `crit_wait` ≪ P0; `from_win` high |
| Q3 | Which knob buys hide? | Leave-one-out on hide metrics | −prefetch raises `crit_wait`; −LFU is not a loss |
| Q4 | Cold vs service hide | Fig.11 reload vs discarded warmup | If only warm, say so |
| Q5 | Oracle + PipeANN residual | Oracle row + PipeANN T=1 57.6 @ 0.925 | Gap to oracle = hide debt; gap to PipeANN = PQ/block vs oneshot-fp |

Drop Shared FAM provisioning as a *main* Q4. One-copy stays discussion.

---

## Do not write

- Product CXL.mem SSD; 160 GB/s; 12 GB/s achieved (useful fill still ~0.16–0.19 GB/s).
- Hot-set QPS win; historical `install_all` ÷5 (not reproduced true-cold).
- `--threads>1 --shared-window` as main result.
- “Search cannot see CXL-SSD” until Task 4 passes.
- T=8 PipeANN on the same line as T=1 serving.
- `vmem.ko` / BAR numbers.

---

## Self-review

| Spec requirement | Task |
|------------------|------|
| Prefetch off critical path | 3d measure, 3e lookahead, 4 pass gate |
| Page + bandwidth + cont hide SSD | D2/D4 remap, Q3 ablation |
| Host sees CXL-DRAM, capacity on SSD | D1 graph-in-DRAM, oracle row |
| Honest if hide fails | Task 2 + Task 4 shrink rule |
| Latest hardware identity | Task 1 (landed), Task 4 header |
| No sixth module | File map |
