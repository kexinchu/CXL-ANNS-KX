# Text2Image-10M: prefetch vs prefetch+pagebin

Yandex big-ANN **text2image-10M** (IP, dim=200, float32 → **800B/vec ≈ 5.12 vec/4K page**).
DiskANN R=32 MIPS, oneshot FP, L=300, k=10, nq=100, seed=42, DAX 1GiB, `--no-page-group`.

| Config | warm QPS | hit% | ssd_misses | vs P0 |
|---|---:|---:|---:|---|
| packed P0 | 2.32 | 11.31 | 585037 | baseline |
| packed P3 (prefetch) | **13.53** | 16.75 | 158344 | QPS ×5.8, hit +5.4pp |
| **pagebin A + P3** | **17.36** | **42.05** | 138020 | QPS ×7.5 vs P0; +28% QPS / +25pp hit vs P3 |

Iso-recall `dist=521835`, recall@10=0.924.

Contrast Wiki-Cohere 25M (768-d, ~1.33 vec/page): A-only hit 22→29% (+7pp), QPS 12→13.4 (+10%).
On T2I, denser pages amplify reorder: hit **16.8→42%**, QPS **13.5→17.4**.
EOF
