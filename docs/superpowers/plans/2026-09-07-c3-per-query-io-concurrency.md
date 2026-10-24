# C3 Per-Query I/O Concurrency Motivation Experiment

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce one publication scatter plot — Option A — of effective queue depth versus concurrent queries (T = 1, 2, 4, 8, 16) with a measured `QDsat` line, replacing the schematic `fig:mot-bubbles`.

**Architecture:** Keep the frozen PQ-64 end-batch search path unchanged. Add observation-only inflight/fill accounting, a closed-loop T-sweep, a fio-derived `QDsat`, and an iostat sidecar. The paper figure is a single axes plot. Histograms and time series stay in the raw-result note only.

**Tech Stack:** C++17 `serving/search_beam`, `HidePipe` / `PrefetchHub`, bash + `iostat`, Python 3 + matplotlib, T2I-10M frozen artifacts under `/mnt/disk0/chukexin_motivation`.

**Claim under test:**

```text
Avoid speculative prefetch
        ↓
Wait until rerank candidates C_L are known
        ↓
Issue only required missing vector-page fills
        ↓
Each query exposes a finite independent fill set
        ↓
Effective QD at T=1 is far below QDsat
        ↓
QD_eff(T) rises toward QDsat as independent queries overlap
```

**Target figure (locked — Option A):** one scatter, x = concurrent queries `{1,2,4,8,16}`, y = effective queue depth, dashed horizontal `QDsat`. Caption: *Per-query dependencies limit Flash I/O concurrency.* Do not add extra paper panels. Do not force the points to look linear if the data are not.

---

## Frozen path (do not change search semantics)

The system under test is the locked PQ-64 end-batch path in `serving/search_beam.cpp`:

```text
host graph + PQ-64 ADC beam
  -> commit C_L (beam = 400)
  -> hide_collect_vec_pages for those IDs
  -> HidePipe::issue missing unique pages (optional extent_run extras)
  -> wait until covering
  -> exact MIPS rerank from resident bytes
```

Key functions:

| Step | Function | File |
|---|---|---|
| PQ beam, no NAND | `pqq_beam` | `serving/search_beam.cpp:1462` |
| Collect + issue C_L pages | `pqq_issue` → `hide_collect_vec_pages` + `PrefetchHub::submit` | `serving/search_beam.cpp:1492`, `serving/hide_fill.hpp:22`, `serving/cont_batch.hpp:159` |
| Filter already-resident pages | `HidePipe::issue` | `serving/hide_fill.hpp:538` |
| Async copy | `hide_start` / `vmem_read_pages` / `PageCopyPool` | `serving/hide_fill.hpp:250`, `serving/vmem_prefetch.hpp` |
| Overlap queries | `--cont-batch T` scheduler | `serving/search_beam.cpp:2426` |
| Serialized one-query | T=1 `run_one_q` / hide_pqbeam recipe | `tools/run_10m_pq_nq100.sh` |

Do **not** revive `--pipe-drive`, `--oneshot-fp`, speculative neighbor prefetch, or change `pqq_beam` / `pqq_issue` / `pqq_rank` control flow. Observation hooks only.

Frozen T2I-10M recipe (from `docs/notes/2026-09-04-prefetcher-pq-freeze.md` and `tools/run_10m_pq_nq100.sh`):

```text
BIN=serving/search_beam
SRV=/mnt/disk0/chukexin_motivation/serving_t2i_10m
NAV=results/paper_figs/nav_10k.bin
GRAPH=$SRV/diskann_t2i_10m.graph.bin
PQ_PIV=/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin
PQ_CMP=/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin
LEN=20480004096
VMEM_OFF=1181116006400   # 1100 GiB extent image
HOST_BYTES=2GiB
--diskann-layout --pq-nav --beam 400 --k 10 --policy P3
--no-pipe-drive --extent-run --no-direct-install --no-score-cache
--shuffle-seed 42 --cpu-affinity --dram-backend numa --dram-numa 1
```

---

## File map

