# YFCC-10M Download Record

**Date:** 2026-09-03  
**Local directory:** `/mnt/disk0/chukexin_motivation/data/yfcc10m`

The files below come from the YFCC dataset entry used by the official Big ANN
Benchmarks harness.  This is the unfiltered workload selected for FlashANNS's
ordinary recall@10 evaluation; filtered-query metadata was intentionally not
downloaded.

| File | Bytes | Header (`uint32`) | SHA-256 |
|---|---:|---|---|
| `base.10M.u8bin` | 1,920,000,008 | `10000000, 192` | `589030afcbcc44a50ff798cec935f0980815a02eb7075b224d4f7c12febe96bf` |
| `query.public.100K.u8bin` | 19,200,008 | `100000, 192` | `77a937d67baf7523a7d9089a6e49c0ac8c6a3e7eb5499d759afeacf0a843df82` |
| `unfiltered.GT.public.ibin` | 80,000,008 | `100000, 100` | `52899b776a4485d67e41c9edf35dd2fb776bb4c1af5568db95d5000aaa673e0f` |

Source URLs:

- <https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/yfcc100M/base.10M.u8bin>
- <https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/yfcc100M/query.public.100K.u8bin>
- <https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/yfcc100M/unfiltered.GT.public.ibin>

Format metadata is defined by the Big ANN Benchmarks YFCC dataset class:
192-dimensional `uint8` vectors, Euclidean/L2 distance, and CC BY 4.0 license.

## Boundary for the next experiment step

The raw dataset is ready, but it has not been converted, indexed, or staged to
CXL storage.  The current T2I path expects `float32`, whereas YFCC is natively
`uint8`; choose and document the runtime datatype/conversion policy before
building the graph.  Freeze a seeded 10k-query subset from the 100k public
queries and retain its IDs and hash in the final experiment manifest.
