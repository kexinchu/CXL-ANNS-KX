# Serving prefetch — 200k proxy (25M quotas)

**目的：** host 仅 entry；全库进 SSD 映像；1 GiB CXL-DRAM 窗口 + P0–P4 预取；先在 200k 验证路径（25M 文件尚未就绪）。

**做法：** `build_layout_25m` 写 float16+PQ32+R32 映像与 entry；`search_beam` 经 `DramWindow::lookup_or_promote` 读 PQ/邻接；P1/P3 同步预取，P2 异步，`--rerank`=P4。配额：`cap_budget.py` / `cap_enforce.sh` / `cap_smoke`（vmem@200 GiB 划 100 GiB，DRAM 1 GiB OK）。

**结果（50 queries，beam=32，iters=48，image=file-backed）：** 窗口 1 GiB ≫ 200k 工作集 → hit≈97.6%，`avg_promote_ns`≈3–8 µs（miss 升迁）。预取增大预算时常 **降** QPS（P1@4 MB 333 vs P0 峰值~880）；P3@256 KB 有时更好（~779）；P2 异步明显更慢（~125–140）。符合「盲目/过度预取有害」。

| policy | budget | QPS | hit% | avg_promote_ns | prefetch_pages |
|--------|-------:|----:|-----:|---------------:|---------------:|
| P0 | 0 | 468 | 97.58 | 6067 | 0 |
| P0 | 64KB | 881 | 97.58 | 3025 | 0 |
| P1 | 64KB | 772 | 97.58 | 3024 | 553 |
| P1 | 4MB | 333 | 97.61 | 8899 | 5130 |
| P3 | 256KB | 779 | 97.58 | 2775 | 1417 |
| P2 | 256KB | 126 | 97.59 | 7438 | 3350 |
| P1+rerank | 256KB | 384 | 97.19 | 6635 | 1417 |

**未解决：** 真 25M 语料；映像打进 `/dev/vmem0` 的冷 NAND miss；缩小 DRAM 或强制冷窗口以拉大 hit/miss 台阶；真实 PQ（现为占位 uint8）；recall 未对 GT 标定。
