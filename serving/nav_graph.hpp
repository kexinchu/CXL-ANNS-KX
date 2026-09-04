#pragma once
#include "placement.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include <vector>

// Host-only 10k navigation graph. Neighbors are full-corpus IDs.
struct NavGraph {
  uint32_t n0 = 0, dim = 0, R = 0;
  std::vector<uint32_t> ids;
  std::vector<uint8_t> recs;  // n0 * payload (vec + nnbrs + nbrs[R])
  size_t rec_bytes = 0;

  bool loaded() const { return n0 > 0 && !recs.empty(); }

  bool load(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(&n0), 4);
    in.read(reinterpret_cast<char*>(&dim), 4);
    in.read(reinterpret_cast<char*>(&R), 4);
    if (!in || n0 == 0 || n0 > 1000000) return false;
    rec_bytes = Placement::diskann_payload_bytes(dim, 4, R);
    ids.resize(n0);
    in.read(reinterpret_cast<char*>(ids.data()), (size_t)n0 * 4);
    recs.resize((size_t)n0 * rec_bytes);
    in.read(reinterpret_cast<char*>(recs.data()), recs.size());
    return (bool)in;
  }

  const float* vec_i(uint32_t i) const {
    return reinterpret_cast<const float*>(recs.data() + (size_t)i * rec_bytes);
  }
  const uint32_t* nbrs_i(uint32_t i) const {
    return reinterpret_cast<const uint32_t*>(recs.data() + (size_t)i * rec_bytes +
                                             (size_t)dim * 4 + 4);
  }

  static float mips_neg(const float* a, const float* b, uint32_t d) {
    float s = 0;
    for (uint32_t i = 0; i < d; ++i) s += a[i] * b[i];
    return -s;
  }

  // Random 10k is too sparse for induced edges. Entry = exact 10k scan (cheap).
  uint32_t search_entry(const float* q, uint32_t L0 = 64) const {
    (void)L0;
    if (!loaded()) return 0;
    uint32_t best_i = 0;
    float best = mips_neg(vec_i(0), q, dim);
    for (uint32_t i = 1; i < n0; ++i) {
      float d = mips_neg(vec_i(i), q, dim);
      if (d < best) {
        best = d;
        best_i = i;
      }
    }
    return ids[best_i];
  }
};
