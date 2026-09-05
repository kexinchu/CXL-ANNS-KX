# FlashANNS Integrated Evaluation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce validated Q2--Q4 measurements and final figures for the frozen FlashANNS Wise Prefetcher plus continuous pipeline on T2I-10M, YFCC-10M, and LAION-10M with a fixed 4 GiB CXL-side page cache.

**Architecture:** Preserve the frozen page/I/O/scheduler path at runtime commit `15e6632`, add only the approved MIPS/L2 distance adapter and observation hooks, and drive all systems through a manifest/preflight/runner/validator pipeline. Complete one dataset vertically at a time (T2I, YFCC, LAION), seal immutable run records, then render the three paper figures from accepted JSON only.

**Tech Stack:** C++17, GNU Make, Python 3 standard library, JSON, NumPy/Matplotlib, SHA-256, `/dev/vmem0`, vmem sysfs, NVMe block counters.

**Spec:** `docs/superpowers/specs/2026-09-04-flashanns-prefetcher-evaluation-design.md` at or after commit `1dc182c`.

**2026-09-05 execution amendment:** Oracle is not an executable system. Q2 is
Demand, PipeANN, and FlashANNS; smoke proof is Demand plus FlashANNS. The
`oracle_image` artifact remains host-side correctness evidence only.

**Current live milestone (2026-09-05):** T2I full host/device identity passes.
The first two-record Demand/FlashANNS same-search proof used mismatched host
windows and is diagnostic only. Demand is now locked to the same frozen 128 MiB
per-thread window as FlashANNS; the two-record smoke must be rerun from separate
cold resets before calibration or Q2. YFCC and LAION remain blocked on dataset
admission.

---

## Frozen Boundaries

- Runtime baseline: `15e6632`.
- Evaluation-contract baseline: `1dc182c`.
- Paper files are not experiment inputs. Ignore concurrent edits under
  `paper/` until Task 15.
- Do not modify candidate-list maintenance, page collection, issue order,
  completion handling, steal scheduling, pipeline depth, or scoring residency.
- The only approved semantic extension is `--metric mips|l2`, used
  consistently by PQ ADC and exact rerank.
- Removed early-CL, lookahead, Blind, score-page, pipe-drive, admit-gap, and
  speculative-beam controls never enter a command.
- Oracle and `--oracle-dram` never enter a command, accepted record, aggregate,
  or plot.
- All internal measured runs require `cache_limit=4294967296`.
- A runner never loads/unloads a driver, changes sysfs, stages an image, resets
  a cache, or writes a backing device.
- Raw run directories are immutable and untracked. Commit code, schemas,
  manifests, validated tables, figures, and readiness records only.
- Any `/mnt/disk0` build, live image stage, cache-limit change, or reset is a
  separate approval checkpoint.

## Canonical File Map

| Path | Responsibility |
|---|---|
| `serving/distance_metric.hpp` | Parse and apply MIPS/L2 exact distance. |
| `serving/pq_table.hpp` | Build query LUTs using the declared metric. |
| `serving/eval_trace.hpp` | Store deterministic per-query trace records and sidecars. |
| `serving/search_beam.cpp` | Parse metric/trace flags and add observation-only hooks. |
| `serving/metrics.hpp` | Add event-counted page and scheduler counters. |
| `serving/hide_fill.hpp` | Count requested pages before and issued pages after existing extent formation. |
| `experiments/eval/flashanns/config.py` | Validate datasets, systems, matrix, and frozen constants. |
| `experiments/eval/flashanns/datasets.json` | Dataset artifacts, metrics, layouts, sizes, and readiness. |
| `experiments/eval/flashanns/systems.json` | Exact core and ablation system definitions. |
| `experiments/eval/flashanns/matrix.json` | Smoke, calibration, Q2, Q3, and Q4 matrices. |
| `experiments/eval/flashanns/verify_dataset.py` | Validate source and serving artifacts. |
| `experiments/eval/flashanns/freeze_artifacts.py` | Stream SHA-256 manifests. |
| `experiments/eval/flashanns/preflight.py` | Fail-closed read-only live checks. |
| `experiments/eval/flashanns/restore_volatile.py` | Restore and verify only RAM-tier stripes after a cold module reload. |
| `experiments/eval/flashanns/run_one.py` | Execute one already-approved run and seal `run.json`. |
| `experiments/eval/flashanns/run_matrix.py` | Deterministically expand a phase; never reset hardware. |
| `experiments/eval/flashanns/validate_run.py` | Recompute sidecars, recall, metrics, and proof gates. |
| `experiments/eval/flashanns/aggregate.py` | Build five-repeat estimates and bootstrap intervals. |
| `experiments/eval/flashanns/plot_q2_q4.py` | Render provisional and final Q2--Q4 figures. |
| `experiments/eval/flashanns/schema/run.schema.json` | Required run identity and evidence fields. |
| `experiments/eval/flashanns/tests/` | Offline unit and negative tests. |
| `results/eval/flashanns/` | Generated manifests, readiness, validated CSV, provenance, and plots. |

---

### Task 0: Reconfirm the Frozen Baseline and Record Live Blockers

**Files:**
- Inspect: `serving/search_beam.cpp`
- Inspect: `serving/cont_batch.hpp`
- Inspect: `serving/hide_fill.hpp`
- Inspect: `serving/pq_table.hpp`
- Inspect: `serving/metrics.hpp`
- Produce: `results/eval/flashanns/readiness/baseline.json`

- [ ] **Step 1: Prove the runtime sources still equal the freeze**

Run:

~~~bash
git diff --exit-code 15e6632 -- serving/search_beam.cpp serving/cont_batch.hpp serving/hide_fill.hpp serving/pq_table.hpp serving/metrics.hpp serving/prefetch.hpp
~~~

Expected: exit 0 before evaluation changes.

- [ ] **Step 2: Run and record the offline baseline**

~~~bash
make -C serving/tests test
g++ -O3 -std=c++17 -march=native -pthread -I. serving/search_beam.cpp -o /tmp/search_beam-15e6632 -lnuma
sha256sum /tmp/search_beam-15e6632
~~~

Expected: all 11 current C++ tests pass and compilation succeeds.

- [ ] **Step 3: Read live state without opening or changing the device**

~~~bash
for f in backend backing_count nvme_dev target_bdf cache_limit cache_used dirty_bytes io_errors evictions; do
  printf '%s=' "$f"
  sed -n '1p' "/sys/class/vmem/vmem0/$f"
done
test -e /dev/vmem0
~~~

Expected at plan-writing time: this gate fails because the cache is 100 MiB,
`dirty_bytes` is nonzero, and `/dev/vmem0` is absent. Record the values and
do not repair them in this task.

- [ ] **Step 4: Write the baseline record**

The record must contain:

~~~json
{
  "runtime_base": "15e6632",
  "contract_base": "1dc182c",
  "offline_tests": {"passed": 11, "failed": 0},
  "source_equal_to_runtime_base": true,
  "live_ready": false,
  "live_blockers": [
    "cache_limit is not 4294967296",
    "dirty_bytes is nonzero",
    "/dev/vmem0 is absent"
  ]
}
~~~

Populate observed values rather than copying the drafting-time numbers.

- [ ] **Step 5: Commit only the readiness record**

~~~bash
git add results/eval/flashanns/readiness/baseline.json
git commit -m "test: record integrated evaluation baseline"
~~~

---

### Task 1: Add the Approved MIPS/L2 Distance Adapter

**Files:**
- Create: `serving/distance_metric.hpp`
- Modify: `serving/pq_table.hpp`
- Modify: `serving/prefetch.hpp`
- Modify: `serving/search_beam.cpp`
- Create: `serving/tests/test_distance_metric.cpp`
- Modify: `serving/tests/test_pq_table.cpp`
- Modify: `serving/tests/Makefile`

- [ ] **Step 1: Write failing exact-distance tests**

