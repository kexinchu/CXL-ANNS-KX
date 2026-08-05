#pragma once
#include "metrics.hpp"

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <mutex>

struct DramWindow {
  static constexpr size_t kPage = 4096;

  uint8_t* arena = nullptr;
  size_t capacity = 0;
  size_t page_bytes = kPage;
  size_t n_frames = 0;
  Metrics* metrics = nullptr;
  std::mutex mu;

  std::unordered_map<uint64_t, size_t> map;  // page_off -> frame idx
  std::vector<uint64_t> frame_key;            // key or UINT64_MAX if free
  size_t clock_hand = 0;

  void init(void* dram, size_t dram_bytes, Metrics* m, size_t page = kPage) {
    arena = static_cast<uint8_t*>(dram);
    capacity = dram_bytes;
    page_bytes = page;
    n_frames = capacity / page_bytes;
    metrics = m;
    map.clear();
    frame_key.assign(n_frames, UINT64_MAX);
    clock_hand = 0;
  }

  size_t evict_frame_unlocked() {
    for (size_t t = 0; t < n_frames; ++t) {
      size_t i = clock_hand % n_frames;
      clock_hand++;
      uint64_t key = frame_key[i];
      if (key == UINT64_MAX) return i;
      map.erase(key);
      frame_key[i] = UINT64_MAX;
      if (metrics) metrics->evicts++;
      return i;
    }
    return 0;
  }

  const uint8_t* lookup_or_promote(const uint8_t* ssd_base, const uint8_t* ssd_ptr,
                                   size_t /*len*/) {
    std::lock_guard<std::mutex> g(mu);
    uint64_t off = (uint64_t)(ssd_ptr - ssd_base);
    uint64_t page_off = off & ~(uint64_t)(page_bytes - 1);
    auto it = map.find(page_off);
    if (it != map.end()) {
      if (metrics) metrics->dram_hits++;
      return arena + it->second * page_bytes + (size_t)(off - page_off);
    }

    auto t0 = std::chrono::steady_clock::now();
    size_t frame = n_frames;
    for (size_t i = 0; i < n_frames; ++i) {
      if (frame_key[i] == UINT64_MAX) {
        frame = i;
        break;
      }
    }
    if (frame == n_frames) frame = evict_frame_unlocked();

    std::memcpy(arena + frame * page_bytes, ssd_base + page_off, page_bytes);
    frame_key[frame] = page_off;
    map[page_off] = frame;
    auto t1 = std::chrono::steady_clock::now();
    if (metrics) {
      metrics->ssd_misses++;
      metrics->promote_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      metrics->promote_bytes += page_bytes;
    }
    return arena + frame * page_bytes + (size_t)(off - page_off);
  }

  bool try_prefetch(const uint8_t* ssd_base, const uint8_t* ssd_ptr, size_t* budget) {
    if (!budget || *budget < page_bytes) return false;
    uint64_t off = (uint64_t)(ssd_ptr - ssd_base);
    uint64_t page_off = off & ~(uint64_t)(page_bytes - 1);
    {
      std::lock_guard<std::mutex> g(mu);
      if (map.count(page_off)) return true;
    }
    lookup_or_promote(ssd_base, ssd_base + page_off, page_bytes);
    if (metrics) metrics->prefetch_pages++;
    *budget -= page_bytes;
    return true;
  }
};
