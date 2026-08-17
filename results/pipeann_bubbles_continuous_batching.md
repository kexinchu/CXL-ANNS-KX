# PipeANN pipeline bubbles → continuous batching (SSD validation)

> Machine: gpu01. Media: `/dev/nvme0n1` (Dell PM1733a, enterprise NVMe), index on `/mnt/disk0`.
> Dataset: 984,133 × 1024-d float (`diskann_data_1m`), 100 queries; L2; PipeANN disk index R=64 L=128 PQ=32.
> Reader: `LinuxAlignedFileReader` (io_uring, O_DIRECT) → reads hit the SSD, not page cache.
> Code: `third_party/PipeANN` (thustorage/PipeANN, OSDI'25) + our instrumentation.

## 0. TL;DR
1. **Bubble sources confirmed** (code + measured): single-query `pipe_search` cannot fill its own
   I/O pipeline. Utilization 0.53–0.92, **14–42% of loop iterations are pool-starved**, and widening
   the width ceiling W makes utilization *worse*, not better. Instrumented `mean_inflight` matches
   `iostat` device queue depth (aqu-sz) to within 2%.
2. **Cross-query interleaving fills the bubbles.** PipeANN's `coro_search` (mode 3) is a *static* batch
   of 8 queries/thread → device QD 9.6→13.8, QPS +44%, iso-recall.
3. **Continuous batching (our `cont_search`, mode 4)** — admit the next query the instant a lane
   retires. v1 was *lane-level* (per-hop barrier); v2 (§3.1) is a **read-level global scheduler**
   that keeps the device at a **target depth `D`** at single-read granularity (no per-hop / group
   barrier). Read-level makes device queue depth a controllable knob: software depth = `threads×D`
   exactly, and measured device aqu-sz scales with it (16-thread D=32 → aqu **mean 302, peak 502**;
   vs coro 223, pipe 82), all at **iso-recall** (86.60 == pipe on untiled 100q). On this fast NVMe
   QPS is throughput-bound (~4.3k), so the win is precise depth control, not raw QPS — see §3.1.

## 1. Bubble sources in PipeANN `pipe_search` (mode 2)
From `src/search/pipe_search_common.h` main loop (l.404–437):
- **S1 dependency / pool-starvation** — `pool.next_read_nbr()` returns null: nothing to prefetch
  because the next hop isn't known until the current page is scored (pointer-chasing). Dominant in
  the approach phase / low-L.
- **S2 dynamic-width ramp** — `cur_beam_width` starts at 4 and only grows when I/O waste < 10%, so it
  sits far below the Wmax ceiling during the approach phase.
- **S3 one-at-a-time refill** — `send_best_read_req(1)` issues a single read per iteration → transient
  underfill after bursty completions.
- **S4 end-of-query drain** — final loop only drains, in-flight → 0.

### Measured (single thread, true SSD; instrumentation cross-checked with iostat)
`pipe_util = mean_inflight / mean_dynamic_W`; `starve_frac = starved_iters / loop_iters`.

| L | Wmax | pipe_util | mean_inflight | mean_dyn_W | starve_frac | io_waste | Recall@10 |
|---|------|-----------|---------------|------------|-------------|----------|-----------|
| 100 | 16 | 0.72 | 5.27 | 7.25 | 0.35 | 0.02 | 82.8 |
| 150 | 16 | 0.85 | 7.99 | 9.23 | 0.21 | 0.01 | 86.6 |
| 200 | 16 | 0.92 | 10.28 | 11.11 | 0.14 | 0.00 | 88.6 |
| 100 | 32 | 0.63 | 5.61 | 8.89 | 0.35 | 0.03 | 82.7 |
| 150 | 32 | 0.76 | 8.72 | 11.21 | 0.24 | 0.01 | 86.6 |
| 200 | 32 | 0.86 | 12.17 | 14.08 | 0.18 | 0.01 | 88.6 |
| 100 | 64 | 0.58 | 5.93 | 10.32 | 0.36 | 0.03 | 82.9 |
| 150 | 64 | 0.71 | 9.43 | 13.34 | 0.24 | 0.02 | 86.6 |
| 200 | 64 | 0.81 | 13.37 | 16.40 | 0.17 | 0.01 | 88.7 |

Takeaways:
- **Widening W lowers utilization** (L=100: 0.72→0.63→0.58 for W=16→32→64). A single query can't
  supply enough independent reads; raising the ceiling only widens the gap. mean_inflight never
  exceeds ~13 even at Wmax=64.
- **Recall is invariant to W** (82.8/82.7/82.9 at L=100) — W is a parallelism/latency knob, L is the
  recall knob. Confirms decouple-recall-from-fetch-parallelism.
- **iostat cross-check** (L=150, W=32): instrumented mean_inflight 9.85 vs device aqu-sz **9.80**.
- vs `beam_search` (mode 0, strict compute–I/O order): pipe is ~2.5–3× QPS at iso-recall
  (L=200: pipe 644 vs beam 235 QPS; 1537µs vs 4194µs), but still leaves 15–40% bubbles.

## 2. Cross-query interleaving fills the bubbles
`coro_search` (mode 3) runs 8 queries/thread interleaved over one io_uring (static batch).
NOTE: upstream hardcodes `kMaxVectorDim=512`; patched to 1024 for this dataset.

Single thread, L=150, 8,000-query workload (tiled), iso-recall:

| mode | per-query W | QPS | device aqu-sz |
|------|-------------|-----|---------------|
| 2 pipe (single-query pipeline) | 32 | 754 | 9.8 |
| 3 coro (static batch of 8) | 4 | 1083 | 13.8 |

→ interleaving independent queries raises device QD 9.8→13.8 and QPS +44% at the same recall.
But static batch tops out ~14 and has a **tail-drain bubble**: a group of 8 returns only when the
slowest finishes, so QD sags as the batch empties.

## 3. Continuous batching (`cont_search`, mode 4) — admit-on-retire
Implemented in `src/search/coro_search.cpp` (`SSDIndex::cont_search`) + harness mode 4
(`PIPEANN_LANES` env sets lanes). Keeps `n_lanes` queries in flight; when a lane's query ends it is
finalized and the next pending query is admitted immediately (no group barrier).

Single thread, L=150, 8,000 queries, iso-recall (86.5%):

| mode | config | QPS | device aqu-sz |
|------|--------|-----|---------------|
| 2 pipe | W=32 | 754 | 9.8 |
| 3 coro static | batch 8, W=4 | 1083 | 13.8 |
| **4 cont** | **lanes 8, W=4** | **1157** | **14.4** |
| 4 cont | lanes 16, W=4 | 1109 | 14.0 |

- Continuous > static > single-query on both QPS and device QD, iso-recall.
- lanes=16 ≤ lanes=8 here: **one core is now the bottleneck** (proxy in-flight ≈31 » device aqu 14),
  i.e. the CPU can't consume/re-issue fast enough to push this enterprise SSD past ~14 from a single
  core.

Multi-thread (16 threads, workload finishes in ~2 s so QPS is saturation-bound/noisy, but device QD is
clear):

| mode | QPS | device aqu-sz |
|------|-----|---------------|
| 2 pipe | 4017 | 113 |
| 3 coro static | 4251 | 260 |
| 4 cont lanes=8 | 4224 | **319** |

Continuous batching sustains the highest device queue depth.

Iso-recall check (untiled 100 queries, L=150): mode2 86.60, mode3 86.50, mode4 86.50 → identical
recall; continuous batching is a pure scheduling change.

## 3.1 Read-level global scheduler (target device depth D) — mode 4 v2

The first `cont_search` was **lane-level**: each of `n_lanes` queries issued a whole
beam of `W` reads and then waited at a *per-hop barrier* (all `W` had to land before that
lane advanced). Device depth was therefore an emergent quantity capped near the single-core
ceiling (~14) and not directly controllable.

Rewrote `SSDIndex::cont_search` (`src/search/coro_search.cpp`) as a **read-level global
scheduler**:
- One global pool of `D` read slots (each owns a persistent `IORequest` + sector-aligned
  buffer; io_uring stores `&req` in `user_data`, so the buffer must outlive the read).
- Keep the device at **target depth `D`** by issuing *individual* reads, round-robin across
  `n_lanes` live queries (one read per lane per pass → reads spread across queries, per-query
  speculative waste stays bounded).
- **No per-hop and no group barrier**: any lane whose page landed advances immediately
  (expand → re-insert → supply its next read); any lane that retires is finalized and refilled
  with the next pending query at once. The only bubbles left are (a) fewer than `D` candidates
  exist across all live lanes, or (b) the query stream is drained.
- Knobs: `beam_width` → target depth `D`; `PIPEANN_LANES` → `n_lanes` concurrent queries.
  Harness prints `mean_QD/thread` and `agg_QD` (software-side achieved depth).

**Iso-recall** (untiled 100q, L=150, Recall@10): pipe `86.60` == cont read-level D=32 lanes=8
`86.60` (lanes=16 `86.40`) → identical recall; pure scheduling change.

### Achieved device depth scales with D (8000q, L=150, iso-recall ≈ 43.3 across all modes
— low absolute value is a tiled-GT artifact; untiled is 86.6 and identical across modes)

