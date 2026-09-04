# FlashANNS Related Work and Citation Design Specification

**Status:** Approved and implemented  
**Date:** 2026-09-03  
**Scope:** Section 7, supporting citations elsewhere in the manuscript, the
bibliography, and removal of the Appendix from the compiled paper

## Objective

Rewrite Related Work as a compact, claim-driven ASPLOS comparison and raise the
number of distinct references actually cited by the manuscript to 45--55.  The
target is 48--52 verified references, leaving headroom for later additions or
removals.  Citation count alone is not a success criterion: every reference
must support a specific technical statement and have verified bibliographic
metadata.

The revision must preserve the paper's central boundary.  FlashANNS manages a
bounded CXL-DRAM-shaped window over a flash-authoritative corpus and separates
score hide from query hide.  Related systems must not be described as using
CXL-SSD, full-precision resident scoring, or continuous cross-query scheduling
unless their primary source establishes that fact.

## Selected Approach

Use a layered, argument-driven survey rather than concentrating 45 citations in
Section 7.  Related Work remains approximately 0.9--1.1 two-column pages,
including its comparison table.  Foundational citations are placed in
Background, Motivation, or Design when they directly support statements there;
Section 7 focuses on the closest systems and the distinguishing axes.

## Section 7 Narrative

Section 7 contains five paragraphs in this order.

1. **Graph-based ANNS foundations and in-memory indexes.** Establish the graph
   traversal lineage and distinguish graph construction/search algorithms from
   FlashANNS's storage-residency contribution.  Representative families include
   HNSW/NSG/Vamana-style indexes and optimized in-memory ANNS libraries.
2. **SSD-resident and distributed graph ANNS.** Compare DiskANN,
   FreshDiskANN, SPANN, PipeANN, and directly relevant distributed or remote
   graph-serving systems.  State whether navigation uses compressed vectors,
   where full-precision vectors reside, and whether the slow path is block I/O,
   a KV request, or another explicit request interface.
3. **Tiered, far-memory, and disaggregated ANNS.** Cover heterogeneous-memory,
   NVM, RDMA, and memory-tiering approaches.  The comparison must distinguish a
   far-load latency tier from tens-of-microseconds NAND behind a small window.
4. **CXL memory and memory-semantic SSD systems.** Separate full-corpus
   CXL-DRAM/near-memory ANNS from systems that expose flash through a CXL memory
   interface.  Hardware-medium papers do not automatically provide the
   graph-search residency policy or scheduling mechanisms contributed here.
5. **Prefetching, asynchronous I/O, and continuous serving.** Cite only the
   most relevant systems foundations for cache-aware prefetch, asynchronous
   request overlap, batching, and completion-driven scheduling.  This paragraph
   explains mechanism lineage without claiming that generic prefetch or batching
   solves dependency-constrained graph traversal.

Each paragraph follows the same compact logic: where the corpus resides, what
interface exposes the slow path, what latency the system is designed for, how
that latency is hidden, and the one material difference from FlashANNS.

## Comparison Table

Retain one double-column table because it lets a reviewer audit distinctions
faster than prose.  Remove all author-facing `Fill-in` language.  Rows represent
system families or the closest individual systems; citations stay in the row
labels or adjacent prose.

The table uses these reviewer-relevant columns:

- system or system family;
- full-precision vector home;
- graph/navigation home;
- slow-path interface;
- capacity assumption;
- primary latency-hiding mechanism;
- residency/cache unit.

Do not add a row unless it changes at least one comparison dimension.  Do not
use boldface to imply an unmeasured win; bold may identify FlashANNS only.

## Citation Portfolio

The final 45--55 distinct cited references should be balanced approximately as
follows.  Counts overlap when a paper genuinely spans categories.

| Category | Target distinct references |
|---|---:|
| Graph ANNS algorithms, construction, and libraries | 8--10 |
| SSD and billion-scale graph/vector search | 9--12 |
| Tiered, heterogeneous, NVM, and disaggregated ANNS | 7--9 |
| CXL memory, CXL-SSD, and near-memory ANNS | 8--10 |
| Prefetching, asynchronous I/O, batching, and serving | 6--8 |
| Specifications or empirical-method references | 2--4 |

