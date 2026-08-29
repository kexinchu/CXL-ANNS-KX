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
  uint64_t promote_pages = 0;  // pages issued by P3 fetch or demand promote
  uint64_t promote_used = 0;   // of those, later used by a score/copy
  uint64_t hotset_pins = 0;

  void reset() { *this = Metrics{}; }

  void note_promote_pages(uint64_t n) { promote_pages += n; }
  void note_promote_used(uint64_t n) { promote_used += n; }

  double precision_pct() const {
    return promote_pages ? 100.0 * (double)promote_used / (double)promote_pages : 0.0;
  }

  void add_from(const Metrics& o) {
    dram_hits += o.dram_hits;
    ssd_misses += o.ssd_misses;
    promote_bytes += o.promote_bytes;
    promote_ns += o.promote_ns;
    prefetch_pages += o.prefetch_pages;
    evicts += o.evicts;
    queries += o.queries;
    distance_comps += o.distance_comps;
    promote_pages += o.promote_pages;
    promote_used += o.promote_used;
    hotset_pins += o.hotset_pins;
  }

  void print(FILE* f = stdout) const {
    double hit = dram_hits + ssd_misses
                     ? 100.0 * (double)dram_hits / (double)(dram_hits + ssd_misses)
                     : 0.0;
    double ns_miss =
        ssd_misses ? (double)promote_ns / (double)ssd_misses : 0.0;
    fprintf(f,
            "metrics queries=%llu dram_hits=%llu ssd_misses=%llu hit_pct=%.2f "
            "promote_bytes=%llu avg_promote_ns=%.1f prefetch_pages=%llu "
            "evicts=%llu dist=%llu precision_pct=%.1f promote_pages=%llu "
            "promote_used=%llu hotset_pins=%llu\n",
            (unsigned long long)queries, (unsigned long long)dram_hits,
            (unsigned long long)ssd_misses, hit,
            (unsigned long long)promote_bytes, ns_miss,
            (unsigned long long)prefetch_pages, (unsigned long long)evicts,
            (unsigned long long)distance_comps, precision_pct(),
            (unsigned long long)promote_pages, (unsigned long long)promote_used,
            (unsigned long long)hotset_pins);
  }
};
