#pragma once
#include <cstdint>
#include <cstddef>

#pragma pack(push, 1)
struct CxanLayoutHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t n;
  uint32_t dim;
  uint32_t R;
  uint32_t pq_bytes;
  uint32_t vec_bytes;
  uint32_t entry_id;
  uint32_t entry_nodes;
  uint32_t pad;
  uint64_t off_pq, len_pq;
  uint64_t off_graph, len_graph;
  uint64_t off_vectors, len_vectors;
  uint64_t off_pivots, len_pivots;
  uint64_t checksum;
};
#pragma pack(pop)

static constexpr uint64_t kCxanMagic = 0x314e415843ull;

struct Placement {
  const CxanLayoutHeader* hdr = nullptr;
  const uint8_t* ssd_base = nullptr;  // mapped image base
  size_t ssd_bytes = 0;
  void* dram_arena = nullptr;
  size_t dram_bytes = 0;

  const uint8_t* pq(uint32_t id) const {
    return ssd_base + hdr->off_pq + (size_t)id * hdr->pq_bytes;
  }
  const uint32_t* nbrs(uint32_t id) const {
    return reinterpret_cast<const uint32_t*>(ssd_base + hdr->off_graph +
                                             (size_t)id * hdr->R * 4);
  }
  const uint8_t* vec(uint32_t id) const {
    return ssd_base + hdr->off_vectors + (size_t)id * hdr->dim * hdr->vec_bytes;
  }
};
