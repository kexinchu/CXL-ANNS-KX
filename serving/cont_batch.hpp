#pragma once
// Cross-query interleave, not fat IO batching.
// Keep T queries in flight. Issue prefetch immediately. When A cannot
// score, the stepper steals B. Per-query latency may rise; the metric is QPS.

#include "hide_fill.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

enum class CbSt : uint8_t { Empty, Ready, Wait, Busy, Done, Hold };

enum class Pipe2Act : uint8_t { Fill, Rank, Wait, Done, Pump, Issue };

struct Pipe2Dec {
  Pipe2Act act = Pipe2Act::Done;
  int slot = -1;
};

// Per-thread D-deep interleave. Fill an empty seat first so NAND stays issued
// during another query's beam; only then Rank a covering slot; else Wait; else Done.
// Thread t sleeps this many microseconds before the timed loop.
// stagger_us=0: no offset. Else t * stagger_us (thread 0 never sleeps).
inline uint64_t pipe2_stagger_us(int t, uint32_t stagger_us) {
  if (t <= 0 || stagger_us == 0) return 0;
  return (uint64_t)stagger_us * (uint64_t)t;
}

inline bool issue_qd_ok(uint32_t inflight, uint32_t qd) {
  return qd == 0 || inflight < qd;
}

// QD tracks an actually submitted NAND wave, not a query that reached Issue.
// A resident committed set is already rankable and legitimately adds no token.
inline bool issue_consumes_qd(bool need_empty, bool covered, bool token_added) {
  return !need_empty && !covered && token_added;
}

// A Wait query can lose coverage after its completed pages are evicted. Once
// its local I/O drains, it must refill the same committed pages to make
// forward progress; this does not admit or expand a new query.
inline bool should_refill_missing(CbSt st, bool covering, bool any_inflight) {
  return st == CbSt::Wait && !covering && !any_inflight;
}

// cli==0 means match T so NAND waves scale with compute threads.
inline uint32_t effective_issue_qd(uint32_t cli, int nthreads) {
  if (cli == 0) return nthreads > 0 ? (uint32_t)nthreads : 1u;
  return cli;
}

inline void note_scheduler_inflight(Metrics* metrics, uint32_t inflight) {
  if (metrics) metrics->note_inflight_depth(inflight);
}

// Dual-queue: never block the compute thread.
// Feed prefetch (Issue Hold) when QD has room; else Fill or Rank; else Pump.
inline Pipe2Dec steal_decide(const CbSt* st, const bool* covering, int n, bool has_more,
                             bool qd_ok) {
  Pipe2Dec d;
  if (n <= 0) return d;
  if (qd_ok) {
    for (int i = 0; i < n; ++i) {
      if (st[i] == CbSt::Hold) {
        d.act = Pipe2Act::Issue;
        d.slot = i;
        return d;
      }
    }
  }
  if (has_more) {
    for (int i = 0; i < n; ++i) {
      if (st[i] == CbSt::Empty) {
        d.act = Pipe2Act::Fill;
        d.slot = i;
        return d;
      }
    }
  }
  for (int i = 0; i < n; ++i) {
    if ((st[i] == CbSt::Wait || st[i] == CbSt::Ready) && covering && covering[i]) {
      d.act = Pipe2Act::Rank;
      d.slot = i;
      return d;
    }
  }
  for (int i = 0; i < n; ++i) {
    if (st[i] == CbSt::Wait || st[i] == CbSt::Hold) {
      d.act = Pipe2Act::Pump;
      d.slot = i;
      return d;
    }
  }
  return d;
}

inline Pipe2Dec pipe2_decide(const CbSt* st, const bool* covering, int n, bool has_more) {
  Pipe2Dec d;
  if (n <= 0) return d;
  if (has_more) {
    for (int i = 0; i < n; ++i) {
      if (st[i] == CbSt::Empty) {
        d.act = Pipe2Act::Fill;
        d.slot = i;
        return d;
      }
    }
  }
  for (int i = 0; i < n; ++i) {
    if ((st[i] == CbSt::Wait || st[i] == CbSt::Ready) && covering && covering[i]) {
      d.act = Pipe2Act::Rank;
      d.slot = i;
      return d;
    }
  }
  for (int i = 0; i < n; ++i) {
    if (st[i] == CbSt::Wait || st[i] == CbSt::Ready) {
      d.act = Pipe2Act::Wait;
      d.slot = i;
      return d;
    }
  }
  return d;
}

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

  void bind(PageCopyPool* pool, DramWindow* win, Placement* pl, VmemIo* vio, Metrics* m) {
    pipe.pool = pool;
    pipe.win = win;
    pipe.pl = pl;
    pipe.vio = vio;
    pipe.m = m;
  }

  uint64_t submit(const std::vector<uint64_t>& pages) {
    if (pages.empty()) return 0;
    std::lock_guard<std::mutex> g(mu);
    issue_n++;
    issue_pages += pages.size();
    return pipe.issue(pages, /*ttl=*/128, /*stall_if_full=*/false);
  }

  uint64_t submit_block(const std::vector<uint64_t>& pages) {
    if (pages.empty()) return 0;
    std::lock_guard<std::mutex> g(mu);
    issue_n++;
    issue_pages += pages.size();
    return pipe.issue(pages, /*ttl=*/128, /*stall_if_full=*/true);
  }

  uint64_t wait_covering(const std::vector<uint64_t>& need,
                         std::vector<uint64_t>* extra_toks = nullptr) {
    std::lock_guard<std::mutex> g(mu);
    return pipe.wait_covering(need, extra_toks);
  }

  void pump() {
    std::lock_guard<std::mutex> g(mu);
    pipe.pump();
  }

  int free_slots() {
    std::lock_guard<std::mutex> g(mu);
    int n = 0;
    for (int s = 0; s < HidePipe::kSlots; ++s) {
      if (!pipe.slot[s].active) n++;
    }
    return n;
  }

  bool any_inflight() {
    std::lock_guard<std::mutex> g(mu);
    for (int s = 0; s < HidePipe::kSlots; ++s) {
      if (pipe.slot[s].active) return true;
    }
    return false;
  }
};
