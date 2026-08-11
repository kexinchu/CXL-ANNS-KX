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
#include "serving/promote_pipe.hpp"
#include "serving/page_copy_pool.hpp"
#include "serving/metrics.hpp"

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
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
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

// CXL-DRAM window via devdax (true Type-3 byte-addressable memory).
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
  for (size_t off = 0; off < bytes; off += 2 * 1024 * 1024) b[off] = 0;
  printf("mapped CXL-DRAM dax %s off=%lld len=%zu\n", dax_dev, (long long)offset, bytes);
  return p;
}

// Legacy fallback: anonymous pages bound to a NUMA node (may be local DRAM).
static void* map_dram_numa(size_t bytes, unsigned numa_node) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) die("mmap dram anon");
  unsigned long nodemask = 1UL << numa_node;
  if (mbind(p, bytes, MPOL_BIND, &nodemask, sizeof(nodemask) * 8,
            MPOL_MF_MOVE | MPOL_MF_STRICT) != 0)
    perror("mbind warn");
  auto* b = static_cast<volatile char*>(p);
  for (size_t off = 0; off < bytes; off += 2 * 1024 * 1024) b[off] = 0;
  printf("mapped DRAM window anon+mbind node=%u len=%zu (fallback; not DAX)\n", numa_node,
         bytes);
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
// P2 uses expand/score decoupling (P2v2): discover nbrs into pending, score when resident;
// lookahead sync-prefetches non-cur future hops (no second thread on DAX).
// lookahead prefetches future candidates only (excludes current expand).
static std::vector<uint32_t> search_one_fp(Placement& pl, DramWindow& win, Prefetch& pref,
                                           PromotePipe* pipe, const EntryGraph& eg,
                                           const float* qf, uint32_t beam, uint32_t k,
                                           uint32_t iters) {
  pref.on_query_begin(pl, win, eg.nodes, eg.entry_id);

  const size_t vb = (size_t)pl.hdr->dim * pl.hdr->vec_bytes;
  auto get_vec = [&](uint32_t id) {
    return win.lookup_or_promote(pl.ssd_base, pl.vec(id), vb);
  };
  auto get_nbr = [&](uint32_t id) {
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
    if (win.metrics) win.metrics->distance_comps++;
    insert_cand(id, d);
  };

  {
    seen.insert(eg.entry_id);
    score_id(eg.entry_id);
  }

  // ---- P3v2: persistent page pool + score∥install pipeline ----
  // Workers: SSD→host only. Search: score as pages land; DAX install overlapped with
  // the next hop's fetches. Install prefers pages of likely-next expands + high cover.
  if (pref.policy == PrefetchPolicy::P3) {
    const uint32_t W = pref.pipe_w ? pref.pipe_w : 4;
    const uint32_t install_top = pref.install_top ? pref.install_top : 8;
    uint32_t expands = 0;
    const size_t pb = win.page_bytes;

    PageCopyPool pool;
    pool.start(W);

    struct PendingInstall {
      uint64_t page_off = 0;
      std::vector<uint8_t> data;
    };
    std::deque<PendingInstall> pending_install;

    auto drain_installs = [&](size_t max_n) {
      size_t n = 0;
      while (n < max_n && !pending_install.empty()) {
        auto& pi = pending_install.front();
        win.install_full_page(pi.page_off, pi.data.data());
        pending_install.pop_front();
        ++n;
      }
      return n;
    };

    auto hop_parallel_score = [&](std::vector<uint32_t>& ids) {
      if (ids.empty()) {
        drain_installs(pending_install.size());
        return;
      }

      // Opportunistically drain prior installs before submitting this hop's fetches.
      if (!pending_install.empty()) drain_installs(std::min(pending_install.size(), (size_t)4));

      std::unordered_map<uint32_t, std::vector<uint64_t>> id_pages;
      id_pages.reserve(ids.size() * 2);
      std::vector<uint64_t> pages;
      pages.reserve(ids.size() * 2);
      std::unordered_set<uint64_t> page_set;
      page_set.reserve(ids.size() * 4);

      for (uint32_t id : ids) {
        if (win.is_resident(pl.ssd_base, pl.vec(id), vb)) continue;
        uint64_t off = (uint64_t)(pl.vec(id) - pl.ssd_base);
        uint64_t end = off + vb;
        uint64_t first = off & ~(uint64_t)(pb - 1);
        uint64_t last = (end - 1) & ~(uint64_t)(pb - 1);
        auto& ips = id_pages[id];
        for (uint64_t p = first; p <= last; p += pb) {
          ips.push_back(p);
          if (page_set.insert(p).second) pages.push_back(p);
        }
      }

      std::vector<uint64_t> fetch;
      fetch.reserve(pages.size());
      std::unordered_set<uint64_t> fetch_set;
      for (uint64_t p : pages) {
        if (pref.budget_left < pb) break;
        if (win.is_resident(pl.ssd_base, pl.ssd_base + p, 1)) continue;
        fetch.push_back(p);
        fetch_set.insert(p);
        pref.budget_left -= pb;
      }

      std::unordered_map<uint64_t, size_t> page_idx;
      std::vector<std::vector<uint8_t>> host;
      std::unique_ptr<std::atomic<uint8_t>[]> ready;
      std::vector<uint8_t> consumed;  // observed ready by search thread
      if (!fetch.empty()) {
        host.resize(fetch.size());
        ready.reset(new std::atomic<uint8_t>[fetch.size()]);
        consumed.assign(fetch.size(), 0);
        for (size_t i = 0; i < fetch.size(); ++i) {
          host[i].resize(pb);
          page_idx[fetch[i]] = i;
          ready[i].store(0, std::memory_order_relaxed);
          pool.submit(pl.ssd_base + fetch[i], host[i].data(), pb, &ready[i]);
        }
      }

      std::unordered_set<uint64_t> host_ready;
      host_ready.reserve(fetch.size() * 2);
      std::vector<Cand> scored_local;
      scored_local.reserve(ids.size());
      std::unordered_set<uint32_t> done;
      done.reserve(ids.size() * 2);

      auto assemble_from_host = [&](uint32_t id, uint8_t* buf) -> bool {
        auto it = id_pages.find(id);
        if (it == id_pages.end()) return false;
        for (uint64_t p : it->second) {
          if (!fetch_set.count(p) && !win.is_resident(pl.ssd_base, pl.ssd_base + p, 1))
            return false;
          if (fetch_set.count(p) && !host_ready.count(p)) return false;
        }
        uint64_t off = (uint64_t)(pl.vec(id) - pl.ssd_base);
        size_t copied = 0;
        while (copied < vb) {
          uint64_t cur = off + copied;
          uint64_t po = cur & ~(uint64_t)(pb - 1);
          size_t in_page = (size_t)(cur - po);
          size_t n = std::min(vb - copied, pb - in_page);
          auto pit = page_idx.find(po);
          if (pit != page_idx.end() && host_ready.count(po)) {
            std::memcpy(buf + copied, host[pit->second].data() + in_page, n);
          } else {
            win.copy_through(pl.ssd_base, pl.ssd_base + cur, n, buf + copied);
          }
          copied += n;
        }
        return true;
      };

      auto try_score_ready = [&]() {
        for (uint32_t id : ids) {
          if (done.count(id)) continue;
          alignas(64) uint8_t buf[4096];
          if (vb > sizeof(buf)) {
            fprintf(stderr, "vec too large\n");
            std::exit(2);
          }
          if (!assemble_from_host(id, buf)) continue;
          float d = vec_mips_neg(buf, qf, pl.hdr->dim, pl.hdr->vec_bytes);
          if (win.metrics) win.metrics->distance_comps++;
          insert_cand(id, d);
          scored_local.push_back({d, id});
          done.insert(id);
        }
      };

      // Score ids already fully in the window (no fetch needed).
      for (uint32_t id : ids) {
        if (id_pages.count(id)) continue;
        score_id(id);
        done.insert(id);
        for (const auto& c : cand) {
          if (c.id == id) {
            scored_local.push_back(c);
            break;
          }
        }
      }

      size_t ready_n = 0;
      while (ready_n < fetch.size()) {
        bool progress = false;
        for (size_t i = 0; i < fetch.size(); ++i) {
          if (consumed[i]) continue;
          if (!ready[i].load(std::memory_order_acquire)) continue;
          consumed[i] = 1;
          host_ready.insert(fetch[i]);
          ++ready_n;
          progress = true;
        }
        if (progress) {
          try_score_ready();
        } else if (!pending_install.empty()) {
          // Overlap prior-hop DAX install with in-flight SSD→host copies.
          drain_installs(1);
        } else {
          std::this_thread::yield();
        }
      }
      pool.wait_idle();
      try_score_ready();

      for (uint32_t id : ids) {
        if (done.count(id)) continue;
        score_id(id);
        for (const auto& c : cand) {
          if (c.id == id) {
            scored_local.push_back(c);
            break;
          }
        }
      }

      // Smart install: prefer pages of likely-next expands among this hop's host pages.
      std::unordered_map<uint32_t, float> id_weight;
      id_weight.reserve(ids.size() * 2);
      for (const auto& c : scored_local) id_weight[c.id] = 1.f / (1.f + std::max(c.dist, 0.f));

      std::vector<Cand> unexp;
      unexp.reserve(cand.size());
      for (const auto& c : cand) {
        if (expanded.count(c.id)) continue;
        unexp.push_back(c);
      }
      std::sort(unexp.begin(), unexp.end(),
                [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
      const uint32_t next_n =
          std::min<uint32_t>(install_top, (uint32_t)unexp.size());
      for (uint32_t i = 0; i < next_n; ++i) {
        // Only boost if this hop actually holds the id's pages in host.
        if (!id_pages.count(unexp[i].id)) continue;
        id_weight[unexp[i].id] += 1000.f - (float)i;
      }

      struct PageUtil {
        uint64_t page;
        float util;
        size_t idx;
      };
      std::unordered_map<uint64_t, float> page_util;
      page_util.reserve(page_idx.size() * 2);
      for (uint32_t id : ids) {
        auto it = id_pages.find(id);
        if (it == id_pages.end()) continue;
        float w = 1.f;
        auto wit = id_weight.find(id);
        if (wit != id_weight.end()) w = wit->second;
        for (uint64_t p : it->second) {
          if (!page_idx.count(p)) continue;
          page_util[p] += w;
        }
      }

      std::vector<PageUtil> ranked;
      ranked.reserve(page_util.size());
      for (const auto& kv : page_util) {
        auto pit = page_idx.find(kv.first);
        if (pit == page_idx.end()) continue;
        if (host[pit->second].empty()) continue;
        ranked.push_back({kv.first, kv.second, pit->second});
      }
      std::sort(ranked.begin(), ranked.end(), [](const PageUtil& a, const PageUtil& b) {
        if (a.util != b.util) return a.util > b.util;
        return a.page < b.page;
      });

      // Page cap ≈ old top-T id install volume; utility only reorders which pages.
      const size_t max_pages = (size_t)install_top;
      size_t queued_n = 0;
      for (const auto& pu : ranked) {
        if (queued_n >= max_pages) break;
        if (host[pu.idx].empty()) continue;
        PendingInstall pi;
        pi.page_off = pu.page;
        pi.data = std::move(host[pu.idx]);
        pending_install.push_back(std::move(pi));
        ++queued_n;
      }
    };

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
      uint32_t R = pl.hdr->R;
      if (R > 64) R = 64;
      alignas(64) uint8_t nbuf[64 * 4];
      win.copy_through(pl.ssd_base, reinterpret_cast<const uint8_t*>(pl.nbrs(cur)),
                       (size_t)R * 4, nbuf);
      std::memcpy(nbrs_local, nbuf, (size_t)R * 4);

      std::vector<uint32_t> to_score;
      to_score.reserve(R);
      for (uint32_t j = 0; j < R; ++j) {
        uint32_t nb = nbrs_local[j];
        if (nb >= pl.hdr->n || seen.count(nb)) continue;
        seen.insert(nb);
        to_score.push_back(nb);
      }
      hop_parallel_score(to_score);
    }

    drain_installs(pending_install.size());
    pool.stop_join();

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
                                        uint32_t iters, bool rerank, bool oneshot_fp) {
  if (oneshot_fp) return search_one_fp(pl, win, pref, pipe, eg, qf, beam, k, iters);

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
  const char* dax_dev = getenv("CXAN_DAX_DEV") ? getenv("CXAN_DAX_DEV") : "/dev/dax0.0";
  off_t vmem_off = -1;
  off_t dax_off = getenv("CXAN_DAX_OFFSET") ? (off_t)strtoull(getenv("CXAN_DAX_OFFSET"), nullptr, 10)
                                            : 0;
  size_t vmem_len = 0;
  std::string policy_s = "P0";
  std::string dram_backend = getenv("CXAN_DRAM_BACKEND") ? getenv("CXAN_DRAM_BACKEND") : "dax";
  // dax | numa
  size_t budget = 256 * 1024;
  size_t dram_bytes = getenv("CXAN_DRAM_BYTES")
                          ? strtoull(getenv("CXAN_DRAM_BYTES"), nullptr, 10)
                          : (1ull << 30);
  uint32_t pipe_w = 4;
  uint32_t install_top = 4;
  uint32_t beam = 32, k = 10, iters = 64;
  unsigned dram_numa = getenv("CXAN_DRAM_NODE") ? (unsigned)atoi(getenv("CXAN_DRAM_NODE")) : 1;
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
    else if (a == "--dax-dev") dax_dev = need(a.c_str());
    else if (a == "--dax-offset") dax_off = (off_t)strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--dram-backend") dram_backend = need(a.c_str());
    else if (a == "--dram-numa") dram_numa = (unsigned)atoi(need(a.c_str()));
    else if (a == "--policy") policy_s = need(a.c_str());
    else if (a == "--budget") budget = strtoull(need(a.c_str()), nullptr, 10);
    else if (a == "--pipe-w") pipe_w = (uint32_t)atoi(need(a.c_str()));
    else if (a == "--install-top") install_top = (uint32_t)atoi(need(a.c_str()));
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
    fprintf(stderr,
            "need (--image FILE | --vmem-dev DEV --vmem-offset OFF --vmem-len LEN) "
            "--entry --queries [--gt]\n"
            "dram window: --dram-backend dax|numa [--dax-dev /dev/dax0.0] [--dax-offset N]\n"
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
  void* dram = nullptr;
  if (dram_backend == "dax") {
    dram = map_dram_dax(dax_dev, dax_off, dram_bytes);
  } else {
    dram = map_dram_numa(dram_bytes, dram_numa);
  }
  DramWindow win;
  win.init(dram, dram_bytes, &metrics);

  Prefetch pref;
  pref.policy = parse_policy(policy_s);
  pref.budget_per_query = budget;
  pref.pin_entry = pin_entry;
  pref.pipe_w = pipe_w ? pipe_w : 4;
  pref.install_top = install_top ? install_top : 8;
  // P2v2 is cooperative single-threaded (DAX is not safe for concurrent promote).
  if (false && pref.policy == PrefetchPolicy::P2) pref.start_async(&pl, &win);

  if (pref.policy == PrefetchPolicy::P3) {
    printf("P3v2 smart-install pool W=%u budget=%zu install_top=%u\n",
           pref.pipe_w, budget, pref.install_top);
  }

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
    auto ids = search_one(pl, win, pref, nullptr, eg, qf, qpq.data(), beam, k, iters, rerank,
                          oneshot_fp);
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