Create `test_distance_metric.cpp` with these assertions:

~~~cpp
#include "serving/distance_metric.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
  const float a[] = {1, 2, 3};
  const float b[] = {3, 2, 1};
  assert(parse_distance_metric("mips") == DistanceMetric::Mips);
  assert(parse_distance_metric("l2") == DistanceMetric::L2);
  assert(std::fabs(distance_f32(a, b, 3, DistanceMetric::Mips) + 10.f) < 1e-6f);
  assert(std::fabs(distance_f32(a, b, 3, DistanceMetric::L2) - 8.f) < 1e-6f);
  bool rejected = false;
  try { (void)parse_distance_metric("cosine"); }
  catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
  std::puts("test_distance_metric OK");
}
~~~

Add a PQ test using two one-dimensional centroids and assert that MIPS and L2
produce different, analytically correct orderings for the same query.

- [ ] **Step 2: Confirm the new test fails**

~~~bash
make -C serving/tests test_distance_metric
~~~

Expected: missing `serving/distance_metric.hpp`.

- [ ] **Step 3: Implement the pure metric interface**

Create:

~~~cpp
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

enum class DistanceMetric : uint8_t { Mips, L2 };

inline DistanceMetric parse_distance_metric(const std::string& value) {
  if (value == "mips") return DistanceMetric::Mips;
  if (value == "l2") return DistanceMetric::L2;
  throw std::invalid_argument("metric must be mips or l2");
}

inline const char* distance_metric_name(DistanceMetric value) {
  return value == DistanceMetric::Mips ? "mips" : "l2";
}

inline float distance_f32(const float* x, const float* q, uint32_t dim,
                          DistanceMetric metric) {
  float sum = 0;
  if (metric == DistanceMetric::Mips) {
    for (uint32_t i = 0; i < dim; ++i) sum -= x[i] * q[i];
  } else {
    for (uint32_t i = 0; i < dim; ++i) {
      const float d = x[i] - q[i];
      sum += d * d;
    }
  }
  return sum;
}
~~~

- [ ] **Step 4: Make PQ LUT construction metric-aware**

Retain the pivot centroid in `PqTable::centroid`. Add:

~~~cpp
void fill_lut_l2(const float* q, float* dst) const {
  for (uint32_t c = 0; c < nchunks; ++c) {
    float* chunk = dst + (size_t)c * kCentroids;
    for (uint32_t i = 0; i < kCentroids; ++i) chunk[i] = 0;
    for (uint32_t d = chunk_off[c]; d < chunk_off[c + 1]; ++d) {
      const float* centers = tables_T.data() + (size_t)d * kCentroids;
      for (uint32_t i = 0; i < kCentroids; ++i) {
        const float delta = q[d] - centroid[d] - centers[i];
        chunk[i] += delta * delta;
      }
    }
  }
}

void fill_lut(const float* q, float* dst, DistanceMetric metric) const {
  if (metric == DistanceMetric::Mips) fill_lut_ip(q, dst);
  else fill_lut_l2(q, dst);
}
~~~

Change `begin_query_ip` into `begin_query(q, metric)` and keep no implicit
metric default inside evaluation code.

- [ ] **Step 5: Dispatch only the PQ paths**

Add `DistanceMetric metric = DistanceMetric::Mips` to `Prefetch`. Parse
`--metric`, reject other strings, print `metric=<name>`, and use:

~~~cpp
pq->begin_query(qf, pref.metric);
pq->fill_lut(qf, q.lut.data(), pref.metric);
c.dist = vec_distance(src, qf, pl.hdr->dim, pl.hdr->vec_bytes, pref.metric);
c.dist = vec_distance(src, q.qf, pl.hdr->dim, pl.hdr->vec_bytes, q.metric);
~~~

Add `DistanceMetric metric` to `PqQ` and initialize it from `Prefetch`.
Do not reorder any candidate, page, issue, wait, or scheduling statement.

- [ ] **Step 6: Verify both metrics and audit the frozen diff**

~~~bash
make -C serving/tests test_distance_metric test_pq_table
serving/tests/test_distance_metric
serving/tests/test_pq_table
make -C serving/tests test
g++ -O3 -std=c++17 -march=native -pthread -I. serving/search_beam.cpp -o /tmp/search_beam-metrics -lnuma
git diff --function-context 15e6632 -- serving/search_beam.cpp serving/cont_batch.hpp serving/hide_fill.hpp
~~~

Expected: all tests pass; `cont_batch.hpp` and page/scheduler statements are
unchanged; the search diff contains distance dispatch only.

- [ ] **Step 7: Commit**

~~~bash
git add serving/distance_metric.hpp serving/pq_table.hpp serving/prefetch.hpp serving/search_beam.cpp serving/tests/test_distance_metric.cpp serving/tests/test_pq_table.cpp serving/tests/Makefile
git commit -m "feat: add dataset metric adapter"
~~~

---

### Task 2: Lock Dataset, System, and Matrix Configuration

**Files:**
- Create: `experiments/__init__.py`
- Create: `experiments/eval/__init__.py`
- Create: `experiments/eval/flashanns/__init__.py`
- Create: `experiments/eval/flashanns/config.py`
- Create: `experiments/eval/flashanns/datasets.json`
- Create: `experiments/eval/flashanns/systems.json`
- Create: `experiments/eval/flashanns/matrix.json`
- Create: `experiments/eval/flashanns/tests/test_config.py`

- [ ] **Step 1: Write failing configuration tests**

Tests must assert:

~~~python
self.assertEqual(datasets["t2i10m"]["metric"], "mips")
self.assertEqual(datasets["laion10m"]["metric"], "mips")
self.assertEqual(datasets["yfcc10m"]["metric"], "l2")
self.assertEqual(constants["cache_limit"], 4 * 1024**3)
self.assertEqual(systems["flashanns"]["threads"], 8)
self.assertEqual(systems["flashanns"]["pipe_depth"], 2)
self.assertEqual(systems["flashanns"]["issue_qd"], 0)
self.assertEqual(systems["flashanns"]["per_thread_window"], 128 * 1024**2)
~~~

Also reject a ready dataset missing an artifact, a metric not in
`{"mips","l2"}`, a live removed flag, a system other than eight threads in
Q2, and any cache limit other than 4294967296.

- [ ] **Step 2: Confirm the module is absent**

~~~bash
python3 -m unittest experiments.eval.flashanns.tests.test_config -v
~~~

Expected: import failure.

- [ ] **Step 3: Define dataset records**

Use these exact T2I artifact keys and paths:

~~~json
{
  "source_base": "/mnt/disk0/chukexin_motivation/data/text2image_10m/base.10M.fbin",
  "source_queries": "/mnt/disk0/chukexin_motivation/data/text2image_10m/query.public.100K.fbin",
  "source_gt": "/mnt/disk0/chukexin_motivation/data/text2image_10m/text2image-10M",
  "execution_base": "/mnt/disk0/chukexin_motivation/data/text2image_10m/base.10M.fbin",
  "oracle_image": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m.bin",
  "extent_image": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m_extent.bin",
  "graph": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m.graph.bin",
  "nav_graph": "/root/chukexin/CXL-ANNS-KX/results/paper_figs/nav_10k.bin",
  "entry": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/serving_entry_t2i_10m_pagebin.bin",
  "query_subset": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/query_10k.fbin",
  "ground_truth": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/gt_10k_k10.ibin",
  "id_map": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/new_to_old_pagebin.bin",
  "slot_map": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/id_to_slot_10m_extent.bin",
  "pq64_pivots": "/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin",
  "pq64_codes": "/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin"
}
~~~

T2I paths use the existing `serving_t2i_10m` and
`pipeann_t2i10m` trees. YFCC paths are rooted at
`/mnt/disk0/chukexin_motivation/serving_yfcc_10m`; LAION paths are rooted at
`/mnt/disk0/chukexin_motivation/serving_laion_10m`. Mark only T2I ready at
initial creation. `oracle_image` is used only by offline host-side
vector/record integrity checks; it is never passed to a measured command.

