#pragma once
#include "common.hpp"
#include "dataset.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

// O_DIRECT-friendly aligned buffer helper.
struct AlignedBuf {
  void* p = nullptr;
  size_t bytes = 0;
  explicit AlignedBuf(size_t n) {
    bytes = n;
    if (posix_memalign(&p, 4096, n)) die("posix_memalign failed");
  }
  ~AlignedBuf() { free(p); }
  AlignedBuf(const AlignedBuf&) = delete;
  AlignedBuf& operator=(const AlignedBuf&) = delete;
};

struct VectorStoreSSD {
  int fd = -1;
  DatasetMeta meta{};
  size_t vec_bytes = 0;
  bool odirect = true;

  void open_file(const std::string& dir, bool use_odirect = true) {
    meta = read_meta(dir);
    vec_bytes = size_t(meta.dim) * sizeof(float);
    odirect = use_odirect;
    int flags = O_RDONLY;
    if (odirect) flags |= O_DIRECT;
    fd = ::open(vec_path(dir).c_str(), flags);
    if (fd < 0) {
      // Fallback if FS rejects O_DIRECT
      odirect = false;
      fd = ::open(vec_path(dir).c_str(), O_RDONLY);
      if (fd < 0) die("open vectors failed");
      std::fprintf(stderr, "WARN: O_DIRECT unavailable, using buffered reads\n");
    }
  }

  void close_file() {
    if (fd >= 0) ::close(fd);
    fd = -1;
  }

  // Read vector id into dst (dst must have dim floats). Counts SSD stats.
  void pread_vec(uint32_t id, float* dst, Stats& st) {
    off_t off = off_t(id) * off_t(vec_bytes);
    if (odirect) {
      // Align: read containing 4K pages then copy.
      off_t aligned = off & ~off_t(4095);
      size_t pad = size_t(off - aligned);
      size_t need = pad + vec_bytes;
      size_t alloc = (need + 4095) & ~size_t(4095);
      AlignedBuf buf(alloc);
      ssize_t r = ::pread(fd, buf.p, alloc, aligned);
      if (r < (ssize_t)(pad + vec_bytes)) die("O_DIRECT pread short");
      std::memcpy(dst, static_cast<char*>(buf.p) + pad, vec_bytes);
      st.ssd_bytes += alloc;
    } else {
      ssize_t r = ::pread(fd, dst, vec_bytes, off);
      if (r != (ssize_t)vec_bytes) die("pread short");
      st.ssd_bytes += vec_bytes;
    }
    st.ssd_reads += 1;
  }
};

// CXL (or DRAM) capacity cache keyed by vector id. Record-granularity.
struct RecordCache {
  int node = 2;  // CXL
  uint32_t dim = 0;
  size_t capacity_vecs = 0;
  std::unordered_map<uint32_t, float*> map;
  std::vector<uint32_t> lru_order;  // naive: scan erase from vector
  float* slab = nullptr;
  std::vector<float*> free_list;
  size_t slab_bytes = 0;

  void init(uint32_t dim_, size_t cap, int numa_node) {
    dim = dim_;
    capacity_vecs = cap;
    node = numa_node;
    slab_bytes = cap * size_t(dim) * sizeof(float);
    slab = numa_alloc_array<float>(cap * size_t(dim), node);
    free_list.reserve(cap);
    for (size_t i = 0; i < cap; ++i) free_list.push_back(slab + i * dim);
    map.reserve(cap * 2);
  }

  void clear_stats_only() {}

  void destroy() {
    map.clear();
    free_list.clear();
    numa_free_bytes(slab, slab_bytes);
    slab = nullptr;
  }

  bool contains(uint32_t id) const { return map.find(id) != map.end(); }

  float* find(uint32_t id) {
    auto it = map.find(id);
    if (it == map.end()) return nullptr;
    // crude LRU bump: append id (duplicates ok; capacity enforced on insert)
    lru_order.push_back(id);
    return it->second;
  }

  float* insert_slot(uint32_t id) {
    if (map.count(id)) return map[id];
    while (map.size() >= capacity_vecs) evict_one();
    float* slot = free_list.back();
    free_list.pop_back();
    map[id] = slot;
    lru_order.push_back(id);
    return slot;
  }

  void evict_one() {
    while (!lru_order.empty()) {
      uint32_t victim = lru_order.front();
      lru_order.erase(lru_order.begin());
      auto it = map.find(victim);
      if (it == map.end()) continue;
      // only evict if not recently re-appended: approximate — always evict found
      free_list.push_back(it->second);
      map.erase(it);
      return;
    }
  }
};

// Page-granularity cache: 4KiB pages of the vector file laid out in CXL.
struct PageCache {
  static constexpr size_t kPage = 4096;
  int node = 2;
  size_t vec_bytes = 0;
  size_t capacity_pages = 0;
  std::unordered_map<uint64_t, char*> map;  // page index -> data
  std::vector<uint64_t> lru;
  char* slab = nullptr;
  std::vector<char*> free_list;
  size_t slab_bytes = 0;

  void init(size_t vec_bytes_, size_t cap_pages, int numa_node) {
    vec_bytes = vec_bytes_;
    capacity_pages = cap_pages;
    node = numa_node;
    slab_bytes = cap_pages * kPage;
    slab = static_cast<char*>(numa_alloc_bytes(slab_bytes, node));
    free_list.reserve(cap_pages);
    for (size_t i = 0; i < cap_pages; ++i) free_list.push_back(slab + i * kPage);
  }

  void destroy() {
    map.clear();
    free_list.clear();
    numa_free_bytes(slab, slab_bytes);
    slab = nullptr;
  }

  char* find_page(uint64_t pidx) {
    auto it = map.find(pidx);
    if (it == map.end()) return nullptr;
    lru.push_back(pidx);
    return it->second;
  }

  char* insert_page(uint64_t pidx) {
    if (map.count(pidx)) return map[pidx];
    while (map.size() >= capacity_pages) {
      while (!lru.empty()) {
        uint64_t v = lru.front();
        lru.erase(lru.begin());
        auto it = map.find(v);
        if (it == map.end()) continue;
        free_list.push_back(it->second);
        map.erase(it);
        break;
      }
    }
    char* slot = free_list.back();
    free_list.pop_back();
    map[pidx] = slot;
    lru.push_back(pidx);
    return slot;
  }
};
