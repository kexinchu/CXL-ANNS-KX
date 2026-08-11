# P3v2 smart-install + soft-pin

## Git

Pushed to `kexinchu/CXL-ANNS-KX` (`main`). Soft-pin follow-up on top of P3v2 pool commit.

## Soft-pin

Installed utility-ranked pages get a hop TTL and resist clock eviction (`soft_pin_bytes_cap` ≤256 MiB). `tick_soft_pins` is O(#soft-pins), not O(frames).

## fetch_top sweep (W=4, install_top=4, budget=64 MiB)

| fetch_top | QPS | hit% | notes |
|-----------|-----|------|-------|
| **0 (budget only)** | **13.64** | **22.41** | default |
| 8 | 1.56 | 10.30 | too much demand fallback |
| 16 | 2.14 | 11.70 | |
| 32 | 6.29 | 17.41 | |
| 64 | 13.77 | 22.41 | ≈ uncapped |

Truncating parallel fetch below a full hop’s pages loses the host-score win; keep `--fetch-top 0`.

## Results (L=300, nq=100, seed=42, budget=64 MiB, W=4)

| Config | QPS | hit% | recall | notes |
|--------|-----|------|--------|-------|
| P0 | 1.50 | 10.47 | 0.929 | |
| **P3 install_top=4 soft-pin fetch_top=0** | **~13.7** | **22.41** | **0.929** | evicts=0 |

`dist=643533` on all runs.

Recommended: `--policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4 --fetch-top 0`