- [ ] **Step 4: Define systems without dead flags**

Create these IDs:

~~~json
{
  "demand": {"threads": 8, "per_thread_window": 134217728, "flags": ["--no-vmem-prefetch", "--pipe-w", "1", "--no-extent-run", "--no-steal-sched"]},
  "pipeann": {"kind": "external-pipeann", "threads": 8, "flags": []},
  "flashanns": {"threads": 8, "flags": ["--per-thread-window", "--pipe-depth", "2", "--issue-qd", "0", "--steal-sched", "--extent-run"]},
  "serial-t1": {"threads": 1, "flags": ["--no-vmem-prefetch", "--pipe-w", "1", "--no-extent-run"]},
  "batch-t1": {"threads": 1, "flags": ["--pipe-w", "16", "--no-extent-run"]},
  "extent-t1": {"threads": 1, "flags": ["--pipe-w", "16", "--extent-run"]},
  "nosteal-t8": {"threads": 8, "flags": ["--per-thread-window", "--pipe-depth", "2", "--issue-qd", "0", "--no-steal-sched", "--extent-run"]}
}
~~~

Config validation rejects a system ID named `oracle` and treats
`--oracle-dram` as a removed flag.

The smoke proof must verify the semantic label `demand`; if it still submits
asynchronous batch I/O, relabel it and do not use it as Demand.

- [ ] **Step 5: Define matrices**

~~~json
{
  "seed": 20260904,
  "query_seed": 42,
  "cache_limit": 4294967296,
  "k": 10,
  "base_L": [50, 100, 200, 400, 800, 1600],
  "extended_L": [2400, 3200],
  "smoke": {"nq": 100, "repeats": 1},
  "calibration": {"nq": 500, "repeats": 1},
  "q2": {"nq": 10000, "repeats": 5, "systems": ["demand", "pipeann", "flashanns"]},
  "q3_t1": {"nq": 10000, "repeats": 5, "systems": ["serial-t1", "batch-t1", "extent-t1"]},
  "q3_t8": {"nq": 10000, "repeats": 5, "systems": ["nosteal-t8", "flashanns"]},
  "q4": {"nq": 10000, "repeats": 5, "systems": ["flashanns"], "states": ["cold", "warm"]}
}
~~~

- [ ] **Step 6: Implement validation and run tests**

`load_configs(repo_root)` returns `datasets, systems, matrix` and raises
`ConfigError` for every condition tested in Step 1.

~~~bash
python3 -m unittest experiments.eval.flashanns.tests.test_config -v
~~~

Expected: all tests pass.

- [ ] **Step 7: Commit**

~~~bash
git add experiments
git commit -m "test: lock integrated evaluation matrix"
~~~

---

### Task 3: Add Deterministic T=1/T=8 Sidecars

**Files:**
- Create: `serving/eval_trace.hpp`
- Modify: `serving/search_beam.cpp`
- Create: `serving/tests/test_eval_trace.cpp`
- Modify: `serving/tests/Makefile`

- [ ] **Step 1: Write the failing concurrent-order test**

Construct `EvalTrace(3, 2)`, record query ordinals in order 2, 0, 1, finish,
and assert sidecar order is 0, 1, 2 and exact sizes are:

~~~text
query_ids.u32          nq * 4
latency_ns.u64         nq * 8
candidate_offsets.u64  (nq + 1) * 8
candidate_ids.u32      offsets[nq] * 4
result_ids.u32         nq * k * 4
~~~

- [ ] **Step 2: Confirm failure**

~~~bash
make -C serving/tests test_eval_trace
~~~

Expected: missing header.

- [ ] **Step 3: Implement index-addressed trace storage**

Use:

~~~cpp
struct EvalTraceRow {
  uint32_t query_id = UINT32_MAX;
  uint64_t latency_ns = 0;
  std::vector<uint32_t> candidates;
  std::vector<uint32_t> results;
  bool valid = false;
};

class EvalTrace {
 public:
  EvalTrace(size_t nq, uint32_t k);
  void record(size_t ordinal, uint32_t query_id, uint64_t latency_ns,
              const std::vector<CandId>& candidates,
              const std::vector<uint32_t>& results);
  bool finish(const std::filesystem::path& dir) const;
};
~~~

Pre-size rows so distinct T=8 queries write distinct indices. `finish` runs
after workers join, rejects any invalid row, writes `.tmp` files, fsyncs, and
renames atomically.

- [ ] **Step 4: Hook both PQ execution paths**

Parse `--eval-trace-dir`. In T=1 copy the committed candidate IDs immediately
before page issue. In the parkable T=8 path copy `PqQ::cand` immediately
before `pqq_issue`; record final mapped IDs in `finish_slot` and
`finish_local`. Record integer nanoseconds and original query ID
`qidx[qi]`.

Do not add a global lock to the timed path and do not change scheduler state.

- [ ] **Step 5: Verify**

~~~bash
make -C serving/tests test_eval_trace
serving/tests/test_eval_trace
make -C serving/tests test
g++ -O3 -std=c++17 -march=native -pthread -I. serving/search_beam.cpp -o /tmp/search_beam-trace -lnuma
git diff --function-context 15e6632 -- serving/search_beam.cpp
~~~

Expected: sidecar test and full suite pass; only metric/trace hooks touch frozen
functions.

- [ ] **Step 6: Commit**

~~~bash
git add serving/eval_trace.hpp serving/search_beam.cpp serving/tests/test_eval_trace.cpp serving/tests/Makefile
git commit -m "test: add deterministic evaluation sidecars"
~~~

---

### Task 4: Add Event and Scheduler Accounting

**Files:**
- Modify: `serving/metrics.hpp`
- Modify: `serving/hide_fill.hpp`
- Modify: `serving/cont_batch.hpp`
- Modify: `serving/search_beam.cpp`
- Modify: `serving/tests/test_metrics.cpp`
- Modify: `serving/tests/test_cont_batch.cpp`

- [ ] **Step 1: Add failing additive-counter tests**

Assert:

~~~cpp
Metrics m;
m.note_pf_issue_event(7, 10);
m.note_pf_issue_event(3, 3);
assert(m.pf_requested_page_events == 10);
assert(m.pf_issued_page_events == 13);
assert(m.pf_extent_extra_page_events == 3);
m.note_inflight_depth(0);
m.note_inflight_depth(8);
assert(m.inflight_depth_sum == 8);
assert(m.inflight_depth_samples == 2);
assert(m.inflight_depth_max == 8);
~~~

Also verify `add_from` sums events rather than deduplicating across queries.

- [ ] **Step 2: Confirm the tests fail**

~~~bash
make -C serving/tests test_metrics test_cont_batch
~~~

Expected: missing fields/helpers.

- [ ] **Step 3: Add counters**

Add:

~~~cpp
uint64_t pf_requested_page_events = 0;
uint64_t pf_issued_page_events = 0;
uint64_t pf_extent_extra_page_events = 0;
uint64_t issue_command_events = 0;
uint64_t inflight_depth_sum = 0;
uint64_t inflight_depth_samples = 0;
uint64_t inflight_depth_max = 0;

void note_pf_issue_event(uint64_t requested, uint64_t issued) {
  pf_requested_page_events += requested;
  pf_issued_page_events += issued;
  pf_extent_extra_page_events += issued > requested ? issued - requested : 0;
  if (issued) issue_command_events++;
}
~~~

Merge and print them on one stable `eval_events` line. Keep existing legacy
metrics for historical logs but never use unique-set sizes as per-query event
counts in new figures.

- [ ] **Step 4: Count around existing operations**

In `hide_issue`, snapshot request count after residency/in-flight filtering
and before the existing extent transform; call `note_pf_issue_event` after
the transform. In the scheduler, sample the already-computed NAND-inflight
count without changing decisions.

- [ ] **Step 5: Verify and audit**

