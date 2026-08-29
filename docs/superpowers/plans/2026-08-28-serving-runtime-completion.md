# Serving Runtime Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the Route A serving runtime that the paper can name: placement, bounded promote (already P3), a real cross-query hot set, iso-recall selection, ablation knobs, and counters that match the Eval tables.

**Architecture:** Keep `serving/search_beam` as the system. Do not merge PipeANN. Do not add Shared FAM, device-side 160 GB/s, or an F9 miss-chop controller. New code is small headers plus flags on the existing beam loop. PipeANN `cont_search` stays a block-path / mmap contrast, not the product binary.

**Tech Stack:** C++17, existing `serving/*.hpp`, `g++ -O2 -pthread -lnuma`, bash + python3 for harnesses. Unit tests need no `/dev/vmem0`.

**Out of scope (explicit):** unifying PipeANN into CXAN; multi-tenant QoS; kernel PTE vs soft-cache counters on the serving path (keep F8 in `motivation_exps`); product CXL.mem BAR.

---

## Current baseline (do not regress)

Working path today:

```text
serving/search_beam --policy P3 --oneshot-fp --budget 64MiB \
  --pipe-w 4 --install-top 4 --fetch-top 0
```

On Text2Image-10M this is ~15× P0 at iso-recall ≈ 0.93 (`results/t2i10m_contbatch_cxl_dram_ssd_bw.md`). Pagebin layout is a data transform (`tools/pack_layout_pagebin.cpp`), not a runtime flag.

Gaps this plan closes:

| Gap | Plan module | What “done” means |
|-----|-------------|-------------------|
| Graph always on `ssd_base` | D1 | `--graph-in-dram` copies adjacency to host; E1 can A/B |
| No `used/promoted` | D2 / D5 | CSV prints `precision_pct` |
| No cross-query LFU set | D3 | `--hotset-bytes` pins frequent IDs; `--no-hotset` ablates |
| No recall controller | D4 | harness picks min `L` at a recall floor; optional `--early-stop` |
| Cannot leave-one-out | E7 | one flag per contributed module |

---

## File map

| File | Responsibility |
|------|----------------|
| Create: `serving/hot_set.hpp` | Cross-query LFU+aging; no I/O |
| Create: `serving/tests/test_hot_set.cpp` | Hot-set unit tests |
| Create: `serving/tests/test_placement.cpp` | `nbrs()` host vs SSD base |
| Create: `serving/tests/test_metrics.cpp` | Precision math |
| Create: `serving/tests/Makefile` | `make -C serving/tests` |
| Create: `tools/iso_recall_pick.sh` | Sweep `L`, pick min meeting floor |
| Create: `tools/run_serving_ablation.sh` | E7 leave-one-out |
| Modify: `serving/metrics.hpp` | `promote_pages`, `promote_used`, `hotset_pins` |
| Modify: `serving/placement.hpp` | `graph_host` override for `nbrs()` |
| Modify: `serving/dram_window.hpp` | `pin_vec_ids()` helper used by hot set |
| Modify: `serving/search_beam.cpp` | Flags, access recording, early-stop, CSV |
| Modify: `serving/prefetch.hpp` | Only if P3 must notify precision (prefer search_beam) |

Do not edit `third_party/PipeANN/**` in this plan.

---

### Task 1: Precision counters in Metrics

**Files:**
- Modify: `serving/metrics.hpp`
- Create: `serving/tests/test_metrics.cpp`
- Create: `serving/tests/Makefile`

- [ ] **Step 1: Write the failing test**

Create `serving/tests/test_metrics.cpp`:

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
  Metrics a, b;
  a.note_promote_pages(4);
  a.note_promote_used(1);
  b.note_promote_pages(6);
  b.note_promote_used(5);
  a.add_from(b);
  assert(a.promote_pages == 10);
  assert(a.promote_used == 6);
  std::puts("test_metrics OK");
  return 0;
}
```

Create `serving/tests/Makefile`:

```makefile
CXX ?= g++
CXXFLAGS ?= -O0 -g -std=c++17 -I../..
.PHONY: test
test: test_metrics
	./test_metrics
