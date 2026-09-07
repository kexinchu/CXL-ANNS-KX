# Demand Original-Layout Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure a fully unoptimized, original-layout Demand baseline on all three 10M datasets and add its accepted five-repeat frontiers to Figure 8.

**Architecture:** Add `demand-orc` as a supplemental internal system with an explicit `original` layout, while preserving the frozen main matrix and all existing run identities. A shared layout selector controls staging, preflight, command construction, and record identity; a resumable one-dataset campaign produces 30 accepted runs per dataset and restores an extent-layout image after device use.

**Tech Stack:** Python 3 standard library and `unittest`, Bash, existing `search_beam`, VMEM character device `/dev/vmem0`, Matplotlib PDF output, LaTeX/ACM manuscript.

---

## File Map

- Create `experiments/eval/flashanns/layout.py`: the sole mapping from a system layout to a dataset image.
- Modify `experiments/eval/flashanns/systems.json`: declare layouts and the `demand-orc` system.
- Modify `experiments/eval/flashanns/matrix.json`: add isolated `smoke_original` and `q2_original` phases.
- Modify `experiments/eval/flashanns/config.py`: validate the supplemental contract without changing frozen Q2.
- Modify `experiments/eval/flashanns/run_matrix.py`: build original-layout commands without a slot map.
- Modify `experiments/eval/flashanns/run_one.py`: preflight the selected image and persist its identity.
- Modify `experiments/eval/flashanns/stage.py`, `preflight.py`, and `restore_volatile.py`: accept an explicit layout.
- Modify `experiments/eval/flashanns/validate_run.py`: reject mislabeled original-layout records.
- Modify `experiments/eval/flashanns/aggregate.py`: preserve layout as a grouping/provenance dimension.
- Create `tools/run_eval_demand_orc_campaign.sh`: stage, smoke, run, seal, and restore one dataset.
- Modify `experiments/eval/flashanns/plot_six_figures.py`: render four Figure 8 series.
- Modify `paper/sections/eval.tex`: distinguish original- and optimized-layout Demand.
- Modify focused tests under `experiments/eval/flashanns/tests/` for every boundary above.

### Task 1: Encode the Layout Contract

**Files:**
- Create: `experiments/eval/flashanns/layout.py`
- Modify: `experiments/eval/flashanns/systems.json`
- Modify: `experiments/eval/flashanns/matrix.json`
- Modify: `experiments/eval/flashanns/config.py`
- Test: `experiments/eval/flashanns/tests/test_config.py`

- [ ] **Step 1: Write failing configuration tests**

Add tests asserting that `demand-orc` is internal, T=8, original-layout, single-depth, and isolated from frozen `q2`:

```python
def test_original_layout_demand_is_supplemental(self):
    system = self.systems["demand-orc"]
    self.assertEqual(system["kind"], "internal")
    self.assertEqual(system["layout"], "original")
    self.assertEqual(system["threads"], 8)
    self.assertEqual(system["pipe_depth"], 1)
    self.assertIn("--no-vmem-prefetch", system["flags"])
    self.assertIn("--no-extent-run", system["flags"])
    self.assertIn("--no-steal-sched", system["flags"])
    self.assertEqual(self.matrix["q2"]["systems"], ["demand", "pipeann", "flashanns"])
    self.assertEqual(self.matrix["smoke_original"]["systems"], ["demand-orc"])
    self.assertEqual(self.matrix["q2_original"]["systems"], ["demand-orc"])

def test_rejects_original_layout_on_non_orc_system(self):
    systems = copy.deepcopy(self.systems)
    systems["demand"]["layout"] = "original"
    with self.assertRaisesRegex(ConfigError, "only demand-orc"):
        validate_configs(self.datasets, systems, self.matrix)
```

- [ ] **Step 2: Run the focused tests and confirm failure**

Run:

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_config -v
```

Expected: the new tests fail because `demand-orc` and the supplemental phases are undefined.

- [ ] **Step 3: Add the layout selector**

Create `layout.py` with one fail-closed function:

```python
from __future__ import annotations

import copy
from typing import Any


class LayoutError(ValueError):
    pass


LAYOUT_ARTIFACT = {"extent": "extent_image", "original": "oracle_image"}


