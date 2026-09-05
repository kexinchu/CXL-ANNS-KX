// Beam search over CXAN layout via 1GiB DRAM window + prefetch policies.
// Build:
//   g++ -O3 -mavx2 -mfma -std=c++17 -pthread -I. serving/search_beam.cpp -o serving/search_beam -lnuma
//
// Example:
//   source tools/cap_enforce.sh
//   ./serving/search_beam --image .../serving_layout_200k.bin --entry .../serving_entry_200k.bin \
//       --queries /mnt/disk0/chukexin_motivation/diskann_data/query.bin \
//       --policy P0 --budget 262144 --beam 32 --k 10 --nprobe 50

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "serving/dram_window.hpp"
#include "serving/eval_trace.hpp"
#include "serving/placement.hpp"
#include "serving/prefetch.hpp"
#include "serving/promote_pipe.hpp"
#include "serving/page_copy_pool.hpp"
#include "serving/metrics.hpp"
#include "serving/vmem_prefetch.hpp"
#include "serving/hide_fill.hpp"
#include "serving/cont_batch.hpp"
#include "serving/nav_graph.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static FILE* g_expand_dump = nullptr;

#include <immintrin.h>
#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <sched.h>
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

static bool is_dax_path(const char* path) {
  return path && strncmp(path, "/dev/dax", 8) == 0;
}

// Read-only CXL-DRAM corpus. Never write zeros — that would wipe a staged index.
static void* map_dax_corpus_ro(const char* path, size_t* out_len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) die("open dax corpus");
  const size_t peek = 2ull << 20;  // dax mmap must be 2MiB-aligned
  void* hmap = mmap(nullptr, peek, PROT_READ, MAP_SHARED, fd, 0);
  if (hmap == MAP_FAILED) die("mmap dax header");
  auto* hdr = reinterpret_cast<const CxanLayoutHeader*>(hmap);
  if (hdr->magic != kCxanMagic || hdr->n == 0) {
    munmap(hmap, peek);
    close(fd);
    fprintf(stderr, "bad dax corpus magic/n at %s\n", path);
    std::exit(2);
  }
  size_t need = (size_t)hdr->off_vectors + (size_t)hdr->len_vectors;
  if (need < 4096) need = 4096;
  const size_t two_m = 2ull << 20;
  need = (need + two_m - 1) & ~(two_m - 1);
  FILE* sf = fopen("/sys/bus/dax/devices/dax0.0/size", "r");
  if (sf) {
    unsigned long long ds = 0;
    if (fscanf(sf, "%llu", &ds) == 1 && ds >= need) need = (size_t)ds;
    fclose(sf);
  }
  munmap(hmap, peek);
  void* p = mmap(nullptr, need, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) die("mmap dax corpus");
  close(fd);
  *out_len = need;
  printf("mapped CXL-DRAM corpus %s len=%zu (PROT_READ, no zero-fault)\n", path, need);
  return p;
}

static void* map_file_ro(const char* path, size_t* out_len, bool populate = false) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) die("open image");
  struct stat st {};
  if (fstat(fd, &st) != 0) die("fstat");
  int flags = MAP_PRIVATE;
  if (populate) flags |= MAP_POPULATE;
  void* p = mmap(nullptr, st.st_size, PROT_READ, flags, fd, 0);
  if (p == MAP_FAILED) die("mmap image");
  close(fd);
  *out_len = (size_t)st.st_size;
  if (populate) {
    auto* b = static_cast<volatile const char*>(p);
    size_t step = 2ull * 1024 * 1024;
    for (size_t off = 0; off < (size_t)st.st_size; off += step) (void)b[off];
    printf("populated host layout %s len=%zu (DRAM oracle)\n", path, (size_t)st.st_size);
  }
  return p;
}

static uint64_t vmem_cache_used() {
  FILE* f = fopen("/sys/class/vmem/vmem0/cache_used", "r");
  if (!f) return UINT64_MAX;
  unsigned long long v = 0;
  if (fscanf(f, "%llu", &v) != 1) v = UINT64_MAX;
  fclose(f);
  return (uint64_t)v;
}

// /sys/block/<name>/stat field 3 is sectors read (512B). Timed-window only.
// Dual-disk vmem lists comma-separated names; sum every backing.
static uint64_t one_dev_sectors(const char* name) {
  char path[160];
  snprintf(path, sizeof(path), "/sys/block/%s/stat", name);
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  unsigned long long rio = 0, rm = 0, rsect = 0;
  if (fscanf(f, "%llu %llu %llu", &rio, &rm, &rsect) != 3) rsect = 0;
  fclose(f);
  return (uint64_t)rsect;
}

static uint64_t nvme_read_sectors() {
  uint64_t total = 0;
  FILE* nf = fopen("/sys/class/vmem/vmem0/nvme_dev", "r");
  if (nf) {
    char line[256] = {};
    if (fgets(line, sizeof(line), nf)) {
      char* tok = strtok(line, ", \t\n");
      while (tok) {
        const char* base = strrchr(tok, '/');
        base = base ? base + 1 : tok;
        if (base[0]) total += one_dev_sectors(base);
        tok = strtok(nullptr, ", \t\n");
      }
    }
    fclose(nf);
    if (total) return total;
  }
  return one_dev_sectors("nvme2n1");
}

static void* map_vmem_ro(const char* dev, off_t offset, size_t len, int* out_fd = nullptr) {
  int fd = open(dev, O_RDWR);
  if (fd < 0) die("open vmem");
  void* p = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, offset);
  if (p == MAP_FAILED) die("mmap vmem");
  if (out_fd)
    *out_fd = fd;
  else
    close(fd);
  return p;
}

// CXL-DRAM is the vmem BAR only (/dev/vmem, /dev/vmem0, ...). Not /dev/dax*.
static bool is_vmem_bar_dev(const char* path) {
  if (!path || !path[0]) return false;
  static const char kPref[] = "/dev/vmem";
  const size_t n = sizeof(kPref) - 1;
  if (strncmp(path, kPref, n) != 0) return false;
  for (const char* p = path + n; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  return true;
}

// Window via vmem BAR (CXL-DRAM) or legacy /dev/dax* (not CXL-DRAM on this machine).
// Prefer this over anonymous+mbind(node1), which can land on local socket DRAM.
static void* map_dram_dax(const char* dax_dev, off_t offset, size_t bytes) {
  if (!dax_dev || !dax_dev[0]) die("dax_dev");
  if (bytes == 0) die("dax bytes");
  if ((offset & 4095) || (bytes & 4095)) {
    fprintf(stderr, "dax offset/len must be 4KiB-aligned (off=%lld len=%zu)\n",
            (long long)offset, bytes);
    std::exit(2);
  }
  int fd = open(dax_dev, O_RDWR);
  if (fd < 0) die("open dax");
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
  if (p == MAP_FAILED) die("mmap dax");
  close(fd);
  // Fault-in window pages so first promote is not conflated with DAX fault cost.
  auto* b = static_cast<volatile char*>(p);
  for (size_t off = 0; off < bytes; off += 4096) b[off] = 0;
  if (is_vmem_bar_dev(dax_dev))
    printf("mapped CXL-DRAM vmem BAR %s off=%lld len=%zu\n", dax_dev, (long long)offset,
           bytes);
  else
    printf("mapped CXL-DRAM dax %s off=%lld len=%zu\n", dax_dev, (long long)offset, bytes);
  return p;
}

// HOST scoring window: anonymous pages bound to a NUMA node (socket DRAM).
static void* map_dram_numa(size_t bytes, unsigned numa_node) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) die("mmap dram anon");
  unsigned long nodemask = 1UL << numa_node;
  if (mbind(p, bytes, MPOL_BIND, &nodemask, sizeof(nodemask) * 8,
            MPOL_MF_MOVE | MPOL_MF_STRICT) != 0)
    perror("mbind warn");
  auto* b = static_cast<volatile char*>(p);
  for (size_t off = 0; off < bytes; off += 4096) b[off] = 0;
  printf("mapped HOST DRAM anon+mbind node=%u len=%zu (fallback; not DAX)\n", numa_node,
         bytes);
  return p;
}

static std::vector<int> list_cpu_ids() {
  std::vector<int> cpus;
  FILE* f = fopen("/proc/cpuinfo", "r");
  if (!f) return cpus;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "processor", 9) != 0) continue;
    char* colon = strchr(line, ':');
    if (colon) cpus.push_back(atoi(colon + 1));
  }
  fclose(f);
  return cpus;
}

static void bind_worker_cpu(int cpu_id) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu_id, &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0) perror("affinity");
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
  if (vec_bytes == 4) {
    auto* v = reinterpret_cast<const float*>(raw);
    __m256 acc = _mm256_setzero_ps();
    uint32_t i = 0;
    for (; i + 8 <= dim; i += 8) {
      acc = _mm256_fmadd_ps(_mm256_loadu_ps(v + i), _mm256_loadu_ps(q + i), acc);
    }
    alignas(32) float tmp[8];
    _mm256_store_ps(tmp, acc);
    float s = tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] + tmp[6] + tmp[7];
    for (; i < dim; ++i) s += v[i] * q[i];
    return -s;
  }
  auto* v = reinterpret_cast<const uint16_t*>(raw);
  float s = 0;
  for (uint32_t i = 0; i < dim; ++i) s += f16_to_f32(v[i]) * q[i];
  return -s;
}

static float vec_distance(const uint8_t* raw, const float* q, uint32_t dim, uint32_t vec_bytes,
                          DistanceMetric metric) {
  return metric == DistanceMetric::Mips ? vec_mips_neg(raw, q, dim, vec_bytes)
                                        : vec_l2(raw, q, dim, vec_bytes);
}

static Metrics* cur_met(DramWindow& win) {
  return tls_metrics ? tls_metrics : win.metrics;
}

struct Cand {
  float dist;
  uint32_t id;
  bool operator<(const Cand& o) const { return dist < o.dist; }
};

