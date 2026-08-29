# CXL-SSD ↔ CXL-DRAM bandwidth: is it 160 GB/s? Can host / device-side saturate it?

Date: 2026-08-21. Kernel 6.18.0-rc5. `/dev/vmem0` = `vmem_sw` software CXL-SSD
(backing `/dev/nvme1n1` CD8P @ `d8:00.0`).

## 0. What is actually on the machine this boot

| Path | Status | Notes |
|------|--------|--------|
| Montage CXL Type-3 DRAM (`1b00:c002`, historically 128 GiB @ `64:00.0`) | **Absent** | `64:00.0` is now Xilinx Alveo U250. No `mem0`, no `/dev/dax*`, NUMA is 2×~64 GiB host DRAM. cmdline **lacks** `efi=nosoftreserve`. |
| Software CXL-SSD `/dev/vmem0` | Online | 1.95 TB logical; 28 GiB RAM window (host pages); 4 GiB DRAM cache; NAND = NVMe. |
| FPGA `15:00.0` BAR0 32 GiB | Present, unused | Unbound, `LnkSta` **16 GT/s ×4 (downgraded)**. mmap reads **all `0xFF`**. Not a live DRAM. |
| Device-side DMA / HPS (`HPS_PAGE_FETCH`) | Not bound | Needs `mem2nvme`+`vmem` on programmed FPGA. Not running. |

**160 GB/s is a plan-document assumption** (device-side DRAM–SSD path in
`docs/plan/2026-07-31-cxl-ssd-anns-asplos-plan.md` §diagram / line 139), **not a
measured link on this box**. Historical Montage link was **32 GT/s ×8** (~32 GB/s
raw one-way). FPGA BAR is PCIe 4.0 ×4 (~8 GB/s raw one-way) and currently dead.

## 1. Host-initiated memcpy ceilings

| Test | Threads | GB/s |
|------|--------:|-----:|
| Host DRAM → host DRAM (memcpy 1 GiB) | 1 | 12.0 |
| same | 16 | 30.5 |
| same | 32–64 | **32.2–32.5** (plateau) |
| `vmem` RAM-tier (offset 0, host pages) → host | 16 | 30.5 |
| host → `vmem` RAM-tier | 16 | 31.4 |

Host memcpy saturates ~**32 GB/s** on this machine (copy = read+write). Even if a
real 160 GB/s device-side bus existed, **CPU `memcpy` from the host cannot reach
it**.

## 2. CXL-SSD NAND path (the real cold corpus)

Cold protocol: reload `vmem_sw` (`cache_used=0`), 512 MiB at logical **200+ GiB**
(past RAM window and the staged index).

| Who issues the fill | Time | Bandwidth | vs host-fault |
|---------------------|-----:|----------:|--------------:|
| **A. Host `memcpy` (page-fault, QD≈1)** | 11.31 s | **0.047 GB/s** | 1.0× |
| B. Same range again (4 GiB cache hit) | 0.042 s | 12.65 GB/s | (not NAND) |
| **C. Kernel `PREFETCH_BATCH` (D=64 concurrent bios)** | 0.389 s | **1.381 GB/s** | **29×** |
| C then memcpy (should hit) | 0.174 s | 3.08 GB/s | |
| C prefetch+memcpy total | 0.563 s | 0.95 GB/s | 20× |
| D. Host load-only (no dest store) | 9.60 s | 0.056 GB/s | ~1× |

Backing `nvme1n1` during the run: mean **0.07 GB/s**, IOPS ~18k, **inflight 1.3**,
device-idle 8%. fio ceiling of this class of NVMe is ~7 GB/s — we are **latency /
QD-bound, not media-bound**.

## 3. FPGA BAR (closest “device DRAM” window)

After `setpci COMMAND` Memory-enable: BAR0 maps, contents `ff ff ff …`.
Host sequential copy **1 GiB: 0.07 GB/s** (1 thread), **0.01–0.03 GB/s** with 8–16
threads (uncached MMIO / UR path). This is **not** a 160 GB/s (or even 8 GB/s)
DRAM.

## 4. Answers to the three questions

1. **Is CXL-SSD ↔ CXL-DRAM 160 GB/s?**  
   **Not on this machine, and not measurable today.** Montage CXL-DRAM is offline.
   The software SSD path’s NAND ceiling is the backing NVMe (~7 GB/s). 160 GB/s
   remains a *target* for a real device-internal / P2P path that is not exposed.

2. **Can host-side requests saturate that bandwidth?**  
   **No.** Host fault-driven fills run at **0.05 GB/s**. Host memcpy of already-resident
   data tops out at **~32 GB/s**. Both are far below 160. Host-issued I/O also
   cannot exceed the NVMe wall (~7 GB/s) even with a perfect queue.

3. **If prefetch/evict sits on a device-side compute unit, can we saturate 160?**  
   **The software proxy of “move issue off the CPU memcpy loop” helps a lot but
   does not approach 160.** Kernel `PREFETCH_BATCH` reaches **1.4 GB/s** (29× the
   host-fault path) and still only ~20% of the NVMe wall / **<1% of 160**. True
   device-side saturation would need (a) a live CXL-DRAM, (b) a programmed FPGA /
   HPS DMA that copies SSD→device-DRAM without host `memcpy`, and (c) a bus that
   is actually 160 GB/s. None of (a)(b)(c) are present this boot.

## 5. Implication for the paper claim

The useful measured fact: **who issues the fill matters**. Host load/store on
memory-semantic CXL-SSD collapses to QD≈1 (**0.05 GB/s**). Putting prefetch in
the device driver (concurrent NAND) is **~30×**. That supports the design
(“don’t let the host pointer-chase the miss”), but **do not write 160 GB/s as
an achieved number** until Montage + hardware CXL-SSD DMA are back and a
device-side copy is measured.
