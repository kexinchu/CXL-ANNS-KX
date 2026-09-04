# CXL-ANNS-KX

Research workspace for **graph ANNS on unified CXL–SSD memory** (ASPLOS-oriented).

## Old layout snapshot

The **pagebin + host-graph** stack (claim rows 50.25 / 85.8, `--graph-file`, 267 GiB expand-bundle, host `mbind` window) is frozen at:

**`/root/chukexin/CXL-ANNS-KX_bak`**

Do not restage or dual-stripe that tree’s 420 GiB pagebin. Reproduce old Oracle/hide only from `_bak`.

This directory (`CXL-ANNS-KX`) is the **DiskANN packed-entry** migration:

- One fixed-stride record per node: vector + neighbor IDs together (`STRIDE=2048` on a 32 GiB `/dev/dax0.0`)
- Full 10M graph+vectors packed as version-2 DiskANN (`diskann_t2i_10m.bin`). Staging onto `/dev/dax0.0` is blocked on this machine: dense writes read back as `0xFF`. Oracle numbers in `docs/notes/2026-09-03-diskann-oracle.md` are MAP_POPULATE of that host file.
- Host keeps only a **10k-node** navigation graph (`nav_10k.bin`)
- Old `--graph-file` / `--nbr-bundle` / `--oracle-window` path remains if the image header is version 1

See `docs/superpowers/specs/2026-09-03-cxl-dram-diskann-layout-design.md`.

**One-line thesis:** Full-precision vectors often must live on flash; CXL–SSD gives a unified VA + hot window, but the hard problem is **residency / stall control** (not “mmap is nice”). Naive page caches, blind graph prefetch, and blindly increasing `efSearch` amplify SSD misses; semantic pin / bounded promote / iso-recall search control are the intended remedies.

## Repository layout

```
docs/
  plan/          ASPLOS research plan + measured motivation tables
  literature/    CXL–ANNS related-work survey
  notes/         CXL memory + CXL–SSD structure notes
  results/       Canonical motivation findings summary
motivation_exps/ Userspace motivation harness on /dev/vmem0 (CXL–SSD software path)
patches/         DiskANN --no_pq_nav experimental patch
```

## Motivation experiments (`motivation_exps/`)

Targets gpu01-class setup: Montage CXL Type-3 + `/dev/vmem0` (vmem_sw → NVMe).

```bash
cd motivation_exps
make
./motivation vmem-identity
./motivation vmem-verify --dir <populated_laion_dir> --nq 8   # must ALL PASS
./motivation vmem-mot-ef  --dir <dir> --nq 16                 # efSearch 100..1600
./motivation vmem-mot-pin --dir <dir> --nq 16 --cache-mb 64   # pin vs nopin
./motivation vmem-mot-me  --dir <dir> --nq 16                 # M-E1/3/4/5
./motivation vmem-mot12   --dir <dir> --nq 16                 # earlier MOT12 suite
```

Key logs under `motivation_exps/results/`. Narrative summary: `docs/results/MOTIVATION_FINDINGS.md`.

**Not in this repo:** multi-hundred-GB LAION/DiskANN index blobs (keep on local `/mnt/disk0`).

## DiskANN no-PQ navigation patch

Official SSD DiskANN cannot turn off in-memory PQ. Experimental flag `--no_pq_nav` ranks unseen neighbors with disk full-precision reads (same sector layout / aio beam / node cache).

```bash
cd DiskANN_cpp
git apply /path/to/patches/diskann-no-pq-nav/diskann-no-pq-nav.patch
# rebuild search_disk_index, then:
./build/apps/search_disk_index ... --no_pq_nav
```

## Hardware / identity assumptions

See `motivation_exps/include/vmem_cxl.hpp` (`require_cxl_vmem_identity`): `/dev/vmem0` software backend, expected NVMe BDF, Montage `mem0`/`region0`. Adjust constants for your machine.

## Status

Motivation / problem framing and measurements are in progress. System design (semantic residency + stall-aware search) is specified in `docs/plan/` and not fully implemented as a production runtime yet.