| File | Responsibility |
|---|---|
| Create: `serving/inflight_trace.hpp` | Ring-buffer samples + per-query fill records; no I/O policy. |
| Create: `serving/tests/test_inflight_trace.cpp` | Unit tests for sample, per-query record, CSV dump. |
| Modify: `serving/tests/Makefile` | Build/run the new unit test. |
| Modify: `serving/hide_fill.hpp` | Count active pages; optional sample hook on `issue`/`pump`. |
| Modify: `serving/cont_batch.hpp` | Expose `active_pages()` / `active_slots()`. |
| Modify: `serving/search_beam.cpp` | `--io-trace PATH`, per-query fill log, 1 ms sampler thread. |
| Create: `tools/run_c3_io_concurrency.sh` | Cold pollute, T-sweep, iostat sidecar, JSONL summary. |
| Create: `tools/plot_c3_io_concurrency.py` | Publication PDF/PNG from raw CSVs only. |
| Create: `results/motivation/c3_io_concurrency/README.md` | How to reproduce + metric definitions. |
| Produce: `results/motivation/c3_io_concurrency/raw/...` | Untouched run artifacts. |
| Produce: `paper/figs/mot-c3-io-concurrency.pdf` | Final Motivation figure. |
| Modify: `paper/sections/motivation.tex` + `fig_mot_bubbles.tex` | Replace schematic after data exist. |

---

## Metric contract (do not invent)

The paper y-axis is **effective queue depth**, not pages-per-query and not `%util`.

### Y-axis for Option A: `QD_eff(T)`

Primary (plot this):

- `QD_eff(T) = mean iostat aqu-sz` on the vmem backing NVMe(s), over **active** 0.1 s samples only (`rkB/s > 1e5`).
- Same `iostat -x 0.1` recipe at every T, so T=1 and T=16 are comparable.

`QDsat` (the horizontal line):

1. `fio` 4 KiB randread, `io_uring`, `direct=1`, same NVMe / index file.
2. Sweep `iodepth × numjobs` until IOPS / GB/s plateaus.
3. `QDsat` = mean `aqu-sz` of that saturated fio run, sampled the same way.
4. Also record `QDsat_iops` and `QDsat_gbs` for the result note. Do **not** draw a guessed `D=8`.

Why not plot `miss_n` or `sw_inflight_pages` as the paper y-axis: one query can collect 100–400 unique 4 KiB pages in one committed wave. That page count is the **work size**, not the **device queue**. vmem `READ_BATCH` / extent-run can merge pages into fewer NVMe commands, so page count would sit near or above `QDsat` at T=1 and destroy Option A. Keep page counts in the raw table to explain *why* a query cannot keep issuing: it has only one finite wave, then waits.

### Supporting Layer A — per-query wave size (table only)

Measured at `HidePipe` after residency filter:

- `cand_n`, `page_n`, `miss_n`, `issue_n`, `extent_extra_n`
- `sw_inflight_pages(t)`, `sw_inflight_slots(t)`, `q_wait(t)`, `fill_wait_ns`

### Supporting Layer B — bandwidth (table only)

- `read_GBs` from `rkB/s`; BW util = `read_GBs / QDsat_gbs`
- `%util` is busy time, never the paper y-axis

Limitation sentence for the note: `QD_eff` is device `aqu-sz`, the closest defensible outstanding-I/O measurement. It is not NAND-channel occupancy.

### Layer C — correctness (gate, not the y-axis)

- `recall@10` must match across T within 0.005 on the same query IDs.
- `score_from_window` dominates; bounce / Flash-on-score must be 0 or reject the hide run.
- `--dump-scored` hash of committed IDs must match T=1 vs T=16.

---

## Experiment matrix

Primary dataset: **T2I-10M**, frozen PQ-64, seed 42, k=10, L=400.

| ID | Mode | Flags | What it shows |
|---|---|---|---|
| T1 | Serial single query | `--threads 1 --shared-window`, no `--cont-batch` | `QD_eff(1)`: one committed wave, then wait. Must be ≪ `QDsat`. |
| T2 | Cont-batch T=2 | `--cont-batch 2 --cont-workers 2` | Two independent waves. |
| T4 | Cont-batch T=4 | `--cont-batch 4 --cont-workers 4` | |
| T8 | Cont-batch T=8 | `--cont-batch 8 --cont-workers 8` | |
| T16 | Cont-batch T=16 | `--cont-batch 16 --cont-workers 16` | Option A rightmost point. Still below `QDsat` is allowed and matches the mockup; above `QDsat` is also reportable. |

Do **not** put blocking memcpy / demand-fault on this figure. That is a different pathology and is not Option A.

