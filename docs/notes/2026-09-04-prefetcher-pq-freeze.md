# Prefetcher + PQ-nav — freeze B (2026-09-04)

**Superseded as the dual-contract note.** The single freeze is
`docs/notes/2026-09-04-prefetcher-freeze.md` (PQ-64 end-batch, 110.94 / 116.29).
This file remains only as the keep/drop diary that closed nq=20 and pipe-drive.

**Verdict:** B is a **second frozen search contract** (PQ-64 beam + end-batch
FP rerank). Do **not** replace the oneshot-fp hide-copy freeze, and do **not**
replace claim rows 50.25 / 49.25 / 85.8 / oneshot 25.90 / oneshot oracle 131.

**CLI stays A.** DiskANN defaults remain e4 a1 + `--extent-run`. B is opt-in
`--pq-nav` and needs the 64B codebook. `--pipe-drive` stays off on B.

nq=20 and pipe-drive on/off are **closed**. Locked hide row is **nopipe**.

## Two contracts (both kept)

| | **A — hide-copy (already frozen)** | **B — PQ-nav (this note)** |
|--|-----------------------------------|----------------------------|
| Rank during beam | full-precision MIPS | 64-byte PQ ADC in host |
| NAND during beam | every expand’s neighbor vectors (~22 MB/q) | none (`N(u)` + codes in DRAM) |
| NAND at end | — | one FP rerank wave of L=400 |
| Hide T=1 nq=100 | **25.90** QPS / 0.939 | **116.29** QPS / **0.923** |
| Hide T=1 nq=20 | (A extent was a cold-cache tail) | **110.94** QPS / **0.925** |
| Oracle T=1 nq=100 | **131** QPS / 0.935 | **161.52** QPS / **0.923** |
| Oracle T=1 nq=20 | — | **161.63** QPS / **0.925** |
| Hide / oracle nq=100 | 0.20× | **0.72×** |
| Default CLI | yes (e4 a1 + `--extent-run`) | no (opt-in `--pq-nav`) |

A is the iso-FP ablation and the old claim path. B is the serving path that
breaks best-first NAND serialize. B does not become the DiskANN default
because it requires extra host files and is a different paper contract.

## B — what is locked

Host DRAM holds **graph** (`--graph-file`, logical `N(u)`) and **PQ**
(pivots + 64-byte codes, permuted by `new_to_old_pagebin.bin`). The window
only exists so the final L vectors can be scored from `DramWindow`
(`from_win = 100`).

```
G0 10k nav → entry
  → PQ beam L=400 (host graph + host codes, no SSD)
  → issue vector pages of the L candidates (one wave)
  → wait_covering → FP MIPS rerank → top-10
```

T=1, k=10, seed-42, cache 100 MiB, window 2 GiB, extent image @ **1100 GiB**,
`--id-slot-map`, `--extent-run`, bounce install. `--graph-file` is
`diskann_t2i_10m.graph.bin` (logical), not a slot-order extract.

Codebook (do not overwrite the PipeANN 32B pair; do not regenerate without
a keep/drop vs **116.29 / 0.923**):

```
/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin
/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin
```

Built from `mem_R32.data` by `tools/build_diskann_pq` (centroid, 64 chunks,
k=256, 15 iters, L2 encode). ADC vs exact −IP Spearman **0.985**.

### Runtime flags (B freeze)

```
--diskann-layout --nav-graph … --graph-file … --id-slot-map … \
--pq-nav --pq-pivots idx_t2i64_pq_pivots.bin \
--pq-compressed idx_t2i64_pq_compressed.bin \
--beam 400 --k 10 \
--expand-batch 8 --no-pipe-drive --extent-run \
--no-score-page --no-stripe-fill --no-direct-install \
--no-score-cache --no-hide-warm-entry --no-sync-hop \
--threads 1 --shared-window --policy P3
```

No `--oneshot-fp` (that is A, or the dropped hybrid). Recipe:

```
PQ_BYTES=64 NQ=100 PIPE=0 tools/run_10m_pq_nq100.sh hide_pqbeam
PQ_BYTES=64 NQ=20  PIPE=0 tools/run_10m_pq_nq100.sh hide_pqbeam
PQ_BYTES=64 NQ=20           tools/run_10m_pq_nq100.sh oracle
```

Script default is now `PIPE=0`.

## Keep / drop (same 10M pollute protocol)

