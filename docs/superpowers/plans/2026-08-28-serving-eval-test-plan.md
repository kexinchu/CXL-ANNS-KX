# Serving + Paper Evaluation Test Plan

> Pair with `2026-08-28-serving-runtime-completion.md`. This file does not add features. It says what to run, what “pass” means, and which number goes on which paper figure.

**Goal:** After the runtime tasks land, produce the Route A Eval numbers with one protocol, and fail loudly when a claim is not supported.

**Primary corpus:** Text2Image-10M (200-d, MIPS), already scripted in `tools/run_t2i10m_pipeline.sh`.  
**Primary binary:** `serving/search_beam`.  
**Contrast binary (not the system):** PipeANN `search_disk_index` mode `cont` / `pipe`.  
**Block SOTA:** DiskANN / PipeANN on the same machine, same recall floor, PQ **on** unless a row is labeled `--no_pq_nav`.

---

## 0. Global protocol (every serving row)

Copy this block. A row that violates it is invalid.

```text
oneshot-fp
k=10
iters=0
shuffle-seed=42
dram-backend=numa          # unless /dev/dax0.0 is present this boot
dram-bytes=1GiB
host-cap=32MiB
threads=1                  # multi-thread + shared window is a known loss
cold: --flush-window --no-hotset --max-q 100
warm (D3 only): no flush, --hotset-bytes 64MiB, --max-q 200, one discarded warm-up pass
identity: /dev/vmem0 software CXL-SSD; say so in the log header
```

Reload `vmem_sw` (or equivalent cache reset) **once per cold table**, not once per row, if rows share a process-fresh `--flush-window`. If the 4 GiB device cache is still warm across rows and you claim “cold”, the table is void.

Always keep the first 15 CSV columns stable. Parse with `grep '^CSV,'`.

Recall floor for T2I money plot: **0.92** (historical P3 ≈ 0.93 at L=300). If a config cannot reach 0.92 at L=400, mark the row `NO_FLOOR` and do not plot it as a win.

---

## 1. Unit tests (no device)

```bash
cd /root/chukexin/CXL-ANNS-KX/serving/tests && make test
```

| Test | Pass |
|------|------|
| `test_metrics` | precision 8/15, `add_from` merges |
| `test_placement` | host `nbrs()` ≠ SSD bytes after override |
| `test_hot_set` | top-2 order; aging drops freq 1 |

**Fail action:** fix runtime plan Tasks 1–3; do not run device tests.

---

## 2. Smoke (device, 20 queries)

```bash
source /mnt/disk0/chukexin_motivation/serving_t2i_10m/serve_vmem.env
# build search_beam as in the implementation plan
# then the Task 5 command with --max-q 20
```

| Check | Pass |
|-------|------|
| exit code | 0 |
| `recall@10` printed | yes |
| QPS vs historical P3 | same order of magnitude (≥ 8 on 20q is enough; 20q is noisy) |
| new CSV columns | present |

**Fail action:** do not start E0–E7.

---

## 3. Paper matrix

Use `tools/iso_recall_pick.sh` to choose `L` per *family* (P0 family vs P3 family), then freeze that `L` for ablations in the same family.

### E0 — Motivation replay (already have logs)

Do **not** re-port F1–F9 into `search_beam`. Re-run only if reviewers need a fresh timestamp:

```bash
cd /root/chukexin/CXL-ANNS-KX/motivation_exps && make
./motivation vmem-verify --dir <laion_dir> --nq 8    # ALL PASS
# optional: vmem-mot-ef / vmem-mot-pin
```

| Pass | H0 cliff still ~100×; F2 record ≻ page; F5 precision collapse; F9 adaptive does not win |
| Paper | Fig 3–5, Table 1 |

### E1 — Placement (new)

Same P3 flags, `--max-q 100`, `--flush-window`, two rows:

| Row | Extra |
|-----|--------|
| graph on SSD (default) | (none) |
| graph in DRAM | `--graph-in-dram` |

| Pass | recall within 0.005; QPS(graph-DRAM) ≥ QPS(default) **or** a documented small loss with a reason (lock/copy). If default is *much* faster, E1 cannot claim “graph must live in DRAM” — change the Intro sentence instead. |
| Paper | Fig 11 sensitivity or a 2-row table in §6.5 |

### E2 — Promote budget (exists, re-run with new CSV)

P3, L frozen, sweep:

```text
--budget 16MiB|64MiB|256MiB
--install-top 1|4|16
--install-all-fetched          # must lose (pollution)
```

| Pass | a sweet spot exists; `--install-all-fetched` QPS ≤ 1/3 of `--install-top 4` at 64 MiB (historical ÷5). Precision of install-all is lower than install-top 4. |
| Paper | Fig 10b |

### E3 — Shared hot set (new; **warm** protocol)

Packed or pagebin P3, **no** `--flush-window`, one warm-up pass discarded, then measure:

| Row | beam | hotset-bytes |
|-----|------|----------------|
| A | 150 | 0 |
| B | 150 | 64 MiB |
| C | 300 | 0 |
| D | 300 | 64 MiB |

| Pass | At iso-recall (both ≥ 0.92), B QPS ≥ A, and **either** B ≥ C **or** we drop the F7 slogan to “hot set helps at fixed beam” and do not claim it beats a deeper beam. |
| Paper | Fig 10 / E3 table. If fail, D3 stays “entry pin + soft-pin” in the paper. |

### E4 — Controller (harness, not F9)

```bash
tools/iso_recall_pick.sh --floor 0.92 --beams 150,200,300,400 -- \
  serving/search_beam ... --policy P3 ... --flush-window --max-q 100
```

Optional: same L with `--early-stop-patience 8`. Keep the row only if recall still ≥ 0.92 and QPS rises.

| Pass | Picker exits 0; chosen L is the smallest in the list that meets the floor. Early-stop is extra, not required for “M4 done.” |
| Paper | §6.3 method sentence + money-plot L annotation |

### E5 — vs DiskANN / PipeANN (money plot)

**Serving rows (CXL-SSD, cold, iso-recall ≥ 0.92):** Demand P0, P3, P3+pagebin.

**Block rows (same host, same T2I index if available, else DiskANN disk index on the same NVMe):** PipeANN `pipe` and `cont` at L that hits ≥ 0.92; DiskANN `search_disk_index` PQ **on**.

Optional extra row: DiskANN `--no_pq_nav` labeled as such (do not put it on the same line as PQ-on).

| Pass | P3+pagebin ≫ P0 (expect ~10×+). Residual gap to PQ-on DiskANN/PipeANN is **reported**, not hidden. If serving loses absolute QPS, the paper claim is “gap to naive CXL-SSD” + DRAM/IO, not “we beat DiskANN.” |
| Paper | Fig 9, Table 4 |

Write results to `results/paper_figs/e5_iso_recall.csv`.

### E6 — Scale

Minimum:

| Axis | Points |
|------|--------|
| nq / concurrency | `--max-q 50,100` ; `--threads 1` only for the main table; threads=8 as a **negative** appendix if you want the lock lesson |
| layout | packed vs pagebin (dim=200 already shows the layout tax) |
| optional | Wiki-25M P3 smoke `--max-q 20` if the layout is still staged — trend only, not the money plot |

| Pass | P3 still beats P0 at nq=100. Threads=8 shared window must not be plotted as a win. |
| Paper | Fig 11a |

### E7 — Ablation

`tools/run_serving_ablation.sh` (implementation Task 7), plus the warm D3 pair from E3.

| Pass | Removing P3 promote (P0) collapses QPS. Removing prefetch (`--no-vmem-prefetch`) drops QPS vs full P3. `install_all` drops vs `install_top=4`. Hot set off vs on only judged on the warm table. |
| Paper | Fig 10a, Table 5 |

---

## 4. PipeANN contrast (not E5 substitute)

Use existing notes; re-measure only if you need a cold CXL mmap number on this boot:

```text
# NVMe: cont vs pipe at iso-recall (already in results/pipeann_bubbles_continuous_batching.md)
# CXL mmap: PIPEANN_MMAP_DEV=/dev/vmem0 + PIPEANN_VMEM_PREFETCH=1 vs 0
# Protocol: reload vmem between cold points
```

| Pass | Qualitative: blocking mmap much slower than async prefetch; `cont` without prefetch loses on CXL. |
| Paper | Fig 5 / Fig 8 caption, not the money-plot Y axis |

---

## 5. Honest-fail list (do not “fix” by widening beam)

- Claiming cold while `cache_used` on vmem is full.
- Comparing P3 `--max-q 100` to DiskANN 200k queries without saying so.
- Putting PQ-off DiskANN on the PQ-on line.
- Reporting 160 GB/s or 12 GB/s as achieved (useful fill ~0.55 GB/s on T2I P3).
- Multi-host one-copy numbers (none exist).
- `--threads>1 --shared-window` as the main result.

---

## 6. Artifact checklist (after numbers exist)

- [ ] `serving/tests` pass on a clean tree
- [ ] `results/paper_figs/e5_iso_recall.csv`
- [ ] `results/paper_figs/e7_ablation.csv`
- [ ] `results/paper_figs/e3_hotset_warm.csv` or a written decision to demote D3
- [ ] Log header on each file: kernel, vmem backend, DAX present/absent, seed, floor
- [ ] No internal hostname in a file you will zip for ASPLOS

---

## 7. Order of fire (calendar)

1. Unit + smoke (1 hour if the machine is up).  
2. E2 + E7 cold (same L) — decides whether P3 is still the system.  
3. E5 serving three rows + DiskANN/PipeANN one afternoon.  
4. E3 warm hot set — decide D3 wording.  
5. E1 graph-in-DRAM.  
6. E4 picker annotation.  
7. E0 only if FINDINGS logs are missing.

If step 2 fails, stop implementation feature work and debug P3. Do not start E5.
