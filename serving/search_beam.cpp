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
#include <random>
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

static void* map_vmem_ro(const char* dev, off_t offset, size_t len) {
  int fd = open(dev, O_RDWR);
  if (fd < 0) die("open vmem");
  void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
  if (p == MAP_FAILED) die("mmap vmem");
  close(fd);
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

static float vec_mips_neg(const uint8_t* raw, const float* q, uint32_t dim, uint32_t vec_bytes) {
  // Return -IP so ascending sort keeps higher IP first (MIPS).
  double s = 0;
  if (vec_bytes == 4) {
    auto* v = reinterpret_cast<const float*>(raw);
    for (uint32_t i = 0; i < dim; ++i) s += (double)v[i] * (double)q[i];
  } else {
    auto* v = reinterpret_cast<const uint16_t*>(raw);
    for (uint32_t i = 0; i < dim; ++i) s += (double)f16_to_f32(v[i]) * (double)q[i];
  }
  return (float)(-s);
}

struct Cand {
  float dist;
  uint32_t id;
  bool operator<(const Cand& o) const { return dist < o.dist; }
};

// One-shot FP ANNS (DiskANN/Vamana greedy): candidate list size L=beam,
// expand closest unexpanded up to `iters` times. All vector reads via DramWindow.
static std::vector<uint32_t> search_one_fp(Placement& pl, DramWindow& win, Prefetch& pref,
                                           const EntryGraph& eg, const float* qf, uint32_t beam,
                                           uint32_t k, uint32_t iters) {
  pref.on_query_begin(pl, win, eg.nodes, eg.entry_id);

  auto get_vec = [&](uint32_t id) {
    return win.lookup_or_promote(pl.ssd_base, pl.vec(id),
                                 (size_t)pl.hdr->dim * pl.hdr->vec_bytes);
  };
  auto get_nbr = [&](uint32_t id) {
    const uint8_t* s = reinterpret_cast<const uint8_t*>(pl.nbrs(id));
    return reinterpret_cast<const uint32_t*>(
        win.lookup_or_promote(pl.ssd_base, s, (size_t)pl.hdr->R * 4));
  };

  const uint32_t L = beam ? beam : k;
  std::vector<Cand> cand;
  cand.reserve(L + pl.hdr->R + 8);
  std::unordered_set<uint32_t> seen;     // scored at least once (never re-score)
  std::unordered_set<uint32_t> expanded; // neighborhood already expanded

  auto insert_cand = [&](uint32_t id, float d) {
    if (cand.size() >= L) {
      auto worst = std::max_element(cand.begin(), cand.end(),
                                    [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
      if (d >= worst->dist) return;
      *worst = {d, id};
    } else {
      cand.push_back({d, id});
    }
  };

  {
    const uint8_t* raw = get_vec(eg.entry_id);
    float d = vec_mips_neg(raw, qf, pl.hdr->dim, pl.hdr->vec_bytes);
    if (win.metrics) win.metrics->distance_comps++;
    seen.insert(eg.entry_id);
    insert_cand(eg.entry_id, d);
  }

  // iters==0 → run to DiskANN fixed point (expand until no unexpanded in L).
  for (uint32_t it = 0; iters == 0 || it < iters; ++it) {
    int best_i = -1;
    float best_d = 0;
    for (size_t i = 0; i < cand.size(); ++i) {
      if (expanded.count(cand[i].id)) continue;
      if (best_i < 0 || cand[i].dist < best_d) {
        best_i = (int)i;
        best_d = cand[i].dist;
      }
    }
    if (best_i < 0) break;
    uint32_t cur = cand[(size_t)best_i].id;
    expanded.insert(cur);

    // Distance-priority: prefetch nbr vectors of closest remaining candidates.
    {
      std::vector<Prefetch::CandDist> unexp;
      unexp.reserve(cand.size());
      for (auto& c : cand) {
        if (!expanded.count(c.id)) unexp.push_back({c.dist, c.id});
      }
      unexp.push_back({best_d, cur});
      pref.prefetch_by_cand_distance(pl, win, unexp, seen);
    }

    const uint32_t* nbrs_ptr = get_nbr(cur);
    uint32_t nbrs_local[64];
    uint32_t R = pl.hdr->R;
    if (R > 64) R = 64;
    std::memcpy(nbrs_local, nbrs_ptr, R * sizeof(uint32_t));
    for (uint32_t j = 0; j < R; ++j) {
      uint32_t nb = nbrs_local[j];
      if (nb >= pl.hdr->n || seen.count(nb)) continue;
      seen.insert(nb);
      const uint8_t* raw = get_vec(nb);
      float d = vec_mips_neg(raw, qf, pl.hdr->dim, pl.hdr->vec_bytes);
      if (win.metrics) win.metrics->distance_comps++;
      insert_cand(nb, d);
    }
  }

  std::sort(cand.begin(), cand.end(),
            [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
  std::vector<uint32_t> out;
  for (size_t i = 0; i < cand.size() && out.size() < k; ++i) out.push_back(cand[i].id);
  return out;
}

static std::vector<uint32_t> search_one(Placement& pl, DramWindow& win, Prefetch& pref,
                                        const EntryGraph& eg, const float* qf,
                                        const uint8_t* qpq, uint32_t beam, uint32_t k,
                                        uint32_t iters, bool rerank, bool oneshot_fp) {
  if (oneshot_fp) return search_one_fp(pl, win, pref, eg, qf, beam, k, iters);

  pref.on_query_begin(pl, win, eg.nodes, eg.entry_id);
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

static double percentile(std::vector<double>& v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  double idx = p * (v.size() - 1);
  size_t i = (size_t)idx;
  double f = idx - i;
  if (i + 1 >= v.size()) return v.back();
  return v[i] * (1 - f) + v[i + 1] * f;
}

static std::vector<uint32_t> load_gt_row(const uint32_t* gt, uint32_t gt_k, uint32_t qi,
                                         uint32_t k) {
  std::vector<uint32_t> row;
  uint32_t m = std::min(k, gt_k);
  for (uint32_t i = 0; i < m; ++i) row.push_back(gt[(size_t)qi * gt_k + i]);
  return row;
}

static double recall_at_k(const std::vector<uint32_t>& pred, const std::vector<uint32_t>& gt) {
  if (gt.empty()) return 0;
  std::unordered_set<uint32_t> g(gt.begin(), gt.end());
  uint32_t hit = 0;
  for (uint32_t id : pred) if (g.count(id)) hit++;
  return (double)hit / (double)gt.size();
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const char* image = nullptr;
  const char* entry = nullptr;
  const char* queries = nullptr;
  const char* gt_path = nullptr;
  const char* vmem_dev = nullptr;
  off_t vmem_off = -1;
  size_t vmem_len = 0;
  std::string policy_s = "P0";
  size_t budget = 256 * 1024;
  size_t dram_bytes = getenv("CXAN_DRAM_BYTES")
                          ? strtoull(getenv("CXAN_DRAM_BYTES"), nullptr, 10)
                          : (1ull << 30);
  uint32_t beam = 32, k = 10, iters = 64;
  bool rerank = false;
  bool oneshot_fp = false;  // one-shot FP MIPS (no PQ / no re-rank stage)
  bool flush_window = false;
  int max_q = -1;
  int shuffle_seed = -1;
  size_t host_cap = 32ull << 20;  // host-side cache budget
  bool pin_entry = true;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char*) -> const char* {
      if (i + 1 >= argc) exit(2);
      return argv[++i];
    };
    if (a == "--image") image = need(a.c_str());
    else if (a == "--entry") entry = need(a.c_str());
    else if (a == "--queries") queries = need(a.c_str());
    else if (a == "--gt") gt_path = need(a.c_str());
    else if (a == "--vmem-dev") vmem_dev = need(a.c_str());
    else if (a == "--vmem-offset") vmem_off = (off_t)strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--vmem-len") vmem_len = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--policy") policy_s = need(a.c_str());
    else if (a == "--budget") budget = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--dram-bytes") dram_bytes = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--beam") beam = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--k") k = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--iters") iters = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--rerank") rerank = true;
    else if (a == "--oneshot-fp") oneshot_fp = true;
    else if (a == "--host-cap") host_cap = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--max-q") max_q = atoi(need(a.c_str()));
    else if (a == "--shuffle-seed") shuffle_seed = atoi(need(a.c_str()));
    else if (a == "--flush-window") flush_window = true;
    else if (a == "--pin-entry") pin_entry = true;
    else if (a == "--no-pin-entry") pin_entry = false;
    else {
      fprintf(stderr, "unknown %s\n", a.c_str());
      return 2;
    }
  }
  if (oneshot_fp) rerank = false;
  if (dram_bytes > (1ull << 30)) {
    fprintf(stderr, "CXL-DRAM window capped at 1GiB (got %zu)\n", dram_bytes);
    return 2;
  }
  if ((!image && !vmem_dev) || !entry || !queries) {
    fprintf(stderr, "need (--image FILE | --vmem-dev DEV --vmem-offset OFF --vmem-len LEN) "
                    "--entry --queries [--gt]\n"
                    "optional: --oneshot-fp --shuffle-seed N --flush-window --max-q M\n");
    return 2;
  }
  if (numa_available() < 0) {
    fprintf(stderr, "numa required\n");
    return 2;
  }

  size_t img_len = 0;
  void* img = nullptr;
  if (vmem_dev) {
    if (vmem_off < 0 || vmem_len == 0) {
      fprintf(stderr, "vmem requires --vmem-offset and --vmem-len\n");
      return 2;
    }
    img = map_vmem_ro(vmem_dev, vmem_off, vmem_len);
    img_len = vmem_len;
    printf("mapped vmem %s off=%lld len=%zu\n", vmem_dev, (long long)vmem_off, vmem_len);
  } else {
    img = map_file_ro(image, &img_len);
  }
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
  pref.pin_entry = pin_entry;
  if (pref.policy == PrefetchPolicy::P2) pref.start_async(&pl, &win);

  EntryGraph eg = load_entry(entry);
  {
    struct stat est {};
    if (stat(entry, &est) != 0) die("stat entry");
    if ((size_t)est.st_size > host_cap) {
      fprintf(stderr, "entry file %zu exceeds host cap %zu\n", (size_t)est.st_size, host_cap);
      return 2;
    }
  }

  int qfd = open(queries, O_RDONLY);
  if (qfd < 0) die("open queries");
  uint32_t nq_file = 0, qdim = 0;
  if (read(qfd, &nq_file, 4) != 4 || read(qfd, &qdim, 4) != 4) die("query hdr");
  if (qdim != hdr->dim) {
    fprintf(stderr, "query dim mismatch %u vs %u\n", qdim, hdr->dim);
    return 2;
  }
  // Stream-select: map file, copy only chosen queries into host (≤ host_cap).
  size_t qfile_bytes = (size_t)nq_file * qdim * 4;
  void* qmap = mmap(nullptr, 8 + qfile_bytes, PROT_READ, MAP_PRIVATE, qfd, 0);
  if (qmap == MAP_FAILED) die("mmap queries");
  close(qfd);
  const float* qfile = reinterpret_cast<const float*>((char*)qmap + 8);

  std::vector<uint32_t> qidx(nq_file);
  std::iota(qidx.begin(), qidx.end(), 0);
  if (shuffle_seed >= 0) {
    std::mt19937 rng((uint32_t)shuffle_seed);
    std::shuffle(qidx.begin(), qidx.end(), rng);
  }
  uint32_t nq = nq_file;
  if (max_q > 0 && (uint32_t)max_q < nq) nq = (uint32_t)max_q;
  qidx.resize(nq);

  size_t q_host = (size_t)nq * qdim * 4;
  struct stat est {};
  stat(entry, &est);
  size_t host_used = (size_t)est.st_size + q_host;
  if (host_used > host_cap) {
    fprintf(stderr, "host cache %zu (entry+queries) exceeds cap %zu; lower --max-q\n",
            host_used, host_cap);
    munmap(qmap, 8 + qfile_bytes);
    return 2;
  }
  std::vector<float> qbuf(q_host / 4);
  for (uint32_t i = 0; i < nq; ++i) {
    std::memcpy(qbuf.data() + (size_t)i * qdim, qfile + (size_t)qidx[i] * qdim,
                (size_t)qdim * 4);
  }
  munmap(qmap, 8 + qfile_bytes);

  uint32_t gt_n = 0, gt_k = 0;
  std::vector<uint32_t> gt_all;
  if (gt_path) {
    int gfd = open(gt_path, O_RDONLY);
    if (gfd < 0) die("open gt");
    if (read(gfd, &gt_n, 4) != 4 || read(gfd, &gt_k, 4) != 4) die("gt hdr");
    if (gt_n < nq_file) {
      fprintf(stderr, "gt n=%u < query file n=%u\n", gt_n, nq_file);
      return 2;
    }
    // Only keep GT rows for selected queries (host budget).
    gt_all.resize((size_t)nq * gt_k);
    for (uint32_t i = 0; i < nq; ++i) {
      off_t off = 8 + (off_t)qidx[i] * gt_k * 4;
      if (pread(gfd, gt_all.data() + (size_t)i * gt_k, gt_k * 4, off) != (ssize_t)(gt_k * 4))
        die("pread gt");
    }
    close(gfd);
    host_used += gt_all.size() * 4;
    if (host_used > host_cap) {
      fprintf(stderr, "host cache with GT %zu exceeds cap %zu\n", host_used, host_cap);
      return 2;
    }
  }

  printf("query_select nq=%u/%u shuffle_seed=%d flush_window=%d oneshot_fp=%d "
         "pin_entry=%d host_used=%zu host_cap=%zu dram_bytes=%zu pin_cap=%zu\n",
         nq, nq_file, shuffle_seed, (int)flush_window, (int)oneshot_fp, (int)pin_entry,
         host_used, host_cap, dram_bytes, win.pin_bytes_cap);
  fflush(stdout);

  std::vector<double> lat_ms;
  lat_ms.reserve(nq);
  double recall_sum = 0;
  uint32_t recall_n = 0;

  auto t0 = std::chrono::steady_clock::now();
  for (uint32_t qi = 0; qi < nq; ++qi) {
    if (flush_window) win.flush();
    const float* qf = qbuf.data() + (size_t)qi * qdim;
    std::vector<uint8_t> qpq(hdr->pq_bytes);
    if (!oneshot_fp) {
      for (uint32_t i = 0; i < hdr->pq_bytes; ++i) {
        float x = (i < qdim) ? qf[i] : 0.f;
        int v = (int)std::lround((x * 0.5f + 0.5f) * 255.f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        qpq[i] = (uint8_t)v;
      }
    }
    auto tq0 = std::chrono::steady_clock::now();
    auto ids = search_one(pl, win, pref, eg, qf, qpq.data(), beam, k, iters, rerank, oneshot_fp);
    auto tq1 = std::chrono::steady_clock::now();
    lat_ms.push_back(std::chrono::duration<double, std::milli>(tq1 - tq0).count());
    if (gt_path) {
      auto g = load_gt_row(gt_all.data(), gt_k, qi, k);
      recall_sum += recall_at_k(ids, g);
      recall_n++;
    }
    metrics.queries++;
  }
  auto t1 = std::chrono::steady_clock::now();
  double sec = std::chrono::duration<double>(t1 - t0).count();
  double qps = nq / sec;
  double mean_lat = 0;
  for (double x : lat_ms) mean_lat += x;
  mean_lat /= lat_ms.empty() ? 1 : lat_ms.size();
  double p50 = percentile(lat_ms, 0.50);
  double p90 = percentile(lat_ms, 0.90);
  double p99 = percentile(lat_ms, 0.99);
  double recall = recall_n ? recall_sum / recall_n : -1.0;
  double hit_pct = 0;
  uint64_t acc = metrics.dram_hits + metrics.ssd_misses;
  if (acc) hit_pct = 100.0 * (double)metrics.dram_hits / (double)acc;

  printf("policy=%s budget=%zu dram_bytes=%zu nq=%u beam=%u k=%u iters=%u oneshot_fp=%d "
         "pin_bytes=%zu/%zu\n",
         policy_s.c_str(), budget, dram_bytes, nq, beam, k, iters, (int)oneshot_fp,
         win.pin_bytes_used, win.pin_bytes_cap);
  printf("wall_s=%.3f throughput_QPS=%.2f\n", sec, qps);
  printf("latency_ms mean=%.3f p50=%.3f p90=%.3f p99=%.3f\n", mean_lat, p50, p90, p99);
  if (recall_n) printf("recall@%u=%.4f\n", k, recall);
  printf("cxl_dram_hit_pct=%.2f\n", hit_pct);
  metrics.print();
  printf("CSV,%s,%zu,%zu,%u,%u,%u,%.2f,%.3f,%.3f,%.3f,%.3f,%.4f,%.2f,%llu,%llu\n",
         policy_s.c_str(), budget, dram_bytes, nq, beam, iters, qps, mean_lat, p50, p90, p99,
         recall, hit_pct, (unsigned long long)metrics.dram_hits,
         (unsigned long long)metrics.ssd_misses);

  if (pref.policy == PrefetchPolicy::P2) pref.stop_async();
  munmap(dram, dram_bytes);
  munmap(img, img_len);
  return 0;
}