test_metrics: test_metrics.cpp ../../serving/metrics.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_metrics.cpp
clean:
	rm -f test_metrics test_hot_set test_placement
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests && make test_metrics
```

Expected: compile error (`note_promote_pages` / `precision_pct` missing).

- [ ] **Step 3: Extend Metrics**

In `serving/metrics.hpp`, add fields and helpers next to the existing counters:

```cpp
uint64_t promote_pages = 0;  // pages issued by P3 fetch or demand promote
uint64_t promote_used = 0;   // of those, later used by a score/copy
uint64_t hotset_pins = 0;

void note_promote_pages(uint64_t n) { promote_pages += n; }
void note_promote_used(uint64_t n) { promote_used += n; }

double precision_pct() const {
  return promote_pages ? 100.0 * (double)promote_used / (double)promote_pages : 0.0;
}
```

Add the three fields to `add_from` and print `precision_pct=%.1f promote_pages=%llu promote_used=%llu hotset_pins=%llu` in `print()`.

- [ ] **Step 4: Run test to verify it passes**

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests && make test
```

Expected: `test_metrics OK`.

- [ ] **Step 5: Commit** (only if the user asked to commit)

```bash
git add serving/metrics.hpp serving/tests/test_metrics.cpp serving/tests/Makefile
git commit -m "feat: track promote precision in serving metrics"
```

---

### Task 2: Graph-in-DRAM placement (E1 knob)

**Files:**
- Modify: `serving/placement.hpp`
- Create: `serving/tests/test_placement.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
#include "serving/placement.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdio>

int main() {
  CxanLayoutHeader h{};
  h.magic = kCxanMagic;
  h.n = 4;
  h.dim = 2;
  h.R = 2;
  h.vec_bytes = 4;
  h.pq_bytes = 1;
  h.off_graph = 64;
  h.len_graph = 4 * 2 * 4;
  h.off_vectors = 128;
  h.len_vectors = 4 * 8;

  std::vector<uint8_t> img(256, 0);
  std::memcpy(img.data(), &h, sizeof(h));
  uint32_t graph_ssd[8] = {1, 2, 0, 3, 0, 1, 2, 1};
  std::memcpy(img.data() + 64, graph_ssd, sizeof(graph_ssd));

  Placement p;
  p.set_header(reinterpret_cast<const CxanLayoutHeader*>(img.data()));
  p.ssd_base = img.data();
  assert(p.nbrs(1)[1] == 3);

  std::vector<uint8_t> host(h.len_graph);
  std::memcpy(host.data(), img.data() + 64, h.len_graph);
  uint32_t* gh = reinterpret_cast<uint32_t*>(host.data());
  gh[3] = 99;  // mutate host copy only
  p.set_graph_host(host.data(), host.size());
  assert(p.nbrs(1)[1] == 99);
  assert(reinterpret_cast<const uint32_t*>(img.data() + 64)[3] == 3);
  std::puts("test_placement OK");
  return 0;
}
```

Add to `serving/tests/Makefile`:

```makefile
test: test_metrics test_placement
	./test_metrics
	./test_placement
test_placement: test_placement.cpp ../../serving/placement.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_placement.cpp
```

- [ ] **Step 2: Run to see it fail**

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests && make test_placement
```

Expected: compile error (`set_graph_host` missing).

- [ ] **Step 3: Implement host graph override**

In `serving/placement.hpp`, add members and change `nbrs()`:

```cpp
const uint8_t* graph_host = nullptr;
size_t graph_host_bytes = 0;

void set_graph_host(const uint8_t* p, size_t n) {
  graph_host = p;
  graph_host_bytes = n;
}

const uint32_t* nbrs(uint32_t id) const {
  const uint8_t* base = graph_host ? graph_host : (ssd_base + hdr->off_graph);
  return reinterpret_cast<const uint32_t*>(base + (size_t)id * hdr->R * 4);
}
```

Leave `pq()` and `vec()` on `ssd_base`. Do not copy vectors in this task.

- [ ] **Step 4: Pass the test**

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests && make test
```

Expected: both tests print `OK`.

