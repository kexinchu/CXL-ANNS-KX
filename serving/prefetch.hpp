#pragma once
#include "dram_window.hpp"
#include "placement.hpp"

#include <cstdint>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <vector>

enum class PrefetchPolicy { P0, P1, P2, P3 };

struct Prefetch {
  PrefetchPolicy policy = PrefetchPolicy::P0;
  size_t budget_per_query = 256 * 1024;
  size_t budget_left = 0;
  uint32_t neighbor_k = 16;

  // async queue for P2
  std::deque<const uint8_t*> q;
  std::mutex mu;
  std::atomic<bool> stop{false};
  std::thread worker;
  Placement* pl = nullptr;
  DramWindow* win = nullptr;

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

  void on_query_begin(Placement& p, DramWindow& w,
                      const std::vector<uint32_t>& entry_ids) {
    budget_left = budget_per_query;
    if (policy == PrefetchPolicy::P3 || policy == PrefetchPolicy::P1 ||
        policy == PrefetchPolicy::P2) {
      // entry-warm (P3 always; also helps P1/P2)
      for (uint32_t id : entry_ids) {
        if (budget_left < w.page_bytes) break;
        w.try_prefetch(p.ssd_base, p.pq(id), &budget_left);
        if (budget_left < w.page_bytes) break;
        w.try_prefetch(p.ssd_base, reinterpret_cast<const uint8_t*>(p.nbrs(id)),
                       &budget_left);
      }
    }
  }

  void on_expand(Placement& p, DramWindow& w, uint32_t node) {
    if (policy == PrefetchPolicy::P0) return;
    const uint32_t* nbr = p.nbrs(node);
    uint32_t lim = neighbor_k < p.hdr->R ? neighbor_k : p.hdr->R;
    for (uint32_t i = 0; i < lim; ++i) {
      uint32_t nb = nbr[i];
      if (nb >= p.hdr->n) continue;
      if (policy == PrefetchPolicy::P2) {
        std::lock_guard<std::mutex> g(mu);
        if (q.size() < 1024) {
          q.push_back(p.pq(nb));
          q.push_back(reinterpret_cast<const uint8_t*>(p.nbrs(nb)));
        }
      } else {
        if (budget_left < w.page_bytes) return;
        w.try_prefetch(p.ssd_base, p.pq(nb), &budget_left);
        if (budget_left < w.page_bytes) return;
        w.try_prefetch(p.ssd_base, reinterpret_cast<const uint8_t*>(p.nbrs(nb)),
                       &budget_left);
      }
    }
  }
};
