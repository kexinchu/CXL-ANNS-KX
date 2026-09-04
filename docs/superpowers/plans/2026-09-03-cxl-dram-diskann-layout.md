# CXL-DRAM DiskANN-layout — 对照现码的改动 Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** 在**不拆掉 50.25 复现路径**的前提下，加一条 DiskANN 定长条目 + CXL-DRAM 整库 + host 10k 导引图的运行时；先交出 T=1 CXL-DRAM Oracle。

**Architecture:** 现码是「图在 host 文件、向量在 pagebin、可选 267 GiB expand-bundle」。新路径是「一条 `id*STRIDE` 记录里同时有 vec 和 nbrs；整库 mmap `/dev/dax0.0`；host 只留 10k G0」。用 `--diskann-layout` 开关，旧 `--graph-file --nbr-bundle` 原样保留。

**Tech Stack:** 现有 `placement.hpp` / `search_beam.cpp` / `hide_fill.hpp` / `dram_window.hpp`；新建 packer、nav、测试。

**Spec:** `docs/superpowers/specs/2026-09-03-cxl-dram-diskann-layout-design.md`

**分支：** 在 `/root/chukexin/CXL-ANNS-KX-t1-oracle`（`t1-toward-oracle`）上改。不要改 `hide-toward-oracle` 的默认 CLI，以免 50.25 不可复现。

---

## 1. 现码实际在干什么（必须先承认）

`search_one_fp`（`serving/search_beam.cpp:315`）一次 expand 走两条地址：

```cpp
pl.vec(id)    // 向量：ssd_base + off_vectors + id * vec_stride   （pagebin，800 B）
pl.nbrs(id)   // 图：graph_host[id * R] 或 ssd_base + off_graph
              // 运行时 graph_host 来自 --graph-file（1.28 GiB 拷进 host）
```

`hide_read_nbrs`（`hide_fill.hpp:84`）是 `memcpy(pl.nbrs(id))`，所以 **邻居 ID 从不走 DramWindow**。P3 + `--nbr-bundle` 再额外 mmap 267 GiB，把 N(u) 的**向量副本**打成 7 张 4K。

`--oracle-window`：discarded 把本评测向量 WS pin 进 **host numa 窗**，再 `freeze_fills`。图已经在 host。这就是 85.8，**不是**「图+向量在 CXL-DRAM」。

`--oracle-dram`：`--image` MAP_POPULATE 进 **host 文件**，仍然不是 dax。

`--dram-backend` 默认已是 `numa`（2026-09-02 改的）。`--cxl-dram-dev` / dax 是死 BAR 路径。

### 现码 → 新路径对照

| 现码 | 文件:行 | 新路径（`--diskann-layout`） |
|------|---------|------------------------------|
| `graph_in_dram=true` + `--graph-file` 拷 1.28 GiB | `search_beam.cpp:1126,1338-1375` | **禁止**拷全图。只 load `nav_10k.bin`（~10 MiB） |
| `Placement::nbrs` 读 `graph_host` | `placement.hpp:60-63` | `entry(id)+804`，与 vec 同一 `STRIDE` |
| `Placement::vec` 读 `off_vectors+id*800` | `placement.hpp:64-66` | `entry(id)+0` |
| `hide_read_nbrs` 无条件 memcpy host 图 | `hide_fill.hpp:84-86` | 从 **同一条目** 读；Hide 时条目必须先 resident |
| `get_nbr` 对图做 `lookup_or_promote` | `search_beam.cpp:326-330` | 与 `get_vec` 合并：promote **整条 STRIDE** |
| `--nbr-bundle` / `has_bundle` / 267 GiB | `search_beam.cpp:1312-1336` | 本轨 **不启用**。预取单位 = 邻居的整条 entry |
| `hide_collect_bundle_pages` | `hide_fill.hpp:37` | 新 `hide_collect_entry_pages`：要 id 的 `[id*STRIDE, +STRIDE)` |
| `for_ids_contained_in_page` 的 5-in-4K / bundle 7 页 | `placement.hpp:101-134` | 一页 4K 含 `4096/STRIDE` 个完整条目（2048→2，4096→1） |
| `--oracle-window` + host 1/2 GiB 窗 | `search_beam.cpp:1611` | 本轨不用。Oracle = 整库已在 dax |
| `--oracle-dram` + host `--image` | `search_beam.cpp:1277-1283` | 改为 mmap `--cxl-dram-dev /dev/dax0.0` |
| `--dram-backend numa` 默认 | `search_beam.cpp:1100` | 旧路径保持 numa。`--diskann-layout` 强制 dax |
| `hide_warm_entry_ball` pin 向量+读 host 图 | `hide_fill.hpp:312` | pin G0 对应全图 entry + entry 点的 1-hop **整条** |
| spec-beam 依赖 `has_bundle()` | `search_beam.cpp:558` | 改发邻居 entry 页；默认仍 0 |
| `test_placement` 测 graph_host 覆盖 | `tests/test_placement.cpp` | **保留**（旧路径）。另加 `test_diskann_entry.cpp` |
| `tools/run_oracle_host_window.sh` | host 2 GiB 窗 | 不删。新脚本 `run_oracle_cxl_dram.sh` |

