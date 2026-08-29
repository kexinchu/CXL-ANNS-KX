# PipeANN on CXL-SSD (memory-semantic, /dev/vmem0) — bring-up + first results

Date: 2026-08-20. Dataset: Text2Image-10M (10M×200-d, MIPS), PipeANN disk index
(`idx_t2i`, 13.65 GB, 3 nodes/4 KB page, mem_L=0).

## 1. Device state this session (changed vs prior sessions)
- Machine rebooted onto kernel **6.18.0-rc5**. Old `/dev/vmem0` + `vmem_sw` were gone.
- A CXL Type-2 accelerator (`0000:3b:00.0`, Intel `8086:0ddb`, driver `cxl_type2_accel`)
  exposes a 32 GiB RAM region → **`/dev/dax6.0`** (devdax, target_node 8).
- **`/dev/dax6.0` is NOT usable as storage**: raw mmap reads back all `0xFF` and writes do
  not persist across mappings (device-private memory; not host-usable without the
  accelerator driver mediating). So the plain devdax path cannot hold the index.
- Fix (as expected): **restore the mem2nvme `vmem_sw` software CXL-SSD**.

## 2. Restoring vmem_sw for the current kernel
Prebuilt `vmem_sw.ko` had vermagic `6.14.0-1009-intel` (won't load on 6.18-rc5). Rebuilt the
self-contained module (`VMEM_SW_ONLY=1`, only `vmem_sw_{main,io,cache}.c`, no NVMe fork):
```bash
cd /root/chukexin/mem2nvme
make -C /lib/modules/6.18.0-rc5/build M=$PWD/host VMEM_SW_ONLY=1 modules   # -> vermagic 6.18.0-rc5
insmod host/vmem_sw.ko nvme_dev=/dev/nvme1n1 target_bdf=0000:d8:00.0 \
  expected_ssd_size_bytes=1920383410176 ram_size_gib=32 cache_size_gib=4 stripe_size_mib=2
```
Backing = KIOXIA CD8P 1.92 TB (serial `7EU0A01P0XK1`, now `nvme1n1` @ `d8:00.0`, unmounted).
Result: **`/dev/vmem0` online**, logical 1.954 TB = 1.92 TB NAND + 32 GiB RAM tier, 4 GiB
DRAM cache, `io_errors=0`. mmap verified persistent on both RAM tier (@0) and NAND tier
(@64 GiB).

## 3. Memory-semantic reader for PipeANN
PipeANN only had SPDK/io_uring/libaio readers (all block-device, O_DIRECT + async). Added an
**env-gated mmap mode** to `LinuxAlignedFileReader` (`PIPEANN_MMAP_DEV`, `PIPEANN_MMAP_POPULATE`):
`open()` maps the dax/vmem device and (optionally) stages the index bytes in; every query-time
read becomes a `memcpy` from the mapping (fault-driven load/store), `poll()` is a no-op
(completion is synchronous). This is the on-story way to run the pipeline over a CXL-SSD.
Files: `include/{aligned_file_reader.h,linux_aligned_file_reader.h}`,
`src/utils/linux_aligned_file_reader.cpp`. Default (no env) = unchanged io_uring path.

```bash
# stage once, then reuse:
PIPEANN_MMAP_DEV=/dev/vmem0 PIPEANN_MMAP_POPULATE=1 search_disk_index float idx_t2i 8 8 query.bin null 10 mips beam 2 0 150
PIPEANN_MMAP_DEV=/dev/vmem0 PIPEANN_MMAP_POPULATE=0 ...   # subsequent runs
```

## 4. First results (1000 queries, L=150, T2I-10M)
| mode     | NVMe (io_uring) QPS | CXL-SSD (vmem0, mmap) QPS |
|----------|---------------------|--------------------------|
| pipe t8  | 3469                | 29.7  (avg lat 269 ms)   |
| pipe t16 | 5624                | 406                      |
| pipe t32 | 7590                | 3849                     |
| cont t16 | 7394                | 148                      |
| cont t32 | 7668                | 303                      |

Same ~167 IOs/query on both paths (identical traversal), so differences are purely the
storage-access model.

### Findings
1. **Memory-semantic access collapses to QD≈1 per thread → latency fully exposed.** Cold
   pipe t8: 29.7 QPS vs 3469 on NVMe (~117× slower); each ~1.6 ms NAND fault is serialized
   because a load/store gives no async in-flight parallelism (unlike io_uring's deep queue).
   The only lever left with synchronous mmap is *more threads* (more concurrent faults).
2. **`cont` is WORSE than `pipe` on CXL-SSD** (cont t32=303 vs pipe t32=3849), the opposite
   of NVMe (cont ≥ pipe). Reason: `cont` issues more *speculative* reads to keep a deep
   device queue; with async io_uring those overlap for free, but with synchronous load/store
   they just add exposed latency. This is direct motivation for the paper's design:
   **QD-scheduling only pays off on CXL-SSD if memory-semantic reads are made asynchronous
   (software prefetch / non-blocking fault issue), not plain blocking `memcpy`.**

### Caveat (measurement)
Absolute CXL QPS here is confounded by **cache warming**: the 4 GiB DRAM cache filled to
capacity across the sequential runs (`cache_used=4.0 GiB` at the end), so later runs read
more from cache. The super-linear pipe scaling (29→406→3849) is mostly cache warming, not
thread scaling. A clean sweep needs per-run cache reset (reload module or add a cache-flush
knob) and a fixed cold/warm protocol. Qualitative findings (1) and (2) hold regardless.

## 5. Async memory-semantic prefetch (the core design) — DONE

**Thesis (from finding 2):** on CXL-SSD, a QD-targeted scheduler only wins if the
memory-semantic reads are made *asynchronous*. A plain `memcpy` from the mapping is a
synchronous fault (one NAND read at a time, QD≈1), so the deep software queue never
reaches the device. We made the frontier reads asynchronous end to end.

### 5.1 Driver: batched concurrent NAND prefetch (`vmem_sw`)
The software backend previously served every miss with **one blocking `submit_bio_wait`
per page under a global `cache_lock`** — zero device parallelism. Added:
- `vmem_sw_page_io_batch()` — submits up to `VMEM_BATCH_MAX=64` single-page bios
  **concurrently** (shared completion) and waits once, so *N* NAND reads are in flight
  at the same time.
- `vmem_sw_prefetch_batch()` + ioctl `VMEM_IOC_PREFETCH_BATCH` — takes a list of logical
  page offsets (the QD-frontier), collects the misses, issues them as one concurrent
  batch, then inserts them into the DRAM cache. Cache hits only refresh LRU.
  Files: `mem2nvme/host/{shell.h,vmem_sw_internal.h,vmem_sw_io.c,vmem_sw_cache.c,vmem_sw_main.c}`.

**Driver microbenchmark** (`mem2nvme/tools/vmem_batch_prefetch_probe.c`, batch vs
one-at-a-time `PREFETCH_LOGICAL`), per-page NAND latency:

| depth D | 1 | 2 | 4 | 8 | 16 | 32 |
|---------|----|----|----|----|----|----|
| µs/page | 25.1 | 11.2 | 6.1 | 3.3 | 3.4 | ~3 |

MLP saturates around **D=8 (~7.6× lower per-page latency)** — exactly the queue depth the
QD scheduler wants to hold.

### 5.2 Pipeline: prefetch the frontier, then compute (`cont_search`)
`cont_search` now, each refill round, **reserves the round's D slots, calls
`reader->prefetch_frontier(offsets, n)` to fault them all concurrently, then does the
memcpy reads** (which now hit the hot DRAM cache). New reader hook
`AlignedFileReader::prefetch_frontier()` → `LinuxAlignedFileReader` issues the batch
ioctl in mmap mode (no-op on io_uring/plain devdax). Gated by `PIPEANN_VMEM_PREFETCH`
for A/B. Files: `include/{aligned_file_reader.h,linux_aligned_file_reader.h}`,
`src/utils/linux_aligned_file_reader.cpp`, `src/search/coro_search.cpp`.

### 5.3 Result — clean COLD A/B (reload → stage → single run)
Same fresh 4 GiB cache, same staging, T2I-10M, `query_1k`, `cont` D=32, 16 lanes, 16 threads:

| cont (cold) | QPS | vs baseline |
|-------------|-----|-------------|
| baseline (blocking `memcpy`, QD≈1) | **23.7** | 1.0× |
| async QD-frontier prefetch (D concurrent NAND) | **270.4** | **11.4×** |

The end-to-end gain (11.4×) exceeds the per-page probe (~6–8×) because the pipeline also
overlaps 16 lanes' frontiers, not just one page's latency. **Recall is unchanged by
construction:** `prefetch_frontier` only warms pages into the device cache; the
`queue_read`→`explore_one` traversal (candidate pool, distance compares, visited set) is
byte-identical to baseline — prefetch changes *when* a page is resident, never *which*
pages are visited.

This closes finding 2: with async memory-semantic reads, QD scheduling turns CXL-SSD's
synchronous fault latency (the earlier `cont`-worse-than-`pipe` anomaly) into an
order-of-magnitude throughput win — the paper's core design contribution, demonstrated on
real flash-backed CXL memory.

### 5.4 Measurement notes / remaining
- **Warm state hides the effect.** Once the 1k-query working set fits the 4 GiB cache
  (after ~2 warm passes) there are almost no NAND faults, so prefetch ≈ baseline (~1.6k
  QPS, +4%). The design matters exactly when the working set exceeds device DRAM — the
  regime CXL-SSD targets. Cold A/B (reload per run) is the honest protocol.
- Still open: sweep D∈{8,16,64} end-to-end; monitor backing `nvme1n1` to separate cache
  hits from NAND faults; relax the global `cache_lock` during batch I/O so *multiple*
  threads' batches overlap (today the batch is concurrent within a call, but calls
  serialize on the mutex).

## 6. Thread-scaling table (CXL-SSD `vmem0`) — the NVMe analog

Same sweep as the NVMe pipe-vs-cont table, but on the memory-semantic CXL-SSD path.
Protocol: **cold per point** (reload → stage → run `query_1k`, T2I-10M, L=150, D=32,
`cont` lanes=16, prefetch on); backing `nvme1n1` monitored, staging excluded by time
cutoff. `inflight` = measured device queue on `nvme1n1` (100 ms avg); `agg_QD` = software
target depth (`threads×D`); `cores` from `/proc/stat` over 86 logical CPUs.

| threads | mode | QPS | dev IOPS | dev BW | dev inflight | agg_QD (sw) | CPU |
|--------:|------|----:|---------:|-------:|-------------:|------------:|----:|
| 8  | pipe | 26.1  | 2 772  | 0.01 GB/s | 1 | — | ~0 |
| 8  | **cont** | **336.8** | **32 946** | 0.13 GB/s | 3 | 254  | ~2 |
| 16 | pipe | 25.2  | 2 671  | 0.01 GB/s | 1 | — | ~1 |
| 16 | **cont** | **289.3** | **31 034** | 0.12 GB/s | 3 | 506  | ~1 |
| 24 | pipe | 23.9  | 2 524  | 0.01 GB/s | 1 | — | ~1 |
| 24 | **cont** | **211.9** | **24 044** | 0.09 GB/s | 3 | 754  | ~1 |
| 32 | pipe | 28.6  | 3 038  | 0.01 GB/s | 1 | — | ~2 |
| 32 | **cont** | **212.6** | **23 927** | 0.09 GB/s | 2 | 1000 | ~1 |

### Contrast with NVMe (the earlier table) and what it means
1. **`pipe` is flat at ~24–29 QPS on CXL-SSD regardless of threads** (device inflight = 1,
   IOPS ~2.7k, CPU idle). On NVMe `pipe` scaled 3467→7590 QPS. Cause: a memory-semantic
   load/store fault is synchronous, *and* the software backend serializes every miss on a
   single global `cache_lock` (`submit_bio_wait` under the mutex) → QD collapses to 1.
2. **`cont`+async-prefetch is 8–13× `pipe`** (336.8 vs 26.1 at t8; device IOPS 12×). The
   batched ioctl issues D concurrent NAND reads in one lock hold, so the deep software
   queue finally reaches the device — the core design win, now visible at the device level.
3. **Neither scales with threads on CXL-SSD** (cont even drops 337→213 as contention
   rises), although software `agg_QD` climbs 254→1000. The **global `cache_lock` is the
   wall**: threads' batches serialize on it, so measured device inflight stays ~1–3 even
   though each batch bursts to 32. On NVMe both modes scaled and saturated ~5.4 GB/s; here
   BW is only ~0.1 GB/s — the CXL memory-semantic path is **latency/lock-bound, not
   bandwidth-bound.**

**Takeaway for the design:** async memory-semantic reads are necessary (they buy the 8–13×)
but not sufficient for scale — the next lever is removing the driver's global serialization
(sharded/lockless cache, or submitting the batch bios without holding the mutex during I/O)
so device queue depth and multi-thread scaling can actually materialize.

*Caveats:* cold 1k-query points (each staged fresh), so absolute QPS is the cold-miss floor;
device `inflight` is a 100 ms average so it understates the in-batch burst depth (up to 32);
`cont` search windows are short (3–4 s), a handful of monitor samples each.

## 7. Re-bring-up 2026-08-21
After reboot: **no `/dev/vmem0`, no `/dev/dax*`**, FPGA `15:00.0` unbound (BAR disabled).
Restored with `tools/restore_vmem_sw.sh` → `/dev/vmem0` software backend, backing
`/dev/nvme1n1` @ `d8:00.0` (CD8P, sn `7EU0A01P0XK1`), `ram_size=28 GiB`, 4 GiB cache.
mmap persist OK on RAM tier and NAND @64 GiB. Index restaged (13.65 GB). **Not a
devdax mount** — memory-semantic path is still `mmap(/dev/vmem0)`.

Cold (reload, `cache_used=0`, `query_1k`, L=150, T2I-10M, 8 threads):

| mode | QPS | backing IOPS | inflight | note |
|------|----:|-------------:|---------:|------|
| pipe mmap | 19.0 | 3.0k | 0.7 | QD≈1, 28% device-idle |
| **cont + prefetch** | **403.4** | **64.5k** | **10.3** | **21× pipe**, agg_QD=255 |
