#pragma once
// DiskANN / PipeANN FixedChunk PQ (MIPS ADC). Codes are stored in search-ID
// order. If the file is old-id (mem_R32), pass new_to_old to permute.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

inline thread_local const float* g_pq_tls_lut = nullptr;

struct PqTable {
  static constexpr uint32_t kCentroids = 256;

  uint32_t n = 0;
  uint32_t dim = 0;
  uint32_t nchunks = 0;
  std::vector<float> tables_T;   // dim * 256, col-major per dim
  std::vector<uint32_t> chunk_off;
  std::vector<uint8_t> codes;    // n * nchunks
  std::vector<float> lut;        // nchunks * 256, filled by begin_query_ip

  bool loaded() const { return n && nchunks && !codes.empty() && !lut.empty(); }

  bool load_pivots(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint32_t nr = 0, nc = 0;
    in.read(reinterpret_cast<char*>(&nr), 4);
    in.read(reinterpret_cast<char*>(&nc), 4);
    if (!in || nr != 5 || nc != 1) return false;
    uint64_t offs[5] = {};
    in.read(reinterpret_cast<char*>(offs), sizeof(offs));
    if (!in) return false;

    if (!read_bin_at(in, offs[0], tables_row_, nr, nc)) return false;
    if (nr != kCentroids) return false;
    dim = nc;
    std::vector<float> centroid;
    uint32_t cr = 0, cc = 0;
    if (!read_bin_at(in, offs[1], centroid, cr, cc)) return false;
    if (cr != dim || cc != 1) return false;
    std::vector<uint32_t> rearrange;
    if (!read_bin_at(in, offs[2], rearrange, cr, cc)) return false;
    if (cr != dim || cc != 1) return false;
    (void)rearrange;
    if (!read_bin_at(in, offs[3], chunk_off, cr, cc)) return false;
    if (cc != 1 || cr < 2) return false;
    nchunks = cr - 1;
    if (chunk_off.back() != dim) return false;

    tables_T.assign((size_t)dim * kCentroids, 0.f);
    for (uint32_t i = 0; i < kCentroids; ++i) {
      for (uint32_t d = 0; d < dim; ++d)
        tables_T[(size_t)d * kCentroids + i] = tables_row_[(size_t)i * dim + d];
    }
    tables_row_.clear();
    lut.assign((size_t)nchunks * kCentroids, 0.f);
    return true;
  }

  bool load_codes(const char* path, const uint32_t* new_to_old, uint32_t map_n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint32_t npts = 0, nc = 0;
    in.read(reinterpret_cast<char*>(&npts), 4);
    in.read(reinterpret_cast<char*>(&nc), 4);
    if (!in || npts == 0 || nc == 0 || nc != nchunks) return false;
    n = npts;
    std::vector<uint8_t> raw((size_t)n * nc);
    in.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)raw.size());
    if (!in) return false;
    if (new_to_old && map_n == n) {
      codes.resize(raw.size());
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t old = new_to_old[i];
        if (old >= n) return false;
        std::memcpy(codes.data() + (size_t)i * nc, raw.data() + (size_t)old * nc, nc);
      }
    } else {
      codes = std::move(raw);
    }
    return true;
  }

  void fill_lut_ip(const float* q, float* dst) const {
    std::memset(dst, 0, (size_t)nchunks * kCentroids * sizeof(float));
    for (uint32_t c = 0; c < nchunks; ++c) {
      float* chunk = dst + (size_t)c * kCentroids;
      for (uint32_t d = chunk_off[c]; d < chunk_off[c + 1]; ++d) {
        const float* centers = tables_T.data() + (size_t)d * kCentroids;
        const float qv = q[d];
        for (uint32_t i = 0; i < kCentroids; ++i) chunk[i] -= qv * centers[i];
      }
    }
  }

  void begin_query_ip(const float* q) {
    thread_local std::vector<float> tls;
    tls.resize((size_t)nchunks * kCentroids);
    fill_lut_ip(q, tls.data());
    g_pq_tls_lut = tls.data();
  }

  float dist(uint32_t id, const float* qlut) const {
    const uint8_t* c = codes.data() + (size_t)id * nchunks;
    float a = 0;
    for (uint32_t ch = 0; ch < nchunks; ++ch) a += qlut[(size_t)ch * kCentroids + c[ch]];
    return a;
  }

  float dist(uint32_t id) const { return dist(id, g_pq_tls_lut ? g_pq_tls_lut : lut.data()); }

  void dist_many(const uint32_t* ids, uint32_t n_ids, float* out) const {
    for (uint32_t i = 0; i < n_ids; ++i) out[i] = dist(ids[i]);
  }

 private:
  std::vector<float> tables_row_;

  template <typename T>
  static bool read_bin_at(std::ifstream& in, uint64_t off, std::vector<T>& out, uint32_t& nr,
                          uint32_t& nc) {
    in.clear();
    in.seekg((std::streamoff)off);
    in.read(reinterpret_cast<char*>(&nr), 4);
    in.read(reinterpret_cast<char*>(&nc), 4);
    if (!in || nr == 0 || nc == 0) return false;
    out.resize((size_t)nr * nc);
    in.read(reinterpret_cast<char*>(out.data()), (std::streamsize)(out.size() * sizeof(T)));
    return (bool)in;
  }
};
