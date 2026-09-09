#pragma once
// Cross-query interleave, not fat IO batching.
// Keep T queries in flight. Issue prefetch immediately. When A cannot
// score, the stepper steals B. Per-query latency may rise; the metric is QPS.

#include "hide_fill.hpp"

#include <atomic>
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

enum class ReserveResult : uint8_t { Prepared, NotReady, Exhausted };

// Admission-only barrier for fixed-size static batches.  Queries within a
// cohort may finish in any order; the next cohort becomes admissible only
// after every query in the current cohort has retired.  cohort_size==0 is the
// dynamic-batching identity: every in-range query is immediately admissible.
class StaticCohortGate {
 public:
  StaticCohortGate(uint32_t total_queries, uint32_t cohort_size)
      : total_(total_queries), cohort_size_(cohort_size) {
    cohort_end_ = cohort_size_ == 0 ? total_ : bounded_end(0);
    retired_.assign((size_t)(cohort_end_ - cohort_begin_), false);
  }

  bool can_admit(uint32_t qi) const {
    std::lock_guard<std::mutex> g(mu_);
    return qi < total_ && (cohort_size_ == 0 || qi < cohort_end_);
  }

  bool complete(uint32_t qi) {
    std::lock_guard<std::mutex> g(mu_);
    if (cohort_size_ == 0) return qi < total_;
    if (qi < cohort_begin_ || qi >= cohort_end_) return false;
    const size_t local = (size_t)(qi - cohort_begin_);
    if (retired_[local]) return false;
    retired_[local] = true;
    ++retired_count_;
    if (retired_count_ == retired_.size()) {
      cohort_begin_ = cohort_end_;
      cohort_end_ = bounded_end(cohort_begin_);
      retired_count_ = 0;
      retired_.assign((size_t)(cohort_end_ - cohort_begin_), false);
    }
    return true;
  }

  bool all_complete() const {
    std::lock_guard<std::mutex> g(mu_);
    return cohort_size_ != 0 && cohort_begin_ == total_;
  }

 private:
  uint32_t bounded_end(uint32_t begin) const {
    const uint64_t end = (uint64_t)begin + cohort_size_;
    return end < total_ ? (uint32_t)end : total_;
  }

  const uint32_t total_;
  const uint32_t cohort_size_;
  mutable std::mutex mu_;
  uint32_t cohort_begin_ = 0;
  uint32_t cohort_end_ = 0;
  size_t retired_count_ = 0;
  std::vector<bool> retired_;
};

// Preserve deterministic, serialized admission while keeping query-local
// preparation outside the reservation mutex. Preparation includes entry
// selection and PQ lookup-table initialization and must scale with workers.
template <class Prepare>
inline bool reserve_then_prepare(std::atomic<uint32_t>& next, uint32_t limit,
                                 std::mutex& admission_mu, Prepare&& prepare) {
  uint32_t qi = 0;
  {
    std::lock_guard<std::mutex> g(admission_mu);
    qi = next.fetch_add(1, std::memory_order_relaxed);
  }
  if (qi >= limit) return false;
  prepare(qi);
  return true;
}

// Open-loop admission must not reserve a future request and then sleep while
// the worker owns other live pipeline slots.  Recheck readiness while holding
// the short reservation lock, but keep all query-local preparation outside it.
template <class Ready, class Prepare>
inline ReserveResult reserve_ready_then_prepare(std::atomic<uint32_t>& next,
                                                uint32_t limit,
                                                std::mutex& admission_mu,
                                                Ready&& ready,
                                                Prepare&& prepare) {
  uint32_t qi = 0;
  {
    std::lock_guard<std::mutex> g(admission_mu);
    qi = next.load(std::memory_order_relaxed);
    if (qi >= limit) return ReserveResult::Exhausted;
    if (!ready(qi)) return ReserveResult::NotReady;
    next.store(qi + 1, std::memory_order_relaxed);
  }
  prepare(qi);
  return ReserveResult::Prepared;
}

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

inline bool should_release_qd(bool held, bool wave_pending) {
  return held && !wave_pending;
}

inline bool should_wait_for_detached_qd(Pipe2Act act, size_t detached_count) {
  return act == Pipe2Act::Done && detached_count != 0;
}

// A Wait query can lose coverage after its completed pages are evicted. Once
// its local I/O drains, it must refill the same committed pages to make
// forward progress; this does not admit or expand a new query.
inline bool query_needs_refill(CbSt st, bool covering, bool query_fill_pending) {
  return st == CbSt::Wait && !covering && !query_fill_pending;
}

// cli==0 means match T so NAND waves scale with compute threads.
inline uint32_t effective_issue_qd(uint32_t cli, int nthreads) {
  if (cli == 0) return nthreads > 0 ? (uint32_t)nthreads : 1u;
  return cli;
}

// Per-thread hubs partition the declared global PageCopyPool budget.  Giving
// every hub the full budget multiplies kernel I/O callers by T and makes the
// non-steal control incomparable with the shared-pool scheduler.
inline size_t per_thread_pool_workers(size_t total, int nthreads, int thread_id) {
  if (total == 0) total = 1;
  if (nthreads <= 1) return total;
  const size_t nt = (size_t)nthreads;
  if (total < nt) total = nt;
  const size_t base = total / nt;
  const size_t extra = total % nt;
  return base + ((size_t)thread_id < extra ? 1 : 0);
}

inline void note_scheduler_inflight(Metrics* metrics, uint32_t inflight) {
  if (metrics) metrics->note_inflight_depth(inflight);
}

// Classify only a scheduler decision that cannot run another query.  A Wait
// with a live fill is waiting for coverage; Hold (or a drained Wait that still
// cannot submit) is admission backpressure.  Runnable decisions are not stall.
inline HostStallCause classify_host_stall(Pipe2Act act, CbSt state,
                                          bool fill_pending) {
  if (act != Pipe2Act::Pump) return HostStallCause::None;
  if (state == CbSt::Wait && fill_pending) return HostStallCause::Coverage;
  if (state == CbSt::Wait || state == CbSt::Hold)
    return HostStallCause::SlotBackpressure;
  return HostStallCause::None;
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

  bool token_pending(uint64_t tok) {
    std::lock_guard<std::mutex> g(mu);
    return pipe.token_pending(tok);
  }

  bool any_token_pending(const std::vector<uint64_t>& toks) {
    std::lock_guard<std::mutex> g(mu);
    for (uint64_t tok : toks) {
      if (pipe.token_pending(tok)) return true;
    }
    return false;
  }
};
