// Build DiskANN/PipeANN FixedChunk PQ (L2 k-means on centered data).
//   g++ -O3 -mavx2 -fopenmp -std=c++17 tools/build_diskann_pq.cpp -o tools/build_diskann_pq
//   ./tools/build_diskann_pq --data mem_R32.data --out-prefix idx_t2i64 --chunks 64
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

static constexpr uint32_t kK = 256;
static constexpr uint32_t kIters = 15;
static constexpr uint64_t kSector = 4096;

static void die(const char* m) {
  perror(m);
  std::exit(1);
}

template <typename T>
static uint64_t write_bin(std::fstream& out, uint64_t off, const T* data, uint32_t nr, uint32_t nc) {
  out.seekp((std::streamoff)off);
  out.write(reinterpret_cast<const char*>(&nr), 4);
  out.write(reinterpret_cast<const char*>(&nc), 4);
  out.write(reinterpret_cast<const char*>(data), (std::streamsize)((size_t)nr * nc * sizeof(T)));
  return 8ull + (uint64_t)nr * nc * sizeof(T);
}

static void kmeans_pp(const float* x, uint32_t n, uint32_t d, float* cents, std::mt19937& rng) {
  std::vector<float> min_d((size_t)n, 1e30f);
  std::uniform_int_distribution<uint32_t> uni(0, n - 1);
  uint32_t first = uni(rng);
  std::memcpy(cents, x + (size_t)first * d, (size_t)d * 4);
  for (uint32_t c = 1; c < kK; ++c) {
    double sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const float* p = x + (size_t)i * d;
      const float* q = cents + (size_t)(c - 1) * d;
      float dist = 0;
      for (uint32_t t = 0; t < d; ++t) {
        float v = p[t] - q[t];
        dist += v * v;
      }
      if (dist < min_d[i]) min_d[i] = dist;
      sum += min_d[i];
    }
    double r = std::uniform_real_distribution<double>(0, sum)(rng);
    uint32_t pick = n - 1;
    for (uint32_t i = 0; i < n; ++i) {
      r -= min_d[i];
      if (r <= 0) {
        pick = i;
        break;
      }
    }
    std::memcpy(cents + (size_t)c * d, x + (size_t)pick * d, (size_t)d * 4);
  }
}

static void kmeans(const float* x, uint32_t n, uint32_t d, float* cents, std::mt19937 rng) {
  kmeans_pp(x, n, d, cents, rng);
  std::vector<uint32_t> assign((size_t)n, 0);
  std::vector<float> acc((size_t)kK * d, 0);
  std::vector<uint32_t> cnt(kK, 0);
  for (uint32_t it = 0; it < kIters; ++it) {
    std::fill(acc.begin(), acc.end(), 0.f);
    std::fill(cnt.begin(), cnt.end(), 0);
    for (uint32_t i = 0; i < n; ++i) {
      const float* p = x + (size_t)i * d;
      float best = 1e30f;
      uint32_t bi = 0;
      for (uint32_t c = 0; c < kK; ++c) {
        const float* q = cents + (size_t)c * d;
        float dist = 0;
        for (uint32_t t = 0; t < d; ++t) {
          float v = p[t] - q[t];
          dist += v * v;
        }
        if (dist < best) {
          best = dist;
          bi = c;
        }
      }
      assign[i] = bi;
      cnt[bi]++;
      float* a = acc.data() + (size_t)bi * d;
      for (uint32_t t = 0; t < d; ++t) a[t] += p[t];
    }
    for (uint32_t c = 0; c < kK; ++c) {
      if (!cnt[c]) continue;
      float inv = 1.f / (float)cnt[c];
      for (uint32_t t = 0; t < d; ++t) cents[(size_t)c * d + t] = acc[(size_t)c * d + t] * inv;
    }
  }
}

int main(int argc, char** argv) {
  const char* data = nullptr;
  const char* prefix = nullptr;
  uint32_t chunks = 64;
  uint32_t train_n = 256000;
  uint32_t seed = 42;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--data") data = argv[++i];
    else if (a == "--out-prefix") prefix = argv[++i];
    else if (a == "--chunks") chunks = (uint32_t)atoi(argv[++i]);
    else if (a == "--train") train_n = (uint32_t)atoi(argv[++i]);
    else if (a == "--seed") seed = (uint32_t)atoi(argv[++i]);
    else {
      fprintf(stderr, "unknown %s\n", a.c_str());
      return 2;
    }
  }
  if (!data || !prefix) {
    fprintf(stderr, "usage: build_diskann_pq --data mem.bin --out-prefix path/idx --chunks 64\n");
    return 2;
  }

  std::ifstream in(data, std::ios::binary);
  if (!in) die("open data");
  uint32_t n = 0, dim = 0;
  in.read(reinterpret_cast<char*>(&n), 4);
  in.read(reinterpret_cast<char*>(&dim), 4);
  if (!in || n == 0 || dim == 0) {
    fprintf(stderr, "bad data header\n");
    return 2;
  }
  if (chunks == 0 || chunks > dim) {
    fprintf(stderr, "chunks %u vs dim %u\n", chunks, dim);
    return 2;
  }
  fprintf(stderr, "data n=%u dim=%u chunks=%u train=%u\n", n, dim, chunks, train_n);

  std::vector<float> all;
  all.resize((size_t)n * dim);
  in.read(reinterpret_cast<char*>(all.data()), (std::streamsize)(all.size() * 4));
  if (!in) die("read data");
  in.close();

  if (train_n > n) train_n = n;
  std::mt19937 rng(seed);
  std::vector<uint32_t> pick(n);
  for (uint32_t i = 0; i < n; ++i) pick[i] = i;
  std::shuffle(pick.begin(), pick.end(), rng);
  std::vector<float> train((size_t)train_n * dim);
  for (uint32_t i = 0; i < train_n; ++i)
    std::memcpy(train.data() + (size_t)i * dim, all.data() + (size_t)pick[i] * dim, (size_t)dim * 4);

  std::vector<float> centroid(dim, 0);
  for (uint32_t i = 0; i < train_n; ++i)
    for (uint32_t d = 0; d < dim; ++d) centroid[d] += train[(size_t)i * dim + d];
  for (uint32_t d = 0; d < dim; ++d) centroid[d] /= (float)train_n;
  for (uint32_t i = 0; i < train_n; ++i)
    for (uint32_t d = 0; d < dim; ++d) train[(size_t)i * dim + d] -= centroid[d];

  const uint32_t low = dim / chunks;
  const uint32_t high = (dim + chunks - 1) / chunks;
  const uint32_t n_high = dim - low * chunks;
  std::vector<uint32_t> off(chunks + 1, 0);
  for (uint32_t b = 0; b < chunks; ++b) off[b + 1] = off[b] + (b < n_high ? high : low);

  std::vector<float> tables((size_t)kK * dim, 0);