Single thread — software holds exactly `D`, but the **device** aqu-sz stays ~11–14 because one
core cannot drain+re-issue fast enough to push this enterprise NVMe deeper (CPU-bound; the same
ceiling coro hits):

| mode | target D | sw mean_QD | dev aqu (mean/peak) | QPS |
|------|----------|-----------|---------------------|-----|
| pipe W=32 | — | 8.9 (inflight) | 8.9 / 10.4 | 734 |
| coro static8 | — | — | 12.0 / 14.1 | 1026 |
| cont D=16 lanes=8 | 16 | 16.0 | 10.4 / 11.5 | 852 |
| cont D=32 lanes=8 | 32 | 31.96 | 10.9 / 13.0 | 913 |
| cont D=32 lanes=16 | 32 | 31.96 | 13.1 / 14.2 | 956 |
| cont D=64 lanes=16 | 64 | 63.56 | 12.9 / 14.8 | 960 |

Multi-thread — device depth becomes **directly dialable** as `agg_QD ≈ threads × D`, and the
device queue tracks it up (iostat), at iso-recall:

| threads | mode | target D | agg_QD (sw) | dev aqu (mean/peak) | QPS |
|---------|------|----------|-------------|---------------------|-----|
| 8  | pipe W=32       | —  | —      | 52.9 / 68.5   | 3436 |
| 8  | coro static8    | —  | —      | 85.3 / 142.2  | 4319 |
| 8  | cont D=16 l=8   | 16 | 128.0  | 66.6 / 114.7  | 4253 |
| 8  | cont D=32 l=8   | 32 | 255.9  | 143.3 / 239.4 | 4168 |
| 16 | pipe W=32       | —  | —      | 82.2 / 132.9  | 4299 |
| 16 | coro static8    | —  | —      | 222.9 / 383.1 | 4341 |
| 16 | cont D=16 l=8   | 16 | 255.9  | 143.1 / 247.1 | 4180 |
| 16 | cont D=32 l=8   | 32 | 511.7  | **302.2 / 502.4** | 4063 |