def select_dataset_layout(dataset: dict[str, Any], layout: str) -> dict[str, Any]:
    if layout not in LAYOUT_ARTIFACT:
        raise LayoutError(f"unsupported internal layout {layout!r}")
    selected = copy.deepcopy(dataset)
    artifact = LAYOUT_ARTIFACT[layout]
    path = selected.get("artifacts", {}).get(artifact)
    if not isinstance(path, str) or not path:
        raise LayoutError(f"layout {layout} lacks artifact {artifact}")
    selected["staging"]["host_artifact"] = artifact
    selected["selected_layout"] = layout
    return selected
```

- [ ] **Step 4: Declare systems and supplemental phases**

Add `"layout": "extent"` to every existing internal system and `"layout": "external"` to PipeANN. Add:

```json
"demand-orc": {
  "kind": "internal",
  "layout": "original",
  "threads": 8,
  "per_thread_window": 134217728,
  "pipe_depth": 1,
  "flags": ["--no-vmem-prefetch", "--pipe-w", "1", "--pipe-depth", "1", "--no-extent-run", "--no-steal-sched"]
}
```

Add these matrix entries without modifying `q2.systems`:

```json
"smoke_original": {"nq": 100, "repeats": 1, "systems": ["demand-orc"]},
"q2_original": {"nq": 10000, "repeats": 5, "systems": ["demand-orc"], "sweep_L": true}
```

Extend `validate_configs()` so both supplemental phases require the exact system list, T=8, 128 MiB per-thread window, and the base-L sweep. Reject `original` on any system other than `demand-orc`, and reject any `demand-orc` layout other than `original`.

- [ ] **Step 5: Run configuration tests**

Run:

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_config -v
```

Expected: all configuration tests pass and frozen `q2` remains exactly `demand, pipeann, flashanns`.

- [ ] **Step 6: Commit the contract**

```bash
git add experiments/eval/flashanns/layout.py experiments/eval/flashanns/systems.json experiments/eval/flashanns/matrix.json experiments/eval/flashanns/config.py experiments/eval/flashanns/tests/test_config.py
git commit -m "eval: define original-layout demand baseline"
```

### Task 2: Build Layout-Correct Commands and Records

**Files:**
- Modify: `experiments/eval/flashanns/run_matrix.py`
- Modify: `experiments/eval/flashanns/run_one.py`
- Test: `experiments/eval/flashanns/tests/test_runner.py`

- [ ] **Step 1: Write failing runner tests**

```python
def test_demand_orc_uses_original_image_without_slot_map(self):
    runs = expand_runs(
        ROOT, "t2i10m", "q2_original", system_id="demand-orc",
        level=400, repeat_id=0, run_tag="4ad7-orc1",
    )
    self.assertEqual(len(runs), 1)
    run = runs[0]
    self.assertEqual(run["layout"], "original")
    self.assertEqual(run["staged_artifact"], "oracle_image")
    self.assertNotIn("--id-slot-map", run["command"])
    self.assertIn("--no-vmem-prefetch", run["command"])
    self.assertIn("--no-extent-run", run["command"])

def test_existing_demand_command_remains_extent_mapped(self):
    run = expand_runs(
        ROOT, "t2i10m", "q2", system_id="demand",
        level=400, repeat_id=0,
    )[0]
    self.assertEqual(run["layout"], "extent")
    index = run["command"].index("--id-slot-map")
    self.assertEqual(run["command"][index + 1], self.datasets["t2i10m"]["artifacts"]["slot_map"])
```

- [ ] **Step 2: Confirm the runner tests fail**

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_runner -v
```

Expected: `q2_original` is unknown and no layout identity exists.

- [ ] **Step 3: Make expansion layout-aware**

In `run_matrix.py`, include `smoke_original` and `q2_original` in phase expansion. Select the dataset before command construction:

```python
from experiments.eval.flashanns.layout import LAYOUT_ARTIFACT, select_dataset_layout

layout = systems[system_id]["layout"]
selected_dataset = (
    dataset if systems[system_id]["kind"] == "external-pipeann"
    else select_dataset_layout(dataset, layout)
)
spec["layout"] = layout
spec["staged_artifact"] = LAYOUT_ARTIFACT.get(layout)
spec["command"] = (
    _pipeann_command(root, dataset_id, dataset, spec)
    if spec["external"]
    else _internal_command(root, selected_dataset, systems[system_id], spec)
)
```

In `_internal_command()`, append the mapping only for extent layout:

```python
if spec["layout"] == "extent":
    command += ["--id-slot-map", a["slot_map"]]
