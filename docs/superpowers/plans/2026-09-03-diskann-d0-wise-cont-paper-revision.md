# FlashANNS D0/Wise/Continuous Paper Revision Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Align the Abstract, Introduction, Background, Motivation, and Design with the final three-tier hardware contract: a small host-side entry selector, a unified DiskANN-record corpus on CXL-SSD, a bounded CXL-DRAM resident window for Hide, and a full-corpus CXL-DRAM Oracle mode.

**Architecture:** Oracle and Hide use the same `id -> HEADER + id * STRIDE` image but are separate execution modes. Hide keeps the authoritative corpus on CXL-SSD and promotes complete entry pages into a bounded CXL-DRAM window; Oracle maps the complete corpus in CXL-DRAM and performs no NAND I/O. The public design has one prerequisite (D0 placement) and two mechanisms (D1 wise prefetching and D2 continuous batching).

**Tech Stack:** English LaTeX (`paper/main.tex`, `paper/sections/*.tex`), TikZ, acmart, `latexmk`, `rg`, `pdftocairo`, `pdftotext`, `sha256sum`, `git diff`.

**Spec:** `docs/superpowers/specs/2026-09-03-cxl-dram-diskann-layout-design.md`

## Global Constraints

- Assume the final hardware exposes a working CXL-DRAM data path and a working CXL-SSD-to-window fill path; do not discuss current device repair or bring-up failures in these sections.
- Oracle and Hide are mutually exclusive timed modes, not two simultaneously active full-corpus copies in the production architecture.
- In Hide mode, CXL-SSD is the authoritative capacity tier and CXL-DRAM contains only a bounded dynamic resident window.
- In Oracle mode, the whole fixed-record corpus is in CXL-DRAM and NAND traffic is zero.
- Host memory contains query state and a 10k sampled entry selector, not the 10M graph.
- The 10k selector performs an exact scan over sampled vectors. It may retain the common record format, but the paper must not claim that the copied neighbor IDs form or traverse an induced graph.
- The full-corpus record is fixed: `float32 vec[200]` (800 B), `uint32 nnbrs` (4 B), `uint32 nbrs[32]` (128 B), then padding to `STRIDE=2048`.
- A 4 KiB fill contains two complete 2048 B entries. Do not restore the old split graph/vector pagebin or the 267 GiB expand-bundle as the primary design.
- Score hide means every distance calculation consumes a resident entry. Query hide remains distinct: a query may still wait for a required fill.
- D1 never drops a current-hop neighbor required by the graph algorithm. A byte budget limits outstanding work and optional lookahead; mandatory pages are issued in waves.
- D2 does not alter the graph algorithm, beam, visited set, or recall semantics; it only interleaves page fills and ready scoring work across queries.
- Entry pinning is a small initialization hint. LFU, a beam controller, and observability are not public design modules.
- Preserve the four Introduction contributions and the C1/C2/C3 ordering.
- Preserve the locked results `1.64`, `50.25`, `85.8`, `0.59x`, and `from_win=100%` until a separately approved result-refresh task replaces them from validated final-hardware runs.
- Use `in-window oracle` for the locked 85.8 QPS baseline and `full-corpus CXL-DRAM Oracle` for the new D0 execution mode. Do not present 85.8 as a measurement of the new mode.
- Do not modify Motivation Figure 3 or Figure 4 sources, captions, scripts, or PDF artifacts.
- Do not modify Implementation, Evaluation, Related Work, Discussion, or Conclusion in this plan.
- Do not commit automatically. The worktree contains author changes; stop for author review with a scoped diff.

---

### Task 1: Freeze the Story, Structure, and Figure Baseline

**Files:**
- Read: `paper/main.tex`
- Read: `paper/sections/intro.tex`
- Read: `paper/sections/background.tex`
- Read: `paper/sections/motivation.tex`
- Read: `paper/sections/design.tex`
- Read: `docs/superpowers/specs/2026-09-03-cxl-dram-diskann-layout-design.md`
- Do not modify repository files

**Interfaces:**
- Consumes: the current section topology, labels, locked result strings, and Motivation figure artifacts.
- Produces: `/tmp` baselines used by Tasks 2--8.

