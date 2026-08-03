#pragma once
// Shared helpers for vmem F4–F9 motivation findings.

#include "beam.hpp"
#include "dataset.hpp"
#include "vmem_cxl.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <list>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct BeamOut {
  std::vector<uint32_t> ids;  // best first, size <= k_out
  Stats st;
};

inline std::vector<float> load_host_vectors(const std::string& dir, uint32_t n,
                                            uint32_t dim) {
  std::vector<float> v(size_t(n) * dim);
  int fd = ::open(vec_path(dir).c_str(), O_RDONLY);
  if (fd < 0) die("open host vectors for GT");
  size_t bytes = v.size() * sizeof(float);
  size_t off = 0;
  while (off < bytes) {
    ssize_t r =
        ::read(fd, reinterpret_cast<char*>(v.data()) + off, bytes - off);
    if (r <= 0) die("read host vectors short");
    off += size_t(r);
  }
  ::close(fd);
  return v;
}

inline std::vector<uint32_t> brute_topk(const float* host, uint32_t n,
                                        uint32_t dim, const float* query,
                                        int k) {
  std::vector<Cand> heap;
  heap.reserve(size_t(k) + 1);
  auto worse = [](const Cand& a, const Cand& b) { return a.dist < b.dist; };
  for (uint32_t i = 0; i < n; ++i) {
    float d = l2_sq(query, host + size_t(i) * dim, dim);
    if ((int)heap.size() < k) {
      heap.push_back({d, i});
      if ((int)heap.size() == k)
        std::make_heap(heap.begin(), heap.end(), worse);
    } else if (d < heap.front().dist) {
      std::pop_heap(heap.begin(), heap.end(), worse);
      heap.back() = {d, i};
      std::push_heap(heap.begin(), heap.end(), worse);
    }
  }
  std::sort_heap(heap.begin(), heap.end(), worse);
  std::vector<uint32_t> out(heap.size());
  for (size_t i = 0; i < heap.size(); ++i) out[i] = heap[i].id;
  return out;
}

inline double recall_at(const std::vector<uint32_t>& pred,
                        const std::vector<uint32_t>& gt, int k) {
  int kk = std::min(k, (int)std::min(pred.size(), gt.size()));
  if (kk <= 0) return 0;
  std::unordered_set<uint32_t> gset(gt.begin(), gt.begin() + kk);
  int hit = 0;
  for (int i = 0; i < kk; ++i)
    if (gset.count(pred[i])) hit++;
  return double(hit) / double(kk);
}

// Demand-fault beam search on vmem; optional userspace admit hook.
struct AdmitPolicy {
  enum Kind { NONE, RECORD_LRU, PAGE16K, GRAPH_EXPAND, HUB_PIN } kind = NONE;
  size_t budget_bytes = 64ull << 20;
  size_t page_vecs = 4;
  std::unordered_set<uint32_t> pinned;  // always-resident logical set
  // LRU state
  std::unordered_map<uint64_t, int> res;
  std::list<uint64_t> lru;
  size_t cap = 1;
  uint64_t promote_pages = 0;  // pages brought by policy / prefetch
  uint64_t promote_used = 0;   // of promoted, later demanded

  void reset_cache(uint32_t dim) {
    res.clear();
    lru.clear();
    promote_pages = 0;
    promote_used = 0;
    size_t vb = size_t(dim) * 4;
    if (kind == RECORD_LRU || kind == GRAPH_EXPAND || kind == HUB_PIN)
      cap = std::max<size_t>(1, budget_bytes / vb);
    else if (kind == PAGE16K)
      cap = std::max<size_t>(1, budget_bytes / (vb * page_vecs));
  }

  void evict_one() {
    while (!lru.empty()) {
      uint64_t k = lru.front();
      lru.pop_front();
      if (res.erase(k)) return;
    }
  }

  bool admit_key(uint64_t key) {
    if (res.count(key)) {
      lru.push_back(key);
      return true;
    }
    while (res.size() >= cap) evict_one();
    res[key] = 1;
    lru.push_back(key);
    return false;
  }
};

