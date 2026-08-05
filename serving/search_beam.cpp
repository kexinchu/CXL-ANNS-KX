// Beam search over CXAN layout via 1GiB DRAM window + prefetch policies.
// Build:
//   g++ -O2 -std=c++17 -pthread -I. serving/search_beam.cpp -o serving/search_beam -lnuma
//
// Example:
//   source tools/cap_enforce.sh
//   ./serving/search_beam --image .../serving_layout_200k.bin --entry .../serving_entry_200k.bin \
//       --queries /mnt/disk0/chukexin_motivation/diskann_data/query.bin \
//       --policy P0 --budget 262144 --beam 32 --k 10 --nprobe 50

#include "serving/dram_window.hpp"
#include "serving/placement.hpp"
#include "serving/prefetch.hpp"
#include "serving/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct EntryGraph {
  uint32_t entry_id = 0;
  uint32_t R = 0;
  std::vector<uint32_t> nodes;
  // optional host adjacency unused by search beyond warm list
};

static void die(const char* s) {
  perror(s);
  std::exit(2);
}

static void* map_file_ro(const char* path, size_t* out_len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) die("open image");
  struct stat st {};
  if (fstat(fd, &st) != 0) die("fstat");
  void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED) die("mmap image");
  close(fd);
  *out_len = (size_t)st.st_size;
  return p;
}

static void* map_dram(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) die("mmap dram");
  unsigned long nodemask = 1UL << 1;
  if (mbind(p, bytes, MPOL_BIND, &nodemask, sizeof(nodemask) * 8,
            MPOL_MF_MOVE | MPOL_MF_STRICT) != 0)
    perror("mbind warn");
  auto* b = static_cast<volatile char*>(p);
  for (size_t off = 0; off < bytes; off += 2 * 1024 * 1024) b[off] = 0;
  return p;
}

static EntryGraph load_entry(const char* path) {
  EntryGraph eg;
  std::ifstream in(path, std::ios::binary);
  uint32_t en = 0, R = 0, entry = 0;
  in.read((char*)&en, 4);
  in.read((char*)&R, 4);
  in.read((char*)&entry, 4);
  eg.nodes.resize(en);
  in.read((char*)eg.nodes.data(), en * 4);
  eg.R = R;
  eg.entry_id = entry;
  return eg;
}

static float pq_l2(const uint8_t* a, const uint8_t* b, uint32_t m) {
  uint32_t s = 0;
  for (uint32_t i = 0; i < m; ++i) {
    int d = (int)a[i] - (int)b[i];
    s += (uint32_t)(d * d);
  }
  return (float)s;
}

#if defined(__FLT16_MAX__)
static float f16_to_f32(uint16_t h) {
  _Float16 x;
  std::memcpy(&x, &h, 2);
  return (float)x;
}
#else
static float f16_to_f32(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1f;
  uint32_t mant = h & 0x3ff;
  uint32_t out;
  if (exp == 0)
    out = sign;
  else if (exp == 31)
    out = sign | 0x7f800000u | (mant << 13);
  else
    out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
  float f;
  std::memcpy(&f, &out, 4);
  return f;
}
#endif

static float vec_l2(const uint8_t* raw, const float* q, uint32_t dim, uint32_t vec_bytes) {
  double s = 0;
  if (vec_bytes == 4) {
    auto* v = reinterpret_cast<const float*>(raw);
    for (uint32_t i = 0; i < dim; ++i) {
      double d = (double)v[i] - (double)q[i];
      s += d * d;
    }
  } else {
    auto* v = reinterpret_cast<const uint16_t*>(raw);
    for (uint32_t i = 0; i < dim; ++i) {
      double d = (double)f16_to_f32(v[i]) - (double)q[i];
      s += d * d;
    }
  }
  return (float)s;
}

struct Cand {
  float dist;
  uint32_t id;
  bool operator<(const Cand& o) const { return dist < o.dist; }
};