Query counts:

- Correctness smoke: `nq=20`, T=1 and T=16.
- Stable device samples: `nq=500` for every T. If T=1 wall time > 15 min, use `NQ_T1=200` and `NQ_REST=500`, and say so.
- Do not use `nq=10` for iostat averages. Need ≥8 active samples per point.

Cache protocol: run `pollute()` from `tools/run_10m_pq_nq100.sh` before every timed pass so fills actually miss. Record `cache_used` and `evictions` before/after.

---

## Figure contract (Option A only)

Single 1-column scatter, PDF vector, white background, 8 pt labels, no in-axes title:

| Element | Locked value |
|---|---|
| X | Concurrent queries: 1, 2, 4, 8, 16 (linear or log2 ticks, but the mockup is linear labels) |
| Y | Effective queue depth = `QD_eff(T)` = mean active `aqu-sz` |
| Points | Five black filled circles, one per T |
| Optional | Thin line connecting the five points; whiskers = p10–p90 of active `aqu-sz` samples if it does not clutter |
| Reference | Horizontal dashed line `QDsat` near the top, labeled `QDsat` |
| Caption | **Per-query dependencies limit Flash I/O concurrency.** Then one sentence with measured `QD_eff(1)`, `QD_eff(16)`, `QDsat`, and iso-recall. |

The visual argument is: T=1 sits far below `QDsat`; adding independent queries raises `QD_eff`; even T=16 need not reach the line. If the measured shape is not monotonic, plot the truth. Do not reshape points to match the mockup.

Debug-only (never the Motivation figure): miss_n histogram, in-flight time series, GB/s vs T. Those go in `results/motivation/c3_io_concurrency/`.

Do **not** plot PipeANN `pipe_search` W-sweep here.

---

### Task 1: Lock the frozen binary and write the metric unit test

**Files:**
- Create: `serving/inflight_trace.hpp`
- Create: `serving/tests/test_inflight_trace.cpp`
- Modify: `serving/tests/Makefile`

- [ ] **Step 1: Confirm the frozen search files are the baseline**

```bash
cd /root/chukexin/CXL-ANNS-KX
git log -1 --oneline -- serving/search_beam.cpp serving/hide_fill.hpp serving/cont_batch.hpp serving/prefetch.hpp
test -x serving/search_beam || g++ -O3 -std=c++17 -march=native -pthread -I. serving/search_beam.cpp -o serving/search_beam -lnuma
ls -l /mnt/disk0/chukexin_motivation/serving_t2i_10m/query_10k.fbin \
      /mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin
cat /sys/class/vmem/vmem0/nvme_dev || true
```

Expected: binary builds; T2I artifacts exist; nvme_dev names the backing disk(s). If `/dev/vmem0` is missing, stop and report; do not simulate NAND with DRAM.

- [ ] **Step 2: Write the failing test for the trace helper**

```cpp
// serving/tests/test_inflight_trace.cpp
#include "serving/inflight_trace.hpp"
#include <cassert>
#include <cstdio>
#include <string>

int main() {
  InflightTrace tr;
  tr.max_samples = 8;
  tr.note_query_fill(/*qi=*/3, /*cand=*/400, /*pages=*/210, /*miss=*/180,
                     /*issued=*/192, /*extent_extra=*/12, /*wait_ns=*/12345);
  assert(tr.fills.size() == 1);
  assert(tr.fills[0].qi == 3);
  assert(tr.fills[0].miss_n == 180);
  assert(tr.fills[0].issue_n == 192);

  tr.sample(/*t_ns=*/1000, /*sw_pages=*/180, /*sw_slots=*/1, /*q_wait=*/1);
  tr.sample(/*t_ns=*/2000, /*sw_pages=*/40, /*sw_slots=*/1, /*q_wait=*/1);
  assert(tr.samples.size() == 2);
  assert(tr.peak_sw_pages() == 180);

  char path[] = "/tmp/c3_trace_test.csv";
  assert(tr.dump_csv(path));
  FILE* f = fopen(path, "r");
  assert(f);
  char buf[256];
  assert(fgets(buf, sizeof(buf), f));  // header
  fclose(f);
  std::remove(path);
  printf("test_inflight_trace ok\n");
  return 0;
}
```