- [ ] **Step 1: Record the section and subsection topology**

Run:

```bash
rg --no-line-number '^\\(section|subsection|paragraph)\{' \
  paper/sections/intro.tex \
  paper/sections/background.tex \
  paper/sections/motivation.tex \
  paper/sections/design.tex \
  > /tmp/flashanns-d012-headings.before
```

Expected: Introduction has one section; Background and Motivation each retain four subsections; Design still exposes the old D1--D5 structure before revision.

- [ ] **Step 2: Record protected Motivation figure hashes**

Run:

```bash
sha256sum \
  paper/figs/mot-cliff.pdf \
  paper/figs/mot-pathology-admission.pdf \
  paper/figs/mot-pathology-prefetch.pdf \
  > /tmp/flashanns-d012-motivation-figures.before.sha256
```

Expected: all three files hash successfully.

- [ ] **Step 3: Record the locked claims and current pagination**

Run:

```bash
rg -n '1\.64|50\.25|85\.8|0\.59|from\\_win' \
  paper/main.tex paper/sections/intro.tex \
  > /tmp/flashanns-d012-locked-claims.before
make -C paper
pdfinfo paper/main.pdf | rg '^(Pages|Page size)'
```

Expected: all five locked values are present and the paper builds as a 12-page letter-size PDF.

---

### Task 2: Lock the Final D0 Vocabulary in the Hardware Spec

**Files:**
- Modify: `docs/superpowers/specs/2026-09-03-cxl-dram-diskann-layout-design.md:8-82`

**Interfaces:**
- Consumes: the implemented `NavGraph::search_entry()` behavior and the final hardware assumption.
- Produces: the vocabulary all paper sections use: `entry selector`, `fixed-record corpus`, `resident window`, `Oracle mode`, and `Hide mode`.

- [ ] **Step 1: Replace “guide graph search” with the exact selector contract**

Use this contract in the spec:

```text
Host entry selector G0: uniformly sample 10,000 global IDs with seed 42.
For each query, exact-scan the sampled vectors and return one global entry ID.
The stored records may retain vec+nbrs for format reuse, but entry selection
does not traverse an induced graph and does not use L0.
```

Remove claims that G0 runs greedy/beam search or returns several seeds.

- [ ] **Step 2: Make Oracle and Hide explicitly exclusive modes**

Replace the shared CXL-DRAM role with two rows:

```text
Oracle mode: CXL-DRAM contains all 10M fixed records; CXL-SSD is outside the timed path.
Hide mode: CXL-SSD contains all 10M fixed records; CXL-DRAM contains only the bounded resident window.
```

Keep the same logical image and ID-to-byte mapping in both modes.

- [ ] **Step 3: Replace “hot subgraph” with “resident entry pages”**

State that the window holds dynamic 4 KiB pages selected by query demand and prefetch. Its contents need not form a connected graph and are not a separately built index.

- [ ] **Step 4: Check the spec for stale D0 terms**

Run:

```bash
rg -n 'greedy/beam|induced|several global|hot subgraph|both copies active' \
  docs/superpowers/specs/2026-09-03-cxl-dram-diskann-layout-design.md
```

Expected: no positive design claim uses those terms. Historical or explicitly rejected alternatives may remain only if marked as rejected.

---

### Task 3: Minimally Align the Abstract and Introduction

**Files:**
- Modify: `paper/main.tex:37-50`
- Modify: `paper/sections/intro.tex:52-89`
- Modify: `paper/sections/intro.tex:108-116`
- Test: `paper/main.pdf`, pages 1--2

**Interfaces:**
- Consumes: Task 2 vocabulary.
- Produces: the architecture promise that Background defines and Design implements.

- [ ] **Step 1: Replace the Abstract placement sentence**

Replace the sentence beginning `Graph and PQ live in host DRAM` with:

```latex
The host keeps query state and a 10k sampled entry selector; the complete graph
and vectors share fixed-size records on CXL-SSD.  A bounded CXL-DRAM window
admits entries required by the current expand or a bounded soon-to-expand
frontier, while fills are issued asynchronously across queries.
```

