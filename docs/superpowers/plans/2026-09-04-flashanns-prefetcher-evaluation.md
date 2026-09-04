# FlashANNS Prefetcher-First Evaluation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and execute a reproducible, T=1-only evaluation of the frozen PQ-64 end-batch prefetcher, completing T2I-10M first and then the LAION-10M/YFCC-10M cross-dataset gate without depending on Continuous Batching.

**Architecture:** Add observation-only C++ tracing around the frozen PQ path, then drive it through a Python manifest/preflight/runner/validator pipeline. Core comparisons keep PQ candidates fixed across serial transfer, batched transfer, extent-aware frozen transfer, and the resident Oracle; immutable JSON plus binary sidecars are the only inputs to aggregation and Figure 9.

**Tech Stack:** C++17, GNU Make, Python 3 standard library, JSON, NumPy/Matplotlib for final aggregation and plotting, `/dev/vmem0`, vmem sysfs, NVMe block counters, SHA-256.

**Spec:** `docs/superpowers/specs/2026-09-04-flashanns-prefetcher-evaluation-design.md`

---

## Scope and Hard Stops

- Do not modify the scheduling or selection behavior of `search_one_pq`,
  `PqTable`, `HidePipe`, the PQ-64 codebooks, the ID-to-slot map, or the staged
  extent image at 1100 GiB.
- Do not pass `--cont-batch` or run any T>1 experiment in this plan.
- Do not add the removed optional-lookahead, Blind, score-page, spec-beam, or
  pipe-drive mechanisms back to the frozen path.
- Do not run `insmod`, `rmmod`, storage format/write, image staging, cache flush,
  or backing-device switch commands from this plan.
- Raw live results are not committed. Commit schemas, manifests, tests,
  scripts, validated summaries, and provenance maps only.
- `serving/search_beam.cpp`, `serving/metrics.hpp`, `serving/pq_table.hpp`,
  `serving/tests/Makefile`, and related tests are already dirty at plan-writing
  time. Do not stage or commit those files until Task 0 establishes a
  user-approved baseline; never absorb in-progress Continuous Batching changes
  into a prefetcher commit implicitly.
- The current drafting-time live state reported
  `/sys/class/vmem/vmem0/dirty_bytes=5279744`. Tasks 1--7 may proceed, but Task
  8 and later live commands are blocked until a fresh preflight proves
  `dirty_bytes=0`, `io_errors=0`, no open users, the expected device topology,
  and valid image magic. Cleaning that state requires a separate user-approved
  procedure.

## File Map

| File | Responsibility |
|---|---|
| `serving/eval_trace.hpp` | Buffer and atomically write per-query query IDs, latency, PQ candidates, and final result sidecars. |
| `serving/search_beam.cpp` | Add optional T=1 trace hooks and proof-only PQ-navigation device counters; do not change candidate selection or issue order. |
| `serving/metrics.hpp` | Add event-counted requested/issued/extent page metrics and PQ-navigation NAND bytes. |
| `serving/hide_fill.hpp` | Record counts immediately before and after existing extent expansion. |
| `serving/tests/test_eval_trace.cpp` | Verify sidecar binary formats and T=1 restriction. |
| `serving/tests/test_metrics.cpp` | Verify event page-use math and aggregation. |
| `experiments/eval/prefetcher/config.py` | Load and validate dataset/system/matrix definitions. |
| `experiments/eval/prefetcher/datasets.json` | Known source paths, expected sizes, readiness state, and layout contracts. |
| `experiments/eval/prefetcher/systems.json` | Exact flags for Oracle, serial, batch, frozen, and diagnostic controls. |
| `experiments/eval/prefetcher/freeze_artifacts.py` | Hash immutable artifacts and write a frozen manifest. |
| `experiments/eval/prefetcher/preflight.py` | Read-only VMEM, sysfs, block-counter, process, and image-magic checks. |
| `experiments/eval/prefetcher/run_one.py` | Expand one validated run, capture snapshots, invoke the binary, and seal `run.json`. |
| `experiments/eval/prefetcher/run_matrix.py` | Deterministically expand smoke, calibration, and final T=1 matrices. |
| `experiments/eval/prefetcher/validate_run.py` | Recompute sidecar counts, recall, latency percentiles, and proof gates. |
| `experiments/eval/prefetcher/aggregate.py` | Aggregate matched five-run blocks and bootstrap confidence intervals. |
| `experiments/eval/prefetcher/plot_figure9.py` | Render the revised three-panel prefetcher figure and provenance map. |
| `experiments/eval/prefetcher/schema/run.schema.json` | Declare required run identity, counters, metrics, and sidecars. |
| `experiments/eval/prefetcher/tests/` | Standard-library unit tests using temporary files and fake sysfs trees. |
| `results/eval/prefetcher/` | Generated artifact manifests, raw runs, validated CSV, figure data, and readiness reports. |

---

### Task 0: Establish a Safe Source Baseline

**Files:**
- Inspect: `serving/search_beam.cpp`
- Inspect: `serving/metrics.hpp`
- Inspect: `serving/pq_table.hpp`
- Inspect: `serving/hide_fill.hpp`
- Inspect: `serving/tests/Makefile`
- Produce: `/tmp/flashanns-prefetcher-baseline.patch`

- [ ] **Step 1: Capture the exact dirty state without changing it**

```bash
git status --short --branch
git diff --binary --output=/tmp/flashanns-prefetcher-baseline.patch -- serving/search_beam.cpp serving/metrics.hpp serving/pq_table.hpp serving/hide_fill.hpp serving/tests/Makefile serving/tests/test_metrics.cpp serving/tests/test_pq_table.cpp
sha256sum /tmp/flashanns-prefetcher-baseline.patch
```

Expected: the patch records the existing Continuous Batching/PQ/test edits and
the hash command succeeds. Do not run `git add`, `git stash`, checkout, reset,
or clean.

- [ ] **Step 2: Ask for an explicit baseline decision**

Present the dirty-file list and patch hash. Continue only after the user either
commits/checkpoints those edits or explicitly authorizes one narrowly scoped
checkpoint commit. If the user keeps them uncommitted, Tasks 2--4 must remain
blocked because their per-task commits would otherwise absorb unrelated work.

- [ ] **Step 3: Recheck the approved baseline**

```bash
git status --short --branch
git rev-parse HEAD
sha256sum /tmp/flashanns-prefetcher-baseline.patch
```

Record the approved base commit and whether the original patch is now committed
or intentionally absent. The patch hash is diagnostic evidence only and is not
committed.

---

### Task 1: Lock Dataset and System Configuration

**Files:**
- Create: `experiments/__init__.py`
- Create: `experiments/eval/__init__.py`
- Create: `experiments/eval/prefetcher/__init__.py`
- Create: `experiments/eval/prefetcher/config.py`
- Create: `experiments/eval/prefetcher/datasets.json`
- Create: `experiments/eval/prefetcher/systems.json`
- Create: `experiments/eval/prefetcher/tests/test_config.py`

- [ ] **Step 1: Write the failing configuration tests**

Create `experiments/eval/prefetcher/tests/test_config.py`:

```python
import json
import tempfile
import unittest
from pathlib import Path

from experiments.eval.prefetcher.config import ConfigError, load_configs


class ConfigTest(unittest.TestCase):
    def test_frozen_systems_are_t1_and_have_no_removed_flags(self):
        root = Path(__file__).resolve().parents[4]
        datasets, systems = load_configs(root)
        self.assertTrue(datasets["t2i10m"]["ready"])
        self.assertEqual(systems["pq-frozen"]["threads"], 1)
        flat = " ".join(systems["pq-frozen"]["flags"])
        for forbidden in ("--cont-batch", "--lookahead-k", "--spec-beam-nbrs",
                          "--score-page", "--pipe-drive"):
            self.assertNotIn(forbidden, flat)

    def test_ready_dataset_cannot_omit_required_artifact(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            path = root / "experiments/eval/prefetcher"
            path.mkdir(parents=True)
            (path / "datasets.json").write_text(json.dumps({
                "broken": {"ready": True, "artifacts": {}}
            }))
            (path / "systems.json").write_text("{}")
            with self.assertRaisesRegex(ConfigError, "broken.*missing artifact"):
                load_configs(root)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests and confirm the module is absent**

Run:

```bash
python3 -m unittest experiments.eval.prefetcher.tests.test_config -v
```

Expected: `ModuleNotFoundError: No module named 'experiments'` or missing
`experiments.eval.prefetcher.config`.

- [ ] **Step 3: Add the exact dataset configuration**

Create `datasets.json` with T2I ready and the other datasets explicitly blocked
on missing serving artifacts rather than represented by empty placeholders:

```json
{
  "t2i10m": {
    "ready": true,
    "metric": "mips",
    "n": 10000000,
    "dim": 200,
    "source_dtype": "float32",
    "execution_dtype": "float32",
    "record_stride": 2048,
    "vmem_device": "/dev/vmem0",
    "vmem_offset": 1181116006400,
    "vmem_length": 20480004096,
    "artifacts": {
      "oracle_image": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m.bin",
      "extent_image": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m_extent.bin",
      "graph": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m.graph.bin",
      "slot_map": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/id_to_slot_10m_extent.bin",
      "entry": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/serving_entry_t2i_10m_pagebin.bin",
      "queries": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/query_10k.fbin",
      "ground_truth": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/gt_10k_k10.ibin",
      "id_map": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/new_to_old_pagebin.bin",
      "pq64_pivots": "/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin",
      "pq64_codes": "/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin",
      "pq32_pivots": "/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i_pq_pivots.bin",
      "pq32_codes": "/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i_pq_compressed.bin"
    },
    "expected_sizes": {
      "oracle_image": 20480004096,
      "extent_image": 20480004096,
      "graph": 1280000000,
      "slot_map": 40000000,
      "entry": 1081356,
      "queries": 8000008,
      "ground_truth": 400008,
      "id_map": 40000000,
      "pq64_pivots": 210788,
      "pq64_codes": 640000008
    }
  },
  "laion10m": {
    "ready": false,
    "source_root": "/mnt/disk0/chukexin_motivation/diskann_data_laion25m",
    "blocked_on": ["10M query subset", "recall@10 ground truth", "10M graph", "PQ-64 codebook", "packed extent image", "slot map"]
  },
  "yfcc10m": {
    "ready": false,
    "source_root": "/mnt/disk0/chukexin_motivation/data/yfcc10m",
    "source_files": ["base.10M.u8bin", "query.public.100K.u8bin", "unfiltered.GT.public.ibin"],
    "blocked_on": ["frozen 10K query subset", "10M graph", "PQ-64 codebook", "packed extent image", "slot map"]
  }
}
```

- [ ] **Step 4: Add exact system definitions**

Create `systems.json`:

```json
{
  "pq-oracle": {
    "threads": 1,
    "flags": ["--oracle-dram", "--policy", "P0", "--pq-nav"]
  },
  "pq-serial": {
    "threads": 1,
    "flags": ["--policy", "P3", "--pq-nav", "--no-vmem-prefetch", "--pipe-w", "1", "--no-extent-run", "--shared-window"]
  },
  "pq-batch": {
    "threads": 1,
    "flags": ["--policy", "P3", "--pq-nav", "--pipe-w", "16", "--no-extent-run", "--shared-window"]
  },
  "pq-frozen": {
    "threads": 1,
    "flags": ["--policy", "P3", "--pq-nav", "--pipe-w", "16", "--extent-run", "--shared-window"]
  },
  "legacy-fp-hop": {
    "threads": 1,
    "diagnostic_only": true,
    "flags": ["--policy", "P3", "--oneshot-fp", "--expand-batch", "4", "--issue-ahead", "1", "--no-sync-hop", "--shared-window"]
  },
  "pq32-sensitivity": {
    "threads": 1,
    "diagnostic_only": true,
    "datasets": ["t2i10m"],
    "flags": ["--policy", "P3", "--pq-nav", "--pipe-w", "16", "--extent-run", "--shared-window"]
  }
}
```

All common frozen flags (`--diskann-layout`, `--threads 1`, `--no-hide-warm-entry`,
`--no-direct-install`, `--no-score-cache`, `--no-stripe-fill`, `--k 10`) are
added centrally by the runner so a system cannot silently omit them.

- [ ] **Step 5: Implement configuration validation**

Create `config.py`:

```python
import json
import argparse
from pathlib import Path


class ConfigError(ValueError):
    pass


REQUIRED_ARTIFACTS = {
    "oracle_image", "extent_image", "graph", "slot_map", "entry", "queries", "ground_truth",
    "id_map", "pq64_pivots", "pq64_codes"
}
FORBIDDEN_FLAGS = {
    "--cont-batch", "--lookahead-k", "--spec-beam-nbrs", "--score-page",
    "--pipe-drive"
}


def _read(path):
    with Path(path).open(encoding="utf-8") as handle:
        return json.load(handle)


def load_configs(repo_root):
    base = Path(repo_root) / "experiments/eval/prefetcher"
    datasets = _read(base / "datasets.json")
    systems = _read(base / "systems.json")
    for name, dataset in datasets.items():
        if dataset.get("ready"):
            missing = REQUIRED_ARTIFACTS - set(dataset.get("artifacts", {}))
            if missing:
                raise ConfigError(f"{name}: missing artifact {sorted(missing)}")
    for name, system in systems.items():
        if system.get("threads") != 1:
            raise ConfigError(f"{name}: prefetcher plan requires threads=1")
        bad = FORBIDDEN_FLAGS.intersection(system.get("flags", []))
        if bad:
            raise ConfigError(f"{name}: forbidden flags {sorted(bad)}")
    return datasets, systems


def require_all_ready(datasets):
    blocked = {name: cfg.get("blocked_on", []) for name, cfg in datasets.items()
               if not cfg.get("ready")}
    if blocked:
        raise ConfigError(f"cross-dataset gate blocked: {blocked}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--require-all-ready", action="store_true")
    args = parser.parse_args()
    datasets, _ = load_configs(Path(args.repo_root).resolve())
    if args.require_all_ready:
        require_all_ready(datasets)


if __name__ == "__main__":
    main()
```

- [ ] **Step 6: Run the tests**

Run:

```bash
python3 -m unittest experiments.eval.prefetcher.tests.test_config -v
```

Expected: two tests pass.

- [ ] **Step 7: Commit configuration**

```bash
git add experiments/__init__.py experiments/eval/__init__.py experiments/eval/prefetcher/__init__.py experiments/eval/prefetcher/config.py experiments/eval/prefetcher/datasets.json experiments/eval/prefetcher/systems.json experiments/eval/prefetcher/tests/test_config.py
git commit -m "test: lock prefetcher evaluation configurations"
```

---

### Task 2: Add Stable T=1 Binary Sidecars

**Files:**
- Create: `serving/eval_trace.hpp`
- Create: `serving/tests/test_eval_trace.cpp`
- Modify: `serving/tests/Makefile`

- [ ] **Step 1: Write the failing sidecar test**

Create `serving/tests/test_eval_trace.cpp`:

```cpp
#include "serving/eval_trace.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

int main() {
  const auto dir = std::filesystem::temp_directory_path() / "flashanns-eval-trace-test";
  std::filesystem::remove_all(dir);
  EvalTrace trace(dir.string(), 2);
  trace.add(7, 1000, {3, 5, 9}, {5, 3});
  trace.add(8, 2000, {4}, {4, 1});
  assert(trace.finish());
  assert(std::filesystem::file_size(dir / "query_ids.u32") == 2 * sizeof(uint32_t));
  assert(std::filesystem::file_size(dir / "latency_ns.u64") == 2 * sizeof(uint64_t));
  assert(std::filesystem::file_size(dir / "candidate_offsets.u64") == 3 * sizeof(uint64_t));
  assert(std::filesystem::file_size(dir / "candidate_ids.u32") == 4 * sizeof(uint32_t));
  assert(std::filesystem::file_size(dir / "result_ids.u32") == 4 * sizeof(uint32_t));
  std::filesystem::remove_all(dir);
  std::puts("test_eval_trace OK");
}
```

- [ ] **Step 2: Confirm the header is missing**

Run:

```bash
make -C serving/tests test_eval_trace
```

Expected: compilation fails with `serving/eval_trace.hpp: No such file or directory`.

- [ ] **Step 3: Implement the observation-only writer**

Create `serving/eval_trace.hpp`:

```cpp
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

class EvalTrace {
 public:
  EvalTrace(std::string dir, uint32_t k) : dir_(std::move(dir)), k_(k) {
    candidate_offsets_.push_back(0);
  }

  void add(uint32_t query_id, uint64_t latency_ns,
           const std::vector<uint32_t>& candidates,
           const std::vector<uint32_t>& results) {
    query_ids_.push_back(query_id);
    latency_ns_.push_back(latency_ns);
    candidate_ids_.insert(candidate_ids_.end(), candidates.begin(), candidates.end());
    candidate_offsets_.push_back(candidate_ids_.size());
    for (uint32_t i = 0; i < k_; ++i)
      result_ids_.push_back(i < results.size() ? results[i] : UINT32_MAX);
  }