- [ ] **Step 3: Run the test and confirm it fails to compile**

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests
g++ -O0 -g -std=c++17 -I../.. -o test_inflight_trace test_inflight_trace.cpp
```

Expected: `fatal error: serving/inflight_trace.hpp: No such file or directory`

- [ ] **Step 4: Implement the helper**

```cpp
// serving/inflight_trace.hpp
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

struct FillRec {
  uint32_t qi = 0;
  uint32_t cand_n = 0;
  uint32_t page_n = 0;
  uint32_t miss_n = 0;
  uint32_t issue_n = 0;
  uint32_t extent_extra_n = 0;
  uint64_t fill_wait_ns = 0;
};

struct InflightSample {
  uint64_t t_ns = 0;
  uint32_t sw_pages = 0;
  uint32_t sw_slots = 0;
  uint32_t q_wait = 0;
};

struct InflightTrace {
  uint32_t max_samples = 200000;
  std::vector<FillRec> fills;
  std::vector<InflightSample> samples;

  void note_query_fill(uint32_t qi, uint32_t cand, uint32_t pages, uint32_t miss,
                       uint32_t issued, uint32_t extra, uint64_t wait_ns) {
    fills.push_back(FillRec{qi, cand, pages, miss, issued, extra, wait_ns});
  }

  void sample(uint64_t t_ns, uint32_t sw_pages, uint32_t sw_slots, uint32_t q_wait) {
    if (samples.size() >= max_samples) return;
    samples.push_back(InflightSample{t_ns, sw_pages, sw_slots, q_wait});
  }

  uint32_t peak_sw_pages() const {
    uint32_t p = 0;
    for (const auto& s : samples) p = std::max(p, s.sw_pages);
    return p;
  }

  bool dump_csv(const char* path) const {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "kind,qi,t_ns,cand_n,page_n,miss_n,issue_n,extent_extra_n,fill_wait_ns,sw_pages,sw_slots,q_wait\n");
    for (const auto& r : fills) {
      fprintf(f, "fill,%u,0,%u,%u,%u,%u,%u,%llu,0,0,0\n", r.qi, r.cand_n, r.page_n,
              r.miss_n, r.issue_n, r.extent_extra_n, (unsigned long long)r.fill_wait_ns);
    }
    for (const auto& s : samples) {
      fprintf(f, "sample,0,%llu,0,0,0,0,0,0,%u,%u,%u\n", (unsigned long long)s.t_ns,
              s.sw_pages, s.sw_slots, s.q_wait);
    }
    fclose(f);
    return true;
  }
};
```

- [ ] **Step 5: Hook the test into the Makefile and run it**

Add `test_inflight_trace` to the `test:` recipe and the binary list in `serving/tests/Makefile`, then:

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests && make test
```

Expected: `test_inflight_trace ok` and all existing tests still pass.

- [ ] **Step 6: Commit**

```bash
git add serving/inflight_trace.hpp serving/tests/test_inflight_trace.cpp serving/tests/Makefile
git commit -m "$(cat <<'EOF'
Add inflight-trace helper for the C3 I/O concurrency motivation.

EOF
)"
```

---

### Task 2: Observation hooks on HidePipe (no policy change)

**Files:**
- Modify: `serving/hide_fill.hpp`
- Modify: `serving/cont_batch.hpp`
- Modify: `serving/tests/test_extent_run.cpp` only if a signature change breaks it (prefer not).

- [ ] **Step 1: Add page/slot counters on `HidePipe`**

In `serving/hide_fill.hpp`, add two const methods next to `any` slot walk (`page_in_slot`):

```cpp
  uint32_t active_slots() const {
    uint32_t n = 0;
    for (int s = 0; s < kSlots; ++s)
      if (slot[s].active) n++;
    return n;
  }

  uint32_t active_pages() const {
    uint32_t n = 0;
    for (int s = 0; s < kSlots; ++s)
      if (slot[s].active) n += (uint32_t)slot[s].pages.size();
    return n;
  }

  // Optional hook: called after issue/pump. Must be cheap when null.
  std::function<void()> on_inflight;
```

Call `if (on_inflight) on_inflight();` at the end of `HidePipe::issue` (after jobs are submitted) and at the end of `HidePipe::pump`. Do not change miss filtering, extent_run, or ioctl chunking.