elif spec["layout"] != "original":
    raise RunnerError(f"unsupported internal layout {spec['layout']}")
```

- [ ] **Step 4: Persist and consume layout identity**

In `run_one.run_spec()`, select the internal dataset using `spec["layout"]` before preflight. Add `layout` and `staged_artifact` to the record's copied keys and verify `staged_artifact` agrees with the selected dataset before launching the binary.

- [ ] **Step 5: Run focused runner tests**

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_runner -v
```

Expected: all tests pass; a dry-run original command contains no `--id-slot-map`, while the existing Demand command remains byte-for-byte equivalent apart from the new record metadata.

- [ ] **Step 6: Commit command construction**

```bash
git add experiments/eval/flashanns/run_matrix.py experiments/eval/flashanns/run_one.py experiments/eval/flashanns/tests/test_runner.py
git commit -m "eval: route original demand without slot mapping"
```

### Task 3: Make Staging and Preflight Layout-Aware

**Files:**
- Modify: `experiments/eval/flashanns/stage.py`
- Modify: `experiments/eval/flashanns/preflight.py`
- Modify: `experiments/eval/flashanns/restore_volatile.py`
- Test: `experiments/eval/flashanns/tests/test_stage.py`
- Test: `experiments/eval/flashanns/tests/test_preflight.py`
- Test: `experiments/eval/flashanns/tests/test_restore_volatile.py`

- [ ] **Step 1: Write failing image-selection tests**

Extend the stage fixture with distinct `oracle_image` and `extent_image` payloads and assert:

```python
selected = select_dataset_layout(dataset, "original")
record = stage_image(selected, target, require_char_device=False, chunk_bytes=4096)
self.assertEqual(record["layout"], "original")
self.assertEqual(record["host_artifact"], "oracle_image")
self.assertEqual(
    target.read_bytes()[offset:offset + len(original_payload)],
    original_payload,
)
```

Add preflight tests asserting that full identity evidence contains `layout` and `host_artifact`, and that reuse fails when either differs from the selected dataset.

- [ ] **Step 2: Confirm the tests fail**

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_stage experiments.eval.flashanns.tests.test_preflight experiments.eval.flashanns.tests.test_restore_volatile -v
```

Expected: layout metadata and mismatch checks are absent.

- [ ] **Step 3: Add explicit CLI layout selection**

Add `--layout`, with choices `extent` and `original`, to all three module CLIs. Immediately after loading the dataset, call:

```python
dataset = select_dataset_layout(datasets[args.dataset], args.layout)
```

Return or persist:

```python
record["layout"] = dataset["selected_layout"]
record["host_artifact"] = dataset["staging"]["host_artifact"]
```

`stage.py` defaults to `extent` only for compatibility with existing invocations. `preflight.py` and `restore_volatile.py` also default to `extent`, while the `demand-orc` campaign always passes `--layout original` explicitly.

- [ ] **Step 4: Reject stale cross-layout evidence**

In `snapshot_and_validate()`, when identity evidence is reused, compare both fields:

```python
expected_layout = dataset["selected_layout"]
expected_artifact = dataset["staging"]["host_artifact"]
if identity_evidence.get("layout") != expected_layout:
    raise PreflightError("identity evidence layout is stale")
if identity_evidence.get("host_artifact") != expected_artifact:
    raise PreflightError("identity evidence host artifact is stale")
```

Add the same metadata to volatile restoration evidence so `snapshot_and_validate()` rejects an extent-layout volatile capture reused for an original-layout run.

- [ ] **Step 5: Run staging/preflight tests**

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_stage experiments.eval.flashanns.tests.test_preflight experiments.eval.flashanns.tests.test_restore_volatile -v
```

Expected: all tests pass without opening `/dev/vmem0`.

- [ ] **Step 6: Commit image identity support**

```bash
git add experiments/eval/flashanns/stage.py experiments/eval/flashanns/preflight.py experiments/eval/flashanns/restore_volatile.py experiments/eval/flashanns/tests/test_stage.py experiments/eval/flashanns/tests/test_preflight.py experiments/eval/flashanns/tests/test_restore_volatile.py
git commit -m "eval: bind staging evidence to physical layout"
```

### Task 4: Enforce Accepted-Record and Aggregation Boundaries

