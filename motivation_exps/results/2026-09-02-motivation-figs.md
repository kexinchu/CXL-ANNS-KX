# Motivation figures — test + draw (2026-09-02)

## What we did not rerun

`/dev/vmem0` is the hide disk: `backend=software` `bdf=0000:d9:00.0` `nvme=/dev/nvme2n1`, T2I staged, `cache_used=275025920`.
The July binary identity gate still expects `d8:00.0` / `/dev/nvme3n2`.
`populate_from_file` would write LAION-200k onto this address space and clobber T2I.
So **vmem-f2 / f4 / f5 / f8 were not rerun.** Fig.3–4 bars come from the locked July logs.

## Fresh read-only probe (this machine, no write)

Binary: `motivation_exps/probe_cliff_ro`
Log: `results/probe_cliff_ro_20260902.log`
800 × 800\,B loads at logical `0x2000000000` (SSD-tier 32\,MiB window).

| tag | ns/load | Δfaults | Δread_ios |
|---|---|---|---|
| RO-cold_ssd | 8105.7 | 800 | 800 |
| RO-warm_pte | 80.8 | 0 | 0 |
| RO-softcache_pte_cold | 786.6 | 800 | 0 |
| RO-after_thrash | 8049.7 | 800 | 800 |

QD: `max_in_flight=1`, mean gap = cold latency (serial).

Three tiers still exist; soft has faults and zero NAND. Absolute cold is ~8\,µs (device-cache-class 800\,B read on the current CD8P), not the July 4\,KB LAION 78--83\,µs. **Fig.3 keeps the July F8 bars** so caption and Fig.4 share one corpus.

## Locked July numbers used in the PDFs (gates)

| panel | numbers | gate | result |
|---|---|---|---|
| Fig.3 F8 | 82.65 / 3.79 / 2.15 µs | cold/warm ≥ 100× vs 0.6\,µs H0 | 82.7/0.61 ≈ 136×; same-probe vs 2.15\,µs is 38× (caption uses 0.6--2\,µs) |
| Fig.4a F2 | QPS 33.5 vs 9.3; NAND 9.9k vs 36.5k | ≥3× / ≥3× | 3.60× / 3.70× |
| Fig.4b F5 | QPS 25.9→5.0; hit 57.51 all; prec 82→29 | drop ≥4×; hit Δ<3pt; prec ↓ | 5.18×; Δhit=0; 82→29 |
| Fig.4c F4 | recall 0.2438 at L=32 and 64 | plateau | yes (hops=32) |
| Fig.5 | schematic + RO QD | max in-flight = 1 | yes |

## Files

- `paper/figs/mot-cliff.pdf`
- `paper/figs/mot-pathology.pdf`
- `paper/sections/fig_mot_bubbles.tex`
- `paper/figs/plot_motivation.py`