- [ ] **Step 2: Mirror counters on `PrefetchHub`**

In `serving/cont_batch.hpp`:

```cpp
  uint32_t active_pages() {
    std::lock_guard<std::mutex> g(mu);
    return pipe.active_pages();
  }
  uint32_t active_slots() {
    std::lock_guard<std::mutex> g(mu);
    return pipe.active_slots();
  }
```

- [ ] **Step 3: Rebuild unit tests that include hide_fill**

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests && make test
```

Expected: PASS. If `test_extent_run` fails, you changed issue behavior — revert policy code.

- [ ] **Step 4: Commit**

```bash
git add serving/hide_fill.hpp serving/cont_batch.hpp
git commit -m "$(cat <<'EOF'
Expose HidePipe in-flight page counts for C3 tracing.

EOF
)"
```

---

### Task 3: Wire `--io-trace` into `search_beam` without changing candidates

**Files:**
- Modify: `serving/search_beam.cpp`

- [ ] **Step 1: Add CLI and a process-wide trace**

Near other flags (`--dump-scored`):

```cpp
const char* io_trace_path = nullptr;
// ...
else if (a == "--io-trace") io_trace_path = need(a.c_str());
```

Create a `static InflightTrace g_io_trace;` and `static std::atomic<uint64_t> g_trace_t0_ns{0};`. Enable only when `io_trace_path` is set.

- [ ] **Step 2: Record each committed wave in `pqq_issue`**

After `hide_collect_vec_pages` fills `q.need`, and after `hub.submit` / `submit_block`, if tracing:

```cpp
// cand_n = q.cand.size()
// page_n = unique pages in q.need before submit
// miss_n / issue_n: read back from hub.pipe last issue, OR compute by
//   counting pages that !win.is_resident before submit (same predicate HidePipe uses)
```

Do **not** change which pages are collected. If `extent_run` adds pages inside `hide_start`, record `issue_n` from `HideInflight::pages.size()` of the new token's slot, and `extent_extra_n = issue_n - miss_n`.

Also record `fill_wait_ns` when `pqq_rank` first succeeds, or from existing `wait_covering` return in the serial T=1 path.

- [ ] **Step 3: 1 ms sampler thread during the timed region**

Start after cache pollute / before `t0`; join after workers finish, before summary printf.

```cpp
std::atomic<bool> stop{false};
std::thread samp([&] {
  while (!stop.load(std::memory_order_relaxed)) {
    uint64_t t = now_ns() - t0;
    uint32_t pages = 0, slots = 0, qwait = 0;
    // serial T=1: read win/hpipe
    // cont-batch: hub.active_pages/slots + count slots in CbSt::Wait
    g_io_trace.sample(t, pages, slots, qwait);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
});
```

For T=1 serial, attach `hpipe.on_inflight` as well so issue/pump edges are not only 1 ms quantized.

- [ ] **Step 4: Dump CSV and print a one-line summary**

```text
io_trace path=... fills=N peak_sw_pages=P mean_miss=M mean_issue=I
```

Dump `${io_trace_path}.csv` via `InflightTrace::dump_csv`.

- [ ] **Step 5: Rebuild and smoke `--help`/unknown-flag path**

```bash
cd /root/chukexin/CXL-ANNS-KX
g++ -O3 -std=c++17 -march=native -pthread -I. serving/search_beam.cpp -o serving/search_beam -lnuma
./serving/search_beam --io-trace /tmp/x --not-a-flag ; echo exit:$?
```

Expected: `unknown --not-a-flag`, exit 2. `--io-trace` is accepted.

- [ ] **Step 6: Commit**

```bash
git add serving/search_beam.cpp
git commit -m "$(cat <<'EOF'
Add --io-trace sampling for committed-page fill waves.

EOF
)"
```

---

### Task 4: Driver, fio ceiling, and correctness gates

**Files:**
- Create: `tools/run_c3_io_concurrency.sh`
- Create: `results/motivation/c3_io_concurrency/README.md`

- [ ] **Step 1: Write the driver**

The script must:

1. Resolve NVMe name from `/sys/class/vmem/vmem0/nvme_dev`.
2. Run a 20 s `fio` 4k randread ceiling onto that device or the T2I image file (`io_uring`, `direct=1`, `bs=4k`, try `numjobs=8 iodepth=128` first; if IOPS < previous DiskANN 4k ceiling ~1.7M, try `numjobs=16`). Save `fio_ceiling.json`.
3. For each mode in `T1 T2 T4 T8 T16`:
   - `pollute` vmem cache.
   - Start `iostat -x 0.1` on the NVMe, PID in a file.
   - Run `numactl --cpunodebind=0 --membind=0 serving/search_beam` with the frozen flags plus `--io-trace $OUT/$mode` and the T flags.
   - Kill iostat; keep the raw log.
4. Parse search_beam stdout for QPS, recall@10, `from_win`, `nvme_read_B`.
5. Write `summary.jsonl` with one object per T: flags, git SHA, nq, recall, QPS, mean/p50 miss_n, issue_n, `QD_eff` (mean active aqu-sz), peak aqu-sz, `QDsat`, mean/peak GB/s, cache_used before/after.

Serial T1 flags: copy `hide_pqbeam_run` from `tools/run_10m_pq_nq100.sh` (`--threads 1`, no `--cont-batch`).

Cont-batch flags: same artifacts plus:

```bash
--cont-batch "$T" --cont-workers "$T" --shared-window --policy P3 --pq-nav ...
```

Do not pass `--oneshot-fp`. Keep `--extent-run` because it is frozen; record extras.

- [ ] **Step 2: Write a validator function in the same script or `tools/check_c3_io_concurrency.py`**

Reject a run when:

- `recall@10` missing or `|recall_T - recall_T1| > 0.005`
- `fills` count != `nq`
- T1 `mean(cand_n)` not in `[350, 400]` (beam=400; early stop can be slightly below)
- T1 `mean(miss_n) == 0` after pollute (cache was hot; rerun)
- `from_win` fraction < 0.99 on hide path
- iostat file has fewer than 8 active samples (`rkB/s > 1e5`)

- [ ] **Step 3: nq=20 correctness on T=1 and T=16**

```bash
NQ=20 tools/run_c3_io_concurrency.sh --smoke
```

Expected: both recalls ~0.92 (frozen hide is 0.923–0.925 on nq=20/100); T=1 mean miss_n ≫ 0; T=16 recall matches T=1; script writes raw files under `results/motivation/c3_io_concurrency/raw/`.

- [ ] **Step 4: Commit scripts only (no fabricated summaries)**

```bash
git add tools/run_c3_io_concurrency.sh results/motivation/c3_io_concurrency/README.md
git commit -m "$(cat <<'EOF'
Add C3 I/O-concurrency experiment driver and metric gates.