Do not change the surrounding score-hide/query-hide definitions or locked result sentence.
Keep the locked baseline named `in-window oracle`; do not rename it to the
`full-corpus CXL-DRAM Oracle` introduced by D0.

- [ ] **Step 2: Retitle only the contents of Figure 1(c) boxes**

Keep all TikZ coordinates, styles, arrows, widths, and the three-panel geometry unchanged. Use:

```text
Host CPU + query state
10k entry selector G0

CXL-DRAM resident window (bounded)
score + expand only resident entries

CXL-SSD NAND: full graph + vectors G
fixed records; miss requires a fill
```

Keep the solid downward `score` edge and dashed upward `fill` edge. Update the caption to say that full-corpus CXL-DRAM is an evaluation Oracle mode, not a second active capacity copy in panel (c).

- [ ] **Step 3: Replace the old host-adjacency premise**

Replace the current placement sentence with:

```latex
A placement premise comes first: the host keeps only query state and a small
entry selector; each full-corpus record co-locates a vector and its neighbor
IDs on CXL-SSD.  Once promoted, that complete entry is read only from the
bounded CXL-DRAM window for both scoring and expansion.
```

- [ ] **Step 4: Remove the old bundle/pagebin mechanism from the positive design preview**

Replace the sentence that advertises `expand-bundle / page-bin` with:

```latex
It issues asynchronous fills only for entries required by the current expand
or a bounded soon-to-expand frontier, deduplicates them at 4 KB granularity,
and limits outstanding and retained bytes separately.
```

The Motivation evidence about coarse admission remains; only the shipped primary layout changes.

- [ ] **Step 5: Align contribution 3 with unified resident entries**

Keep the contribution title and locked score/query-hide sentence. Replace
`Placement keeps neighbor IDs off the window` with:

```latex
Fixed records expose a node's vector and neighbor IDs together once resident;
the prefetcher admits only current or bounded soon-to-expand entry pages, and
batching holds device depth across walks.
```

- [ ] **Step 6: Verify Introduction invariants and layout**

Run:

```bash
git diff --check -- paper/main.tex paper/sections/intro.tex
test "$(rg -c '^  \\item' paper/sections/intro.tex)" -eq 4
rg -n '1\.64|50\.25|85\.8|0\.59|from\\_win' paper/main.tex paper/sections/intro.tex
if rg -n 'Graph and PQ live in host DRAM|Host CPU \+ graph / PQ|adjacency list.*live in host DRAM|expand-bundle / page-bin|neighbor IDs off the window' \
  paper/main.tex paper/sections/intro.tex; then exit 1; fi
make -C paper
pdftocairo -png -f 1 -l 2 -r 150 paper/main.pdf /tmp/flashanns-d012-intro
```

Expected: build succeeds, four contributions remain, locked values remain, and Figure 1 retains its original geometry.

---

### Task 4: Repair Only the D0-Dependent Background and Motivation Sentences

**Files:**
- Modify: `paper/sections/background.tex:29-36`
- Modify: `paper/sections/background.tex:93-126`
- Modify: `paper/sections/motivation.tex:145-160`
- Do not modify: any Motivation figure environment or caption

**Interfaces:**
- Consumes: the revised placement promise from Task 3.
- Produces: C1--C3 premises that no longer require the 10M adjacency list in host DRAM.

- [ ] **Step 1: Keep the Background separation fact generic**

Replace the statement that the adjacency list can sit in host DRAM with:

```latex
Vector and adjacency placement is a system choice: separating them can avoid
metadata misses, while a fixed-record layout can fetch both with one resident
entry.  Either choice must preserve the hop dependency above.
```

This remains background; do not announce FlashANNS mechanisms in Section 2.1.

- [ ] **Step 2: Update the latency table’s host row**

Change `graph / PQ metadata` to `entry selector / query state`. Keep the other rows, dimensions, caption, and label unchanged.

- [ ] **Step 3: Define a resident object as a complete entry**

In `Flash-backed CXL memory`, add one sentence after the hit/miss definition:

```latex
In our fixed-record model, a resident page exposes both a node vector and its
neighbor IDs; a miss promotes the complete 4 KB page before either field is
used by the timed walk.
```

Keep the page-bounded admission discussion and the distinction between issued and completed prefetch.

- [ ] **Step 4: Replace the Motivation host-neighbor premise**

Replace lines 158--160 with:

```latex
Co-locating vectors and neighbor IDs changes the premise, not the challenge:
the entry selected for expansion must already be resident, after which its
neighbor IDs can drive prefetch without a second metadata miss.
```

- [ ] **Step 5: Replace the obsolete software-proxy identity in Background**

Replace the paragraph beginning `The prototype we measure is a software
memory-semantic node` with:

```latex
The final hardware path maps the fixed-record corpus through CXL-SSD and the
bounded resident window through CXL-DRAM.  The full-corpus CXL-DRAM Oracle uses
the same logical image but removes CXL-SSD from the timed path.  Section~\ref{sec:impl}
records the concrete device identities and software interfaces.
```

Do not add bandwidth, product-model, or performance claims to Background.

- [ ] **Step 6: Prove protected figures did not change**

Run:

```bash
sha256sum -c /tmp/flashanns-d012-motivation-figures.before.sha256
git diff --check -- paper/sections/background.tex paper/sections/motivation.tex
if rg -n 'Placement of neighbor IDs in host DRAM is a premise' paper/sections/motivation.tex; then exit 1; fi
make -C paper
```

Expected: all protected hashes pass and no locked hide result is introduced into Motivation.

---

### Task 5: Rewrite the Design Opening and D0 Placement

**Files:**
- Modify: `paper/sections/design.tex:1-66`
- Create: `paper/sections/fig_design_arch.tex`
- Preserve labels: `sec:design`, `fig:design-arch`, `tab:placement`

**Interfaces:**
- Consumes: `entry_id`, `STRIDE=2048`, 4 KiB pages, and the two execution modes.
- Produces: the architectural source of truth for D1 and D2.

- [ ] **Step 1: Replace the Design opening with the split-mode contract**

The opening must state, in this order:

1. FlashANNS is a host runtime plus a residency plane, not a new graph index.
2. Host memory contains G0 and per-query state only.
3. Hide maps the full corpus from CXL-SSD and scores/expands only resident CXL-DRAM entries.
4. Oracle maps the same complete image in CXL-DRAM and disables NAND fills.
5. Score hide is enforced; query hide remains limited by hop-to-hop dependency.

Do not include performance numbers in this opening.

- [ ] **Step 2: Replace the architecture placeholder with a real TikZ figure**

Create `fig_design_arch.tex` with one shared Host box and two clearly separated paths:

```text
Host: query + beam + visited + G0 exact selector

Hide mode:
CXL-SSD full G --4 KB async fill--> CXL-DRAM bounded window --resident read--> score/expand

Oracle mode (dashed evaluation path):
CXL-DRAM full G -----------------------------------------------> score/expand
```

Annotate both storage boxes with `same fixed-record layout`. Do not draw a full CXL-DRAM corpus inside the Hide path. Use different line styles for control, fill, and resident reads. Include the file with `\input{sections/fig_design_arch}` and retain `\label{fig:design-arch}` in `design.tex`.

- [ ] **Step 3: Rename the first subsection to D0**

Use:

```latex
\subsection{D0: Minimal host and unified records}
\label{sec:d0}
```

Explain the 10k exact selector, global IDs, 932 B payload, 2048 B stride, and two records per 4 KiB page. State that G0 search uses vector fields only even if the serialized sample retains the common record shape.

- [ ] **Step 4: Rewrite the placement table**

Keep `tab:placement` and use exactly these conceptual rows:

| Object | Hide mode | Oracle mode |
|---|---|---|
| Query/beam/visited | Host DRAM | Host DRAM |
| G0 10k selector | Host DRAM | Host DRAM |
| Complete G | CXL-SSD | CXL-DRAM |
| Resident entry window | bounded CXL-DRAM | not used |
| Fill source | CXL-SSD NAND | none |