The portfolio prioritizes peer-reviewed conference and journal publications.
ArXiv papers are permitted only when highly relevant and when no published
version supersedes them.  If a published version exists, cite that version and
do not retain the preprint as a second paper merely to increase the count.

## Per-Reference Verification Protocol

Every retained or added BibTeX entry must pass all checks below.

1. Confirm that the paper exists through a primary or authoritative record:
   publisher proceedings, official conference proceedings, DBLP, or the arXiv
   abstract page.  Search-result snippets and third-party citation aggregators
   are discovery aids, not final evidence.
2. Verify the exact title and complete ordered author list.  Preserve accents
   and name particles in BibTeX-safe form.
3. Verify publication type, venue, year, volume/issue where applicable, page
   range or article number, and DOI.  For arXiv, record the identifier, version
   date when relevant, and the absence of a later formal version.
4. Use a stable BibTeX key and a syntactically valid entry type.  Do not retain
   `Anonymous`, `Placeholder`, `Replace with`, guessed venues, or guessed years.
5. Read the abstract and the relevant method/evaluation passage before using the
   citation.  Record the manuscript claim it supports; title similarity is
   insufficient.
6. Cross-check high-impact comparison claims against the paper itself, not only
   its metadata record.  If full text is unavailable, weaken or omit the claim.

Create `docs/notes/2026-09-03-related-work-citation-audit.md` with one row per
reference: BibTeX key, canonical title, publication version, authoritative URL,
manuscript location, supported claim, and verification status.  A reference is
not eligible for the final count until its row is complete.

## Placement Rules Outside Section 7

Add citations outside Related Work only where an existing factual statement
needs support.  Do not restructure Abstract, Introduction, Background,
Motivation, or Design.  Minimal sentence edits are allowed to avoid citation
dumps, correct an inaccurate taxonomy, or narrow a claim after reading the
source.

- Background receives foundational ANNS algorithm/index citations.
- Motivation receives evidence about block SSD, far-memory, and CXL latency or
  capacity assumptions.
- Design receives only direct mechanism-lineage citations for prefetching,
  asynchronous completion, or batching.
- Abstract and Conclusion do not become bibliography surveys.

Use grouped citations only when all cited papers support the same clause.  A
group should normally contain no more than four references; larger families are
split by technical distinction.

## Appendix Removal

Remove `\appendix` and `\input{sections/appendix}` from `paper/main.tex` so the
Appendix is absent from the compiled manuscript.  Preserve
`paper/sections/appendix.tex` as a recoverable author artifact; do not delete the
file.

Any labels referenced solely by the removed Appendix must not create undefined
references.  Appendix-only author instructions must not leak into captions or
Related Work.

## Page and Style Constraints

- Related Work target: 0.9--1.1 compiled pages including its table.
- Prefer precise contrasts over chronological summaries.
- Avoid novelty claims based only on missing citations, such as unrestricted
  `first` statements.
- Avoid dismissive language such as `merely`, `just`, or `not a better X` unless
  followed by a technically exact distinction.
- Do not repeat Background's explanation of graph traversal or CXL protocols.
- Every system name and acronym must be introduced consistently with the rest
  of the manuscript.

## Validation and Exit Criteria

The revision is ready only when all conditions hold:

1. The manuscript contains 45--55 distinct citation keys actually referenced
   by `\cite{}`; the target is 48--52.
2. Every cited key has a complete audit row and an exact BibTeX entry verified
   against an authoritative source.
3. No cited item is nonexistent, duplicated under two versions, superseded by a
   formal version, or assigned an incorrect author list, venue, year, or format.
4. `latexmk -g -pdf -interaction=nonstopmode -halt-on-error main.tex` succeeds
   with no undefined citation or reference.
5. BibTeX reports no missing required fields for cited entries.  Any benign
   style warning is inspected and documented rather than ignored.
6. Related Work follows the five-paragraph taxonomy and every paragraph states
   a concrete FlashANNS distinction.
7. The comparison table contains no placeholder language, all factual fields
   are traceable to cited papers, and its columns fit without overfull text.
8. The Appendix is absent from the PDF, while its source file remains intact.
9. Visual inspection confirms legible citations/table text and a Related Work
   length of approximately 0.9--1.1 pages.
10. No unsupported result, product-CXL claim, or Shared-FAM measurement claim is
    introduced by the revision.