#ifdef _OPENMP
#pragma omp parallel for schedule(static, 1)
#endif
  for (int32_t c = 0; c < (int32_t)chunks; ++c) {
    const uint32_t lo = off[c], hi = off[c + 1], cd = hi - lo;
    if (!cd) continue;
    std::vector<float> cx((size_t)train_n * cd);
    for (uint32_t i = 0; i < train_n; ++i)
      std::memcpy(cx.data() + (size_t)i * cd, train.data() + (size_t)i * dim + lo, (size_t)cd * 4);
    std::vector<float> cents((size_t)kK * cd, 0);
    std::mt19937 lrng(seed + (uint32_t)c * 17u);
    kmeans(cx.data(), train_n, cd, cents.data(), lrng);
    for (uint32_t k = 0; k < kK; ++k)
      std::memcpy(tables.data() + (size_t)k * dim + lo, cents.data() + (size_t)k * cd, (size_t)cd * 4);
  }
  fprintf(stderr, "kmeans done\n");

  const std::string piv = std::string(prefix) + "_pq_pivots.bin";
  const std::string cmp = std::string(prefix) + "_pq_compressed.bin";
  std::fstream po(piv, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  if (!po) die("open pivots");
  std::vector<char> pad((size_t)kSector, 0);
  po.write(pad.data(), (std::streamsize)kSector);
  uint64_t offs[5] = {};
  offs[0] = kSector;
  offs[1] = offs[0] + write_bin(po, offs[0], tables.data(), kK, dim);
  offs[2] = offs[1] + write_bin(po, offs[1], centroid.data(), dim, 1);
  std::vector<uint32_t> rearr(dim);
  for (uint32_t d = 0; d < dim; ++d) rearr[d] = d;
  offs[3] = offs[2] + write_bin(po, offs[2], rearr.data(), dim, 1);
  offs[4] = offs[3] + write_bin(po, offs[3], off.data(), chunks + 1, 1);
  write_bin(po, 0, offs, 5, 1);
  po.close();
  fprintf(stderr, "wrote %s size=%llu\n", piv.c_str(), (unsigned long long)offs[4]);

  std::ofstream co(cmp, std::ios::binary);
  if (!co) die("open compressed");
  uint32_t n32 = n, c32 = chunks;
  co.write(reinterpret_cast<const char*>(&n32), 4);
  co.write(reinterpret_cast<const char*>(&c32), 4);

  const uint32_t blk = 65536;
  std::vector<uint8_t> codes((size_t)blk * chunks);
  for (uint32_t base = 0; base < n; base += blk) {
    const uint32_t nb = std::min(blk, n - base);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int64_t i = 0; i < (int64_t)nb; ++i) {
      const float* x = all.data() + ((size_t)base + (size_t)i) * dim;
      uint8_t* outc = codes.data() + (size_t)i * chunks;
      for (uint32_t c = 0; c < chunks; ++c) {
        const uint32_t lo = off[c], hi = off[c + 1];
        float best = 1e30f;
        uint32_t bi = 0;
        for (uint32_t k = 0; k < kK; ++k) {
          const float* q = tables.data() + (size_t)k * dim + lo;
          float dist = 0;
          for (uint32_t t = lo; t < hi; ++t) {
            float v = (x[t] - centroid[t]) - q[t - lo];
            dist += v * v;
          }
          if (dist < best) {
            best = dist;
            bi = k;
          }
        }
        outc[c] = (uint8_t)bi;
      }
    }
    co.write(reinterpret_cast<const char*>(codes.data()), (std::streamsize)nb * chunks);
    if ((base & ((1u << 20) - 1)) == 0)
      fprintf(stderr, "encode %u / %u\n", base, n);
  }
  co.close();
  fprintf(stderr, "wrote %s\n", cmp.c_str());
  return 0;
}
