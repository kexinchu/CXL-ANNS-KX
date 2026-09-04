# FlashANNS citation audit

Date: 2026-09-03; revalidated and extended 2026-09-04  
Policy: each retained key was checked against the linked publisher/proceedings,
DBLP record, or (only where no formal version exists) the arXiv abstract.  The
ordered author list and publication version in `paper/refs.bib` were checked at
the same time.  `Verified` also means that the abstract/method description
supports the bounded claim below; it does not license broader comparisons.

| Key | Canonical title | Publication version | Authoritative record | Manuscript location | Supported claim | Status |
|---|---|---|---|---|---|---|
| `hnsw` | Efficient and Robust Approximate Nearest Neighbor Search Using Hierarchical Navigable Small World Graphs | IEEE TPAMI 42(4), 2020 | [DBLP](https://dblp.org/rec/journals/pami/MalkovY20) | Background; Related Work | Hierarchical navigable graph search | Verified |
| `nsg` | Fast Approximate Nearest Neighbor Search With The Navigating Spreading-out Graph | PVLDB 12(5), 2019 | [PVLDB](http://www.vldb.org/pvldb/vol12/p461-fu.pdf) | Background; Related Work | Monotonic/proximity graph construction and search | Verified |
| `diskann` | DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node | NeurIPS 2019 | [NeurIPS](https://proceedings.neurips.cc/paper/2019/hash/09853c7fb1d3f8ee67a61b6bf4a7f8e6-Abstract.html) | Background; Related Work | Vamana/DiskANN and SSD-resident billion-scale search | Verified |
| `freshdiskann` | FreshDiskANN: A Fast and Accurate Graph-Based ANN Index for Streaming Similarity Search | arXiv:2105.09613, 2021; no formal version found | [arXiv](https://arxiv.org/abs/2105.09613) | Background; Related Work | Streaming updates to a disk graph index | Verified |
| `spann` | SPANN: Highly-efficient Billion-scale Approximate Nearest Neighborhood Search | NeurIPS 2021 | [DBLP](https://dblp.org/rec/conf/nips/ChenZWLLLYW21) | Related Work | Memory/disk hybrid inverted-index search | Verified |
| `spfresh` | SPFresh: Incremental In-Place Update for Billion-Scale Vector Search | SOSP 2023 | [DBLP](https://dblp.org/rec/conf/sosp/XuLLXCZLYYYCY23) | Related Work | Incremental in-place update for billion-scale vector indexes | Verified |
| `filtereddiskann` | Filtered-DiskANN: Graph Algorithms for Approximate Nearest Neighbor Search with Filters | WWW 2023 | [DBLP](https://dblp.org/rec/conf/www/GollapudiKSKBRL23) | Related Work | Filter-aware graph ANNS | Verified |
| `diskannlib` | The DiskANN Library: Graph-Based Indices for Fast, Fresh and Filtered Vector Search | IEEE Data Engineering Bulletin 48(3), 2024 | [DBLP](https://dblp.org/rec/journals/debu/KrishnaswamyMS24) | Related Work | DiskANN family and implementation taxonomy | Verified |
| `pipeann` | Achieving Low-Latency Graph-Based Vector Search via Aligning Best-First Search Algorithm with SSD | OSDI 2025 | [USENIX](https://www.usenix.org/conference/osdi25/presentation/guo) | Background; Design; Related Work | Intra-query and cross-query asynchronous SSD overlap | Verified |
| `starling` | Starling: An I/O-Efficient Disk-Resident Graph Index Framework for High-Dimensional Vector Similarity Search on Data Segment | PACM MOD 2(1), 2024 | [DBLP](https://dblp.org/rec/journals/pacmmod/WangXYWPKGXGX24) | Related Work | I/O-efficient disk-resident graph layout/search | Verified |
| `faiss` | Billion-Scale Similarity Search with GPUs | IEEE TBD 7(3), 2021 | [DBLP](https://dblp.org/rec/journals/tbd/JohnsonDJ21) | Background; Related Work | GPU-accelerated billion-scale vector search | Verified |
| `scann` | Accelerating Large-Scale Inference with Anisotropic Vector Quantization | ICML 2020 | [PMLR](https://proceedings.mlr.press/v119/guo20h.html) | Background; Related Work | Quantization-based candidate ranking | Verified |
| `pq` | Product Quantization for Nearest Neighbor Search | IEEE TPAMI 33(1), 2011 | [DBLP](https://dblp.org/rec/journals/pami/JegouDS11) | Background | Product-quantized distance approximation | Verified |
| `opq` | Optimized Product Quantization for Approximate Nearest Neighbor Search | CVPR 2013 | [DBLP](https://dblp.org/rec/conf/cvpr/GeHK013) | Background | Learned rotation improves product quantization | Verified |
| `annbench` | ANN-Benchmarks: A Benchmarking Tool for Approximate Nearest Neighbor Algorithms | SISAP 2017 | [DBLP](https://dblp.org/rec/conf/sisap/AumullerBF17) | Evaluation; Related Work | Recall--performance evaluation methodology | Verified |
| `bigannchallenge` | Results of the NeurIPS'21 Challenge on Billion-Scale Approximate Nearest Neighbor Search | PMLR 176, 2022 | [PMLR](https://proceedings.mlr.press/v176/simhadri22a.html) | Evaluation; Related Work | Billion-scale ANNS workload/metric practice | Verified |
| `hm-ann` | HM-ANN: Efficient Billion-Point Nearest Neighbor Search on Heterogeneous Memory | NeurIPS 2020 | [NeurIPS](https://proceedings.neurips.cc/paper/2020/hash/788d986905533aba051261497ecffcbb-Abstract.html) | Background; Related Work | ANNS across heterogeneous memory tiers | Verified |
| `milvus` | Milvus: A Purpose-Built Vector Data Management System | SIGMOD 2021 | [DBLP](https://dblp.org/rec/conf/sigmod/WangYGJXLWGLXYY21) | Related Work | Distributed vector database architecture | Verified |
| `manu` | Manu: A Cloud Native Vector Database Management System | PVLDB 15(12), 2022 | [DBLP](https://dblp.org/rec/journals/pvldb/GuoLXYYLCXLLCQW22) | Related Work | Cloud-native vector database dataflow | Verified |
| `vbase` | VBASE: Unifying Online Vector Similarity Search and Relational Queries via Relaxed Monotonicity | OSDI 2023 | [USENIX](https://www.usenix.org/conference/osdi23/presentation/zhang-qianxi) | Related Work | Integrated vector/relational query execution | Verified |
| `distributedann` | DISTRIBUTEDANN: Efficient Scaling of a Single DISKANN Graph Across Thousands of Computers | arXiv:2509.06046, 2025; no formal version found | [arXiv](https://arxiv.org/abs/2509.06046) | Related Work | Sharding one logical DiskANN graph across machines | Verified |
| `secondtier` | Characterizing the Dilemma of Performance and Index Size in Billion-Scale Vector Search and Breaking It with Second-Tier Memory | arXiv:2405.03267, 2024; no formal version found | [arXiv](https://arxiv.org/abs/2405.03267) | Background; Related Work | Second-tier memory reduces host-DRAM index pressure | Verified |
| `fatrq` | FaTRQ: Tiered Residual Quantization for LLM Vector Search in Far-Memory-Aware ANNS Systems | arXiv:2601.09985, 2026; no formal version found | [arXiv](https://arxiv.org/abs/2601.09985) | Background; Related Work | Far-memory-aware residual quantization | Verified |
| `infiniswap` | Efficient Memory Disaggregation with Infiniswap | NSDI 2017 | [USENIX](https://www.usenix.org/conference/nsdi17/technical-sessions/presentation/gu) | Related Work | Remote memory exposed through page swapping | Verified |
| `aifm` | AIFM: High-Performance, Application-Integrated Far Memory | OSDI 2020 | [USENIX](https://www.usenix.org/conference/osdi20/presentation/ruan) | Related Work | Object-granular application-integrated far memory | Verified |
| `leap` | Effectively Prefetching Remote Memory with Leap | USENIX ATC 2020 | [USENIX](https://www.usenix.org/conference/atc20/presentation/al-maruf) | Design; Related Work | Runtime remote-memory prefetching | Verified |
| `canvas` | Canvas: Isolated and Adaptive Swapping for Multi-Applications on Remote Memory | NSDI 2023 | [USENIX](https://www.usenix.org/conference/nsdi23/presentation/wang-chenxi) | Related Work | Isolated/adaptive remote-memory swapping | Verified |
| `nomad` | Nomad: Non-Exclusive Memory Tiering via Transactional Page Migration | OSDI 2024 | [DBLP](https://dblp.org/rec/conf/osdi/XiangLD0RY024) | Motivation; Related Work | Page migration across nonexclusive tiers | Verified |
| `tpp` | TPP: Transparent Page Placement for CXL-Enabled Tiered-Memory | ASPLOS 2023 | [DBLP](https://dblp.org/rec/conf/asplos/MarufWDWABPCKC23) | Motivation; Related Work | OS page placement for CXL tiering | Verified |
| `memtierscxl` | Managing Memory Tiers with CXL in Virtualized Environments | OSDI 2024 | [USENIX](https://www.usenix.org/conference/osdi24/presentation/zhong-yuhong) | Motivation; Related Work | VM-aware CXL memory-tier management | Verified |
| `canfarmemory` | Can Far Memory Improve Job Throughput? | EuroSys 2020 | [DBLP](https://dblp.org/rec/conf/eurosys/AmaroBLOAPRS20) | Motivation; Related Work | Throughput/latency tradeoff for far memory | Verified |
| `pond` | Pond: CXL-Based Memory Pooling Systems for Cloud Platforms | ASPLOS 2023 | [DBLP](https://dblp.org/rec/conf/asplos/LiBHEZNSRLAHFB23) | Related Work | CXL memory pooling with latency/capacity management | Verified |
| `rethinkingruntimes` | Rethinking Software Runtimes for Disaggregated Memory | ASPLOS 2021 | [DBLP](https://dblp.org/rec/conf/asplos/CalciuIPKMMK21) | Related Work | Runtime-managed fine-grained disaggregated memory | Verified |
| `cxlanns` | CXL-ANNS: Software-Hardware Collaborative Memory Disaggregation and Computation for Billion-Scale Approximate Nearest Neighbor Search | USENIX ATC 2023 | [USENIX](https://www.usenix.org/conference/atc23/presentation/jang) | Background; Related Work | Full-corpus CXL-memory ANNS and relationship-aware caching | Verified |
| `cosmos` | Cosmos: A CXL-Based Full In-Memory System for Approximate Nearest Neighbor Search | IEEE CAL 24(1), 2025 | [IEEE DOI](https://doi.org/10.1109/LCA.2025.3570235) | Background; Related Work | Device-side execution for full-memory CXL ANNS | Verified |
| `cxlssd` | Overcoming the Memory Wall with CXL-Enabled SSDs | USENIX ATC 2023 | [USENIX](https://www.usenix.org/conference/atc23/presentation/yang-shao-peng) | Background; Related Work | SSD exposed through CXL memory semantics | Verified |
| `skybyte` | SkyByte: Architecting an Efficient Memory-Semantic CXL-based SSD with OS and Hardware Co-design | HPCA 2025 | [Author PDF](https://platformxlab.github.io/papers/skybyte-hpca25.pdf) | Background; Related Work | Memory-semantic CXL-SSD OS/hardware co-design | Verified |
| `fromblocktobyte` | From Block to Byte: Transforming PCIe Solid-State Devices With Compute Express Link Memory Protocol and Instruction Annotation | IEEE Micro 45(6), 2025 | [IEEE](https://ieeexplore.ieee.org/document/11072206/) | Background; Related Work | Byte-addressable CXL protocol and annotated access over SSD | Verified |
| `hellobytes` | Hello Bytes, Bye Blocks: PCIe Storage Meets Compute Express Link for Memory Expansion (CXL-SSD) | HotStorage 2022 | [DBLP](https://dblp.org/rec/conf/hotstorage/Jung22) | Related Work | CXL-SSD design motivation | Verified |
| `cacheinhand` | Cache in Hand: Expander-Driven CXL Prefetcher for Next Generation CXL-SSD | HotStorage 2023 | [DBLP](https://dblp.org/rec/conf/hotstorage/KwonLJ23) | Design; Related Work | Expander-side generic CXL-SSD prefetching | Verified |
| `directcxl` | Direct Access, High-Performance Memory Disaggregation with DirectCXL | USENIX ATC 2022 | [USENIX](https://www.usenix.org/conference/atc22/presentation/gouk) | Related Work | Direct CXL memory disaggregation | Verified |
| `cmmh` | Performance Characterizations and Usage Guidelines of Samsung CMM-H | IEEE Micro 45(5), 2025 | [IEEE DOI](https://doi.org/10.1109/MM.2025.3591577) | Background; Related Work | Empirical behavior of a NAND-backed hybrid CXL module | Verified |
| `revisitingcmmh` | Revisiting Memory Hierarchies with CMM-H: Use Device-side Caching to Integrate DRAM and SSD for a Hybrid CXL Memory | HotStorage 2025 | [ACM DOI](https://doi.org/10.1145/3736548.3737828) | Related Work | Device-side caching over DRAM and SSD in hybrid CXL memory | Verified |
| `hymcache` | HyMCache: A KV Cache Framework for Multi-Turn LLM Serving with CXL-Hybrid Memory | arXiv:2607.18141, 2026; no formal version found | [arXiv](https://arxiv.org/abs/2607.18141) | Related Work | Workload-specific prefetch and cache management for SSD-backed CXL hybrid memory | Verified |
| `phnsw` | P-HNSW: Crash-Consistent HNSW for Vector Databases on Persistent Memory | Applied Sciences 15(19), 2025 | [Publisher](https://www.mdpi.com/2076-3417/15/19/10554) | Related Work | Persistent-memory crash consistency for HNSW | Verified |
| `bauhaus` | Bauhaus: Restructuring Vector Database for LLM Retrieval on CXL-Based Tiered Memory | IEEE TC 75(4), 2026 | [Institutional record](https://pure.korea.ac.kr/en/publications/bauhaus-restructuring-vector-database-for-llm-retrieval-on-cxl-ba/) | Related Work | Vector-database restructuring for CXL tiered memory | Verified |
| `demystifycxl` | Demystifying CXL Memory with Genuine CXL-Ready Systems and Devices | MICRO 2023 | [DBLP](https://dblp.org/rec/conf/micro/SunYYKSHJALJ0AX23) | Background; Motivation | Measured CXL-memory device latency/bandwidth behavior | Verified |
| `neomem` | NeoMem: Hardware/Software Co-Design for CXL-Native Memory Tiering | MICRO 2024 | [DBLP](https://dblp.org/rec/conf/micro/0002CZW0XCQXZ024) | Motivation; Related Work | CXL-native hot-page monitoring and tiering | Verified |
| `memorypooling` | Memory Pooling With CXL | IEEE Micro 43(2), 2023 | [DBLP](https://dblp.org/rec/journals/micro/GoukKB0J23) | Related Work | CXL memory pooling architecture | Verified |
| `cxl31` | Compute Express Link Specification, Revision 3.1 | CXL Consortium, 2023 | [CXL Consortium](https://www.computeexpresslink.org/spec-landing-page-compute-express-link) | Background | CXL.mem and shared-memory terminology | Verified |
| `orca` | Orca: A Distributed Serving System for Transformer-Based Generative Models | OSDI 2022 | [USENIX](https://www.usenix.org/conference/osdi22/presentation/yu) | Design; Related Work | Iteration-level scheduling avoids fixed-batch barriers | Verified |

## Rejected or replaced records

- `cxlanns-tocs`: removed.  It duplicated the ATC paper and incorrectly named
  the 2024 journal extension as ACM TOCS; the actual venue is ACM Transactions
  on Storage.  The manuscript uses the peer-reviewed ATC version once.
- `cxl-anyssd`: removed because the previous entry contained guessed thesis
  metadata and no authoritative public record sufficient for a claim.
- `cxl30`: renamed `cxl31` to match the actual specification revision.
- `sigplan-empirical`: no longer cited after the Appendix was removed.
- Preprint duplicates of `spfresh`, `starling`, and `demystifycxl` were rejected
  in favor of the formal SOSP, PACM MOD, and MICRO publications.
- The arXiv versions of `cosmos` and `cmmh` were replaced by their formal
  IEEE CAL and IEEE Micro publications during the 2026-09-04 revalidation.
