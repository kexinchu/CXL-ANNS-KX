#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numa.h>
#include <stdexcept>
#include <string>
#include <vector>

inline void die(const std::string& msg) {
  std::fprintf(stderr, "ERROR: %s\n", msg.c_str());
  std::exit(1);
}

inline double now_sec() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

struct Timer {
  double t0 = now_sec();
  double elapsed() const { return now_sec() - t0; }
  void reset() { t0 = now_sec(); }
};

// Allocate on a specific NUMA node (0=DRAM, 2=CXL on gpu01).
inline void* numa_alloc_bytes(std::size_t bytes, int node) {
  if (numa_available() < 0) die("libnuma not available");
  void* p = numa_alloc_onnode(bytes, node);
  if (!p) die("numa_alloc_onnode failed");
  std::memset(p, 0, bytes);  // fault pages in on target node
  return p;
}

inline void numa_free_bytes(void* p, std::size_t bytes) {
  if (p) numa_free(p, bytes);
}

template <typename T>
T* numa_alloc_array(std::size_t n, int node) {
  return static_cast<T*>(numa_alloc_bytes(n * sizeof(T), node));
}

struct Stats {
  uint64_t queries = 0;
  uint64_t hops = 0;
  uint64_t vector_fetches = 0;
  uint64_t cxl_hits = 0;
  uint64_t cxl_misses = 0;
  uint64_t ssd_reads = 0;
  uint64_t ssd_bytes = 0;
  uint64_t page_hits = 0;
  uint64_t page_misses = 0;
  uint64_t semantic_next_hits = 0;
  uint64_t semantic_next_total = 0;
  double seconds = 0;

  void print(const char* tag) const {
    double qps = queries / std::max(seconds, 1e-9);
    double hit = (cxl_hits + cxl_misses)
                     ? 100.0 * cxl_hits / (cxl_hits + cxl_misses)
                     : 0.0;
    double page_hit = (page_hits + page_misses)
                          ? 100.0 * page_hits / (page_hits + page_misses)
                          : 0.0;
    double sem_hit = semantic_next_total
                         ? 100.0 * semantic_next_hits / semantic_next_total
                         : 0.0;
    std::printf(
        "[%s] q=%lu hops=%lu fetches=%lu qps=%.1f hit%%=%.2f page_hit%%=%.2f "
        "semantic_next_hit%%=%.2f ssd_reads=%lu ssd_MB=%.2f time=%.3fs\n",
        tag, (unsigned long)queries, (unsigned long)hops,
        (unsigned long)vector_fetches, qps, hit, page_hit, sem_hit,
        (unsigned long)ssd_reads, ssd_bytes / (1024.0 * 1024.0), seconds);
  }
};