---

### Task 3: HotSet (D3) as a pure structure

**Files:**
- Create: `serving/hot_set.hpp`
- Create: `serving/tests/test_hot_set.cpp`

Semantics (lock these; search_beam must not invent another policy):

1. `on_touch(id)` increments frequency and sets `last_q = queries`.
2. `on_query_end()` increments `queries`; every `aging_period` queries, `freq >>= 1` and drop `freq==0`.
3. `top_ids(nbytes, vec_bytes)` returns IDs in freq desc, last_q desc, until `count * vec_bytes >= nbytes`.
4. Budget 0 ⇒ empty. Unknown id is ignored.

- [ ] **Step 1: Failing test**

```cpp
#include "serving/hot_set.hpp"
#include <cassert>
#include <cstdio>

int main() {
  HotSet hs;
  hs.aging_period = 2;
  hs.on_touch(7);
  hs.on_touch(7);
  hs.on_touch(3);
  hs.on_query_end();
  auto a = hs.top_ids(16, 8);  // 2 vectors
  assert(a.size() == 2);
  assert(a[0] == 7);
  assert(a[1] == 3);

  HotSet empty;
  assert(empty.top_ids(1 << 20, 8).empty());

  HotSet age;
  age.aging_period = 1;
  age.on_touch(1);
  age.on_query_end();  // freq 1 -> 0, drop
  assert(age.top_ids(64, 8).empty());

  std::puts("test_hot_set OK");
  return 0;
}
```

- [ ] **Step 2: Run — must fail to compile**

- [ ] **Step 3: Implement `serving/hot_set.hpp`**

```cpp
#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct HotSet {
  uint32_t aging_period = 32;
  uint32_t queries = 0;

  struct Ent {
    uint32_t freq = 0;
    uint32_t last_q = 0;
  };
  std::unordered_map<uint32_t, Ent> tab;

  void on_touch(uint32_t id) {
    Ent& e = tab[id];
    e.freq++;
    e.last_q = queries;
  }

  void on_query_end() {
    queries++;
    if (!aging_period || (queries % aging_period) != 0) return;
    for (auto it = tab.begin(); it != tab.end();) {
      it->second.freq >>= 1;
      if (it->second.freq == 0) it = tab.erase(it);
      else ++it;
    }
  }

  std::vector<uint32_t> top_ids(size_t nbytes, size_t vec_bytes) const {
    std::vector<uint32_t> out;
    if (nbytes == 0 || vec_bytes == 0) return out;
    std::vector<std::pair<Ent, uint32_t>> rows;
    rows.reserve(tab.size());
    for (const auto& kv : tab) rows.push_back({kv.second, kv.first});
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      if (a.first.freq != b.first.freq) return a.first.freq > b.first.freq;
      return a.first.last_q > b.first.last_q;
    });
    size_t used = 0;
    for (const auto& r : rows) {
      if (used + vec_bytes > nbytes) break;
      out.push_back(r.second);
      used += vec_bytes;
    }
    return out;
  }
};
```

- [ ] **Step 4: `make test` includes `test_hot_set` and all three pass**

---

### Task 4: Pin helper on DramWindow

**Files:**
- Modify: `serving/dram_window.hpp`

- [ ] **Step 1: Add `pin_vec_ids` after existing `pin()`**

```cpp
template <typename PlacementT>
void pin_vec_ids(const PlacementT& p, const std::vector<uint32_t>& ids, Metrics* m) {
  size_t vb = p.packed_vec_bytes();
  for (uint32_t id : ids) {
    if (!p.hdr || id >= p.hdr->n) continue;
    pin(p.ssd_base, p.vec(id), vb);
    if (m) m->hotset_pins++;
  }
}
```

No new unit test (needs a mapped arena). Smoke-tested in Task 5.

---

### Task 5: Wire D3 + D1 + precision + early-stop into `search_beam`

**Files:**
- Modify: `serving/search_beam.cpp`

CLI (add next to existing `--policy` parsing; unknown flags must still error):

