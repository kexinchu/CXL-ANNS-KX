# Hide vs Oracle — 2026-08-30 (real CXL-SSD)

Identity: `6.18.0-rc5`, FPGA `nvmex` `0000:d9:00.0` → `/dev/nvme4n1`, `/dev/vmem0`=`vmem_sw`. **DAX absent** (DramWindow = 1 GiB `mbind` node 1). Claim unchanged: **100% score-from-window**.

T2I-10M, oneshot-fp, L=400, k=10, seed=42.

Protocol: `rmmod`+`insmod` until `cache_used=0`. Graph from host file (`CXAN_HOST_LAYOUT` or `--graph-file`). Never copy graph from `/dev/vmem0`.

## Oracles

| Oracle | What it models | How |
|--------|----------------|-----|
| **host DRAM** | Corpus already in a large CXL-DRAM | `--oracle-dram --image layout_t2i_10m.bin` MAP_POPULATE 9.6 GiB |
| **1 GiB window** | Hide *succeeded*: WS already in the window | `--oracle-window`: discarded pass, then `freeze_fills` |

## Locked hide (current runtime + unlocked `vmem_sw`)

| Row | nq | QPS | mean ms | p99 ms | recall@10 | from_win% | fill GB/s | promote |
|-----|---:|----:|--------:|-------:|----------:|----------:|----------:|--------:|
| Oracle host DRAM | 20 | **66.5** | 14.8 | 47.6 | 0.950 | 100 | 0 | 0 |
| Oracle 1 GiB window | 20 | **85.8** | 11.4 | 15.5 | 0.925 | 100 | 0 | 0 |
| Hide packed stall32 (no score-page) | 20 | 24.67 | 40.5 | 57.4 | 0.930 | 100 page | slot ~25 | packed IDs |
| Hide pagebin **no siblings** | 20 | 26.34 | 37.9 | 76.6 | 0.955 | 100 page | **slot 25.4** | 1-of-5 |
| Hide pagebin score-page scalar | 20 | 14.94 | 66.9 | 110.8 | 0.965 | slot 100 | occ 20.4% | |
| Hide pagebin **AVX2 score-page** | 20 | 21.30 | 46.9 | 75.6 | 0.965 | slot 100 extra | occ 29.2% | extra compute |
| Hide pagebin expand-bundle ebatch=1 | 20 | 42.17 | 23.7 | 31.9 | 0.940 | slot 100 | occ 28.2% | |
| Hide pagebin bundle ebatch=8 | 20 | 47.24 | 21.1 | 28.5 | 0.945 | slot 100 | occ 36.0% | |
| Hide pagebin **ahead=2** | 20 | **50.25** | 19.8 | 26.6 | **0.940** | **slot 100** | occ **38.5%** | LOCKED |
| Hide pagebin **ahead=2** | 100 | **49.25** | 20.3 | 35.6 | **0.933** | **slot 100** | occ **36.0%** | LOCKED |
| Hide pagebin `--cont-batch 4` | 20 | 6.80 | 546 | 1005 | 0.955 | **100.00** | 0.31 | 46 MiB/q |
| Hide pagebin nq=100 | 100 | 2.79 | 358 | 851 | 0.932 | **100.00** | 0.12 | 42 MiB/q |

Locked: expand-bundle + ebatch 8 + issue-ahead 2 + look=0. **slot_use 100%**. nq=20 **50.25 QPS** recall 0.940; nq=100 **49.25** recall 0.933. NVMe **0.600/1.560 = 38.5%**. page_use 97%. Build: `g++ -O3 -mavx2 -mfma`. `--nbr-bundle`.

Iso-recall sweep (same seed, true-cold, pagebin): L=200/220 fail 0.92 on nq=20; L=250 nq=20 is 13.13/0.945 but nq=100 is **0.891** — do not plot. L=300 nq=100 is 0.913 — do not plot. Floor holds at **L=400**.

CSV: `results/paper_figs/hide_matrix.csv`.

## What moved QPS (do not shrink the claim)