EOF
)"
```

---

### Task 5: Full T-sweep, raw artifacts, and claim check

**Files:**
- Produce: `results/motivation/c3_io_concurrency/raw/<timestamp>/`
- Produce: `results/motivation/c3_io_concurrency/c3_io_concurrency.md`

- [ ] **Step 1: Run the full matrix**

```bash
NQ=500 tools/run_c3_io_concurrency.sh
```

If T=1 is too long, set `NQ_T1=200 NQ_REST=500`. Do not kill a run and invent averages.

- [ ] **Step 2: Read the JSONL and decide whether the claim holds**

The experiment **supports Option A / C3** only if all of the following are true:

1. T=1 `QD_eff` is far below `QDsat` (state the ratio; a useful bar is `< 0.35 × QDsat`, but report the number even if the bar is missed).
2. `QD_eff(T)` rises as T goes 1 → 2 → 4 → 8 → 16, at iso-recall. A later knee is fine.
3. T=1 `median(miss_n)` is a finite committed wave (tens to a few hundred pages), then the query waits. This is the mechanism behind the low T=1 point, not the plotted y-value.
4. Committed candidates / recall do not change with T.

The experiment **fails the claim** if `QD_eff(1) ≥ 0.90 × QDsat`, or if adding queries does not raise `QD_eff`. Write the negative result. Do not move the `QDsat` line or switch y-axis to `%util` / page count to rescue the mockup.

- [ ] **Step 3: Write `c3_io_concurrency.md` from the files, not from memory**

Include: setup, fio/`QDsat` table, per-T table (`T, nq, recall, QPS, mean miss, mean issue, QD_eff, peak aqu-sz, GB/s`), limitation paragraph (`aqu-sz` vs NAND channels vs page count), and a yes/no on Option A.

- [ ] **Step 4: Commit results + note**

```bash
git add results/motivation/c3_io_concurrency
git commit -m "$(cat <<'EOF'
Record measured C3 per-query I/O concurrency results.

