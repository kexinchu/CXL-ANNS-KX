# PQ end-batch prefetcher — design (2026-09-04)

**LOCKED.** Implementation and 10M rows are frozen. Do not change
`search_one_pq`, `PqTable`, `HidePipe` bounce+`wait_covering`, or the
extent@1100 recipe. See `docs/notes/2026-09-04-prefetcher-freeze.md`.

Replace the 2026-09-04 hide-copy freeze (oneshot-fp e4 a1, hide/oracle **0.20×**)
with the measured 10M winner: host PQ-64 beam + one end-batch vector-page issue.
Hide/oracle is **0.69× (nq=20) / 0.72× (nq=100)**. There is one frozen
prefetcher, not two contracts.

## Why the old freeze is obsolete

Old freeze (`docs/notes/2026-09-04-prefetcher-freeze.md`): score every expand
from full-precision vectors. Hide must NAND ~22 MB/q during the beam. Locked
10M row was **25.90 QPS / 0.20×** oracle.

New path: rank the beam with 64-byte PQ ADC in host DRAM; NAND only the final
L=400 candidate pages. Locked rows:

| nq | hide QPS | mean | recall@10 | hide/oracle |
|---:|---------:|-----:|----------:|------------:|
| 20 | **110.94** | 9.01 ms | **0.925** | **0.69×** (161.63) |
| 100 | **116.29** | 8.60 ms | **0.923** | **0.72×** (161.52) |

Logs: `results/paper_figs/hide_10m_pq64beam_nopipe_nq{20,100}.log`.
`--pipe-drive` on this path is a regression (71.72 / 72.48) and is not part of
the design.

Paper claim rows 50.25 / 49.25 / 85.8 stay historical (pagebin oneshot, different
layout). This spec replaces only the **DiskANN-10M prefetcher freeze**.

## Search contract (the prefetcher)

```
G0 10k nav → entry
  → PQ-64 beam L=400 (host N(u) + host codes, no SSD)
  → issue vector pages of the L candidates (one HidePipe wave)
  → wait_covering → install bounce → DramWindow
  → FP MIPS rerank → top-10
```

Invariants:

1. Score FP only after the page is in the host `DramWindow` (`from_win = 100`).
2. Issue only committed rerank **miss** pages. No mid-beam NAND. No spec-beam.
3. Bounce buffer → window memcpy. `--direct-install` stays a no-op.
4. Host `--graph-file` is required so `N(u)` is not an SSD mmap fault.
5. Extent image @ 1100 GiB + `--id-slot-map` + `--extent-run` on that one wave.
6. T=1, L=400, k=10, seed-42, cache 100 MiB, window 2 GiB host numa.

## Core (keep / freeze)

| piece | where | role |
|-------|-------|------|
| `PqTable` 64B MIPS ADC | `serving/pq_table.hpp` | beam rank, no NAND |
| `search_one_pq` | restore into `serving/search_pq.hpp` | the search |
| host graph | `--graph-file diskann_t2i_10m.graph.bin` | `hide_read_nbrs` |
| `HidePipe` + bounce + `wait_covering` | `serving/hide_fill.hpp` | one rerank wave |
| `hide_extent_run` | same | tight-span hole fill on that wave |
| `--id-slot-map` + extent layout | 1100 GiB image | 2-in-4K physical pairing |
| `from_win=100` | `DramWindow` | score-after-install |
| pollute @420, 100 MiB cache | protocol | fair miss path |

Codebook (do not overwrite / regenerate without keep/drop vs 116.29 / 0.923):

```
/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin
/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin
```

## Dead (delete from the DiskANN default path)

These were keep/drop **drops**, or flags that `search_one_pq` never reads:

- `--pipe-drive` / mid-beam `issue_top_unresident`
- hop width as prefetcher identity (`--expand-batch` / `--issue-ahead` as e4 a1)
- `--score-page`, `--spec-beam-nbrs`, `--expand-sib`, `--lookahead-k`
- `--stripe-fill`, `--direct-install`, `--score-cache`, `--hide-warm-entry`
- `--pq-nav --oneshot-fp` hybrid (23.50 QPS, NAND still ~2 GB)
- PQ-32 as a quality claim (recall 0.742)

`--oneshot-fp` may remain as an **ablation** that issues neighbor pages per
expand through the same `HidePipe`. It is not the freeze and not the CLI
default. Do not keep hop extras (spec-beam, score-page, pipe-drive) to serve it.

## CLI after the replace

DiskANN hide default:

```
--diskann-layout --pq-nav --pq-pivots … --pq-compressed …
--graph-file … --id-slot-map … --extent-run
--no-oneshot-fp --no-pipe-drive --no-score-page --no-stripe-fill
--no-direct-install --no-score-cache --no-hide-warm-entry
--beam 400 --k 10 --threads 1 --policy P3
```

Refuse DiskANN hide without codebook unless `--oneshot-fp` (ablation) is set.

## Success

1. Source again contains `search_one_pq` (it is **missing** from the current
   `search_beam.cpp`; 110.94 / 116.29 came from an uncommitted binary).
2. Unit tests for `PqTable` ADC and `hide_extent_run` pass.
3. Reconfirm on vmem (pollute @420, extent @1100, seed-42, `fuser` empty):
   - nq=20: QPS ≥ 110, recall@10 ≥ 0.92, `from_win=100`
   - nq=100: QPS ≥ 114 (116.29 −2%), recall@10 ≥ 0.92, `from_win=100`
4. One freeze note. Dual A/B language gone.
5. Dropped flags are gone from `Prefetch` / DiskANN CLI defaults.

## Hard stops (unchanged)

No `insmod vmem.ko`, no `--switch`, no FPGA BAR, no `allow_hps_mmio=1`.
Reload `vmem_sw` only if `fuser /dev/vmem0` is empty. Do not write 420 / 460 /
800 / 900 / 930 / 950 / 1100. Do not `map_dram_dax` on a staged index.

## Out of scope

PrefetchHub / T>1, restaging images, replacing 50.25 / 85.8, retraining PQ,
FPGA offload.
