# Break best-first prefetch (T=1 latency)

**Status:** Design for review — do not implement until approved  
**Date:** 2026-08-31  
**Goal:** 单路 hide mean 从 ~19.8 ms 压向窗口 oracle **11.4 ms**；query 实际会打分的向量尽量在 expand 之前已在 DramWindow，critical path 上不再 `wait_covering` / 同步读 CXL-SSD。`from_win=100`，bounce=0，recall@10 ≥ 0.92。不改 claim 默认，除非 nq=20 **与** nq=100 同时赢。

## Constraint

- Host DRAM **只存稀疏图**。向量和 expand 用的 bundle 在 NAND。
- 打分仍只读窗口。禁止 `--score-page`、禁止为利用率打多余 sibling。
- 图在 host：邻居 **ID** 在 expand/insert 当下就能读；缺的是这些 ID 的 **向量页**。

## Why look=8 is not the answer

`--lookahead-k 8` 对「当前未 expand 的最近 8 个」预取 N(·) 页，但 **不 commit expand**。cand 一更新，这 8 个就不是下一波 `pick_batch`。结果：31 QPS，VOID。  
`ebatch` 16/32、`ahead=3` 在 **T=1** 上也 ≤46。  
本设计必须和这两条失败路径分开：预取集合 = **已经进 beam、且大概率会被 expand 的点** 的 N(·)，不是「每一跳猜 8 个未来 expand」。

## Approaches

### A — 进 cand 就预取 N(u)（推荐）

`finish_score` → `insert_cand` 成功后：若 `u` 在 cand 里距离序的前 **M** 名，异步 `issue` N(u) 的 bundle 页（`stall_if_full=false`），每点只发一次。

- 现在：expand `u` 才拉 N(u) 向量（best-first 锁）。
- 之后：`u` 一成为「够好的候选」就拉 N(u)。下一波 `pick_batch` 选中 `u` 时页应已在飞或已在窗。
- 和 look=8 的差别：集合跟 **beam 成员** 走，不跟「本跳猜的 8 个」走；不重复每 hop 全量刷新。
- 浪费：进 cand 后又被 evict、从未 expand 的点。M 用来砍这个尾巴。
- CLI：`--spec-beam-nbrs M`，默认 **0**（claim 路径不变）。先扫 M ∈ {8, 16, 32, 64}。

### B — 放宽 expand 选择（slack best-first）

不仅 expand 最近的，还 expand「距离 < best+δ」或固定前 K 个未评分完的点。更像某些 PipeANN 松弛。  
风险：多 expand → 更多距离计算和 NAND；recall 可能涨也可能 QPS 掉。T=1 上加宽 ebatch 已失败。**不作第一刀。**

### C — 加厚 entry 2-hop warm

`hide_warm_entry_ball` 已做 entry+1-hop+~8192 extras（计时前）。再扩 2-hop 只能盖住开头，L=400 后程仍 miss（hit 已是 13.75%）。**可作 A 的补充，不能单独到 11.4。**

## Recommended

做 **A**。B 不作默认。C 仅当 A 的前几跳仍 `crit_wait` 再加。

### Mechanics (A)

1. `insert_cand` 改为返回是否留下该 id。
2. `Prefetch::spec_beam_nbrs`（M）。`spec_issued` 防重发。
3. 排名：cand 中 `dist` 严格更优的个数 `< M` 才发。
4. `hide_read_nbrs` + `hide_collect_bundle_pages`（`want` = 未见邻居）。`pipe.issue(..., stall_if_full=false)`。
5. **不**对这些页上的 sibling 打分。
6. 日志：`spec_issue_pages`、`spec_uniq_ids`、`crit_wait`、`overlap`、`from_win`。

### Acceptance (same binary)

| 行 | 必须 |
|----|------|
| T=1 nq=20 | mean **低于** 复基线；`from_win=100`；recall≥0.92；QPS 不低于复基线 −2% |
| T=1 nq=100 | 同上 |
| 替换 50.25 | 两行 QPS **都**高于锁定 hide |
| T=1 回归 M=0 | 仍约 50 / 20 ms |

`crit_wait` 应下降。若 issue_use/字节涨、mean 不降 → 整刀撤回。

### Non-goals

Device walk、改 `ntcx`、双盘 restage、`--cont-batch` 共享窗、占用刷 100%、改 Intro。

## Open question (one)

M 的第一轮扫描是否包含 **64**？64 接近「几乎每个进 beam 的点都预取 N(·)」，NAND 税最大。建议第一轮 **8/16/32**，64 仅当前三档 `crit_wait` 仍高且窗口未满时再加。