~~~bash
make -C serving/tests test_metrics test_cont_batch
make -C serving/tests test
git diff --function-context 15e6632 -- serving/hide_fill.hpp serving/cont_batch.hpp serving/search_beam.cpp
~~~

Expected: only counter calls surround existing behavior.

- [ ] **Step 6: Commit**

~~~bash
git add serving/metrics.hpp serving/hide_fill.hpp serving/cont_batch.hpp serving/search_beam.cpp serving/tests/test_metrics.cpp serving/tests/test_cont_batch.cpp
git commit -m "test: add integrated evaluation counters"
~~~

---

### Task 5: Implement Dataset Verification and Artifact Freezing

**Files:**
- Create: `experiments/eval/flashanns/verify_dataset.py`
- Create: `experiments/eval/flashanns/freeze_artifacts.py`
- Create: `experiments/eval/flashanns/tests/test_verify_dataset.py`
- Create: `experiments/eval/flashanns/tests/test_freeze_artifacts.py`

- [ ] **Step 1: Write synthetic format tests**

Cover `fbin`, `u8bin`, and `ibin` headers, exact file length, 10k unique
query IDs, aligned GT extraction, permutation maps, packed headers, and sampled
record readback.

For YFCC widening, assert every sampled `uint8` coordinate equals the emitted
`float32` value exactly and that direct native L2 and widened-runtime L2
return identical top-k IDs.

- [ ] **Step 2: Confirm import failures**

~~~bash
python3 -m unittest experiments.eval.flashanns.tests.test_verify_dataset experiments.eval.flashanns.tests.test_freeze_artifacts -v
~~~

- [ ] **Step 3: Implement checked interfaces**

Provide these exact interfaces and behavior:

~~~text
read_bin_header(path: Path, item_size: int) -> (n: int, dim: int)
  Read two little-endian uint32 fields and require size == 8 + n*dim*item_size.
validate_query_ids(ids: list[int], available: int, required: int = 10000)
  Require exact count, uniqueness, and 0 <= id < available.
validate_gt_subset(source_gt: Path, query_ids: list[int], output_gt: Path, k: int = 10)
  Select the declared rows and first k IDs, then reread and compare every ID.
validate_permutation(values: list[int], n: int)
  Require len(values) == n and sorted(values) == range(n).
compare_widened_u8(native_path: Path, float_path: Path, sample_ids: list[int], dim: int)
  Require float(native_coordinate) == widened_coordinate for every sampled coordinate.
verify_packed_readback(dataset: dict, sample_count: int = 1024, seed: int = 20260904)
  Compare source vector bytes/values, neighbor count, and logical neighbor IDs.
hash_file(path: Path, block_bytes: int = 8388608) -> dict
  Return absolute path, streamed byte count, and lowercase SHA-256.
freeze_dataset(dataset: dict, output: Path)
  Validate expected sizes, hash sorted artifact keys, and atomically write JSON.
~~~

All failures raise `DatasetError` or `ArtifactError` with the artifact name.

- [ ] **Step 4: Run tests**

~~~bash
python3 -m unittest experiments.eval.flashanns.tests.test_verify_dataset experiments.eval.flashanns.tests.test_freeze_artifacts -v
~~~

Expected: pass.

- [ ] **Step 5: Commit**

~~~bash
git add experiments/eval/flashanns/verify_dataset.py experiments/eval/flashanns/freeze_artifacts.py experiments/eval/flashanns/tests/test_verify_dataset.py experiments/eval/flashanns/tests/test_freeze_artifacts.py
git commit -m "test: verify evaluation dataset artifacts"
~~~

---

### Task 6: Implement the 4 GiB Fail-Closed Preflight

**Files:**
- Create: `experiments/eval/flashanns/preflight.py`
- Create: `experiments/eval/flashanns/restore_volatile.py`
- Create: `experiments/eval/flashanns/live-contract.json`
- Create: `experiments/eval/flashanns/tests/test_preflight.py`
- Create: `experiments/eval/flashanns/tests/test_restore_volatile.py`

- [ ] **Step 1: Write fake-sysfs failures**

Independently reject:

~~~text
cache_limit != 4294967296
dirty_bytes != 0
io_errors != 0
missing /dev/vmem0
unexpected backend/backing_count/nvme_dev/target_bdf
open users
bad image magic
sampled staged-image mismatch
cold cache_used != 0
cold run without accepted full-image and RAM-stripe restoration evidence
warm run without accepted cold-parent evidence
~~~

- [ ] **Step 2: Define the identity contract**

The common fields are:

~~~json
{
  "device": "/dev/vmem0",
  "sysfs": "/sys/class/vmem/vmem0",
  "cache_limit": 4294967296,
  "required_dirty_bytes": 0,
  "required_io_errors": 0,
  "sample_pages": 32,
  "sample_seed": 20260904
}
~~~

Dataset-specific image offset, length, host image, and magic come from
`datasets.json`.

- [ ] **Step 3: Implement read-only checks**

Expose these exact interfaces and behavior:

~~~text
snapshot(sysfs: Path, device: Path, dataset: dict, read_device: bool = True) -> dict
  Read declared sysfs fields, open-user list, device existence, and optional magic.
validate(record: dict, contract: dict, dataset: dict, state: str) -> None
  Accumulate every mismatch and raise one PreflightError if the list is nonempty.
sampled_layout_digest(path: Path, base_offset: int, length: int,
                      pages: int, seed: int) -> str
  Hash page number plus 4096 bytes for deterministic random pages.
full_layout_digest(path: Path, base_offset: int, length: int,
                   block_bytes: int = 67108864) -> str
  Stream the complete declared host or device interval and return SHA-256.
snapshot_and_validate(contract: dict, dataset: dict, state: str,
                      identity_evidence: dict | None = None,
                      volatile_evidence: dict | None = None) -> dict
  Perform snapshot, identity comparison, and state checks. For a cold run,
  accept only a matching full-stage identity record plus fresh volatile
  RAM-stripe restoration evidence, so validation does not warm the SSD cache.
ram_segments(offset: int, length: int, ram_size: int, ssd_size: int,
             stripe_size: int) -> list[Segment]
  Reproduce vmem_sw_layout_map() and return only dataset intersections in RAM.
restore_volatile_stripes(...)
  Write and reread only those intersections; require equal SHA-256 and
  cache_used=dirty_bytes=io_errors=0.
atomic_json_write(path: Path, value: dict) -> None
  Write sorted JSON to a sibling .tmp, fsync, and rename.
~~~

Preflight opens the device `O_RDONLY` only. The restoration CLI opens it for
write only with explicit `--write`, after the caller has established an empty
cache and an idle device, and touches only computed RAM-tier intersections.

- [ ] **Step 4: Run tests and preserve the live failure**

~~~bash
python3 -m unittest experiments.eval.flashanns.tests.test_preflight experiments.eval.flashanns.tests.test_restore_volatile -v
python3 -m experiments.eval.flashanns.preflight --dataset t2i10m --state cold --out results/eval/flashanns/preflight/t2i-attempt.json
~~~

Expected now: unit tests pass; live command fails closed. Do not change the
driver or cache in response.

- [ ] **Step 5: Commit code and contract, not failed output**

~~~bash
git add experiments/eval/flashanns/preflight.py experiments/eval/flashanns/restore_volatile.py experiments/eval/flashanns/live-contract.json experiments/eval/flashanns/tests/test_preflight.py experiments/eval/flashanns/tests/test_restore_volatile.py
git commit -m "test: add 4 GiB live preflight"
~~~

---

### Task 7: Implement the Manifest Runner and Validator

**Files:**
- Create: `experiments/eval/flashanns/schema/run.schema.json`
- Create: `experiments/eval/flashanns/run_one.py`
- Create: `experiments/eval/flashanns/run_matrix.py`
- Create: `experiments/eval/flashanns/validate_run.py`
- Create: `experiments/eval/flashanns/tests/test_runner.py`
- Create: `experiments/eval/flashanns/tests/test_validate_run.py`

