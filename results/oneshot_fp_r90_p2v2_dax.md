# P0 / P1 / P2v2 on DAX (expand/score decouple)

**Setup:** Wikipedia-Cohere 25M, DiskANN R=32 MIPS, oneshot FP, L=300, k=10, nq=100, shuffle_seed=42, pin-entry, no flush (cross-query DramWindow reuse), `--dram-backend dax` (`/dev/dax0.0`, 1 GiB), host ≤32 MiB.

**P2v2 (this run):** cooperative single-threaded expand/score decoupling — expand pushes nbrs to `pending`; lookahead sync-prefetches *other* unexpanded cands (excludes `cur`); score when `is_resident` or demand when pending bound / frontier stall. No async worker (DAX+vmem concurrent promote previously SIGBUS).

**Infra note:** Before this suite, `vmem_sw` pointed at missing `/dev/nvme1n1` (namespace drifted to `/dev/nvme1n2` @ `d8:00.0`), causing cold-page SIGBUS. Reloaded via `tools/restore_vmem_sw.sh` (`ram_size_gib=28`). Numbers below are **warm soft-cache** pass after restore.

## Results (warm)

| Policy | recall@10 | QPS | hit% | mean ms | notes |
|--------|-----------|-----|------|---------|--------|
| **P0** | 0.929 | **1.41** | 10.47 | 711 | baseline demand promote |
| **P1** | 0.929 | 1.11 | **37.44** | 901 | sync lookahead (incl. cur) |
| **P2v2** | **0.937** | 0.89 | 33.55 | 1122 | expand∥score; lookahead ≠ cur |

CSV: `results/oneshot_fp_r90_p2v2_dax_warm_ab.csv`  
Cold-after-restore pass: `results/oneshot_fp_r90_p2v2_dax_ab.csv` (same hit%/recall; P0 QPS depressed by cold SSD).

## Takeaways

1. **P2v2 hit% ≈ P1** (33.6% vs 37.4%), vs old async P2 (~10.5% ≈ P0) which enqueued then immediately demand-scored.
2. **QPS:** P0 > P1 > P2v2 under this cooperative design — P2v2 does more distance comps (756k vs 643k) and more promotes/evicts; lookahead is still sync, so no I/O/compute overlap.
3. **Recall** slightly higher on P2v2 from the larger explored set (pending + deferred scoring), still ≥0.90 target.
4. True multi-thread async on DAX remains unsafe here; next step for QPS would be staging promotes into host DRAM then single-threaded install into DAX, or hardware that tolerates concurrent DAX writers.