### 明确不改（50.25 复现）

无 `--diskann-layout` 时：行为与现在完全相同（`--graph-file`、pagebin、`--nbr-bundle`、`--oracle-window`、numa 窗）。CLI 默认 **不** 打开 `--diskann-layout`。

---

## 2. 新布局（写入 header，复用 magic）

沿用 `CxanLayoutHeader`，用字段区分模式：

```
version = 2
off_graph = 0, len_graph = 0     // 图不在单独段
off_vectors = 4096
len_vectors = n * STRIDE         // 每条是 vec+nbrs+pad
vec_stride 由 set_header 算成 STRIDE（len_vectors/n）
```

条目（T2I `dim=200` `vec_bytes=4` `R=32`）：

```
+0     float  vec[200]     800
+800   u32    nnbrs        4
+804   u32    nbrs[32]     128
+932   pad                 STRIDE-932
```

`placement.hpp` 增加（旧 API 保留）：

```cpp
bool diskann_layout = false;  // set when version==2 || --diskann-layout
size_t entry_stride() const { return vec_stride; }
const uint8_t* entry(uint32_t id) const {
  return ssd_base + hdr->off_vectors + (size_t)id * vec_stride;
}
// nbrs()/vec()：if (diskann_layout) 从 entry() 取；else 旧逻辑
```

`set_header`：`len_graph==0 && len_vectors== n*stride && stride>=932` → 自动 `diskann_layout=true`。

STRIDE：Task 0 读 `/sys/bus/dax/devices/dax0.0/size`。`< 40 GiB` → **2048**（整库 19.07 GiB）。否则 4096。

---

## 3. 搜索语义（只改 diskann 路径）

```
q
 → NavGraph::search_entry(q, L0=64)     // 只碰 host ~10 MiB
 → start = 最好的全图 ID
 → 在 G 上 L=400 oneshot-fp
      expand u:
        e = entry(u)                    // 一次 promote STRIDE
        score 用 e+0
        出边用 e+804                    // 不再 hide_read_nbrs(host)
        对未见邻居 v：prefetch entry(v)
```

Hide：`DramWindow` 仍按 **4K 页** 管。`hide_collect_vec_pages` 对 diskann 改为收集 `entry(id)` 所在 4K（STRIDE=2048 时一页 2 个完整点，这是 DiskANN 式「一次 I/O 带邻居记录」）。

Oracle：`mmap(/dev/dax0.0)` 为 `ssd_base`，`freeze_fills=1`，`vmem` 不打开。G0 仍在 host。`nvme_read_B` 必须 0。

---

## 文件地图（只动这些）

| 动作 | 路径 | 改什么 |
|------|------|--------|
| 改 | `serving/placement.hpp` | `entry()` / diskann `vec`/`nbrs` / `for_ids_contained_in_page` |
| 改 | `serving/hide_fill.hpp` | `hide_collect_entry_pages`；`hide_read_nbrs` 在 diskann 下从 entry 读 |
| 改 | `serving/search_beam.cpp` | CLI、映射 dax、跳过全图拷贝、G0 entry、P3 发 entry 页 |
| 改 | `serving/prefetch.hpp` | pin/prefetch 用 `entry(id)` 而非拆开 vec+nbrs |
| 保持 | `serving/tests/test_placement.cpp` | 旧 graph_host 契约 |
| 新建 | `serving/tests/test_diskann_entry.cpp` | 932/STRIDE、同条 vec+nbrs |
| 新建 | `serving/nav_graph.hpp` | 10k G0 load + 小 beam |
| 新建 | `serving/tests/test_nav_graph.cpp` | seed=42 可复现、体积、entry |
| 新建 | `tools/pack_diskann_entry.cpp` | pagebin+`pagebin_graph.bin` → 镜像（只读 pagebin） |
| 新建 | `tools/build_nav_graph.cpp` | 抽 10k |
| 新建 | `tools/stage_diskann_to_dax.sh` | 镜像 → dax，抽检 |
| 新建 | `tools/run_oracle_cxl_dram.sh` | T=1 Oracle |
| 保持 | `tools/run_oracle_host_window.sh` | 历史 2 GiB host 窗 |
| 改 | `docs/superpowers/specs/2026-09-03-cxl-dram-diskann-layout-design.md` | STRIDE 锁定后回写 |
| 追加 | `docs/notes/2026-09-03-t1-toward-oracle.md` | 新 Oracle 数；不改 50.25 |