**Files:**
- Modify: `experiments/eval/flashanns/validate_run.py`
- Modify: `experiments/eval/flashanns/aggregate.py`
- Test: `experiments/eval/flashanns/tests/test_validate_run.py`
- Test: `experiments/eval/flashanns/tests/test_aggregate.py`

- [ ] **Step 1: Write failing validation tests**

```python
def test_demand_orc_requires_original_layout_and_no_slot_map(self):
    item = record("demand-orc")
    item.update(layout="original", staged_artifact="oracle_image")
    validate_record(item)
    item["command"] += ["--id-slot-map", "/tmp/map"]
    with self.assertRaisesRegex(RunValidationError, "slot map"):
        validate_record(item)

def test_aggregation_rejects_mixed_layout_repetitions(self):
    records = [accepted_record(repeat=i, layout="original") for i in range(5)]
    records[-1]["layout"] = "extent"
    with self.assertRaisesRegex(AggregationError, "layout mismatch"):
        aggregate_records(records)
```

- [ ] **Step 2: Confirm tests fail**

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_validate_run experiments.eval.flashanns.tests.test_aggregate -v
```

- [ ] **Step 3: Validate original-layout records fail closed**

For `demand-orc`, require `layout == "original"`, `staged_artifact == "oracle_image"`, and absence of `--id-slot-map`. For every newly generated internal record containing `layout`, require the corresponding command behavior. Preserve immutable legacy accepted records that predate the field by inferring `extent` only when their command contains `--id-slot-map`; do not rewrite them.

- [ ] **Step 4: Group and export layout provenance**

Add normalized layout to the aggregation key between `system` and `state`. Use:

```python
def _record_layout(record: dict[str, Any]) -> str:
    if record.get("layout"):
        return str(record["layout"])
    command = record.get("command", [])
    if not record.get("external") and "--id-slot-map" in command:
        return "extent"
    raise AggregationError(f"run {record.get('run_id')} lacks physical layout identity")
```

The CSV and provenance JSON must include this layout value. Five repetitions with differing layouts must not form one plotted mark.

- [ ] **Step 5: Run record tests**

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_validate_run experiments.eval.flashanns.tests.test_aggregate -v
```

Expected: all tests pass and historical extent-layout records remain consumable without mutation.

- [ ] **Step 6: Commit validation boundaries**

```bash
git add experiments/eval/flashanns/validate_run.py experiments/eval/flashanns/aggregate.py experiments/eval/flashanns/tests/test_validate_run.py experiments/eval/flashanns/tests/test_aggregate.py
git commit -m "eval: seal layout identity in demand evidence"
```

### Task 5: Add a Resumable One-Dataset Campaign

**Files:**
- Create: `tools/run_eval_demand_orc_campaign.sh`
- Modify: `tools/eval_host_cold_lib.sh`
- Test: `experiments/eval/flashanns/tests/test_runner.py`

- [ ] **Step 1: Write a failing campaign-contract test**

Read the new script as text and assert it contains the safety and cardinality gates:

```python
def test_demand_orc_campaign_is_original_only_and_resumable(self):
    source = (ROOT / "tools/run_eval_demand_orc_campaign.sh").read_text()
    self.assertIn("--layout original", source)
    self.assertIn("--phase q2_original", source)
    self.assertIn("--system demand-orc", source)
    self.assertIn("EXPECTED_ACCEPTED=30", source)
    self.assertIn("trap restore_extent EXIT", source)
    self.assertNotIn("--id-slot-map", source)
```

- [ ] **Step 2: Confirm the test fails because the script does not exist**

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_runner.RunnerTest.test_demand_orc_campaign_is_original_only_and_resumable -v
```

- [ ] **Step 3: Make volatile restoration accept an explicit layout**

Extend `eval_reset_and_restore` with a fifth argument:

```bash
local layout=${5:-extent}
python3 -m experiments.eval.flashanns.restore_volatile \
  --dataset "$dataset" --layout "$layout" --write --out "$evidence"
