# C3: per-query I/O concurrency (Option A)

Reproduce:

```bash
# Restage extent image if vmem@1100GiB != diskann_t2i_10m_extent.bin
bash tools/stage_10m_extent_to_vmem.sh
NQ=500 bash tools/run_c3_io_concurrency.sh
OPENBLAS_NUM_THREADS=1 python3 tools/plot_c3_io_concurrency.py \
  results/motivation/c3_io_concurrency/raw/<stamp>
```

## Metrics

- `QD_eff(T)` = mean `aqu-sz` on the fio NVMe (`nvme3n1`) over active 0.1 s samples.
- Active sample: `rkB/s > 1e4` (10 MB/s). The plan’s `1e5` cutoff left T=1 with zero samples on this 4 GiB vmem cache.
- `QDsat` = mean `aqu-sz` of fio 4K randread, io_uring, qd=128, 8 jobs, 20 s, same device.

## Measured run

`raw/20260907_215508/` — T2I-10M PQ-64, beam 400, seed 42, nq=500, 512 MiB host window (this host cannot MAP_SHARED 2 GiB).

| T | recall@10 | QPS | QD_eff | peak aqu-sz |
|--:|----------:|----:|-------:|------------:|
| 1 | 0.920 | 81.8 | 0.98 | 1.20 |
| 2 | 0.920 | 157.3 | 1.81 | 2.14 |
| 4 | 0.920 | 252.7 | 2.85 | 3.38 |
| 8 | 0.920 | 336.2 | 3.79 | 4.82 |
| 16 | 0.920 | 288.4 | 3.35 | 4.41 |

`QDsat = 974`, `QDsat_gbs = 7.54`. `QD_eff(1)/QDsat = 0.001`.

Option A: **yes** — T=1 is far below `QDsat`; QD rises through T=8; T=16 is a knee (plotted, not reshaped). Iso-recall holds (`|Δ| = 0`).

Limitation: `QD_eff` is device `aqu-sz`, not NAND-channel occupancy. The 4 GiB vmem cache absorbs most page fills (NVMe bytes much smaller than issued pages times 4 KiB), so NVMe QD stays single-digit even while software issues about 349 pages per query.
