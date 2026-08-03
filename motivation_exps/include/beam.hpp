#pragma once
#include "common.hpp"
#include "dataset.hpp"
#include "store.hpp"

#include <algorithm>
#include <queue>
#include <unordered_set>
#include <vector>

struct Cand {
  float dist;
  uint32_t id;
  bool operator<(const Cand& o) const { return dist < o.dist; }  // max-heap via greater
};

// Min-dist first for exploration; use greater for priority_queue top = worst among best.
struct ByDistGreat {
  bool operator()(const Cand& a, const Cand& b) const { return a.dist > b.dist; }
};

inline std::vector<uint32_t> make_queries(uint32_t n, uint32_t nq, uint64_t seed,
                                          bool zipf) {
  std::mt19937_64 rng(seed);
  std::vector<uint32_t> q(nq);
  if (!zipf) {
    std::uniform_int_distribution<uint32_t> u(0, n - 1);
    for (uint32_t i = 0; i < nq; ++i) q[i] = u(rng);
    return q;
  }
  // Simple zipf-like: prefer IDs near meta entry region via geometric.
  std::geometric_distribution<int> geo(0.002);
  for (uint32_t i = 0; i < nq; ++i) {
    int off = geo(rng);
    uint32_t id = uint32_t((n / 2 + off) % n);
    q[i] = id;
  }
  return q;
}

// Fetch policy interface used by beam search.
struct FetchCtx {
  Graph* g = nullptr;
  VectorStoreSSD* ssd = nullptr;
  RecordCache* rec = nullptr;
  PageCache* page = nullptr;
  Stats* st = nullptr;
  // Mode flags
  enum Mode {
    EXPLICIT_SSD,      // always SSD pread into temp (no CXL cache)
    SYNC_PTR_FAULT,    // miss: sync promote 1 vector then continue (F1 false friend)
    BATCH_PROMOTE,     // miss: collect, batch pread into CXL, then compute (F1 good)
    PAGE_CACHE,        // F2: 4K page cache in CXL
    RECORD_CACHE,      // F2: record cache in CXL
  } mode = EXPLICIT_SSD;

  std::vector<float> tmp;  // dim
  std::vector<uint32_t> batch_miss;

  void ensure_tmp(uint32_t dim) { tmp.resize(dim); }

  // Ensure vector `id` is available; returns pointer valid until next conflicting op.
  const float* fetch(uint32_t id) {
    st->vector_fetches++;
    switch (mode) {
      case EXPLICIT_SSD: {
        ssd->pread_vec(id, tmp.data(), *st);
        return tmp.data();
      }
      case SYNC_PTR_FAULT:
      case RECORD_CACHE:
      case BATCH_PROMOTE: {
        if (float* p = rec->find(id)) {
          st->cxl_hits++;
          return p;
        }
        st->cxl_misses++;
        if (mode == BATCH_PROMOTE) {
          // Caller should use prefetch_batch; fallback sync:
          float* slot = rec->insert_slot(id);
          ssd->pread_vec(id, slot, *st);
          return slot;
        }
        // SYNC_PTR_FAULT / RECORD_CACHE miss path: sync promote one
        float* slot = rec->insert_slot(id);
        ssd->pread_vec(id, slot, *st);
        return slot;
      }
      case PAGE_CACHE: {
        size_t vb = ssd->vec_bytes;
        uint64_t byte_off = uint64_t(id) * vb;
        uint64_t p0 = byte_off / PageCache::kPage;
        uint64_t p1 = (byte_off + vb - 1) / PageCache::kPage;
        bool all_hit = true;
        for (uint64_t p = p0; p <= p1; ++p) {
          if (!page->find_page(p)) {
            all_hit = false;
            break;
          }
        }
        if (all_hit) {
          st->cxl_hits++;
          st->page_hits++;
        } else {
          st->cxl_misses++;
          st->page_misses++;
          for (uint64_t p = p0; p <= p1; ++p) {
            if (page->find_page(p)) {
              st->page_hits++;
              continue;
            }
            st->page_misses++;
            char* slot = page->insert_page(p);
            // read page from SSD
            AlignedBuf buf(PageCache::kPage);
            ssize_t r = ::pread(ssd->fd, buf.p, PageCache::kPage,
                               off_t(p * PageCache::kPage));
            if (r <= 0) {
              // last partial page
              std::memset(slot, 0, PageCache::kPage);
            } else {
              std::memcpy(slot, buf.p, size_t(r));
              if (size_t(r) < PageCache::kPage)
                std::memset(slot + r, 0, PageCache::kPage - size_t(r));
            }
            st->ssd_reads++;
            st->ssd_bytes += PageCache::kPage;
          }
        }
        {
          char* dst = reinterpret_cast<char*>(tmp.data());
          size_t rem = vb;
          uint64_t c = byte_off;
          size_t o = 0;
          while (rem) {
            uint64_t p = c / PageCache::kPage;
            size_t po = size_t(c % PageCache::kPage);
            char* pg = page->find_page(p);
            if (!pg) die("page missing after fill");
            size_t n = std::min(rem, PageCache::kPage - po);
            std::memcpy(dst + o, pg + po, n);
            rem -= n;
            c += n;
            o += n;
          }
        }
        return tmp.data();
      }
    }
    return tmp.data();
  }

