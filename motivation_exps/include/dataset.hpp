#pragma once
#include "common.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <string>
#include <vector>

struct DatasetMeta {
  uint32_t n = 0;
  uint32_t dim = 0;
  uint32_t degree = 0;
  uint32_t entry = 0;
};

inline std::string meta_path(const std::string& dir) { return dir + "/meta.bin"; }
inline std::string vec_path(const std::string& dir) { return dir + "/vectors.bin"; }
inline std::string graph_path(const std::string& dir) { return dir + "/graph.bin"; }

inline void write_meta(const std::string& dir, const DatasetMeta& m) {
  std::ofstream out(meta_path(dir), std::ios::binary);
  out.write(reinterpret_cast<const char*>(&m), sizeof(m));
}

inline DatasetMeta read_meta(const std::string& dir) {
  DatasetMeta m{};
  std::ifstream in(meta_path(dir), std::ios::binary);
  if (!in) die("cannot read meta: " + meta_path(dir));
  in.read(reinterpret_cast<char*>(&m), sizeof(m));
  return m;
}

// Generate synthetic unit vectors + approximate RNG graph (LSH-ish by random projection buckets).
inline void generate_dataset(const std::string& dir, uint32_t n, uint32_t dim,
                             uint32_t degree, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> nd(0.f, 1.f);

  DatasetMeta meta{n, dim, degree, 0};
  write_meta(dir, meta);

  // Vectors on SSD
  {
    int fd = ::open(vec_path(dir).c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) die("open vectors for write failed");
    std::vector<float> v(dim);
    for (uint32_t i = 0; i < n; ++i) {
      double norm = 0;
      for (uint32_t d = 0; d < dim; ++d) {
        v[d] = nd(rng);
        norm += double(v[d]) * double(v[d]);
      }
      norm = std::sqrt(std::max(norm, 1e-12));
      for (uint32_t d = 0; d < dim; ++d) v[d] = float(v[d] / norm);
      if (::write(fd, v.data(), dim * sizeof(float)) !=
          (ssize_t)(dim * sizeof(float)))
        die("write vector failed");
    }
    ::close(fd);
  }

  // Graph: for each node, connect to `degree` neighbors with similar random key
  // (gives locality + Zipf-friendly hot entry region).
  std::vector<float> keys(n);
  for (uint32_t i = 0; i < n; ++i) keys[i] = nd(rng);
  std::vector<uint32_t> order(n);
  for (uint32_t i = 0; i < n; ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
  std::vector<uint32_t> rank(n);
  for (uint32_t i = 0; i < n; ++i) rank[order[i]] = i;

  meta.entry = order[n / 2];
  write_meta(dir, meta);

  int gfd = ::open(graph_path(dir).c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (gfd < 0) die("open graph for write failed");
  std::vector<uint32_t> nbrs(degree);
  std::uniform_int_distribution<int> local(-int(degree) * 4, int(degree) * 4);
  std::uniform_int_distribution<uint32_t> any(0, n - 1);
  for (uint32_t i = 0; i < n; ++i) {
    int base = int(rank[i]);
    for (uint32_t k = 0; k < degree; ++k) {
      uint32_t nb;
      // 50% local (clustered) + 50% random long-range → colder working set
      if ((rng() & 1u) == 0u) {
        int j = base + local(rng);
        if (j < 0) j = 0;
        if (j >= int(n)) j = int(n) - 1;
        nb = order[j];
      } else {
        nb = any(rng);
      }
      if (nb == i) nb = (i + 1) % n;
      nbrs[k] = nb;
    }
    if (::write(gfd, nbrs.data(), degree * sizeof(uint32_t)) !=
        (ssize_t)(degree * sizeof(uint32_t)))
      die("write graph failed");
  }
  ::close(gfd);
  std::printf("generated n=%u dim=%u degree=%u entry=%u at %s\n", n, dim, degree,
              meta.entry, dir.c_str());
}

struct Graph {
  DatasetMeta meta{};
  std::vector<uint32_t> nbr;  // n * degree, kept in DRAM (small)

  void load(const std::string& dir) {
    meta = read_meta(dir);
    nbr.resize(size_t(meta.n) * meta.degree);
    int fd = ::open(graph_path(dir).c_str(), O_RDONLY);
    if (fd < 0) die("open graph failed");
    size_t bytes = nbr.size() * sizeof(uint32_t);
    size_t off = 0;
    while (off < bytes) {
      ssize_t r = ::read(fd, reinterpret_cast<char*>(nbr.data()) + off, bytes - off);
      if (r <= 0) die("read graph failed");
      off += size_t(r);
    }
    ::close(fd);
  }

  const uint32_t* neighbors(uint32_t id) const {
    return nbr.data() + size_t(id) * meta.degree;
  }
};

inline float l2_sq(const float* a, const float* b, uint32_t dim) {
  float s = 0;
  for (uint32_t i = 0; i < dim; ++i) {
    float d = a[i] - b[i];
    s += d * d;
  }
  return s;
}
