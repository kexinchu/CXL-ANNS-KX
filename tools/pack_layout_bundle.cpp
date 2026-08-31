// Expand-bundle page pack: put neighbors of the same expand on the same pages.
// Does NOT add redundant scoring. Permute only.
//
//   g++ -O3 -std=c++17 -pthread tools/pack_layout_bundle.cpp -o tools/pack_layout_bundle
//
// Metrics on an existing map:
//   ./tools/pack_layout_bundle --in-file layout.bin --eval-map new_to_old.bin
//
// New permute, metrics only:
//   ./tools/pack_layout_bundle --in-file layout.bin --metrics-only
//
// Full write (host and/or vmem):
//   ./tools/pack_layout_bundle --in-file layout.bin --out-file out.bin \
//       --out-entry e.bin --out-map m.bin --out-json j.json --out-graph g.bin

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#pragma pack(push, 1)
struct LayoutHeader {
  uint64_t magic = 0x314e415843ull;
  uint32_t version = 1;
  uint32_t n = 0, dim = 0, R = 0, pq_bytes = 0, vec_bytes = 0;
  uint32_t entry_id = 0, entry_nodes = 0, pad = 0;
  uint64_t off_pq = 0, len_pq = 0;
  uint64_t off_graph = 0, len_graph = 0;
  uint64_t off_vectors = 0, len_vectors = 0;
  uint64_t off_pivots = 0, len_pivots = 0;
  uint64_t checksum = 0;
};
#pragma pack(pop)

static uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) / a * a; }
static void die(const std::string& s) {
  fprintf(stderr, "%s\n", s.c_str());
  std::exit(2);
}

static void* map_rw(const char* path, size_t len, int* fd_out, bool create) {
  int fd = create ? ::open(path, O_RDWR | O_CREAT, 0644) : ::open(path, O_RDONLY);
  if (fd < 0) die(std::string("open ") + path);
  if (create) {
    if (::ftruncate(fd, (off_t)len) != 0) die("ftruncate");
  }
  int prot = create ? (PROT_READ | PROT_WRITE) : PROT_READ;
  void* p = mmap(nullptr, len, prot, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) die(std::string("mmap ") + path);
  if (fd_out) *fd_out = fd;
  return p;
}

static void* map_vmem(const char* dev, off_t off, size_t len, int prot) {
  int fd = ::open(dev, (prot & PROT_WRITE) ? O_RDWR : O_RDONLY);
  if (fd < 0) die("open vmem");
  void* p = mmap(nullptr, len, prot, MAP_SHARED, fd, off);
  if (p == MAP_FAILED) die("mmap vmem");
  ::close(fd);
  return p;
}

