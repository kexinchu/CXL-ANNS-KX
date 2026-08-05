# Data replacement + CXL-DRAM index residency

**LAION-25M embeddings：** 无公开整包可下；改用 Big-ANN 公开的 **Wikipedia-Cohere 35M**，裁 **前 25M × 768-d float32**（约 71.5 GiB）替换原 LAION≈1M。

| 项 | 原 1M | 新 25M |
|----|-------|--------|
| 路径 | `data/laion1m`, `diskann_data_1m` | `data/wiki_cohere_25m`, `diskann_data_25m` |
| 来源 | lance LAION CLIP 1024-d | [wiki-cohere-35M](https://comp21storage.z5.web.core.windows.net/wiki-cohere-35M/) |
| 度量 | L2 | MIPS（Cohere） |

**下载：** `data/wiki_cohere_25m/download_parallel.sh`（8 路 Range）。完成后：`tools/wait_build_wiki25m_cxl.sh`。

**CXL-DRAM 写入（DiskANN 路径）：**
1. `build_memory_index` → `mem_R32` + `mem_R32.data`
2. `numactl --membind=1 search_memory_index`（进程 RSS 落在 node1）
3. 或 `tools/load_index_cxl_dram` 显式 `mbind` 拷贝 base+index

**已验证（旧 1M，路径打通）：** mem index 建完；`membind=1` search ≈ **2359 QPS**（L=64,K=10）。25M 下载完成后自动建索引并同样绑 CXL-DRAM。
