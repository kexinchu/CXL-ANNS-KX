# Frozen T=8: prefetcher + continuous batching + pipeline (2026-09-04)

This is the multi-thread serving method on top of the locked PQ-64 end-batch
prefetcher. It does **not** replace T=1 hide **110.94 / 116.29**, oracle
**161.52**, or paper rows **50.25 / 49.25 / 85.8**.

T=1 stays `search_one_pq` (shared window, no steal). See
`docs/notes/2026-09-04-prefetcher-freeze.md` and
`docs/notes/2026-09-04-prefetcher-pq-freeze.md`.

## Locked row (this machine, seed-42, nq=500, pollute)

| | QPS | NAND occ | from_win | recall@10 |
|--|----:|---------:|---------:|----------:|
| **hide T=8 steal (default)** | **689.32** | **64.3%** | **100** | **0.920** |
| hide T=8 band (same recipe) | 670–690 | ~63% | 100 | 0.920 |
| oracle T=8 (this machine) | **952** | — | — | — |
| hide / oracle | **≈0.71×** | | | |

Log: `results/paper_figs/hide_10m_pq64_thr8_ptw128_d2_steal_qdT_nq500_locked689.log`

Userspace is at a local max of this PQ-end-batch + steal path. The residual
to 952 is Flash materialization + kernel `copy_to_user` under `cache_lock`,
not another scheduler knob.

## Method (do not regress)

```
PQ-64 ADC beam on host graph
  → one end-batch FP wave of committed C_L (L=400)
  → HideInflight 32-page READ_BATCH (slab bounce + page prefault)
  → PageCopyPool::submit_fns + notify_all → DramWindow
  → steal_decide: Issue Hold if QD room → Fill empty → Rank covering → Pump
  → score from_win only (compute never wait_covering)
```

Runtime (T=8):

```
--steal-sched --pipe-depth 2 --issue-qd 0   # 0 → QD = T = 8
--per-thread-window --dram-bytes 128MiB --stagger-us 0
--no-direct-install
hide_issue chunk=32, shared PageCopyPool, serial admit (mutex, no sleep)
```

Recipe:

```
NQ=500 PTW=1 W=8 WIN_MIB=128 tools/run_hide_10m_t8_pq.sh t8
```

`t8` is this steal path. It is not `--cont-batch` / `--cont-workers`.

## Dropped (do not re-try as the default)

| Attempt | Result |
|---------|--------|
| Fat ioctl `READ_BATCH` chunk=256 | T=8 648→312; T=1 99→89. Keep chunk=32 |
| T>8 or `issue_qd=T` at T=16 | Worse than T=8 QD=8. T=12 QD=12 ~583–594 |
| `--early-cl` @256/320 | T=1 +6 QPS; T=8 581–646 vs 673–682. Extra waste pages. **Removed** |
| `--admit-gap-us` 500/1500 | 500 ≈ default; 1500 → 624. Steal already is CB. **Removed** |
| `--direct-install` (prefaulted) | No hang, but T=1 94 vs ~104; T=8 **648 vs 689** |
| `--score-bounce` / `--score-vmem` | 433 / 212 |
| Start stagger sleep | Threads re-phase; occ unchanged. Frozen path uses 0 |

OCC/CL_TRACE instrumentation is gone. `--early-cl`, `--early-cl-at`,
`--admit-gap-us` are gone.

## Invariants

- T=1 `search_one_pq` unchanged; `from_win=100`
- No `--pipe-drive` / `--oneshot-fp` / `--direct-install` as default
- No `--stripe-fill` / spec-beam
- `slot_use≈55%` is the frozen 2-in-4K layout, not a scheduler bug
- Do not raise CXL cache to 4 GiB on 10M
- Node-0 memory: steal uses **one** shared pool + 128 MiB windows