The caption must state that the modes share a logical layout but not a timed data path.

- [ ] **Step 5: Compile and visually inspect the architecture**

Run:

```bash
git diff --check -- paper/sections/design.tex paper/sections/fig_design_arch.tex
make -C paper
pdftocairo -png -f 5 -l 7 -r 150 paper/main.pdf /tmp/flashanns-d012-d0
```

Expected: the figure fits across two columns, Oracle is visibly alternate/dashed, and the Hide path has only a bounded CXL-DRAM window.

---

### Task 6: Consolidate the Wise Prefetcher into D1

**Files:**
- Modify: `paper/sections/design.tex` after `sec:d0`
- Create: `paper/sections/fig_design_prefetch.tex`
- Preserve label: `fig:design-promote`
- Remove positive module labels: `sec:d3`, `sec:d4` for prefetch content

**Interfaces:**
- Consumes: `entry(u)`, `page(id)`, resident-state lookup, per-query byte budget, and fill completion.
- Produces: the only path that changes an entry from absent to scoreable.

- [ ] **Step 1: Create the D1 subsection**

Use:

```latex
\subsection{D1: Frontier-directed prefetch and bounded residency}
\label{sec:d1}
```

Organize the prose under four paragraph leads: `Whom`, `What`, `How much`, and `When score may run`.

- [ ] **Step 2: Specify mandatory versus optional pages**

State this algorithm precisely:

```text
u = best unexpanded candidate whose entry is resident
need = unseen neighbors stored in entry(u)
mandatory = deduplicate(page(v) for v in need)
lookahead = pages from N(v) for at most top-M resident, unexpanded candidates
issue mandatory first; issue lookahead only within the remaining byte budget
if mandatory exceeds the budget, issue it in waves rather than dropping nodes
score v only after page(v) reaches RESIDENT
```

This distinction is required to preserve the graph walk and recall semantics.

- [ ] **Step 3: Define the residency state machine**

Use four states:

```text
ABSENT -> FILLING -> RESIDENT -> EVICTABLE
```

Define page-offset deduplication, completion-driven installation, generation/TTL protection against stale slots, and a small entry pin. `install_top` controls retention after use; it never controls whether a mandatory neighbor is scored.

- [ ] **Step 4: State the score-hide gate explicitly**

The scoring function may read only a `RESIDENT` entry. A miss may suspend the query, but it must not fall through to a CXL-SSD load or score from a temporary bounce buffer. This is score hide; eliminating the suspension is query hide and belongs to D2.

- [ ] **Step 5: Replace the promotion placeholder with a real flow figure**

Create `fig_design_prefetch.tex` with:

```text
resident entry(u)
  -> read N(u)
  -> classify resident/missing pages
  -> mandatory queue + bounded lookahead queue
  -> 4 KB dedup/budget
  -> async fill
  -> FILLING/RESIDENT completion
  -> score and candidate insertion
```

Visually separate mandatory current-hop work from optional frontier lookahead. Retain `\label{fig:design-promote}`.

- [ ] **Step 6: Remove obsolete positive mechanisms**

Delete or demote these claims from Design:

- the full graph and PQ codebook live in host DRAM;
- the 267 GiB expand-bundle is the shipped layout;
- page occupants are scored merely to increase utilization;
- LFU/shared hot set is a D3 module;
- a beam-width controller is a design module.

Keep one negative paragraph saying that blind multi-root/two-hop coverage, install-all, score-page, and LFU are rejected by Motivation evidence.

- [ ] **Step 7: Compile and check terminology**

Run:

```bash
git diff --check -- paper/sections/design.tex paper/sections/fig_design_prefetch.tex
if rg -n 'D3:|expand-bundle|score unused|LFU.*module|graph adjacency.*host DRAM' paper/sections/design.tex; then exit 1; fi
make -C paper
```

Expected: Design exposes D0 and D1, and no positive third prefetch/residency module remains.

---

### Task 7: Define Continuous Batching as D2

**Files:**
- Modify: `paper/sections/design.tex`
- Create: `paper/sections/fig_design_cont.tex`
- Preserve label: `fig:design-pipe`