int main(int argc, char** argv) {
  setvbuf(stderr, nullptr, _IONBF, 0);
  std::string in_file, in_dev, out_file, out_dev, out_json, out_entry, out_map, out_graph,
      eval_map, from_trace, sim_trace;
  off_t in_off = 0, out_off = -1;
  uint64_t in_len = 0;
  uint64_t page = 4096;
  int nthreads = 8;
  bool metrics_only = false;
  uint32_t hot_n = 200000;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&]() -> std::string {
      if (i + 1 >= argc) die("missing arg");
      return argv[++i];
    };
    if (a == "--in-file") in_file = need();
    else if (a == "--in-vmem-dev") in_dev = need();
    else if (a == "--in-vmem-offset") in_off = (off_t)std::stoll(need());
    else if (a == "--in-vmem-len") in_len = std::stoull(need());
    else if (a == "--out-file") out_file = need();
    else if (a == "--out-vmem-dev") out_dev = need();
    else if (a == "--out-vmem-offset") out_off = (off_t)std::stoll(need());
    else if (a == "--out-json") out_json = need();
    else if (a == "--out-entry") out_entry = need();
    else if (a == "--out-map") out_map = need();
    else if (a == "--out-graph") out_graph = need();
    else if (a == "--eval-map") eval_map = need();
    else if (a == "--from-trace") from_trace = need();
    else if (a == "--sim-trace") sim_trace = need();
    else if (a == "--threads") nthreads = std::stoi(need());
    else if (a == "--page") page = std::stoull(need());
    else if (a == "--hot-n") hot_n = (uint32_t)std::stoul(need());
    else if (a == "--metrics-only") metrics_only = true;
    else die("unknown " + a);
  }
  if (in_file.empty() && (in_dev.empty() || in_len == 0)) die("need --in-file or --in-vmem-*");

  const char* in = nullptr;
  size_t in_map_len = 0;
  int in_fd = -1;
  if (!in_file.empty()) {
    struct stat st {};
    if (stat(in_file.c_str(), &st) != 0) die("stat in-file");
    in_map_len = (size_t)st.st_size;
    in = (const char*)map_rw(in_file.c_str(), in_map_len, &in_fd, false);
  } else {
    in_map_len = in_len;
    in = (const char*)map_vmem(in_dev.c_str(), in_off, in_len, PROT_READ);
  }
  auto* ih = (const LayoutHeader*)in;
  if (ih->magic != 0x314e415843ull) die("bad magic");
  uint32_t n = ih->n, dim = ih->dim, R = ih->R;
  size_t packed = (size_t)dim * ih->vec_bytes;
  size_t in_stride = packed;
  if (ih->n && ih->len_vectors) {
    size_t st = (size_t)(ih->len_vectors / ih->n);
    if (st >= packed) in_stride = st;
  }
  uint32_t slots = packed ? (uint32_t)(page / packed) : 1;
  if (slots < 1) slots = 1;
  fprintf(stderr, "in n=%u dim=%u R=%u packed=%zu stride=%zu slots/page=%u hot_n=%u\n", n, dim,
          R, packed, in_stride, slots, hot_n);

  const uint32_t* graph = reinterpret_cast<const uint32_t*>(in + ih->off_graph);
  uint32_t entry_old = ih->entry_id;
  if (entry_old >= n) entry_old = 0;

  std::vector<uint32_t> new_to_old(n), old_to_new(n, UINT32_MAX);
  uint32_t assigned = 0;
  auto assign_one = [&](uint32_t u) {
    if (old_to_new[u] != UINT32_MAX) return false;
    old_to_new[u] = assigned;
    new_to_old[assigned] = u;
    assigned++;
    return true;
  };

  std::vector<uint16_t> depth(n, 0xFFFF);
  std::vector<uint32_t> bfs_ord;
  bfs_ord.reserve(n);
  {
    std::queue<uint32_t> q;
    depth[entry_old] = 0;
    q.push(entry_old);
    while (!q.empty()) {
      uint32_t u = q.front();
      q.pop();
      bfs_ord.push_back(u);
      uint16_t d = depth[u];
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t v = graph[(size_t)u * R + k];
        if (v >= n || depth[v] != 0xFFFF) continue;
        depth[v] = (uint16_t)(d + 1);
        q.push(v);
      }
    }
    fprintf(stderr, "bfs reachable=%zu entry_old=%u\n", bfs_ord.size(), entry_old);
  }

  auto load_trace = [&](const std::string& path) {
    std::ifstream tf(path, std::ios::binary);
    if (!tf) die("open trace " + path);
    uint32_t mag = 0, tq = 0;
    tf.read((char*)&mag, 4);
    tf.read((char*)&tq, 4);
    if (mag != 0x44524353u) die("bad trace magic");
    std::vector<std::vector<uint32_t>> qs(tq);
    for (uint32_t qi = 0; qi < tq; ++qi) {
      uint32_t ns = 0;
      tf.read((char*)&ns, 4);
      qs[qi].resize(ns);
      if (ns) tf.read((char*)qs[qi].data(), (size_t)ns * 4);
    }
    return qs;
  };

  if (!eval_map.empty()) {
    std::ifstream mf(eval_map, std::ios::binary);
    if (!mf) die("open eval-map");
    mf.read((char*)new_to_old.data(), (size_t)n * 4);
    if ((size_t)mf.gcount() != (size_t)n * 4) die("eval-map size");
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t o = new_to_old[i];
      if (o >= n) die("eval-map id");
      old_to_new[o] = i;
    }
    assigned = n;
    fprintf(stderr, "loaded eval-map %s\n", eval_map.c_str());
  } else if (!from_trace.empty()) {
    auto qs = load_trace(from_trace);
    uint64_t touch = 0;
    for (auto& q : qs) {
      for (uint32_t id : q) {
        if (id < n && assign_one(id)) touch++;
      }
    }
    for (uint32_t u = 0; u < n; ++u) assign_one(u);
    fprintf(stderr, "trace-pack queries=%zu first_touch=%llu then leftovers\n", qs.size(),
            (unsigned long long)touch);
  } else {
    // Invert edges so placing v decrements remaining-neighbor counts of parents.
    std::vector<uint32_t> indeg(n, 0);
    for (uint32_t u = 0; u < n; ++u) {
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t v = graph[(size_t)u * R + k];
        if (v < n) indeg[v]++;
      }
    }
    std::vector<uint64_t> inoff(n + 1, 0);
    for (uint32_t i = 0; i < n; ++i) inoff[i + 1] = inoff[i] + indeg[i];
    std::vector<uint32_t> inadj(inoff[n]);
    std::vector<uint64_t> cur = inoff;
    for (uint32_t u = 0; u < n; ++u) {
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t v = graph[(size_t)u * R + k];
        if (v < n) inadj[cur[v]++] = u;
      }
    }
    std::vector<uint8_t> rem(n, 0);
    for (uint32_t u = 0; u < n; ++u) {
      uint8_t r = 0;
      for (uint32_t k = 0; k < R; ++k)
        if (graph[(size_t)u * R + k] < n) r++;
      rem[u] = r;
    }
    std::vector<uint8_t> is_hot(n, 0);
    uint32_t nhot = hot_n < (uint32_t)bfs_ord.size() ? hot_n : (uint32_t)bfs_ord.size();
    for (uint32_t i = 0; i < nhot; ++i) is_hot[bfs_ord[i]] = 1;
    // rem is 0..R — bucket queue (heap of 320M stale keys was too slow).
    std::vector<uint32_t> buckets[2][33];
    for (int h = 0; h < 2; ++h)
      for (int r = 0; r <= 32; ++r) buckets[h][r].reserve((size_t)n / 32 + 8);
    for (uint32_t u = 0; u < n; ++u) buckets[is_hot[u]][rem[u]].push_back(u);
    int cur_h = 1, cur_r = 32;
    auto pop_best = [&]() -> uint32_t {
      for (;;) {
        while (cur_h >= 0) {
          while (cur_r >= 0 && buckets[cur_h][cur_r].empty()) cur_r--;
          if (cur_r >= 0) break;
          cur_h--;
          cur_r = 32;
        }
        if (cur_h < 0) return UINT32_MAX;
        uint32_t u = buckets[cur_h][cur_r].back();
        buckets[cur_h][cur_r].pop_back();
        if ((int)is_hot[u] == cur_h && (int)rem[u] == cur_r) return u;
      }
    };

    uint32_t nblocks = 0, sum_block = 0, max_block = 0;
    while (assigned < n) {
      uint32_t u = pop_best();
      if (u == UINT32_MAX) break;
      if (rem[u] == 0) continue;
      uint32_t block_ids[64];
      uint32_t nb = 0;
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t v = graph[(size_t)u * R + k];
        if (v >= n || old_to_new[v] != UINT32_MAX) continue;
        block_ids[nb++] = v;
      }
      if (!nb) {
        rem[u] = 0;
        continue;
      }
      for (uint32_t i = 0; i < nb; ++i) assign_one(block_ids[i]);
      nblocks++;
      sum_block += nb;
      if (nb > max_block) max_block = nb;
      for (uint32_t i = 0; i < nb; ++i) {
        uint32_t v = block_ids[i];
        for (uint64_t e = inoff[v]; e < inoff[v + 1]; ++e) {
          uint32_t w = inadj[e];
          if (!rem[w]) continue;
          rem[w]--;
          buckets[is_hot[w]][rem[w]].push_back(w);
          if ((int)is_hot[w] > cur_h || ((int)is_hot[w] == cur_h && (int)rem[w] > cur_r)) {
            cur_h = is_hot[w];
            cur_r = rem[w];
          }
        }
      }
      if ((nblocks & 0x3ffff) == 0)
        fprintf(stderr, "  bundle blocks=%u assigned=%u/%u last=%u\n", nblocks, assigned, n,
                nb);
    }
    for (uint32_t u = 0; u < n; ++u) assign_one(u);
    fprintf(stderr,
            "bundle permute done assigned=%u blocks=%u mean_block=%.2f max_block=%u\n",
            assigned, nblocks, nblocks ? sum_block / (double)nblocks : 0, max_block);
  }

  uint32_t entry_new = old_to_new[entry_old];
  fprintf(stderr, "entry_old=%u entry_new=%u\n", entry_old, entry_new);

  // --- metrics (old graph + new ids) ---
  auto page_of = [&](uint32_t nid) -> uint64_t {
    return ((uint64_t)nid * packed) / page;
  };
  auto contained_lo_hi = [&](uint64_t p) -> std::pair<uint32_t, uint32_t> {
    // IDs whose full packed vec lies in page p (relative to vectors-at-0;
    // off_vectors is page-aligned in the written image).
    uint64_t plo = p * page, phi = plo + page;
    if (packed > page) return {0, 0};
    uint32_t i0 = (uint32_t)((plo + packed - 1) / packed);
    uint32_t i1 = (uint32_t)(phi / packed);
    if (i1) i1--;
    if (i0 >= n) return {0, 0};
    if (i1 >= n) i1 = n - 1;
    if (i1 < i0) return {0, 0};
    return {i0, i1};
  };

  {
    uint64_t cnt = 0, d1 = 0, d2 = 0, d5 = 0, d64 = 0, share = 0;
    for (uint32_t i = 0; i < n; i += 8) {
      uint32_t old_u = new_to_old[i];
      uint64_t pu = page_of(i);
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t old_v = graph[(size_t)old_u * R + k];
        if (old_v >= n || old_v == old_u) continue;
        uint32_t nv = old_to_new[old_v];
        uint32_t dn = (nv > i) ? (nv - i) : (i - nv);
        cnt++;
        if (dn <= 1) d1++;
        if (dn <= 2) d2++;
        if (dn < slots) d5++;
        if (dn <= 64) d64++;
        if (page_of(nv) == pu) share++;
      }
    }
    auto pct = [&](uint64_t x) { return cnt ? 100.0 * x / cnt : 0.0; };
    fprintf(stderr,
            "NEW |d|<=1 %.3f%% <=2 %.3f%% <slots %.3f%% <=64 %.3f%% page-share %.3f%% "
            "(cnt=%llu)\n",
            pct(d1), pct(d2), pct(d5), pct(d64), pct(share), (unsigned long long)cnt);
  }

  {
    uint64_t N = 0, pages_sum = 0, slots_sum = 0, used_sum = 0;
    uint32_t lim = hot_n < (uint32_t)bfs_ord.size() ? hot_n : (uint32_t)bfs_ord.size();
    std::vector<uint64_t> tmp;
    tmp.reserve(64);
    for (uint32_t i = 0; i < lim; ++i) {
      uint32_t old_u = bfs_ord[i];
      tmp.clear();
      uint32_t nbr_new[64];
      uint32_t nn = 0;
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t old_v = graph[(size_t)old_u * R + k];
        if (old_v >= n || old_v == old_u) continue;
        uint32_t nv = old_to_new[old_v];
        nbr_new[nn++] = nv;
        uint64_t off = (uint64_t)nv * packed;
        uint64_t p0 = off / page, p1 = (off + packed - 1) / page;
        for (uint64_t p = p0; p <= p1; ++p) tmp.push_back(p);
      }
      std::sort(tmp.begin(), tmp.end());
      tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
      pages_sum += tmp.size();
      uint32_t contained = 0, used = 0;
      for (uint64_t p : tmp) {
        auto [a, b] = contained_lo_hi(p);
        for (uint32_t id = a; id <= b && id < n; ++id) {
          uint64_t vo = (uint64_t)id * packed;
          if (vo < p * page || vo + packed > p * page + page) continue;
          contained++;
          for (uint32_t t = 0; t < nn; ++t)
            if (nbr_new[t] == id) {
              used++;
              break;
            }
        }
      }
      slots_sum += contained;
      used_sum += used;
      N++;
    }
    fprintf(stderr,
            "hot%u mean_pages/expand=%.2f expand_slot_use=%.2f%% "
            "(used=%llu contained=%llu N=%llu) ideal_pages=%.2f\n",
            lim, N ? pages_sum / (double)N : 0,
            slots_sum ? 100.0 * used_sum / slots_sum : 0, (unsigned long long)used_sum,
            (unsigned long long)slots_sum, (unsigned long long)N,
            R / (double)std::max(1u, slots));
  }

  if (!sim_trace.empty()) {
    auto qs = load_trace(sim_trace);
    uint64_t used_sum = 0, slots_sum = 0, pages_sum = 0;
    std::vector<uint64_t> tmp;
    for (auto& q : qs) {
      tmp.clear();
      for (uint32_t old_id : q) {
        if (old_id >= n) continue;
        uint32_t nv = old_to_new[old_id];
        if (nv == UINT32_MAX) continue;
        uint64_t off = (uint64_t)nv * packed;
        uint64_t p0 = off / page, p1 = (off + packed - 1) / page;
        for (uint64_t p = p0; p <= p1; ++p) tmp.push_back(p);
      }
      std::sort(tmp.begin(), tmp.end());
      tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
      pages_sum += tmp.size();
      std::vector<uint32_t> scored_new;
      scored_new.reserve(q.size());
      for (uint32_t old_id : q)
        if (old_id < n && old_to_new[old_id] != UINT32_MAX)
          scored_new.push_back(old_to_new[old_id]);
      std::sort(scored_new.begin(), scored_new.end());
      scored_new.erase(std::unique(scored_new.begin(), scored_new.end()), scored_new.end());
      for (uint64_t p : tmp) {
        auto [a, b] = contained_lo_hi(p);
        for (uint32_t id = a; id <= b && id < n; ++id) {
          uint64_t vo = (uint64_t)id * packed;
          if (vo < p * page || vo + packed > p * page + page) continue;
          slots_sum++;
          if (std::binary_search(scored_new.begin(), scored_new.end(), id)) used_sum++;
        }
      }
    }
    fprintf(stderr,
            "sim-trace nq=%zu mean_pages/q=%.1f query_slot_use=%.2f%% "
            "(used=%llu contained=%llu)\n",
            qs.size(), qs.empty() ? 0 : pages_sum / (double)qs.size(),
            slots_sum ? 100.0 * used_sum / slots_sum : 0, (unsigned long long)used_sum,
            (unsigned long long)slots_sum);
  }

  if (metrics_only || assigned != n) {
    if (!in_file.empty()) {
      munmap((void*)in, in_map_len);
      ::close(in_fd);
    } else {
      munmap((void*)in, in_map_len);
    }
    fprintf(stderr, "metrics-only, no write\n");
    return 0;
  }
  if (out_file.empty() && (out_dev.empty() || out_off < 0))
    die("need --out-file or --out-vmem-* (or --metrics-only)");
  if (out_json.empty() || out_entry.empty() || out_map.empty())
    die("need --out-json --out-entry --out-map");

  LayoutHeader hdr = *ih;
  hdr.vec_bytes = ih->vec_bytes;
  hdr.entry_id = entry_new;
  hdr.pad = slots;
  uint64_t cursor = align_up(sizeof(LayoutHeader), page);
  hdr.off_pq = cursor;
  hdr.len_pq = (uint64_t)n * ih->pq_bytes;
  cursor = align_up(cursor + hdr.len_pq, page);
  hdr.off_graph = cursor;
  hdr.len_graph = (uint64_t)n * R * 4;
  cursor = align_up(cursor + hdr.len_graph, page);
  hdr.off_vectors = cursor;
  hdr.len_vectors = (uint64_t)n * packed;
  cursor = align_up(cursor + hdr.len_vectors, page);
  hdr.off_pivots = cursor;
  hdr.len_pivots = ih->len_pivots ? ih->len_pivots : 64 * 1024;
  cursor = align_up(cursor + hdr.len_pivots, page);
  hdr.checksum = hdr.n ^ hdr.dim ^ hdr.R ^ hdr.len_vectors;
  fprintf(stderr, "out packed layout_bytes=%llu (%.2f GiB)\n", (unsigned long long)cursor,
          cursor / (1024.0 * 1024 * 1024));

  char* out = nullptr;
  int out_fd = -1;
  if (!out_file.empty()) {
    out = (char*)map_rw(out_file.c_str(), cursor, &out_fd, true);
  } else {
    out = (char*)map_vmem(out_dev.c_str(), out_off, cursor, PROT_READ | PROT_WRITE);
  }
  madvise(out, cursor, MADV_SEQUENTIAL);
  std::memcpy(out, &hdr, sizeof(hdr));

  {
    const char* sp = in + ih->off_pq;
    char* dp = out + hdr.off_pq;
    uint32_t pb = ih->pq_bytes;
    for (uint32_t ni = 0; ni < n; ++ni) {
      uint32_t ou = new_to_old[ni];
      std::memcpy(dp + (size_t)ni * pb, sp + (size_t)ou * pb, pb);
    }
    fprintf(stderr, "pq remapped\n");
  }
  {
    uint32_t* dg = reinterpret_cast<uint32_t*>(out + hdr.off_graph);
    for (uint32_t ni = 0; ni < n; ++ni) {
      uint32_t ou = new_to_old[ni];
      const uint32_t* src = graph + (size_t)ou * R;
      uint32_t* dst = dg + (size_t)ni * R;
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t ov = src[k];
        dst[k] = (ov < n) ? old_to_new[ov] : 0;
      }
    }
    fprintf(stderr, "graph remapped\n");
  }
  {
    const char* sv = in + ih->off_vectors;
    char* dv = out + hdr.off_vectors;
    std::atomic<uint32_t> next{0};
    auto worker = [&]() {
      constexpr uint32_t kChunk = 4096;
      for (;;) {
        uint32_t begin = next.fetch_add(kChunk, std::memory_order_relaxed);
        if (begin >= n) break;
        uint32_t end = begin + kChunk;
        if (end > n) end = n;
        for (uint32_t ni = begin; ni < end; ++ni) {
          uint32_t ou = new_to_old[ni];
          std::memcpy(dv + (uint64_t)ni * packed, sv + (uint64_t)ou * in_stride, packed);
        }
      }
    };
    std::vector<std::thread> th;
    for (int t = 0; t < nthreads; ++t) th.emplace_back(worker);
    for (auto& t : th) t.join();
    fprintf(stderr, "vectors remapped packed\n");
  }
  if (ih->len_pivots)
    std::memcpy(out + hdr.off_pivots, in + ih->off_pivots, (size_t)hdr.len_pivots);

  uint32_t entry_nodes = ih->entry_nodes ? ih->entry_nodes : 8192;
  if (entry_nodes > n) entry_nodes = n;
  std::vector<uint32_t> entry_list;
  entry_list.reserve(entry_nodes);
  {
    std::vector<uint8_t> eseen((n + 7) / 8, 0);
    auto emark = [&](uint32_t x) { eseen[x >> 3] |= (uint8_t)(1u << (x & 7)); };
    auto ehas = [&](uint32_t x) { return (eseen[x >> 3] >> (x & 7)) & 1; };
    std::queue<uint32_t> q;
    q.push(entry_new);
    emark(entry_new);
    const uint32_t* ng = reinterpret_cast<const uint32_t*>(out + hdr.off_graph);
    while (!q.empty() && entry_list.size() < entry_nodes) {
      uint32_t u = q.front();
      q.pop();
      entry_list.push_back(u);
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t v = ng[(size_t)u * R + k];
        if (v >= n || ehas(v)) continue;
        emark(v);
        q.push(v);
      }
    }
    hdr.entry_nodes = (uint32_t)entry_list.size();
    std::memcpy(out, &hdr, sizeof(hdr));
  }

  {
    std::ofstream o(out_entry, std::ios::binary);
    uint32_t en = (uint32_t)entry_list.size();
    uint32_t eid = hdr.entry_id;
    o.write((char*)&en, 4);
    o.write((char*)&R, 4);
    o.write((char*)&eid, 4);
    o.write((char*)entry_list.data(), en * 4);
    const uint32_t* ng = reinterpret_cast<const uint32_t*>(out + hdr.off_graph);
    for (uint32_t id : entry_list) o.write((char*)(ng + (size_t)id * R), R * 4);
  }
  {
    std::ofstream o(out_map, std::ios::binary);
    o.write((char*)new_to_old.data(), (size_t)n * 4);
  }
  if (!out_graph.empty()) {
    std::ofstream o(out_graph, std::ios::binary);
    o.write(out + hdr.off_graph, (size_t)hdr.len_graph);
  }
  {
    std::ofstream js(out_json);
    js << "{\n"
       << "  \"n\": " << n << ",\n"
       << "  \"dim\": " << dim << ",\n"
       << "  \"R\": " << R << ",\n"
       << "  \"vec_bytes\": " << hdr.vec_bytes << ",\n"
       << "  \"packed\": " << packed << ",\n"
       << "  \"page_slots\": " << slots << ",\n"
       << "  \"entry_id_old\": " << entry_old << ",\n"
       << "  \"entry_id\": " << hdr.entry_id << ",\n"
       << "  \"entry_nodes\": " << hdr.entry_nodes << ",\n"
       << "  \"image_bytes\": " << cursor << ",\n"
       << "  \"off_vectors\": " << hdr.off_vectors << ",\n"
       << "  \"len_vectors\": " << hdr.len_vectors << ",\n"
       << "  \"vmem_offset\": " << (long long)(out_off >= 0 ? out_off : 0) << ",\n"
       << "  \"reorder\": \"expand_bundle_remaining_hot\",\n"
       << "  \"note\": \"neighbors of one expand packed into consecutive pages; no extra "
          "score\"\n"
       << "}\n";
  }

  fprintf(stderr, "msync...\n");
  msync(out, cursor, MS_SYNC);
  munmap(out, cursor);
  if (out_fd >= 0) ::close(out_fd);
  if (!in_file.empty()) {
    munmap((void*)in, in_map_len);
    ::close(in_fd);
  } else {
    munmap((void*)in, in_map_len);
  }
  fprintf(stderr, "DONE bundle packed bytes=%llu\n", (unsigned long long)cursor);
  return 0;
}
