#!/usr/bin/env python3
"""Prepare LAION-1M embeddings + HNSW-derived kNN graph for motivation_exps."""
from __future__ import annotations

import argparse
import os
import struct
import time

import numpy as np


def l2_normalize(x: np.ndarray) -> np.ndarray:
    n = np.linalg.norm(x, axis=1, keepdims=True)
    return x / np.maximum(n, 1e-12)


def write_meta(path: str, n: int, dim: int, degree: int, entry: int) -> None:
    with open(path, "wb") as f:
        f.write(struct.pack("IIII", n, dim, degree, entry))


def build_knn_graph(xb: np.ndarray, degree: int, seed: int = 42):
    import faiss

    n, dim = xb.shape
    M = max(16, degree // 2)
    index = faiss.IndexHNSWFlat(dim, M, faiss.METRIC_L2)
    index.hnsw.efConstruction = 200
    index.hnsw.efSearch = max(64, degree * 2)
    index.verbose = True
    t0 = time.time()
    index.add(xb)
    print(f"HNSW add done in {time.time()-t0:.1f}s")

    graph = np.zeros((n, degree), dtype=np.uint32)
    bs = 4096
    t1 = time.time()
    for start in range(0, n, bs):
        end = min(n, start + bs)
        _, I = index.search(xb[start:end], degree + 1)
        for i in range(end - start):
            oid = start + i
            nbrs = []
            for nb in I[i]:
                nb = int(nb)
                if nb < 0 or nb == oid:
                    continue
                nbrs.append(nb)
                if len(nbrs) >= degree:
                    break
            while len(nbrs) < degree:
                nbrs.append(nbrs[-1] if nbrs else (oid + 1) % n)
            graph[oid, :] = np.asarray(nbrs[:degree], dtype=np.uint32)
        if end % 50000 < bs or end == n:
            print(f"  knn extract {end}/{n}")
    print(f"knn extract done in {time.time()-t1:.1f}s")
    # entry: medoid proxy = vector closest to mean (cheap: use index 0 after shuffle seed)
    rng = np.random.default_rng(seed)
    entry = int(rng.integers(0, n))
    return graph, entry


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--npy", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=0, help="0 = all rows")
    ap.add_argument("--degree", type=int, default=32)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    print("loading", args.npy)
    raw = np.load(args.npy, mmap_mode="r")
    print("raw", raw.shape, raw.dtype)
    n_all = raw.shape[0]
    n = n_all if args.n <= 0 else min(args.n, n_all)
    dim = int(raw.shape[1])
    rng = np.random.default_rng(args.seed)
    if n < n_all:
        idx = np.sort(rng.choice(n_all, size=n, replace=False))
        xb = np.asarray(raw[idx], dtype=np.float32)
    else:
        xb = np.asarray(raw[:n], dtype=np.float32)
    xb = l2_normalize(xb)
    print("using", xb.shape)

    vec_path = os.path.join(args.out, "vectors.bin")
    print("writing", vec_path, f"({xb.nbytes/1e9:.2f} GB)")
    xb.astype(np.float32).tofile(vec_path)

    print("building graph...")
    graph, entry = build_knn_graph(xb, args.degree, args.seed)
    graph.tofile(os.path.join(args.out, "graph.bin"))
    write_meta(os.path.join(args.out, "meta.bin"), n, dim, args.degree, entry)
    print(f"done n={n} dim={dim} degree={args.degree} entry={entry} -> {args.out}")


if __name__ == "__main__":
    main()
