# Figure 9 Standalone Notebook Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create and execute one self-contained notebook that embeds the 33 accepted-evidence aggregate marks used by paper Figure 9 and renders both constituent plots.

**Architecture:** Extract the current Figure 9 rows once while constructing the notebook, serialize the selected fields as a Python literal inside a code cell, and perform no evidence-file reads when the notebook executes. A focused structural test checks the embedded matrix before execution; notebook cells then repeat the fail-closed checks and render PDF and PNG outputs beside the notebook.

**Tech Stack:** Python 3, `csv`, `json`, `nbformat`, `nbconvert`, Matplotlib, NumPy, unittest.

---

### Task 1: Add a failing notebook contract test

**Files:**
- Create: `paper/evaluation_workspace/notebooks/test_figure9_notebook.py`
- Test: `paper/evaluation_workspace/notebooks/test_figure9_notebook.py`

- [ ] **Step 1: Write the failing test**

Create a unittest that loads `figure9.ipynb` through `nbformat`, locates the
`FIGURE9_ROWS` assignment, executes that data cell in isolation, and asserts:

```python
self.assertEqual(len(rows), 33)
self.assertEqual(sum(row["phase"] == "q3_t1" for row in rows), 3)
self.assertEqual(sum(row["phase"] == "q3_t8" for row in rows), 30)
self.assertTrue(all(len(row["run_ids"]) == 5 for row in rows))
self.assertEqual(len({run_id for row in rows for run_id in row["run_ids"]}), 165)
```

Also assert that notebook source contains no `read_csv`, `open(`, or accepted
evidence path, and that it names the four required outputs.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
python3 -m unittest paper/evaluation_workspace/notebooks/test_figure9_notebook.py -v
```

Expected: fail because `figure9.ipynb` does not exist.

### Task 2: Build the self-contained notebook

**Files:**
- Create: `paper/evaluation_workspace/notebooks/figure9.ipynb`

- [ ] **Step 1: Select the exact source rows**

From
`results/eval/flashanns/provisional/non-q3-load-all-three-demand-orc/validated.csv`,
select all `q3_t1` rows for T2I-10M and all `q3_t8` rows. Reject the extraction
unless it produces three and thirty rows respectively and every row contains
five run IDs.

- [ ] **Step 2: Embed bounded data fields**

Serialize into `FIGURE9_ROWS` only the identity, plotting, confidence-bound,
and provenance fields used by the two plots. Convert numeric values to numbers
and `run_ids` to five-element lists before embedding them.

- [ ] **Step 3: Add notebook validation and renderers**

Implement notebook cells that verify the complete dataset/system/thread
matrix, finite metrics, unique row keys, and 165 unique run IDs. Port the
`_wise` and `_batching` visual semantics from
`experiments/eval/flashanns/plot_six_figures.py`, emit PDF/PNG pairs to
`figure9-output/`, and display the PNG previews inline.

- [ ] **Step 4: Run the contract test**

Run:

```bash
python3 -m unittest paper/evaluation_workspace/notebooks/test_figure9_notebook.py -v
```

Expected: all tests pass.

### Task 3: Execute and inspect the artifact

**Files:**
- Modify: `paper/evaluation_workspace/notebooks/figure9.ipynb`
- Produce: `paper/evaluation_workspace/notebooks/figure9-output/wise-ablation.pdf`
- Produce: `paper/evaluation_workspace/notebooks/figure9-output/wise-ablation.png`
- Produce: `paper/evaluation_workspace/notebooks/figure9-output/batching-ablation.pdf`
- Produce: `paper/evaluation_workspace/notebooks/figure9-output/batching-ablation.png`

- [ ] **Step 1: Execute top to bottom**

Run:

```bash
MPLCONFIGDIR=/tmp/flashanns-mplconfig python3 -m jupyter nbconvert --execute --to notebook --inplace --ExecutePreprocessor.timeout=120 paper/evaluation_workspace/notebooks/figure9.ipynb
```

Expected: exit 0 with executed outputs stored in the notebook.

- [ ] **Step 2: Verify output contract**

Run the unittest again, use `pdfinfo` to require one page for both PDFs, use
`pdffonts` to require embedded fonts, and compare the embedded T=8 medians with
`600.78/3016.23/421.24` for FlashANNS and
`559.17/2933.21/406.01` for Wise-only.

- [ ] **Step 3: Inspect repository scope**

Run `git status --short` and `git diff --check` restricted to the plan, test,
and notebook paths. Do not stage or modify unrelated dirty-worktree files.

### Fallback: Standalone Python artifact

If `nbformat` or Jupyter execution support is unavailable, create
`paper/evaluation_workspace/figure9.py` instead. The script embeds the same 33
rows and performs the same fail-closed validation, rendering, and output
checks. Test it with `paper/evaluation_workspace/test_figure9.py`, run it from
an unrelated working directory to prove path independence, and emit the four
outputs below `paper/evaluation_workspace/figure9-output/`.
