#pragma once
#include <cstdint>
#include <cstdio>

struct Metrics {
  uint64_t dram_hits = 0;
  uint64_t ssd_misses = 0;
  uint64_t promote_bytes = 0;
  uint64_t promote_ns = 0;
  uint64_t prefetch_pages = 0;
  uint64_t evicts = 0;
  uint64_t queries = 0;
  uint64_t distance_comps = 0;

  void reset() { *this = Metrics{}; }

  void print(FILE* f = stdout) const {
    double hit = dram_hits + ssd_misses
                     ? 100.0 * (double)dram_hits / (double)(dram_hits + ssd_misses)
                     : 0.0;
    double ns_miss =
        ssd_misses ? (double)promote_ns / (double)ssd_misses : 0.0;
    fprintf(f,
            "metrics queries=%llu dram_hits=%llu ssd_misses=%llu hit_pct=%.2f "
            "promote_bytes=%llu avg_promote_ns=%.1f prefetch_pages=%llu "
            "evicts=%llu dist=%llu\n",
            (unsigned long long)queries, (unsigned long long)dram_hits,
            (unsigned long long)ssd_misses, hit,
            (unsigned long long)promote_bytes, ns_miss,
            (unsigned long long)prefetch_pages, (unsigned long long)evicts,
            (unsigned long long)distance_comps);
  }
};
