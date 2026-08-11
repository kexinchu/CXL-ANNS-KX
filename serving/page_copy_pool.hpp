#pragma once
// Persistent workers for CXL-SSD (vmem) → host page copies. Search thread only
// writes DAX. Per-page ready flags let the caller score/install while copies run.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

struct PageCopyPool {
  struct Job {
    const uint8_t* src = nullptr;
    uint8_t* dst = nullptr;
    size_t len = 0;
    std::atomic<uint8_t>* ready = nullptr;
  };

  size_t nworkers = 0;
  std::vector<std::thread> workers;
  std::mutex mu;
  std::condition_variable cv;
  std::vector<Job> q;
  size_t q_head = 0;
  std::atomic<size_t> inflight{0};
  bool stop = false;

  void start(size_t w) {
    if (w == 0) w = 1;
    if (!workers.empty()) return;
    nworkers = w;
    q.reserve(4096);
    for (size_t t = 0; t < nworkers; ++t) {
      workers.emplace_back([this] { loop(); });
    }
  }

  void stop_join() {
    {
      std::lock_guard<std::mutex> g(mu);
      stop = true;
    }
    cv.notify_all();
    for (auto& th : workers) {
      if (th.joinable()) th.join();
    }
    workers.clear();
    nworkers = 0;
    q.clear();
    q_head = 0;
    inflight.store(0, std::memory_order_relaxed);
    stop = false;
  }

  ~PageCopyPool() { stop_join(); }

  void submit(const uint8_t* src, uint8_t* dst, size_t len, std::atomic<uint8_t>* ready) {
    if (ready) ready->store(0, std::memory_order_relaxed);
    inflight.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> g(mu);
      if (q_head > 1024 && q_head * 2 > q.size()) {
        q.erase(q.begin(), q.begin() + (std::ptrdiff_t)q_head);
        q_head = 0;
      }
      q.push_back(Job{src, dst, len, ready});
    }
    cv.notify_one();
  }

  void wait_idle() {
    while (inflight.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
  }

  size_t queued_approx() {
    std::lock_guard<std::mutex> g(mu);
    return q.size() - q_head;
  }

 private:
  void loop() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return stop || q_head < q.size(); });
        if (stop && q_head >= q.size()) return;
        job = q[q_head++];
      }
      if (job.dst && job.src && job.len) std::memcpy(job.dst, job.src, job.len);
      if (job.ready) job.ready->store(1, std::memory_order_release);
      inflight.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
};
