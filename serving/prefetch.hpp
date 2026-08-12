#pragma once
#include "dram_window.hpp"
#include "placement.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
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
  size_t async_q_cap = 4096;
  uint32_t pipe_w = 4;  // P3 outstanding staging width
  uint32_t install_top = 4;  // P3: max host pages/hop queued for DAX install
  uint32_t fetch_top = 0;    // P3: max SSD→host pages/hop (0 = budget only)
  bool page_group_b = false; // B: install/soft-pin adjacent pages of a 2-page group

  struct Job {
    const uint8_t* ptr = nullptr;
    size_t len = 0;
  };

  std::deque<Job> q;
  std::mutex mu;
  std::condition_variable cv;
  std::atomic<bool> stop{false};
  std::thread worker;
  Placement* pl = nullptr;
  DramWindow* win = nullptr;
  bool entry_pinned = false;

  static size_t bytes_charge(size_t page_bytes, size_t len) {
    if (len == 0) len = page_bytes;
    // 3072B vectors often span 2 pages.
    size_t n = (len + page_bytes - 1) / page_bytes;
    if (len <= page_bytes && len > page_bytes / 2) n = 2;
    if (n < 1) n = 1;
    return n * page_bytes;
  }

  void enqueue(const uint8_t* ptr, size_t len) {
    if (!ptr || !win) return;
    size_t need = bytes_charge(win->page_bytes, len);
    std::lock_guard<std::mutex> g(mu);
    if (budget_left < need) return;
    if (q.size() >= async_q_cap) return;
    budget_left -= need;
    q.push_back({ptr, len});
    cv.notify_one();
  }

  // Critical-path I/O (e.g. pending nbrs of expanded node): ignore budget, keep queue bound.
  void enqueue_force(const uint8_t* ptr, size_t len) {
    if (!ptr || !win) return;
    std::lock_guard<std::mutex> g(mu);
    if (q.size() >= async_q_cap) return;
    q.push_back({ptr, len});
    cv.notify_one();
  }

  void start_async(Placement* p, DramWindow* w) {
    pl = p;
    win = w;
    stop = false;
    worker = std::thread([this] {
      while (true) {
        Job job;
        {
          std::unique_lock<std::mutex> g(mu);
          cv.wait_for(g, std::chrono::milliseconds(1),
                      [&] { return stop.load() || !q.empty(); });
          if (stop.load() && q.empty()) break;
          if (q.empty()) continue;
          job = q.front();
          q.pop_front();
        }
        if (!job.ptr || !win || !pl) continue;
        size_t b = SIZE_MAX / 4;
        win->try_prefetch(pl->ssd_base, job.ptr, &b, job.len ? job.len : win->page_bytes);
      }
    });
  }

  void stop_async() {
    {
      std::lock_guard<std::mutex> g(mu);
      stop = true;
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
  }

  // Pin small hot entry region once (survives cross-query reuse / soft flush).
  void pin_entry_hot(Placement& p, DramWindow& w, const std::vector<uint32_t>& entry_ids,
                     uint32_t start_id) {
    if (!pin_entry || entry_pinned) return;
    size_t vb = (size_t)p.hdr->dim * p.hdr->vec_bytes;
    size_t nb = (size_t)p.hdr->R * 4;
    w.pin(p.ssd_base, p.vec(start_id), vb);
    w.pin(p.ssd_base, reinterpret_cast<const uint8_t*>(p.nbrs(start_id)), nb);
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
    // P0: demand only. P3: hop pipeline owns budget (no entry try_prefetch).
    if (policy == PrefetchPolicy::P0 || policy == PrefetchPolicy::P3) return;
    size_t vb = (size_t)p.hdr->dim * p.hdr->vec_bytes;
    for (uint32_t id : entry_ids) {
      if (budget_left < w.page_bytes) break;
      // P1/P2v2: cooperative single-threaded entry warm.
      w.try_prefetch(p.ssd_base, p.vec(id), &budget_left, vb);
    }
  }

  void on_expand(Placement& p, DramWindow& w, uint32_t node) {
    if (policy == PrefetchPolicy::P0) return;
    const uint32_t* nbr_ptr = reinterpret_cast<const uint32_t*>(
        w.lookup_or_promote(p.ssd_base, reinterpret_cast<const uint8_t*>(p.nbrs(node)),
                            (size_t)p.hdr->R * 4));
    uint32_t lim = neighbor_k < p.hdr->R ? neighbor_k : p.hdr->R;
    uint32_t nbr_local[64];
    if (lim > 64) lim = 64;
    std::memcpy(nbr_local, nbr_ptr, lim * sizeof(uint32_t));
    size_t vb = (size_t)p.hdr->dim * p.hdr->vec_bytes;
    for (uint32_t i = 0; i < lim; ++i) {
      uint32_t nb = nbr_local[i];
      if (nb >= p.hdr->n) continue;
      if (budget_left < w.page_bytes) return;
      w.try_prefetch(p.ssd_base, p.vec(nb), &budget_left, vb);
    }
  }

  struct CandDist {
    float dist;
    uint32_t id;
  };

  // Prefetch neighbor vectors of the closest unexpanded candidates first.
  // exclude_id: skip this node (e.g. current expand) so I/O targets future hops.
  // P1/P2v2: synchronous try_prefetch (P2v2 uses this only for non-cur lookahead).
  void prefetch_by_cand_distance(Placement& p, DramWindow& w,
                                 const std::vector<CandDist>& unexp,
                                 const std::unordered_set<uint32_t>& seen,
                                 uint32_t exclude_id = UINT32_MAX) {
    if (policy == PrefetchPolicy::P0 || unexp.empty()) return;
    std::vector<CandDist> order;
    order.reserve(unexp.size());
    for (const auto& c : unexp) {
      if (c.id == exclude_id) continue;
      order.push_back(c);
    }
    if (order.empty()) return;
    std::sort(order.begin(), order.end(),
              [](const CandDist& a, const CandDist& b) { return a.dist < b.dist; });
    uint32_t lim_nodes = lookahead_k < (uint32_t)order.size() ? lookahead_k
                                                              : (uint32_t)order.size();
    size_t vb = (size_t)p.hdr->dim * p.hdr->vec_bytes;
    for (uint32_t i = 0; i < lim_nodes; ++i) {
      if (budget_left < w.page_bytes) return;
      uint32_t node = order[i].id;
      uint32_t nbr_local[64];
      uint32_t R = p.hdr->R;
      if (R > 64) R = 64;
      {
        alignas(64) uint8_t nbuf[64 * 4];
        w.copy_through(p.ssd_base, reinterpret_cast<const uint8_t*>(p.nbrs(node)),
                       (size_t)R * 4, nbuf);
        std::memcpy(nbr_local, nbuf, (size_t)R * 4);
      }
      uint32_t lim = neighbor_k < R ? neighbor_k : R;
      for (uint32_t j = 0; j < lim; ++j) {
        uint32_t nb = nbr_local[j];
        if (nb >= p.hdr->n || seen.count(nb)) continue;
        if (budget_left < w.page_bytes) return;
        w.try_prefetch(p.ssd_base, p.vec(nb), &budget_left, vb);
      }
    }
  }
};