1. **Score path already matched the contract** on the old 2.83 row (`from_win=100`, bounce=0). Time was NAND of newly discovered pages.
2. **Pagebin + `--graph-file`**: 1.41 GiB → 0.92 GiB promote / 20 q (35% fewer unique NAND bytes). Graph dump lives at `/mnt/disk0/chukexin_motivation/serving_t2i_10m/pagebin_graph.bin`.
3. **`PREFETCH_BATCH` off the search thread**: ioctl is synchronous (NAND). `hide_issue` now runs prefetch+memcpy on pool workers; `wait_covering` waits only for this hop's miss pages, not the lookahead slot.
4. **`vmem_sw` drops `cache_lock` during `page_io_batch`**: concurrent prefetch ioctls can overlap. Fill 0.18 → 0.28 GB/s. Module rebuilt in-tree (`VMEM_SW_ONLY=1`).
5. **`--cont-batch 4`**: +0.7 QPS, mean latency 165→546 ms. Device still ~0.3 GB/s. Not a latency win.
6. **nq=100 / cross-query pin**: failed. 4.15 GiB unique into a 1 GiB window; 244 k evicts. Random T2I queries have mostly disjoint vector pages. Soft-pin / no-tick did not create reuse.
7. **`vmem_sw` coalesced bios + `VMEM_BATCH_MAX=256`**: isolated prefetch probe **2.07 GB/s**. Hide useful fill stayed ~0.3 GB/s (beam still hop-bound).
8. **Defer miss scoring**: score resident neighbors immediately; pending ids fill in the background; `wait_covering` only when the frontier has no expandable candidate. pagebin L=400 nq=20 **7.79 QPS**, `crit_wait` ≈1.9 ms/q, recall 0.955, bounce=0.
9. **Iso-recall L sweep (true-cold)**: L=250 nq=20 is 13.13/0.945 but nq=100 is 0.891. L=300 nq=100 is 0.913. Floor holds at L=400 only. Do not plot the nq=20-only L=250 row.

10. **Clock frame alloc**: `install_full_page` scanned all 256 k frames for `UINT64_MAX` before the O(1) evict. Filling the window was $O(n^2)$; a full window made every nq=100 install $O(n_\text{frames})$. Replaced with clock `alloc_frame_unlocked()`. pagebin **7.79→23.43** / **2.96→26.94**; packed **3.81→19.46**. `from_win=100`, bounce=0, recall holds.
11. **`VMEM_IOC_READ_BATCH`**: 7.67 / 3.01 — not the ceiling (void as a win).
12. **Direct READ_BATCH into unfaulted window frames**: hung (`D` on `brk` / `copy_to_user` under `cache_lock`). Reverted. Do not retry without prefaulting dests.

13. **Prefetch page-use (unique issued pages that later cover a score).** `hide_prec` is scores/pages and is *not* this. Timed-window only (warm-entry cleared from the sets).
    - look=8: page_use **93.27%**, look_use 92.70%, miss_use 100%. ~7.5 k unused lookahead pages.
    - look=0 (only pages of vectors we will score): page_use **100%** on pagebin nq=20/100 and packed nq=20. QPS 20.81→**22.96** (nq=20). Default is now 0.
    - byte_use (scored vec bytes / issued page bytes) is 24–30% pagebin / 18% packed: 800 B vectors in 4 KiB pages, ~1.3 scored vecs/page. That is packing/locality, not unused prefetch pages.
14. **Real NVMe occupancy** (`stat` field 3 × 512, timed pass only — not “ioctl issued” and not `promote_GBps`):
    - Isolated PREFETCH_BATCH: seq **1.950 GB/s**, rnd true-cold **1.560 GB/s**.
    - Hide look=0: nq=20 **0.523 GB/s**, nq=100 **0.523 GB/s** → **0.523/1.560 = 33.6%** of rnd peak. Packed 0.634 GB/s → 40.6%.
    - Used-page bytes / nvme bytes ≈ **100%** (device moved what we scored). Waste is QD, not unused prefetch.