- [ ] **Step 1: Write dry-run tests**

Assert T2I smoke expands exactly `demand` and `flashanns`; every command uses
`/dev/vmem0`, contains the dataset metric, 4 GiB identity contract, correct
thread count and trace directory, and contains neither Oracle nor removed
flags. Assert Q2 expands exactly 15 runs at one frozen `L` (three systems times
five repeats) and system shuffling is deterministic per `(dataset, phase, L,
repeat, state)`.

- [ ] **Step 2: Define required run fields**

The schema requires:

~~~json
[
  "run_id", "dataset", "metric", "phase", "system", "state", "L", "k",
  "nq", "repeat", "command", "git", "binary_sha256",
  "artifact_manifest_sha256", "preflight_before", "preflight_after",
  "device_before", "device_after", "metrics", "sidecars", "validation"
]
~~~

`validation.status` is one of `pending`, `accepted`, or `rejected`.

- [ ] **Step 3: Implement command expansion**

Common internal flags include:

~~~text
--diskann-layout --pq-nav --metric DATASET_METRIC
--beam L --k 10 --iters 0 --max-q NQ --shuffle-seed 42
--cpu-affinity --policy P3 --no-hide-warm-entry
--no-direct-install --no-score-cache --no-stripe-fill
--expand-batch 8 --issue-ahead 1
--eval-trace-dir RUN_DIR/trace
~~~

Append exact dataset paths and system flags. Reject duplicate/conflicting flags.
The PipeANN adapter records its native command and emits the same sidecar
schema; it never masquerades as the internal binary.

- [ ] **Step 4: Implement execution order**

`run_one.py` performs:

~~~python
before = preflight.snapshot_and_validate(contract, dataset, state, evidence)
device_before = read_block_counters(before)
completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                           text=True, check=False)
after = preflight.snapshot_and_validate(contract, dataset, "post", evidence)
device_after = read_block_counters(after)
record = build_record(spec, completed.returncode, before, after,
                      device_before, device_after, run_dir)
atomic_json_write(run_dir / "run.json", record)
~~~

The runner refuses an existing run ID and refuses reused cold-reset evidence.
`--system` selects one admitted system so matched systems can consume separate
cold resets. `--identity-evidence` and `--volatile-evidence` pass the two
accepted preflight records into `run_one.py`; a cold execution without either
record fails before creating a run directory.

- [ ] **Step 5: Implement validation**

Recompute sidecar sizes, hashes, query IDs, recall, latency percentiles, command
deltas, and all schema fields. For same-search blocks, require identical metric,
artifact manifest, query IDs, `L`, candidate offsets, candidate IDs, returned
IDs, and recall.

Reject every Oracle run record, FlashANNS bounce scores, score-triggered Flash,
incomplete queries, or a non-4-GiB preflight. Aggregation rejects Oracle even
if a stale record is marked accepted, and plotting defines no Oracle series.

- [ ] **Step 6: Run tests and dry-run**

~~~bash
python3 -m unittest experiments.eval.flashanns.tests.test_runner experiments.eval.flashanns.tests.test_validate_run -v
python3 -m experiments.eval.flashanns.run_matrix --dataset t2i10m --phase smoke --dry-run
~~~

Expected: pass and deterministic commands only; no device open or output run
directory.

- [ ] **Step 7: Commit**

~~~bash
git add experiments/eval/flashanns/schema experiments/eval/flashanns/run_one.py experiments/eval/flashanns/run_matrix.py experiments/eval/flashanns/validate_run.py experiments/eval/flashanns/tests/test_runner.py experiments/eval/flashanns/tests/test_validate_run.py
git commit -m "test: add integrated evaluation runner"
~~~

---

### Task 8: Implement Aggregation and Q2--Q4 Plot Tests

**Files:**
- Create: `experiments/eval/flashanns/aggregate.py`
- Create: `experiments/eval/flashanns/plot_q2_q4.py`
- Create: `experiments/eval/flashanns/tests/test_aggregate.py`
- Create: `experiments/eval/flashanns/tests/test_plots.py`

- [ ] **Step 1: Write synthetic five-repeat tests**

Build synthetic accepted records for three datasets and assert:

- exactly five repetitions per Q2/Q3 point;
- exactly five cold/warm pairs per Q4 point;
- median and seed-20260904 bootstrap intervals are deterministic;
- a four-repeat block, candidate-hash mismatch, rejected run, or missing
  cold-parent link fails;
- provisional single-dataset and final three-dataset modes use the same
  aggregation code.

- [ ] **Step 2: Implement deterministic aggregation**

Use:

~~~python
def bootstrap_ci(values, seed=20260904, samples=10000):
    rng = random.Random(seed)
    medians = []
    for _ in range(samples):
        draw = [values[rng.randrange(len(values))] for _ in values]
        medians.append(statistics.median(draw))
    medians.sort()
    return medians[250], medians[9750]
~~~

Emit run IDs beside every row and a provenance entry for every plotted mark.

- [ ] **Step 3: Implement the fixed figure contracts**

Generate:

~~~text
results/eval/flashanns/figures/q2-main.pdf
results/eval/flashanns/figures/q3-ablation.pdf
results/eval/flashanns/figures/q4-cold-warm.pdf
~~~

Q2 is 2x2: QPS, mean/p99, critical wait, score source. Q3 has T=1 transfer and
T=8 scheduler panels. Q4 has paired cold/warm marks for three datasets. Use no
dual y-axis, keep system colors stable, and show 95% intervals.

- [ ] **Step 4: Run tests**

~~~bash
MPLCONFIGDIR=/tmp/flashanns-mpl python3 -m unittest experiments.eval.flashanns.tests.test_aggregate experiments.eval.flashanns.tests.test_plots -v
~~~

Expected: deterministic CSV/provenance and one-page vector PDFs.

- [ ] **Step 5: Commit**

~~~bash
git add experiments/eval/flashanns/aggregate.py experiments/eval/flashanns/plot_q2_q4.py experiments/eval/flashanns/tests/test_aggregate.py experiments/eval/flashanns/tests/test_plots.py
git commit -m "test: add Q2 Q3 Q4 evidence pipeline"
~~~

---

### Task 9: Complete the T2I-10M Vertical Slice

**Files:**
- Produce: `results/eval/flashanns/manifests/t2i10m.json`
- Produce: `results/eval/flashanns/readiness/t2i-proof.json`
- Produce: `results/eval/flashanns/calibration/t2i10m.json`
- Produce: `results/eval/flashanns/readiness/t2i-final.json`
- Produce: `results/eval/flashanns/provisional/t2i/`

- [ ] **Step 1: Freeze and verify existing artifacts**

~~~bash
python3 -m experiments.eval.flashanns.verify_dataset --dataset t2i10m --full
python3 -m experiments.eval.flashanns.freeze_artifacts --dataset t2i10m --out results/eval/flashanns/manifests/t2i10m.json
~~~

Expected: exact sizes, hashes, permutation, PQ-64, and 1,024 readbacks pass.

- [ ] **Step 2: Stop for the T2I live-state approval**

Required external outcome:

~~~text
/dev/vmem0 exists
cache_limit=4294967296
cache_used=0
dirty_bytes=0
io_errors=0
expected two NVMe devices/BDFs
accepted full T2I host/device extent-image identity from initial staging
all T2I RAM-tier stripes restored from the admitted host extent image after the current reload
fresh RAM-tier stripe digest matches; SSD-tier identity evidence is unchanged
~~~

The current hybrid layout has a 28 GiB volatile RAM tier interleaved with SSD
in 2 MiB stripes. Every module reload therefore requires restoring only the
RAM-tier intersections of the active dataset extent before a run; never rewrite
the already-staged SSD-tier stripes. For T2I's current interval this restoration
is 152 MiB. The restoration proof must still show `cache_used=0`. Do not perform
recovery, cache change, staging, or reset inside the runner.

