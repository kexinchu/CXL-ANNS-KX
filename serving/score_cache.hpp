#pragma once
#include <cstdint>
#include <vector>

// Partition neighbor IDs by whether their 4K page was present in the vmem
// software cache (hit[page_ix]=1). Hits score from the ioctl bounce; misses
// go through hide. Never mmap-faults.
inline void score_cache_split(const uint32_t* ids, const uint32_t* page_ix, uint32_t n_ids,
                              const uint8_t* hit, uint32_t n_pages,
                              std::vector<uint32_t>& now, std::vector<uint32_t>& later) {
  now.clear();
  later.clear();
  now.reserve(n_ids);
  later.reserve(n_ids);
  for (uint32_t i = 0; i < n_ids; ++i) {
    const uint32_t p = page_ix[i];
    if (p < n_pages && hit[p])
      now.push_back(ids[i]);
    else
      later.push_back(ids[i]);
  }
}
