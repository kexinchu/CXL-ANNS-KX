# DiskANN `--no_pq_nav` patch

Applies to Microsoft DiskANN `cpp_main` (tested on a local checkout under `/mnt/disk0/chukexin_motivation/DiskANN_cpp`).

## Behavior

- Adds `PQFlashIndex::set_no_pq_navigation(bool)` and CLI `--no_pq_nav` on `search_disk_index`.
- When enabled, neighbor ranking uses full-precision vectors from the disk index (coord cache or sector reads) instead of in-memory PQ codes.
- PQ tables may still load; they are unused for navigation.
- Navigation reads and later expand may double-read the same node (no cross-phase sector reuse) → IO is an upper bound.

## Apply

```bash
cd DiskANN_cpp
git apply diskann-no-pq-nav.patch
cmake --build build --target search_disk_index -j
```
