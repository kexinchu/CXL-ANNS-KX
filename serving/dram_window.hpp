#pragma once
#include "metrics.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <mutex>

struct DramWindow {
  static constexpr size_t kPage = 4096;
  static constexpr size_t kSpillSlots = 16;

  uint8_t* arena = nullptr;
  size_t capacity = 0;
  size_t page_bytes = kPage;
  size_t n_frames = 0;
  Metrics* metrics = nullptr;
  std::mutex mu;

  std::unordered_map<uint64_t, size_t> map;  // page_off -> frame idx
  std::vector<uint64_t> frame_key;            // key or UINT64_MAX if free
  std::vector<uint8_t> frame_pinned;          // 1 = never evict
  std::unordered_set<uint64_t> pinned_pages;
  size_t clock_hand = 0;
  size_t pin_bytes_used = 0;
  size_t pin_bytes_cap = 0;  // 0 = default 64MiB or 1/16 capacity

  std::vector<std::vector<uint8_t>> spill;
  size_t spill_hand = 0;

  void init(void* dram, size_t dram_bytes, Metrics* m, size_t page = kPage) {
    arena = static_cast<uint8_t*>(dram);
    capacity = dram_bytes;
    page_bytes = page;
    n_frames = capacity / page_bytes;
    metrics = m;
    map.clear();
    frame_key.assign(n_frames, UINT64_MAX);
    frame_pinned.assign(n_frames, 0);
    pinned_pages.clear();
    clock_hand = 0;
    pin_bytes_used = 0;
    if (pin_bytes_cap == 0) {
      size_t def = 64ull << 20;
      size_t frac = capacity / 16;
      pin_bytes_cap = def < frac ? def : frac;
    }
    spill.assign(kSpillSlots, {});
    spill_hand = 0;
  }

  // Drop unpinned pages only (keeps pinned entry/hot region across queries).
  void flush() {
    std::lock_guard<std::mutex> g(mu);
    for (size_t i = 0; i < n_frames; ++i) {
      if (frame_pinned[i]) continue;
      uint64_t key = frame_key[i];
      if (key != UINT64_MAX) map.erase(key);
      frame_key[i] = UINT64_MAX;
    }
    clock_hand = 0;
  }

  void flush_all() {
    std::lock_guard<std::mutex> g(mu);
    map.clear();
    std::fill(frame_key.begin(), frame_key.end(), UINT64_MAX);
    std::fill(frame_pinned.begin(), frame_pinned.end(), 0);
    pinned_pages.clear();
    pin_bytes_used = 0;
    clock_hand = 0;
  }

  size_t evict_frame_unlocked() {
    for (size_t t = 0; t < n_frames; ++t) {
      size_t i = clock_hand % n_frames;
      clock_hand++;
      if (frame_pinned[i]) continue;
      uint64_t key = frame_key[i];
      if (key == UINT64_MAX) return i;
      map.erase(key);
      frame_key[i] = UINT64_MAX;
      if (metrics) metrics->evicts++;
      return i;
    }
    // All frames pinned or full of pins — fall back to any free, else frame 0.
    for (size_t i = 0; i < n_frames; ++i) {
      if (frame_key[i] == UINT64_MAX) return i;
    }
    for (size_t i = 0; i < n_frames; ++i) {
      if (!frame_pinned[i]) {
        uint64_t key = frame_key[i];
        if (key != UINT64_MAX) map.erase(key);
        frame_key[i] = UINT64_MAX;
        if (metrics) metrics->evicts++;
        return i;
      }
    }
    return 0;
  }

