# P3v2 smart-install + install_top/W sweep

**Change:** install pages ranked by utility = likely-next-expand boost + inverse distance + page cover; cap `--install-top` pages/hop (not top-T ids blindly).

## Setup

L=300, k=10, nq=100, seed=42, pin-entry, no flush, DAX 1 GiB, oneshot FP, budget=64 MiB, `ram_size_gib=28`.

## Sweep (all `dist=643533`, recall=0.929)

| Config | QPS | hit% | mean ms |
|--------|-----|------|---------|
| P0 | 1.45 | 10.47 | 689 |
| **P3 W=4 install_top=4** | **12.65** | **22.41** | **79** |
| P3 W=4 install_top=8 | 5.93 | 19.42 | 169 |
| P3 W=4 install_top=12 | 3.76 | 16.99 | 266 |
| P3 W=4 install_top=16 | 2.98 | 14.99 | 336 |
| P3 W=8 install_top=4 | 10.87 | 22.41 | 92 |
| P3 W=8 install_top=8 | 5.78 | 19.36 | 173 |

Prior pool+pipeline (distance top-8 ids): ~4.23 QPS, ~16.2% hit.

```
CSV,P0,67108864,1073741824,100,300,0,1.45,688.819,751.469,966.771,1043.745,0.9290,10.47,105216,899476
CSV,P3,67108864,1073741824,100,300,0,12.65,79.049,78.257,108.129,122.261,0.9290,22.41,43903,151982
CSV,P3,67108864,1073741824,100,300,0,5.93,168.589,154.546,292.504,324.931,0.9290,19.42,64099,265906
```

## Takeaways

1. **Lean + smart beats fat install:** `install_top=4` raises both QPS and hit% vs 8/16 (extra installs inflate `ssd_misses` and evict useful frames).
2. **W=4 ≥ W=8** here; more workers add little once pool is warm.
3. **Defaults:** `--policy P3 --budget $((64<<20)) --pipe-w 4 --install-top 4`.
