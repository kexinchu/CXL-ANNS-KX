# P0 vs P2 (async distance-priority prefetch) @ recall≥0.90

Same setup as reuse A/B: L=300 fixed-point, 1 GiB CXL-DRAM, pin entry, **no flush**, nq=100, shuffle-seed=42, budget 16 MiB/query.

**P2:** `prefetch_by_cand_distance` enqueues `{vec_ptr,len}` to a worker thread (`cv` wake); search thread only sync-reads adjacency, then continues scoring.

## Results

| Policy | recall@10 | QPS | mean ms | p50 | p90 | p99 | CXL-DRAM hit% | prefetch_pages |
|--------|-----------|-----|---------|-----|-----|-----|---------------|----------------|
| **P0** no prefetch | **0.929** | **1.32** | 757.4 | 857.5 | 1111.7 | 1209.3 | **10.47%** | 0 |
| **P2** async dist-prefetch | **0.929** | **1.32** | 754.9 | 825.5 | 1088.1 | 1215.1 | **10.54%** | 10380 |

Raw: `results/oneshot_fp_r90_p0_p2_ab.csv`

## vs sync P1 (prior reuse run, same L/nq)

| | hit% | QPS | mean ms | note |
|--|------|-----|---------|------|
| P0 | 10.47% | 1.32–1.33 | ~750 | baseline |
| P1 sync | **37.44%** | 1.11 | 899 | blocks search; forces residency before score |
| P2 async | 10.54% | 1.32 | 755 | ≈P0; little overlap with demand path |

## Why P2 ≈ P0 here

Greedy expand does: enqueue nbr vectors of `cur` → **immediately** `get_vec(nb)` on the same neighbors. Worker almost always loses the race → demand promote still pays; async work is mostly late / redundant.  
P1’s high hit% came from **finishing** promote before score. Async helps only if prefetch is issued **earlier** than use (deeper lookahead across iterations, or multi-query pipeline)—not same-expand enqueue-then-touch.