Takeaways:
- **Read-level scheduling makes device queue depth a first-class, controllable knob.** Software
  depth = `threads × D` exactly (16×32 = 511.7 observed); measured device aqu-sz scales with it
  (16-thread D=32 → aqu mean 302, peak 502).
- **QPS is throughput-bound at ~4.3k on this NVMe.** Once the device throughput knee is reached,
  extra depth buys latency headroom / speculation, not QPS — so read-level cont matches coro/pipe
  QPS at iso-recall rather than beating it *on fast block NVMe*. Its value is the **precise depth
  control** that the lane-level batch (and pipe/coro) could not provide.
- The single-core QPS sweet spot stays at small effective width (coro/W=4): the CPU saturates
  before the device, so deep D just adds speculative CPU work. Depth control pays off exactly when
  the media miss penalty is large (flash-backed CXL-SSD, ~140× a device-cache hit) — deep queues
  hide misses — and when many hosts share one device and need explicit per-tenant QD budgeting.

Repro:
```bash
# read-level cont: beamwidth arg = target depth D; PIPEANN_LANES = concurrent queries
PIPEANN_LANES=8 tests/search_disk_index float idx_1m 16 32 \
  query_tiled.bin gt100_tiled.bin 10 l2 pq 4 0 150     # threads=16, D=32
# device QD cross-check: iostat -x -y 1 N nvme0n1  (aqu-sz column; note '=' in log names breaks awk)
```

## 3.2 Why read-level barely moved QPS on NVMe — and where it *does* win

The read-level scheduler holds device depth at target `D`, yet single-/multi-thread QPS on the
PM1733a was flat (even slightly down at large `D`). Root cause, measured, not guessed:

**The NVMe is bandwidth-bound at the depths we reach.** At 16 threads a busy iostat sample shows
`r/s = 729{,}572`, `rkB/s = 5.836 GB/s`, `%util = 100`, while **CPU is 65--77% idle**. So the
device throughput wall (~5.8 GB/s of 8 KB random reads ≈ 730k IOPS) is the limiter, not queue
depth and not CPU. Once bandwidth-bound, a deeper queue only adds speculative reads — it cannot
raise QPS. Per-query cost is high: `~170` device reads × 8 KB = **1.36 MB/query** at L=150.

