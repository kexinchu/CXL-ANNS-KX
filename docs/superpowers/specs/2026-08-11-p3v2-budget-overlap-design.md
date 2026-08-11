# P3v2: hop page budget + I/O–score overlap

Date: 2026-08-11  
Status: implemented

## Goal

QPS **> P0**, **iso-recall** (same `dist`), hit% between P0 and full-window P3.

## Final design

1. **Current hop**: parallel CXL-SSD→host page fetch (`--pipe-w` workers), **score from host** (critical path; no DAX round-trip before MIPS).
2. **Budget**: `--budget` caps pages fetched via the parallel path; remainder uses demand `score_id`.
3. **Window**: after scoring, `install_full_page` only for the closest `lookahead_k` (default 8) ids’ pages → CXL-DRAM (hit% > P0, install cost bounded).
4. **Safety**: workers never write DAX. Expand order unchanged (= P0).
5. **P3 `on_query_begin`**: pin only; budget owned by hop fetch.

## Acceptance (L=300, nq=100, seed=42, DAX, oneshot FP)

| Policy | QPS | recall | hit% | dist |
|--------|-----|--------|------|------|
| P0 | 1.49 | 0.929 | 10.47 | 643533 |
| P3 pool+pipeline budget=64MiB W=4 | **4.23** | 0.929 | 16.20 | 643533 |
| P3 pool+pipeline budget=16MiB W=4 | 2.09 | 0.929 | 11.59 | 643533 |

(Earlier host-score without pool was ~2.30 QPS at 64 MiB.)

