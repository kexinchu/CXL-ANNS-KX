# T=1 Hide e8/a2 design + page occupancy / remap (2026-09-04)

Does not replace 50.25 / 85.8. 1M DiskANN, recall@10 vs 10M GT is ~0.035.

## What Hide T=1 e8/a2 (86.6 QPS / 11.5 ms) actually is

Corpus: 1M DiskANN prefix on `/dev/vmem0` at **900 GiB**, `STRIDE=2048` (2 items / 4K). Host keeps only the 10k nav graph (exact scan → entry). Scoring window = host numa **2 GiB**. vmem page cache capped at **400 MiB**. `--policy P3 --pipe-w 16 --expand-batch 8 --issue-ahead 2 --no-score-page --look-k 0 --spec-beam-nbrs 0`. Warmup then timed nq=20.

Search: G0 10k scan → L=400 oneshot-fp. `pick_batch` commits the 8 closest unexpanded cand, then `issue_bundle_batch`. **There is no nbr-bundle on DiskANN**, so that function falls into the per-expand `hop_hide_score` path: each of the 8 expands issues its N(u) 4K pages and scores immediately. `issue-ahead=2` then picks another 8. e8 is a mild best-first break (lock 8 expands before re-ranking); it is **not** a single QD burst of 8 bundles.

`--no-score-page`: sibling on the same 4K page is not scored unless it is also a wanted neighbor. `page_use=100%` in the old log is “issued page later covered a score”, **not** 2-slot occupancy.

## Occupancy metric (2-in-4K)

For each issued 4K page: used 0 / 1 / 2 of the contained IDs this run → 0% / 50% / 100%. Mean = used_slots / slots.

Frozen 86.6 row reconstructed: `slots_on_pages=73018` `slots_scored=45654` → mean **62.52%**. This run’s warm sequential e8/a2: **63.01%** (74% of pages at 50%, 26% at 100%, 0% at 0%).

## T=1 steps (nq=20, L=400, k=10, seed=42, 400 MiB cache)

| Step | QPS | mean ms | page_occ | 50% / 100% | notes |
|------|----:|--------:|---------:|------------|-------|
| S0 sequential e8/a2 (warm) | **87.49** | 11.43 | **63.01%** | 74.0 / 26.0 | matches prior 86.6 |
| S1 spec-beam M=16 | 84.39 | 11.85 | 61.58% | 74.2 / 24.5 | 1.3% pages unused |
| S0b sequential `--score-page` | 73.26 | 13.65 | **100%** | 0 / 100 | junk siblings; dist 50k→83k |
| Neighbor remap e8/a2 (warm) | 76–79 | 12.7–13.1 | **64.3%** | 71.4 / 28.6 | static nbr-pair 91.7% of pages |
| Neighbor remap + score-page | 44.63 | 22.40 | 100% | 0 / 100 | slower |
| Neighbor + score + expand-sib | 44.66 | 22.39 | 100% | 0 / 100 | more expands |
| Neighbor + score + sib + spec16 | 62.68 | 15.95 | 99.18% | 0 / 99.2 | still below S0 |
| Co-expand remap e8/a2 (warm) | 73–75 | 13.3 | **62.8–63.0%** | ~74 / ~26 | static pair 99.0%; no query win |

Sequential layout: only **8.94%** of pages are graph-neighbor pairs. Neighbor remap raises that to **91.69%**. Query occupancy does not follow.

## Why remap does not raise query occupancy

Search only scores IDs in N(expanded). Occupancy is “given we paid for A’s page, was sibling B also scored in this query?” B is a neighbor of A (after remap), not necessarily a neighbor of the current expand. Co-expand pairing packs some static N(u), but those parents are not the ~400 nodes a query actually expands — a global 2-matching cannot satisfy every parent. So mean occupancy stays ~63% whether pages are sequential, neighbor-paired, or co-expand-paired.

`--score-page` forces 100% occupancy by scoring the sibling. On sequential layout those siblings are mostly junk (QPS 87→73). After neighbor remap they are real edges, but extra distance comps still lose T=1 (87→45).

## T=1 ceiling on this setup

Warm sequential e8/a2 is still the best T=1 row: **~87 QPS / 11.4 ms / occ 63%**. Breaking best-first (spec-beam, expand-sib) and page remap did not beat it. 1M cache-Oracle remains 275.59; Hide is still ~32% of that. Cache used the 400 MiB cap after staging writes (`evictions` climbing); the 86.6 row had ~179 MiB and `evictions=0`.

Remapped images live at vmem **910 GiB** (neighbor) and **920 GiB** (co-expand). 420/460/900 untouched.

## Co-occurrence + expand-trace packing (2026-09-04 night)

Weight `(v,w)` by how often they co-appear in the same `N(u)` (static graph) plus nq=20 Hide expand traces (`--dump-expands`, +1000 per co-issue). Greedy max-weight matching, 2-in-4K. Image at vmem **930 GiB**.

Predicted pages/expand **10.56** (seq 12.11). Warm T=1 e8/a2:

| Run | QPS | mean ms | page_occ | issued pages |
|-----|----:|--------:|---------:|-------------:|
| s9b | 94.56 | 10.57 | 90.24% | 25202 |
| s9c | 103.67 | 9.64 | 90.19% | 24874 |

vs sequential warm **87.49 / 11.43 / 63% / 36302 pages**. Occupancy: ~80% of pages at 100%, ~20% at 50%. `--score-page` still loses (69.7 QPS). spec16 = 85.4.

Closest published design: **Starling** block shuffling (PACMMOD 2024, Wang et al.) maximizes overlap ratio so graph-neighbors share a disk block, then block-searches (score everyone in the block). This run is the 2-slot special case, weighted by **expand co-fetch** rather than graph adjacency, and Hide still does **not** score unused siblings. VeloANN (2026) colors records during graph build. FlashANNS expand-bundle is a per-node copy of `N(u)`, not a global matching.

## MEPP removed

`--mode mepp`, `diskann_t2i_1m_mepp.bin`, `id_to_slot_1m_mepp.bin`, and s12 logs deleted. Default layout stays **930 GiB cooccur+trace**.

## T=1 pipe / spec-beam on cooccur (2026-09-04)

DiskANN default is now **issue the committed batch’s pages, then drain** (`--no-sync-hop`). `--sync-hop` restores the old per-expand `hop_hide_score`. 400 MiB vmem cache is already at `cache_limit`.

Hot cache (nvme ≈ 0):

| tag | QPS | mean | occ | issued | spec pages |
|-----|----:|-----:|----:|-------:|-----------:|
| **pipe e1 a1** | **108.63** | 9.20 | 91.2% | 24.0k | 0 |
| sync e8 a2 (old) | 105.40 | 9.49 | 89.7% | 25.3k | 0 |
| pipe e4 a2 | 105.57 | 9.47 | 89.6% | 25.9k | 0 |
| pipe e1 a2 | 102.92 | 9.71 | 91.0% | 24.1k | 0 |
| e1 + spec 8 | 103.46 | 9.66 | 89.2% | 24.3k | 3908 |
| e1 + spec 16 | 99.67 | 10.03 | 84.3% | 24.3k | 7975 |
| e1 + spec 32 | 91.67 | 10.91 | 79.7% | 27.2k | 14697 |
| look=8 / spec16+look8 | 86–100 | — | 85–86% | more | waste |

Cold/mixed cache (first sweep): **pipe e8 a2 = 58 QPS** (17 MB NAND, occ 86%); pipe e1 a1 = 99. Widening the committed batch while pipelining IO prefetches N(u) that best-first would not have expanded next.

PipeANN-style spec-beam / lookahead does **not** raise T=1 here: 400 MiB is already full of the query WS; extra async pages evict useful ones. Best row is strict best-first + issue-then-drain on the cooccur layout.

## 100 MiB cache A / B / C (2026-09-04)

vmem_sw `cache_size_mib=100` (`cache_limit=104857600`, ≈1/20 of the 1.91 GiB 1M image). T=1 nq=20 L=400 e1 a1 `--no-score-page --graph-file`. Does not replace 50.25 / 85.8. 1M recall@10 vs 10M GT stays ~0.035.

Prior 400 MiB hide-copy row **108.63 QPS / 9.20 ms** used 94 MiB of cache. Default `hide_warm_entry` still pinned 8192 nav extras (~31 MiB) and evicted that WS, so `--score-cache` could not hit (`from_cache≈23`).

| tag | layout | flags | QPS | mean ms | from_cache | nvme | notes |
|-----|--------|-------|----:|--------:|-----------:|-----:|-------|
| a0_nowarm | 930 cooccur | hide-copy, no extras | 91.18 | 10.97 | 0 | 5.5 MB | cache already held WS |
| **a1_nowarm** | 930 cooccur | **`--score-cache`**, no extras | **179.82** | **5.56** | **46463** | 0.5 MB | A: score from vmem soft-cache, no window install |
| b0_nowarm | 950 extent | hide-copy, no extras | 99.84 | 10.01 | 0 | 50 MB | pairs kept; pages of a trace expand now adjacent |
| **b1_cache** | 950 extent | `--score-cache`, no extras | **177.08** | **5.65** | **45733** | 1.1 MB | hot A+B ≈ A; NAND already ~0 |
| c_extras | 950 | `--score-cache` + default 8192 extras | 89.45 | 11.18 | 28 | 94 MB | extras evict WS |
| c0_dirty | 950 | `--score-cache`, dirty cache, no pin | 81.02 | 12.34 | 34 | 98 MB | 200 MiB pagebin pollute |
| **c1_pin** | 950 | `--score-cache` + 80 MiB trace pin | **175.68** | **5.69** | 3 | 16 MB | C: pin freq pages (not 8192 extras); window already filled |
| **c2_hot** | 950 | `--score-cache` after C | **180.39** | **5.54** | **45711** | 1.3 MB | A+B+C hot |

Extent remap (`--mode extent --map-in` cooccur map) packs each expand’s still-free 4K pages as one run. Trace expands: **1.27 stripes / 63.8% adjacent** (was ~5.3 stripes / 4% adjacent). Image at **950 GiB**. 420 / 460 / 900 / 930 untouched.