// One-shot FP ANNS (DiskANN/Vamana greedy): candidate list size L=beam,
// expand closest unexpanded up to `iters` times. All vector reads via DramWindow.
// P2 uses expand/score decoupling (P2v2): discover nbrs into pending, score when resident;
// lookahead sync-prefetches non-cur future hops (no second thread on DAX).
// lookahead prefetches future candidates only (excludes current expand).
static std::vector<uint32_t> search_one_pq(Placement& pl, DramWindow& win, Prefetch& pref,
                                           const EntryGraph& eg, const float* qf, uint32_t beam,
                                           uint32_t k, uint32_t iters, PageCopyPool* ext_pool,
                                           VmemIo* vio,
                                           std::vector<uint32_t>* committed_out = nullptr) {
  PqTable* pq = pref.pq;
  if (!pq || !pq->loaded()) {
    fprintf(stderr, "pq-nav needs loaded --pq-pivots/--pq-compressed\n");
    std::exit(2);
  }
  pq->begin_query(qf, pref.metric);
  pref.on_query_begin(pl, win, eg.nodes, eg.entry_id);

  const size_t vb = (size_t)pl.hdr->dim * pl.hdr->vec_bytes;
  const size_t pb = win.page_bytes;
  const uint32_t L = beam ? beam : k;
  uint32_t Rlim = pl.hdr->R;
  if (Rlim > 64) Rlim = 64;

  std::vector<Cand> cand;
  cand.reserve(L + pl.hdr->R + 8);
  std::unordered_set<uint32_t> seen;
  std::unordered_set<uint32_t> expanded;

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
  auto pq_insert = [&](uint32_t id) {
    if (id >= pq->n) return;
    float d = pq->dist(id);
    if (cur_met(win)) cur_met(win)->distance_comps++;
    insert_cand(id, d);
  };

  seen.insert(eg.entry_id);
  pq_insert(eg.entry_id);

  auto pick_best = [&]() -> int {
    int bi = -1;
    float bd = 0;
    for (size_t i = 0; i < cand.size(); ++i) {
      if (expanded.count(cand[i].id)) continue;
      if (bi < 0 || cand[i].dist < bd) {
        bi = (int)i;
        bd = cand[i].dist;
      }
    }
    return bi;
  };
  auto expand_one = [&](uint32_t cur) {
    uint32_t nbrs_local[64];
    hide_read_nbrs(pl, cur, nbrs_local, Rlim);
    for (uint32_t j = 0; j < Rlim; ++j) {
      uint32_t nb = nbrs_local[j];
      if (nb >= pl.hdr->n || seen.count(nb)) continue;
      seen.insert(nb);
      pq_insert(nb);
    }
  };

  auto fp_rerank_dram = [&]() {
    for (Cand& c : cand) {
      c.dist = vec_distance(pl.vec(c.id), qf, pl.hdr->dim, pl.hdr->vec_bytes, pref.metric);
      if (cur_met(win)) {
        cur_met(win)->distance_comps++;
        cur_met(win)->note_score_from_window(1);
        cur_met(win)->note_pf_scored_id(c.id);
      }
    }
  };

  if (pref.oracle_dram) {
    uint32_t expands = 0;
    while (true) {
      int bi = pick_best();
      if (bi < 0) break;
      if (iters != 0 && expands >= iters) break;
      uint32_t cur = cand[(size_t)bi].id;
      expanded.insert(cur);
      expands++;
      expand_one(cur);
    }
    if (committed_out) {
      committed_out->clear();
      committed_out->reserve(cand.size());
      for (const Cand& c : cand) committed_out->push_back(c.id);
    }
    fp_rerank_dram();
    std::sort(cand.begin(), cand.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    std::vector<uint32_t> out;
    for (size_t i = 0; i < cand.size() && out.size() < k; ++i) out.push_back(cand[i].id);
    return out;
  }

  const uint32_t W = pref.pipe_w ? pref.pipe_w : 8;
  PageCopyPool local_pool;
  PageCopyPool* pool = ext_pool ? ext_pool : &local_pool;
  if (pool->nworkers == 0) pool->start(W);
  HidePipe hpipe;
  hpipe.pool = pool;
  hpipe.win = &win;
  hpipe.pl = &pl;
  hpipe.vio = vio;
  hpipe.m = cur_met(win);
  hpipe.extent_run = pref.extent_run;
  hpipe.direct_install = pref.direct_install;
  hpipe.score = pref.hide_score;
  uint64_t last_issue_tok = 0;

  auto issue_ids = [&](const std::vector<uint32_t>& ids) {
    std::vector<uint64_t> pages;
    std::unordered_set<uint64_t> ps;
    pages.reserve(ids.size() * 2);
    for (uint32_t id : ids) hide_collect_vec_pages(pl, vb, pb, id, pages, &ps);
    uint64_t tok = 0;
    if (!pages.empty()) tok = hpipe.issue(pages, /*ttl=*/128, /*stall_if_full=*/true);
    last_issue_tok = tok;
    return pages;
  };

  uint32_t expands = 0;
  while (true) {
    hpipe.pump();
    int bi = pick_best();
    if (bi < 0) break;
    if (iters != 0 && expands >= iters) break;
    uint32_t cur = cand[(size_t)bi].id;
    expanded.insert(cur);
    expands++;
    expand_one(cur);
  }

  std::vector<uint32_t> rerank_ids;
  rerank_ids.reserve(cand.size());
  for (const Cand& c : cand) rerank_ids.push_back(c.id);
  if (committed_out) *committed_out = rerank_ids;
  auto need = issue_ids(rerank_ids);
  std::vector<uint64_t> extra_toks;
  if (last_issue_tok) extra_toks.push_back(last_issue_tok);
  uint64_t wns = hpipe.wait_covering(need, &extra_toks);
  if (!wns) wns = hpipe.wait_all();
  if (cur_met(win)) {
    cur_met(win)->device_fill_ns += wns;
    cur_met(win)->crit_wait_ns += wns;
  }
  hpipe.pump();

  alignas(64) uint8_t buf[4096];
  if (vb > sizeof(buf)) {
    fprintf(stderr, "vec too large for score buf\n");
    std::exit(2);
  }
  std::vector<uint64_t> scored_pages;
  std::unordered_set<uint64_t> sps;
  for (Cand& c : cand) {
    const uint8_t* src = hpipe.vec_src(pl.vec(c.id), vb);
    bool hit = src != nullptr;
    if (!src) {
      hit = hpipe.copy_vec(pl.vec(c.id), vb, buf);
      src = buf;
      if (!hit) win.copy_through(pl.ssd_base, pl.vec(c.id), vb, buf);
    }
    c.dist = vec_distance(src, qf, pl.hdr->dim, pl.hdr->vec_bytes, pref.metric);
    if (cur_met(win)) {
      cur_met(win)->distance_comps++;
      if (hpipe.score == HideScore::Bounce) cur_met(win)->note_score_from_bounce(1);
      else if (hpipe.score == HideScore::Vmem) cur_met(win)->note_score_from_cache(1);
      else if (hit) cur_met(win)->note_score_from_window(1);
      else cur_met(win)->note_score_from_bounce(1);
      cur_met(win)->note_pf_scored_id(c.id);
    }
    hide_collect_vec_pages(pl, vb, pb, c.id, scored_pages, &sps);
  }
  if (cur_met(win)) cur_met(win)->note_pf_used(scored_pages);
  for (uint64_t t : extra_toks) hpipe.release_tok(t);
  hpipe.release_pages(need);
  if (!ext_pool) pool->stop_join();

  std::sort(cand.begin(), cand.end(),
            [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
  std::vector<uint32_t> out;
  for (size_t i = 0; i < cand.size() && out.size() < k; ++i) out.push_back(cand[i].id);
  return out;
}

static std::vector<uint32_t> search_one_fp(Placement& pl, DramWindow& win, Prefetch& pref,
                                           PromotePipe* pipe, const EntryGraph& eg,
                                           const float* qf, uint32_t beam, uint32_t k,
                                           uint32_t iters, PageCopyPool* ext_pool = nullptr,
                                           VmemIo* vio = nullptr) {
  pref.on_query_begin(pl, win, eg.nodes, eg.entry_id);

  const size_t vb = (size_t)pl.hdr->dim * pl.hdr->vec_bytes;
  auto get_vec = [&](uint32_t id) {
    if (pl.diskann_layout && pl.vec_stride)
      return win.lookup_or_promote(pl.ssd_base, pl.entry(id), pl.vec_stride);
    return win.lookup_or_promote(pl.ssd_base, pl.vec(id), vb);
  };
  auto get_nbr = [&](uint32_t id) {
    if (pl.diskann_layout && pl.vec_stride) {
      const uint8_t* e =
          win.lookup_or_promote(pl.ssd_base, pl.entry(id), pl.vec_stride);
      const size_t off = (size_t)pl.hdr->dim * pl.hdr->vec_bytes + 4;
      return reinterpret_cast<const uint32_t*>(e + off);
    }
    const uint8_t* s = reinterpret_cast<const uint8_t*>(pl.nbrs(id));
    return reinterpret_cast<const uint32_t*>(
        win.lookup_or_promote(pl.ssd_base, s, (size_t)pl.hdr->R * 4));
  };

  const uint32_t L = beam ? beam : k;
  std::vector<Cand> cand;
  cand.reserve(L + pl.hdr->R + 8);
  std::unordered_set<uint32_t> seen;
  std::unordered_set<uint32_t> expanded;

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

  auto score_id = [&](uint32_t id) {
    alignas(64) uint8_t buf[4096];
    if (vb > sizeof(buf)) {
      fprintf(stderr, "vec too large for score buf\n");
      std::exit(2);
    }
    win.copy_through(pl.ssd_base, pl.vec(id), vb, buf);
    float d = vec_mips_neg(buf, qf, pl.hdr->dim, pl.hdr->vec_bytes);
    if (cur_met(win)) cur_met(win)->distance_comps++;
    insert_cand(id, d);
  };

  {
    seen.insert(eg.entry_id);
    if (pref.oracle_dram) {
      float d = vec_mips_neg(pl.vec(eg.entry_id), qf, pl.hdr->dim, pl.hdr->vec_bytes);
      if (cur_met(win)) {
        cur_met(win)->distance_comps++;
        cur_met(win)->note_score_from_window(1);
        cur_met(win)->note_pf_scored_id(eg.entry_id);
      }
      insert_cand(eg.entry_id, d);
    } else if (pref.policy == PrefetchPolicy::P3) {
      alignas(64) uint8_t buf[4096];
      bool hit = win.copy_if_resident(pl.ssd_base, pl.vec(eg.entry_id), vb, buf);
      if (!hit) win.copy_through(pl.ssd_base, pl.vec(eg.entry_id), vb, buf);
      if (cur_met(win)) {
        if (hit) cur_met(win)->note_score_from_window(1);
        else cur_met(win)->note_score_from_bounce(1);
        cur_met(win)->distance_comps++;
      }
      insert_cand(eg.entry_id, vec_mips_neg(buf, qf, pl.hdr->dim, pl.hdr->vec_bytes));
    } else {
      score_id(eg.entry_id);
    }
  }

  // ---- Oracle: full corpus already in host DRAM (stand-in for large CXL-DRAM) ----
  if (pref.oracle_dram) {
    auto score_dram = [&](uint32_t id) {
      float d = vec_mips_neg(pl.vec(id), qf, pl.hdr->dim, pl.hdr->vec_bytes);
      if (cur_met(win)) {
        cur_met(win)->distance_comps++;
        cur_met(win)->note_score_from_window(1);
        cur_met(win)->note_pf_scored_id(id);
      }
      insert_cand(id, d);
    };
    uint32_t Rlim = pl.hdr->R;
    if (Rlim > 64) Rlim = 64;
    uint32_t expands = 0;
    while (true) {
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
      if (iters != 0 && expands >= iters) break;
      uint32_t cur = cand[(size_t)best_i].id;
      expanded.insert(cur);
      expands++;
      uint32_t nbrs_local[64];
      hide_read_nbrs(pl, cur, nbrs_local, Rlim);
      for (uint32_t j = 0; j < Rlim; ++j) {
        uint32_t nb = nbrs_local[j];
        if (nb >= pl.hdr->n || seen.count(nb)) continue;
        seen.insert(nb);
        score_dram(nb);
      }
    }
    std::sort(cand.begin(), cand.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    std::vector<uint32_t> out;
    for (size_t i = 0; i < cand.size() && out.size() < k; ++i) out.push_back(cand[i].id);
    return out;
  }

  // ---- P3 hide: score only from DramWindow; fills are lookahead / miss recovery ----
  if (pref.policy == PrefetchPolicy::P3) {
    const uint32_t W = pref.pipe_w ? pref.pipe_w : 8;
    uint32_t expands = 0;
    const size_t pb = win.page_bytes;
    uint32_t Rlim = pl.hdr->R;
    if (Rlim > 64) Rlim = 64;

    PageCopyPool local_pool;
    PageCopyPool* pool = ext_pool ? ext_pool : &local_pool;
    if (pool->nworkers == 0) pool->start(W);

    HidePipe pipe;
    pipe.pool = pool;
    pipe.win = &win;
    pipe.pl = &pl;
    pipe.vio = vio;
    pipe.m = cur_met(win);
    pipe.extent_run = pref.extent_run;

    auto vec_ptr = [&](uint32_t id, uint32_t src, uint32_t slotk) -> const uint8_t* {
      if (pl.has_bundle()) return pl.bundle_slot(src, slotk);
      (void)src;
      (void)slotk;
      return pl.vec(id);
    };

    auto score_resident = [&](uint32_t id, uint32_t src, uint32_t slotk, bool before_ioctl) {
      alignas(64) uint8_t buf[4096];
      if (vb > sizeof(buf)) {
        fprintf(stderr, "vec too large for score buf\n");
        std::exit(2);
      }
      const uint8_t* vp = vec_ptr(id, src, slotk);
      if (!win.copy_if_resident(pl.ssd_base, vp, vb, buf)) {
        win.copy_through(pl.ssd_base, vp, vb, buf);
        if (cur_met(win)) cur_met(win)->note_score_from_bounce(1);
      } else if (cur_met(win)) {
        if (before_ioctl) cur_met(win)->note_score_from_window(1);
        else cur_met(win)->note_score_from_bounce(1);
      }
      float d = vec_mips_neg(buf, qf, pl.hdr->dim, pl.hdr->vec_bytes);
      if (cur_met(win)) {
        cur_met(win)->distance_comps++;
        cur_met(win)->note_pf_score_vec(vb);
        cur_met(win)->note_pf_scored_id(id);
        std::vector<uint64_t> sp;
        std::unordered_set<uint64_t> ss;
        uint64_t off = (uint64_t)(vp - pl.ssd_base);
        uint64_t end = off + vb;
        uint64_t first = off & ~(uint64_t)(pb - 1);
        uint64_t last = (end - 1) & ~(uint64_t)(pb - 1);
        for (uint64_t p = first; p <= last; p += pb) {
          if (ss.insert(p).second) sp.push_back(p);
        }
        cur_met(win)->note_pf_used(sp);
      }
      insert_cand(id, d);
    };

    struct Pend {
      uint32_t id, src, k;
    };
    std::vector<Pend> pending;
    std::unordered_set<uint32_t> pending_set;

    auto finish_score = [&](uint32_t id, const uint8_t* vp, float d) {
      if (cur_met(win)) {
        cur_met(win)->distance_comps++;
        cur_met(win)->note_score_from_window(1);
        cur_met(win)->note_pf_score_vec(vb);
        cur_met(win)->note_pf_scored_id(id);
        std::vector<uint64_t> sp;
        uint64_t off = (uint64_t)(vp - pl.ssd_base);
        uint64_t first = off & ~(uint64_t)(pb - 1);
        uint64_t last = (off + vb - 1) & ~(uint64_t)(pb - 1);
        for (uint64_t p = first; p <= last; p += pb) sp.push_back(p);
        cur_met(win)->note_pf_used(sp);
      }
      insert_cand(id, d);
    };

    auto drain_score = [&]() {
      if (pending.empty()) return;
      std::vector<Pend> still;
      still.reserve(pending.size());
      for (const Pend& e : pending) {
        const uint8_t* vp = vec_ptr(e.id, e.src, e.k);
        alignas(64) uint8_t buf[4096];
        if (vb > sizeof(buf)) {
          fprintf(stderr, "vec too large for score buf\n");
          std::exit(2);
        }
        if (!win.try_copy_resident(pl.ssd_base, vp, vb, buf)) {
          still.push_back(e);
          continue;
        }
        pending_set.erase(e.id);
        finish_score(e.id, vp, vec_mips_neg(buf, qf, pl.hdr->dim, pl.hdr->vec_bytes));
      }
      pending.swap(still);
    };
    auto drain_pending = [&]() {
      pipe.pump();
      drain_score();
    };
    // after_pump during hide_wait raised nq=20 QPS but nq=100 evicts (44.60 < 49.25).

    auto hop_hide_score = [&](uint32_t src, const uint32_t* nbrs_all, uint32_t n_nbr,
                              std::vector<uint32_t>& ids) {
      pipe.pump();
      std::unordered_map<uint32_t, uint32_t> id_to_k;
      if (pl.has_bundle()) {
        for (uint32_t k = 0; k < n_nbr; ++k) id_to_k[nbrs_all[k]] = k;
      }
      auto slotk = [&](uint32_t id) -> uint32_t {
        if (!pl.has_bundle()) return 0;
        auto it = id_to_k.find(id);
        return it == id_to_k.end() ? 0 : it->second;
      };
      std::vector<uint32_t> hit, miss;
      hit.reserve(ids.size());
      miss.reserve(ids.size());
      for (uint32_t id : ids) {
        uint32_t k = slotk(id);
        if (win.is_resident(pl.ssd_base, vec_ptr(id, src, k), vb))
          hit.push_back(id);
        else
          miss.push_back(id);
      }
      if (!pref.freeze_fills && !miss.empty()) {
        std::vector<uint64_t> now;
        std::unordered_set<uint64_t> ps;
        if (pl.has_bundle()) {
          std::unordered_set<uint32_t> want(miss.begin(), miss.end());
          hide_collect_bundle_pages(pl, vb, pb, src, nbrs_all, n_nbr, want, now, &ps,
                                   cur_met(win), 0.f);
        } else {
          for (uint32_t id : miss) hide_collect_vec_pages(pl, vb, pb, id, now, &ps);
        }
        pipe.issue(now, /*ttl=*/128, /*stall_if_full=*/true);
        for (uint32_t id : miss) {
          if (pending_set.insert(id).second)
            pending.push_back(Pend{id, src, slotk(id)});
        }
      }
      for (uint32_t id : hit) {
        score_resident(id, src, slotk(id), true);
        pipe.pump();
      }
      drain_pending();
    };

    const uint32_t ebatch = pref.expand_batch ? pref.expand_batch : 1;
    const uint32_t ahead = pref.issue_ahead ? pref.issue_ahead : 1;

    auto pick_batch = [&]() -> std::vector<uint32_t> {
      std::vector<uint32_t> batch;
      batch.reserve(ebatch);
      for (uint32_t t = 0; t < ebatch; ++t) {
        int bi = -1;
        float bd = 0;
        for (size_t i = 0; i < cand.size(); ++i) {
          if (expanded.count(cand[i].id)) continue;
          if (bi < 0 || cand[i].dist < bd) {
            bi = (int)i;
            bd = cand[i].dist;
          }
        }
        if (bi < 0) break;
        if (iters != 0 && expands >= iters) break;
        uint32_t id = cand[(size_t)bi].id;
        expanded.insert(id);
        expands++;
        batch.push_back(id);
      }
      return batch;
    };

    std::vector<uint64_t> issued_pages;
    auto issue_bundle_batch = [&](const std::vector<uint32_t>& batch) {
      if (batch.empty()) return;
      // Old DiskANN path: score each expand before the next (no IO pipe).
      if (!pl.has_bundle() && pref.sync_hop) {
        for (uint32_t cur : batch) {
          uint32_t nbrs_local[64];
          hide_read_nbrs(pl, cur, nbrs_local, Rlim);
          std::vector<uint32_t> to_score;
          to_score.reserve(Rlim);
          for (uint32_t j = 0; j < Rlim; ++j) {
            uint32_t nb = nbrs_local[j];
            if (nb >= pl.hdr->n || seen.count(nb)) continue;
            seen.insert(nb);
            to_score.push_back(nb);
          }
          if (g_expand_dump && !to_score.empty()) {
            uint32_t c = (uint32_t)to_score.size();
            std::fwrite(&c, 4, 1, g_expand_dump);
            std::fwrite(to_score.data(), 4, c, g_expand_dump);
          }
          hop_hide_score(cur, nbrs_local, Rlim, to_score);
        }
        return;
      }
      std::vector<uint64_t> now;
      std::unordered_set<uint64_t> ps;
      now.reserve(batch.size() * 16);
      for (uint32_t cur : batch) {
        uint32_t nbrs_local[64];
        hide_read_nbrs(pl, cur, nbrs_local, Rlim);
        std::vector<uint32_t> to_score;
        to_score.reserve(Rlim);
        for (uint32_t j = 0; j < Rlim; ++j) {
          uint32_t nb = nbrs_local[j];
          if (nb >= pl.hdr->n || seen.count(nb)) continue;
          seen.insert(nb);
          to_score.push_back(nb);
        }
        if (g_expand_dump && !to_score.empty()) {
          uint32_t c = (uint32_t)to_score.size();
          std::fwrite(&c, 4, 1, g_expand_dump);
          std::fwrite(to_score.data(), 4, c, g_expand_dump);
        }
        if (pl.has_bundle()) {
          std::unordered_set<uint32_t> want(to_score.begin(), to_score.end());
          hide_collect_bundle_pages(pl, vb, pb, cur, nbrs_local, Rlim, want, now, &ps,
                                   cur_met(win), 0.f);
          for (uint32_t j = 0; j < Rlim; ++j) {
            uint32_t nb = nbrs_local[j];
            if (!want.count(nb)) continue;
            if (pending_set.insert(nb).second) pending.push_back(Pend{nb, cur, j});
          }
        } else {
          for (uint32_t id : to_score) {
            hide_collect_vec_pages(pl, vb, pb, id, now, &ps);
            if (pending_set.insert(id).second) pending.push_back(Pend{id, cur, 0});
          }
        }
      }
      issued_pages.swap(now);
    };

    while (true) {
      if (iters != 0 && expands >= iters) break;
      uint32_t issued_waves = 0;
      for (uint32_t w = 0; w < ahead; ++w) {
        auto batch = pick_batch();
        if (batch.empty()) break;
        issued_pages.clear();
        issue_bundle_batch(batch);
        if (!issued_pages.empty())
          pipe.issue(issued_pages, /*ttl=*/128, /*stall_if_full=*/true);
        pipe.pump();
        issued_waves++;
      }
      if (issued_waves == 0) {
        if (pending.empty() || pref.freeze_fills) break;
        std::vector<uint64_t> need;
        std::unordered_set<uint64_t> nps;
        for (const Pend& e : pending) {
          if (pl.has_bundle()) {
            uint64_t off = pl.bundle_off + (uint64_t)e.src * pl.bundle_stride +
                           (uint64_t)e.k * pl.packed_vec_bytes();
            uint64_t end = off + vb;
            uint64_t first = off & ~(uint64_t)(pb - 1);
            uint64_t last = (end - 1) & ~(uint64_t)(pb - 1);
            for (uint64_t p = first; p <= last; p += pb)
              if (nps.insert(p).second) need.push_back(p);
          } else {
            hide_collect_vec_pages(pl, vb, pb, e.id, need, &nps);
          }
        }
        pipe.issue(need, /*ttl=*/128, /*stall_if_full=*/true);
        uint64_t wns = pipe.wait_covering(need);
        if (!wns && !pending.empty()) wns = pipe.wait_all();
        if (cur_met(win)) {
          cur_met(win)->device_fill_ns += wns;
          cur_met(win)->crit_wait_ns += wns;
        }
        drain_pending();
        continue;
      }
      drain_pending();
    }

    {
      uint64_t wns = pipe.wait_all();
      if (cur_met(win)) cur_met(win)->device_fill_ns += wns;
    }
    drain_pending();
    if (!ext_pool) pool->stop_join();

    std::sort(cand.begin(), cand.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    std::vector<uint32_t> out;
    for (size_t i = 0; i < cand.size() && out.size() < k; ++i) out.push_back(cand[i].id);
    return out;
  }

  // ---- P2v2: expand discovers edges; score when resident; lookahead ≠ cur ----
  if (pref.policy == PrefetchPolicy::P2) {
    std::deque<uint32_t> pending;
    std::unordered_set<uint32_t> pending_set;
    const size_t pending_cap = std::max<size_t>(256, (size_t)L * 2);
    uint32_t expands = 0;

    auto drain_ready = [&]() {
      size_t n = pending.size();
      for (size_t i = 0; i < n; ++i) {
        uint32_t id = pending.front();
        pending.pop_front();
        if (win.is_resident(pl.ssd_base, pl.vec(id), vb)) {
          pending_set.erase(id);
          score_id(id);
        } else {
          pending.push_back(id);
        }
      }
    };

    auto force_one_pending = [&]() {
      if (pending.empty()) return;
      uint32_t id = pending.front();
      pending.pop_front();
      pending_set.erase(id);
      score_id(id);
    };

    while (true) {
      drain_ready();

      int best_i = -1;
      float best_d = 0;
      for (size_t i = 0; i < cand.size(); ++i) {
        if (expanded.count(cand[i].id)) continue;
        if (best_i < 0 || cand[i].dist < best_d) {
          best_i = (int)i;
          best_d = cand[i].dist;
        }
      }

      if (best_i < 0) {
        // All current cand expanded; demand-score one pending so new frontier appears.
        if (pending.empty()) break;
        force_one_pending();
        continue;
      }

      if (iters != 0 && expands >= iters) {
        while (!pending.empty()) force_one_pending();
        break;
      }

      uint32_t cur = cand[(size_t)best_i].id;
      expanded.insert(cur);
      expands++;

      // Discover cur's nbrs into pending (do not score yet).
      {
        uint32_t nbrs_local[64];
        uint32_t R = pl.hdr->R;
        if (R > 64) R = 64;
        alignas(64) uint8_t nbuf[64 * 4];
        win.copy_through(pl.ssd_base, reinterpret_cast<const uint8_t*>(pl.nbrs(cur)),
                         (size_t)R * 4, nbuf);
        std::memcpy(nbrs_local, nbuf, (size_t)R * 4);
        for (uint32_t j = 0; j < R; ++j) {
          uint32_t nb = nbrs_local[j];
          if (nb >= pl.hdr->n || seen.count(nb)) continue;
          seen.insert(nb);
          if (!pending_set.count(nb)) {
            pending_set.insert(nb);
            pending.push_back(nb);
          }
        }
      }

      // Lead time: sync-promote nbrs of *other* unexpanded candidates (exclude cur).
      {
        std::vector<Prefetch::CandDist> unexp;
        unexp.reserve(cand.size());
        for (auto& c : cand) {
          if (!expanded.count(c.id)) unexp.push_back({c.dist, c.id});
        }
        pref.prefetch_by_cand_distance(pl, win, unexp, seen, cur);
      }

      // Score whatever became resident (often from prior hop lookahead).
      drain_ready();
      // Bound pending: demand-score oldest so search progresses.
      while (pending.size() > pending_cap) force_one_pending();
      // If frontier is stuck (no unexpanded left in cand), pull at least one pending.
      {
        bool any_unexp = false;
        for (auto& c : cand) {
          if (!expanded.count(c.id)) {
            any_unexp = true;
            break;
          }
        }
        if (!any_unexp && !pending.empty()) force_one_pending();
      }
    }

    std::sort(cand.begin(), cand.end(),
              [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    std::vector<uint32_t> out;
    for (size_t i = 0; i < cand.size() && out.size() < k; ++i) out.push_back(cand[i].id);
    return out;
  }

  // ---- P0 / P1: classic expand-then-score (P1 may sync-prefetch including cur) ----
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
      score_id(nb);
    }
  }

  std::sort(cand.begin(), cand.end(),
            [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
  std::vector<uint32_t> out;
  for (size_t i = 0; i < cand.size() && out.size() < k; ++i) out.push_back(cand[i].id);
  return out;
}

static std::vector<uint32_t> search_one(Placement& pl, DramWindow& win, Prefetch& pref,
                                        PromotePipe* pipe, const EntryGraph& eg, const float* qf,
                                        const uint8_t* qpq, uint32_t beam, uint32_t k,
                                        uint32_t iters, bool rerank, bool oneshot_fp,
                                        PageCopyPool* ext_pool = nullptr, VmemIo* vio = nullptr,
                                        std::vector<uint32_t>* committed_out = nullptr) {
  if (!oneshot_fp && pref.pq_nav && pref.pq)
    return search_one_pq(pl, win, pref, eg, qf, beam, k, iters, ext_pool, vio, committed_out);
  if (oneshot_fp)
    return search_one_fp(pl, win, pref, pipe, eg, qf, beam, k, iters, ext_pool, vio);

  pref.on_query_begin(pl, win, eg.nodes, eg.entry_id);
  std::vector<Cand> beam_v;
  std::unordered_set<uint32_t> visited;
  beam_v.push_back({0.f, eg.entry_id});

  auto get_pq = [&](uint32_t id) {
    const uint8_t* s = pl.pq(id);
    return win.lookup_or_promote(pl.ssd_base, s, pl.hdr->pq_bytes);
  };
  auto get_nbr = [&](uint32_t id) {
    if (pl.diskann_layout && pl.vec_stride) {
      const uint8_t* e =
          win.lookup_or_promote(pl.ssd_base, pl.entry(id), pl.vec_stride);
      const size_t off = (size_t)pl.hdr->dim * pl.hdr->vec_bytes + 4;
      return reinterpret_cast<const uint32_t*>(e + off);
    }
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
        if (cur_met(win)) cur_met(win)->distance_comps++;
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
      if (cur_met(win)) cur_met(win)->distance_comps++;
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

// Frozen DiskANN P3 hop (e4 a1, no score-page / spec / cache). Never wait_covering.
struct P3Q {
  uint32_t qi = 0;
  const float* qf = nullptr;
  std::vector<Cand> cand;
  std::unordered_set<uint32_t> seen;
  std::unordered_set<uint32_t> expanded;
  std::unordered_set<uint32_t> pending_set;
  struct Pend {
    uint32_t id, src, k;
  };
  std::vector<Pend> pending;
  uint32_t expands = 0;
  CbSt st = CbSt::Empty;
  std::vector<uint64_t> need;
  std::chrono::steady_clock::time_point t0;
};

static void p3q_insert(P3Q& q, uint32_t L, uint32_t id, float d) {
  if (q.cand.size() >= L) {
    auto worst = std::max_element(q.cand.begin(), q.cand.end(),
                                  [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    if (d >= worst->dist) return;
    *worst = {d, id};
  } else {
    q.cand.push_back({d, id});
  }
}

static bool p3q_has_unexpanded(const P3Q& q, uint32_t iters) {
  if (iters != 0 && q.expands >= iters) return false;
  for (const Cand& c : q.cand) {
    if (!q.expanded.count(c.id)) return true;
  }
  return false;
}

static std::vector<uint32_t> p3q_pick(P3Q& q, uint32_t ebatch, uint32_t iters) {
  std::vector<uint32_t> batch;
  batch.reserve(ebatch);
  for (uint32_t t = 0; t < ebatch; ++t) {
    int bi = -1;
    float bd = 0;
    for (size_t i = 0; i < q.cand.size(); ++i) {
      if (q.expanded.count(q.cand[i].id)) continue;
      if (bi < 0 || q.cand[i].dist < bd) {
        bi = (int)i;
        bd = q.cand[i].dist;
      }
    }
    if (bi < 0) break;
    if (iters != 0 && q.expands >= iters) break;
    uint32_t id = q.cand[(size_t)bi].id;
    q.expanded.insert(id);
    q.expands++;
    batch.push_back(id);
  }
  return batch;
}

static void p3q_drain(P3Q& q, Placement& pl, DramWindow& win, uint32_t L, size_t vb, size_t pb) {
  if (q.pending.empty()) return;
  std::vector<P3Q::Pend> still;
  still.reserve(q.pending.size());
  for (const P3Q::Pend& e : q.pending) {
    const uint8_t* vp = pl.vec(e.id);
    alignas(64) uint8_t buf[4096];
    if (vb > sizeof(buf)) {
      fprintf(stderr, "vec too large for score buf\n");
      std::exit(2);
    }
    if (!win.try_copy_resident(pl.ssd_base, vp, vb, buf)) {
      still.push_back(e);
      continue;
    }
    q.pending_set.erase(e.id);
    float d = vec_mips_neg(buf, q.qf, pl.hdr->dim, pl.hdr->vec_bytes);
    if (cur_met(win)) {
      cur_met(win)->distance_comps++;
      cur_met(win)->note_score_from_window(1);
      cur_met(win)->note_pf_score_vec(vb);
      cur_met(win)->note_pf_scored_id(e.id);
      std::vector<uint64_t> sp;
      uint64_t off = (uint64_t)(vp - pl.ssd_base);
      uint64_t first = off & ~(uint64_t)(pb - 1);
      uint64_t last = (off + vb - 1) & ~(uint64_t)(pb - 1);
      for (uint64_t p = first; p <= last; p += pb) sp.push_back(p);
      cur_met(win)->note_pf_used(sp);
    }
    p3q_insert(q, L, e.id, d);
  }
  q.pending.swap(still);
}

static void p3q_collect_need(P3Q& q, Placement& pl, size_t vb, size_t pb) {
  q.need.clear();
  std::unordered_set<uint64_t> nps;
  for (const P3Q::Pend& e : q.pending) hide_collect_vec_pages(pl, vb, pb, e.id, q.need, &nps);
}

static void p3q_init(P3Q& q, Placement& pl, DramWindow& win, Prefetch& pref, const EntryGraph& eg,
                     const float* qf, uint32_t qi, uint32_t L) {
  q = P3Q{};
  q.qi = qi;
  q.qf = qf;
  q.t0 = std::chrono::steady_clock::now();
  q.cand.reserve(L + pl.hdr->R + 8);
  pref.on_query_begin(pl, win, eg.nodes, eg.entry_id);
  const size_t vb = (size_t)pl.hdr->dim * pl.hdr->vec_bytes;
  q.seen.insert(eg.entry_id);
  alignas(64) uint8_t buf[4096];
  bool hit = win.copy_if_resident(pl.ssd_base, pl.vec(eg.entry_id), vb, buf);
  if (!hit) win.copy_through(pl.ssd_base, pl.vec(eg.entry_id), vb, buf);
  if (cur_met(win)) {
    if (hit) cur_met(win)->note_score_from_window(1);
    else cur_met(win)->note_score_from_bounce(1);
    cur_met(win)->distance_comps++;
  }
  p3q_insert(q, L, eg.entry_id, vec_mips_neg(buf, qf, pl.hdr->dim, pl.hdr->vec_bytes));
  q.st = CbSt::Ready;
}

// Drain resident scores, issue the next e4 wave if the pipe has a slot, park.
// Never wait_covering — the stepper moves to another in-flight query.
static bool p3q_progress(P3Q& q, Placement& pl, DramWindow& win, PrefetchHub& hub, uint32_t L,
                         uint32_t ebatch, uint32_t ahead, uint32_t iters, uint32_t Rlim) {
  const size_t vb = (size_t)pl.hdr->dim * pl.hdr->vec_bytes;
  const size_t pb = win.page_bytes;
  const size_t p0 = q.pending.size();
  p3q_drain(q, pl, win, L, vb, pb);
  bool did = q.pending.size() < p0;
  uint32_t waves = 0;
  const uint32_t nwave = ahead ? ahead : 1;
  for (uint32_t w = 0; w < nwave; ++w) {
    if (!p3q_has_unexpanded(q, iters)) break;
    if (hub.free_slots() <= 0) break;
    auto batch = p3q_pick(q, ebatch ? ebatch : 1, iters);
    if (batch.empty()) break;
    std::vector<uint64_t> now;
    std::unordered_set<uint64_t> ps;
    now.reserve(batch.size() * 16);
    for (uint32_t cur : batch) {
      uint32_t nbrs_local[64];
      hide_read_nbrs(pl, cur, nbrs_local, Rlim);
      for (uint32_t j = 0; j < Rlim; ++j) {
        uint32_t nb = nbrs_local[j];
        if (nb >= pl.hdr->n || q.seen.count(nb)) continue;
        q.seen.insert(nb);
        hide_collect_vec_pages(pl, vb, pb, nb, now, &ps);
        if (q.pending_set.insert(nb).second) q.pending.push_back({nb, cur, 0});
      }
    }
    hub.submit(now);
    waves++;
    did = true;
  }
  if (!q.pending.empty()) {
    p3q_collect_need(q, pl, vb, pb);
    q.st = CbSt::Wait;
  } else {
    q.st = p3q_has_unexpanded(q, iters) ? CbSt::Ready : CbSt::Done;
  }
  return did || waves > 0;
}

static std::vector<uint32_t> p3q_finish(P3Q& q, uint32_t k) {
  std::sort(q.cand.begin(), q.cand.end(),
            [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
  std::vector<uint32_t> out;
  for (size_t i = 0; i < q.cand.size() && out.size() < k; ++i) out.push_back(q.cand[i].id);
  return out;
}

// Frozen PQ-64 end-batch, parkable: host ADC beam → one C_L issue → FP rerank.
// Never wait_covering; another in-flight query's beam hides the materialization.
struct PqQ {
  uint32_t qi = 0;
  const float* qf = nullptr;
  DistanceMetric metric = DistanceMetric::Mips;
  std::vector<float> lut;
  std::vector<Cand> cand;
  std::unordered_set<uint32_t> seen;
  std::unordered_set<uint32_t> expanded;
  uint32_t expands = 0;
  CbSt st = CbSt::Empty;
  std::vector<uint64_t> need;
  std::vector<uint64_t> fill_toks;
  std::vector<uint32_t> trace_candidates;
  bool nand_held = false;
  std::vector<uint8_t> fp_done;
  uint32_t fp_n = 0;
  std::chrono::steady_clock::time_point t0;
};

static bool pqq_covering(HidePipe& pipe, const std::vector<uint64_t>& need) {
  return pipe.covers(need);
}

static void pqq_insert(PqQ& q, PqTable& pq, uint32_t L, uint32_t id) {
  if (id >= pq.n) return;
  float d = pq.dist(id, q.lut.data());
  if (q.cand.size() >= L) {
    auto worst = std::max_element(q.cand.begin(), q.cand.end(),
                                  [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    if (d >= worst->dist) return;
    *worst = {d, id};
  } else {
    q.cand.push_back({d, id});
  }
}

static void pqq_init(PqQ& q, Placement& pl, DramWindow& win, Prefetch& pref, const EntryGraph& eg,
                     const float* qf, uint32_t qi, uint32_t L) {
  PqTable* pq = pref.pq;
  q = PqQ{};
  q.qi = qi;
  q.qf = qf;
  q.metric = pref.metric;
  q.t0 = std::chrono::steady_clock::now();
  q.cand.reserve(L + pl.hdr->R + 8);
  q.lut.resize((size_t)pq->nchunks * PqTable::kCentroids);
  pq->fill_lut(qf, q.lut.data(), q.metric);
  pref.on_query_begin(pl, win, eg.nodes, eg.entry_id);
  q.seen.insert(eg.entry_id);
  pqq_insert(q, *pq, L, eg.entry_id);
  q.st = CbSt::Ready;
}

static void pqq_beam(PqQ& q, Placement& pl, PqTable& pq, uint32_t L, uint32_t iters, uint32_t Rlim,
                     PrefetchHub* hub) {
  uint32_t since_pump = 0;
  while (true) {
    if (iters != 0 && q.expands >= iters) break;
    int bi = -1;
    float bd = 0;
    for (size_t i = 0; i < q.cand.size(); ++i) {
      if (q.expanded.count(q.cand[i].id)) continue;
      if (bi < 0 || q.cand[i].dist < bd) {
        bi = (int)i;
        bd = q.cand[i].dist;
      }
    }
    if (bi < 0) break;
    uint32_t cur = q.cand[(size_t)bi].id;
    q.expanded.insert(cur);
    q.expands++;
    uint32_t nbrs_local[64];
    hide_read_nbrs(pl, cur, nbrs_local, Rlim);
    for (uint32_t j = 0; j < Rlim; ++j) {
      uint32_t nb = nbrs_local[j];
      if (nb >= pl.hdr->n || q.seen.count(nb)) continue;
      q.seen.insert(nb);
      pqq_insert(q, pq, L, nb);
    }
    if (hub && (++since_pump & 15u) == 0) hub->pump();
  }
}

static void pqq_issue(PqQ& q, Placement& pl, PrefetchHub& hub, bool stall = false) {
  const size_t vb = (size_t)pl.hdr->dim * pl.hdr->vec_bytes;
  const size_t pb = hub.pipe.win->page_bytes;
  q.need.clear();
  std::unordered_set<uint64_t> ps;
  q.need.reserve(q.cand.size() * 2);
  for (const Cand& c : q.cand) hide_collect_vec_pages(pl, vb, pb, c.id, q.need, &ps);
  q.trace_candidates.clear();
  q.trace_candidates.reserve(q.cand.size());
  for (const Cand& c : q.cand) q.trace_candidates.push_back(c.id);
  uint64_t tok = stall ? hub.submit_block(q.need) : hub.submit(q.need);
  if (tok) q.fill_toks.push_back(tok);
  q.fp_done.assign(q.cand.size(), 0);
  q.fp_n = 0;
  q.st = q.need.empty() ? CbSt::Ready : CbSt::Wait;
}

// Score any covering C_L vector (window / bounce / vmem). Still commit-before-fetch:
// only committed IDs, no mid-beam NAND.
static bool pqq_rank(PqQ& q, Placement& pl, HidePipe& pipe) {
  if (!pqq_covering(pipe, q.need)) return false;
  const size_t vb = (size_t)pl.hdr->dim * pl.hdr->vec_bytes;
  const size_t pb = pipe.win->page_bytes;
  alignas(64) uint8_t buf[4096];
  if (vb > sizeof(buf)) {
    fprintf(stderr, "vec too large for score buf\n");
    std::exit(2);
  }
  std::vector<uint64_t> scored_pages;
  std::unordered_set<uint64_t> sps;
  for (Cand& c : q.cand) {
    const uint8_t* src = pipe.vec_src(pl.vec(c.id), vb);
    if (!src) {
      if (!pipe.copy_vec(pl.vec(c.id), vb, buf)) return false;
      src = buf;
    }
    c.dist = vec_distance(src, q.qf, pl.hdr->dim, pl.hdr->vec_bytes, q.metric);
    if (cur_met(*pipe.win)) {
      cur_met(*pipe.win)->distance_comps++;
      if (pipe.score == HideScore::Bounce) cur_met(*pipe.win)->note_score_from_bounce(1);
      else if (pipe.score == HideScore::Vmem) cur_met(*pipe.win)->note_score_from_cache(1);
      else cur_met(*pipe.win)->note_score_from_window(1);
      cur_met(*pipe.win)->note_pf_scored_id(c.id);
    }
    hide_collect_vec_pages(pl, vb, pb, c.id, scored_pages, &sps);
  }
  if (cur_met(*pipe.win)) cur_met(*pipe.win)->note_pf_used(scored_pages);
  for (uint64_t t : q.fill_toks) pipe.release_tok(t);
  pipe.release_pages(q.need);
  q.st = CbSt::Done;
  return true;
}

static std::vector<uint32_t> pqq_finish(PqQ& q, uint32_t k) {
  std::sort(q.cand.begin(), q.cand.end(),
            [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
  std::vector<uint32_t> out;
  for (size_t i = 0; i < q.cand.size() && out.size() < k; ++i) out.push_back(q.cand[i].id);
  return out;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const char* image = nullptr;
  const char* entry = nullptr;
  const char* queries = nullptr;
  const char* gt_path = nullptr;
  const char* id_map_path = nullptr;
  const char* vmem_dev = nullptr;
  const char* dax_dev = getenv("CXAN_DAX_DEV") ? getenv("CXAN_DAX_DEV") : "/dev/dax0.0";
  const char* cxl_dram_dev = getenv("CXAN_CXL_DRAM_DEV");
  off_t vmem_off = -1;
  off_t dax_off = getenv("CXAN_DAX_OFFSET") ? (off_t)strtoull(getenv("CXAN_DAX_OFFSET"), nullptr, 10)
                                            : 0;
  size_t vmem_len = 0;
  std::string policy_s = "P0";
  std::string metric_s = "mips";
  std::string dram_backend = getenv("CXAN_DRAM_BACKEND") ? getenv("CXAN_DRAM_BACKEND") : "numa";
  // numa = HOST scoring window (default). dax / --require-cxl-dram = BAR path (HPS).
  size_t budget = 64ull << 20;  // hide: 64 MiB lookahead budget default
  size_t dram_bytes = getenv("CXAN_DRAM_BYTES")
                          ? strtoull(getenv("CXAN_DRAM_BYTES"), nullptr, 10)
                          : (2ull << 30);
  size_t host_bytes = getenv("CXAN_HOST_BYTES")
                          ? strtoull(getenv("CXAN_HOST_BYTES"), nullptr, 10)
                          : (2ull << 30);
  bool require_cxl_dram = false;
  bool cpu_affinity = false;
  uint32_t pipe_w = 16;
  uint32_t install_top = 4;
  uint32_t fetch_top = 0;
  uint32_t beam = 32, k = 10, iters = 64;
  unsigned dram_numa = getenv("CXAN_DRAM_NODE") ? (unsigned)atoi(getenv("CXAN_DRAM_NODE")) : 1;
  bool rerank = false;
  bool oneshot_fp = false;  // one-shot FP MIPS (no PQ / no re-rank stage)
  bool flush_window = false;
  int max_q = -1;
  int shuffle_seed = -1;
  size_t host_cap = 32ull << 20;  // host-side cache budget
  bool pin_entry = true;
  bool page_group_b = false;
  bool install_all_fetched = false;
  bool vmem_prefetch = true;
  bool direct_install = false;
  HideScore hide_score = HideScore::Window;
  bool graph_in_dram = true;
  bool hide_warm_entry = true;
  const char* hide_warm_ids_path = nullptr;
  size_t hide_warm_bytes = 0;
  uint32_t expand_batch = 1;
  uint32_t issue_ahead = 1;
  bool issue_ahead_explicit = false;
  bool use_nbr_bundle = false;
  bool expand_batch_explicit = false;
  bool oracle_dram = false;
  bool oracle_window = false;
  bool diskann_cli = false;
  const char* nav_graph_path = nullptr;
  uint32_t nav_l = 64;
  const char* graph_file = nullptr;
  const char* dump_scored = nullptr;
  int nthreads = 1;
  int cont_inflight = 0;
  bool cont_batch_mode = false;
  bool cont_workers_set = false;
  bool per_thread_window = true;  // each request thread owns a DramWindow (no shared lock)
  int pipe_depth = 2;             // per-thread in-flight queries (PTW PQ path)
  uint32_t stagger_us = 0;        // frozen steal path: no start sleep
  bool steal_sched = true;        // frozen: dual-queue, never block compute on NAND
  uint32_t issue_qd = 0;          // 0 = match T (steal-sched)
  bool sync_hop = false;
  bool extent_run = false;
  bool extent_run_explicit = false;
  bool pq_nav = false;
  const char* pq_pivots_path = nullptr;
  const char* pq_compressed_path = nullptr;
  const char* id_slot_map_path = nullptr;
  const char* dump_expands_path = nullptr;
  const char* query_ids_path = nullptr;
  const char* eval_trace_dir = nullptr;

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
    else if (a == "--id-map") id_map_path = need(a.c_str());
    else if (a == "--vmem-dev") vmem_dev = need(a.c_str());
    else if (a == "--vmem-offset") vmem_off = (off_t)strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--vmem-len") vmem_len = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--dax-dev") dax_dev = need(a.c_str());
    else if (a == "--cxl-dram-dev") cxl_dram_dev = need(a.c_str());
    else if (a == "--dax-offset") dax_off = (off_t)strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--dram-backend") dram_backend = need(a.c_str());
    else if (a == "--host-bytes") host_bytes = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--require-cxl-dram") require_cxl_dram = true;
    else if (a == "--cpu-affinity") cpu_affinity = true;
    else if (a == "--dram-numa") dram_numa = (unsigned)atoi(need(a.c_str()));
    else if (a == "--policy") policy_s = need(a.c_str());
    else if (a == "--metric") metric_s = need(a.c_str());
    else if (a == "--budget") budget = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--pipe-w") pipe_w = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--install-top") install_top = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--fetch-top") fetch_top = (uint32_t)atoi(need(a.c_str()));
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
    else if (a == "--page-group") page_group_b = true;
    else if (a == "--no-page-group") page_group_b = false;
    else if (a == "--install-all-fetched") install_all_fetched = true;
    else if (a == "--no-vmem-prefetch") vmem_prefetch = false;
    else if (a == "--graph-in-dram") graph_in_dram = true;
    else if (a == "--no-graph-in-dram") graph_in_dram = false;
    else if (a == "--hide-warm-entry") hide_warm_entry = true;
    else if (a == "--no-hide-warm-entry") hide_warm_entry = false;
    else if (a == "--hide-warm-ids") hide_warm_ids_path = need(a.c_str());
    else if (a == "--hide-warm-bytes") hide_warm_bytes = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--lookahead-k") {
      need(a.c_str()); /* dropped: not a live Prefetch field */
    }
    else if (a == "--nbr-bundle") use_nbr_bundle = true;
    else if (a == "--expand-batch") {
      expand_batch = (uint32_t)atoi(need(a.c_str()));
      expand_batch_explicit = true;
    }
    else if (a == "--issue-ahead") {
      issue_ahead = (uint32_t)atoi(need(a.c_str()));
      issue_ahead_explicit = true;
    }
    else if (a == "--score-page" || a == "--no-score-page") {
      /* dropped: no live score_page path */
    }
    else if (a == "--min-issue-use") {
      need(a.c_str()); /* dropped: bundle-only, not a live Prefetch field */
    }
    else if (a == "--spec-beam-nbrs") {
      need(a.c_str()); /* dropped */
    }
    else if (a == "--expand-sib" || a == "--no-expand-sib") {
    }
    else if (a == "--sync-hop") sync_hop = true;
    else if (a == "--no-sync-hop") sync_hop = false;
    else if (a == "--score-bounce") hide_score = HideScore::Bounce;
    else if (a == "--score-vmem" || a == "--score-cache") hide_score = HideScore::Vmem;
    else if (a == "--no-score-cache") hide_score = HideScore::Window;
    else if (a == "--direct-install") direct_install = true;
    else if (a == "--no-direct-install") direct_install = false;
    else if (a == "--stripe-fill" || a == "--no-stripe-fill") {
    }
    else if (a == "--extent-run") {
      extent_run = true;
      extent_run_explicit = true;
    } else if (a == "--no-extent-run") {
      extent_run = false;
      extent_run_explicit = true;
    }
    else if (a == "--pipe-drive") { /* dropped: not part of frozen PQ path */ }
    else if (a == "--no-pipe-drive") { /* frozen path: pipe-drive is not implemented */ }
    else if (a == "--pq-nav") pq_nav = true;
    else if (a == "--no-pq-nav") pq_nav = false;
    else if (a == "--pq-pivots") pq_pivots_path = need(a.c_str());
    else if (a == "--pq-compressed") pq_compressed_path = need(a.c_str());
    else if (a == "--id-slot-map") id_slot_map_path = need(a.c_str());
    else if (a == "--dump-expands") dump_expands_path = need(a.c_str());
    else if (a == "--query-ids") query_ids_path = need(a.c_str());
    else if (a == "--eval-trace-dir") eval_trace_dir = need(a.c_str());
    else if (a == "--oracle-dram") oracle_dram = true;
    else if (a == "--oracle-window") oracle_window = true;
    else if (a == "--diskann-layout") diskann_cli = true;
    else if (a == "--nav-graph") nav_graph_path = need(a.c_str());
    else if (a == "--nav-l") nav_l = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--graph-file") graph_file = need(a.c_str());
    else if (a == "--dump-scored") dump_scored = need(a.c_str());
    else if (a == "--cont-batch") {
      cont_inflight = atoi(need(a.c_str()));
      per_thread_window = false;
      cont_batch_mode = cont_inflight > 0;
      if (!cont_workers_set && cont_inflight > 0) nthreads = cont_inflight;
    }
    else if (a == "--cont-workers") {
      nthreads = atoi(need(a.c_str()));
      cont_workers_set = true;
      per_thread_window = false;
      if (cont_inflight > 0) cont_batch_mode = true;
    }
    else if (a == "--threads") nthreads = atoi(need(a.c_str()));
    else if (a == "--per-thread-window") per_thread_window = true;
    else if (a == "--shared-window") per_thread_window = false;
    else if (a == "--pipe-depth") pipe_depth = atoi(need(a.c_str()));
    else if (a == "--stagger-us") stagger_us = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--stagger-ms") stagger_us = (uint32_t)atoi(need(a.c_str())) * 1000u;
    else if (a == "--steal-sched") steal_sched = true;
    else if (a == "--no-steal-sched") steal_sched = false;
    else if (a == "--issue-qd") issue_qd = (uint32_t)atoi(need(a.c_str()));
    else {
      fprintf(stderr, "unknown %s\n", a.c_str());
      return 2;
    }
  }
  if (oneshot_fp) rerank = false;
  DistanceMetric metric;
  try {
    metric = parse_distance_metric(metric_s);
  } catch (const std::invalid_argument& e) {
    fprintf(stderr, "bad --metric %s: %s\n", metric_s.c_str(), e.what());
    return 2;
  }
  if (eval_trace_dir && !pq_nav) {
    fprintf(stderr, "--eval-trace-dir requires --pq-nav\n");
    return 2;
  }
  if (dump_expands_path && dump_expands_path[0]) {
    g_expand_dump = std::fopen(dump_expands_path, "wb");
    if (!g_expand_dump) {
      fprintf(stderr, "failed to open --dump-expands %s\n", dump_expands_path);
      return 2;
    }
    printf("dump-expands %s\n", dump_expands_path);
  }
  std::vector<uint32_t> hide_warm_extra;
  if (hide_warm_ids_path && hide_warm_ids_path[0]) {
    FILE* wf = std::fopen(hide_warm_ids_path, "rb");
    if (!wf) {
      fprintf(stderr, "failed to open --hide-warm-ids %s\n", hide_warm_ids_path);
      return 2;
    }
    std::fseek(wf, 0, SEEK_END);
    long sz = std::ftell(wf);
    std::fseek(wf, 0, SEEK_SET);
    if (sz < 0 || (sz % 4) != 0) {
      fprintf(stderr, "bad --hide-warm-ids size %ld\n", sz);
      std::fclose(wf);
      return 2;
    }
    hide_warm_extra.resize((size_t)sz / 4);
    if (!hide_warm_extra.empty() &&
        std::fread(hide_warm_extra.data(), 4, hide_warm_extra.size(), wf) != hide_warm_extra.size()) {
      fprintf(stderr, "short --hide-warm-ids\n");
      std::fclose(wf);
      return 2;
    }
    std::fclose(wf);
    printf("hide-warm-ids %s n=%zu cap_bytes=%zu\n", hide_warm_ids_path, hide_warm_extra.size(),
           hide_warm_bytes);
  }
  if (diskann_cli && (!nav_graph_path || !nav_graph_path[0])) {
    fprintf(stderr, "--diskann-layout needs --nav-graph\n");
    return 2;
  }
  if (diskann_cli && oracle_dram && dram_backend == "numa") {
    fprintf(stderr, "refuse numa as CXL-DRAM oracle under --diskann-layout\n");
    return 2;
  }
  if (diskann_cli && use_nbr_bundle) {
    fprintf(stderr, "refuse --nbr-bundle with --diskann-layout\n");
    return 2;
  }
  // Three-layer roles: CXL-DRAM is the vmem BAR only. Refuse numa / dax / empty.
  if (require_cxl_dram) {
    if (dram_backend == "numa") {
      fprintf(stderr, "refuse numa as CXL-DRAM: anon+mbind is HOST DRAM\n");
      return 2;
    }
    if (!cxl_dram_dev || !cxl_dram_dev[0]) {
      fprintf(stderr,
              "--require-cxl-dram needs --cxl-dram-dev or CXAN_CXL_DRAM_DEV (/dev/vmem*)\n");
      return 2;
    }
    if (!is_vmem_bar_dev(cxl_dram_dev)) {
      fprintf(stderr, "refuse %s as CXL-DRAM: want /dev/vmem* BAR, not dax/anon\n",
              cxl_dram_dev);
      return 2;
    }
  }
  if (require_cxl_dram || oracle_dram || oracle_window)
    per_thread_window = false;  // shared host 2GiB + one BAR (or Oracle)
  if (host_bytes > (2ull << 30)) {
    fprintf(stderr, "HOST DRAM budget capped at 2GiB (got %zu)\n", host_bytes);
    return 2;
  }
  if (dram_backend == "numa" && dram_bytes > host_bytes) {
    fprintf(stderr, "HOST DRAM window %zu exceeds --host-bytes %zu\n", dram_bytes, host_bytes);
    return 2;
  }
  if (oracle_dram && !image && diskann_cli && cxl_dram_dev && cxl_dram_dev[0])
    image = cxl_dram_dev;
  if ((!image && !vmem_dev) || !entry || !queries) {
    fprintf(stderr,
            "need (--image FILE | --vmem-dev DEV --vmem-offset OFF --vmem-len LEN) "
            "--entry --queries [--gt]\n"
            "dram window: --dram-backend dax|numa [--dax-dev /dev/dax0.0] [--dax-offset N]\n"
            "             [--cxl-dram-dev DEV] [--host-bytes N] [--require-cxl-dram] "
            "[--cpu-affinity]\n"
            "optional: --oneshot-fp --shuffle-seed N --flush-window --max-q M\n");
    return 2;
  }
  if (dram_backend != "dax" && dram_backend != "numa") {
    fprintf(stderr, "bad --dram-backend %s (want dax|numa)\n", dram_backend.c_str());
    return 2;
  }
  if (dram_backend == "numa" && numa_available() < 0) {
    fprintf(stderr, "numa required for --dram-backend numa\n");
    return 2;
  }
  if (oracle_dram && !image && !vmem_dev) {
    if (diskann_cli && cxl_dram_dev && cxl_dram_dev[0])
      image = cxl_dram_dev;
    else {
      fprintf(stderr, "--oracle-dram needs --image, --vmem-dev, or --cxl-dram-dev\n");
      return 2;
    }
  }
  if (oracle_dram) {
    hide_warm_entry = false;
    // Keep --vmem-dev: corpus already in the vmem VA (page-cache / ram_map).
    // Only drop vmem when scoring a host/dax --image.
    if (image && image[0]) vmem_dev = nullptr;
  }

  size_t img_len = 0;
  void* img = nullptr;
  int vmem_fd = -1;
  if (vmem_dev) {
    if (vmem_off < 0 || vmem_len == 0) {
      fprintf(stderr, "vmem requires --vmem-offset and --vmem-len\n");
      return 2;
    }
    img = map_vmem_ro(vmem_dev, vmem_off, vmem_len, &vmem_fd);
    img_len = vmem_len;
    printf("mapped vmem %s off=%lld len=%zu fd=%d prefetch=%d cache_used=%llu\n", vmem_dev,
           (long long)vmem_off, vmem_len, vmem_fd, (int)vmem_prefetch,
           (unsigned long long)vmem_cache_used());
    if (oracle_dram) {
      auto* b = static_cast<volatile const char*>(img);
      for (size_t off = 0; off < img_len; off += 4096) (void)b[off];
      printf("oracle warmed vmem corpus cache_used=%llu\n",
             (unsigned long long)vmem_cache_used());
    }
  } else if (image && is_dax_path(image)) {
    img = map_dax_corpus_ro(image, &img_len);
  } else {
    img = map_file_ro(image, &img_len, oracle_dram);
  }
  auto* hdr = reinterpret_cast<CxanLayoutHeader*>(img);
  if (hdr->magic != kCxanMagic) {
    fprintf(stderr, "bad magic\n");
    return 2;
  }

  Placement pl;
  pl.set_header(hdr);
  if (diskann_cli) pl.diskann_layout = true;
  pl.ssd_base = static_cast<const uint8_t*>(img);
  pl.ssd_bytes = img_len;
  NavGraph nav;
  if (nav_graph_path && nav_graph_path[0]) {
    if (!nav.load(nav_graph_path)) {
      fprintf(stderr, "failed to load --nav-graph %s\n", nav_graph_path);
      return 2;
    }
    printf("nav-graph %s n0=%u dim=%u R=%u\n", nav_graph_path, nav.n0, nav.dim, nav.R);
  }
  if (pl.diskann_layout && !nav.loaded()) {
    fprintf(stderr, "diskann layout needs --nav-graph\n");
    return 2;
  }
  if (id_slot_map_path && id_slot_map_path[0]) {
    if (!pl.load_id_slot_map(id_slot_map_path)) {
      fprintf(stderr, "failed to load --id-slot-map %s\n", id_slot_map_path);
      return 2;
    }
    printf("id-slot-map %s n=%u (physical neighbor pairing)\n", id_slot_map_path, hdr->n);
  }
  if (use_nbr_bundle && pl.diskann_layout) {
    fprintf(stderr, "refuse --nbr-bundle with diskann layout\n");
    return 2;
  }
  if (pl.diskann_layout && !use_nbr_bundle) {
    if (!extent_run_explicit) extent_run = true;
    if (!oneshot_fp && !pq_nav) {
      fprintf(stderr,
              "diskann hide needs --pq-nav (codebook) or --oneshot-fp (ablation)\n");
      return 2;
    }
  }
  if (use_nbr_bundle) {
    const uint64_t packed = (uint64_t)hdr->dim * hdr->vec_bytes;
    const uint64_t stride = (hdr->R * packed + 4095ull) & ~4095ull;
    const uint64_t bbytes = (uint64_t)hdr->n * stride;
    const uint64_t layout_len = vmem_len ? vmem_len : img_len;
    if (vmem_dev && vmem_fd >= 0 && layout_len + bbytes > img_len) {
      munmap(img, img_len);
      img = mmap(nullptr, layout_len + bbytes, PROT_READ | PROT_WRITE, MAP_SHARED, vmem_fd,
                 vmem_off);
      if (img == MAP_FAILED) die("mmap vmem layout+bundle");
      img_len = layout_len + bbytes;
      hdr = reinterpret_cast<CxanLayoutHeader*>(img);
      pl.set_header(hdr);
      pl.ssd_base = static_cast<const uint8_t*>(img);
    }
    pl.ssd_bytes = img_len;
    pl.bundle_off = layout_len;
    pl.bundle_stride = stride;
    pl.bundle_bytes = bbytes;
    if (!expand_batch_explicit) expand_batch = 8;
    if (!issue_ahead_explicit) issue_ahead = 2;
    printf("nbr-bundle off=%llu stride=%llu bytes=%.2f GiB\n",
           (unsigned long long)pl.bundle_off, (unsigned long long)pl.bundle_stride,
           pl.bundle_bytes / (1024.0 * 1024 * 1024));
  }
  std::vector<uint8_t> graph_host_buf;
  if (graph_in_dram && hdr->len_graph && !pl.diskann_layout) {
    graph_host_buf.resize((size_t)hdr->len_graph);
    bool ok = false;
    if (graph_file && graph_file[0]) {
      FILE* gf = fopen(graph_file, "rb");
      if (gf && fread(graph_host_buf.data(), 1, graph_host_buf.size(), gf) ==
                    graph_host_buf.size())
        ok = true;
      if (gf) fclose(gf);
      if (ok) printf("graph-file %s\n", graph_file);
    }
    const char* host_layout = getenv("CXAN_HOST_LAYOUT");
    if (!ok && host_layout && host_layout[0]) {
      FILE* hf = fopen(host_layout, "rb");
      if (hf) {
        CxanLayoutHeader hh{};
        if (fread(&hh, 1, sizeof(hh), hf) == sizeof(hh) && hh.magic == kCxanMagic &&
            hh.n == hdr->n && hh.R == hdr->R && hh.len_graph == hdr->len_graph &&
            fseeko(hf, (off_t)hh.off_graph, SEEK_SET) == 0 &&
            fread(graph_host_buf.data(), 1, graph_host_buf.size(), hf) ==
                graph_host_buf.size())
          ok = true;
        fclose(hf);
        if (ok && memcmp(graph_host_buf.data(), pl.ssd_base + hdr->off_graph,
                         std::min<size_t>(64, graph_host_buf.size())) != 0) {
          fprintf(stderr, "WARN CXAN_HOST_LAYOUT graph != mapped image; using mmap\n");
          ok = false;
        }
      }
    }
    if (!ok) {
      memcpy(graph_host_buf.data(), pl.ssd_base + hdr->off_graph, graph_host_buf.size());
      fprintf(stderr, "WARN graph-in-dram copied from mmap (may warm vmem cache)\n");
    }
    pl.set_graph_host(graph_host_buf.data(), graph_host_buf.size());
    printf("graph-in-dram bytes=%zu src=%s\n", graph_host_buf.size(),
           graph_file && ok ? "graph-file" : (ok ? "CXAN_HOST_LAYOUT" : "mmap"));
  }
  if (graph_in_dram && pl.diskann_layout && graph_file && graph_file[0] && hdr->n &&
      hdr->R) {
    const size_t need = (size_t)hdr->n * (size_t)hdr->R * 4;
    graph_host_buf.resize(need);
    FILE* gf = fopen(graph_file, "rb");
    bool okg = gf && fread(graph_host_buf.data(), 1, need, gf) == need;
    if (gf) fclose(gf);
    if (okg) {
      pl.set_graph_host(graph_host_buf.data(), need);
      printf("diskann graph-file %s bytes=%zu (host N(u), no SSD ID faults)\n", graph_file,
             need);
    } else {
      fprintf(stderr, "WARN diskann --graph-file %s read failed; N(u) still from mmap\n",
              graph_file);
    }
  }
  printf("vec_stride=%zu (packed=%zu) graph_in_dram=%d diskann=%d\n", pl.vec_stride,
         (size_t)hdr->dim * hdr->vec_bytes, (int)graph_in_dram, (int)pl.diskann_layout);

  Metrics metrics;
  void* dram = nullptr;
  if (cxl_dram_dev && cxl_dram_dev[0] && dram_backend != "numa" && !require_cxl_dram)
    dax_dev = cxl_dram_dev;
  if (pl.diskann_layout && oracle_dram) {
    // Corpus is already the CXL-DRAM mmap. Do not RW-map / zero-fault /dev/dax0.0.
    const size_t dummy = 64ull << 20;
    dram = mmap(nullptr, dummy, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (dram == MAP_FAILED) die("mmap dummy oracle window");
    dram_bytes = dummy;
    metrics.window_is_cxl_dram = true;
    printf("oracle DiskANN scores from corpus mmap (%s); dummy window %zu\n",
           image ? image : (vmem_dev ? vmem_dev : "?"), dummy);
  } else if (require_cxl_dram) {
    // Validated above: cxl_dram_dev is /dev/vmem*. Never fall back to /dev/dax*.
    dram = map_dram_dax(cxl_dram_dev, dax_off, dram_bytes);
    metrics.window_is_cxl_dram = true;
  } else if (dram_backend == "dax") {
    dram = map_dram_dax(dax_dev, dax_off, dram_bytes);
  } else {
    dram = map_dram_numa(dram_bytes, dram_numa);
  }
  DramWindow win;
  if (hide_warm_entry) {
    win.pin_bytes_cap = 256ull << 20;
    // Leave headroom so clock eviction stays O(1).
    win.soft_pin_bytes_cap = 512ull << 20;
    if (dram_bytes >= (2ull << 30)) {
      win.pin_bytes_cap = 512ull << 20;
      win.soft_pin_bytes_cap = 1024ull << 20;
    }
  }
  win.init(dram, dram_bytes, &metrics);
  win.soft_pin_neighbors = page_group_b;

  Prefetch pref;
  pref.policy = parse_policy(policy_s);
  pref.metric = metric;
  pref.budget_per_query = budget;
  pref.pin_entry = pin_entry;
  issue_qd = effective_issue_qd(issue_qd, nthreads);
  if (steal_sched && nthreads > 1) {
    uint32_t io_w = issue_qd > (uint32_t)nthreads ? issue_qd : (uint32_t)nthreads;
    if (pipe_w < io_w) pipe_w = io_w;
  }
  pref.pipe_w = pipe_w ? pipe_w : 4;
  pref.install_top = install_top ? install_top : 4;
  pref.fetch_top = fetch_top;
  pref.page_group_b = page_group_b;
  pref.install_all_fetched = install_all_fetched;
  pref.expand_batch = expand_batch ? expand_batch : 1;
  pref.issue_ahead = issue_ahead ? issue_ahead : 1;
  pref.sync_hop = sync_hop;
  pref.extent_run = extent_run;
  pref.direct_install = direct_install;
  pref.hide_score = hide_score;
  pref.oracle_dram = oracle_dram;
  printf("metric=%s\n", distance_metric_name(pref.metric));
  // P2v2 is cooperative single-threaded (DAX is not safe for concurrent promote).
  if (false && pref.policy == PrefetchPolicy::P2) pref.start_async(&pl, &win);

  if (pref.policy == PrefetchPolicy::P3) {
    printf("P3v2 softpin W=%u budget=%zu install_top=%u fetch_top=%u page_group_b=%d "
           "install_all=%d threads=%d inflight=%d per_thread_window=%d cont_batch=%d "
           "pipe_depth=%d expand_batch=%u issue_ahead=%u slot_map=%d sync_hop=%d extent_run=%d "
           "direct_install=%d hide_score=%s stagger_us=%u steal_sched=%d issue_qd=%u\n",
           pref.pipe_w, budget, pref.install_top, pref.fetch_top, (int)page_group_b,
           (int)install_all_fetched, nthreads, cont_inflight, (int)per_thread_window,
           (int)cont_batch_mode, pipe_depth, pref.expand_batch, pref.issue_ahead,
           (int)!pl.id_to_slot.empty(), (int)pref.sync_hop, (int)pref.extent_run,
           (int)pref.direct_install,
           pref.hide_score == HideScore::Bounce ? "bounce"
           : pref.hide_score == HideScore::Vmem ? "vmem"
                                                 : "window",
           stagger_us, (int)steal_sched, issue_qd);
  }

  EntryGraph eg = load_entry(entry);
  const std::vector<uint32_t>& hide_warm_extras =
      (hide_warm_ids_path && hide_warm_ids_path[0]) ? hide_warm_extra : eg.nodes;
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

  std::vector<uint32_t> qidx;
  if (query_ids_path && query_ids_path[0]) {
    FILE* qf = std::fopen(query_ids_path, "rb");
    if (!qf) {
      fprintf(stderr, "failed to open --query-ids %s\n", query_ids_path);
      return 2;
    }
    std::fseek(qf, 0, SEEK_END);
    long qsz = std::ftell(qf);
    std::fseek(qf, 0, SEEK_SET);
    if (qsz < 4 || (qsz % 4) != 0) {
      fprintf(stderr, "bad --query-ids size %ld\n", qsz);
      std::fclose(qf);
      return 2;
    }
    qidx.resize((size_t)qsz / 4);
    if (std::fread(qidx.data(), 4, qidx.size(), qf) != qidx.size()) {
      fprintf(stderr, "short --query-ids\n");
      std::fclose(qf);
      return 2;
    }
    std::fclose(qf);
    for (uint32_t id : qidx) {
      if (id >= nq_file) {
        fprintf(stderr, "query-id %u >= nq_file %u\n", id, nq_file);
        return 2;
      }
    }
  } else {
    qidx.resize(nq_file);
    std::iota(qidx.begin(), qidx.end(), 0);
    if (shuffle_seed >= 0) {
      std::mt19937 rng((uint32_t)shuffle_seed);
      std::shuffle(qidx.begin(), qidx.end(), rng);
    }
    if (max_q > 0 && (uint32_t)max_q < (uint32_t)qidx.size()) qidx.resize((size_t)max_q);
  }
  uint32_t nq = (uint32_t)qidx.size();
  if (max_q > 0 && (uint32_t)max_q < nq) {
    nq = (uint32_t)max_q;
    qidx.resize(nq);
  }

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

  std::vector<uint32_t> id_map;
  if (id_map_path) {
    int mfd = open(id_map_path, O_RDONLY);
    if (mfd < 0) die("open id-map");
    struct stat st {};
    if (fstat(mfd, &st) != 0) die("stat id-map");
    const size_t need = (size_t)hdr->n * 4;
    if ((size_t)st.st_size < need) {
      fprintf(stderr, "id-map size %lld < n*4=%zu\n", (long long)st.st_size, need);
      return 2;
    }
    id_map.resize(hdr->n);
    if (read(mfd, id_map.data(), need) != (ssize_t)need) die("read id-map");
    close(mfd);
    printf("loaded id-map new→old n=%u (file=%lld) from %s\n", hdr->n,
           (long long)st.st_size, id_map_path);
  }

  PqTable pq_store;
  if (pq_nav) {
    if (!pq_pivots_path || !pq_compressed_path) {
      fprintf(stderr, "--pq-nav needs --pq-pivots and --pq-compressed\n");
      return 2;
    }
    if (!pq_store.load_pivots(pq_pivots_path)) { fprintf(stderr, "failed to load --pq-pivots %s\n", pq_pivots_path); return 2; }
    const uint32_t* n2o = id_map.empty() ? nullptr : id_map.data();
    const uint32_t n2o_n = (uint32_t)id_map.size();
    if (!pq_store.load_codes(pq_compressed_path, n2o, n2o_n)) { fprintf(stderr, "failed to load --pq-compressed %s\n", pq_compressed_path); return 2; }
    pref.pq = &pq_store;
    pref.pq_nav = true;
    printf("pq-nav pivots=%s codes=%s n=%u dim=%u chunks=%u permuted=%d\n",
           pq_pivots_path, pq_compressed_path, pq_store.n, pq_store.dim, pq_store.nchunks, (int)(n2o != nullptr));
  }

  printf("query_select nq=%u/%u shuffle_seed=%d query_ids=%s first=%u last=%u flush_window=%d oneshot_fp=%d "
         "oracle_dram=%d oracle_window=%d "
         "pin_entry=%d host_used=%zu host_cap=%zu host_bytes=%zu dram_bytes=%zu pin_cap=%zu\n",
         nq, nq_file, shuffle_seed,
         (query_ids_path && query_ids_path[0]) ? query_ids_path : "-",
         qidx.empty() ? 0 : qidx.front(), qidx.empty() ? 0 : qidx.back(),
         (int)flush_window, (int)oneshot_fp,
         (int)oracle_dram, (int)oracle_window, (int)pin_entry,
         host_used, host_cap, host_bytes, dram_bytes, win.pin_bytes_cap);
  printf("roles host_bytes=%zu require_cxl_dram=%d cpu_affinity=%d per_thread_window=%d "
         "cxl_dram_dev=%s\n",
         host_bytes, (int)require_cxl_dram, (int)cpu_affinity, (int)per_thread_window,
         (cxl_dram_dev && cxl_dram_dev[0]) ? cxl_dram_dev : "-");
  fflush(stdout);

  std::vector<double> lat_ms;
  lat_ms.reserve(nq);
  double recall_sum = 0;
  uint32_t recall_n = 0;
  std::unique_ptr<EvalTrace> eval_trace;

  const bool use_shared_pool =
      pref.policy == PrefetchPolicy::P3 &&
      (!(per_thread_window && nthreads > 1) || steal_sched);
  PageCopyPool shared_pool;
  if (use_shared_pool) shared_pool.start(pref.pipe_w);
  VmemIo vio;
  vio.fd = vmem_fd;
  vio.image_off = vmem_off >= 0 ? vmem_off : 0;
  vio.prefetch = vmem_prefetch && vmem_fd >= 0;

  if (hide_warm_entry && pref.policy == PrefetchPolicy::P3 && !oracle_dram) {
    PageCopyPool* wp = use_shared_pool ? &shared_pool : nullptr;
    PageCopyPool warm_pool;
    if (!wp) {
      warm_pool.start(pref.pipe_w);
      wp = &warm_pool;
    }
    size_t nw = hide_warm_entry_ball(pl, win, *wp, &vio, hide_warm_extras, eg.entry_id,
                                    hide_warm_bytes);
    pref.entry_pinned = true;
    printf("hide_warm_entry ids=%zu pin_bytes=%zu/%zu\n", nw, win.pin_bytes_used,
           win.pin_bytes_cap);
    fflush(stdout);
    metrics.pf_issued.clear();
    metrics.pf_used.clear();
    metrics.pf_look.clear();
    metrics.pf_scored_vec_bytes = 0;
    metrics.pf_ids_on_issued.clear();
    metrics.pf_ids_scored.clear();
    metrics.pf_issue_slots = 0;
    metrics.pf_issue_want_slots = 0;
  }

  auto run_one_q = [&](uint32_t qi, Prefetch& lp, DramWindow& w, PageCopyPool* pool) {
    if (flush_window) w.flush();
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
    EntryGraph qeg = eg;
    if (nav.loaded()) qeg.entry_id = nav.search_entry(qf, nav_l);
    std::vector<uint32_t> committed;
    auto ids = search_one(pl, w, lp, nullptr, qeg, qf, qpq.data(), beam, k, iters, rerank,
                          oneshot_fp, pool, &vio, eval_trace ? &committed : nullptr);
    auto tq1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tq1 - tq0).count();
    double rec = -1;
    if (!id_map.empty()) {
      for (uint32_t& id : ids) {
        if (id < id_map.size()) id = id_map[id];
      }
    }
    if (gt_path) {
      auto g = load_gt_row(gt_all.data(), gt_k, qi, k);
      rec = recall_at_k(ids, g);
    }
    if (eval_trace) {
      const uint64_t ns =
          (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(tq1 - tq0).count();
      eval_trace->record(qi, qidx[qi], ns, std::move(committed), std::move(ids));
    }
    return std::make_pair(ms, rec);
  };

  // Shared / T=1: pin eval WS into the window that the timed loop actually uses.
  // Per-thread windows are created below — warming the unused main window would
  // freeze_fills before those windows exist (empty 512MiB, bogus T-way oracle).
  if (oracle_window && !(nthreads > 1 && per_thread_window)) {
    printf("oracle-window discarded pass nq=%u (pin WS into shared window)\n", nq);
    fflush(stdout);
    for (uint32_t qi = 0; qi < nq; ++qi)
      (void)run_one_q(qi, pref, win, use_shared_pool ? &shared_pool : nullptr);
    metrics.reset();
    pref.freeze_fills = true;
    vio.prefetch = false;
    printf("oracle-window timed pass freeze_fills=1 pin_bytes=%zu\n", win.pin_bytes_used);
    fflush(stdout);
  }

  FILE* scored_fp = nullptr;
  if (dump_scored) {
    if (nthreads > 1) die("--dump-scored needs --threads 1");
    scored_fp = fopen(dump_scored, "wb");
    if (!scored_fp) die("open --dump-scored");
    uint32_t mag = 0x44524353u, z = nq;
    fwrite(&mag, 4, 1, scored_fp);
    fwrite(&z, 4, 1, scored_fp);
    printf("dump-scored %s nq=%u\n", dump_scored, nq);
  }

  struct ThrCtx {
    void* dram = nullptr;
    DramWindow win;
    PageCopyPool pool;
    Metrics m;
  };
  std::vector<std::unique_ptr<ThrCtx>> thr_ctx;
  if (nthreads > 1 && per_thread_window) {
    auto pin = 256ull << 20;
    auto soft = 512ull << 20;
    if (pin > dram_bytes / 4) pin = dram_bytes / 4;
    if (soft > dram_bytes / 2) soft = dram_bytes / 2;
    for (int t = 0; t < nthreads; ++t) {
      auto c = std::make_unique<ThrCtx>();
      if (require_cxl_dram) {
        fprintf(stderr, "refuse per-thread window under --require-cxl-dram\n");
        return 2;
      }
      if (dram_backend == "dax")
        c->dram = map_dram_dax(dax_dev, dax_off + (off_t)t * (off_t)dram_bytes, dram_bytes);
      else
        c->dram = map_dram_numa(dram_bytes, dram_numa);
      c->m.window_is_cxl_dram = metrics.window_is_cxl_dram;
      if (hide_warm_entry) {
        c->win.pin_bytes_cap = pin;
        c->win.soft_pin_bytes_cap = soft;
      }
      c->win.init(c->dram, dram_bytes, &c->m);
      c->win.soft_pin_neighbors = page_group_b;
      if (pref.policy == PrefetchPolicy::P3 && !use_shared_pool) {
        c->pool.start(pref.pipe_w);
        if (hide_warm_entry && !oracle_dram) {
          hide_warm_entry_ball(pl, c->win, c->pool, &vio, hide_warm_extras, eg.entry_id,
                              hide_warm_bytes);
          c->m.pf_issued.clear();
          c->m.pf_used.clear();
          c->m.pf_look.clear();
          c->m.pf_ids_on_issued.clear();
          c->m.pf_ids_scored.clear();
          c->m.pf_scored_vec_bytes = 0;
          c->m.pf_issue_slots = 0;
          c->m.pf_issue_want_slots = 0;
        }
      }
      thr_ctx.push_back(std::move(c));
    }
    printf("per-thread windows=%d dram_bytes=%zu warm_before_timed=1\n", nthreads, dram_bytes);
    fflush(stdout);
  }

  if (oracle_window && nthreads > 1 && per_thread_window) {
    printf("oracle-window discarded pass nq=%u threads=%d (pin each thread's own queries)\n",
           nq, nthreads);
    fflush(stdout);
    for (int t = 0; t < nthreads; ++t) {
      for (uint32_t qi = (uint32_t)t; qi < nq; qi += (uint32_t)nthreads)
        (void)run_one_q(qi, pref, thr_ctx[(size_t)t]->win, &thr_ctx[(size_t)t]->pool);
    }
    metrics.reset();
    for (auto& c : thr_ctx) c->m.reset();
    pref.freeze_fills = true;
    vio.prefetch = false;
    printf("oracle-window timed pass freeze_fills=1 pin_bytes=%zu\n",
           thr_ctx.empty() ? 0 : thr_ctx[0]->win.pin_bytes_used);
    fflush(stdout);
  }

  if (eval_trace_dir && eval_trace_dir[0]) {
    eval_trace = std::make_unique<EvalTrace>(nq, k);
    printf("eval_trace_dir=%s nq=%u k=%u\n", eval_trace_dir, nq, k);
    fflush(stdout);
  }

  const uint64_t nvme_sect0 = nvme_read_sectors();
  std::vector<int> aff_cpus;
  if (cpu_affinity) aff_cpus = list_cpu_ids();
  if (cpu_affinity && nthreads <= 1 && !aff_cpus.empty()) bind_worker_cpu(aff_cpus[0]);
  auto t0 = std::chrono::steady_clock::now();
  const int cb_inflight = cont_inflight > 0 ? cont_inflight : nthreads;
  const int cb_workers = nthreads > 0 ? nthreads : 1;
  const bool run_cb_sched = cont_batch_mode && cb_inflight > 0 && cb_workers > 0 &&
                            pref.policy == PrefetchPolicy::P3 && !oracle_dram &&
                            pref.pq_nav && pref.pq && !oneshot_fp;
  if (run_cb_sched) {
    const int T = cb_inflight;
    const int W = cb_workers;
    const uint32_t L = beam ? beam : k;
    uint32_t Rlim = hdr->R;
    if (Rlim > 64) Rlim = 64;
    PrefetchHub hub;
    hub.bind(use_shared_pool ? &shared_pool : nullptr, &win, &pl, &vio, &metrics);
    hub.pipe.extent_run = pref.extent_run;
    hub.pipe.direct_install = pref.direct_install;
    hub.pipe.score = pref.hide_score;
    if (!hub.pipe.pool) {
      fprintf(stderr, "cont-batch needs a shared PageCopyPool\n");
      return 2;
    }
    std::vector<PqQ> slots((size_t)T);
    lat_ms.assign(nq, 0);
    std::vector<double> recs(nq, -1);
    printf("cont_batch_sched=1 pq_endbatch=1 inflight=%d workers=%d interleave=1\n", T, W);
    fflush(stdout);
    std::mutex sched_mu;
    std::atomic<uint32_t> next_q{0};
    std::atomic<uint32_t> finished{0};
    std::atomic<int> rr{0};
    auto finish_slot = [&](PqQ& q) {
      auto ids = pqq_finish(q, k);
      auto tq1 = std::chrono::steady_clock::now();
      lat_ms[q.qi] = std::chrono::duration<double, std::milli>(tq1 - q.t0).count();
      if (!id_map.empty()) {
        for (uint32_t& id : ids) {
          if (id < id_map.size()) id = id_map[id];
        }
      }
      if (gt_path) {
        auto g = load_gt_row(gt_all.data(), gt_k, q.qi, k);
        recs[q.qi] = recall_at_k(ids, g);
      }
      if (eval_trace) {
        const uint64_t ns =
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(tq1 - q.t0).count();
        eval_trace->record(q.qi, qidx[q.qi], ns, std::move(q.trace_candidates),
                           std::move(ids));
      }
      q = PqQ{};
      q.st = CbSt::Empty;
      finished.fetch_add(1, std::memory_order_relaxed);
    };
    auto admit = [&]() {
      while (next_q.load(std::memory_order_relaxed) < nq) {
        int slot = -1;
        for (int i = 0; i < T; ++i) {
          if (slots[(size_t)i].st == CbSt::Empty) {
            slot = i;
            break;
          }
        }
        if (slot < 0) break;
        uint32_t qi = next_q.fetch_add(1, std::memory_order_relaxed);
        if (qi >= nq) break;
        EntryGraph qeg = eg;
        const float* qf = qbuf.data() + (size_t)qi * qdim;
        if (nav.loaded()) qeg.entry_id = nav.search_entry(qf, nav_l);
        pqq_init(slots[(size_t)slot], pl, win, pref, qeg, qf, qi, L);
      }
    };
    auto pick_pq = [&]() -> int {
      int start = rr.fetch_add(1, std::memory_order_relaxed);
      for (int koff = 0; koff < T; ++koff) {
        int i = (start + koff) % T;
        if (slots[(size_t)i].st == CbSt::Ready) return i;
      }
      for (int koff = 0; koff < T; ++koff) {
        int i = (start + koff) % T;
        if (slots[(size_t)i].st == CbSt::Wait &&
            pqq_covering(hub.pipe, slots[(size_t)i].need))
          return i;
      }
      return -1;
    };
    std::vector<std::thread> ths;
    std::mutex merge_mu;
    ths.reserve((size_t)W);
    for (int t = 0; t < W; ++t) {
      ths.emplace_back([&, t] {
        if (cpu_affinity) {
          int cpu_id = aff_cpus.empty() ? t : aff_cpus[(size_t)t % aff_cpus.size()];
          bind_worker_cpu(cpu_id);
        }
        Metrics local_m;
        local_m.window_is_cxl_dram = metrics.window_is_cxl_dram;
        tls_metrics = &local_m;
        while (finished.load(std::memory_order_relaxed) < nq) {
          hub.pump();
          int idx = -1;
          {
            std::lock_guard<std::mutex> g(sched_mu);
            admit();
            idx = pick_pq();
            if (idx >= 0) slots[(size_t)idx].st = CbSt::Busy;
          }
          if (idx < 0) {
            if (!hub.any_inflight()) {
              std::lock_guard<std::mutex> g(sched_mu);
              for (int i = 0; i < T; ++i) {
                if (slots[(size_t)i].st == CbSt::Wait && !slots[(size_t)i].need.empty())
                  hub.submit(slots[(size_t)i].need);
              }
            }
            std::this_thread::yield();
            continue;
          }
          PqQ& q = slots[(size_t)idx];
          // Busy replaced Ready or covering-Wait. Infer work from need/expands.
          if (q.need.empty()) {
            pqq_beam(q, pl, *pref.pq, L, iters, Rlim, &hub);
            pqq_issue(q, pl, hub);
            if (q.st == CbSt::Ready || pqq_covering(hub.pipe, q.need)) pqq_rank(q, pl, hub.pipe);
          } else {
            pqq_rank(q, pl, hub.pipe);
          }
          {
            std::lock_guard<std::mutex> g(sched_mu);
            if (q.st == CbSt::Done) finish_slot(q);
            else if (q.st == CbSt::Busy) q.st = CbSt::Wait;
          }
        }
        tls_metrics = nullptr;
        std::lock_guard<std::mutex> g(merge_mu);
        metrics.add_from(local_m);
      });
    }
    for (auto& th : ths) th.join();
    for (uint32_t qi = 0; qi < nq; ++qi) {
      if (recs[qi] >= 0) {
        recall_sum += recs[qi];
        recall_n++;
      }
      metrics.queries++;
    }
    printf("cont_batch_hub issue_n=%llu issue_pages=%llu avg_pages=%.1f\n",
           (unsigned long long)hub.issue_n, (unsigned long long)hub.issue_pages,
           hub.issue_n ? (double)hub.issue_pages / (double)hub.issue_n : 0.0);
  } else if (nthreads <= 1) {
    for (uint32_t qi = 0; qi < nq; ++qi) {
      auto r = run_one_q(qi, pref, win, use_shared_pool ? &shared_pool : nullptr);
      if (scored_fp) {
        uint32_t ns = (uint32_t)metrics.pf_ids_scored.size();
        fwrite(&ns, 4, 1, scored_fp);
        for (uint32_t id : metrics.pf_ids_scored) fwrite(&id, 4, 1, scored_fp);
        metrics.pf_ids_scored.clear();
      }
      lat_ms.push_back(r.first);
      if (r.second >= 0) {
        recall_sum += r.second;
        recall_n++;
      }
      metrics.queries++;
    }
  } else {
    lat_ms.assign(nq, 0);
    std::vector<double> recs(nq, -1);
    std::atomic<uint32_t> next_q{0};
    std::atomic<uint32_t> nand_inflight{0};
    std::mutex admit_mu;
    std::vector<std::thread> ths;
    std::mutex merge_mu;
    ths.reserve((size_t)nthreads);
    for (int t = 0; t < nthreads; ++t) {
      ths.emplace_back([&, t] {
        if (cpu_affinity) {
          int cpu_id = aff_cpus.empty() ? t : aff_cpus[(size_t)t % aff_cpus.size()];
          bind_worker_cpu(cpu_id);
        }
        Prefetch lp;
        lp.policy = pref.policy;
        lp.metric = pref.metric;
        lp.budget_per_query = pref.budget_per_query;
        lp.pin_entry = pref.pin_entry;
        lp.pipe_w = pref.pipe_w;
        lp.install_top = pref.install_top;
        lp.fetch_top = pref.fetch_top;
        lp.page_group_b = pref.page_group_b;
        lp.install_all_fetched = pref.install_all_fetched;
        lp.oracle_dram = pref.oracle_dram;
        lp.freeze_fills = pref.freeze_fills;
        lp.expand_batch = pref.expand_batch;
        lp.issue_ahead = pref.issue_ahead;
        lp.sync_hop = pref.sync_hop;
        lp.extent_run = pref.extent_run;
        lp.direct_install = pref.direct_install;
        lp.hide_score = pref.hide_score;
        lp.pq_nav = pref.pq_nav;
        lp.pq = pref.pq;
        lp.neighbor_k = pref.neighbor_k;
        DramWindow* tw = &win;
        PageCopyPool* pool = use_shared_pool ? &shared_pool : nullptr;
        Metrics local_m;
        if (per_thread_window) {
          tw = &thr_ctx[(size_t)t]->win;
          pool = use_shared_pool ? &shared_pool : &thr_ctx[(size_t)t]->pool;
          tls_metrics = &thr_ctx[(size_t)t]->m;
        } else {
          tls_metrics = &local_m;
          local_m.window_is_cxl_dram = metrics.window_is_cxl_dram;
        }
        const bool run_pipe2 = per_thread_window && pipe_depth >= 2 && lp.pq_nav && lp.pq &&
                               !oneshot_fp && !oracle_dram && !oracle_window &&
                               lp.policy == PrefetchPolicy::P3 && pool;
        if (run_pipe2) {
          const uint32_t L = beam ? beam : k;
          uint32_t Rlim = hdr->R;
          if (Rlim > 64) Rlim = 64;
          const int D = pipe_depth > 8 ? 8 : pipe_depth;
          PrefetchHub hub;
          hub.bind(pool, tw, &pl, &vio, tls_metrics);
          hub.pipe.extent_run = lp.extent_run;
          hub.pipe.direct_install = lp.direct_install;
          hub.pipe.score = lp.hide_score;
          std::vector<PqQ> slots((size_t)D);
          uint32_t next_qi = (uint32_t)t;
          auto finish_local = [&](PqQ& q) {
            auto ids = pqq_finish(q, k);
            auto tq1 = std::chrono::steady_clock::now();
            lat_ms[q.qi] = std::chrono::duration<double, std::milli>(tq1 - q.t0).count();
            if (!id_map.empty()) {
              for (uint32_t& id : ids) {
                if (id < id_map.size()) id = id_map[id];
              }
            }
            if (gt_path) {
              auto g = load_gt_row(gt_all.data(), gt_k, q.qi, k);
              recs[q.qi] = recall_at_k(ids, g);
            }
            if (eval_trace) {
              const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      tq1 - q.t0)
                                      .count();
              eval_trace->record(q.qi, qidx[q.qi], ns, std::move(q.trace_candidates),
                                 std::move(ids));
            }
            if (q.nand_held) {
              nand_inflight.fetch_sub(1, std::memory_order_relaxed);
              q.nand_held = false;
            }
            q = PqQ{};
            q.st = CbSt::Empty;
          };
          auto try_issue = [&](PqQ& q) -> bool {
            if (!issue_qd_ok(nand_inflight.load(std::memory_order_relaxed), issue_qd))
              return false;
            nand_inflight.fetch_add(1, std::memory_order_relaxed);
            q.nand_held = true;
            pqq_issue(q, pl, hub, /*stall=*/false);
            if (q.need.empty()) {
              nand_inflight.fetch_sub(1, std::memory_order_relaxed);
              q.nand_held = false;
              return true;
            }
            if (q.fill_toks.empty()) {
              nand_inflight.fetch_sub(1, std::memory_order_relaxed);
              q.nand_held = false;
              q.st = CbSt::Hold;
              return false;
            }
            return true;
          };
          if (steal_sched) {
            if (t == 0) {
              printf("steal_sched=1 depth=%d threads=%d shared_pool=1 issue_qd=%u "
                     "admit_serial=1\n",
                     D, nthreads, issue_qd);
              fflush(stdout);
            }
            for (;;) {
              hub.pump();
              CbSt stbuf[8];
              bool cov[8] = {};
              for (int i = 0; i < D; ++i) {
                stbuf[i] = slots[(size_t)i].st;
                if (stbuf[i] == CbSt::Wait || stbuf[i] == CbSt::Ready)
                  cov[i] = pqq_covering(hub.pipe, slots[(size_t)i].need);
              }
              const bool has_more = next_q.load(std::memory_order_relaxed) < nq;
              const uint32_t inflight_now = nand_inflight.load(std::memory_order_relaxed);
              note_scheduler_inflight(tls_metrics, inflight_now);
              const bool qd_ok = issue_qd_ok(inflight_now, issue_qd);
              auto dec = steal_decide(stbuf, cov, D, has_more, qd_ok);
              if (dec.act == Pipe2Act::Done) break;
              PqQ& q = slots[(size_t)dec.slot];
              if (dec.act == Pipe2Act::Pump) {
                std::this_thread::yield();
                continue;
              }
              if (dec.act == Pipe2Act::Fill) {
                uint32_t qi = 0;
                {
                  std::lock_guard<std::mutex> g(admit_mu);
                  qi = next_q.fetch_add(1, std::memory_order_relaxed);
                  if (qi >= nq) continue;
                  if (flush_window) tw->flush();
                  const float* qf = qbuf.data() + (size_t)qi * qdim;
                  EntryGraph qeg = eg;
                  if (nav.loaded()) qeg.entry_id = nav.search_entry(qf, nav_l);
                  pqq_init(q, pl, *tw, lp, qeg, qf, qi, L);
                }
                pqq_beam(q, pl, *lp.pq, L, iters, Rlim, &hub);
                if (!try_issue(q)) q.st = CbSt::Hold;
                continue;
              }
              if (dec.act == Pipe2Act::Issue) {
                if (!try_issue(q)) q.st = CbSt::Hold;
                continue;
              }
              if (!pqq_covering(hub.pipe, q.need)) continue;
              if (!pqq_rank(q, pl, hub.pipe)) {
                q.st = CbSt::Wait;
                continue;
              }
              finish_local(q);
            }
          } else {
            if (t == 0) {
              printf("pipe2_sched=1 depth=%d threads=%d per_thread_hub=1 stagger_us=%u\n", D,
                     nthreads, stagger_us);
              fflush(stdout);
            }
            {
              const uint64_t sus = pipe2_stagger_us(t, stagger_us);
              if (sus) std::this_thread::sleep_for(std::chrono::microseconds(sus));
            }
            for (;;) {
              hub.pump();
              CbSt stbuf[8];
              bool cov[8] = {};
              for (int i = 0; i < D; ++i) {
                stbuf[i] = slots[(size_t)i].st;
                if (stbuf[i] == CbSt::Wait || stbuf[i] == CbSt::Ready)
                  cov[i] = pqq_covering(hub.pipe, slots[(size_t)i].need);
              }
              auto dec = pipe2_decide(stbuf, cov, D, next_qi < nq);
              if (dec.act == Pipe2Act::Done) break;
              PqQ& q = slots[(size_t)dec.slot];
              if (dec.act == Pipe2Act::Fill) {
                uint32_t qi = next_qi;
                next_qi += (uint32_t)nthreads;
                if (flush_window) tw->flush();
                const float* qf = qbuf.data() + (size_t)qi * qdim;
                EntryGraph qeg = eg;
                if (nav.loaded()) qeg.entry_id = nav.search_entry(qf, nav_l);
                pqq_init(q, pl, *tw, lp, qeg, qf, qi, L);
                pqq_beam(q, pl, *lp.pq, L, iters, Rlim, &hub);
                pqq_issue(q, pl, hub, /*stall=*/true);
                continue;
              }
              if (dec.act == Pipe2Act::Wait) {
                uint64_t wns = hub.wait_covering(q.need, &q.fill_toks);
                if (tls_metrics) {
                  tls_metrics->device_fill_ns += wns;
                  tls_metrics->crit_wait_ns += wns;
                }
                continue;
              }
              if (!pqq_covering(hub.pipe, q.need)) {
                uint64_t wns = hub.wait_covering(q.need, &q.fill_toks);
                if (tls_metrics) {
                  tls_metrics->device_fill_ns += wns;
                  tls_metrics->crit_wait_ns += wns;
                }
              }
              if (!pqq_rank(q, pl, hub.pipe)) {
                uint64_t tok = hub.submit_block(q.need);
                if (tok) q.fill_toks.push_back(tok);
                q.st = CbSt::Wait;
                continue;
              }
              finish_local(q);
            }
          }
        } else if (pref.freeze_fills && per_thread_window && !require_cxl_dram && !oracle_dram &&
            !oracle_window) {
          // Same shard as the discarded pass: this window only holds qi%T==t.
          for (uint32_t qi = (uint32_t)t; qi < nq; qi += (uint32_t)nthreads) {
            auto r = run_one_q(qi, lp, *tw, pool);
            lat_ms[qi] = r.first;
            recs[qi] = r.second;
          }
        } else {
          for (;;) {
            uint32_t qi = next_q.fetch_add(1, std::memory_order_relaxed);
            if (qi >= nq) break;
            auto r = run_one_q(qi, lp, *tw, pool);
            lat_ms[qi] = r.first;
            recs[qi] = r.second;
          }
        }
        tls_metrics = nullptr;
        if (!per_thread_window) {
          std::lock_guard<std::mutex> g(merge_mu);
          metrics.add_from(local_m);
        }
      });
    }
    for (auto& th : ths) th.join();
    for (uint32_t qi = 0; qi < nq; ++qi) {
      if (recs[qi] >= 0) {
        recall_sum += recs[qi];
        recall_n++;
      }
      metrics.queries++;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  if (nthreads > 1) {
    for (auto& c : thr_ctx) {
      metrics.add_from(c->m);
      if (pref.policy == PrefetchPolicy::P3) c->pool.stop_join();
      if (c->dram) munmap(c->dram, dram_bytes);
    }
  }
  if (use_shared_pool) shared_pool.stop_join();
  if (eval_trace && !eval_trace->finish(eval_trace_dir)) {
    fprintf(stderr, "failed to finish evaluation trace in %s\n", eval_trace_dir);
    return 2;
  }
  const uint64_t nvme_sect1 = nvme_read_sectors();
  metrics.nvme_read_bytes =
      nvme_sect1 >= nvme_sect0 ? (nvme_sect1 - nvme_sect0) * 512ull : 0;
  if (scored_fp) {
    fclose(scored_fp);
    scored_fp = nullptr;
  }
  double sec = std::chrono::duration<double>(t1 - t0).count();
  metrics.wall_ns = (uint64_t)(sec * 1e9);
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
  double promote_gbs = sec > 0 ? (double)metrics.promote_bytes / 1e9 / sec : 0;
  printf("wall_s=%.3f throughput_QPS=%.2f\n", sec, qps);
  printf("cxl_ssd_to_dram_promote_GBps=%.3f promote_bytes=%llu target=12.0\n", promote_gbs,
         (unsigned long long)metrics.promote_bytes);
  printf("latency_ms mean=%.3f p50=%.3f p90=%.3f p99=%.3f\n", mean_lat, p50, p90, p99);
  if (recall_n) printf("recall@%u=%.4f\n", k, recall);
  printf("cxl_dram_hit_pct=%.2f\n", hit_pct);
  {
    const size_t pb = win.page_bytes ? win.page_bytes : 4096;
    metrics.page_occ_n0 = metrics.page_occ_n50 = metrics.page_occ_n100 = 0;
    metrics.page_occ_pages = metrics.page_occ_used_slots = metrics.page_occ_slots = 0;
    for (uint64_t p : metrics.pf_issued) {
      uint32_t slots = 0, used = 0;
      pl.for_ids_contained_in_page(p, pb, [&](uint32_t id) {
        slots++;
        if (metrics.pf_ids_scored.count(id)) used++;
      });
      metrics.note_page_occ_slots(used, slots);
    }
  }
  metrics.print();
  {
    const double nvme_gbs = sec > 0 ? (double)metrics.nvme_read_bytes / 1e9 / sec : 0;
    const double peak = 1.560;  // single-disk rnd PREFETCH_BATCH; dual 256k-page peak is 1.744
    printf("nvme_real_GBps=%.3f occ_vs_rnd_peak=%.1f%% peak_GBps=%.3f page_use=%.2f "
           "slot_use=%.2f issue_use=%.2f\n",
           nvme_gbs, peak > 0 ? 100.0 * nvme_gbs / peak : 0, peak,
           metrics.prefetch_page_use_pct(), metrics.prefetch_slot_use_pct(),
           metrics.prefetch_issue_use_pct());
  }
  printf("CSV,%s,%zu,%zu,%u,%u,%u,%.2f,%.3f,%.3f,%.3f,%.3f,%.4f,%.2f,%llu,%llu,"
         "%.2f,%.2f,%llu,%.2f\n",
         policy_s.c_str(), budget, dram_bytes, nq, beam, iters, qps, mean_lat, p50, p90, p99,
         recall, hit_pct, (unsigned long long)metrics.dram_hits,
         (unsigned long long)metrics.ssd_misses, metrics.score_from_window_pct(),
         metrics.hide_precision_pct(), (unsigned long long)metrics.crit_wait_ns,
         metrics.overlap_ratio());

  if (g_expand_dump) {
    std::fclose(g_expand_dump);
    g_expand_dump = nullptr;
  }
  if (pref.policy == PrefetchPolicy::P2) pref.stop_async();
  munmap(dram, dram_bytes);
  munmap(img, img_len);
  return 0;
}
