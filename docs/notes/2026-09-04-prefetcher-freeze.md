# Frozen hide-copy prefetcher (2026-09-04)

Locked after the sequential keep/drop sweep on 1M test-500. This is the
issue policy, not “100 MiB cache + 2 GiB window ≈ corpus”.

Does not replace T2I-10M claim rows 50.25 / 49.25 / 85.8.

## Core (do not regress)

1. Score only after the page is in the host `DramWindow` (`from_win ≈ 100`).
2. Issue only committed-expand **miss** pages (`page_use = 100`). No spec-beam,
   no `--score-page`, no `--stripe-fill`.
3. `--no-sync-hop`: pick `expand_batch` nodes, `READ_BATCH` their miss pages,
   then `wait_covering` / drain. Default width **e4 a1**.
4. Bounce buffer → window memcpy. `--direct-install` is a no-op (hung).
5. Host `--graph-file` is required on DiskANN hide so `N(u)` is not an SSD
   mmap fault. Extract `N(u)` from the **logical** packed image (id order),
   not `graph_R32.bin` and not a slot-order extract of a remapped file.
6. **Page layout (query rebuild):** build-query `--dump-expands` (disjoint from
   timed queries) → `--mode cooccur --trace-pairs` (2-in-4K from expand
   co-issue; leftover strongest-neighbor) → `--mode extent --map-in` (each
   expand’s still-free pages become one run; traces packed first). Search
   must pass `--id-slot-map`. Full-graph C(R,2) cooccur is infeasible at 10M
   (~450 new pairs/node).
7. Search contract: G0 10k nav → entry → oneshot-fp L=400 k=10 T=1.

## Dropped

| Opt | Why |
|-----|-----|
| `--direct-install` | hang after `roles` |
| `--stripe-fill` | 103.85 → 99.09 QPS on 1M test-500 |
| e8 a2 / e4 a2 | worse than e4 a1 |
| `--score-cache` / 80 MiB trace pin | nq=500 WS ≫ 100 MiB |

## 1M evidence (pollute baseline, extent @950, test-500)

| row | QPS | mean ms | recall@10 |
|-----|----:|--------:|----------:|
| e1 a1 bounce | 99.43 | 10.06 | 0.0884 |
| **e4 a1 bounce (kept)** | **103.17** | **9.69** | 0.0884 |
| host oracle | 266.69 | 3.75 | 0.0882 |

1M recall vs 10M GT is not a quality claim.

## Runtime flags

```
--diskann-layout --nav-graph … --graph-file … \
--expand-batch 4 --issue-ahead 1 --no-sync-hop --no-score-page \
--no-score-cache --no-direct-install --no-stripe-fill --no-hide-warm-entry \
--threads 1 --shared-window --policy P3 --oneshot-fp --beam 400 --k 10
```

CLI defaults for DiskANN (no `--nbr-bundle`) now match e4 a1 and `--no-score-page`.

10M recipe: `tools/run_hide_10m_t1.sh` (`LAYOUT=seq|extent`). Extent image at
1100 GiB; sequential stays at 800 GiB. On seed-42 nq=100, extent QPS is flat
vs sequential (24.75 vs 24.99); keep both. See
`docs/notes/2026-09-04-prefetcher-10m-t1.md`.
