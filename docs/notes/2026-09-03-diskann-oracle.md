# DiskANN + 10k nav — 2026-09-03 numbers

**Does not replace** hide 50.25 / 49.25 or host-window oracle 85.8.
Old layout snapshot: `/root/chukexin/CXL-ANNS-KX_bak`.

## Layout

- Record: vec[200] + nnbrs + nbrs[32], `STRIDE=2048`, header `version=2`
- Host image: `/mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m.bin` (19.07 GiB)
- Nav: `results/paper_figs/nav_10k.bin` (seed=42, exact 10k scan → entry)
- 8 random ids verified vs pagebin vectors + `pagebin_graph.bin`

## `/dev/dax0.0` — slow-write / thermal check (2026-09-03 night)

Device is `mem0` @ `0000:15:00.0` (Intel `8086:0ddb` `cxl_type2_accel`), region `qos_class_mismatch=true`, `target_node=2` not a live NUMA node. Package temp stayed **41 °C** during fills. **Slowing the write does not fix persistence.**

| Test | Rate | In-process verify | New-process read |
|------|------|-------------------|------------------|
| 2 MiB windows + sfence/4K, no gap | ~10 GiB/s | pass 512 MiB | mostly `0xFF` (169/256 windows) |
| same + 20 ms/window | ~100 MiB/s | head clobber @ 240 MiB | — |
| 4 KiB + 200 µs, 16 MiB | ~16 MiB/s | — | 7/8 windows partial `0xFF` |
| CLWB / clflushopt per line | slow | — | all `0xFF` |

In-process “OK” is CPU cache. After munmap / new process, media is still `0xFF`. Not a temperature spike.

Tried `daxctl reconfigure-device --mode=system-ram --force --no-online`: mode switched, node 2 appeared with **0 MB**, onlining 256×128 MiB blocks is **EPERM**. Reverted to `devdax`. Did not leave the box in system-ram.

True CXL-DRAM Oracle is still blocked on this device. Host-file MAP_POPULATE numbers below are **not** that Oracle.

## Oracle (DiskANN oneshot-fp L=400 k=10 nq=20 seed=42)

| T | QPS | mean ms | recall@10 | nvme_read_B |
|---|----:|--------:|----------:|------------:|
| 1 | 118.92 | 8.407 | 0.945 | 0 |
| 4 | 297.51 | 10.903 | 0.945 | 0 |
| 8 | 413.73 | 13.375 | 0.945 | 0 |
| 16 | 530.78 | 18.436 | 0.945 | 0 |
| 32 | 488.28 | 21.196 | 0.945 | 0 |

Logs: `results/paper_figs/oracle_cxl_dram_T{1,4,8,16,32}_nq20.log`

## T=1 hide

CXL-SSD hide **not run**: `0000:d9:00.0` is bound to **unvme**; `/dev/nvme2n1` is gone; mmap at 800 GiB SIGBUS. Pagebin at 420 GiB still reads `CXAN1` from the 1.1 GiB vmem cache. Did not unbind unvme or touch 420/460 GiB.

Host-file P3 (1 GiB numa window, same DiskANN image + nav) after fixing `hide_issue` (fd<0 was treating `vmem_read_pages` return 0 as success and installing empty pages):

| T | QPS | mean ms | recall@10 |
|---|----:|--------:|----------:|
| 1 | 36.47 | 27.42 | 0.955 |

Log: `results/paper_figs/hide_diskann_T1_hostfile_nq20.log`

## 1M Oracle on `/dev/vmem0` page cache (2026-09-04)

`/dev/dax*` is gone (`cxl list` empty). CXL-SSD is up: dual `nvmex` d8+d9, `cache_limit=4 GiB`.

Staged `diskann_t2i_1m.bin` (n=1e6, STRIDE=2048, ~1.91 GiB) at vmem **900 GiB** (not 420/460). `cache_used=2034245632` before/after timed loops. `nvme_read_B=0`. Pagebin at 420 GiB still `CXAN1`.

This is **vmem host page cache**, not Montage CXL.mem. Recall@10 vs 10M GT is ~0.035 because the corpus is a 1M prefix.

| T | QPS | mean ms | recall@10 | nvme_read_B | cache_used |
|---|----:|--------:|----------:|------------:|-----------:|
| 1 | 275.59 | 3.63 | 0.035 | 0 | 2034245632 |
| 4 | 513.24 | 6.57 | 0.035 | 0 | 2034245632 |
| 8 | 823.91 | 6.72 | 0.035 | 0 | 2034245632 |
| 16 | 658.01 | 12.93 | 0.035 | 0 | 2034245632 |
| 32 | 629.63 | 12.09 | 0.035 | 0 | 2034245632 |

Logs: `results/paper_figs/oracle_1m_vmem_T{1,4,8,16,32}_nq20.log`

## 1M Hide, 400 MiB page cache, P3 + cont-batch (2026-09-04)

`/dev/dax*` still absent — 1M corpus stays on CXL-SSD at vmem **900 GiB**. Reloaded `vmem_sw` with `cache_size_mib=400` (`cache_limit=419430400`). Scoring window = host numa 2 GiB. `--policy P3 --expand-batch 8 --issue-ahead 2 --cont-batch T`. Warmup T=1 then timed rows. `evictions=0`; cache used ~170–182 MiB (20-query WS did not fill the 400 MiB cap). Pagebin at 420 GiB still `CXAN1`.

| T | QPS | mean ms | from_win | nvme_read_B | cache_used |
|---|----:|--------:|---------:|------------:|-----------:|
| 1 (cold warmup) | 66.48 | 15.04 | 99.97% | 156045312 | 172830720 |
| 1 | **86.62** | 11.54 | 99.97% | 6012928 | 178843648 |
| 4 | 83.69 | 44.28 | 99.98% | 1527808 | 180371456 |
| 8 | 69.21 | 104.07 | 99.98% | 483328 | 180854784 |
| 16 | 60.49 | 224.48 | 99.99% | 1290240 | 182145024 |
| 32 | 52.79 | 299.10 | 100.00% | 356352 | 182501376 |

vs 1M page-cache Oracle T=1 **275.59** → Hide T=1 is **31%** of that Oracle. Higher T on a shared 2 GiB window + nq=20 does not raise QPS (latency scales ~T).

Logs: `results/paper_figs/hide_1m_400mb_T{1,4,8,16,32}_nq20.log`
