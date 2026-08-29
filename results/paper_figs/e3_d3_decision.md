# E3 / D3 decision — 2026-08-29

**FAIL.** Shared LFU hot set does not help at iso-beam on this boot.

| Row | beam | hotset | QPS | recall@10 | hit% |
|-----|------|--------|-----:|----------:|-----:|
| A | 300 | 0 | **14.29** | 0.914 | 16.8 |
| B | 300 | 64 MiB | **2.41** | 0.914 | 92.1 |
| C | 150 | 0 | **37.45** | 0.847 | 14.1 |
| D | 150 | 64 MiB | **5.29** | 0.847 | 93.8 |

B is 6× slower than A; D is 7× slower than C. Hit rate jumps because `pin_vec_ids` forces residency, then the 1 GiB window thrashes (`evicts` 318k / 73k). Neither pair meets the 0.92 floor at beam 150; at beam 300 both are 0.914.

**Paper:** D3 stays “entry pin + soft-pin”. Do not claim a shared hot set beats a deeper beam or helps at fixed beam.
