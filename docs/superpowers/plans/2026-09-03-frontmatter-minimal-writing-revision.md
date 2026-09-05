# FlashANNS Pre-Design Minimal Writing Revision Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox syntax for tracking.

**Goal:** 在不改变 Introduction、Background、Motivation 既有结构的前提下，
修正事实与术语不一致、收紧超出证据的表述、减少审稿人理解歧义，并消除这三节
中最明显的排版溢出。

**Architecture:** 这是一次句子级 revision，不是章节重写。按
Introduction → Background → Motivation 的阅读顺序逐节修改，每一节完成后做
局部 claim 检查；最后统一检查 C1/C2/C3、score/query hide 和延迟数字，并编译
前五页进行视觉验收。

**Tech Stack:** English LaTeX (paper/sections/*.tex), acmart,
latexmk, pdftotext, pdftocairo, rg, git diff.

**Spec:** docs/superpowers/plans/2026-09-02-flashanns-paper-plan.md
（§0--§1.9 的冻结故事线）以及 /root/chukexin/paper-write-prompts.md
（锁定数字、硬件诚实边界和禁止项）。

## Global Constraints

- 只允许修改：
  - paper/sections/intro.tex
  - paper/sections/background.tex
  - paper/sections/motivation.tex
- 不修改 paper/main.tex、Design 及后续章节。
- 不增删、重命名或重排任何 section、subsection、贡献项、图、表或 label。
- 不修改绘图脚本或 PDF；尤其不重新生成 mot-cliff.pdf 和
  mot-pathology-admission.pdf / mot-pathology-prefetch.pdf。
- Fig. 1 只允许替换一个会造成符号冲突的文字标签；不改变 TikZ 几何结构。
- 保留四条贡献和 C1/C2/C3 → D0/D1/D2 的既有映射。
- 保留锁定结果：Demand 1.64 QPS、score hide 50.25 QPS、
  from_win=100、oracle 85.8 QPS、query hide 0.59×。
- Motivation 不出现 50.25、85.8、0.59× 或 from_win。
- 不把 query hide 写成已达到；不把 LFU、one-copy 或 Shared FAM 写成系统贡献。
- CXL-DRAM 只指真实 /dev/dax0.0 测量或引用的 DRAM 类设备；1 GiB score
  window 仍明确是 host DRAM proxy。
- 不自动提交。当前工作树包含并行中的作者改动，等待作者审阅后再决定 commit。

---

## 审计结论与优先级

| Priority | 位置 | 当前问题 | 最小修复 |
|---|---|---|---|
| P0 | Intro C1 | 40--80 us / 140× 与新 Fig. 3 的 89 us / 130--320× 不一致 | Intro 改成新测量范围 |
| P0 | Intro / Background | Intro 用 R 表示 worker 数，Background 又用 R 表示图度数 | 不再用 R 表示 worker 数 |
| P0 | Background Table 1 | 20--80 us 不覆盖实测 88.7--88.8 us；80--120 ns 无就近依据 | 表只表达量级 |
| P0 | Background Table 1 | lll 表格超出单栏约 69.5 pt | 保留三列四行，改定宽列 |
| P0 | Motivation C2 | top-1/2/8 frontier roots 被误写成 1/2/8-hop prefetch | 明确三档均为 two-hop |
| P0 | Motivation beam | “NAND grows faster / adds stall” 超出 F4 数据 | 只写 32→64 recall 与 NAND 同时持平 |
| P0 | Motivation Table 2 | caption 把 C1--C3 全归为 LAION-200k measured deltas | 分清三类证据来源 |
| P1 | Background block path | “PQ codebook ranks neighbors” 遗漏 PQ codes | 写清 PQ codes + codebook |
| P1 | Background prefetch | 发出 prefetch 被写成自动离开临界路径 | 加入 complete-in-time 条件 |
| P1 | Motivation C3 | “beam ramps slowly”和“pays the cliff again”因果不清 | 改为 fill stream 和 critical-path wait |
| P1 | Intro framing | C2/C3 不是单纯 hardware facts | 改为 hardware--search interactions |
| P2 | 三节整体 | mmap'd、score window、full precision 等写法略不统一 | 做限定词和连字符 sweep |

---

### Task 1: Freeze the Structural and Artifact Baseline

**Files:**
- Read: paper/sections/intro.tex
- Read: paper/sections/background.tex
- Read: paper/sections/motivation.tex
- Do not modify repository files

**Interfaces:**
- Consumes: 当前 section 结构、图表 label 和现有 PDF。
- Produces: /tmp 下的 heading 与 figure hash 基线。

- [ ] **Step 1: Record the exact section/subsection topology**

Run:

    rg --no-line-number -o '^\\(section|subsection)\{[^}]+\}' \
      paper/sections/intro.tex \
      paper/sections/background.tex \
      paper/sections/motivation.tex \
      > /tmp/flashanns-frontmatter-headings.before

Expected: Introduction 只有一个 section；Background 保持四个 subsection；
Motivation 保持四个 subsection，顺序不变。

- [ ] **Step 2: Record figure artifact hashes**

Run:

    sha256sum \
      paper/figs/mot-cliff.pdf \
      paper/figs/mot-pathology-admission.pdf \
      paper/figs/mot-pathology-prefetch.pdf \
      > /tmp/flashanns-frontmatter-figures.before.sha256

Expected: three hashes are recorded successfully.

- [ ] **Step 3: Record current front-section warnings**

Run:

    make -C paper
    rg -n 'Overfull|Underfull|undefined' paper/main.log \
      > /tmp/flashanns-frontmatter-warnings.before

Expected: baseline captures the current Intro paragraph warnings and the
Background Table 1 overfull warning.

---

### Task 2: Tighten Introduction Without Changing Its Five-Block Structure

**Files:**
- Modify: paper/sections/intro.tex:12-30
- Modify: paper/sections/intro.tex:51-83 (text labels only)
- Modify: paper/sections/intro.tex:92-116
- Modify: paper/sections/intro.tex:118-131
- Test: compiled paper/main.pdf, pages 1--2

**Interfaces:**
- Consumes: score/query-hide split and locked C1/main-result numbers.
- Produces: the claims that Background defines and Motivation substantiates.

- [ ] **Step 1: Replace ambiguous first/scaling wording**

Make only these sentence-level changes:

1. Replace subjective “makes scoring usable” with the observable contract:
   “enforces score hide: full-precision distances read only bytes already
   resident in the score window.”
2. Replace “Adding R query workers ...” with:
   “Scaling the service across hosts commonly replicates or shards the
   flash-resident index.”
3. Remove “and a pooled HDM can be one copy” from the CXL-DRAM contrast.
   Keep Shared FAM only in the existing scope sentence at the paragraph end.
4. In Fig. 1(a), replace “R workers implies R copies” with
   “Scale-out: replicate or shard”.

Acceptance:

- R first appears in Background as graph degree.
- The scoped first remains limited to graph-ANNS over an SSD-backed CXL
  address space.
- Fig. 1 geometry and caption structure remain unchanged.

- [ ] **Step 2: Align C1 with the finalized Fig. 3 evidence**

1. Replace “Three hardware facts” with “Three hardware--search interactions”.
2. Replace 40--80 us / 140× with:
   “On our measured path, a miss is about 89 us, or 130--320× a CXL-DRAM
   item load.”
3. Keep the following C2/C3 order and labels unchanged.

Acceptance:

- Intro, Motivation, and Table 2 use 130--320×.
- No 40--80 us or standalone 140× remains in Introduction.
- The sentence says “our measured path,” not “a product CXL.mem SSD.”

- [ ] **Step 3: Make the achieved contract observable in P4 and contribution 3**

1. Render from_win=100% in prose as:
   “100% of full-precision scores read the window,” with the counter in
   parentheses.
2. Use the same formulation in contribution 3 instead of “100% window hits.”
3. Keep query hide explicitly at 0.59× and retain the hop-i → hop-i+1 residual.
4. Shorten contribution 4 only enough to remove the current 21.9 pt overfull
   line; retain Demand, FlashANNS, oracle, and PipeANN.

Acceptance:

- score hide is achieved; query hide remains an unmet oracle.
- all four locked performance numbers remain present.
- four numbered contribution items remain.

- [ ] **Step 4: Compile and inspect Introduction**

Run:

    git diff --check -- paper/sections/intro.tex
    make -C paper
    pdftocairo -f 1 -l 2 -png -r 130 paper/main.pdf \
      /tmp/flashanns-intro-review
    rg -n 'Overfull.*|sections/intro.tex' paper/main.log

Expected:

- build exits 0 and has no undefined references;
- the previous 21.9 pt contribution overfull is gone;
- Fig. 1 remains on page 2 and contributions remain readable.

---

### Task 3: Correct Background Definitions and Repair Table 1 Width

**Files:**
- Modify: paper/sections/background.tex:11-37
- Modify: paper/sections/background.tex:42-58
- Modify: paper/sections/background.tex:63-78
- Modify: paper/sections/background.tex:83-130
- Test: compiled paper/main.pdf, pages 2--3

**Interfaces:**
- Consumes: Introduction terminology.
- Produces: precise definitions used by Motivation.

- [ ] **Step 1: Keep the walk structure but remove over-generalization**

Preserve the four-step enumerate. Replace
“Quality rises with how many useful neighbors are scored” with:
“Recall depends on the traversal budget and which useful neighbors are
scored.” Keep the next sentence about stall.

- [ ] **Step 2: Correct the block-SSD/PipeANN mechanism**

1. Replace “A compact PQ codebook in DRAM ranks neighbors” with
   “PQ-compressed coordinates and their codebook in DRAM rank candidates
   without fetching every full-precision vector.”
2. Replace “a static node cache pins entry vertices” with
   “a DRAM node cache retains entry-region and frequently visited vertices.”
3. Do not attribute the term “continuous batching” to PipeANN. Describe its
   cross-query interleaving as the block-interface analogue of later
   continuous issue.
4. Preserve the object distinction: PipeANN schedules sectors/I/O requests;
   FlashANNS schedules memory-semantic fills.

Acceptance:

- PQ codes, codebook, full-precision vectors, sectors, and node cache have
  distinct roles.
- no sentence implies that FlashANNS incorporates the PipeANN binary.

- [ ] **Step 3: Tighten CXL-DRAM characterization and citations**

1. Put citations supporting 140--410 ns immediately after that range.
2. Replace “DRAM- or NVM-priced far tier” with
   “a memory-class far tier without a per-miss NAND fill,” unless a cited
   work requires narrower wording.
3. Keep detailed comparison in Related Work.

- [ ] **Step 4: Make the CXL-SSD paragraph explicitly a device model**

1. Start with “In the device model studied here, ...”.
2. Define a hit as served from the score window at DRAM-class latency.
3. Keep a miss as NAND fill plus retry.
4. State: “An early prefetch can overlap the fill; only a fill that completes
   before scoring leaves the scoring path.”
5. Simplify the proxy limit to:
   “The proxy preserves fill semantics, but not product CXL.mem timing.”
   Retain the host-DRAM mbind statement.

Acceptance:

- the paragraph cannot be read as a product CXL.mem SSD evaluation;
- issuing prefetch is not equated with achieving query hide.

- [ ] **Step 5: Preserve Table 1 structure while fixing values and width**

Keep the same caption, label, three columns, and four rows. Change lll to:

    \begin{tabular}{@{}p{0.28\columnwidth}p{0.22\columnwidth}p{0.38\columnwidth}@{}}

Use these evidence-safe values:

    Host DRAM load        & tens--hundreds of ns & graph / PQ, if placed there \\
    Score-window hit      & DRAM class           & resident full-precision bytes \\
    CXL-DRAM (literature) & 140--410 ns           & CXL-ANNS / Cosmos pool \\
    NAND fill (this path) & tens of us            & window miss to flash \\

Use the paper's LaTeX spacing and math conventions during implementation.

Acceptance:

- the current ~69.5 pt table overfull is gone;
- no Background range conflicts with 88.7--88.8 us;
- Table 1 remains a scale guide, not a duplicate result table.

- [ ] **Step 6: Compile and inspect Background**

Run:

    git diff --check -- paper/sections/background.tex
    make -C paper
    pdftocairo -f 2 -l 3 -png -r 130 paper/main.pdf \
      /tmp/flashanns-background-review
    rg -n 'Overfull.*|sections/background.tex' paper/main.log

Expected:

- Table 1 stays inside the right column;
- Background remains four subsections and approximately 1.0--1.2 pages;
- Motivation still begins on page 3.

---

### Task 4: Correct Motivation Evidence Language Without Moving Figures

**Files:**
- Modify: paper/sections/motivation.tex:10-41
- Modify: paper/sections/motivation.tex:57-90
- Modify: paper/sections/motivation.tex:109-130
- Modify: paper/sections/motivation.tex:146-179
- Do not modify: paper/sections/motivation.tex:43-55 (Fig. 3)
- Do not modify: paper/sections/motivation.tex:92-107 (two-panel Fig. 4)
- Test: compiled paper/main.pdf, pages 3--5

**Interfaces:**
- Consumes: Background definitions and locked F2/F4/F5 evidence.
- Produces: C1/C2/C3 statements that Design can answer without inheriting an
  overclaim.

- [ ] **Step 1: Make the section opening name its evidence role**

Replace “how much does the default cost?” with:
“what throughput or NAND-traffic penalty does it cause?”
Keep the same paragraph and C1--C3/Table 2 bridge.

- [ ] **Step 2: Tighten C1 inference without touching Fig. 3**

1. Replace “The same load” with “A matched-size CXL-SSD window-miss read”.
2. Replace “Graph search multiplies the tax by hops × degree” with:
   “A graph walk can pay that tax across hops and newly scored neighbors.”
3. Replace “The new fact is ...” with “The relevant distinction is ...”.
4. Replace informal “mmap'd DRAM” with “memory-mapped DRAM.”

Acceptance:

- 0.28/0.67/89 us and 130--320× remain unchanged;
- Fig. 3 source block and PDF remain unchanged;
- prose does not imply that all hop × degree accesses miss.

- [ ] **Step 3: Correct the exact F2/F5 experiment description**

1. Explain the 16-KB case as a coarse admission unit grouping four 4-KB pages.
2. Replace the incorrect “1-, 2-, or 8-hop neighborhood” with:
   “synchronous two-hop prefetch rooted at the top-1, top-2, or top-8 frontier
   candidates.”
3. Replace “bounded ensure” with “bounded prefetch”.
4. Keep hit rate, NAND traffic, promotion precision, and QPS distinct.

Acceptance:

- top-M changes while two-hop depth remains fixed;
- no Design mechanism or hide-runtime result appears;
- Fig. 4 assets and caption remain unchanged.

- [ ] **Step 4: Limit the beam paragraph to F4 evidence**

Use three sentences with this boundary:

1. Widening L can become another form of unwise coverage.
2. From L=32 to 64, recall and NAND reads both plateau under the fixed hop
   budget.
3. Extra width buys no measured quality benefit in that interval; quality and
   I/O cost must be reported together.

Do not claim that NAND grows faster than recall over the entire sweep or that
L=64 adds measured stall: F4 has equal NAND and recall, and non-monotonic QPS.

- [ ] **Step 5: Repair C3 causal language**

1. Replace “the beam ramps slowly” with “the fill stream ramps slowly.”
2. Explain that a block interface exposes I/O requests that a scheduler can
   interleave across queries.
3. Replace “pays the cliff again” with:
   “If the query waits for those fills before scoring, NAND latency remains on
   its critical path.”
4. Keep the limitation that cross-query issue cannot make one pointer-chasing
   walk equal the in-window oracle.

- [ ] **Step 6: Fix Table 2 evidence provenance**

Keep four columns, three rows, label, and mapping. Replace the caption's
LAION-only attribution with:

“C1 uses the 10M-item probes in Fig. 3; C2 uses LAION-200k; C3 follows from
the dependency and issue path illustrated above.”

Change C1's constraint to “Score only resident bytes” if it fits the current
column; otherwise use “No NAND wait in scoring.”

Acceptance:

- schematic C3 is not presented as a LAION-200k measured delta;
- C1 → score hide, C2 → wise prefetcher, C3 → continuous batching remains.

- [ ] **Step 7: Compile and inspect Motivation**

Run:

    git diff --check -- paper/sections/motivation.tex
    make -C paper
    pdftocairo -f 3 -l 5 -png -r 130 paper/main.pdf \
      /tmp/flashanns-motivation-review
    rg -n 'Overfull.*|sections/motivation.tex' paper/main.log

Expected:

- Fig. 3, both Fig. 4 panels, Fig. 5, and Table 2 remain present;
- no new float-only page appears;
- Design follows Motivation without a new transition section.

---

### Task 5: Cross-Section Consistency and Frozen-Structure Gate

**Files:**
- Verify: paper/sections/intro.tex
- Verify: paper/sections/background.tex
- Verify: paper/sections/motivation.tex
- Verify: paper/main.pdf
- Do not modify additional files

**Interfaces:**
- Consumes: Tasks 2--4.
- Produces: an author-reviewable three-file diff and compiled PDF.

- [ ] **Step 1: Prove structure did not change**

Run:

    rg --no-line-number -o '^\\(section|subsection)\{[^}]+\}' \
      paper/sections/intro.tex \
      paper/sections/background.tex \
      paper/sections/motivation.tex \
      > /tmp/flashanns-frontmatter-headings.after
    diff -u /tmp/flashanns-frontmatter-headings.before \
      /tmp/flashanns-frontmatter-headings.after

Expected: no diff.

- [ ] **Step 2: Prove figure assets did not change**

Run:

    sha256sum -c /tmp/flashanns-frontmatter-figures.before.sha256

Expected: all three figure PDFs report OK.

- [ ] **Step 3: Run stale-wording and forbidden-result sweeps**

Run:

    rg -n \
      '40--80|about \$140|20--80|1-, 2-, or 8-hop|the beam ramps|adds stall|Deltas are from LAION-200k|mmap.d DRAM' \
      paper/sections/intro.tex \
      paper/sections/background.tex \
      paper/sections/motivation.tex

Expected: no matches.

Run:

    rg -n '50\.25|85\.8|0\.59|from_win' paper/sections/motivation.tex

Expected: no matches.

Run:

    rg -n '50\.25|85\.8|0\.59|from_win' paper/sections/intro.tex

Expected: all four locked result concepts remain represented.

- [ ] **Step 4: Check reader-visible vocabulary**

Run:

    pdftotext -f 1 -l 5 -layout paper/main.pdf \
      /tmp/flashanns-frontmatter.after.txt
    rg -n \
      'score hide|query hide|score window|in-window oracle|CXL-DRAM|CXL-SSD|NAND fill' \
      /tmp/flashanns-frontmatter.after.txt

Confirm manually:

- score hide means full-precision scoring from resident window bytes;
- query hide is the unmet no-fill-wait oracle;
- Motivation hit rate is not used as a synonym for score hide;
- CXL-DRAM is not used for the host-DRAM score window;
- block I/O requests and memory-semantic fills remain distinct.

- [ ] **Step 5: Build and visually inspect front matter**

Run:

    make -C paper
    pdfinfo paper/main.pdf | rg 'Pages:|Page size:'
    pdftocairo -f 1 -l 5 -png -r 140 paper/main.pdf \
      /tmp/flashanns-frontmatter-final

Expected:

- latexmk exits 0; paper remains 12 pages;
- Introduction/Fig. 1 stay on pages 1--2;
- Background stays on pages 2--3;
- Motivation begins on page 3;
- Design begins on page 4 as before;
- Table 1 stays inside its column;
- figure numbers remain label-driven; do not hard-code numbers to match the
  historical plan.

- [ ] **Step 6: Review only the scoped diff**

Run:

    git diff --check -- \
      paper/sections/intro.tex \
      paper/sections/background.tex \
      paper/sections/motivation.tex
    git diff --stat -- \
      paper/sections/intro.tex \
      paper/sections/background.tex \
      paper/sections/motivation.tex
    git diff -- \
      paper/sections/intro.tex \
      paper/sections/background.tex \
      paper/sections/motivation.tex

Expected:

- scoped diff check exits 0;
- only sentence-level prose, one Fig. 1 text label, and Table 1 column widths
  change;
- unrelated worktree edits remain untouched and are listed in the handoff.

---

## Author Review Gate

Execution stops after Task 5. Present the three-file diff, updated
paper/main.pdf, and page screenshots to the author. Do not enter Design and
do not commit until the author explicitly approves this front-matter revision.
