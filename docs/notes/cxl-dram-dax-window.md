# CXL-DRAM window via DAX

Serving DramWindow defaults to **`mmap(/dev/dax0.0)`** (devdax), i.e. true Montage
Type-3 CXL memory — not anonymous pages + `mbind(node1)` (which can be local socket DRAM).

## Flags / env

| Item | Default |
|------|---------|
| `--dram-backend` | `dax` (`numa` = legacy fallback) |
| `--dax-dev` / `CXAN_DAX_DEV` | `/dev/dax0.0` |
| `--dax-offset` / `CXAN_DAX_OFFSET` | `0` (4 KiB aligned) |
| `--dram-bytes` / `CXAN_DRAM_BYTES` | ≤1 GiB |

`source tools/cap_enforce.sh` exports the DAX defaults.

## Placement story

- **CXL-SSD** (`/dev/vmem0`): one-copy ANNS image  
- **CXL-DRAM (DAX)**: software hot window / remote cache  
- **Host**: entry + selected queries only (≤32 MiB)
