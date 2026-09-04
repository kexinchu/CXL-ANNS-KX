#pragma once
// Cross-query interleave (not fat IO batching).
// Keep T queries in flight. Issue prefetch immediately. When A cannot score,
// a worker steals B. Per-query latency may rise; the metric is QPS.

#include "hide_fill.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

enum class CbSt : uint8_t { Empty, Ready, Wait, Busy, Done };

inline std::vector<uint64_t> merge_unique_pages(const std::vector<uint64_t>* const* parts,
                                                int n) {
  std::vector<uint64_t> out;
  std::unordered_set<uint64_t> seen;
  for (int i = 0; i < n; ++i) {
    if (!parts[i]) continue;
    for (uint64_t p : *parts[i]) {
      if (seen.insert(p).second) out.push_back(p);
    }
  }
  return out;
}

inline int pick_ready(const CbSt* st, int n) {
  for (int i = 0; i < n; ++i) {
    if (st[i] == CbSt::Ready) return i;
  }
  return -1;
}

// Prefer a query that can compute; else one that may drain. Skip Busy.
inline int pick_steal(const CbSt* st, int n, int start = 0) {
  if (n <= 0) return -1;
  if (start < 0) start = 0;
  for (int k = 0; k < n; ++k) {
    int i = (start + k) % n;
    if (st[i] == CbSt::Ready) return i;
  }
  for (int k = 0; k < n; ++k) {
    int i = (start + k) % n;
    if (st[i] == CbSt::Wait) return i;
  }
  return -1;
}

struct PrefetchHub {
  HidePipe pipe;
  std::mutex mu;
  uint64_t issue_n = 0;
  uint64_t issue_pages = 0;
  uint64_t drop_n = 0;

  void bind(PageCopyPool* pool, DramWindow* win, Placement* pl, VmemIo* vio, Metrics* m) {
    pipe.pool = pool;
    pipe.win = win;
    pipe.pl = pl;
    pipe.vio = vio;
    pipe.m = m;
  }

  // Fire-and-forget. Never hide_wait. Retry later if the pipe is full.
  bool submit(const std::vector<uint64_t>& pages) {
    if (pages.empty()) return true;
    std::lock_guard<std::mutex> g(mu);
    bool ok = pipe.issue(pages, /*ttl=*/128, /*stall_if_full=*/false);
    if (ok) {
      issue_n++;
      issue_pages += pages.size();
    } else {
      drop_n++;
    }
    return ok;
  }

  void pump() {
    std::lock_guard<std::mutex> g(mu);
    pipe.pump();
  }

  bool any_inflight() {
    std::lock_guard<std::mutex> g(mu);
    for (int s = 0; s < HidePipe::kSlots; ++s) {
      if (pipe.slot[s].active) return true;
    }
    return false;
  }
};