15. **chunk=256** look=0: 21.88 QPS, no occupancy win. Reverted to 32.
16. **Miss drop on full pipe**: `issue(miss)` used `stall_if_full=false` and 8 slots — hop 9+ dropped NAND until the frontier died. `stall_if_full` + 32 slots: pagebin **22.96→31.40** / **26.40→33.49**, nvme **0.52→0.69 GB/s** (43.9% of rnd peak). page_use stayed **100%**. `--cont-batch 2` SIGSEGV (shared window); not a row.
17. **In-page slot use (the real utilization).** `--no-score-page` slot_use **25.4%**. `--score-page` **100%**. recall 0.965.
18. **Issue-first then score:** 15.27 QPS / 20.9% occ — not a win (FP still on the search thread after NAND is issued).
19. **AVX2+FMA `vec_mips_neg`:** same 100% slot-use, nq=20 **14.94→21.30 QPS**, nvme **0.32→0.46 GB/s (29.2%)**. nq=100 **21.32 / 25.6%**. Distance comps still ~513 k.
20. **Parallel sibling score on the fill pool:** 11.34 QPS / 15% occ. Workers stolen from `PREFETCH_BATCH`. Reverted.
21. **Expand-bundle (page management, not extra compute).** Star-BFS page-share is 1%; expand slot-use **26%**. Remaining-max permute and train-trace pack do not transfer (eval 19–23%). Per-node replica of \(N(u)\): 7 sequential 4 KiB pages, 32×800 B, 267 GiB after the pagebin image. Expand prefetches only pages that hold unseen neighbors and scores only those neighbors. nq=20 **42.17 QPS** slot **100%** recall 0.940; nq=100 **42.93 / 0.924**. `from_win=100`. Occupancy still **28–29%**.
22. **look=8 on future bundles:** page_use 86%, slot 89%, QPS 31. Those pages are not yet committed expands. VOID.
23. **expand-batch 8:** issue 8 committed expands' bundles as one QD burst. nq=20 **47.24 QPS** slot **100%** page_use 96.9% nvme **0.561 / 36.0%**. nq=100 **47.53 / 0.942 / 34.4%**. ebatch 16/32 no QPS win.
24. **issue-ahead 2:** two committed waves before score (no unused lookahead). Drain uses one `try_copy` (no double lock). nq=20 **50.25 QPS** slot **100%** nvme **0.600 / 38.5%**. nq=100 **49.25 / 0.933 / 36.0%**. ahead=3 loses QPS.
25. **Score thread (not fill pool):** 2.37 QPS. `work_mu` convoy. Reverted.
26. **Slim warm (entry+1-hop bundles only):** 47.54 QPS / hit 0. Old 8192 primary warm helps. Reverted.
27. **pipe_w=32:** 46.47 QPS. No win.
28. **Zero-copy `with_resident` (score under window lock):** nq=20 **48.07** (slightly under 50.25). Reverted to `try_copy` then score outside the lock.
29. **Shared 1 GiB window, 4 threads:** no crash after per-thread `tls_metrics`. QPS **50.71**, mean **70 ms**, nvme **0.563 / 36.1%**. Window mutex serializes install+copy; cross-query reuse *cuts* NAND bytes. Not an occupancy win.
30. **Private 512 MiB windows (warm before timed):** useful-only IO overlapped across queries. slot_use stays **100%** (no extra scores).

| Threads | nq | QPS | mean ms | recall | nvme GB/s | occ vs 1.560 | notes |
|--------:|---:|----:|--------:|-------:|----------:|-------------:|-------|
| 1 (LOCKED) | 20 | **50.25** | 19.8 | 0.940 | 0.600 | **38.5%** | 1 GiB |
| 2 | 20 | 60.35 | 25.5 | 0.950 | 0.761 | 48.8% | 2×512 MiB |
| 3 | 20 | 71.15 | 30.5 | 0.945 | 0.881 | 56.5% | 3×512 MiB |
| 4 | 20 | 79.02 | 34.4 | 0.950 | 0.969 | 62.1% | 4×512 MiB |
| 2 | 100 | 63.85 | 23.5 | 0.928 | 0.748 | 48.0% | evicts 36k |
| **4** | **100** | **87.21** | 29.3 | **0.941** | **1.027** | **65.8%** | evicts 0 |
| 8 | 100 | 69.14 | 75.5 | 0.945 | 0.817 | 52.3% | oversub |

## Gap (claim stays)

- **slot_use 100%** (bundle pages, no extra scores), `from_win=100`. Single-query hide row unchanged: **50.25 / 49.25**.
- vs single-query oracle **50.25/85.8 ≈ 0.59×**.
- Real occupancy (nvme `stat` sectors / isolated rnd peak 1.560): single-query **38.5%**; 4 concurrent queries **1.027/1.560 = 65.8%**.
- Useful-only ceiling: ~12 MB/q × 87 QPS ≈ **1.04 GB/s ≈ 67%** of 1.560. 65.8% is that ceiling. 100% of 1.560 on *useful* traffic needs ~132 QPS or ~18 MB useful/q — not available without unused pages (forbidden) or a different query.
- Do not report page-touch, `promote_GBps`, or “busy because an IO is in flight” as occupancy.
- Shared 1 GiB + 4 threads is **not** the occupancy row (lock convoy).

31. **Kernel already merges adjacent 4K** in `vmem_sw_page_io_batch`. Userspace sort + one 56-page `READ_BATCH` **hurt** T=1 (nq=20 46.25, nq=100 41.48). Reverted chunk/sort.
32. **Score during `hide_wait`:** nq=20 **52.39** / occ 41.9%, nq=100 **44.60** (evicts up). Not locked.
33. **`insert_cand` tracked-worst:** nq=20 **52.71** / occ 40.3% / recall 0.950, nq=100 **47.14** < locked 49.25. Reverted so the hide binary still matches **50.25 / 49.25**.
34. Batch `install_full_pages` (one window lock per ready run) kept; same installs, fewer lock/unlock.

## Next attack (still no claim change)

T=1 hide tax did not beat **both** locked rows. Occupancy of useful traffic is at the bytes/q ceiling under 4-query overlap. Remaining: more useful bytes/q without unused slots; hugepage/copy path that does not change the beam. Do not issue unused pages. Do not retry score-page extra compute, score-during-wait, sort+one-chunk READ_BATCH, score thread+mutex, slim warm, pipe_w=32, look=8 future bundles, shared-window 4t for occupancy.
