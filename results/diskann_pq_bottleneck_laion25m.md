# DiskANN CPU+SSD+PQ Bottleneck Sweep (LAION-25M)

Date: 2026-08-17  
Raw results: `/mnt/disk0/chukexin_motivation/results/diskann_pq_bottleneck_20260814_230459`  
Bandwidth saturation results: `/mnt/disk0/chukexin_motivation/results/diskann_pq_bw_saturation_20260817`

## Setup

- Dataset: LAION image embeddings, 25,000,000 vectors, 512 dimensions, normalized float32.
- DiskANN index: CPU + local NVMe SSD, PQ navigation enabled.
- Build/search config: `R=32`, `Lbuild=64`, `L=64`, `K=10`, `W=4`, `num_nodes_to_cache=10000`.
- Search DRAM budget: 8 GiB; in-memory PQ compression: 332 bytes/vector.
- Query set: 50,000 vectors for the original sweep; 500,000 repeated queries for the high-concurrency bandwidth-saturation rerun.
- Machine reports 86 logical CPUs to DiskANN; OS reports 172 CPUs.

## Important Note on NVMe `%util`

`iostat %util` is **device busy time**, not bandwidth saturation. It means the block device had at least one request in service during that sampling interval. A stream of small random reads can drive `%util` close to 100% even when GB/s is far below the sequential bandwidth limit. Therefore, the table below reports both:

- `NVMe busy%`: active-device time from `iostat %util`.
- `avg/peak read GB/s`: actual read bandwidth from `iostat rkB/s`.

A read-only fio baseline on the same DiskANN index file gives two useful ceilings. The 4KB random-read ceiling is the right denominator for DiskANN's small random SSD reads; the 1MB sequential-read ceiling is shown only as the device's sequential maximum.

| Baseline | BW | NVMe util | Notes |
|---|---:|---:|---|
| 4KB random read, `io_uring`, `iodepth=128`, `numjobs=16` | 7.01 GB/s avg | 99.63% | matches DiskANN small random reads |
| sequential read, `io_uring`, `bs=1M`, `iodepth=32`, `numjobs=1` | 7.47 GB/s avg | 99.71% | device sequential ceiling |

## Results

| Threads | QPS | mean latency us | p99.9 us | mean IOs/q | avg read GB/s | peak read GB/s | NVMe BW util avg/peak | NVMe busy% | job CPU% |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 221.99 | 4441.68 | 11474 | 78.20 | 0.10 | 2.11 | 1.4% / 30.1% | 56.7% | 41% |
| 2 | 432.73 | 4559.78 | 11564 | 78.20 | 0.13 | 0.17 | 1.9% / 2.4% | 80.9% | 81% |
| 4 | 810.91 | 4869.92 | 11897 | 78.20 | 0.24 | 0.31 | 3.4% / 4.4% | 95.4% | 144% |
| 8 | 1568.87 | 5034.74 | 12455 | 78.20 | 0.44 | 0.54 | 6.3% / 7.7% | 94.0% | 262% |
| 16 | 3215.21 | 4913.80 | 15063 | 78.20 | 0.83 | 1.12 | 11.8% / 16.0% | 87.5% | 444% |
| 32 | 5759.94 | 5491.77 | 17539 | 78.20 | 1.36 | 2.11 | 19.4% / 30.1% | 80.5% | 766% |
| 64 | 13410.76 | 4711.67 | 16488 | 78.20 | 4.12 | 4.37 | 58.8% / 62.3% | 98.4% | 1669% |
| 86 | 16166.80 | 5253.30 | 15163 | 78.20 | 4.89 | 5.21 | 69.8% / 74.3% | 96.8% | 2078% |
| 128 | 19648.21 | 6413.00 | 12949 | 78.20 | 6.02 | 6.19 | 85.9% / 88.3% | 98.2% | 2747% |
| 172 | 21323.82 | 7962.95 | 14779 | 78.20 | 6.52 | 6.69 | 93.0% / 95.4% | 98.2% | 2974% |
| 256 | 21848.26 | 11627.85 | 22450 | 78.20 | 6.52 | 6.85 | 93.0% / 97.7% | 95.7% | 2962% |
| 344 | 21808.22 | 15680.00 | 31629 | 78.20 | 6.52 | 6.85 | 93.0% / 97.7% | 96.0% | 3082% |

## Bandwidth Saturation Validation

The original 50k-query runs are short, so their whole-run average read bandwidth is diluted by process startup, PQ loading, cache setup, and result writing. To test bandwidth saturation directly, I repeated the 50k query set to 500k queries and reran the high-concurrency points while collecting 1-second `iostat` samples. `avg read GB/s` below is the average over active samples with read bandwidth above 0.1 GB/s; `peak read GB/s` is the maximum 1-second sample.

| Test | QPS | mean latency us | p99.9 us | mean IOs/q | avg read GB/s | peak read GB/s | NVMe busy% | job CPU% | wall time |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| DiskANN original search, `T=172,L=64,W=4`, 500k queries | 21323.82 | 7962.95 | 14779 | 78.20 | 6.52 | 6.69 | 98.2% | 2974% | 0:32.89 |
| DiskANN oversubscribed, `T=256,L=64,W=4`, 500k queries | 21848.26 | 11627.85 | 22450 | 78.20 | 6.52 | 6.85 | 95.7% | 2962% | 0:34.94 |
| DiskANN oversubscribed, `T=344,L=64,W=4`, 500k queries | 21808.22 | 15680.00 | 31629 | 78.20 | 6.52 | 6.85 | 96.0% | 3082% | 0:37.58 |

The high-concurrency DiskANN run drives the NVMe to about 6.52 GB/s active average and 6.85 GB/s peak read bandwidth. Relative to the 7.01 GB/s fio 4KB random-read ceiling, this is about 93% average and 98% peak bandwidth utilization. This is the run that should be cited when the claim is specifically "NVMe read bandwidth is nearly saturated."

## Interpretation

DiskANN with PQ navigation is **not CPU-compute bound** in this run, but the limiting factor should be described more precisely as **random-I/O service rate / I/O overlap**, not simple sequential bandwidth saturation.

Evidence:

1. `mean IOs/q` is fixed at 78.2 across thread counts, so the workload keeps issuing the same number of random SSD reads per query.
2. `NVMe busy%` reaches about 95% by 4-8 threads, but read bandwidth is only 0.24-0.44 GB/s, far below the 7.01 GB/s 4KB random-read baseline. This means low-thread saturation is not bandwidth saturation; it is small-random-I/O busy time.
3. With a longer query run and enough search threads, DiskANN drives the SSD read path to 6.52 GB/s active average and 6.85 GB/s peak, about 93%/98% of the 4KB random-read bandwidth ceiling.
4. After `T=172`, adding more threads (`T=256/344`) no longer improves QPS or average read bandwidth, but tail latency grows sharply. This is the saturation knee.
5. CPU is not saturated at the saturation knee: the job consumes about 30 CPU cores on a 172-CPU machine while the SSD read bandwidth is near its random-read ceiling.
6. QPS increases mostly by adding I/O concurrency and hiding SSD latency, then flattens as random-I/O queueing and tail latency rise.

Conclusion: for this LAION-25M CPU+SSD+PQ configuration, DiskANN is not limited by CPU distance computation. It is also not purely limited by sequential NVMe bandwidth. The first-order bottleneck is the random SSD I/O path: per-query I/O count, device busy time under small reads, and the amount of I/O concurrency needed to hide SSD latency.
