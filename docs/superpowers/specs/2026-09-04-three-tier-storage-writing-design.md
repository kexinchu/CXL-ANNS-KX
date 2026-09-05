# FlashANNS Three-Tier Storage Writing Design

**Status:** Approved for paper revision on 2026-09-04  
**Scope:** Paper wording, tables, and system figures; Figure 3 and Figure 4 are protected.  
**Supersedes:** Any manuscript statement that places the complete graph in host DRAM, treats the host cache as CXL-DRAM, or equates `vmem_sw` with a physical SSD.

## Goal

Make every section describe the same three-tier data path while preserving the manuscript's section order and its two central mechanisms: Wise Prefetching and Continuous Batching.

## Authoritative Storage Contract

From the CPU outward, the active system has three storage tiers:

1. **Small host-memory cache.** It holds query state and a bounded set of recently admitted complete records. Host software runs graph search, asynchronous prefetch, and continuous batching here.
2. **4 GiB CXL-DRAM page cache.** It is the CXL-side cache managed through the `vmem_sw` memory-semantic interface. Host load/store accesses this tier. A cache miss requires a Flash fill.
3. **Dual-SSD Flash capacity.** `/dev/nvme1n1` at `0000:d8:00.0` and `/dev/nvme2n1` at `0000:d9:00.0` hold the authoritative graph-and-vector corpus. The backing devices are striped at 2 MiB granularity.

The complete graph adjacency and full-precision vectors share fixed-size DiskANN-style records on Flash. The host cache and CXL-DRAM cache hold only subsets of those records. Query, beam, visited-set, scheduler, and cache metadata are host-local control state and are not a second corpus copy.

`vmem_sw` is the software address-space and cache-management layer. It is not itself a physical SSD and must not be named as a fourth storage tier. The manuscript may describe the 4 GiB cache as the CXL-DRAM cache role, while the Implementation section must disclose exactly how the measured prototype realizes that role.

## Access and Fill Semantics

The CPU first checks the host cache. A host-cache miss may issue a load/store to a CXL-DRAM-resident page. A CXL-DRAM miss requires a 4 KiB fill from Flash. The 4 KiB page is the residency, prefetch, admission, and deduplication unit; 2 MiB is only the dual-SSD striping unit.

There are two distinct movements:

- **Flash fill:** Flash to the 4 GiB CXL-DRAM page cache.
- **Host admission:** CXL-DRAM to the small host-memory cache.

Wise Prefetching initiates Flash fills before a dependent score needs the page. Continuous Batching interleaves ready work and outstanding fills across queries. Neither mechanism changes graph candidates, distances, visited state, beam width, or the recall contract.

## Correctness and Hide Contract

A score may consume a host-cache hit or a CXL-DRAM-cache hit. A score-hide failure occurs only when the scoring path triggers or waits for a Flash fill. Therefore the paper must replace the ambiguous invariant "all scores come from the window" with:

> No scoring access triggers or waits for a Flash fill.

The minimum evidence is a three-level accounting:

- `score_from_host_cache`;
- `score_from_cxl_dram`;
- `score_triggered_flash_fill`, which must be zero for score hide;
- critical-path Flash wait, Flash bytes/read requests, and promotion bytes.

A temporary bounce buffer populated only after a synchronous Flash read is not a host-cache hit and does not satisfy score hide.

## Oracle Contract

The full-corpus CXL-DRAM Oracle is a separate evaluation configuration in which all graph-and-vector records are resident in sufficiently large CXL-DRAM and timed Flash traffic is zero. It is not the normal 4 GiB cache tier and must not be drawn as a simultaneous second copy in the active three-tier system.

The legacy 1 GiB host-window result (85.8 QPS) is an in-window host-memory upper bound, not the full-corpus CXL-DRAM Oracle. Legacy 50.25/49.25 QPS results used a host-resident graph and cannot support the new end-to-end placement claim.

## Paper-Wide Vocabulary

Use these terms consistently:

| Term | Exact meaning |
|---|---|
| host cache | small software-managed L1 data cache in CPU memory |
| CXL-DRAM cache | fixed 4 GiB CXL-side page cache |
| Flash backing / Flash tier | dual NVMe capacity holding complete graph-and-vector records |
| `vmem_sw` | memory-semantic mapping and cache manager spanning CXL-DRAM and Flash |
| resident | always qualify as host-resident or CXL-resident |
| fill | 4 KiB movement from Flash into CXL-DRAM |
| admission | movement/copy from CXL-DRAM into the host cache |

Avoid unqualified `window`, `/dev/vmem0 = CXL-SSD`, `graph/PQ live in host DRAM`, `graph-in-host`, and `score only from the window`.

## Section and Figure Constraints

- Preserve the order and top-level structure of Abstract, Introduction, Background, Motivation, Design, Implementation, Evaluation, Related Work, Discussion, and Conclusion.
- Keep Design centered on Wise Prefetching and Continuous Batching; placement is a shared invariant, not a third contribution.
- Redraw Figure 1(c) and the Design mechanism figures to show host cache, 4 GiB CXL-DRAM cache, and dual-SSD Flash.
- Do not modify Figure 3 assets or data.
- Do not modify Figure 4 in this revision.
- Remove Appendix inclusion as previously approved.
- Do not promote legacy measurements into claims for the revised hierarchy. Use placeholders or explicitly name them as legacy until the three-tier experiments are rerun.

## Acceptance Criteria

1. No manuscript sentence places the complete graph or corpus in host DRAM for FlashANNS.
2. Every architecture description contains the same three tiers and direction of movement.
3. `vmem_sw`, CXL-DRAM, and the dual Flash devices are not conflated.
4. Score hide is defined by absence of score-triggered Flash fills, not by an ambiguous window hit.
5. Evaluation distinguishes host-cache hits, CXL-DRAM hits, and Flash fills and aggregates both backing devices.
6. Oracle is visibly an alternate all-CXL-DRAM configuration.
7. Figure 3 and Figure 4 source/artifact hashes remain unchanged.
8. The LaTeX build completes without undefined references introduced by this revision.