  // For BATCH_PROMOTE: promote misses with concurrent SSD reads.
  void promote_batch(const std::vector<uint32_t>& ids) {
    batch_miss.clear();
    for (uint32_t id : ids) {
      if (!rec->contains(id)) batch_miss.push_back(id);
    }
    if (batch_miss.empty()) return;
    // Parallel pread into cache slots (models batched/async I/O vs serial faults).
    std::vector<float*> slots(batch_miss.size());
    for (size_t i = 0; i < batch_miss.size(); ++i)
      slots[i] = rec->insert_slot(batch_miss[i]);
#pragma omp parallel for schedule(static) if (batch_miss.size() > 4)
    for (int i = 0; i < (int)batch_miss.size(); ++i) {
      Stats local{};
      ssd->pread_vec(batch_miss[size_t(i)], slots[size_t(i)], local);
#pragma omp atomic
      st->ssd_reads += local.ssd_reads;
#pragma omp atomic
      st->ssd_bytes += local.ssd_bytes;
    }
  }
};

inline void beam_search_one(FetchCtx& ctx, const float* query, uint32_t entry,
                            int beam, int hops_limit, std::vector<uint32_t>* visited_out) {
  Graph& g = *ctx.g;
  Stats& st = *ctx.st;
  const uint32_t dim = g.meta.dim;

  std::priority_queue<Cand> best;  // max-heap by dist (keep beam smallest)
  std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
  std::unordered_set<uint32_t> visited;
  visited.reserve(size_t(beam) * 8);

  auto consider = [&](uint32_t id, float dist) {
    if (visited.count(id)) return;
    visited.insert(id);
    frontier.push({dist, id});
    best.push({dist, id});
    if ((int)best.size() > beam) best.pop();
  };

  // seed
  if (ctx.mode == FetchCtx::BATCH_PROMOTE) {
    ctx.promote_batch({entry});
  }
  const float* ev = ctx.fetch(entry);
  consider(entry, l2_sq(query, ev, dim));

  int hops = 0;
  while (!frontier.empty() && hops < hops_limit) {
    Cand cur = frontier.top();
    frontier.pop();
    st.hops++;
    hops++;

    const uint32_t* nbrs = g.neighbors(cur.id);
    std::vector<uint32_t> nb(nbrs, nbrs + g.meta.degree);

    // Semantic next-hop: neighbor already resident in the active CXL cache?
    for (uint32_t id : nb) {
      ctx.st->semantic_next_total++;
      bool sem = false;
      if (ctx.rec && ctx.rec->contains(id)) sem = true;
      if (ctx.page) {
        size_t vb = ctx.ssd->vec_bytes;
        uint64_t byte_off = uint64_t(id) * vb;
        uint64_t p0 = byte_off / PageCache::kPage;
        uint64_t p1 = (byte_off + vb - 1) / PageCache::kPage;
        sem = true;
        for (uint64_t p = p0; p <= p1; ++p) {
          if (!ctx.page->map.count(p)) {
            sem = false;
            break;
          }
        }
      }
      if (sem) ctx.st->semantic_next_hits++;
    }

    if (ctx.mode == FetchCtx::BATCH_PROMOTE) {
      ctx.promote_batch(nb);
    }

    for (uint32_t id : nb) {
      const float* v = ctx.fetch(id);
      float dist = l2_sq(query, v, dim);
      // Only expand if better than worst in best or best not full
      if ((int)best.size() < beam || dist < best.top().dist) {
        consider(id, dist);
      } else {
        // still fetched (realistic ANNS often computes all neighbors)
        visited.insert(id);
      }
    }
  }
  if (visited_out) {
    visited_out->assign(visited.begin(), visited.end());
  }
}