inline BeamOut vmem_beam_search(VmemArena& arena, const Graph& g,
                                const float* query, int beam, int hops,
                                int k_out, AdmitPolicy* pol = nullptr,
                                const std::unordered_set<uint32_t>* force_pin =
                                    nullptr) {
  BeamOut out;
  Stats& st = out.st;
  const uint32_t dim = g.meta.dim;
  const uint32_t degree = g.meta.degree;

  std::priority_queue<Cand> best;
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

  auto touch = [&](uint32_t id) -> const float* {
    st.vector_fetches++;
    bool pinned =
        (force_pin && force_pin->count(id)) || (pol && pol->pinned.count(id));
    if (pinned) {
      st.cxl_hits++;
      if (!arena.page_resident(id, dim)) arena.touch_vec(id, dim);
      return arena.vec_ptr(id, dim);
    }
    if (pol && pol->kind != AdmitPolicy::NONE) {
      bool hit = false;
      if (pol->kind == AdmitPolicy::RECORD_LRU ||
          pol->kind == AdmitPolicy::GRAPH_EXPAND ||
          pol->kind == AdmitPolicy::HUB_PIN) {
        hit = pol->admit_key(id);
        if (!hit) {
          st.cxl_misses++;
          arena.touch_vec(id, dim);
          if (pol->kind == AdmitPolicy::GRAPH_EXPAND) {
            const uint32_t* nbrs = g.neighbors(id);
            for (uint32_t k = 0; k < degree; ++k) {
              if (!pol->res.count(nbrs[k])) {
                pol->admit_key(nbrs[k]);
                arena.touch_vec(nbrs[k], dim);
                pol->promote_pages++;
              }
            }
          }
        } else {
          st.cxl_hits++;
          if (!arena.page_resident(id, dim)) arena.touch_vec(id, dim);
        }
      } else {  // PAGE16K
        uint64_t blk = id / pol->page_vecs;
        hit = pol->admit_key(blk);
        uint32_t base = uint32_t(blk * pol->page_vecs);
        if (!hit) {
          st.cxl_misses++;
          st.page_misses++;
          for (size_t j = 0; j < pol->page_vecs && base + j < g.meta.n; ++j) {
            arena.touch_vec(base + uint32_t(j), dim);
            pol->promote_pages++;
          }
        } else {
          st.cxl_hits++;
          st.page_hits++;
          if (!arena.page_resident(id, dim)) arena.touch_vec(id, dim);
        }
      }
      return arena.vec_ptr(id, dim);
    }
    // raw vmem residency
    if (arena.page_resident(id, dim)) st.cxl_hits++;
    else st.cxl_misses++;
    return arena.touch_vec(id, dim);
  };

  touch(g.meta.entry);
  consider(g.meta.entry, l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));

  int hops_done = 0;
  while (!frontier.empty() && hops_done < hops) {
    Cand cur = frontier.top();
    frontier.pop();
    st.hops++;
    hops_done++;
    const uint32_t* nbrs = g.neighbors(cur.id);
    for (uint32_t k = 0; k < degree; ++k) {
      uint32_t id = nbrs[k];
      const float* v = touch(id);
      float dist = l2_sq(query, v, dim);
      st.semantic_next_total++;
      if (arena.page_resident(id, dim)) st.semantic_next_hits++;
      if ((int)best.size() < beam || dist < best.top().dist)
        consider(id, dist);
      else
        visited.insert(id);
    }
  }

  std::vector<Cand> sorted;
  while (!best.empty()) {
    sorted.push_back(best.top());
    best.pop();
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
  int nout = std::min(k_out, (int)sorted.size());
  out.ids.resize(nout);
  for (int i = 0; i < nout; ++i) out.ids[i] = sorted[i].id;
  return out;
}

inline std::vector<uint32_t> compute_indegree_hubs(const Graph& g, size_t topk) {
  std::vector<uint32_t> deg(g.meta.n, 0);
  for (uint32_t i = 0; i < g.meta.n; ++i) {
    const uint32_t* nbrs = g.neighbors(i);
    for (uint32_t k = 0; k < g.meta.degree; ++k) deg[nbrs[k]]++;
  }
  std::vector<uint32_t> ids(g.meta.n);
  for (uint32_t i = 0; i < g.meta.n; ++i) ids[i] = i;
  std::partial_sort(
      ids.begin(), ids.begin() + std::min(topk, ids.size()), ids.end(),
      [&](uint32_t a, uint32_t b) { return deg[a] > deg[b]; });
  ids.resize(std::min(topk, ids.size()));
  return ids;
}