```

Existing callers omit the argument and retain extent behavior.

- [ ] **Step 4: Create the dataset campaign script**

The script accepts exactly one of `t2i10m`, `yfcc10m`, or `laion10m`; pins the current `search_beam` SHA-256; uses tag `<binary-prefix>-orc1`; requires `FLASHANNS_RESTORE_DATASET` to name the extent image present before the campaign; and defines:

```bash
EXPECTED_ACCEPTED=30
LAYOUT=original
PHASE=q2_original
SYSTEM=demand-orc
RESTORE_DATASET=${FLASHANNS_RESTORE_DATASET:?set the currently staged extent dataset}
RAW=$BASE/raw/$DATASET/$TAG/$PHASE
ACCEPTED=$BASE/accepted/$DATASET/$TAG/$PHASE
IDENTITY=$BASE/preflight/${DATASET}-${TAG}-original-full-identity.json
```

Before overwriting the aperture, the script performs a full read-only identity
check of `RESTORE_DATASET` with `--layout extent`. This records the image that
must be restored and refuses a mistaken restore target. Its ordered write and
identity actions are:

```bash
python3 -m experiments.eval.flashanns.stage --dataset "$DATASET" --layout original
python3 -m experiments.eval.flashanns.preflight --dataset "$DATASET" \
  --layout original --state post --full-identity --out "$IDENTITY"
```

Run one `smoke_original` point at L=400, validate it against an existing same-dataset Demand record at L=400 with `--compare-same-search`, then execute nested loops over the six L values and five repetitions. Before each run call:

```bash
eval_reset_and_restore "$ROOT" "$DATASET" "$evidence" 4 original
```

Run, validate, and seal with `validate_run --seal-dir`. If the sealed accepted record already exists, print `SKIP accepted` and continue; if raw or volatile evidence exists without an accepted record, stop rather than overwrite it.

The `EXIT` trap stages `RESTORE_DATASET` with `--layout extent`, creates a full
extent identity record, and reports failure through the script exit code. It
never deletes raw, rejected, or claimed evidence.

- [ ] **Step 5: Syntax-check and run the campaign-contract test**

```bash
bash -n tools/run_eval_demand_orc_campaign.sh
python3 -m unittest experiments.eval.flashanns.tests.test_runner -v
```

Expected: shell syntax and tests pass; no device write occurs.

- [ ] **Step 6: Commit the campaign driver**

```bash
git add tools/run_eval_demand_orc_campaign.sh tools/eval_host_cold_lib.sh experiments/eval/flashanns/tests/test_runner.py
git commit -m "eval: add resumable original-demand campaign"
```

### Task 6: Qualify Software Before Device Writes

**Files:**
- Verify only; do not modify files in this task.

- [ ] **Step 1: Run the complete evaluation unit suite**

```bash
python3 -m unittest discover -s experiments/eval/flashanns/tests -v
```

Expected: every test passes.

- [ ] **Step 2: Verify the binary and original source images**

```bash
sha256sum serving/search_beam
python3 -m experiments.eval.flashanns.verify_dataset --dataset t2i10m --full
python3 -m experiments.eval.flashanns.verify_dataset --dataset yfcc10m --full
python3 -m experiments.eval.flashanns.verify_dataset --dataset laion10m --full
```

Expected: one pinned binary hash and three admitted dataset proofs; source image sizes and headers match their manifests.

- [ ] **Step 3: Dry-run all 90 commands**

For every dataset, L, and repeat, run `run_matrix --dry-run` with the supplemental phase and inspect automatically:

```bash
TAG="$(sha256sum serving/search_beam | awk '{print substr($1,1,4)}')-orc1"
for dataset in t2i10m yfcc10m laion10m; do
  for level in 50 100 200 400 800 1600; do
    for repeat in 0 1 2 3 4; do
      python3 -m experiments.eval.flashanns.run_matrix \
        --dataset "$dataset" --phase q2_original --system demand-orc \
        --L "$level" --repeat "$repeat" --run-tag "$TAG" --dry-run
    done
  done