  bool finish() const {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) return false;
    return write("query_ids.u32", query_ids_) &&
           write("latency_ns.u64", latency_ns_) &&
           write("candidate_offsets.u64", candidate_offsets_) &&
           write("candidate_ids.u32", candidate_ids_) &&
           write("result_ids.u32", result_ids_);
  }

 private:
  template <typename T>
  bool write(const char* name, const std::vector<T>& values) const {
    const auto final = std::filesystem::path(dir_) / name;
    const auto temp = final.string() + ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(values.data()),
              static_cast<std::streamsize>(values.size() * sizeof(T)));
    out.close();
    if (!out) return false;
    std::error_code ec;
    std::filesystem::rename(temp, final, ec);
    return !ec;
  }

  std::string dir_;
  uint32_t k_;
  std::vector<uint32_t> query_ids_;
  std::vector<uint64_t> latency_ns_;
  std::vector<uint64_t> candidate_offsets_;
  std::vector<uint32_t> candidate_ids_;
  std::vector<uint32_t> result_ids_;
};
```

- [ ] **Step 4: Add the Make target**

Add `test_eval_trace` to the `test:` prerequisites and execution list in
`serving/tests/Makefile`, then add:

```makefile
test_eval_trace: test_eval_trace.cpp ../../serving/eval_trace.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_eval_trace.cpp
```

Add `test_eval_trace` to the `clean` removal list.

- [ ] **Step 5: Run the focused and full offline tests**

```bash
make -C serving/tests test_eval_trace
serving/tests/test_eval_trace
make -C serving/tests test
```

Expected: `test_eval_trace OK` and the existing suite passes.

- [ ] **Step 6: Commit sidecar support**

```bash
git add serving/eval_trace.hpp serving/tests/test_eval_trace.cpp serving/tests/Makefile
git commit -m "test: add immutable evaluation sidecars"
```

---

### Task 3: Hook Sidecars into the Frozen PQ Path

**Files:**
- Modify: `serving/search_beam.cpp:356-523`
- Modify: `serving/search_beam.cpp:1106-1114`
- Modify: `serving/search_beam.cpp:2231-2262`
- Modify: `serving/search_beam.cpp:2501-2516`
- Create: `serving/tests/test_eval_trace_mode.cpp`
- Modify: `serving/tests/Makefile`

- [ ] **Step 1: Add a failing T=1-mode validation test**

Add this helper test in `test_eval_trace_mode.cpp`:

```cpp
#include "serving/eval_trace_mode.hpp"
#include <cassert>
#include <cstdio>

int main() {
  assert(eval_trace_mode_valid(1, false));
  assert(!eval_trace_mode_valid(2, false));
  assert(!eval_trace_mode_valid(1, true));
  std::puts("test_eval_trace_mode OK");
}
```

Run `make -C serving/tests test_eval_trace_mode`; expect the missing-header
compile failure.

- [ ] **Step 2: Add the pure mode guard**

Create `serving/eval_trace_mode.hpp`:

```cpp
#pragma once
inline bool eval_trace_mode_valid(int threads, bool cont_batch) {
  return threads == 1 && !cont_batch;
}
```

Add the test target to `serving/tests/Makefile` using the same C++17 flags as
`test_eval_trace`.

- [ ] **Step 3: Add an optional candidate output without changing selection**

Extend `search_one_pq` with a final optional argument:

```cpp
std::vector<uint32_t>* candidate_ids_out = nullptr
```

The Oracle branch returns before `rerank_ids` exists, so immediately before its
`fp_rerank_dram()` call copy the unsorted PQ candidate IDs:

```cpp
    if (candidate_ids_out) {
      candidate_ids_out->clear();
      candidate_ids_out->reserve(cand.size());
      for (const Cand& c : cand) candidate_ids_out->push_back(c.id);
    }
```

In the non-Oracle branch, immediately after constructing `rerank_ids` and
before `issue_ids`, add:

```cpp
  if (candidate_ids_out) *candidate_ids_out = rerank_ids;
```

Extend `search_one` with the same optional argument and pass it only to
`search_one_pq`:

```cpp
  if (!oneshot_fp && pref.pq_nav && pref.pq)
    return search_one_pq(pl, win, pref, eg, qf, beam, k, iters, ext_pool, vio,
                         candidate_ids_out);
```

All non-PQ paths leave the vector empty.

- [ ] **Step 4: Add the trace CLI and fail-closed restrictions**

Include `serving/eval_trace.hpp` and `serving/eval_trace_mode.hpp`. Add:

```cpp
  const char* eval_trace_dir = nullptr;
```

to main's CLI state, parse `--eval-trace-dir`, and after argument parsing add:

```cpp
  if (eval_trace_dir && !eval_trace_mode_valid(nthreads, cont_batch_mode)) {
    fprintf(stderr, "--eval-trace-dir requires --threads 1 and no --cont-batch\n");
    return 2;
  }
```

Construct `std::unique_ptr<EvalTrace> eval_trace` after `nq` and `k` are known.

- [ ] **Step 5: Record original query ID, integer latency, candidates, and mapped results**

Inside `run_one_q`, declare `std::vector<uint32_t> candidates`, pass its address
to `search_one`, and compute integer nanoseconds. Move the existing `id_map`
translation outside the `if (gt_path)` block so result IDs are always written
in the original dataset-ID domain; then compute recall and write:

```cpp
    const uint64_t latency_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tq1 - tq0).count());
    if (eval_trace)
      eval_trace->add(qidx[qi], latency_ns, candidates, ids);
```

Call `eval_trace->finish()` after the T=1 loop and return 2 if it fails. Keep the
existing stdout and return value unchanged.

- [ ] **Step 6: Compile and run offline regression tests**

```bash
make -C serving/tests test_eval_trace test_eval_trace_mode
serving/tests/test_eval_trace
serving/tests/test_eval_trace_mode
make -C serving/tests test
g++ -O3 -std=c++17 -march=native -pthread -I. serving/search_beam.cpp -o serving/search_beam -lnuma
```

Expected: all tests and the serving binary compile. Do not execute the binary on
`/dev/vmem0` in this task.

- [ ] **Step 7: Inspect the frozen-function diff**

```bash
git diff --function-context -- serving/search_beam.cpp serving/eval_trace.hpp
```

Expected: `search_one_pq` has only the optional candidate copy; candidate
insertion, beam termination, page issue, wait, scoring, and sorting statements
are byte-for-byte unchanged.

- [ ] **Step 8: Commit trace hooks**

```bash
git add serving/search_beam.cpp serving/eval_trace_mode.hpp serving/tests/test_eval_trace_mode.cpp serving/tests/Makefile
git commit -m "test: trace frozen PQ candidates and results"
```

---

### Task 4: Correct Event Page Accounting and Add Proof-Only Phase Counters

**Files:**
- Modify: `serving/metrics.hpp:7-196`
- Modify: `serving/hide_fill.hpp:194-223`
- Modify: `serving/search_beam.cpp:356-495`
- Modify: `serving/tests/test_metrics.cpp`

- [ ] **Step 1: Write failing event-accounting assertions**

Append to `test_metrics.cpp` before its final print:

```cpp
  Metrics evt;
  evt.note_pf_issue_event(7, 10);
  evt.note_pf_issue_event(3, 3);
  assert(evt.pf_requested_page_events == 10);
  assert(evt.pf_issued_page_events == 13);
  assert(evt.pf_extent_extra_page_events == 3);
  assert(evt.prefetch_event_page_use_pct() > 76.9);
  assert(evt.prefetch_event_page_use_pct() < 77.0);
  evt.pq_nav_nand_bytes = 512;
  Metrics evt2;
  evt2.note_pf_issue_event(1, 2);
  evt2.pq_nav_nand_bytes = 1024;
  evt.add_from(evt2);
  assert(evt.pf_issued_page_events == 15);
  assert(evt.pq_nav_nand_bytes == 1536);
```

Run `make -C serving/tests test_metrics`; expect missing members/methods.

- [ ] **Step 2: Add additive counters to `Metrics`**

Add:

```cpp
  uint64_t pf_requested_page_events = 0;
  uint64_t pf_issued_page_events = 0;
  uint64_t pf_extent_extra_page_events = 0;
  uint64_t pq_nav_nand_bytes = 0;

  void note_pf_issue_event(uint64_t requested, uint64_t issued) {
    pf_requested_page_events += requested;
    pf_issued_page_events += issued;
    if (issued > requested) pf_extent_extra_page_events += issued - requested;
  }
  double prefetch_event_page_use_pct() const {
    return pf_issued_page_events
        ? 100.0 * static_cast<double>(pf_requested_page_events) /
              static_cast<double>(pf_issued_page_events)
        : 0.0;
  }
```

Merge all four counters in `add_from` and print them on a new stable line:

```cpp
fprintf(f, "prefetch_events requested=%llu issued=%llu extent_extra=%llu page_use_pct=%.2f pq_nav_nand_B=%llu\n",
        (unsigned long long)pf_requested_page_events,
        (unsigned long long)pf_issued_page_events,
        (unsigned long long)pf_extent_extra_page_events,
        prefetch_event_page_use_pct(),
        (unsigned long long)pq_nav_nand_bytes);
```

- [ ] **Step 3: Count before and after the existing extent transform**

In `hide_issue`, after residency/deduplication and before `hide_extent_run`,
save:

```cpp
  const uint64_t requested_pages = miss.size();
```

Immediately after the existing `hide_extent_run` call, add:

```cpp
  if (m) m->note_pf_issue_event(requested_pages, miss.size());