**Interfaces:**
- Consumes: per-query traversal state and D1 page requests/completions.
- Produces: cross-query work-conserving issue without changing search semantics.

- [ ] **Step 1: Create the D2 subsection**

Use:

```latex
\subsection{D2: Continuous batching across query stalls}
\label{sec:d2}
```

Define four query states: `READY_EXPAND`, `WAIT_FILL`, `READY_SCORE`, and `DONE`.

- [ ] **Step 2: Define scheduler behavior**

Specify these rules:

1. Run a `READY_EXPAND` query until it emits mandatory and optional page requests.
2. Move it to `WAIT_FILL` only for its mandatory pages.
3. Submit mandatory requests ahead of lookahead requests.
4. Deduplicate identical 4 KiB offsets across all in-flight queries.
5. On completion, install the page once and wake every covered query.
6. Run `READY_SCORE` work before issuing additional optional lookahead.
7. Use round-robin or age ordering among ready queries so a hot query cannot starve others.
8. Stop issuing optional work when the bounded window lacks eviction headroom.

- [ ] **Step 3: Bound the claim to work-conserving issue**

State that continuous batching keeps useful fill opportunities available across pointer-chasing gaps. Do not claim a closed-loop target-QD controller, full single-query query hide, or a particular bandwidth unless those are separately implemented and measured.

- [ ] **Step 4: Explain correctness**

Prefetch timing cannot add candidates, mark nodes visited, or change distances. Only the normal graph-walk logic mutates `candidate`, `expanded`, and `visited`. Therefore the same query, entry selector, `L`, and hop budget must return the same IDs independent of D2 scheduling.

- [ ] **Step 5: Replace the pipeline placeholder**

Create `fig_design_cont.tex` with three query lanes, one shared mandatory fill queue, one lower-priority lookahead queue, a completion fan-out, and a score lane. Show that Q1/Q2 run while Q0 is in `WAIT_FILL`; do not redraw the Motivation failure timelines. Retain `\label{fig:design-pipe}`.

- [ ] **Step 6: Compile and inspect the timeline**

Run:

```bash
git diff --check -- paper/sections/design.tex paper/sections/fig_design_cont.tex
make -C paper
pdftocairo -png -f 5 -l 8 -r 150 paper/main.pdf /tmp/flashanns-d012-cont
```

Expected: D2 is visibly cross-query, mandatory and optional fills have distinct priority, and no static group barrier appears in the successful path.

---

### Task 8: Demote Non-Modules and Add the Validation Contract

**Files:**
- Modify: `paper/sections/design.tex`

**Interfaces:**
- Consumes: D0/D1/D2 claims.
- Produces: falsifiable counters for later Implementation and Evaluation revisions.

- [ ] **Step 1: Replace D3 and D5 with two short paragraphs**

Use paragraph leads rather than numbered design modules:

```latex
\paragraph{What is deliberately absent.}
```

Mention that a small entry pin is initialization, while LFU, install-all, score-page, and blind two-hop coverage are rejected policies.

```latex
\paragraph{Validation contract.}
```

Require `score_from_window`, `score_from_bounce`, `crit_wait_ns`, mandatory/optional issued pages, useful pages, promotion precision, NAND bytes, and iso-recall QPS.

- [ ] **Step 2: Tie every mechanism back to one challenge**

End Design with exactly this mapping:

```text
C1 -> resident-only score gate
C2 -> D1 frontier-directed, bounded prefetch
C3 -> D2 cross-query continuous batching
D0 -> prerequisite shared by D1 and D2
```

Do not introduce D3, D4, or D5 in the contribution vocabulary.

- [ ] **Step 3: Remove all figure placeholders and fill-in markers**

Run:

```bash
if rg -n '\\phbox|placeholder|Fill-in:|D3:|D4:|D5:' paper/sections/design.tex; then exit 1; fi
```

Expected: no matches.

---

### Task 9: Cross-Section Consistency and Final Paper Gate