- [ ] **Step 3: Run the 100-query proof**

After approved state preparation:

~~~bash
python3 -m experiments.eval.flashanns.run_matrix --dataset t2i10m --phase smoke --system demand \
  --identity-evidence results/eval/flashanns/preflight/t2i-full-identity.json \
  --volatile-evidence results/eval/flashanns/preflight/t2i-ram-restore-demand.json \
  --out results/eval/flashanns/raw/t2i10m/proof
# Perform a separate approved cold reload and RAM-only restoration here.
python3 -m experiments.eval.flashanns.run_matrix --dataset t2i10m --phase smoke --system flashanns \
  --identity-evidence results/eval/flashanns/preflight/t2i-full-identity.json \
  --volatile-evidence results/eval/flashanns/preflight/t2i-ram-restore-flashanns.json \
  --out results/eval/flashanns/raw/t2i10m/proof
python3 -m experiments.eval.flashanns.validate_run --compare-same-search \
  results/eval/flashanns/raw/t2i10m/proof/t2i10m-smoke-L400-r0-proof-demand/run.json \
  results/eval/flashanns/raw/t2i10m/proof/t2i10m-smoke-L400-r0-proof-flashanns/run.json \
  --out results/eval/flashanns/readiness/t2i-proof.json
~~~

This proof has exactly two records. Query IDs, candidate offsets, candidate IDs,
returned IDs, and recomputed recall must match between Demand and FlashANNS.

- [ ] **Step 4: Calibrate recall**

~~~bash
python3 -m experiments.eval.flashanns.run_matrix --dataset t2i10m --phase calibration --state cold --out results/eval/flashanns/raw/t2i10m/calibration
python3 -m experiments.eval.flashanns.validate_run --freeze-anchor 0.90 --extra-anchor 0.92 results/eval/flashanns/raw/t2i10m/calibration --out results/eval/flashanns/calibration/t2i10m.json
~~~

- [ ] **Step 5: Run Q2, Q3, and Q4 in order**

~~~bash
python3 -m experiments.eval.flashanns.run_matrix --dataset t2i10m --phase q2 --anchors results/eval/flashanns/calibration/t2i10m.json
python3 -m experiments.eval.flashanns.run_matrix --dataset t2i10m --phase q3_t1 --anchors results/eval/flashanns/calibration/t2i10m.json
python3 -m experiments.eval.flashanns.run_matrix --dataset t2i10m --phase q3_t8 --anchors results/eval/flashanns/calibration/t2i10m.json
python3 -m experiments.eval.flashanns.run_matrix --dataset t2i10m --phase q4 --paired-cold-warm --anchors results/eval/flashanns/calibration/t2i10m.json
~~~

Each cold invocation consumes its own approved reset snapshot and fresh
RAM-tier-only restoration proof.

- [ ] **Step 6: Seal and render provisional figures**

~~~bash
python3 -m experiments.eval.flashanns.validate_run --dataset-milestone t2i10m --out results/eval/flashanns/readiness/t2i-final.json
python3 -m experiments.eval.flashanns.aggregate --datasets t2i10m --out results/eval/flashanns/provisional/t2i/validated.csv --provenance results/eval/flashanns/provisional/t2i/provenance.json
MPLCONFIGDIR=/tmp/flashanns-mpl python3 -m experiments.eval.flashanns.plot_q2_q4 --csv results/eval/flashanns/provisional/t2i/validated.csv --out-dir results/eval/flashanns/provisional/t2i
~~~

- [ ] **Step 7: Commit only sealed evidence**

~~~bash
git add results/eval/flashanns/manifests/t2i10m.json results/eval/flashanns/calibration/t2i10m.json results/eval/flashanns/readiness/t2i-proof.json results/eval/flashanns/readiness/t2i-final.json
git commit -m "eval: complete T2I integrated evidence"
~~~

---

### Task 10: Prepare and Admit YFCC-10M

**Files:**
- Modify: `experiments/eval/flashanns/datasets.json`
- Create: `experiments/eval/flashanns/prepare_yfcc.py`
- Create: `experiments/eval/flashanns/tests/test_prepare_yfcc.py`
- Produce externally: `/mnt/disk0/chukexin_motivation/serving_yfcc_10m/`

- [ ] **Step 1: Test streaming uint8-to-float widening**

Verify headers, exact coordinate equality, fixed first-10k query selection,
aligned top-10 GT extraction, and native-vs-widened exact-L2 top-k equality.

- [ ] **Step 2: Implement preparation without normalization**

`prepare_yfcc.py` writes fbin in bounded chunks, copies query rows and GT rows
by recorded IDs, and seals a conversion manifest. It rejects normalization,
dimension changes, NaN, or a float value unequal to its input byte.

- [ ] **Step 3: Run offline tests**

~~~bash
python3 -m unittest experiments.eval.flashanns.tests.test_prepare_yfcc -v
python3 -m experiments.eval.flashanns.verify_dataset --dataset yfcc10m --source-only
~~~

Expected: source headers and the three known SHA-256 values pass.

- [ ] **Step 4: Stop for host-artifact build approval**

The approved build writes only under
`/mnt/disk0/chukexin_motivation/serving_yfcc_10m` and produces the exact
artifact keys in Task 2. It uses `dist_fn=l2`, `R=32`, PQ-64, deterministic
seed 42, and the recorded query subset. No device staging occurs here.

- [ ] **Step 5: Verify, freeze, and mark ready**

~~~bash
python3 -m experiments.eval.flashanns.verify_dataset --dataset yfcc10m --full
python3 -m experiments.eval.flashanns.freeze_artifacts --dataset yfcc10m --out results/eval/flashanns/manifests/yfcc10m.json
python3 -m unittest experiments.eval.flashanns.tests.test_config -v
~~~

- [ ] **Step 6: Commit code, ready config, and manifest**

~~~bash
git add experiments/eval/flashanns/prepare_yfcc.py experiments/eval/flashanns/tests/test_prepare_yfcc.py experiments/eval/flashanns/datasets.json results/eval/flashanns/manifests/yfcc10m.json
git commit -m "test: admit YFCC 10M L2 dataset"
~~~

---

### Task 11: Complete the YFCC-10M Vertical Slice

**Files:**
- Produce: `results/eval/flashanns/readiness/yfcc-proof.json`
- Produce: `results/eval/flashanns/calibration/yfcc10m.json`
- Produce: `results/eval/flashanns/readiness/yfcc-final.json`
- Produce: `results/eval/flashanns/provisional/yfcc/`

- [ ] **Step 1: Stop for YFCC staging and live-state approval**

Required external outcome:

~~~text
/dev/vmem0 exists
cache_limit=4294967296
cache_used=0
dirty_bytes=0
io_errors=0
expected two NVMe devices/BDFs
accepted full YFCC host/device extent-image identity from initial staging
all YFCC RAM-tier stripes restored from the admitted host extent image after the current reload
fresh RAM-tier stripe digest matches; SSD-tier identity evidence is unchanged
~~~

Derive the exact RAM-tier intersections from the frozen 2 MiB hybrid-layout
mapping and restore only those bytes after every reload. Require
`cache_used=0` after restoration. Do not perform staging, the cache-limit
change, restoration, or reset in the runner.

- [ ] **Step 2: Run and validate the 100-query L2 proof**

~~~bash
python3 -m experiments.eval.flashanns.run_matrix --dataset yfcc10m --phase smoke --system demand \
  --identity-evidence results/eval/flashanns/preflight/yfcc-full-identity.json \
  --volatile-evidence results/eval/flashanns/preflight/yfcc-ram-restore-demand.json \
  --out results/eval/flashanns/raw/yfcc10m/proof
