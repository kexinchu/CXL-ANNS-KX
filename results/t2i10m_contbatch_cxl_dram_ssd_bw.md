# Continuous batching on CXL-DRAM ← CXL-SSD (real ANNS)

Date: 2026-08-22. Text2Image-10M, oneshot FP, L=300, k=10, nq=50, seed=42,
recall@10 ≈ 0.93. CXL-SSD = `/dev/vmem0` (`vmem_sw` → `nvme1n1`). CXL-DRAM
window = 1 GiB **anon+mbind node1** (`/dev/dax0.0` absent this boot).

Goal: use **real graph fetches** to raise CXL-DRAM hit% and QPS, and see how
close the **useful** SSD→DRAM fill gets to 12 GB/s.

## 1. What continuous batching changes (P0 → P3)

P3 is the serving-stack analog of PipeANN `cont`: each hop’s miss pages are
issued together (`PREFETCH_BATCH` + `PageCopyPool`), scored as they land, and
a few high-utility pages are installed into the 1 GiB window for later hops /
queries. Pool now **persists across queries** (no per-query thread spawn).

| Config | QPS | hit% | promote GB/s | evicts | notes |
|--------|----:|-----:|-------------:|-------:|-------|
| **P0** demand promote | 1.18 | 9.08 | 0.029 | 36k | QD≈1 memcpy faults |
| **P3** W=4 install_top=4 | **17.73** | 10.76 | **0.547** | 0 | +15× QPS, +19× useful fill |
| P3 W=32 | 14.56 | 10.76 | 0.449 | 0 | extra workers do not help |
| P3 install_all | 3.16 | 9.08 | 0.148 | 36k | window thrash |
| P3 install_all + 8 threads | 2.30 | 23.08 | 0.108 | 36k | lock + evict |
| **P3 + pagebin** W=4 | **21.33** | **41.69** | **0.565** | 0 | **best** |

Iso-recall throughout (0.930–0.932). `dist` comps stay ~256k.

## 2. Bottlenecks (why not 12 GB/s)

1. **Useful graph volume is ~0.55 GB/s, not 12.**  
   P3 pagebin: 1.32 GB promoted / 2.35 s = 0.565 GB/s. That is *all* the
   miss pages the walk actually needs at 21 QPS. 12 / 0.565 ≈ **21×** more
   bytes/s would require either ~21× QPS or fetching pages the walk never
   touches (forbidden: “不是为了打满而打满”).

2. **Selective install is mandatory.** `install_all` writes every fetched page
   into the 1 GiB window → 36k evicts, hit% collapses to P0, QPS ÷5. Extra
   bandwidth here is **pollution**, not hit%.

3. **Sharing the window across many host threads loses.** 8 search threads +
   one `DramWindow` mutex: latency 56 ms → 3.3 s. Hit% can rise (23%) while
   QPS dies. Continuous batching belongs **inside the hop pipeline**, not as
   N uncoordinated search threads.

4. **W>4 is not the NAND wall.** After `PREFETCH_BATCH`, W=4 already covers
   a hop’s miss set. W=32 adds sync overhead, QPS drops 17.7→14.6.

5. **No hardware CXL-DRAM↔SSD 12 GB/s path this boot.** Window is host DRAM
   via `mbind`. NAND ceiling of the software SSD is still the backing NVMe
   (~7 GB/s), and we are **not** media-bound — we are **graph-demand-bound**.

## 3. What *did* raise hit% (the actual objective)

- **Hop-level continuous issue (P3 vs P0):** QPS 1.18→17.7. Hit% only +1.7 pp
  on packed layout because each hop still installs just 4 pages.
- **Pagebin (pack neighbors into the same 4 KB page):** hit **10.8% → 41.7%**,
  QPS **17.7→21.3**, promote still ~0.56 GB/s. Same bytes, **more of them hit
  the window** — this is the “use bandwidth to buy hit%” win.
- Cross-query reuse (`--flush-window` off) + pin-entry: evicts=0 on the
  winning configs.

## 4. Code (this round)

- `serving/vmem_prefetch.hpp` — `VMEM_IOC_PREFETCH_BATCH` before hop memcpy
- `search_beam`: persistent `PageCopyPool`, `--install-all-fetched`,
  `--threads`, `--no-vmem-prefetch`, print `cxl_ssd_to_dram_promote_GBps`
- Default P3 still `install_top=4` (do **not** install-all)

## 5. Recommended serving line

```bash
# pagebin layout + P3 selective install (best hit% and QPS)
serving/search_beam --policy P3 --pipe-w 4 --install-top 4 --budget $((64<<20)) \
  --dram-backend numa --oneshot-fp --beam 300 --max-q 50 \
  --vmem-dev /dev/vmem0 --vmem-offset $CXAN_SSD_OFFSET ...
```

Do **not** chase 12 GB/s by widening install or thread count on this stack.
Next levers that still serve hit%/QPS: larger nq to see window reuse; keep
pagebin; if Montage/DAX returns, re-measure the same P3 line on real
CXL-DRAM (promote memcpy destination changes, policy does not).