EOF
)"
```

---

### Task 6: Publication figure and Motivation swap

**Files:**
- Create: `tools/plot_c3_io_concurrency.py`
- Produce: `paper/figs/mot-c3-io-concurrency.pdf`
- Modify: `paper/sections/fig_mot_bubbles.tex`
- Modify: `paper/sections/motivation.tex`

- [ ] **Step 1: Plot only from raw CSV/JSONL**

```python
# tools/plot_c3_io_concurrency.py
# Usage: python3 tools/plot_c3_io_concurrency.py results/motivation/c3_io_concurrency/raw/<run>
# Reads: */io_trace.csv, iostat logs, summary.jsonl, fio_ceiling.json
# Writes: paper/figs/mot-c3-io-concurrency.pdf and .png
```

Style: match Option A. White background, 8 pt labels, black circles, no in-axes title. Draw `QDsat` as a dashed horizontal line labeled `QDsat`. X label `Concurrent queries`. Y label `Effective queue depth`. Caption starts with **Per-query dependencies limit Flash I/O concurrency.**

- [ ] **Step 2: Replace the schematic include**

In `paper/sections/motivation.tex` `\begin{figure}` for `fig:mot-bubbles`:

- `\includegraphics{figs/mot-c3-io-concurrency.pdf}` instead of `\input{sections/fig_mot_bubbles}`.
- Caption: measured `QD_eff` at T=1 and T=16, measured `QDsat`, iso-recall. Delete “schematic”.
- Keep the systems argument: one committed wave cannot hold `QDsat`; independent queries raise effective QD.

Keep `fig_mot_bubbles.tex` in tree as historical, or add a one-line comment that it is superseded.

- [ ] **Step 3: Build the paper PDF if `latexmk` is available**

```bash
cd /root/chukexin/CXL-ANNS-KX/paper && make
```

Expected: figure renders; no unresolved refs.

- [ ] **Step 4: Commit**

```bash
git add tools/plot_c3_io_concurrency.py paper/figs/mot-c3-io-concurrency.pdf \
        paper/sections/motivation.tex paper/sections/fig_mot_bubbles.tex
git commit -m "$(cat <<'EOF'
Replace C3 schematic with measured per-query I/O concurrency.

EOF
)"
```

---

## Implementation constraints

- Do not modify PQ beam, candidate insertion, or rerank IDs to “create more I/O”.
- Do not use PipeANN bubble numbers in the new figure.
- Do not call `iostat %util` bandwidth saturation.
- Do not overwrite `results/pipeann_bubbles_continuous_batching.md`.
- If a metric cannot be measured (e.g. true NAND channel occupancy), say so and use `aqu-sz` + fio GB/s.
- Prefetcher freeze remains: `--no-pipe-drive`, PQ-64, end-batch only.

---

## Self-review

**Spec coverage**

| Requirement | Task |
|---|---|
| Inspect codebase / Flash-fill path | Task 1 + architecture section |
| Instrumentation | Tasks 1–3 |
| Experiment driver | Task 4 |
| Debug / correctness | Task 4 smoke + reject gates |
| Run + raw results | Task 5 |
| Plot + final figure | Task 6 |
| Summarize support for claim | Task 5 note + Task 6 caption |
| No fabricated data | gates + “fails the claim” branch |
| Replace schematic Motivation figure | Task 6 |

**Placeholder scan:** none. Commands, paths, structs, and reject rules are concrete.

**Type consistency:** `InflightTrace`, `FillRec`, `InflightSample`, `--io-trace`, `QD_eff`, `QDsat`, `active_pages()`, modes `T1/T2/T4/T8/T16` are used with the same names throughout.

---

## What the executing agent must report at the end

1. Files changed.
2. Fio ceiling (GB/s, IOPS, aqu-sz).
3. Per-T `QD_eff`, plus T=1 `cand_n / page_n / miss_n / issue_n`.
4. Measured `QDsat` and `QD_eff(1) / QDsat`.
5. Recall@10 per T.
6. Whether Option A is supported, with the limitation sentence: plotted y is device `aqu-sz`, not page count and not NAND-channel occupancy.
