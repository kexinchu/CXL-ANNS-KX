# Frozen prefetcher on T2I-10M T=1 (2026-09-04)

**Superseded.** The single 10M freeze is
`docs/notes/2026-09-04-prefetcher-freeze.md` (PQ-64 end-batch, 110.94 / 116.29).
This file is the morning oneshot e4 a1 diary only.

Does not replace 50.25 / 49.25 / 85.8. Cache 100 MiB, window 2 GiB, e4 a1
bounce, `--no-score-page`, host `diskann_t2i_10m.graph.bin` (logical `N(u)`).

Two images (neither overwrites 420/460/900/930/950):

| layout | vmem | how |
|--------|------|-----|
| sequential | **800 GiB** | packed DiskANN, slot=id |
| **extent (frozen layout)** | **1100 GiB** | build-500 traces (disjoint from seed-42 timed-100) → `cooccur --trace-pairs` → `extent --map-in` |

Build traces: 205994 expands. Trace-set prediction after extent:
**9.02 pages / 3.16 stripes / 65% adj** (static N(u) stays ~30 pages / 21
stripes / 5% adj). Full-graph C(R,2) cooccur was killed: ~450 new pairs/node.

| row | nq | QPS | mean | p50 | p99 | recall@10 | nvme GB | slot_use |
|-----|---:|----:|-----:|----:|----:|----------:|--------:|---------:|
| seq e4 a1 | 20 | 25.04 | 39.93 | 42.06 | 71.72 | 0.945 | 0.47 | 57.0% |
| seq e4 a1 | **100** | **24.99** | **40.01** | 35.45 | 102.38 | 0.921 | 2.21 | 58.2% |
| extent e4 a1 | 20 | 13.81 | 72.40 | 42.31 | 391.04 | 0.955 | 0.45 | 60.1% |
| **extent e4 a1** | **100** | **24.75** | **40.41** | 37.59 | **82.43** | **0.939** | **2.07** | **61.4%** |
| host-file oracle | 100 | 130.83 | 7.64 | 7.48 | 14.80 | 0.935 | 0 | — |

nq=20 extent mean is a cold-cache tail (p99 391 after a 20 GiB stage + 420
pollute). Steady nq=100: **layout does not raise QPS** vs sequential
(24.75 vs 24.99). It issues 6% fewer pages, lifts slot_use 58→61%, cuts p99
102→82, recall 0.921→0.939. Hide / oracle stays **≈0.19×**.

## Pipeline bubble (nq=100, both layouts)

Wall is NAND-bound. `overlap≈0.03–0.04`. `crit_wait` undercounts ioctl/bounce.

| term | seq ms/q | extent ms/q |
|------|--------:|------------:|
| wall | 40.01 | 40.41 |
| host oracle compute | 7.64 | 7.64 |
| install memcpy | 13.05 | 12.53 |
| counted crit_wait | 1.64 | 1.12 |
| NAND @ measured GB/s | ≈40 (0.55) | ≈40 (0.51) |
| NAND if 1.56 peak | ≈14.1 | ≈13.3 |

Bubbles still open:

1. **Best-first serialize (main).** `a1`: next e4 wave waits for scores.
   Compute 7.6 ms cannot hide a ~20–40 ms NAND wave.
2. **Shallow QD.** Tens of 4K pages/wave. Occupancy 33–35% of 1.56 GB/s.
3. **Second copy.** ~12.5 ms/q bounce→window. `direct-install` stays dropped.
4. **Held-out layout miss.** Extent packing follows **build** expands. Timed
   seed-42 queries see almost the static graph (~21 stripes), so QPS is flat.
   More / closer traces, or online clustering, would be required for a QPS win.
5. **Sibling waste.** slot_use still ~61%. 77% of pages score only one of two
   STRIDE-2048 slots.
6. **Window ≪ corpus.** 2 GiB / 19 GiB. Sequential nq=100 had 18337 evicts
   (p99 102). Extent nq=100 had 0 evicts in that run (p99 82) — not a layout
   claim by itself.

## What is worth trying next (not done)

| idea | why it might help | risk |
|------|-------------------|------|
| `issue-ahead=2` **on extent** | overlap next wave while this one scores; pages more sequential on build-like walks | 1M sequential e4a2 was slightly worse |
| Issue the rest of an extent **run** (not stripe holes) | 65% adj on build traces; one 2 MiB sequential read | same waste as `--stripe-fill` if test walks miss the run |
| Score from bounce / one-copy | drop 12.5 ms memcpy | previous direct-install hung |
| Bigger build trace (2k–5k queries) | layout currently underfits the timed WS | remap + restage 20 GiB |
| Keep sequential @800 for serving | QPS already 25; extent is a no-op on this test | give up layout story on 10M held-out |

Logs: `hide_10m_t1_e4a1_nq{20,100}.log`, `hide_10m_t1_e4a1_extent_nq{20,100}.log`,
`oracle_10m_host_T1_nq100.log`.
