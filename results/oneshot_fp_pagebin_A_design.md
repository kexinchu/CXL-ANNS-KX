# Packed pagebin reorder (A) + optional page-group B

## What shipped
- **A (`tools/pack_layout_pagebin.cpp`)**: dense-packed layout; star-BFS neighbor-block ID remap so unused neighbors of a visit land in contiguous IDs. No 4K-per-vector pad.
- **B (`--page-group`)**: install/soft-pin ±1 page of a 2-page group. Default **off** (`--no-page-group`).
- Placement supports non-packed strides; search remaps GT via `--id-map`.

## 25M Wiki-Cohere (768-d, 3072B/vec, ~1.33 vec/page), P3 L=300 nq=100

| Config | QPS | hit% | ssd_misses |
|---|---:|---:|---:|
| packed, no B | 12.2 | 22.4 | 151982 |
| **pagebin A only** | **13.4** | **29.0** | **143468** |
| pagebin A+B | 11.2 | 27.0 | 172345 |

Iso-recall `dist=643533`, recall@10=0.929. B hurts QPS; keep A only.