done | tee /tmp/demand-orc-dry-run.txt
test "$(wc -l </tmp/demand-orc-dry-run.txt)" -eq 90
! grep -q -- '--id-slot-map' /tmp/demand-orc-dry-run.txt
```

Expected: exactly 90 commands and zero slot-map flags.

- [ ] **Step 4: Inspect live state read-only**

```bash
cat /sys/class/vmem/vmem0/backend
cat /sys/class/vmem/vmem0/nvme_dev
cat /sys/class/vmem/vmem0/cache_limit
cat /sys/class/vmem/vmem0/dirty_bytes
cat /sys/class/vmem/vmem0/io_errors
fuser /dev/vmem0
```

Expected: `vmem_sw`, the two admitted CD8P backing devices, 4 GiB cache, zero dirty bytes, zero I/O errors, and no open user. Any mismatch stops execution before staging.

### Task 7: Execute and Seal the 90 Measurements

**Files:**
- Create runtime evidence only under `results/eval/flashanns/`; do not edit source files.

- [ ] **Step 1: Identify and pin the currently staged extent image**

Try the three admitted extent images with full identity validation and accept
exactly one match:

```bash
RESTORE_DATASET=
for candidate in t2i10m yfcc10m laion10m; do
  evidence="/tmp/${candidate}-initial-extent.json"
  if sudo python3 -m experiments.eval.flashanns.preflight \
      --dataset "$candidate" --layout extent --state post --full-identity \
      --out "$evidence"; then
    test -z "$RESTORE_DATASET"
    RESTORE_DATASET=$candidate
  fi
done
test -n "$RESTORE_DATASET"
export RESTORE_DATASET
TAG="$(sha256sum serving/search_beam | awk '{print substr($1,1,4)}')-orc1"
```

Expected: exactly one current extent image matches. If none or more than one
matches, stop before writing `/dev/vmem0`.

- [ ] **Step 2: Run T2I-10M and audit 30 accepted records**

```bash
sudo env FLASHANNS_RESTORE_DATASET="$RESTORE_DATASET" tools/run_eval_demand_orc_campaign.sh t2i10m
find "results/eval/flashanns/accepted/t2i10m/$TAG/q2_original" -name run.json -type f | wc -l
```

Expected: 30 accepted records, all with `system=demand-orc`, `layout=original`, `staged_artifact=oracle_image`, 10,000 completed queries, and no slot-map argument.

- [ ] **Step 3: Run YFCC-10M and audit 30 accepted records**

```bash
sudo env FLASHANNS_RESTORE_DATASET="$RESTORE_DATASET" tools/run_eval_demand_orc_campaign.sh yfcc10m
find "results/eval/flashanns/accepted/yfcc10m/$TAG/q2_original" -name run.json -type f | wc -l
```

Expected: 30 accepted records satisfying the same contract.

- [ ] **Step 4: Run LAION-10M and audit 30 accepted records**

```bash
sudo env FLASHANNS_RESTORE_DATASET="$RESTORE_DATASET" tools/run_eval_demand_orc_campaign.sh laion10m
find "results/eval/flashanns/accepted/laion10m/$TAG/q2_original" -name run.json -type f | wc -l
```

Expected: 30 accepted records satisfying the same contract.

- [ ] **Step 5: Validate global cardinality and metric completeness**

```bash
jq -s -e '
  length == 90 and
  all(.[]; .validation.status == "accepted") and
  all(.[]; .system == "demand-orc" and .layout == "original") and
  all(.[]; .staged_artifact == "oracle_image") and
  all(.[]; .metrics.completed_queries == 10000) and
  all(.[]; (.metrics["recall@10"] | type) == "number") and
  all(.[]; (.metrics.mean_latency_ms | type) == "number") and
  all(.[]; (.metrics.throughput_QPS | type) == "number")
' $(find results/eval/flashanns/accepted/{t2i10m,yfcc10m,laion10m}/"$TAG"/q2_original -name run.json -type f | sort)
```

Expected: `true`. A rejected or incomplete point is retained and rerun only with a new campaign suffix after diagnosing its cause.

### Task 8: Aggregate and Add the Fourth Figure 8 Series

**Files:**
- Modify: `experiments/eval/flashanns/plot_six_figures.py`
- Modify: `experiments/eval/flashanns/tests/test_plots.py`
- Regenerate: `paper/figs/frontier-t2i.pdf`
- Regenerate: `paper/figs/frontier-yfcc.pdf`
- Regenerate: `paper/figs/frontier-laion.pdf`

- [ ] **Step 1: Write failing four-series plot tests**

In the synthetic frontier fixture, generate rows for:

```python
for system in ("demand-orc", "demand", "pipeann", "flashanns"):
    for L, recall in ((100, 0.85), (400, 0.92)):
        rows.append(self._row(dataset=dataset, phase=("q2_original" if system == "demand-orc" else "q2"), system=system, L=L, recall_at_10_median=recall))