static std::vector<uint32_t> search_one(Placement& pl, DramWindow& win, Prefetch& pref,
                                        const EntryGraph& eg, const float* qf,
                                        const uint8_t* qpq, uint32_t beam, uint32_t k,
                                        uint32_t iters, bool rerank) {
  pref.on_query_begin(pl, win, eg.nodes);
  std::priority_queue<Cand> candidates;  // max-heap by dist via inverted push
  // use vector+sort for simplicity
  std::vector<Cand> beam_v;
  std::unordered_set<uint32_t> visited;
  beam_v.push_back({0.f, eg.entry_id});

  auto get_pq = [&](uint32_t id) {
    const uint8_t* s = pl.pq(id);
    return win.lookup_or_promote(pl.ssd_base, s, pl.hdr->pq_bytes);
  };
  auto get_nbr = [&](uint32_t id) {
    const uint8_t* s = reinterpret_cast<const uint8_t*>(pl.nbrs(id));
    return reinterpret_cast<const uint32_t*>(
        win.lookup_or_promote(pl.ssd_base, s, pl.hdr->R * 4));
  };

  for (uint32_t it = 0; it < iters; ++it) {
    if (beam_v.empty()) break;
    std::sort(beam_v.begin(), beam_v.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    if (beam_v.size() > beam) beam_v.resize(beam);
    std::vector<Cand> next;
    next.reserve(beam * pl.hdr->R);
    for (auto& c : beam_v) {
      if (visited.count(c.id)) continue;
      visited.insert(c.id);
      pref.on_expand(pl, win, c.id);
      const uint32_t* nbrs = get_nbr(c.id);
      for (uint32_t j = 0; j < pl.hdr->R; ++j) {
        uint32_t nb = nbrs[j];
        if (nb >= pl.hdr->n || visited.count(nb)) continue;
        const uint8_t* code = get_pq(nb);
        float d = pq_l2(qpq, code, pl.hdr->pq_bytes);
        if (win.metrics) win.metrics->distance_comps++;
        next.push_back({d, nb});
      }
    }
    if (next.empty()) break;
    std::sort(next.begin(), next.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    if (next.size() > beam) next.resize(beam);
    beam_v.swap(next);
  }

  std::vector<Cand> pool = beam_v;
  // also include visited best by recomputing from visited set is heavy; use pool
  std::sort(pool.begin(), pool.end(),
            [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
  if (pool.size() > std::max(k * 5, beam)) pool.resize(std::max(k * 5, beam));

  if (rerank) {
    for (auto& c : pool) {
      const uint8_t* raw = win.lookup_or_promote(pl.ssd_base, pl.vec(c.id),
                                                 (size_t)pl.hdr->dim * pl.hdr->vec_bytes);
      c.dist = vec_l2(raw, qf, pl.hdr->dim, pl.hdr->vec_bytes);
      if (win.metrics) win.metrics->distance_comps++;
    }
    std::sort(pool.begin(), pool.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
  }

  std::vector<uint32_t> out;
  for (size_t i = 0; i < pool.size() && out.size() < k; ++i) out.push_back(pool[i].id);
  return out;
}

static PrefetchPolicy parse_policy(const std::string& s) {
  if (s == "P0") return PrefetchPolicy::P0;
  if (s == "P1") return PrefetchPolicy::P1;
  if (s == "P2") return PrefetchPolicy::P2;
  if (s == "P3") return PrefetchPolicy::P3;
  fprintf(stderr, "unknown policy %s\n", s.c_str());
  exit(2);
}

int main(int argc, char** argv) {
  const char* image = nullptr;
  const char* entry = nullptr;
  const char* queries = nullptr;
  std::string policy_s = "P0";
  size_t budget = 256 * 1024;
  size_t dram_bytes = getenv("CXAN_DRAM_BYTES")
                          ? strtoull(getenv("CXAN_DRAM_BYTES"), nullptr, 10)
                          : (1ull << 30);
  uint32_t beam = 32, k = 10, iters = 64;
  bool rerank = false;
  int max_q = -1;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char*) -> const char* {
      if (i + 1 >= argc) exit(2);
      return argv[++i];
    };
    if (a == "--image") image = need(a.c_str());
    else if (a == "--entry") entry = need(a.c_str());
    else if (a == "--queries") queries = need(a.c_str());
    else if (a == "--policy") policy_s = need(a.c_str());
    else if (a == "--budget") budget = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--dram-bytes") dram_bytes = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--beam") beam = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--k") k = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--iters") iters = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--rerank") rerank = true;
    else if (a == "--max-q") max_q = atoi(need(a.c_str()));
    else {
      fprintf(stderr, "unknown %s\n", a.c_str());
      return 2;
    }
  }
  if (!image || !entry || !queries) {
    fprintf(stderr, "need --image --entry --queries\n");
    return 2;
  }
  if (numa_available() < 0) {
    fprintf(stderr, "numa required\n");
    return 2;
  }

  size_t img_len = 0;
  void* img = map_file_ro(image, &img_len);
  auto* hdr = reinterpret_cast<CxanLayoutHeader*>(img);
  if (hdr->magic != kCxanMagic) {
    fprintf(stderr, "bad magic\n");
    return 2;
  }

  Placement pl;
  pl.hdr = hdr;
  pl.ssd_base = static_cast<const uint8_t*>(img);
  pl.ssd_bytes = img_len;

  Metrics metrics;
  void* dram = map_dram(dram_bytes);
  DramWindow win;
  win.init(dram, dram_bytes, &metrics);

  Prefetch pref;
  pref.policy = parse_policy(policy_s);
  pref.budget_per_query = budget;
  if (pref.policy == PrefetchPolicy::P2) pref.start_async(&pl, &win);

  EntryGraph eg = load_entry(entry);

  // queries: DiskANN fbin
  int qfd = open(queries, O_RDONLY);
  if (qfd < 0) die("open queries");
  uint32_t nq = 0, qdim = 0;
  if (read(qfd, &nq, 4) != 4 || read(qfd, &qdim, 4) != 4) die("query hdr");
  if (qdim != hdr->dim) {
    fprintf(stderr, "query dim mismatch %u vs %u\n", qdim, hdr->dim);
    return 2;
  }
  if (max_q > 0 && (uint32_t)max_q < nq) nq = (uint32_t)max_q;
  std::vector<float> qbuf((size_t)nq * qdim);
  if (read(qfd, qbuf.data(), qbuf.size() * 4) != (ssize_t)(qbuf.size() * 4)) die("query body");
  close(qfd);

  auto t0 = std::chrono::steady_clock::now();
  for (uint32_t qi = 0; qi < nq; ++qi) {
    const float* qf = qbuf.data() + (size_t)qi * qdim;
    std::vector<uint8_t> qpq(hdr->pq_bytes);
    for (uint32_t i = 0; i < hdr->pq_bytes; ++i) {
      float x = (i < qdim) ? qf[i] : 0.f;
      int v = (int)std::lround((x * 0.5f + 0.5f) * 255.f);
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      qpq[i] = (uint8_t)v;
    }
    auto ids = search_one(pl, win, pref, eg, qf, qpq.data(), beam, k, iters, rerank);
    (void)ids;
    metrics.queries++;
  }
  auto t1 = std::chrono::steady_clock::now();
  double sec = std::chrono::duration<double>(t1 - t0).count();
  double qps = nq / sec;

  printf("policy=%s budget=%zu dram_bytes=%zu nq=%u beam=%u k=%u iters=%u rerank=%d\n",
         policy_s.c_str(), budget, dram_bytes, nq, beam, k, iters, (int)rerank);
  printf("wall_s=%.3f QPS=%.2f\n", sec, qps);
  metrics.print();
  printf("CSV,%s,%zu,%.2f,%llu,%llu,%.2f\n", policy_s.c_str(), budget, qps,
         (unsigned long long)metrics.dram_hits, (unsigned long long)metrics.ssd_misses,
         (metrics.dram_hits + metrics.ssd_misses)
             ? 100.0 * metrics.dram_hits / (metrics.dram_hits + metrics.ssd_misses)
             : 0.0);

  if (pref.policy == PrefetchPolicy::P2) pref.stop_async();
  munmap(dram, dram_bytes);
  munmap(img, img_len);
  return 0;
}
