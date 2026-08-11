# P3v2 smart-install + soft-pin

## Git

Pushed to `kexinchu/CXL-ANNS-KX` (`main`). Soft-pin follow-up on top of P3v2 pool commit.

## Soft-pin

Installed utility-ranked pages get a hop TTL and resist clock eviction (`soft_pin_bytes_cap` ≤256 MiB). `tick_soft_pins` is O(#soft-pins), not O(frames).

## Results (L=300, nq=100, seed=42, budget=64 MiB, W=4)

| Config | QPS | hit% | recall | notes |
|--------|-----|------|--------|-------|
| P0 | 1.44 | 10.47 | 0.929 | |
| **P3 install_top=4 + soft-pin** | **13.70** | **22.41** | **0.929** | evicts=0 |
| P3 install_top=8 + soft-pin | 6.38 | 19.41 | 0.929 | |
| Prior P3 install_top=4 (no soft-pin) | ~12.65 | 22.41 | 0.929 | |

`dist=643533` on all runs.

```
CSV,P0,67108864,1073741824,100,300,0,1.44,695.358,769.796,986.378,1076.602,0.9290,10.47,105216,899476
CSV,P3,67108864,1073741824,100,300,0,13.70,73.001,72.152,99.293,112.293,0.9290,22.41,43903,151982
CSV,P3,67108864,1073741824,100,300,0,6.38,156.809,143.665,278.534,341.396,0.9290,19.41,64016,265837
```

## Takeaways

1. At `install_top=4` the 1 GiB window never evicts — soft-pin cannot raise hit further; default remains lean install.
2. Fixing full-frame soft-pin tick restored/improved QPS (~13.7 vs ~12.7).
3. Next: **fetch_top** (fewer wasted SSD→host pages) while keeping iso-recall.