| Flag | Default | Meaning |
|------|---------|---------|
| `--graph-in-dram` | off | copy `off_graph..len_graph` to `std::vector<uint8_t>` and `set_graph_host` |
| `--hotset-bytes N` | `0` | 0 = off; else pin `top_ids` after each query (skip if `--flush-window`) |
| `--hotset-aging N` | `32` | `HotSet::aging_period` |
| `--no-hotset` | | force `hotset-bytes=0` |
| `--early-stop-patience N` | `0` | 0 = off; stop when k-th cand dist has not improved for N expands |
| `--early-stop-eps F` | `1e-6` | improvement threshold (MIPS uses more-negative-is-better: treat as `fabs`) |
| `--no-promote` | | force `--policy P0` (E7 alias) |

- [ ] **Step 1: After header map, optional graph copy**

```cpp
std::vector<uint8_t> graph_host_buf;
if (graph_in_dram) {
  graph_host_buf.assign(pl.ssd_base + hdr->off_graph,
                        pl.ssd_base + hdr->off_graph + hdr->len_graph);
  pl.set_graph_host(graph_host_buf.data(), graph_host_buf.size());
  printf("graph_in_dram bytes=%zu\n", graph_host_buf.size());
}
```

- [ ] **Step 2: Hot set lifecycle**

Construct one `HotSet` in `main` (shared across queries; **not** per-thread unless `--shared-window` — first version: single-thread only, document `--threads>1` + hotset as unsupported).

In the oneshot / P3 expand loop, when an id is **scored** (real distance, not just fetched), call `hs.on_touch(id)`.

After each query, `hs.on_query_end()`. If `hotset_bytes > 0 && !flush_window`, `win.pin_vec_ids(pl, hs.top_ids(hotset_bytes, pl.packed_vec_bytes()), &metrics)`.

If `flush_window`, do **not** pin from the hot set (cold protocol must stay cold). Still update frequencies so a later warm run in the same process could use them; the paper cold scripts always use `--flush-window` and `--no-hotset`.

- [ ] **Step 3: Precision**

When P3 builds `fetch` (the miss page list), `metrics.note_promote_pages(fetch.size())`.

When `assemble_from_host` or demand `score_id` first uses a page that was in `fetch_set` this hop, `metrics.note_promote_used(1)` once per page per query (keep a `unordered_set<uint64_t> used_pages` per query).

Demand-only P0: `note_promote_pages` on each `lookup_or_promote` miss is already `ssd_misses`; also increment `promote_pages` there so P0 precision is ~100% (used immediately). Do this in `search_beam` after a miss score, not by changing every `DramWindow` call site if that is faster — either is fine if CSV is consistent.

- [ ] **Step 4: Early-stop**

In the expand loop, track `best_k` = k-th candidate distance after each expand. If `patience>0` and `fabs(best_k - prev_best_k) < eps` for `patience` consecutive expands, `break`. Expand order before the break is unchanged (iso-path prefix). Print `early_stop_hops_avg` in the summary.

- [ ] **Step 5: CSV**

Extend the existing `CSV,` line — **append** columns, do not reorder the first 15 (scripts already parse them):

```text
...,precision_pct,promote_pages,promote_used,hotset_pins,graph_in_dram,hotset_bytes,early_stop_patience
```

- [ ] **Step 6: Smoke (needs device + T2I env)**

```bash
source /mnt/disk0/chukexin_motivation/serving_t2i_10m/serve_vmem.env
g++ -O2 -std=c++17 -pthread -I/root/chukexin/CXL-ANNS-KX \
  /root/chukexin/CXL-ANNS-KX/serving/search_beam.cpp \
  -o /root/chukexin/CXL-ANNS-KX/serving/search_beam -lnuma
numactl --cpunodebind=0 --membind=0 /root/chukexin/CXL-ANNS-KX/serving/search_beam \
  --vmem-dev "$CXAN_VMEM_DEV" --vmem-offset "$CXAN_SSD_OFFSET" --vmem-len "$CXAN_LAYOUT_LEN" \
  --entry "$CXAN_ENTRY" --queries "$CXAN_QUERIES" --gt "$CXAN_GT" \
  --dram-backend numa --dram-bytes "$CXAN_DRAM_BYTES" --host-cap $((32<<20)) \
  --policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 \
  --oneshot-fp --beam 300 --k 10 --iters 0 --flush-window --shuffle-seed 42 --max-q 20 \
  --no-hotset
```

