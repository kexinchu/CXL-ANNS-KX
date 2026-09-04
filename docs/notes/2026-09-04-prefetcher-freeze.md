# Frozen PQ-64 end-batch prefetcher (2026-09-04)

One DiskANN-10M freeze: host PQ-64 beam + one end-batch FP rerank wave.
PIPE stays 0. Does not replace T2I-10M claim rows 50.25 / 49.25 / 85.8.

```
G0 10k nav → entry
  → PQ-64 beam L=400 (host N(u) + host codes, no SSD)
  → issue vector pages of the L candidates (one HidePipe wave)
  → wait_covering → install bounce → DramWindow
  → FP MIPS rerank → top-10
```

## Invariants (do not regress)

1. Score FP only after the page is in the host `DramWindow` (`from_win = 100`).
2. Issue only committed rerank **miss** pages. No mid-beam NAND. No spec-beam.
3. Bounce buffer → window memcpy. `--direct-install` stays a no-op.
4. Host `--graph-file` is required so `N(u)` is not an SSD mmap fault.
5. Extent image @ 1100 GiB + `--id-slot-map` + `--extent-run` on that one wave.
6. T=1, L=400, k=10, seed-42, cache 100 MiB, window 2 GiB host numa.

## Runtime flags

DiskANN hide requires `--pq-nav` (codebook) or `--oneshot-fp` (ablation).
`--extent-run` defaults on. There is no e4 hop-width default. Freeze path is
`--pq-nav` + codebook, no `--oneshot-fp`.

```
--diskann-layout --pq-nav --pq-pivots … --pq-compressed …
--graph-file … --id-slot-map … --extent-run
--no-oneshot-fp --no-pipe-drive --no-score-page --no-stripe-fill
--no-direct-install --no-score-cache --no-hide-warm-entry
--beam 400 --k 10 --threads 1 --policy P3
```

Recipe: `PQ_BYTES=64 PIPE=0 tools/run_10m_pq_nq100.sh hide_pqbeam` (`NQ=20|100`).
Oracle: `PQ_BYTES=64 tools/run_10m_pq_nq100.sh oracle`.

Codebook (do not overwrite / regenerate without keep/drop vs 116.29 / 0.923):

```
/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_pivots.bin
/mnt/disk0/chukexin_motivation/pipeann_t2i10m/idx_t2i64_pq_compressed.bin
```

## Evidence (pollute @420, seed-42, extent @1100)

| nq | hide QPS | mean | recall@10 | hide/oracle |
|---:|---------:|-----:|----------:|------------:|
| 20 | **110.94** | 9.01 ms | **0.925** | **0.69×** (161.63) |
| 100 | **116.29** | 8.60 ms | **0.923** | **0.72×** (161.52) |

Logs: `results/paper_figs/hide_10m_pq64beam_nopipe_nq{20,100}.log`.
`--pipe-drive` is a DROP on this path and is not a freeze row.

## Superseded hide-copy (2026-09-04 morning)

Old freeze: oneshot-fp **e4 a1**, hide **25.90** QPS / **0.20×** oracle
(NAND ~22 MB/q during the beam). **Replaced by 110.94 / 116.29.**

That contract scored every expand from full-precision vectors, used hop width
as prefetcher identity, and lived in
`docs/notes/2026-09-04-prefetcher-10m-t1.md`. Keep the morning note as
history. Do not treat 25.90 / e4 a1 / 0.20× as the DiskANN-10M freeze.
