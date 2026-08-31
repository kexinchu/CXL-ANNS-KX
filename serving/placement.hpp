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
  size_t vec_stride = 0;  // bytes between vectors (packed or page-aligned)
  void* dram_arena = nullptr;
  size_t dram_bytes = 0;
  const uint8_t* graph_host = nullptr;
  size_t graph_host_bytes = 0;
  // Per-node packed N(u) immediately after the layout image (page-aligned).
  uint64_t bundle_off = 0;
  uint64_t bundle_stride = 0;
  uint64_t bundle_bytes = 0;

  void set_graph_host(const uint8_t* p, size_t n) {
    graph_host = p;
    graph_host_bytes = n;
  }

  void set_header(const CxanLayoutHeader* h) {
    hdr = h;
    size_t packed = (size_t)h->dim * h->vec_bytes;
    if (h->n && h->len_vectors) {
      size_t stride = (size_t)(h->len_vectors / h->n);
      vec_stride = stride >= packed ? stride : packed;
    } else {
      vec_stride = packed;
    }
  }

  const uint8_t* pq(uint32_t id) const {
    return ssd_base + hdr->off_pq + (size_t)id * hdr->pq_bytes;
  }
  const uint32_t* nbrs(uint32_t id) const {
    const uint8_t* base = graph_host ? graph_host : (ssd_base + hdr->off_graph);
    return reinterpret_cast<const uint32_t*>(base + (size_t)id * hdr->R * 4);
  }
  const uint8_t* vec(uint32_t id) const {
    return ssd_base + hdr->off_vectors + (size_t)id * vec_stride;
  }
  const uint8_t* bundle_slot(uint32_t src, uint32_t k) const {
    return ssd_base + bundle_off + (uint64_t)src * bundle_stride +
           (uint64_t)k * packed_vec_bytes();
  }
  bool has_bundle() const { return bundle_stride && bundle_bytes; }

  size_t packed_vec_bytes() const {
    return hdr ? (size_t)hdr->dim * hdr->vec_bytes : 0;
  }

  // IDs whose packed vectors overlap pages [page_lo, page_hi] (inclusive offs).
  template <typename Fn>
  void for_ids_touching_pages(uint64_t page_lo, uint64_t page_hi, size_t page_bytes,
                              Fn fn) const {
    if (!hdr || !vec_stride || !page_bytes) return;
    uint64_t v0 = hdr->off_vectors;
    uint64_t v1 = v0 + hdr->len_vectors;
    uint64_t lo = page_lo > v0 ? page_lo : v0;
    uint64_t hi = page_hi + page_bytes;
    if (hi > v1) hi = v1;
    if (lo >= hi) return;
    uint64_t rel_lo = lo - v0;
    uint64_t rel_hi = hi - v0;
    size_t packed = packed_vec_bytes();
    uint32_t i0 = 0;
    if (rel_lo >= packed) {
      uint64_t num = rel_lo - packed + 1;
      i0 = (uint32_t)((num + vec_stride - 1) / vec_stride);
    }
    uint32_t i1 = (uint32_t)((rel_hi - 1) / vec_stride);
    if (i1 >= hdr->n) i1 = hdr->n - 1;
    for (uint32_t i = i0; i <= i1; ++i) fn(i);
  }

  // IDs whose full packed vector lies inside one page (the 5-in-4KiB slots).
  template <typename Fn>
  void for_ids_contained_in_page(uint64_t page_off, size_t page_bytes, Fn fn) const {
    if (!hdr || !page_bytes) return;
    const size_t packed = packed_vec_bytes();
    if (!packed || packed > page_bytes) return;
    if (has_bundle() && page_off >= bundle_off && page_off < bundle_off + bundle_bytes) {
      const uint64_t rel = page_off - bundle_off;
      const uint32_t u = (uint32_t)(rel / bundle_stride);
      if (u >= hdr->n) return;
      const uint64_t local = rel % bundle_stride;
      const uint32_t* nb = nbrs(u);
      for (uint32_t k = 0; k < hdr->R; ++k) {
        const uint64_t a = (uint64_t)k * packed;
        const uint64_t b = a + packed;
        if (a >= local && b <= local + page_bytes) fn(nb[k]);
      }
      return;
    }
    if (!vec_stride) return;
    const uint64_t v0 = hdr->off_vectors;
    const uint64_t vlo = page_off > v0 ? page_off : v0;
    const uint64_t vhi = page_off + page_bytes;
    if (vlo >= v0 + hdr->len_vectors || vlo >= vhi) return;
    uint32_t i0 = (uint32_t)((vlo - v0 + vec_stride - 1) / vec_stride);
    uint32_t i1 = (uint32_t)((vhi - v0) / vec_stride);
    if (i1) i1--;
    if (i1 >= hdr->n) i1 = hdr->n - 1;
    for (uint32_t i = i0; i <= i1 && i < hdr->n; ++i) {
      const uint64_t a = v0 + (uint64_t)i * vec_stride;
      const uint64_t b = a + packed;
      if (a >= page_off && b <= page_off + page_bytes) fn(i);
    }
  }
};