**Files:**
- Verify: `paper/main.tex`
- Verify: `paper/sections/intro.tex`
- Verify: `paper/sections/background.tex`
- Verify: `paper/sections/motivation.tex`
- Verify: `paper/sections/design.tex`
- Verify: `paper/sections/fig_design_arch.tex`
- Verify: `paper/sections/fig_design_prefetch.tex`
- Verify: `paper/sections/fig_design_cont.tex`

**Interfaces:**
- Consumes: all prior tasks.
- Produces: an author-reviewable scoped diff that stops before Implementation.

- [ ] **Step 1: Verify protected structures and results**

Run:

```bash
test "$(rg -c '^  \\item' paper/sections/intro.tex)" -eq 4
rg -n '1\.64|50\.25|85\.8|0\.59|from\\_win' paper/main.tex paper/sections/intro.tex
if rg -n '50\.25|85\.8|0\.59|from_win|from\\_win' paper/sections/motivation.tex; then exit 1; fi
sha256sum -c /tmp/flashanns-d012-motivation-figures.before.sha256
```

Expected: four contributions, all locked values in Abstract/Introduction, none of the hide-result values in Motivation, and all protected figures unchanged.

- [ ] **Step 2: Run the architecture contradiction sweep**

Run:

```bash
if rg -n \
  -e 'Graph and PQ live in host DRAM' \
  -e 'Host CPU \+ graph / PQ' \
  -e 'Placement of neighbor IDs in host DRAM is a premise' \
  -e 'full graph.*host DRAM' \
  -e 'CXL-DRAM.*full.*Hide mode' \
  paper/main.tex paper/sections/{intro,background,motivation,design}.tex; then exit 1; fi
```

Expected: no matches.

- [ ] **Step 3: Verify the required positive contracts**

Run:

```bash
rg -n '10k|entry selector|fixed-record|fixed-size record|bounded.*window|Oracle mode|Hide mode|mandatory|lookahead|READY_EXPAND|WAIT_FILL|READY_SCORE' \
  paper/main.tex paper/sections/{intro,background,motivation,design}.tex
```

Expected: each concept appears where introduced and is not duplicated excessively across sections.

- [ ] **Step 4: Run a forced clean semantic build**

Run:

```bash
git diff --check -- \
  paper/main.tex \
  paper/sections/intro.tex \
  paper/sections/background.tex \
  paper/sections/motivation.tex \
  paper/sections/design.tex \
  paper/sections/fig_design_arch.tex \
  paper/sections/fig_design_prefetch.tex \
  paper/sections/fig_design_cont.tex
cd paper
latexmk -g -pdf -interaction=nonstopmode -halt-on-error main.tex
pdfinfo main.pdf | rg '^(Pages|Page size)'
if rg -n 'undefined references|Citation .* undefined|Reference .* undefined' main.log; then exit 1; fi
```

Expected: build exits zero, page size remains letter, and no citations or references are undefined. A page-count change must be reported rather than silently accepted.

- [ ] **Step 5: Visually inspect the narrative handoff**

Run:

```bash
pdftocairo -png -f 1 -l 8 -r 150 main.pdf /tmp/flashanns-d012-final
```

Inspect pages 1--8 for:

- Figure 1 preserves its three-way deployment comparison;
- Motivation Figures 3 and 4 are unchanged;
- Design architecture separates Oracle and Hide modes;
- D1 shows mandatory versus optional fills;
- D2 shows cross-query interleaving;
- tables remain within their columns;
- Design remains within its 2.4--2.6 page budget.

- [ ] **Step 6: Stop for author review**

Report:

```bash
git diff --stat -- \
  docs/superpowers/specs/2026-09-03-cxl-dram-diskann-layout-design.md \
  paper/main.tex paper/sections/intro.tex paper/sections/background.tex \
  paper/sections/motivation.tex paper/sections/design.tex \
  paper/sections/fig_design_arch.tex paper/sections/fig_design_prefetch.tex \
  paper/sections/fig_design_cont.tex
```

Do not edit Implementation or later sections and do not commit. Ask the author to approve the revised frontmatter/Design contract before beginning the Implementation and Evaluation alignment plan.
