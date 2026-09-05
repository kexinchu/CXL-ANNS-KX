# FlashANNS Related Work and Citation Revision Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a verified 45--55-reference manuscript, rewrite Section 7 as an ASPLOS-quality comparison, and remove the Appendix from the compiled paper.

**Architecture:** An audit ledger is the source of truth for citation eligibility. Only entries verified against authoritative metadata and the source's technical content enter `refs.bib`; manuscript citations are then placed by claim and Section 7 is organized by system boundary rather than chronology.

**Tech Stack:** LaTeX/acmart, BibTeX, DBLP, publisher proceedings, arXiv, `rg`, `bibtex`, `latexmk`, `pdftotext`.

**Spec:** `docs/superpowers/specs/2026-09-03-related-work-citation-design.md`

**Implementation status (2026-09-04):** Revalidated and extended to 51 cited
references. Section 7 follows the five-part taxonomy; the preprint records for
Cosmos and CMM-H were replaced by their formal publications, and two recent
hybrid-CXL studies were added. A later author instruction superseded the source-
preservation rule below and deletes the Appendix source as well as its include.

## Global Constraints

- The final manuscript must actually cite 45--55 distinct keys; target 48--52.
- Verify exact title, ordered authors, publication version, venue, year, pages/article number, and DOI or arXiv identifier.
- Prefer formal publications; retain arXiv only when highly relevant and not superseded.
- No `Anonymous`, `Placeholder`, `Replace with`, guessed metadata, or duplicate preprint/published versions.
- Keep Related Work at approximately 0.9--1.1 pages including its comparison table.
- Do not broaden FlashANNS's product-CXL, score-hide, query-hide, or Shared-FAM claims.
- Delete the Appendix source and remove it from the compiled manuscript, per the
  later author instruction.
- Do not commit automatically in the current dirty worktree.

---

### Task 1: Build and Verify the Citation Ledger

**Files:**
- Create: `docs/notes/2026-09-03-related-work-citation-audit.md`
- Read: `paper/refs.bib`, authoritative metadata pages, and source abstracts/full text

**Interfaces:**
- Consumes: current cited keys and the five-category taxonomy in the spec.
- Produces: 48--52 eligible rows with canonical metadata URL, supported claim, and placement.

- [x] Inventory every current entry and mark false, incomplete, duplicated, or superseded records for replacement.
- [x] Select candidates across graph ANNS, SSD ANNS, far/disaggregated memory, CXL/CXL-SSD, and asynchronous serving.
- [x] Verify metadata against DBLP plus a publisher/proceedings or arXiv primary record.
- [x] Read the abstract and relevant method passage; record only claims directly supported by the source.
- [x] Reject any candidate lacking authoritative metadata or sufficient technical evidence.

Verification:

```bash
rg '^\| `[^`]+` \|' docs/notes/2026-09-03-related-work-citation-audit.md | wc -l
```

Expected: 48--52 complete, verified rows.

### Task 2: Replace the Bibliography with Canonical Entries

**Files:**
- Modify: `paper/refs.bib`
- Read: `docs/notes/2026-09-03-related-work-citation-audit.md`

**Interfaces:**
- Consumes: eligible audit rows from Task 1.
- Produces: one canonical BibTeX entry per selected publication.

- [x] Replace placeholder current entries with verified formal versions.
- [x] Add verified candidate entries using stable lowercase keys.
- [x] Preserve complete ordered author lists and BibTeX-safe capitalization.
- [x] Remove duplicate preprint versions and entries not eligible for citation.
- [x] Parse the bibliography with BibTeX through a minimal manuscript build.

Verification:

```bash
cd paper && bibtex main
```

Expected: exit 0 and no missing author, title, venue, year, pages/article number, or publisher fields for cited entries.

### Task 3: Place Supporting Citations by Claim

**Files:**
- Modify: `paper/sections/background.tex`
- Modify: `paper/sections/motivation.tex`
- Modify: `paper/sections/design.tex`
- Modify only if necessary: `paper/sections/intro.tex`

**Interfaces:**
- Consumes: verified keys and supported-claim text from the ledger.
- Produces: citation-supported existing claims without structural rewrites.

- [x] Add graph/index lineage references to the clauses they support in Background.
- [x] Add storage, far-memory, and CXL evidence to matching Motivation clauses.
- [x] Add only direct prefetch/asynchrony/batching lineage to Design.
- [x] Keep citation groups at four or fewer unless one taxonomy sentence requires a larger family.
- [x] Do not add literature surveys to Abstract or Conclusion.

Verification:

```bash
rg -o '\\cite[a-zA-Z]*\{[^}]+\}' paper/sections/{intro,background,motivation,design}.tex
```

Expected: every new group supports the immediately preceding clause and contains no unrelated key.

### Task 4: Rewrite Section 7 and Its Comparison Table

**Files:**
- Modify: `paper/sections/related.tex`

**Interfaces:**
- Consumes: the five-category taxonomy and verified claim ledger.
- Produces: five compact comparison paragraphs plus one reviewer-auditable table.

- [x] Write the five paragraphs in the exact taxonomy order from the spec.
- [x] State corpus home, slow-path interface, delay regime, latency-hiding method, and FlashANNS distinction where applicable.
- [x] Replace dismissive or unsupported novelty wording with bounded technical comparisons.
- [x] Remove all author-facing placeholder instructions.
- [x] Populate table rows only for systems that change a comparison dimension.

Verification:

```bash
rg -n '^\\paragraph\{' paper/sections/related.tex
rg -n 'Fill-in|Placeholder|Replace with|Anonymous' paper/sections/related.tex
```

Expected: five paragraphs and zero placeholder matches.

### Task 5: Remove the Appendix

**Files:**
- Modify: `paper/main.tex`
- Delete: `paper/sections/appendix.tex`

**Interfaces:**
- Consumes: the current manuscript include order.
- Produces: a manuscript that ends after the bibliography.

- [x] Remove `\appendix` and `\input{sections/appendix}` from `paper/main.tex`.
- [x] Delete `paper/sections/appendix.tex` under the later author instruction.
- [x] Check that no remaining text references appendix-only labels.

Verification:

```bash
test ! -e paper/sections/appendix.tex
! rg -n '^\\appendix|sections/appendix' paper/main.tex
```

Expected: both commands succeed.

### Task 6: Audit Counts, Compile, and Inspect the PDF

**Files:**
- Verify: `paper/main.tex`, `paper/main.log`, `paper/main.pdf`
- Update if corrections are required: all files from Tasks 1--5

**Interfaces:**
- Consumes: the revised manuscript and bibliography.
- Produces: the final citation-count, compilation, warning, and page-layout evidence.

- [x] Count unique keys actually present in manuscript citation commands; require 45--55.
- [x] Confirm every cited key exists in `refs.bib` and every eligible ledger key matches its entry.
- [x] Force a full LaTeX/BibTeX rebuild and reject undefined citations/references.
- [x] Inspect BibTeX warnings individually; correct missing or malformed fields.
- [x] Check `git diff --check` on all touched files.
- [x] Render the Related Work pages and visually check table legibility and 0.9--1.1-page length.
- [x] Confirm the Appendix heading and content are absent from extracted PDF text.

Verification:

```bash
cd paper && latexmk -g -pdf -interaction=nonstopmode -halt-on-error main.tex
```

Expected: exit 0, 45--55 distinct cited keys, no undefined citation/reference, no Related Work overfull text, and no compiled Appendix.