**Cross-query hot-node overlap is weak here** (so a shared cache is not the easy win): profiling
every device read (`PIPEANN_RDPROF=1` → per-page histogram) over 100 *distinct* queries gives
`reads/uniq = 1.07`, only `3.3%` of pages read by >1 query, and caching the top 10% hottest pages
serves just `15.8%` of reads (top 1% → 4.9%). The full working set of the workload is small though
(~16k pages ≈ 131 MB), so a bounded DRAM/device cache still helps *under query locality*.

**The scheduler's real lever is latency-hiding, which a fast NVMe cannot exercise.** Throughput
≈ achieved_QD / latency (Little's law). `pipe_search` is pointer-chasing so its device QD is stuck
at ~9.5 no matter what; read-level `cont` holds QD at `D`. So the win appears only when *latency*,
not bandwidth, is the limiter. Emulated by injecting a per-completion delay (`PIPEANN_LAT_US`,
parallel — reads still overlap in the delay buffer), single thread, 100 distinct queries, L=150,
**iso-recall 86.6% throughout**:

| injected latency | pipe W=32 (QD≈9.5) | cont best QPS (D) | speedup |
|------------------|--------------------|-------------------|---------|
| 0 µs (raw NVMe, BW-bound) | 730 | 716 (D=32)  | 0.98× |
| 100 µs | 362 | 686 (D=64)  | 1.9× |
| 300 µs | 178 | 578 (D=128) | 3.2× |
| 1000 µs | 62 | 390 (D=128) | 6.3× |

- **pipe QPS collapses ∝ 1/latency** (730→362→178→62) because its device QD never exceeds ~10.
- **cont degrades gracefully** and the *optimal `D` grows with latency* (32→64→128→128): deeper
  queues hide more of the miss penalty. At 1 ms (flash-miss-scale) read-level is **6.3× pipe**.
- At 0 µs deeper `D` is counter-productive (speculative CPU, nothing to hide) — matching the NVMe
  result. This is the whole point: **the benefit is a function of device latency**, and a
  flash-backed CXL-SSD (device-cache miss ≈ 100 µs–1 ms, ~100× a hit) sits far to the right of this
  table, exactly where read-level continuous batching pays off.

### Next levers to raise QPS
1. **Match `D` to device latency** (auto-tune to the miss-penalty regime) — already shows 6.3×.
2. **Validate on the real high-latency path**: run over the flash-backed CXL-SSD (`/dev/vmem0`)
   instead of injected latency.
3. **Cut bytes/query to raise the bandwidth ceiling** (helps even the NVMe case): (a) PQ-only
   traversal with *deferred* full-precision re-rank of a small finalist set (skip 4 KB coord reads
   for most visited nodes; recall-gated); (b) compact / half-page node reads.
4. **Bounded working-set / device-DRAM cache** for skewed real query loads (~131 MB WS here),
   plus **in-flight read coalescing** on the global slot table (near-free on top of read-level;
   scales with concurrency & locality).

Repro: `PIPEANN_LAT_US=1000 PIPEANN_LANES=32 tests/search_disk_index float idx_1m 1 128 \
  <query100> gt100.bin 10 l2 pq 4 0 150`  (pipe: mode 2, W=32). Bottleneck check:
`iostat -x -y 1 N nvme0n1` (rkB/s, %util) vs `avg-cpu %idle`. Overlap: `PIPEANN_RDPROF=1 ... → /tmp/rdprof.csv`.

## 3.3 Is the bandwidth *always* saturated? (correct metric: IOPS/inflight, not %util)

Careful re-measurement with a 100 ms `/proc/diskstats` sampler over a long steady workload
(80k queries) and the **right saturation metric**. `%util` is misleading on NVMe: it means "≥1 I/O
in flight some of the interval" and pins to ~100% even at shallow depth. True saturation is
**achieved IOPS / device-max (~728k, 5.8 GB/s @ 8 KB)** and **in-flight (aqu-sz)**.

Dimension of the current scheduler: **per-thread** read-level (single SSD-op granularity, no
per-hop barrier), with `num_threads` *independent* schedulers each holding its own depth `D` over
its own io_uring ring; the device is shared but scheduling is not global.

| config | steady IOPS | % of 728k | mean inflight | device-idle (frac_zero) | QPS |
|--------|-------------|-----------|---------------|--------------------------|-----|
| 16thr pipe          | 729,621 | 100% | 135 | 0.0% | 4306 |
| 16thr cont D=32     | 727,579 | 100% | 500 | 0.0% (util min 97%) | 4323 |
| 4thr pipe           | 391,403 | **54%** | 35 | 0.0% (**util 100%!**) | 2306 |
| 4thr cont D=32      | 622,874 | **86%** | 78 | 0.0% | 3757 |
| 4thr cont D=64      | 621,307 | 85% | 85 | 0.0% | 3711 |
| 4thr cont D=128     | 568,797 | 78% | 91 | 0.5% | 3428 |
| 2thr cont D=128     | 309,107 | 42% | 31 | **17.2%** | 1892 |

Findings:
- **At 16 threads the device BW is genuinely pegged (728k IOPS, inflight never 0, no bubbles)** —
  and *both* pipe and cont reach it, so cont's deeper queue is redundant *on this fast NVMe at high
  core count*. QPS is hard-capped at ~4.3k = BW / (170 reads × 8 KB).
- **At few cores there IS a real bandwidth bubble that `%util` hides.** 4-thread pipe shows
  `%util=100%` but delivers only **54%** of device BW; read-level cont recovers it to **86%**
  (1.63× QPS). So finer batching's real win here is *reaching the BW wall with fewer cores*.
- **Deeper `D` does not close the last gap — it backfires.** 4-thread: D=32→86%, D=64→85%,
  D=128→78%; 2-thread D=128 collapses to 42% with **17% device-idle**. The limiter at low core
  count is **CPU work per read** (uring submit/reap + page parse + PQ distance + pool/dedup),
  ~155k IOPS/core. Beyond a core's issue rate, extra depth just parks completed reads unconsumed
  while the *device* drains and starves.

**Answer to "does request-level continuous batching keep BW always full, no bubbles?"**
Yes — but only once enough cores are present to pay the per-read CPU cost (~5+ cores here to reach
728k). At 16 cores it is bubble-free and BW-saturated. You cannot substitute cores with deeper
queues: the ceiling is CPU-per-read, not queue depth.

### To saturate BW with *fewer* cores and stay bubble-free (the real next step)
1. **Global request-level scheduler with I/O–compute separation.** Replace N independent per-thread
   queues with one shared device queue driven by 1–2 dedicated poller threads that keep it pegged at
   target depth by pulling from a *global* ready-read queue that all compute workers push into.
   Removes per-thread fragmentation and decouples "keep the device full" from "do PQ/expansion".
   This is the finer-grained, multi-thread, always-full design; expected to hit 728k with fewer
   total cores and to matter most on high-latency CXL-SSD (a poller sustains depth a stalling
   worker cannot).
2. **Cut CPU per read** so each core issues more: SIMD PQ distance, drop the robin_set (reuse the
   pool's visited bit), batched poll, in-flight dup-read coalescing.
3. **Cut reads per query** (raises the BW-bound QPS ceiling itself): PQ-only traversal + deferred
   full-precision re-rank of a small finalist set.

## 4. Reading for the paper
- The bubbles are **intra-query and structural** (pointer-chasing → QD≈1); the only way to fill them
  is **cross-query** work. That is exactly continuous batching.
- On a fast block SSD + 1 core, the CPU saturates before the device, so the single-core win is modest
  (+7% over static). The continuous-batching advantage grows when (i) the media miss penalty is high
  (flash-backed CXL-SSD: ~140× vs a device-cache hit) so keeping QD high matters much more, and
  (ii) many cores/hosts share one device, where the static-batch group barrier wastes shared QD.
- Open items for the CXL-SSD / one-copy-disaggregated story:
  - cross-query **fetch coalescing/dedup** (one NAND read serves many lanes hitting the same node)
    — not yet implemented; expected to matter under overlapping hot subgraphs.
  - **pollution-aware / accuracy-gated** admission against a bounded shared device cache.
  - **cross-host** lane/QD budgeting.

## 5. Repro
```bash
cd third_party/PipeANN/build
# build index (once): tests/build_disk_index float base.bin idx_1m 64 128 32 64 112 l2 pq
# groundtruth (once): tests/utils/compute_groundtruth float l2 base.bin query.bin 100 gt100.bin null null
# bubbles (pipe): tests/search_disk_index float idx_1m 1 32 query.bin gt100.bin 10 l2 pq 2 0 100 150 200
# static batch:   tests/search_disk_index float idx_1m 1 4  query.bin gt100.bin 10 l2 pq 3 0 150
# continuous:     PIPEANN_LANES=8 tests/search_disk_index float idx_1m 1 4 query.bin gt100.bin 10 l2 pq 4 0 150
# device QD: iostat -x -y 1 N /dev/nvme0n1  (aqu-sz column)
```
