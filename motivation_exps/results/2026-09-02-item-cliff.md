# One-item cliff: CXL-DRAM vs CXL-SSD miss (2026-09-02)

Probe: `motivation_exps/probe_item_cliff`
Log: `results/probe_item_cliff_20260902b.log`

CXL-DRAM: Montage `/dev/dax0.0`, copy item, `clflushopt` + load.
CXL-SSD: `/dev/vmem0` `d9:00.0`, thrash 5 GiB, then load. No write / no restage.

| corpus | item | DRAM $p_{50}$ | SSD $p_{50}$ | ratio |
|---|---|---|---|---|
| LAION-10M (first 10M of 25M, d=512) | 2048 B | 666 ns | 88.7 µs | 133× |
| T2I-10M (staged CXAN1, d=200) | 800 B | 281 ns | 88.8 µs | 316× |

There is no standalone LAION-10M file. SSD-side LAION is a 2048 B load on the same NAND image (T2I is staged). An unused 800 GiB region was ~9 µs (device cache) and was discarded.