| item | decision | why |
|------|----------|-----|
| PQ-64 beam + end-batch FP rerank | **KEEP / freeze B** | nq=20 0.925 and nq=100 0.923; hide 26→116 QPS |
| B `--pipe-drive` e8 | **DROP** | nq=20 71.72 vs **110.94**; nq=100 72.48 vs **116.29** |
| PQ-32 beam | **DROP** as quality claim | recall 0.742 (L=1600 still 0.887) |
| `--pq-nav --oneshot-fp` hybrid | **DROP** | 23.50 QPS, NAND still 2.0 GB |
| oneshot e4 a1 + `--extent-run` | **KEEP as contract A** | 25.90 / 0.939; iso-FP baseline |
| oneshot `--pipe-drive` e8 | keep flag, not default of A | +1% QPS, p99 107→71 |
| oneshot a2 / e8 a2 / `--score-page` / `--stripe-fill` / `--direct-install` | **DROP** | already dropped on A |
| Flip DiskANN CLI to `--pq-nav` | **NO** | B needs codebook; A remains the default contract |

## Evidence (pollute @420, seed-42, extent @1100)

| row | QPS | mean ms | p99 | recall@10 | nvme | pipe |
|-----|----:|--------:|----:|----------:|-----:|:----:|
| A hide e4 a1 + extent-run nq=100 | 25.90 | 38.60 | — | 0.939 | 2.06 GB | off |
| A host oracle oneshot nq=100 | 131.00 | 7.63 | — | 0.935 | 0 | — |
| B host oracle PQ-64 nq=100 | **161.52** | **6.19** | — | **0.923** | 0 | — |
| B host oracle PQ-64 nq=20 | **161.63** | **6.19** | 8.00 | **0.925** | 0 | — |
| B hide e8 pipe-on nq=20 | 71.72 | 13.94 | 26.42 | 0.925 | 0.033 GB | on |
| B hide **nopipe** nq=20 | **110.94** | **9.01** | 19.68 | **0.925** | 0.030 GB | **off** |
| B hide e8 pipe-on nq=100 | 72.48 | 13.80 | — | 0.923 | 0.17 GB | on |
| B hide **nopipe** nq=100 | **116.29** | **8.60** | 13.44 | **0.923** | **0.15 GB** | **off** |

Logs:

- `oracle_10m_pq64_T1_nq100.log`, `oracle_10m_pq64_T1_nq20.log`
- `hide_10m_pq64beam_e8pipe_nq100.log`, `hide_10m_pq64beam_e8pipe_nq20.log`
- `hide_10m_pq64beam_nopipe_nq100.log`, `hide_10m_pq64beam_nopipe_nq20.log`

## Why pipe-drive is a DROP on B

On B, expands need no NAND. `--pipe-drive` only prefetches top-W unresident
rerank candidates during the CPU beam. That issues extra empty pages
(nq=20: 7918 issued / 742 empty vs nopipe 7176 / 0 empty) and collapses
useful QD on the real end-batch wave. Wall time is worse even though
`crit_wait` looks smaller: nq=20 hide 13.94 ms (pipe) vs **9.01 ms** (nopipe);
overlap 0.02 vs **0.21**.

Keep `--pipe-drive` as an A flag (oneshot p99). It is **not** part of B.

## Why this is a new prefetcher, not a wider ebatch

Oneshot A is NAND-bound: ~22 MB/q × 0.53 GB/s ≈ 40 ms, compute only 7.6 ms.
`--pipe-drive` can raise overlap to 0.25 but then QD collapses (e4 23.80).

B removes those 22 MB from the beam. The remaining IO is one rerank wave
(~400 vectors, ~1.5 MB/q). Best-first **information** dependence moves to PQ
in DRAM; the hide residual is rerank install + shallow random-read QD
(nvme ≈ 0.17 GB/s on that wave). Hide/oracle moves from 0.20× (A) to **0.72×**.

## Still open (not blockers)

1. Codebook is **home-built**, not the PipeANN 32B release. Pin the two
   `idx_t2i64_*` files; do not regenerate without a keep/drop vs **116.29 / 0.923**.
2. PQ-beam `page_use` metric is wrong (reports 0). Fix before any page-use claim.
3. CLI defaults stay **A**. Flip only if a later paper row wants B as the
   DiskANN default *and* the codebook is always present.

## Out of scope (unchanged)

T=8 PrefetchHub, `--direct-install`, restaging a larger trace set, replacing
50.25 / 85.8, writing 420 / 460 / 800 / 900 / 930 / 950 / 1100.