Expected: process exits 0, `recall@10` prints, `CSV,` has the new columns, QPS is the same order as historical P3 (~10+, not P0 ~1).

---

### Task 6: Iso-recall picker (D4 harness)

**Files:**
- Create: `tools/iso_recall_pick.sh`

This **is** the M4 controller for the paper: pick the smallest `L` whose recall meets the floor. Do not implement F9.

```bash
#!/usr/bin/env bash
# Usage: iso_recall_pick.sh --floor 0.92 --beams 150,200,300,400 -- <search_beam args...>
set -euo pipefail
FLOOR=0.92
BEAMS=150,200,300,400
while [[ $# -gt 0 ]]; do
  case $1 in
    --floor) FLOOR=$2; shift 2 ;;
    --beams) BEAMS=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "unknown $1"; exit 2 ;;
  esac
done
BEST=""
IFS=',' read -ra LS <<< "$BEAMS"
for L in "${LS[@]}"; do
  echo "=== try L=$L floor=$FLOOR ==="
  set +e
  out=$("$@" --beam "$L" --iters 0 2>&1)
  ec=$?
  set -e
  echo "$out" | tail -n 20
  [[ $ec -eq 0 ]] || exit $ec
  rec=$(echo "$out" | awk -F= '/^recall@/{print $2; exit}')
  if awk -v r="$rec" -v t="$FLOOR" 'BEGIN{exit !(r+0>=t)}'; then
    BEST=$L
    echo "PICKED L=$L recall=$rec"
    echo "$out" | grep '^CSV,'
    exit 0
  fi
done
echo "NO_BEAM_MET_FLOOR $FLOOR" >&2
exit 1
```

- [ ] **Step 1: `chmod +x tools/iso_recall_pick.sh`**
- [ ] **Step 2: Dry-run help path** (`./tools/iso_recall_pick.sh` without `--` must exit 2)
- [ ] **Step 3: On T2I, pick L for P0 and P3 at floor 0.92, `--max-q 50`.** Expected: both pick some L; P3 QPS > P0 QPS at the picked points.

---

### Task 7: Ablation runner (E7)

**Files:**
- Create: `tools/run_serving_ablation.sh`

Five rows, same `--beam` (the L from Task 6 on full P3), same seed, `--flush-window`, `--max-q 100`, T2I packed layout unless `PAGEBIN=1`:

| Row | Flags |
|-----|--------|
| full | P3 + pagebin env if `PAGEBIN=1` + `--hotset-bytes 0` under flush (hot set needs a **second** warm table, see test plan) |
| `-promote` | `--policy P0` |
| `-async/cont` | P3 + `--no-vmem-prefetch` + `--pipe-w 1` |
| `-pagebin` | packed env (not pagebin) |
| `-early-stop` | full without `--early-stop-patience` (baseline) vs with |

Cold ablation cannot show D3. Warm ablation (no `--flush-window`, `--hotset-bytes $((64<<20))`) is a separate table in the test plan.

Script writes `results/ablation_t2i.csv` by grepping `CSV,`.

Acceptance: `-promote` QPS is ≤ 1/5 of full on T2I cold (historical ~1.2 vs ~17). If that fails, stop and do not invent a new promote policy.

---

## Definition of done (implementation)

1. `make -C serving/tests test` passes with no device.
2. `search_beam --help`-adjacent unknown flags still error; new flags parse.
3. T2I `--max-q 20` P3 smoke exits 0.
4. CSV contains precision and hotset columns.
5. `--graph-in-dram` prints `graph_in_dram bytes=` and still searches.
6. PipeANN tree is untouched.

---

## Execution order

Task 1 → 2 → 3 → 4 → 5 → 6 → 7. Do not start Task 5 before 1–3 tests pass.

After this plan: run `docs/superpowers/plans/2026-08-28-serving-eval-test-plan.md` (does not add features).