```

After rendering, use `pdftotext` to assert both `Demand (original layout)` and `Demand (optimized layout)` appear in every frontier PDF.

- [ ] **Step 2: Confirm the plot test fails**

```bash
python3 -m unittest experiments.eval.flashanns.tests.test_plots -v
```

- [ ] **Step 3: Add styles and select both phases**

Add distinct grayscale-safe definitions:

```python
COLORS["demand-orc"] = "#202020"
LABELS["demand-orc"] = "Demand (original layout)"
LABELS["demand"] = "Demand (optimized layout)"
```

Have `_frontier()` select `q2_original` only for `demand-orc`, select `q2` for the other systems, and draw the four systems in this order:

```python
("demand-orc", "demand", "pipeann", "flashanns")
```

Use different marker and dash encodings for the two Demand curves so they remain distinguishable without color.

- [ ] **Step 4: Aggregate curated accepted evidence**

Run `aggregate.py` with the existing accepted roots used by the six-figure artifact plus the three new `<binary-prefix>-orc1/q2_original` roots. Write a new immutable aggregate directory:

```text
results/eval/flashanns/provisional/non-q3-load-all-three-demand-orc/
```

Expected: 18 `demand-orc` marks, one per dataset and L, each naming exactly five accepted run IDs.

- [ ] **Step 5: Render and inspect vector PDFs**

```bash
python3 -m experiments.eval.flashanns.plot_six_figures \
  --csv results/eval/flashanns/provisional/non-q3-load-all-three-demand-orc/validated.csv \
  --out-dir paper/figs
pdfinfo paper/figs/frontier-t2i.pdf
pdfinfo paper/figs/frontier-yfcc.pdf
pdfinfo paper/figs/frontier-laion.pdf
pdftotext paper/figs/frontier-t2i.pdf -
```

Expected: one-page vector PDFs with four readable legend entries, six points per Demand curve, and no missing-run exception.

- [ ] **Step 6: Commit plotting changes and generated PDFs**

```bash
git add experiments/eval/flashanns/plot_six_figures.py experiments/eval/flashanns/tests/test_plots.py paper/figs/frontier-t2i.pdf paper/figs/frontier-yfcc.pdf paper/figs/frontier-laion.pdf
git commit -m "eval: plot original-layout demand frontiers"
```

### Task 9: Revise Evaluation Text and Complete Final Verification

**Files:**
- Modify: `paper/sections/eval.tex`
- Verify: `paper/main.pdf`

- [ ] **Step 1: Update the Q2 comparison contract**

Replace the three-system Q2 wording with four clearly defined systems. State that both Demand variants disable prefetching and continuous scheduling, while only **Demand (optimized layout)** retains the extent placement. Do not call `demand-orc` Oracle, Oracle-DRAM, or an in-memory upper bound.

- [ ] **Step 2: Update Figure 8 caption and prose from the aggregate**

Describe the six-point recall–mean-latency and recall–throughput frontiers. Report only values read from the new validated CSV; derive every speedup from matched or interpolated recall according to the existing evaluation method. Remove the stale three-series `\Description` and any fixed numeric sentence contradicted by the regenerated figure.

- [ ] **Step 3: Build and test the manuscript**

```bash
python3 -m unittest discover -s experiments/eval/flashanns/tests -v
cd paper
latexmk -pdf -interaction=nonstopmode -halt-on-error main.tex
cd ..
pdfinfo paper/main.pdf
pdftotext -layout paper/main.pdf /tmp/flashanns-main.txt
! rg -n 'undefined references|Citation.*undefined|Reference.*undefined' paper/main.log
```

Expected: all tests pass, LaTeX exits zero, and the Figure 8 page has no clipping or overlapping caption/legend.

- [ ] **Step 4: Verify evidence provenance and restored device state**

```bash
jq -e '.marks | map(select(.key[2] == "demand-orc")) | length == 18 and all(.[]; (.run_ids | length) == 5)' \
  results/eval/flashanns/provisional/non-q3-load-all-three-demand-orc/provenance.json
cat /sys/class/vmem/vmem0/dirty_bytes
cat /sys/class/vmem/vmem0/io_errors
fuser /dev/vmem0
```

Expected: 18 five-run provenance marks, zero dirty bytes, zero I/O errors, and no open user. Verify the final extent-layout identity record created by the campaign before declaring completion.

- [ ] **Step 5: Commit the paper revision**

```bash
git add paper/sections/eval.tex paper/main.pdf
git commit -m "paper: compare original and optimized demand layouts"
```