```

Do not reorder or alter the existing extent call or `miss` contents.

- [ ] **Step 4: Add proof-only PQ-navigation counter reads**

Parse `--eval-phase-counters` into `bool eval_phase_counters = false` and reject
it unless the run is T=1, PQ navigation is enabled, and `--eval-trace-dir` is
also present.

In `search_one_pq`, capture the backing-sector sum immediately before the PQ
beam loop and immediately after it:

```cpp
  const uint64_t pq_sect0 = eval_phase_counters ? nvme_read_sectors() : 0;
  // existing PQ beam loop remains here
  if (eval_phase_counters) {
    const uint64_t pq_sect1 = nvme_read_sectors();
    if (pq_sect1 >= pq_sect0 && cur_met(win))
      cur_met(win)->pq_nav_nand_bytes += (pq_sect1 - pq_sect0) * 512ull;
  }
```

Pass the boolean as an optional final argument through `search_one`. The proof
flag is used only for 100-query correctness runs because per-query sysfs reads
perturb timing. Final measured runs must reject it in `run_one.py`.

- [ ] **Step 5: Run tests and inspect the scheduling diff**

```bash
make -C serving/tests test_metrics
serving/tests/test_metrics
make -C serving/tests test
g++ -O3 -std=c++17 -march=native -pthread -I. serving/search_beam.cpp -o serving/search_beam -lnuma
git diff --function-context -- serving/hide_fill.hpp serving/search_beam.cpp
```

Expected: tests pass; `hide_issue` has only count snapshots around the existing
extent transform; the PQ loop has only optional before/after counter reads.

- [ ] **Step 6: Commit metrics**

```bash
git add serving/metrics.hpp serving/hide_fill.hpp serving/search_beam.cpp serving/tests/test_metrics.cpp
git commit -m "test: account frozen prefetch issue events"
```

---

### Task 5: Freeze Artifact Identity

**Files:**
- Create: `experiments/eval/prefetcher/freeze_artifacts.py`
- Create: `experiments/eval/prefetcher/tests/test_freeze_artifacts.py`
- Produce: `results/eval/prefetcher/manifests/t2i10m-artifacts.json`

- [ ] **Step 1: Write the failing hash test**

Use two temporary files and assert that `freeze_dataset()` records absolute
path, size, and SHA-256, and rejects an expected-size mismatch with
`ArtifactError`.

```python
from experiments.eval.prefetcher.freeze_artifacts import ArtifactError, hash_file

self.assertEqual(hash_file(path)["size"], len(payload))
self.assertEqual(hash_file(path)["sha256"], hashlib.sha256(payload).hexdigest())
```

Run the focused unittest; expect an import failure.

- [ ] **Step 2: Implement streaming hashing**

Create `freeze_artifacts.py` with:

```python
import argparse
import hashlib
import json
from pathlib import Path

from experiments.eval.prefetcher.config import load_configs


class ArtifactError(RuntimeError):
    pass


def hash_file(path):
    path = Path(path).resolve()
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as handle:
        while True:
            block = handle.read(8 << 20)
            if not block:
                break
            size += len(block)
            digest.update(block)
    return {"path": str(path), "size": size, "sha256": digest.hexdigest()}