C pin list: `tools/make_trace_warm_ids.py` ranks pages by `--dump-expands` frequency, 80 MiB / 20480 pages / 40960 IDs. `--hide-warm-ids` + `--hide-warm-bytes` replace `eg.nodes` extras.

## nq=500 disjoint build / test (2026-09-04)

`query_10k.fbin` split with seed **20260904**: 500 build + 500 test, overlap 0 (`qids_build500.bin` / `qids_test500.bin`). Extent @ 950 and the 80 MiB pin list are rebuilt from **build** traces only (`expands_1m_build500.bin`, 185729 expands, 380472 unique pages ≈ 1.45 GiB). Timed rows use **test** only. `--query-ids` in `search_beam`. Does not replace 50.25 / 85.8 / nq=20 A/B/C rows.

Build-trace extent: pages/expand 6.29, stripes/expand **3.59**, adj **25.4%** (nq=20 leaked traces were 1.27 / 64% because that WS was tiny).

| tag | QPS | mean ms | from_cache | nvme | recall@10 | notes |
|-----|----:|--------:|-----------:|-----:|----------:|-------|
| **Oracle host 1M** | **266.69** | **3.75** | 0 | **0** | 0.0882 | `--oracle-dram --image diskann_t2i_1m.bin`; same test-500; nvme=0 |
| A0 hide 930 | 77.67 | 12.87 | 0 | 1.52 GB | 0.0876 | test WS ≫ 100 MiB |
| A1 score-cache 930 | 81.11 | 12.33 | 863 | 1.51 GB | 0.0876 | cache hits ≈ 0.07% of scores |
| **B0 hide 950** | **92.32** | **10.83** | 0 | 1.51 GB | 0.0882 | extent helps NAND when WS misses |
| B1 score-cache 950 | 82.49 | 12.12 | 797 | 1.51 GB | 0.0884 | same miss-bound as A |
| C0 dirty 950 | 83.25 | 12.01 | 864 | 1.51 GB | 0.0886 | 200 MiB pagebin pollute |
| C1 build-pin 80 MiB | 83.15 | 12.03 | 810 | 1.43 GB | 0.0886 | pin from **build**, not test |
| C2 after C | 80.91 | 12.36 | 932 | 1.51 GB | 0.0882 | still miss-bound |

nq=20 A/B/C (~180 QPS) was the 20-query union (~94 MiB) fitting in 100 MiB. nq=500 test issues ~372k pages / ~1.5 GiB NAND; 100 MiB holds ~1/15 of that union, so `--score-cache` and a disjoint 80 MiB pin do not raise QPS. Hide vs Oracle is **92.32 / 266.69 ≈ 0.35×**. 1M vs 10M GT recall@10 is ~0.088 on this 500, still not a quality claim.

## Hide-copy sequential opts on test-500 (2026-09-04)

Fair sweep: pollute 200 MiB of pagebin @420, then B0 flags on extent @950, test-500, 100 MiB cache, `--no-score-cache --no-hide-warm-entry --graph-file`. Keep if QPS up and recall@10 stays ~0.088; drop if hang or QPS down. Does not replace 50.25 / 85.8 / nq=20 A/B/C / Oracle 266.69 / B0 92.32.

| step | flags | QPS | mean ms | recall | vs prev kept | decision |
|------|-------|----:|--------:|-------:|-------------:|----------|
| baseline | e1 a1 bounce | **99.43** | 10.06 | 0.0884 | — | start |
| Opt1 | `--direct-install` | hang | — | — | hang after `roles` | **DROP** (reverted to bounce) |
| Opt2a | e4 a1 | **103.85** | 9.63 | 0.0884 | +4.42 | **KEEP** (best width) |
| Opt2b | e4 a2 | 103.10 | 9.70 | 0.0886 | +3.67 / −0.75 vs 2a | drop vs 2a |
| Opt2c | e8 a2 | 99.90 | 10.01 | 0.0894 | +0.47 / −3.95 vs 2a | drop vs 2a |
| Opt3 | e4 a1 `--stripe-fill` | 99.09 | 10.09 | 0.0884 | −4.76 vs 2a | **DROP** |
| **final kept** | **e4 a1 bounce** | **103.17** | **9.69** | 0.0884 | +3.74 vs baseline | only Opt2a |

Opt1 reserved window frames then `READ_BATCH` into the arena; reserved frames never committed so `wait_covering` spun, and leftover D-state `search_beam` wedged `/dev/vmem0` until those PIDs exited. CLI `--direct-install` remains a no-op. Opt3 filled 2 MiB stripe holes (`span≤64`); `page_use` 100%→89%, NAND 1.53→1.93 GB, QPS fell.

Final vs Oracle: **103.17 / 266.69 ≈ 0.39×** (was 99.43 / 266.69 ≈ 0.37× on the pollute baseline; 92.32 / 266.69 ≈ 0.35× on the earlier B0 row). Logs: `results/paper_figs/hide_1m_opt_*_nq500.log`.
