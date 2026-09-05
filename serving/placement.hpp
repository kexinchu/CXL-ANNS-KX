#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <vector>

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
// 32 GiB /dev/dax0.0 cannot hold 10M×4096; 10M×2048 + 4 KiB header ≈ 19.07 GiB.
static constexpr size_t kDiskannStride = 2048;

struct Placement {
  const CxanLayoutHeader* hdr = nullptr;
  const uint8_t* ssd_base = nullptr;  // mapped image base
  size_t ssd_bytes = 0;
  size_t vec_stride = 0;  // bytes between vectors (packed or page-aligned)
  void* dram_arena = nullptr;
  size_t dram_bytes = 0;
  const uint8_t* graph_host = nullptr;
  size_t graph_host_bytes = 0;
  bool diskann_layout = false;  // vec + nbrs in one STRIDE record
  // Physical slot of logical id (empty = identity). Neighbor IDs stay logical.
  std::vector<uint32_t> id_to_slot;
  std::vector<uint32_t> slot_to_id;
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
    const size_t need = diskann_payload_bytes(h->dim, h->vec_bytes, h->R);
    diskann_layout = (h->version >= 2) || (h->len_graph == 0 && vec_stride >= need);
  }

  static size_t diskann_payload_bytes(uint32_t dim, uint32_t vec_bytes, uint32_t R) {
    return (size_t)dim * vec_bytes + 4 + (size_t)R * 4;
  }
  static size_t diskann_stride_for(uint32_t dim, uint32_t vec_bytes, uint32_t R, size_t want) {
    size_t need = diskann_payload_bytes(dim, vec_bytes, R);
    size_t s = want ? want : 4096;
    while (s < need) s *= 2;
    return s;
  }
  uint32_t slot_of(uint32_t id) const {
    if (id_to_slot.empty() || id >= id_to_slot.size()) return id;
    return id_to_slot[id];
  }
  uint32_t id_of_slot(uint32_t slot) const {
    if (slot_to_id.empty() || slot >= slot_to_id.size()) return slot;
    return slot_to_id[slot];
  }
  // Other occupant of the same 4K page (2-in-4K → slot xor 1).
  uint32_t page_sibling(uint32_t id) const {
    if (!hdr || !vec_stride) return id;
    const uint32_t spp = (uint32_t)(4096 / vec_stride);
    if (spp < 2) return id;
    const uint32_t s = slot_of(id);
    const uint32_t sib_slot = (s ^ 1u);
    if (sib_slot >= hdr->n) return id;
    return id_of_slot(sib_slot);
  }
  bool load_id_slot_map(const char* path) {
    if (!hdr || !path || !path[0]) return false;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint32_t> slots((size_t)hdr->n);
    const size_t got = std::fread(slots.data(), 4, (size_t)hdr->n, f);
    std::fclose(f);
    if (got != (size_t)hdr->n) return false;
    std::vector<uint8_t> seen((size_t)hdr->n, 0);
    slot_to_id.assign((size_t)hdr->n, 0);
    for (uint32_t i = 0; i < hdr->n; ++i) {
      const uint32_t s = slots[i];
      if (s >= hdr->n || seen[s]) return false;
      seen[s] = 1;
      slot_to_id[s] = i;
    }
    id_to_slot = std::move(slots);
    return true;
  }
  const uint8_t* entry(uint32_t id) const {
    return ssd_base + hdr->off_vectors + (size_t)slot_of(id) * vec_stride;
  }

  const uint8_t* pq(uint32_t id) const {
    return ssd_base + hdr->off_pq + (size_t)id * hdr->pq_bytes;
  }
  const uint32_t* nbrs(uint32_t id) const {
    if (diskann_layout) {
      if (graph_host && hdr) {
        const size_t off = (size_t)id * (size_t)hdr->R * 4;
        if (off + (size_t)hdr->R * 4 <= graph_host_bytes)
          return reinterpret_cast<const uint32_t*>(graph_host + off);
      }
      const size_t vb = (size_t)hdr->dim * hdr->vec_bytes;
      return reinterpret_cast<const uint32_t*>(entry(id) + vb + 4);
    }
    const uint8_t* base = graph_host ? graph_host : (ssd_base + hdr->off_graph);
    return reinterpret_cast<const uint32_t*>(base + (size_t)id * hdr->R * 4);
  }
  uint32_t nnbrs(uint32_t id) const {
    if (diskann_layout) {
      if (graph_host && hdr) return hdr->R;
      const size_t vb = (size_t)hdr->dim * hdr->vec_bytes;
      return *reinterpret_cast<const uint32_t*>(entry(id) + vb);
    }
    return hdr->R;
  }
  const uint8_t* vec(uint32_t id) const {
    if (diskann_layout) return entry(id);
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
    if (diskann_layout && vec_stride) {
      const uint64_t v0 = hdr->off_vectors;
      if (page_off + page_bytes <= v0) return;
      uint32_t i0 = 0;
      if (page_off > v0) i0 = (uint32_t)((page_off - v0 + vec_stride - 1) / vec_stride);
      uint32_t i1 = 0;
      if (page_off + page_bytes > v0)
        i1 = (uint32_t)((page_off + page_bytes - v0) / vec_stride);
      if (i1) i1--;
      if (i1 >= hdr->n) i1 = hdr->n - 1;
      for (uint32_t i = i0; i <= i1 && i < hdr->n; ++i) {
        const uint64_t a = v0 + (uint64_t)i * vec_stride;
        const uint64_t b = a + vec_stride;
        if (a >= page_off && b <= page_off + page_bytes) fn(id_of_slot(i));
      }
      return;
    }
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