# Perform a separate approved cold reload and RAM-only restoration here.
python3 -m experiments.eval.flashanns.run_matrix --dataset yfcc10m --phase smoke --system flashanns \
  --identity-evidence results/eval/flashanns/preflight/yfcc-full-identity.json \
  --volatile-evidence results/eval/flashanns/preflight/yfcc-ram-restore-flashanns.json \
  --out results/eval/flashanns/raw/yfcc10m/proof
python3 -m experiments.eval.flashanns.validate_run --compare-same-search \
  results/eval/flashanns/raw/yfcc10m/proof/yfcc10m-smoke-L400-r0-proof-demand/run.json \
  results/eval/flashanns/raw/yfcc10m/proof/yfcc10m-smoke-L400-r0-proof-flashanns/run.json \
  --out results/eval/flashanns/readiness/yfcc-proof.json
~~~

Require `metric=l2` in every command and record, identical same-search
candidate/result sidecars, and no FlashANNS score reads from CXL.

- [ ] **Step 3: Calibrate and freeze the 0.90 recall anchor**

After a separately approved cold reset:

~~~bash
python3 -m experiments.eval.flashanns.run_matrix --dataset yfcc10m --phase calibration --state cold --out results/eval/flashanns/raw/yfcc10m/calibration
python3 -m experiments.eval.flashanns.validate_run --freeze-anchor 0.90 results/eval/flashanns/raw/yfcc10m/calibration --out results/eval/flashanns/calibration/yfcc10m.json
~~~

- [ ] **Step 4: Run Q2, Q3, and Q4 in order**

~~~bash
python3 -m experiments.eval.flashanns.run_matrix --dataset yfcc10m --phase q2 --anchors results/eval/flashanns/calibration/yfcc10m.json
python3 -m experiments.eval.flashanns.run_matrix --dataset yfcc10m --phase q3_t1 --anchors results/eval/flashanns/calibration/yfcc10m.json
python3 -m experiments.eval.flashanns.run_matrix --dataset yfcc10m --phase q3_t8 --anchors results/eval/flashanns/calibration/yfcc10m.json
python3 -m experiments.eval.flashanns.run_matrix --dataset yfcc10m --phase q4 --paired-cold-warm --anchors results/eval/flashanns/calibration/yfcc10m.json
~~~

Each cold invocation consumes a distinct approved reset snapshot and fresh
RAM-tier-only restoration proof. Require five accepted repetitions for every
Q2/Q3 point and five accepted cold/warm pairs.

- [ ] **Step 5: Seal and render provisional figures**

~~~bash
python3 -m experiments.eval.flashanns.validate_run --dataset-milestone yfcc10m --out results/eval/flashanns/readiness/yfcc-final.json
python3 -m experiments.eval.flashanns.aggregate --datasets yfcc10m --out results/eval/flashanns/provisional/yfcc/validated.csv --provenance results/eval/flashanns/provisional/yfcc/provenance.json
MPLCONFIGDIR=/tmp/flashanns-mpl python3 -m experiments.eval.flashanns.plot_q2_q4 --csv results/eval/flashanns/provisional/yfcc/validated.csv --out-dir results/eval/flashanns/provisional/yfcc
~~~

- [ ] **Step 6: Commit only sealed evidence**

~~~bash
git add results/eval/flashanns/calibration/yfcc10m.json results/eval/flashanns/readiness/yfcc-proof.json results/eval/flashanns/readiness/yfcc-final.json
git commit -m "eval: complete YFCC integrated evidence"
~~~

---

### Task 12: Prepare and Admit LAION-10M

**Files:**
- Modify: `experiments/eval/flashanns/datasets.json`
- Create: `experiments/eval/flashanns/prepare_laion.py`
- Create: `experiments/eval/flashanns/tests/test_prepare_laion.py`
- Produce externally: `/mnt/disk0/chukexin_motivation/serving_laion_10m/`

- [ ] **Step 1: Test deterministic prefix and query selection**

Assert the base source is 25,000,000 x 512 float32, the selected corpus is
exactly IDs `[0, 10000000)`, query IDs are 10,000 unique values selected with
seed 42, and generated GT IDs stay below 10M.

- [ ] **Step 2: Implement preparation metadata**

`prepare_laion.py` streams the first 10M rows without rewriting values,
records source-prefix SHA-256, and writes query-ID/GT manifests. It never treats
the existing 25M graph or GT as valid for the 10M subset without verification.

- [ ] **Step 3: Stop for host-artifact build approval**

Build under `/mnt/disk0/chukexin_motivation/serving_laion_10m` using MIPS,
`R=32`, PQ-64, seed 42, and the declared 10M subset. Generate exact GT for
the admitted query subset before setting ready.

- [ ] **Step 4: Verify, freeze, and mark ready**

~~~bash
python3 -m experiments.eval.flashanns.verify_dataset --dataset laion10m --full
python3 -m experiments.eval.flashanns.freeze_artifacts --dataset laion10m --out results/eval/flashanns/manifests/laion10m.json
~~~

- [ ] **Step 5: Commit**

~~~bash
git add experiments/eval/flashanns/prepare_laion.py experiments/eval/flashanns/tests/test_prepare_laion.py experiments/eval/flashanns/datasets.json results/eval/flashanns/manifests/laion10m.json
git commit -m "test: admit LAION 10M dataset"
~~~

---

### Task 13: Complete the LAION-10M Vertical Slice

**Files:**
- Produce: `results/eval/flashanns/readiness/laion-proof.json`
- Produce: `results/eval/flashanns/calibration/laion10m.json`
- Produce: `results/eval/flashanns/readiness/laion-final.json`
- Produce: `results/eval/flashanns/provisional/laion/`

- [ ] **Step 1: Stop for LAION staging and live-state approval**

Required external outcome:

~~~text
/dev/vmem0 exists
cache_limit=4294967296
cache_used=0
dirty_bytes=0
io_errors=0
expected two NVMe devices/BDFs
accepted full LAION host/device extent-image identity from initial staging
all LAION RAM-tier stripes restored from the admitted host extent image after the current reload
fresh RAM-tier stripe digest matches; SSD-tier identity evidence is unchanged
~~~

Derive the exact RAM-tier intersections from the frozen 2 MiB hybrid-layout
mapping and restore only those bytes after every reload. Require
`cache_used=0` after restoration. Do not perform staging, the cache-limit
change, restoration, or reset in the runner.

- [ ] **Step 2: Run and validate the 100-query MIPS proof**

~~~bash
python3 -m experiments.eval.flashanns.run_matrix --dataset laion10m --phase smoke --system demand \
  --identity-evidence results/eval/flashanns/preflight/laion-full-identity.json \
  --volatile-evidence results/eval/flashanns/preflight/laion-ram-restore-demand.json \
  --out results/eval/flashanns/raw/laion10m/proof
# Perform a separate approved cold reload and RAM-only restoration here.
python3 -m experiments.eval.flashanns.run_matrix --dataset laion10m --phase smoke --system flashanns \
  --identity-evidence results/eval/flashanns/preflight/laion-full-identity.json \
  --volatile-evidence results/eval/flashanns/preflight/laion-ram-restore-flashanns.json \
  --out results/eval/flashanns/raw/laion10m/proof
python3 -m experiments.eval.flashanns.validate_run --compare-same-search \
  results/eval/flashanns/raw/laion10m/proof/laion10m-smoke-L400-r0-proof-demand/run.json \
  results/eval/flashanns/raw/laion10m/proof/laion10m-smoke-L400-r0-proof-flashanns/run.json \
  --out results/eval/flashanns/readiness/laion-proof.json
~~~

Require `metric=mips` in every command and record, identical same-search
candidate/result sidecars, and no FlashANNS score reads from CXL.

- [ ] **Step 3: Calibrate and freeze the 0.90 recall anchor**

After a separately approved cold reset:

~~~bash
python3 -m experiments.eval.flashanns.run_matrix --dataset laion10m --phase calibration --state cold --out results/eval/flashanns/raw/laion10m/calibration
python3 -m experiments.eval.flashanns.validate_run --freeze-anchor 0.90 results/eval/flashanns/raw/laion10m/calibration --out results/eval/flashanns/calibration/laion10m.json
~~~

