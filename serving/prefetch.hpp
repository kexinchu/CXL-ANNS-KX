#pragma once
#include "dram_window.hpp"
#include "placement.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <vector>
#include <unordered_set>

enum class PrefetchPolicy { P0, P1, P2, P3 };

// Prefetch neighbor *vector* pages into the CXL-DRAM window (one-shot FP path).
struct Prefetch {
  PrefetchPolicy policy = PrefetchPolicy::P0;
  size_t budget_per_query = 256 * 1024;
  size_t budget_left = 0;
  uint32_t neighbor_k = 16;
  uint32_t lookahead_k = 8;  // prefetch nbrs of top-K unexpanded cands by distance
  bool pin_entry = true;

  std::deque<const uint8_t*> q;
  std::mutex mu;
  std::atomic<bool> stop{false};
  std::thread worker;
  Placement* pl = nullptr;
  DramWindow* win = nullptr;
  bool entry_pinned = false;

  void start_async(Placement* p, DramWindow* w) {
    pl = p;
    win = w;
    stop = false;
    worker = std::thread([this] {
      while (!stop.load()) {
        const uint8_t* ptr = nullptr;
        {
          std::lock_guard<std::mutex> g(mu);
          if (!q.empty()) {
            ptr = q.front();
            q.pop_front();
          }
        }
        if (!ptr) {
          std::this_thread::sleep_for(std::chrono::microseconds(50));
          continue;
        }
        size_t b = win->page_bytes;
        win->try_prefetch(pl->ssd_base, ptr, &b);
      }
    });
  }

  void stop_async() {
    stop = true;
    if (worker.joinable()) worker.join();
  }

  // Pin small hot entry region once (survives cross-query reuse / soft flush).
  void pin_entry_hot(Placement& p, DramWindow& w, const std::vector<uint32_t>& entry_ids,
                     uint32_t start_id) {
    if (!pin_entry || entry_pinned) return;
    size_t vb = (size_t)p.hdr->dim * p.hdr->vec_bytes;
    size_t nb = (size_t)p.hdr->R * 4;
    // Always pin medoid/start vector + adjacency.
    w.pin(p.ssd_base, p.vec(start_id), vb);
    w.pin(p.ssd_base, reinterpret_cast<const uint8_t*>(p.nbrs(start_id)), nb);
    // Pin adjacency of entry subgraph (small); vectors only for a prefix to stay in pin cap.
    uint32_t vec_pin_lim = 256;
    for (size_t i = 0; i < entry_ids.size(); ++i) {
      uint32_t id = entry_ids[i];
      w.pin(p.ssd_base, reinterpret_cast<const uint8_t*>(p.nbrs(id)), nb);
      if (i < vec_pin_lim) w.pin(p.ssd_base, p.vec(id), vb);
    }
    entry_pinned = true;
  }

  void on_query_begin(Placement& p, DramWindow& w, const std::vector<uint32_t>& entry_ids,
                      uint32_t start_id) {
    budget_left = budget_per_query;
    pin_entry_hot(p, w, entry_ids, start_id);
    if (policy == PrefetchPolicy::P0) return;
    // Budget-warm a few entry vectors beyond pins.
    for (uint32_t id : entry_ids) {
      if (budget_left < w.page_bytes) break;
      w.try_prefetch(p.ssd_base, p.vec(id), &budget_left,
                     (size_t)p.hdr->dim * p.hdr->vec_bytes);
    }
  }

  // Legacy: prefetch first neighbor_k nbr vectors of `node` (no distance order).
  void on_expand(Placement& p, DramWindow& w, uint32_t node) {
    if (policy == PrefetchPolicy::P0) return;
    const uint32_t* nbr_ptr = reinterpret_cast<const uint32_t*>(
        w.lookup_or_promote(p.ssd_base, reinterpret_cast<const uint8_t*>(p.nbrs(node)),
                            (size_t)p.hdr->R * 4));
    uint32_t lim = neighbor_k < p.hdr->R ? neighbor_k : p.hdr->R;
    uint32_t nbr_local[64];
    if (lim > 64) lim = 64;
    std::memcpy(nbr_local, nbr_ptr, lim * sizeof(uint32_t));
    for (uint32_t i = 0; i < lim; ++i) {
      uint32_t nb = nbr_local[i];
      if (nb >= p.hdr->n) continue;
      if (policy == PrefetchPolicy::P2) {
        std::lock_guard<std::mutex> g(mu);
        if (q.size() < 1024) q.push_back(p.vec(nb));
      } else {
        if (budget_left < w.page_bytes) return;
        w.try_prefetch(p.ssd_base, p.vec(nb), &budget_left,
                       (size_t)p.hdr->dim * p.hdr->vec_bytes);
      }
    }
  }

  struct CandDist {
    float dist;
    uint32_t id;
  };

  // Prefetch neighbor vectors of the closest unexpanded candidates first.
  void prefetch_by_cand_distance(Placement& p, DramWindow& w,
                                 const std::vector<CandDist>& unexp,
                                 const std::unordered_set<uint32_t>& seen) {
    if (policy == PrefetchPolicy::P0 || unexp.empty()) return;
    std::vector<CandDist> order = unexp;
    std::sort(order.begin(), order.end(),
              [](const CandDist& a, const CandDist& b) { return a.dist < b.dist; });
    uint32_t lim_nodes = lookahead_k < (uint32_t)order.size() ? lookahead_k
                                                              : (uint32_t)order.size();
    size_t vb = (size_t)p.hdr->dim * p.hdr->vec_bytes;
    size_t nb_bytes = (size_t)p.hdr->R * 4;
    for (uint32_t i = 0; i < lim_nodes; ++i) {
      if (budget_left < w.page_bytes) return;
      uint32_t node = order[i].id;
      const uint32_t* nbr_ptr = reinterpret_cast<const uint32_t*>(
          w.lookup_or_promote(p.ssd_base, reinterpret_cast<const uint8_t*>(p.nbrs(node)),
                              nb_bytes));
      uint32_t R = p.hdr->R;
      uint32_t nbr_local[64];
      if (R > 64) R = 64;
      std::memcpy(nbr_local, nbr_ptr, R * sizeof(uint32_t));
      // Prefer first neighbor_k edges (graph order); still gated by cand priority.
      uint32_t lim = neighbor_k < R ? neighbor_k : R;
      for (uint32_t j = 0; j < lim; ++j) {
        uint32_t nb = nbr_local[j];
        if (nb >= p.hdr->n || seen.count(nb)) continue;
        if (budget_left < w.page_bytes) return;
        if (policy == PrefetchPolicy::P2) {
          std::lock_guard<std::mutex> g(mu);
          if (q.size() < 1024) q.push_back(p.vec(nb));
        } else {
          w.try_prefetch(p.ssd_base, p.vec(nb), &budget_left, vb);
        }
      }
    }
  }
};