  size_t promote_page_unlocked(const uint8_t* ssd_base, uint64_t page_off, bool pin) {
    auto it = map.find(page_off);
    if (it != map.end()) {
      if (metrics) metrics->dram_hits++;
      size_t fr = it->second;
      if (pin && !frame_pinned[fr]) {
        if (pin_bytes_used + page_bytes <= pin_bytes_cap) {
          frame_pinned[fr] = 1;
          pinned_pages.insert(page_off);
          pin_bytes_used += page_bytes;
        }
      }
      return fr;
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
    // Byte-copy: AVX memcpy from /dev/vmem0 → DAX has SIGBUS'd under concurrency.
    {
      auto* dst = arena + frame * page_bytes;
      const auto* src = ssd_base + page_off;
      for (size_t i = 0; i < page_bytes; ++i) dst[i] = src[i];
    }
    frame_key[frame] = page_off;
    frame_pinned[frame] = 0;
    map[page_off] = frame;
    if (pin && pin_bytes_used + page_bytes <= pin_bytes_cap) {
      frame_pinned[frame] = 1;
      pinned_pages.insert(page_off);
      pin_bytes_used += page_bytes;
    }
    auto t1 = std::chrono::steady_clock::now();
    if (metrics) {
      metrics->ssd_misses++;
      metrics->promote_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      metrics->promote_bytes += page_bytes;
    }
    return frame;
  }

  size_t promote_page_unlocked(const uint8_t* ssd_base, uint64_t page_off) {
    return promote_page_unlocked(ssd_base, page_off, false);
  }

  // Promote and pin all pages covering [ptr, ptr+len). Respects pin_bytes_cap.
  void pin(const uint8_t* ssd_base, const uint8_t* ssd_ptr, size_t len) {
    std::lock_guard<std::mutex> g(mu);
    if (len == 0) len = 1;
    uint64_t off = (uint64_t)(ssd_ptr - ssd_base);
    uint64_t end = off + len;
    uint64_t first = off & ~(uint64_t)(page_bytes - 1);
    uint64_t last = (end - 1) & ~(uint64_t)(page_bytes - 1);
    for (uint64_t p = first; p <= last; p += page_bytes)
      promote_page_unlocked(ssd_base, p, true);
  }

  // True if all pages covering [ptr, ptr+len) are already in the window (no promote).
  bool is_resident(const uint8_t* ssd_base, const uint8_t* ssd_ptr, size_t len) {
    std::lock_guard<std::mutex> g(mu);
    if (len == 0) len = 1;
    uint64_t off = (uint64_t)(ssd_ptr - ssd_base);
    uint64_t end = off + len;
    uint64_t first = off & ~(uint64_t)(page_bytes - 1);
    uint64_t last = (end - 1) & ~(uint64_t)(page_bytes - 1);
    for (uint64_t p = first; p <= last; p += page_bytes) {
      if (!map.count(p)) return false;
    }
    return true;
  }

  // Promote as needed and copy [ptr,ptr+len) into dst under the window lock
  // so a concurrent worker cannot evict mid-read (required for P2 async).
  void copy_through(const uint8_t* ssd_base, const uint8_t* ssd_ptr, size_t len, uint8_t* dst) {
    std::lock_guard<std::mutex> g(mu);
    if (len == 0) return;
    uint64_t off = (uint64_t)(ssd_ptr - ssd_base);
    uint64_t end = off + len;
    uint64_t first_page = off & ~(uint64_t)(page_bytes - 1);
    uint64_t last_page = (end - 1) & ~(uint64_t)(page_bytes - 1);
    for (uint64_t p = first_page; p <= last_page; p += page_bytes)
      promote_page_unlocked(ssd_base, p, false);
    size_t copied = 0;
    while (copied < len) {
      uint64_t cur = off + copied;
      uint64_t po = cur & ~(uint64_t)(page_bytes - 1);
      size_t fr = map[po];
      size_t in_page = (size_t)(cur - po);
      size_t n = std::min(len - copied, page_bytes - in_page);
      std::memcpy(dst + copied, arena + fr * page_bytes + in_page, n);
      copied += n;
    }
  }

  const uint8_t* lookup_or_promote(const uint8_t* ssd_base, const uint8_t* ssd_ptr,
                                   size_t len) {
    std::lock_guard<std::mutex> g(mu);
    if (len == 0) len = 1;
    uint64_t off = (uint64_t)(ssd_ptr - ssd_base);
    uint64_t end = off + len;
    uint64_t first_page = off & ~(uint64_t)(page_bytes - 1);
    uint64_t last_page = (end - 1) & ~(uint64_t)(page_bytes - 1);

    for (uint64_t p = first_page; p <= last_page; p += page_bytes)
      promote_page_unlocked(ssd_base, p, false);

    size_t frame0 = map[first_page];
    size_t in0 = (size_t)(off - first_page);
    if (first_page == last_page) {
      return arena + frame0 * page_bytes + in0;
    }

    size_t slot = spill_hand++ % kSpillSlots;
    spill[slot].resize(len);
    size_t copied = 0;
    while (copied < len) {
      uint64_t cur = off + copied;
      uint64_t po = cur & ~(uint64_t)(page_bytes - 1);
      size_t fr = map[po];
      size_t in_page = (size_t)(cur - po);
      size_t n = std::min(len - copied, page_bytes - in_page);
      std::memcpy(spill[slot].data() + copied, arena + fr * page_bytes + in_page, n);
      copied += n;
    }
    return spill[slot].data();
  }

  // Install already-fetched host bytes into the window (no vmem read).
  // Used by PromotePipe: worker fills host staging; search thread writes DAX.
  void install_host(const uint8_t* ssd_base, const uint8_t* ssd_ptr, size_t len,
                    const uint8_t* host) {
    std::lock_guard<std::mutex> g(mu);
    if (len == 0 || !host) return;
    uint64_t off = (uint64_t)(ssd_ptr - ssd_base);
    uint64_t end = off + len;
    uint64_t first = off & ~(uint64_t)(page_bytes - 1);
    uint64_t last = (end - 1) & ~(uint64_t)(page_bytes - 1);
    for (uint64_t p = first; p <= last; p += page_bytes) {
      uint64_t copy_lo = p < off ? off : p;
      uint64_t copy_hi = (p + page_bytes) > end ? end : (p + page_bytes);
      size_t dst_off = (size_t)(copy_lo - p);
      size_t src_off = (size_t)(copy_lo - off);
      size_t n = (size_t)(copy_hi - copy_lo);

      auto it = map.find(p);
      if (it != map.end()) {
        if (metrics) metrics->dram_hits++;
        // Merge into existing frame (shared pages across vectors).
        std::memcpy(arena + it->second * page_bytes + dst_off, host + src_off, n);
        continue;
      }
      size_t frame = n_frames;
      for (size_t i = 0; i < n_frames; ++i) {
        if (frame_key[i] == UINT64_MAX) {
          frame = i;
          break;
        }
      }
      if (frame == n_frames) frame = evict_frame_unlocked();
      auto* dst = arena + frame * page_bytes;
      std::memset(dst, 0, page_bytes);
      std::memcpy(dst + dst_off, host + src_off, n);
      frame_key[frame] = p;
      frame_pinned[frame] = 0;
      map[p] = frame;
    }
  }

  // Install one full SSD page already fetched into host (CXL-SSD→host done by caller).
  // Search thread only: host → CXL-DRAM window.
  void install_full_page(uint64_t page_off, const uint8_t* host_page) {
    std::lock_guard<std::mutex> g(mu);
    if (!host_page) return;
    if (map.count(page_off)) {
      if (metrics) metrics->dram_hits++;
      return;
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
    std::memcpy(arena + frame * page_bytes, host_page, page_bytes);
    frame_key[frame] = page_off;
    frame_pinned[frame] = 0;
    map[page_off] = frame;
    auto t1 = std::chrono::steady_clock::now();
    if (metrics) {
      metrics->ssd_misses++;
      metrics->promote_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      metrics->promote_bytes += page_bytes;
      metrics->prefetch_pages++;
    }
  }

  // Promote [ssd_ptr,len) into the CXL-DRAM window.
  // SSD→host page copies may run concurrently; DAX install is serialized by mu
  // (avoids concurrent vmem→DAX SIGBUS while keeping prefetch destination = window).
  void promote_span(const uint8_t* ssd_base, const uint8_t* ssd_ptr, size_t len) {
    if (len == 0) len = 1;
    uint64_t off = (uint64_t)(ssd_ptr - ssd_base);
    uint64_t end = off + len;
    uint64_t first = off & ~(uint64_t)(page_bytes - 1);
    uint64_t last = (end - 1) & ~(uint64_t)(page_bytes - 1);

    for (uint64_t p = first; p <= last; p += page_bytes) {
      {
        std::lock_guard<std::mutex> g(mu);
        if (map.count(p)) {
          if (metrics) metrics->dram_hits++;
          continue;
        }
      }

      // Full page from CXL-SSD → host scratch (safe to parallelize).
      alignas(64) uint8_t host_page[4096];
      if (page_bytes > sizeof(host_page)) {
        // Fallback: locked promote (should not happen with 4KiB pages).
        std::lock_guard<std::mutex> g(mu);
        promote_page_unlocked(ssd_base, p, false);
        continue;
      }
      auto t0 = std::chrono::steady_clock::now();
      // Concurrent CXL-SSD reads are OK; only DAX installs must be serialized.
      std::memcpy(host_page, ssd_base + p, page_bytes);
      auto t1 = std::chrono::steady_clock::now();

      // Host → CXL-DRAM window (sole DAX writer critical section).
      {
        std::lock_guard<std::mutex> g(mu);
        if (map.count(p)) {
          if (metrics) metrics->dram_hits++;
          continue;
        }
        size_t frame = n_frames;
        for (size_t i = 0; i < n_frames; ++i) {
          if (frame_key[i] == UINT64_MAX) {
            frame = i;
            break;
          }
        }
        if (frame == n_frames) frame = evict_frame_unlocked();
        std::memcpy(arena + frame * page_bytes, host_page, page_bytes);
        frame_key[frame] = p;
        frame_pinned[frame] = 0;
        map[p] = frame;
        if (metrics) {
          metrics->ssd_misses++;
          metrics->promote_ns +=
              std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
          metrics->promote_bytes += page_bytes;
          metrics->prefetch_pages++;
        }
      }
    }
  }

  bool try_prefetch(const uint8_t* ssd_base, const uint8_t* ssd_ptr, size_t* budget,
                    size_t len = 0) {
    if (!budget) return false;
    if (len == 0) len = page_bytes;
    uint64_t off = (uint64_t)(ssd_ptr - ssd_base);
    uint64_t end = off + len;
    uint64_t first = off & ~(uint64_t)(page_bytes - 1);
    uint64_t last = (end - 1) & ~(uint64_t)(page_bytes - 1);
    size_t need = 0;
    {
      std::lock_guard<std::mutex> g(mu);
      for (uint64_t p = first; p <= last; p += page_bytes) {
        if (!map.count(p)) need += page_bytes;
      }
    }
    if (need == 0) return true;
    if (*budget < need) return false;
    lookup_or_promote(ssd_base, ssd_ptr, len);
    if (metrics) metrics->prefetch_pages += need / page_bytes;
    *budget -= need;
    return true;
  }
};