def freeze_dataset(dataset, output):
    frozen = {}
    for name, path in sorted(dataset["artifacts"].items()):
        record = hash_file(path)
        expected = dataset.get("expected_sizes", {}).get(name)
        if expected is not None and record["size"] != expected:
            raise ArtifactError(f"{name}: size {record['size']} != {expected}")
        frozen[name] = record
    target = Path(output)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps({"artifacts": frozen}, indent=2, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    datasets, _ = load_configs(Path(args.repo_root).resolve())
    if args.dataset not in datasets or not datasets[args.dataset].get("ready"):
        raise ArtifactError(f"dataset is not ready: {args.dataset}")
    freeze_dataset(datasets[args.dataset], args.out)


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Run unit tests**

```bash
python3 -m unittest experiments.eval.prefetcher.tests.test_freeze_artifacts -v
```

Expected: hash and mismatch tests pass.

- [ ] **Step 4: Freeze T2I artifacts outside a timed run**

```bash
python3 -m experiments.eval.prefetcher.freeze_artifacts \
  --repo-root /root/chukexin/CXL-ANNS-KX \
  --dataset t2i10m \
  --out results/eval/prefetcher/manifests/t2i10m-artifacts.json
```

Expected: all required files match their expected sizes and receive SHA-256
records. This command may take several minutes for the 20.48 GB image; run it
once and reuse the sealed manifest.

- [ ] **Step 5: Commit code and the small manifest**

```bash
git add experiments/eval/prefetcher/freeze_artifacts.py experiments/eval/prefetcher/tests/test_freeze_artifacts.py results/eval/prefetcher/manifests/t2i10m-artifacts.json
git commit -m "test: freeze T2I prefetcher artifacts"
```

---

### Task 6: Add Fail-Closed Read-Only Live Preflight

**Files:**
- Create: `experiments/eval/prefetcher/preflight.py`
- Create: `experiments/eval/prefetcher/t2i-live-contract.json`
- Create: `experiments/eval/prefetcher/tests/test_preflight.py`

- [ ] **Step 1: Write fake-sysfs failures first**

Create tests that build a temporary `vmem0` directory and verify these exact
rejections independently: nonzero `dirty_bytes`, nonzero `io_errors`, cache
limit not equal to 104857600, backing-device mismatch, BDF mismatch, nonempty
open-user list, bad image magic, and sampled extent-image mismatch.

Use this base fixture:

```python
BASE = {
    "backend": "software",
    "backing_count": "2",
    "nvme_dev": "/dev/nvme1n1,/dev/nvme2n1",
    "target_bdf": "0000:d8:00.0,0000:d9:00.0",
    "cache_limit": "104857600",
    "cache_used": "0",
    "dirty_bytes": "0",
    "io_errors": "0",
    "evictions": "0"
}
```

Run the tests; expect the missing-module failure.

- [ ] **Step 2: Define the current identity contract without claiming layout validity**

Create `t2i-live-contract.json`:

```json
{
  "device": "/dev/vmem0",
  "sysfs": "/sys/class/vmem/vmem0",
  "backend": "software",
  "backing_count": 2,
  "nvme_dev": ["/dev/nvme1n1", "/dev/nvme2n1"],
  "target_bdf": ["0000:d8:00.0", "0000:d9:00.0"],
  "cache_limit": 104857600,
  "required_dirty_bytes": 0,
  "required_io_errors": 0,
  "image_offset": 1181116006400,
  "image_magic": "CXAN",
  "extent_image": "/mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m_extent.bin",
  "image_length": 20480004096,
  "sample_pages": 32,
  "sample_seed": 20260904
}
```

This contract records the observed topology. The preflight must still read and
validate image magic before declaring the staged layout usable.

- [ ] **Step 3: Implement read-only snapshot and validation**

```python
import argparse
import hashlib
import json
import os
import random
import subprocess
from datetime import datetime, timezone
from pathlib import Path


class PreflightError(RuntimeError):
    pass


def probe_users(device):
    proc = subprocess.run(["fuser", device], text=True, capture_output=True,
                          check=False)
    return (proc.stdout + " " + proc.stderr).split()


def probe_magic(device, image_offset):
    fd = os.open(device, os.O_RDONLY)
    try:
        return os.pread(fd, 4, image_offset).decode("ascii", errors="replace")
    finally:
        os.close(fd)


def snapshot(sysfs, device, image_offset, read_device=True,
             user_probe=probe_users, magic_probe=probe_magic):
    fields = ("backend", "backing_count", "nvme_dev", "target_bdf",
              "cache_limit", "cache_used", "dirty_bytes", "io_errors", "evictions")
    values = {name: Path(sysfs, name).read_text().strip() for name in fields}
    users = user_probe(device)
    magic = magic_probe(device, image_offset) if read_device else None
    return {"fields": values, "open_users": users, "image_magic": magic}


def validate(snapshot_record, contract, check_image=True):
    errors = []
    fields = snapshot_record["fields"]
    if fields["dirty_bytes"] != str(contract["required_dirty_bytes"]):
        errors.append(f"dirty_bytes={fields['dirty_bytes']}")
    if fields["io_errors"] != str(contract["required_io_errors"]):
        errors.append(f"io_errors={fields['io_errors']}")
    if fields["cache_limit"] != str(contract["cache_limit"]):
        errors.append(f"cache_limit={fields['cache_limit']}")
    if snapshot_record["open_users"]:
        errors.append(f"open_users={snapshot_record['open_users']}")
    if check_image and snapshot_record["image_magic"] != contract["image_magic"]:
        errors.append(f"image_magic={snapshot_record['image_magic']!r}")
    for key in ("backend", "backing_count", "nvme_dev", "target_bdf"):
        expected = contract[key]
        actual = fields[key]
        if isinstance(expected, list):
            actual = [item for item in actual.split(",") if item]
        elif isinstance(expected, int):
            actual = int(actual)
        if actual != expected:
            errors.append(f"{key}={actual!r}")
    if errors:
        raise PreflightError("; ".join(errors))


def sampled_layout_digest(path, base_offset, image_length, pages, seed):
    rng = random.Random(seed)
    offsets = sorted(rng.sample(range(image_length // 4096), pages))
    digest = hashlib.sha256()
    fd = os.open(path, os.O_RDONLY)
    try:
        for page in offsets:
            data = os.pread(fd, 4096, base_offset + page * 4096)
            if len(data) != 4096:
                raise PreflightError(f"short sampled page {page}")
            digest.update(page.to_bytes(8, "little"))
            digest.update(data)
    finally:
        os.close(fd)
    return digest.hexdigest()


def snapshot_and_validate(contract, require_cache_used=None, read_device=True,
                          identity_evidence=None):
    record = snapshot(contract["sysfs"], contract["device"],
                      contract["image_offset"], read_device=read_device)
    validate(record, contract, check_image=read_device)
    record["captured_utc"] = datetime.now(timezone.utc).isoformat()
    if require_cache_used is not None:
        actual = int(record["fields"]["cache_used"])
        if actual != require_cache_used:
            raise PreflightError(f"cache_used={actual}, required={require_cache_used}")
    if not read_device:
        if not identity_evidence or "layout_sample_sha256" not in identity_evidence:
            raise PreflightError("sysfs-only check requires layout identity evidence")
        record["identity_evidence"] = identity_evidence["layout_sample_sha256"]
        return record
    host_digest = sampled_layout_digest(contract["extent_image"], 0,
                                        contract["image_length"],
                                        contract["sample_pages"],
                                        contract["sample_seed"])
    device_digest = sampled_layout_digest(contract["device"],
                                          contract["image_offset"],
                                          contract["image_length"],
                                          contract["sample_pages"],
                                          contract["sample_seed"])
    if device_digest != host_digest:
        raise PreflightError("sampled extent image mismatch")
    record["layout_sample_sha256"] = device_digest
    return record


def atomic_json_write(path, value):
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_suffix(target.suffix + ".tmp")
    temp.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temp.replace(target)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--contract", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--require-cache-used", type=int)
    parser.add_argument("--allow-cache-used-nonzero", action="store_true")
    parser.add_argument("--sysfs-only", action="store_true")
    parser.add_argument("--identity-evidence")
    args = parser.parse_args()
    contract = json.loads(Path(args.contract).read_text())
    required = None if args.allow_cache_used_nonzero else args.require_cache_used
    identity = (json.loads(Path(args.identity_evidence).read_text())
                if args.identity_evidence else None)
    record = snapshot_and_validate(contract, required,
                                   read_device=not args.sysfs_only,
                                   identity_evidence=identity)
    atomic_json_write(args.out, record)


if __name__ == "__main__":
    main()
```

The CLI writes a JSON snapshot only after validation; it never mutates sysfs or
the device.

- [ ] **Step 4: Run fake-sysfs tests**

```bash
python3 -m unittest experiments.eval.prefetcher.tests.test_preflight -v
```

Expected: all independent rejection tests pass.

- [ ] **Step 5: Run the live preflight and preserve the expected failure**

```bash
python3 -m experiments.eval.prefetcher.preflight \
  --contract experiments/eval/prefetcher/t2i-live-contract.json \
  --out results/eval/prefetcher/preflight/latest.json
```

Expected at the drafting-time state: exit nonzero and report
`dirty_bytes=5279744`. Do not clean, unload, or reset anything in response.

- [ ] **Step 6: Commit preflight code, tests, and contract**

```bash
git add experiments/eval/prefetcher/preflight.py experiments/eval/prefetcher/t2i-live-contract.json experiments/eval/prefetcher/tests/test_preflight.py
git commit -m "test: add fail-closed VMEM preflight"
```

---

### Task 7: Build the Manifest Runner and Run Validator

**Files:**
- Create: `experiments/eval/prefetcher/matrix.json`
- Create: `experiments/eval/prefetcher/schema/run.schema.json`
- Create: `experiments/eval/prefetcher/run_one.py`
- Create: `experiments/eval/prefetcher/run_matrix.py`
- Create: `experiments/eval/prefetcher/validate_run.py`
- Create: `experiments/eval/prefetcher/tests/test_runner.py`
- Create: `experiments/eval/prefetcher/tests/test_validate_run.py`

- [ ] **Step 1: Write failing dry-run expansion tests**

Assert that smoke expansion produces exactly four T2I systems, 100 queries,
`L=400`, one thread, trace output, and phase counters; calibration expands
`L={50,100,200,400,800,1600}`; final mode has 10,000 queries and five repeats;
no command contains `--cont-batch` or a removed flag.

```python
commands = expand_matrix("smoke", datasets, systems)
self.assertEqual([c["system"] for c in commands],
                 ["pq-oracle", "pq-serial", "pq-batch", "pq-frozen"])
self.assertTrue(all(c["nq"] == 100 and c["threads"] == 1 for c in commands))
```

- [ ] **Step 2: Define the matrix**

Create `matrix.json`:

```json
{
  "seed": 20260904,
  "query_seed": 42,
  "k": 10,
  "base_L": [50, 100, 200, 400, 800, 1600],
  "extended_L": [2400, 3200],
  "smoke": {"datasets": ["t2i10m"], "systems": ["pq-oracle", "pq-serial", "pq-batch", "pq-frozen"], "nq": 100, "repeats": 1, "L": [400]},
  "calibration": {"datasets": ["t2i10m"], "systems": ["pq-oracle", "pq-serial", "pq-batch", "pq-frozen", "pq32-sensitivity"], "nq": 500, "repeats": 1},
  "final_t2i": {"datasets": ["t2i10m"], "systems": ["pq-oracle", "pq-serial", "pq-batch", "pq-frozen"], "nq": 10000, "repeats": 5},
  "final_all": {"datasets": ["laion10m", "t2i10m", "yfcc10m"], "systems": ["pq-oracle", "pq-serial", "pq-batch", "pq-frozen"], "nq": 10000, "repeats": 5}
}
```

- [ ] **Step 3: Define required run fields**

The schema must require these top-level keys:

```json
{
  "required": ["run_id", "state", "dataset", "system", "L", "k", "nq", "repeat", "command", "git", "binary_sha256", "artifact_manifest_sha256", "preflight_before", "preflight_after", "device_before", "device_after", "metrics", "sidecars", "validation"],
  "properties": {
    "state": {"enum": ["cold", "warm", "proof"]},
    "system": {"enum": ["pq-oracle", "pq-serial", "pq-batch", "pq-frozen"]},
    "validation": {"required": ["status", "reasons"]}
  }
}
```

Implement explicit Python validation rather than silently skipping checks when
the optional `jsonschema` package is unavailable.

- [ ] **Step 4: Implement deterministic command expansion**

`run_matrix.py` must use `random.Random(seed)` and shuffle systems only within
each `(dataset,L,repeat,state)` block. `run_one.py` must assemble common flags
once and append system flags, dataset paths, PQ paths, `--eval-trace-dir`, and
`--eval-phase-counters` only for proof runs.

The T2I frozen command produced by dry-run must contain:

```text
serving/search_beam --diskann-layout --vmem-dev /dev/vmem0
--vmem-offset 1181116006400 --vmem-len 20480004096
--graph-file /mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m.graph.bin
--id-slot-map /mnt/disk0/chukexin_motivation/serving_t2i_10m/id_to_slot_10m_extent.bin
--pq-nav --pq-pivots /mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin
--pq-compressed /mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin
--beam 400 --k 10 --threads 1 --policy P3 --pipe-w 16 --extent-run
--no-hide-warm-entry --no-direct-install --no-score-cache --no-stripe-fill
--max-q 100 --shuffle-seed 42
--eval-trace-dir /root/chukexin/CXL-ANNS-KX/results/eval/prefetcher/raw/proof/t2i10m.pq-frozen.L400.r0/trace
```

For `pq-oracle`, replace VMEM arguments with `--image` set to
`artifacts.oracle_image` and `--oracle-dram`.
For `pq-serial`, add `--no-vmem-prefetch --pipe-w 1 --no-extent-run`. For
`pq-batch`, use `--pipe-w 16 --no-extent-run`.

- [ ] **Step 5: Seal one run record**

`run_one.py` performs this order exactly:

```python
def read_block_counters(snapshot_record):
    totals = {"reads_completed": 0, "sectors_read": 0}
    for device in snapshot_record["fields"]["nvme_dev"].split(","):
        name = Path(device.strip()).name
        values = Path("/sys/block", name, "stat").read_text().split()
        totals["reads_completed"] += int(values[0])
        totals["sectors_read"] += int(values[2])
    return totals


before = preflight.snapshot_and_validate(contract, read_device=False,
                                         identity_evidence=identity_evidence)
device_before = read_block_counters(before)
completed = subprocess.run(command, text=True, stdout=log, stderr=subprocess.STDOUT,
                           check=False)
after = preflight.snapshot_and_validate(contract, read_device=False,
                                        identity_evidence=identity_evidence)
device_after = read_block_counters(after)
record = build_record(run_spec, command, completed.returncode, before, after,
                      device_before, device_after, run_dir)
preflight.atomic_json_write(run_dir / "run.json", record)
```

`build_record` receives the fully expanded `run_spec` and must populate every
schema-required identity field, parse the stable metric lines from
`stdout.log`, compute block deltas as `after-before`, and record sidecar paths,
sizes, and SHA-256 hashes. It sets `validation.status="pending"`; only
`validate_run.py` may change that state to `accepted` or `rejected`.

`identity_evidence` is the last full sampled-layout preflight record. Timed-run
snapshots are sysfs-only so validation itself does not warm a VMEM data page.
The runner never invokes a reset command. A cold run requires a user-supplied,
validator-approved reset snapshot with `cache_used=0`; a warm run must directly
follow its cold run and cite the cold `run_id`.

The `run_one.py` CLI requires `--dataset`, `--system`, `--L`, `--nq`,
`--repeat`, `--state`, `--identity-evidence`, `--cold-evidence`, and `--out`.
It rejects reused cold evidence by recording its SHA-256 in `run.json` and
checking that no existing run under the output root contains the same hash.

- [ ] **Step 6: Implement sidecar validation**

`validate_run.py` must read little-endian arrays using `array.array`, check exact
lengths (`nq`, `nq`, `nq+1`, `offsets[-1]`, `nq*k`), recompute percentiles using
the same linear interpolation as C++, recompute recall from the dataset GT and
query IDs, hash every sidecar, and reject:

```python
if record["metrics"]["score_from_bounce"] != 0:
    reasons.append("bounce scoring is nonzero")
if system == "pq-oracle" and nand_bytes != 0:
    reasons.append("oracle NAND bytes are nonzero")
if state == "proof" and record["metrics"]["pq_nav_nand_bytes"] != 0:
    reasons.append("PQ navigation touched NAND")
if record["nq"] != len(query_ids):
    reasons.append("query count mismatch")
```

Provide `compare_transfer_runs(paths)` that requires identical dataset,
artifact manifest, query IDs, `L`, candidate offsets, candidate IDs, result
IDs, and recall across `pq-serial`, `pq-batch`, and `pq-frozen`.

- [ ] **Step 7: Run unit tests and dry-run**

```bash
python3 -m unittest discover -s experiments/eval/prefetcher/tests -v
python3 -m experiments.eval.prefetcher.run_matrix --phase smoke --dry-run
```

Expected: tests pass; dry-run prints four commands and performs no device open,
process launch, reset, or output mutation outside its temporary expansion file.

- [ ] **Step 8: Commit runner and validator**

```bash
git add experiments/eval/prefetcher/matrix.json experiments/eval/prefetcher/schema/run.schema.json experiments/eval/prefetcher/run_one.py experiments/eval/prefetcher/run_matrix.py experiments/eval/prefetcher/validate_run.py experiments/eval/prefetcher/tests/test_runner.py experiments/eval/prefetcher/tests/test_validate_run.py
git commit -m "test: add manifest-driven prefetcher runner"
```

---

### Task 8: Run the T2I-10M 100-Query Proof Gate

**Files:**
- Produce: `results/eval/prefetcher/raw/proof/`
- Produce: `results/eval/prefetcher/readiness/t2i-proof.json`

- [ ] **Step 1: Re-run the live preflight**

```bash
python3 -m experiments.eval.prefetcher.preflight \
  --contract experiments/eval/prefetcher/t2i-live-contract.json \
  --out results/eval/prefetcher/preflight/proof-before.json
```

Expected: PASS with `dirty_bytes=0`, `io_errors=0`, empty `open_users`, exact
dual-device identity, 100 MiB cache limit, and `image_magic=CXAN`.

If it fails, stop. Report the observed field and request a separately approved
recovery/reset procedure; do not run the matrix.

- [ ] **Step 2: Capture one approved cold-reset snapshot per transfer mode**

Run the separately approved cold-state procedure, then capture:

```bash
python3 -m experiments.eval.prefetcher.preflight --contract experiments/eval/prefetcher/t2i-live-contract.json --require-cache-used 0 --sysfs-only --identity-evidence results/eval/prefetcher/preflight/proof-before.json --out results/eval/prefetcher/preflight/proof-cold-pq-oracle.json
python3 -m experiments.eval.prefetcher.preflight --contract experiments/eval/prefetcher/t2i-live-contract.json --require-cache-used 0 --sysfs-only --identity-evidence results/eval/prefetcher/preflight/proof-before.json --out results/eval/prefetcher/preflight/proof-cold-pq-serial.json
python3 -m experiments.eval.prefetcher.preflight --contract experiments/eval/prefetcher/t2i-live-contract.json --require-cache-used 0 --sysfs-only --identity-evidence results/eval/prefetcher/preflight/proof-before.json --out results/eval/prefetcher/preflight/proof-cold-pq-batch.json
python3 -m experiments.eval.prefetcher.preflight --contract experiments/eval/prefetcher/t2i-live-contract.json --require-cache-used 0 --sysfs-only --identity-evidence results/eval/prefetcher/preflight/proof-before.json --out results/eval/prefetcher/preflight/proof-cold-pq-frozen.json
```

Expected: `cache_used=0` and every normal preflight gate passes. This plan does
not define or authorize the reset operation itself. Run one command, then its
matching Step 3 system command, then perform the next separately approved reset;
do not capture all four snapshots from one reset.

- [ ] **Step 3: Execute the randomized four-system proof matrix**

```bash
python3 -m experiments.eval.prefetcher.run_one --dataset t2i10m --system pq-oracle --L 400 --nq 100 --repeat 0 --state proof --identity-evidence results/eval/prefetcher/preflight/proof-before.json --cold-evidence results/eval/prefetcher/preflight/proof-cold-pq-oracle.json --out results/eval/prefetcher/raw/proof
python3 -m experiments.eval.prefetcher.run_one --dataset t2i10m --system pq-serial --L 400 --nq 100 --repeat 0 --state proof --identity-evidence results/eval/prefetcher/preflight/proof-before.json --cold-evidence results/eval/prefetcher/preflight/proof-cold-pq-serial.json --out results/eval/prefetcher/raw/proof
python3 -m experiments.eval.prefetcher.run_one --dataset t2i10m --system pq-batch --L 400 --nq 100 --repeat 0 --state proof --identity-evidence results/eval/prefetcher/preflight/proof-before.json --cold-evidence results/eval/prefetcher/preflight/proof-cold-pq-batch.json --out results/eval/prefetcher/raw/proof
python3 -m experiments.eval.prefetcher.run_one --dataset t2i10m --system pq-frozen --L 400 --nq 100 --repeat 0 --state proof --identity-evidence results/eval/prefetcher/preflight/proof-before.json --cold-evidence results/eval/prefetcher/preflight/proof-cold-pq-frozen.json --out results/eval/prefetcher/raw/proof
```

Expected: four completed runs, each with 100 queries and all five sidecars.

- [ ] **Step 4: Validate the proof and transfer equality**

```bash
python3 -m experiments.eval.prefetcher.validate_run \
  --compare-transfer \
  results/eval/prefetcher/raw/proof/*/run.json \
  --out results/eval/prefetcher/readiness/t2i-proof.json
```

Expected:

- identical PQ candidate sidecars for all four systems;
- identical final result sidecars for serial, batch, and frozen;
- identical transfer-mode recall;
- `pq_nav_nand_bytes=0`;
- `pq-frozen score_from_bounce=0`;
- Oracle timed NAND bytes equal zero;
- no mandatory-page, identity, live-state, or sidecar rejection.

- [ ] **Step 5: Verify post-run safety state**

```bash
python3 -m experiments.eval.prefetcher.preflight \
  --contract experiments/eval/prefetcher/t2i-live-contract.json \
  --allow-cache-used-nonzero \
  --out results/eval/prefetcher/preflight/proof-after.json
```

Expected: `dirty_bytes=0`, `io_errors=0`, empty `open_users`, and unchanged
device/image identity. Nonzero dirty bytes blocks every later live task.

- [ ] **Step 6: Commit only the readiness summary**

```bash
git add results/eval/prefetcher/readiness/t2i-proof.json
git commit -m "test: record T2I prefetcher proof gate"
```

Do not add `results/eval/prefetcher/raw/proof/`.

---

### Task 9: Calibrate Recall and Complete T2I Milestone A

**Files:**
- Produce: `results/eval/prefetcher/raw/calibration/t2i10m/`
- Produce: `results/eval/prefetcher/calibration/t2i10m.json`
- Produce: `results/eval/prefetcher/raw/final/t2i10m/`
- Produce: `results/eval/prefetcher/readiness/t2i-milestone-a.json`

- [ ] **Step 1: Run the 500-query L sweep**

After a passing Task 8 post-run preflight and an approved cold reset:

```bash
python3 -m experiments.eval.prefetcher.run_matrix \
  --phase calibration \
  --state cold \
  --out results/eval/prefetcher/raw/calibration/t2i10m
```

Expected: each core system has `L=50,100,200,400,800,1600`; PQ-32 sensitivity
is retained even if its recall is below 0.90.

- [ ] **Step 2: Freeze anchors without plot-time selection**

```bash
python3 -m experiments.eval.prefetcher.validate_run \
  --freeze-anchor 0.90 \
  --extra-anchor t2i10m=0.92 \
  results/eval/prefetcher/raw/calibration/t2i10m/*/run.json \
  --out results/eval/prefetcher/calibration/t2i10m.json
```

Expected: a measured `L` at or above both anchors. If the base sweep misses an
anchor, run only the predeclared 2400/3200 extension and append it before
freezing.

- [ ] **Step 3: Run five cold repetitions and paired warm passes**

```bash
python3 -m experiments.eval.prefetcher.run_matrix \
  --phase final_t2i \
  --anchors results/eval/prefetcher/calibration/t2i10m.json \
  --paired-cold-warm \
  --out results/eval/prefetcher/raw/final/t2i10m
```

Expected: for every core system/anchor, five randomized 10k-query cold runs and
five immediate warm companions. Each cold run requires its own validated reset
evidence; the runner never performs the reset.

- [ ] **Step 4: Validate Milestone A**

```bash
python3 -m experiments.eval.prefetcher.validate_run \
  --milestone t2i-a \
  results/eval/prefetcher/raw/final/t2i10m/*/run.json \
  --out results/eval/prefetcher/readiness/t2i-milestone-a.json
```

Expected: every block has five accepted cold runs, paired warm runs, identical
transfer-mode candidates/results, and independently recomputed recall and
latency statistics.

- [ ] **Step 5: Commit calibration and readiness summaries only**

```bash
git add results/eval/prefetcher/calibration/t2i10m.json results/eval/prefetcher/readiness/t2i-milestone-a.json
git commit -m "test: complete T2I prefetcher milestone"
```

---

### Task 10: Admit LAION-10M and YFCC-10M

**Files:**
- Modify: `experiments/eval/prefetcher/datasets.json`
- Create: `experiments/eval/prefetcher/verify_dataset.py`
- Create: `experiments/eval/prefetcher/tests/test_verify_dataset.py`
- Produce: `results/eval/prefetcher/manifests/laion10m-artifacts.json`
- Produce: `results/eval/prefetcher/manifests/yfcc10m-artifacts.json`

- [ ] **Step 1: Add synthetic header/readback tests**

Test `u8bin`, `fbin`, and `ibin` header parsing, exact file-length validation,
native dimension/dtype preservation, 10k query-subset uniqueness, GT row
selection, slot-map permutation bounds, and 1,024 deterministic packed-record
readbacks.

Run the focused unittest and confirm the verifier is initially absent.

- [ ] **Step 2: Implement the dataset verifier**

Expose these checked interfaces:

```python
def read_header(path, dtype_size):
    with Path(path).open("rb") as handle:
        n, dim = struct.unpack("<II", handle.read(8))
    expected = 8 + n * dim * dtype_size
    if Path(path).stat().st_size != expected:
        raise DatasetError(f"{path}: size mismatch")
    return n, dim


def validate_query_ids(ids, available):
    if len(ids) != 10000 or len(set(ids)) != 10000:
        raise DatasetError("query subset must contain 10000 unique IDs")
    if min(ids) < 0 or max(ids) >= available:
        raise DatasetError("query subset ID out of range")


def validate_slot_map(values, n):
    if len(values) != n or set(values) != set(range(n)):
        raise DatasetError("slot map is not a permutation")
```

The readback verifier compares vector bytes, neighbor count, and neighbor IDs
for IDs produced by `random.Random(20260904).sample(range(10000000), 1024)`.
Its CLI accepts `--source-only`, `--base`, `--queries`, optional `--gt`, and
`--allow-missing-gt`. The last option validates source headers only and can
never change a dataset's `ready` field.

- [ ] **Step 3: Validate known source files before admitting generated artifacts**

```bash
python3 -m experiments.eval.prefetcher.verify_dataset \
  --source-only yfcc10m \
  --base /mnt/disk0/chukexin_motivation/data/yfcc10m/base.10M.u8bin \
  --queries /mnt/disk0/chukexin_motivation/data/yfcc10m/query.public.100K.u8bin \
  --gt /mnt/disk0/chukexin_motivation/data/yfcc10m/unfiltered.GT.public.ibin
```

Expected: 10M x 192 `uint8`, 100k queries, top-100 GT, L2 metric contract.

Run the LAION source-only check explicitly:

```bash
python3 -m experiments.eval.prefetcher.verify_dataset \
  --source-only laion10m \
  --base /mnt/disk0/chukexin_motivation/diskann_data_laion25m/base.bin \
  --queries /mnt/disk0/chukexin_motivation/diskann_data_laion25m/query.bin \
  --allow-missing-gt
```

Expected: source header `25,000,000 x 512 float32` and query header
`50,000 x 512 float32`. This proves the source representation only; the first
10M subset, metric, query subset, GT, graph, PQ-64 files, and packed layout
remain blocked until independently verified.

- [ ] **Step 4: Stop if upstream serving artifacts are absent**

The overall Evaluation Task 1 must provide, per dataset, an Oracle image, a 10M
graph, frozen 10k query IDs and GT, PQ-64 pivots/codes, packed extent image,
entry file, ID map, and slot map. This plan does not invent their paths or silently build
different indexes.

Run:

```bash
python3 -m experiments.eval.prefetcher.config --require-all-ready
```

Expected until those artifacts exist: a nonzero exit listing every concrete
missing artifact from `blocked_on`. Pause this task and complete the upstream
dataset/layout plan before editing `ready` to true.

- [ ] **Step 5: Admit each dataset only after full verification**

Replace its blocked entry with the same explicit fields used by `t2i10m`, run
`verify_dataset --full --samples 1024`, then freeze hashes:

```bash
python3 -m experiments.eval.prefetcher.freeze_artifacts --dataset laion10m --out results/eval/prefetcher/manifests/laion10m-artifacts.json
python3 -m experiments.eval.prefetcher.freeze_artifacts --dataset yfcc10m --out results/eval/prefetcher/manifests/yfcc10m-artifacts.json
python3 -m experiments.eval.prefetcher.config --require-all-ready
```

Expected: both manifests are sealed and the all-ready check passes.

- [ ] **Step 6: Commit admitted manifests and validator**

```bash
git add experiments/eval/prefetcher/datasets.json experiments/eval/prefetcher/verify_dataset.py experiments/eval/prefetcher/tests/test_verify_dataset.py results/eval/prefetcher/manifests/laion10m-artifacts.json results/eval/prefetcher/manifests/yfcc10m-artifacts.json
git commit -m "test: admit LAION and YFCC prefetcher datasets"
```

---

### Task 11: Complete Cross-Dataset Prefetcher Runs

**Files:**
- Produce: `results/eval/prefetcher/raw/proof/{laion10m,yfcc10m}/`
- Produce: `results/eval/prefetcher/raw/calibration/{laion10m,yfcc10m}/`
- Produce: `results/eval/prefetcher/raw/final/{laion10m,yfcc10m}/`
- Produce: `results/eval/prefetcher/readiness/cross-dataset.json`

- [ ] **Step 1: Run and validate 100-query proofs**

For each dataset, after a dataset-specific passing preflight and approved cold
reset:

```bash
python3 -m experiments.eval.prefetcher.run_matrix --phase smoke --datasets laion10m,yfcc10m --state proof --out results/eval/prefetcher/raw/proof
python3 -m experiments.eval.prefetcher.validate_run --compare-transfer results/eval/prefetcher/raw/proof/{laion10m,yfcc10m}/*/run.json
```

Expected: the same candidate/result, zero-PQ-NAND, zero-bounce, and Oracle gates
as T2I.

- [ ] **Step 2: Calibrate each dataset**

```bash
python3 -m experiments.eval.prefetcher.run_matrix --phase calibration --datasets laion10m,yfcc10m --state cold --out results/eval/prefetcher/raw/calibration
python3 -m experiments.eval.prefetcher.validate_run --freeze-anchor 0.90 results/eval/prefetcher/raw/calibration/{laion10m,yfcc10m}/*/run.json --out results/eval/prefetcher/calibration/cross-dataset.json
```

Expected: a frozen measured `L` at recall@10 >= 0.90 for both datasets, with no
plot-time selection or extrapolation.

- [ ] **Step 3: Run five-repeat paired final blocks**

```bash
python3 -m experiments.eval.prefetcher.run_matrix --phase final_all --datasets laion10m,yfcc10m --anchors results/eval/prefetcher/calibration/cross-dataset.json --paired-cold-warm --out results/eval/prefetcher/raw/final
```

Expected: five accepted cold runs plus paired warm runs for every core
system/dataset/anchor block.

- [ ] **Step 4: Validate cross-dataset completion**

```bash
python3 -m experiments.eval.prefetcher.validate_run --milestone cross-dataset results/eval/prefetcher/raw/final/{laion10m,t2i10m,yfcc10m}/*/run.json --out results/eval/prefetcher/readiness/cross-dataset.json
```

Expected: every completion gate in the spec passes. A neutral or negative
performance result remains valid and is retained.

- [ ] **Step 5: Commit the readiness record**

```bash
git add results/eval/prefetcher/readiness/cross-dataset.json
git commit -m "test: complete cross-dataset prefetcher gate"
```

---

### Task 12: Aggregate and Render the Revised Figure 9

**Files:**
- Create: `experiments/eval/prefetcher/aggregate.py`
- Create: `experiments/eval/prefetcher/plot_figure9.py`
- Create: `experiments/eval/prefetcher/tests/test_aggregate.py`
- Produce: `results/eval/prefetcher/validated.csv`
- Produce: `results/eval/prefetcher/figure9-provenance.json`
- Produce: `paper/figs/eval-prefetcher.pdf`

- [ ] **Step 1: Write synthetic matched-block tests**

Use 15 synthetic run records (three systems x five repeats) and assert median,
bootstrap interval determinism at seed 20260904, serial normalization to 1.0,
and rejection when one system has four repetitions or a different candidate
hash.

- [ ] **Step 2: Implement deterministic aggregation**

The aggregator groups by
`(dataset,anchor,L,state,system,artifact_manifest_sha256,candidate_sidecar_sha256)`.
It accepts exactly five cold runs per core system and uses:

```python
def bootstrap_ci(values, seed=20260904, samples=10000):
    rng = random.Random(seed)
    medians = []
    for _ in range(samples):
        draw = [values[rng.randrange(len(values))] for _ in values]
        medians.append(statistics.median(draw))
    medians.sort()
    return medians[int(0.025 * samples)], medians[int(0.975 * samples)]
```

Emit individual run IDs alongside every aggregate row.

- [ ] **Step 3: Implement the three-panel figure**

Render:

- panel (a): matched-recall normalized QPS for serial, batch, and frozen;
- panel (b): requested pages/query, extent-added pages/query, NAND MiB/query,
  and useful-page percentage as aligned subaxes without a dual y-axis;
- panel (c): mean/p99 latency and critical-wait contribution.

Use one consistent system palette, grayscale-safe markers, visible 95% CIs,
and dataset order LAION-10M, T2I-10M, YFCC-10M. Add the Oracle upper-bound
marker without using it as the normalization denominator.

- [ ] **Step 4: Run aggregation and plotting**

```bash
MPLCONFIGDIR=/tmp/flashanns-prefetcher-mpl python3 -m experiments.eval.prefetcher.aggregate --raw results/eval/prefetcher/raw/final --out results/eval/prefetcher/validated.csv --provenance results/eval/prefetcher/figure9-provenance.json
MPLCONFIGDIR=/tmp/flashanns-prefetcher-mpl python3 -m experiments.eval.prefetcher.plot_figure9 --csv results/eval/prefetcher/validated.csv --out paper/figs/eval-prefetcher.pdf
pdfinfo paper/figs/eval-prefetcher.pdf | rg 'Pages|Page size'
```

Expected: one-page vector PDF and a provenance map from every mark to five run
IDs. No historical log or hand-entered performance value appears in the CSV.

- [ ] **Step 5: Run plot tests and commit generated evidence**

```bash
python3 -m unittest experiments.eval.prefetcher.tests.test_aggregate -v
git add experiments/eval/prefetcher/aggregate.py experiments/eval/prefetcher/plot_figure9.py experiments/eval/prefetcher/tests/test_aggregate.py results/eval/prefetcher/validated.csv results/eval/prefetcher/figure9-provenance.json paper/figs/eval-prefetcher.pdf
git commit -m "eval: render frozen prefetcher ablation"
```

---

### Task 13: Reconcile the Overall Evaluation Contract

**Files:**
- Modify: `docs/superpowers/specs/2026-09-03-flashanns-evaluation-design.md:221-251`
- Modify: `docs/superpowers/plans/2026-09-03-flashanns-evaluation.md:207-226`
- Modify: `paper/sections/eval.tex`
- Verify: `paper/sections/intro.tex`
- Verify: `paper/sections/design.tex`

- [ ] **Step 1: Replace the stale Q4 wording**

Replace the optional-lookahead question with:

```text
Q4 Frozen Prefetcher: Does host PQ navigation remove NAND from the best-first
dependency chain, and do batched transfer plus extent-aware issue improve
matched-recall throughput and p99 without changing PQ candidates or results?
```

- [ ] **Step 2: Replace Figure 9 and Task 6 definitions**

Copy the controlled-system matrix, panels, non-claims, and completion gates from
the approved prefetcher spec. Remove the `M` sweep and Demand/Mandatory/Wise/
Blind labels; do not retain them as aliases for different implementations.

- [ ] **Step 3: Insert only validated figure and claims**

Reference `paper/figs/eval-prefetcher.pdf`. Every numeric sentence must map to
`results/eval/prefetcher/figure9-provenance.json`. State that the current NUMA
window is host DRAM unless a physical CXL-DRAM backend passed its identity gate.

- [ ] **Step 4: Audit forbidden stale language**

```bash
rg -n "optional lookahead|Blind|M=|M =|mandatory-only|Wise Prefetcher" docs/superpowers/specs/2026-09-03-flashanns-evaluation-design.md docs/superpowers/plans/2026-09-03-flashanns-evaluation.md paper/sections/eval.tex paper/sections/intro.tex paper/sections/design.tex
```

Expected: no claim that the frozen PQ path implements those mechanisms. A
historical contrast is allowed only when explicitly labeled superseded.

- [ ] **Step 5: Build the paper and commit contract reconciliation**

```bash
latexmk -g -pdf -interaction=nonstopmode -halt-on-error paper/main.tex
rg -n "undefined references|Reference .* undefined|Citation .* undefined" paper/main.log
git diff --check -- docs/superpowers/specs/2026-09-03-flashanns-evaluation-design.md docs/superpowers/plans/2026-09-03-flashanns-evaluation.md paper/sections/eval.tex paper/sections/intro.tex paper/sections/design.tex
git add docs/superpowers/specs/2026-09-03-flashanns-evaluation-design.md docs/superpowers/plans/2026-09-03-flashanns-evaluation.md paper/sections/eval.tex
git commit -m "docs: align evaluation with frozen prefetcher"
```

Expected: LaTeX succeeds and the log search returns no matches.

---

### Task 14: Final Prefetcher-Only Verification

**Files:**
- Produce: `results/eval/prefetcher/readiness/final.md`
- Verify: all files and outputs from Tasks 1--13

- [ ] **Step 1: Run the complete offline test suite**

```bash
make -C serving/tests test
python3 -m unittest discover -s experiments/eval/prefetcher/tests -v
```

Expected: all C++ and Python tests pass.

- [ ] **Step 2: Revalidate every accepted run and provenance edge**

```bash
python3 -m experiments.eval.prefetcher.validate_run --milestone cross-dataset results/eval/prefetcher/raw/final/{laion10m,t2i10m,yfcc10m}/*/run.json --out results/eval/prefetcher/readiness/cross-dataset-recheck.json
python3 -m experiments.eval.prefetcher.aggregate --raw results/eval/prefetcher/raw/final --out /tmp/flashanns-prefetcher-recheck.csv --provenance /tmp/flashanns-prefetcher-recheck-provenance.json
cmp results/eval/prefetcher/validated.csv /tmp/flashanns-prefetcher-recheck.csv
cmp results/eval/prefetcher/figure9-provenance.json /tmp/flashanns-prefetcher-recheck-provenance.json
```

Expected: validation passes and both comparisons are byte-identical.

- [ ] **Step 3: Verify the freeze and scheduling boundary**

```bash
git diff a9f8447 -- serving/pq_table.hpp serving/hide_fill.hpp serving/search_beam.cpp
rg -n -- '--cont-batch|--lookahead-k|--spec-beam-nbrs|--score-page|--pipe-drive' experiments/eval/prefetcher
```

Review the first diff manually: only approved trace/counter hooks may touch the
frozen files. The second command must find no live matrix flag; tests may mention
forbidden strings only as rejection cases.

- [ ] **Step 4: Write the final readiness record**

Record command outputs, commit SHA, validated run count, rejected run count and
reasons, dataset/system block counts, figure hash, provenance hash, physical
window label, and all eight spec completion gates in
`results/eval/prefetcher/readiness/final.md`.

- [ ] **Step 5: Commit final readiness**

```bash
git add results/eval/prefetcher/readiness/final.md results/eval/prefetcher/readiness/cross-dataset-recheck.json
git commit -m "eval: close prefetcher-first verification"
```

At this point the prefetcher evaluation is complete. Continuous Batching may
begin from the sealed dataset manifests, run schema, and validator without
reopening the frozen T=1 implementation or rerunning valid prefetcher blocks.