输入（只读）：`pagebin_graph.bin`、pagebin 向量（host 文件或 vmem **读**）。  
输出：`/mnt/disk0/chukexin_motivation/serving_t2i_10m/diskann_t2i_10m.bin`、`nav_10k.bin`。  
**禁止**写 420 GiB、禁止双盘、禁止 `vmem.ko`。

---

### Task 0: 锁 STRIDE

- [ ] **Step 1**

```bash
echo "dax=$(cat /sys/bus/dax/devices/dax0.0/size 2>/dev/null || echo none)"
ls -l /dev/dax0.0
```

- [ ] **Step 2:** `dax>=40960000000+4096` → STRIDE=4096，否则 **2048**。写入 spec 和 `constexpr size_t kDiskannStride`。

---

### Task 1: placement diskann API（TDD）

- [ ] **Step 1:** 新建 `serving/tests/test_diskann_entry.cpp`（payload=932，stride=2048，`vec` 与 `nbrs` 同 buffer）。编译应失败。
- [ ] **Step 2:** 在 `placement.hpp` 加 `diskann_payload_bytes` / `entry` / 分支后的 `vec`/`nbrs`。`for_ids_contained_in_page`：diskann 时按 `off_vectors + i*STRIDE` 整条落在页内则回调 `i`。
- [ ] **Step 3:** `g++ -std=c++17 -I$ROOT serving/tests/test_diskann_entry.cpp -o /tmp/tde && /tmp/tde` 以及旧 `test_placement` 仍过。

---

### Task 2: packer

- [ ] **Step 1:** `tools/pack_diskann_entry.cpp` 读 graph raw `n*R*4`、向量 `n*800`（从 pagebin 的 `off_vectors`）。写出 header version=2 + n×STRIDE。n=4 fixture 先自测。
- [ ] **Step 2:** 打 10M。校验 `stat` 大小、8 个随机 id 的 vec/nbrs 与输入一致。向量源优先 host `pagebin_image.bin`；没有则 **只读** vmem 拷到 host 再 pack。

---

### Task 3: 10k 导引图

- [ ] **Step 1:** `tools/build_nav_graph.cpp`：`mt19937(42)` 不放回抽 10000，写出 `nav_10k.bin`（ids + 每点 932 B，nbrs 仍是全图 ID）。
- [ ] **Step 2:** `serving/nav_graph.hpp`：`load`、`search_entry(q, L0=64)` → 全图 ID。`test_nav_graph.cpp`：4 点夹具，query=点0 → 返回 0。体积 &lt; 64 MiB。

---

### Task 4: search_beam 接新路径

- [ ] **Step 1: CLI** `--diskann-layout`、`--nav-graph`、`--cxl-dram-dev`。`--diskann-layout` 且无 `--nav-graph` → 退出 2。`--diskann-layout --oracle-dram` 且 backend 为 numa → 退出 2（「拒绝 node1 替身」）。
- [ ] **Step 2:** `graph_in_dram && !diskann` 才拷 `--graph-file`。diskann 时 `pl.diskann_layout=true`，`ssd_base` = mmap dax（Oracle）或 vmem（Hide，偏移新文件，**不是** 420 GiB 旧 pagebin）。
- [ ] **Step 3:** `search_one_fp` 开头：`eg.entry_id = nav.search_entry(q)`。`get_vec`/`get_nbr` 在 diskann 下都 promote `entry(id)`。P3 issue：`hide_collect_entry_pages` 替代 bundle。
- [ ] **Step 4:** `test_cxl_roles_cli.sh` 加：diskann+numa oracle 必须拒绝；无 nav-graph 必须拒绝。旧三条 refuse 测试保持。

---

### Task 5: T=1 CXL-DRAM Oracle

- [ ] **Step 1:** `tools/stage_diskann_to_dax.sh`：镜像写入 `/dev/dax0.0`，回读 8 个 id。
- [ ] **Step 2:** `tools/run_oracle_cxl_dram.sh`：T=1，nq=20，L=400，seed=42，`--diskann-layout --oracle-dram --nav-graph --cxl-dram-dev /dev/dax0.0`。日志 `results/paper_figs/oracle_cxl_dram_T1_nq20.log`。必须 NAND=0、recall≥0.92。
- [ ] **Step 3:** 关掉 G0（随机单 entry）对照一行。recall 若 &lt;0.92，笔记写「10k 导引必要」。**不替换 85.8。**

---

### Task 6（后开）: Hide

同一镜像放到 CXL-SSD **新偏移**（不要覆盖 420 GiB）。对照 Task 5 新 Oracle。

---

## 完成标准

- 无 `--diskann-layout` 的二进制仍能按旧 CLI 复现 50.25 协议
- `test_placement` + `test_diskann_entry` + `test_nav_graph` + 扩过的 `test_cxl_roles_cli.sh` 全过
- pack 抽检与旧图/向量一致
- T=1 dax Oracle 日志存在，NAND=0，recall≥0.92
- 50.25 / 85.8 数字未改
