#pragma once
// Bounded outstanding promote pipeline: N workers copy vmem → host DRAM staging.
// Search thread scores from staging (sole consumer). Do not install partial pages
// into DramWindow (page-false-hit hazard).

#include "metrics.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

struct PromotePipe {
  static constexpr size_t kMaxW = 16;
  static constexpr size_t kPage = 4096;

  size_t W = 4;
  Metrics* metrics = nullptr;

  enum class St : int { Free = 0, Queued = 1, Done = 2 };

  struct Slot {
    std::atomic<int> state{0};
    uint32_t id = 0;
    const uint8_t* ssd_ptr = nullptr;
    size_t len = 0;
    std::vector<uint8_t> host;
  };

  Slot slots[kMaxW];
  const uint8_t* ssd_base = nullptr;

  std::mutex mu;
  std::condition_variable cv_job;
  std::condition_variable cv_done;
  std::vector<size_t> job_q;
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  size_t inflight = 0;
  bool started = false;

  void start(const uint8_t* base, size_t width, Metrics* m) {
    if (started) return;
    ssd_base = base;
    metrics = m;
    W = width < 1 ? 1 : (width > kMaxW ? kMaxW : width);
    stop = false;
    for (size_t i = 0; i < kMaxW; ++i) {
      slots[i].state.store((int)St::Free);
      slots[i].host.clear();
    }
    job_q.clear();
    inflight = 0;
    workers.clear();
    workers.reserve(W);
    for (size_t t = 0; t < W; ++t) workers.emplace_back([this] { this->run(); });
    started = true;
  }

  void stop_join() {
    if (!started) return;
    {
      std::lock_guard<std::mutex> g(mu);
      stop = true;
    }
    cv_job.notify_all();
    cv_done.notify_all();
    for (auto& th : workers)
      if (th.joinable()) th.join();
    workers.clear();
    started = false;
  }

  size_t inflight_count() {
    std::lock_guard<std::mutex> g(mu);
    return inflight;
  }

  int try_submit(uint32_t id, const uint8_t* ssd_ptr, size_t len) {
    if (!started || !ssd_ptr || len == 0) return -1;
    std::lock_guard<std::mutex> g(mu);
    if (inflight >= W) return -1;
    size_t slot = kMaxW;
    for (size_t i = 0; i < W; ++i) {
      if (slots[i].state.load() == (int)St::Free) {
        slot = i;
        break;
      }
    }
    if (slot == kMaxW) return -1;
    slots[slot].id = id;
    slots[slot].ssd_ptr = ssd_ptr;
    slots[slot].len = len;
    slots[slot].host.resize(len);
    slots[slot].state.store((int)St::Queued);
    job_q.push_back(slot);
    inflight++;
    cv_job.notify_all();
    return (int)slot;
  }

  void poll_done(std::vector<int>& out) {
    out.clear();
    for (size_t i = 0; i < W; ++i) {
      if (slots[i].state.load() == (int)St::Done) out.push_back((int)i);
    }
  }

  bool wait_done(std::vector<int>& out) {
    out.clear();
    std::unique_lock<std::mutex> g(mu);
    cv_done.wait(g, [&] {
      if (stop.load()) return true;
      for (size_t i = 0; i < W; ++i) {
        if (slots[i].state.load() == (int)St::Done) return true;
      }
      // Self-heal: inflight desync with empty queue and no active slots.
      if (inflight > 0 && job_q.empty()) {
        bool any_active = false;
        for (size_t i = 0; i < W; ++i) {
          int st = slots[i].state.load();
          if (st == (int)St::Queued || st == (int)St::Done) {
            any_active = true;
            break;
          }
        }
        if (!any_active) inflight = 0;
      }
      return inflight == 0;
    });
    g.unlock();
    poll_done(out);
    return !out.empty();
  }

  const uint8_t* data(int slot) const { return slots[slot].host.data(); }
  size_t length(int slot) const { return slots[slot].len; }
  uint32_t node_id(int slot) const { return slots[slot].id; }

  void release(int slot) {
    std::lock_guard<std::mutex> g(mu);
    int prev = slots[slot].state.exchange((int)St::Free);
    slots[slot].ssd_ptr = nullptr;
    slots[slot].len = 0;
    // Only decrement if we actually retired an outstanding slot.
    if (prev == (int)St::Done || prev == (int)St::Queued) {
      if (inflight > 0) inflight--;
    }
  }

 private:
  void run() {
    while (true) {
      size_t slot = kMaxW;
      {
        std::unique_lock<std::mutex> g(mu);
        cv_job.wait(g, [&] { return stop.load() || !job_q.empty(); });
        if (stop.load() && job_q.empty()) break;
        if (job_q.empty()) continue;
        slot = job_q.front();
        job_q.erase(job_q.begin());
      }
      Slot& s = slots[slot];
      const uint8_t* src = s.ssd_ptr;
      uint8_t* dst = s.host.data();
      size_t len = s.len;
      auto t0 = std::chrono::steady_clock::now();
      std::memcpy(dst, src, len);
      auto t1 = std::chrono::steady_clock::now();
      if (metrics) {
        metrics->ssd_misses++;
        size_t pages = (len + kPage - 1) / kPage;
        if (pages < 1) pages = 1;
        metrics->promote_bytes += pages * kPage;
        metrics->promote_ns +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        metrics->prefetch_pages += pages;
      }
      s.state.store((int)St::Done);
      cv_done.notify_all();
    }
  }
};
