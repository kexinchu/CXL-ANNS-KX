# Motivation Findings（CXL SSD / `/dev/vmem0`）

> 日期：2026-07-31  
> 数据：LAION **200k × 1024**，HNSW-kNN 图 degree=32  
> 路径：向量在 `/dev/vmem0` 统一地址空间（mmap load/store；miss → fault → NVMe）  
> 日志：`vmem_f123_*.log`（H0/F1–F3）、`vmem_f4_f9_20260731_004501.log`（F4–F9）  
> ABI 参考：[vmem_sw demo design](https://github.com/vickiegpt/mem2nvme/blob/main/docs/superpowers/specs/2026-07-30-vmem-sw-comprehensive-c-demo-design.md)

身份门：`PASS cxl-vmem-identity`（software vmem + Montage `mem0/region0`）。  
冷启动：`MADV_DONTNEED` + thrash ≈5 GiB（> `cache_limit` 4 GiB）。

---

## 结论一览

| ID | 假说 | 结论 | 一句话 |
|----|------|------|--------|
| **H0** | hit ≪ SSD miss | **成立** (~140×) | warm ~0.6 µs vs cold ~78 µs |
| **F1** | 图 sync prefetch 必赢 | **未成立** | demand 最快；2-hop `read_ios`×5 |
| **F2** | record ≻ 页 admission | **成立** | QPS ~3.6×，SSD ÷3.7 |
| **F3** | beam↑ → hit↓/SSD↑/QPS↓ | **成立** | uniform 上耦合更陡 |
| **F4** | stall 放大「假 recall」；效率最优点 | **部分成立** | recall 在 beam≥32 饱和；需看 recall/IO |
| **F5** | 升迁精度 ≫ 升迁带宽 | **成立** | L↑ → precision↓、QPS↓、SSD↑ |
| **F6** | 图/hub 驻留 ≻ 页/盲扩展 | **部分成立** | hub_pin ≳ record ≫ page；**盲 graph_expand 更差** |
| **F7** | 共享热集 > 单查询加深 beam | **成立** | beam8+shared ≫ beam32 |
| **F8** | soft-cache ≠ PTE（双台阶） | **成立** | ~83 µs / ~3.8 µs / ~2.2 µs |
| **F9** | 简单 miss 自适应 beam 必赢 | **未成立**（本实现） | 同 recall 下不如 static |

---

## H0 / F1–F3（摘要）

详见既有段落与 `vmem_f123_*.log`。要点：

- **H0**：cold/warm ≈ **140×**；`Δread_ios≈Δfaults`。  
- **F1**：demand 29.4 QPS；graph_2hop 6.6 QPS 且 `read_ios` 52838 vs 10841。  
- **F2**：record 33.5 QPS / 9.9k reads vs page16k 9.3 / 36k。  
- **F3**：uniform beam 4→64：hit 58%→38%，QPS 26→18，SSD 7.8k→12.6k。

---

## F4 — 部分成立（recall 效率，非单调）

nq=16，hops=32，brute-force recall@10（zipf queries）：

| beam | QPS | hit% | recall@10 | SSD reads | recall/s | recall/kIO |
|------|-----|------|-----------|-----------|----------|------------|
| 4 | 35.9 | 62.6 | 0.094 | 4763 | 3.37 | 0.315 |
| 8 | 27.2 | 61.0 | 0.188 | 6286 | 5.10 | 0.477 |
| 16 | 27.2 | 61.4 | 0.175 | 6226 | 4.76 | 0.450 |
| 32 | 23.9 | 61.0 | **0.244** | 6294 | 5.84 | **0.620** |
| 64 | 27.3 | 61.0 | **0.244** | 6294 | **6.66** | **0.620** |

- beam 32→64：**recall 与 SSD 完全持平**（hops 预算下已饱和），再加大 beam 无质量收益。  
- 单纯 QPS 最优在 beam=4，但 recall 最差 → **不能只报吞吐**。  
- 注：绝对 recall 偏低（hops=32 限制）；相对趋势足够支撑「质量/代价需联合度量」。更强版本可加大 hops / 固定 SSD budget 截断。

---

## F5 — 成立（升迁精度 vs 体积）

beam=32，hops=32，nq=16；投机 = top-L 邻居的 2-hop `PREFETCH_LOGICAL`：

| 策略 | QPS | hit% | SSD reads | promote | used | precision |
|------|-----|------|-----------|---------|------|-----------|
| demand | **25.9** | 57.5 | **6839** | 0 | 0 | — |
| top1_2hop | 21.0 | 57.5 | 8606 | 10.3k | 8.5k | **82.4%** |
| top2_2hop | 15.2 | 57.5 | 11.9k | 16.9k | 11.6k | 68.5% |
| top8_2hop | 5.0 | 57.5 | **37.8k** | 45.8k | 13.3k | **29.0%** |

命中率不变（与 F1 一致），但 **L 越大精度越低、SSD 与延迟越差**。  
→ Motivation：**错误升迁代价高于漏升迁**；系统应做有界高精 promote，而非高覆盖预取。

---

## F6 — 部分成立（hub/record ≻ page；盲扩展有害）

同一 64 MB 用户态预算：

| 策略 | QPS | hit% | SSD reads |
|------|-----|------|-----------|
| hub_pin（入度 top + record） | **37.3** | 70.9 | **4706** |
| record_lru | 35.9 | 69.2 | 4981 |
| page16k | 9.6 | 63.7 | 19252 |
| graph_expand（miss 时带入全部邻居） | 3.9 | 84.8† | **47251** |

† hit% 被「先 admit 再 touch」抬高，不代表更少 SSD。  
→ **按入度 pin hub** 略优于纯 LRU；**按连续页**与 **盲图扩展** 均差。与 CXL-ANNS「距 entry 的 hop 缓存」不同：这里优化的是 **SSD miss 悬崖下的字节预算**。

---

## F7 — 成立（共享热集 > 加深 beam）

先用主机侧 walk 统计跨 query 频次，pin ≈32 MB 热点（5769 向量），再冷启动比较：

| 配置 | QPS | hit% | SSD reads |
|------|-----|------|-----------|
| beam32 无 pin | 27.7 | 60.8 | 6313 |
| beam8 无 pin | 30.4 | 64.7 | 5682 |
| **beam8 + shared** | **45.9** | **77.7** | **3595** |
| beam16 + shared | 44.4 | 77.2 | 3680 |

**beam8+共享热集** 相对 beam32：QPS **+66%**，SSD **−43%**。  
→ 多查询/服务场景下，跨 query 驻留比单 query 加大 beam 更划算。

---

## F8 — 成立（双台阶悬崖）

800 个互异向量探针：

| 状态 | ns/向量 | Δfaults | Δread_ios |
|------|---------|---------|-----------|
| cold SSD（thrash 后） | **82650** | 798 | 787 |
| warm PTE | **2152** | 0 | 0 |
| **soft-cache 热 + PTE 冷**（仅 `MADV_DONTNEED`） | **3794** | 798 | **0** |
| thrash 后再测 | 88195 | 798 | 787 |

三级延迟：SSD miss ≫ soft-cache 命中但需 fault ≫ 已建 PTE。  
`read_ios=0` 且 `faults>0` 证明 **F1 的 soft-cache prefetch 不会抬高 mincore/PTE hit%**，但理论上可把 miss 从 ~83 µs 降到 ~3.8 µs——前提是 **预填的页真被用到且不被挤出**（F5 显示大 L 做不到）。

---

## F9 — 未成立（朴素自适应）

按每查询 miss 率调 beam（>45% 减半，<25% 加倍）：

| 策略 | QPS | hit% | recall@10 | SSD |
|------|-----|------|-----------|-----|
| static4 | **53.1** | 73.3 | 0.063 | 3115 |
| static16 | 30.5 | 65.1 | **0.113** | 5621 |
| static32 | 30.6 | 64.9 | **0.113** | 5648 |
| adaptive | 28.9 | 59.3 | 0.113 | **6068** |

同 recall 下 adaptive **更慢、更多 SSD**（final_beam=8，但前期已付高 beam 的 miss）。  
→ 不支持「简单 miss 反馈调 beam 即赢」；需要 **iso-recall 控制器 / 驻留预算**（与 F4+F7 结合），属后续系统设计，非已证 claim。

---

## 对 Motivation / idea 的可用叙事

**已能写进 Motivation：**

1. 统一 AS 下 **~140× hit/miss 悬崖**（H0），且存在 **soft-cache ↔ PTE 双台阶**（F8）。  
2. **页/盲图升迁有害**；**record + hub pin** 更合理（F2/F5/F6）。高维（≈页大小）页错配最强；小 dim 下差距缩小（M-E2）。  
3. **efSearch 与驻留/SSD 强耦合**；应用 **recall/IO 或共享热集**，而非只加大 ef（F3/F4/F7/MOT-EF：ef 100→1600 召回 0.66→0.93，SSD IO ×6.3）。  
4. 朴素 sync 图 prefetch / 朴素自适应 beam **不够**（F1/F9/M-E3）→ 系统点应是 **有界高精 promote + 跨查询驻留 + stall-aware 质量旋钮**。  
5. **重要性加硬（M-E）：** miss≥10% 即 SSD 占 path >66%（M-E1）；并发无 pin 时 QPS 随线程崩（M-E5）。

**相对文献的差异化：**

| 路线 | 本文位置 |
|------|----------|
| CXL-ANNS（全进 CXL + 预取掩盖百 ns） | 我们面对的是 **SSD 十~百 µs** + soft-cache 污染 |
| Second-Tier（CXL 替 SSD 细粒度） | 我们是 **统一 VA 的 CXL-SSD 分层**，不是「索引搬家」 |
| DiskANN（DRAM PQ + 块 I/O） | 我们是 **load/store + fault**，admission 按 record/图语义 |
| DiskANN mem 无 PQ（同机 0.25–0.57 ms/q） | 上界：全精度已在 DRAM |
| DiskANN SSD 无 PQ 导航（`--no_pq_nav`，4.9–15.7 ms，~472–1275 IO） | 同栈去 PQ 后 IO 暴涨；仍快于朴素 cold vmem（调度/cache 差异） |

---

## 复现

```bash
cd /root/chukexin/motivation_exps
./motivation vmem-identity
# populate once: ./motivation vmem-populate --dir /mnt/disk0/chukexin_motivation/data/laion1m_200k
bash scripts/run_vmem_f123.sh          # H0 相关 / F1–F3
bash scripts/run_vmem_f4_f9.sh         # F4–F9
./motivation vmem-mot-me --dir /mnt/disk0/chukexin_motivation/data/laion1m_200k --nq 16 --hops 32 --beam 32
# Prove CXL-SSD path (must ALL PASS):
./motivation vmem-verify --dir /mnt/disk0/chukexin_motivation/data/laion1m_200k --nq 8
# M-E2 host-only (not CXL-SSD): ./motivation gen ... && ./motivation me2-f2 ...
# P2 on CXL-SSD: ./motivation vmem-f2 --dir .../laion1m_200k --nq 16 --cache-mb 64
```