- [ ] **Step 4: Run Q2, Q3, and Q4 in order**

~~~bash
python3 -m experiments.eval.flashanns.run_matrix --dataset laion10m --phase q2 --anchors results/eval/flashanns/calibration/laion10m.json
python3 -m experiments.eval.flashanns.run_matrix --dataset laion10m --phase q3_t1 --anchors results/eval/flashanns/calibration/laion10m.json
python3 -m experiments.eval.flashanns.run_matrix --dataset laion10m --phase q3_t8 --anchors results/eval/flashanns/calibration/laion10m.json
python3 -m experiments.eval.flashanns.run_matrix --dataset laion10m --phase q4 --paired-cold-warm --anchors results/eval/flashanns/calibration/laion10m.json
~~~

Each cold invocation consumes a distinct approved reset snapshot and fresh
RAM-tier-only restoration proof. Require five accepted repetitions for every
Q2/Q3 point and five accepted cold/warm pairs.

- [ ] **Step 5: Seal and render provisional figures**

~~~bash
python3 -m experiments.eval.flashanns.validate_run --dataset-milestone laion10m --out results/eval/flashanns/readiness/laion-final.json
python3 -m experiments.eval.flashanns.aggregate --datasets laion10m --out results/eval/flashanns/provisional/laion/validated.csv --provenance results/eval/flashanns/provisional/laion/provenance.json
MPLCONFIGDIR=/tmp/flashanns-mpl python3 -m experiments.eval.flashanns.plot_q2_q4 --csv results/eval/flashanns/provisional/laion/validated.csv --out-dir results/eval/flashanns/provisional/laion
~~~

- [ ] **Step 6: Commit only sealed evidence**

~~~bash
git add results/eval/flashanns/calibration/laion10m.json results/eval/flashanns/readiness/laion-proof.json results/eval/flashanns/readiness/laion-final.json
git commit -m "eval: complete LAION integrated evidence"
~~~

---

### Task 14: Aggregate and Freeze the Three Figures

**Files:**
- Produce: `results/eval/flashanns/validated.csv`
- Produce: `results/eval/flashanns/figure-provenance.json`
- Produce: `results/eval/flashanns/figures/q2-main.pdf`
- Produce: `results/eval/flashanns/figures/q3-ablation.pdf`
- Produce: `results/eval/flashanns/figures/q4-cold-warm.pdf`

- [ ] **Step 1: Revalidate every run**

~~~bash
python3 -m experiments.eval.flashanns.validate_run --milestone all-three --datasets t2i10m,yfcc10m,laion10m --out results/eval/flashanns/readiness/all-three.json
~~~

- [ ] **Step 2: Aggregate**

~~~bash
python3 -m experiments.eval.flashanns.aggregate --datasets t2i10m,yfcc10m,laion10m --out results/eval/flashanns/validated.csv --provenance results/eval/flashanns/figure-provenance.json
~~~

- [ ] **Step 3: Render and inspect**

~~~bash
MPLCONFIGDIR=/tmp/flashanns-mpl python3 -m experiments.eval.flashanns.plot_q2_q4 --csv results/eval/flashanns/validated.csv --out-dir results/eval/flashanns/figures
pdfinfo results/eval/flashanns/figures/q2-main.pdf | rg 'Pages|Page size'
pdfinfo results/eval/flashanns/figures/q3-ablation.pdf | rg 'Pages|Page size'
pdfinfo results/eval/flashanns/figures/q4-cold-warm.pdf | rg 'Pages|Page size'
~~~

Expected: three one-page vector PDFs and provenance for every mark.

- [ ] **Step 4: Prove reproducibility**

Regenerate into `/tmp/flashanns-recheck` and require byte-identical CSV and
provenance JSON. PDF hashes are recorded; visual equivalence is checked by plot
tests because PDF metadata may differ.

- [ ] **Step 5: Commit**

~~~bash
git add results/eval/flashanns/validated.csv results/eval/flashanns/figure-provenance.json results/eval/flashanns/figures results/eval/flashanns/readiness/all-three.json
git commit -m "eval: freeze integrated Q2 Q3 Q4 figures"
~~~

---

### Task 15: Insert Figures Without Absorbing Concurrent Paper Edits

**Files:**
- Copy: Q2--Q4 PDFs to `paper/figs/`
- Modify narrowly: `paper/sections/eval.tex`
- Verify only: other `paper/` files

- [ ] **Step 1: Capture the current paper diff**

~~~bash
git diff --binary --output=/tmp/flashanns-paper-before-figures.patch -- paper
sha256sum /tmp/flashanns-paper-before-figures.patch
git status --short -- paper
~~~

- [ ] **Step 2: Copy figure artifacts**

Use distinct stable names:

~~~text
paper/figs/eval-q2-main.pdf
paper/figs/eval-q3-ablation.pdf
paper/figs/eval-q4-cold-warm.pdf
~~~

- [ ] **Step 3: Patch only the three figure blocks and validated table cells**

Replace the `phbox` bodies for labels `fig:eval-main`,
`fig:eval-ablate`, and `fig:eval-scale` with `includegraphics`. Populate
numbers only through the validated CSV/provenance mapping. Update Setup to
declare three 10M datasets, 4 GiB cache, metric per dataset, T=8 final system,
and physical-versus-proxy backend truth.

- [ ] **Step 4: Prove unrelated paper edits survived**

Compare the before patch and final diff. Only the three figure blocks, Setup,
validated table cells, and generated PDFs may be new evaluation changes.

- [ ] **Step 5: Build and commit narrowly**

~~~bash
latexmk -g -pdf -interaction=nonstopmode -halt-on-error paper/main.tex
rg -n "undefined references|Reference .* undefined|Citation .* undefined" paper/main.log
git add paper/sections/eval.tex paper/figs/eval-q2-main.pdf paper/figs/eval-q3-ablation.pdf paper/figs/eval-q4-cold-warm.pdf
git commit -m "paper: insert validated integrated evaluation"
~~~

Do not add other concurrent paper changes.

---

### Task 16: Final Verification

**Files:**
- Produce: `results/eval/flashanns/readiness/final.md`

- [ ] **Step 1: Run complete offline tests**

~~~bash
make -C serving/tests test
python3 -m unittest discover -s experiments/eval/flashanns/tests -v
~~~

- [ ] **Step 2: Audit the freeze**

~~~bash
git diff --function-context 15e6632 -- serving/search_beam.cpp serving/cont_batch.hpp serving/hide_fill.hpp serving/pq_table.hpp serving/prefetch.hpp serving/metrics.hpp
rg -n -- '--early-cl|--lookahead-k|--spec-beam-nbrs|--score-page|--pipe-drive|--admit-gap' experiments/eval/flashanns
~~~

Expected: runtime diff contains only the approved metric adapter and
observation hooks; live configurations contain no removed flag.

- [ ] **Step 3: Revalidate evidence and provenance**

~~~bash
python3 -m experiments.eval.flashanns.validate_run --milestone all-three --datasets t2i10m,yfcc10m,laion10m --out /tmp/flashanns-final-recheck.json
cmp results/eval/flashanns/readiness/all-three.json /tmp/flashanns-final-recheck.json
~~~

- [ ] **Step 4: Write final readiness**

Record commit, accepted/rejected counts and reasons, all dataset/system blocks,
metric identities, 4 GiB preflight evidence, figure hashes, provenance hash,
backend truth label, paper build result, and every spec completion gate.

- [ ] **Step 5: Commit**

~~~bash
git add results/eval/flashanns/readiness/final.md
git commit -m "eval: close integrated FlashANNS evaluation"
~~~

At completion, valid experiment data remain sealed even if later paper prose
changes. Only a runtime, artifact, metric, cache, schema, or aggregation change
invalidates a measured block.
