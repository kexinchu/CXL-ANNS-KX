// Greedy neighbor pairing for DiskANN STRIDE=2048 (2 items / 4K page).
// Keeps logical IDs; physically places each pair on one page.
// Writes remapped image + id_to_slot.bin (uint32[n], slot of each id).
#include "serving/placement.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <queue>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

int main(int argc, char** argv) {
  const char* src = nullptr;
  const char* out_img = nullptr;
  const char* out_map = nullptr;
  const char* mode = "neighbor";
  const char* trace_path = nullptr;
  const char* map_in = nullptr;
  bool map_only = false;
  bool trace_pairs = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) std::exit(2);
      return argv[++i];
    };
    if (a == "--src") src = need();
    else if (a == "--out") out_img = need();
    else if (a == "--map") out_map = need();
    else if (a == "--mode") mode = need();
    else if (a == "--trace") trace_path = need();
    else if (a == "--map-in") map_in = need();
    else if (a == "--map-only") map_only = true;
    else if (a == "--trace-pairs") trace_pairs = true;
    else {
      std::fprintf(stderr, "unknown %s\n", a.c_str());
      return 2;
    }
  }
  if (!src || !out_img || !out_map) {
    std::fprintf(stderr,
                 "usage: remap_diskann_pages --src in.bin --out out.bin --map id_to_slot.bin "
                 "[--mode neighbor|coexpand|cooccur|extent] [--trace expands.bin] "
                 "[--map-in id_to_slot.bin] [--map-only]\n");
    return 2;
  }
  const bool extent = (std::string(mode) == "extent");
  if (extent && (!map_in || !map_in[0])) {
    std::fprintf(stderr, "--mode extent needs --map-in (keep existing pairs, reorder pages)\n");
    return 2;
  }

  int fd = open(src, O_RDONLY);
  if (fd < 0) {
    perror("open src");
    return 2;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    perror("stat");
    return 2;
  }
  const size_t len = (size_t)st.st_size;
  void* mp = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mp == MAP_FAILED) {
    perror("mmap");
    return 2;
  }
  auto* hdr = reinterpret_cast<const CxanLayoutHeader*>(mp);
  if (hdr->magic != kCxanMagic || !hdr->n) {
    std::fprintf(stderr, "bad header\n");
    return 2;
  }
  const uint32_t n = hdr->n;
  const size_t stride = (size_t)(hdr->len_vectors / hdr->n);
  if (!stride || stride > 4096 || 4096 % stride != 0) {
    std::fprintf(stderr, "record stride=%zu does not tile a 4 KiB page\n", stride);
    return 2;
  }
  const uint32_t slots_per_page = (uint32_t)(4096 / stride);
  if (slots_per_page > 2) {
    std::fprintf(stderr, "unsupported records-per-page=%u (expected 1 or 2)\n",
                 slots_per_page);
    return 2;
  }
  const uint64_t off_v = hdr->off_vectors;
  const size_t vb = (size_t)hdr->dim * hdr->vec_bytes;
  const uint8_t* base = static_cast<const uint8_t*>(mp);

  auto nnbrs = [&](uint32_t id) -> uint32_t {
    return *reinterpret_cast<const uint32_t*>(base + off_v + (size_t)id * stride + vb);
  };
  auto nbrs = [&](uint32_t id) -> const uint32_t* {
    return reinterpret_cast<const uint32_t*>(base + off_v + (size_t)id * stride + vb + 4);
  };

  std::vector<uint8_t> used((size_t)n, 0);
  std::vector<uint32_t> mate((size_t)n, UINT32_MAX);
  uint64_t paired = 0;
  const bool coexpand = (std::string(mode) == "coexpand");
  const bool cooccur = (std::string(mode) == "cooccur");
  std::vector<uint32_t> id_to_slot((size_t)n, UINT32_MAX);

  auto load_map = [&](const char* path) -> int {
    FILE* inf = std::fopen(path, "rb");
    if (!inf) {
      perror("open --map-in");
      return 2;
    }
    if (std::fread(id_to_slot.data(), 4, (size_t)n, inf) != (size_t)n) {
      std::fprintf(stderr, "short --map-in (want %u slots)\n", n);
      std::fclose(inf);
      return 2;
    }
    std::fclose(inf);
    return 0;
  };

  if (extent) {
    if (int rc = load_map(map_in)) return rc;
    const uint32_t n_pages = (n + slots_per_page - 1) / slots_per_page;
    const uint32_t stripe_pages = 512;  // 2 MiB / 4 KiB
    for (uint32_t id = 0; id < n; ++id) {
      if (id_to_slot[id] >= n) {
        std::fprintf(stderr, "bad map-in slot id=%u\n", id);
        return 2;
      }
    }
    uint64_t n_pairs = 0;
    std::vector<uint8_t> slot_used((size_t)n, 0);
    for (uint32_t id = 0; id < n; ++id) slot_used[id_to_slot[id]] = 1;
    if (slots_per_page == 2) {
      for (uint32_t s = 0; s + 1 < n; s += 2)
        if (slot_used[s] && slot_used[s + 1]) n_pairs++;
    }

    auto pages_of = [&](const uint32_t* ids, uint32_t m, uint32_t* out) -> uint32_t {
      uint32_t k = 0;
      for (uint32_t i = 0; i < m; ++i) {
        const uint32_t id = ids[i];
        if (id >= n) continue;
        out[k++] = id_to_slot[id] / slots_per_page;
      }
      std::sort(out, out + k);
      return (uint32_t)(std::unique(out, out + k) - out);
    };
    uint32_t tmp_ids[64];
    uint32_t tmp_pg[256];
    std::vector<std::vector<uint32_t>> runs;
    runs.reserve((size_t)n + 8192);
    uint64_t trace_expands = 0;
    if (trace_path && trace_path[0]) {
      FILE* tf = std::fopen(trace_path, "rb");
      if (!tf) {
        perror("open --trace");
        return 2;
      }
      uint32_t cnt = 0;
      while (std::fread(&cnt, 4, 1, tf) == 1) {
        if (cnt == 0 || cnt > 256) break;
        std::vector<uint32_t> ids(cnt);
        if (std::fread(ids.data(), 4, cnt, tf) != cnt) break;
        const uint32_t np = pages_of(ids.data(), (uint32_t)ids.size(), tmp_pg);
        if (np) runs.emplace_back(tmp_pg, tmp_pg + np);
        trace_expands++;
      }
      std::fclose(tf);
      std::fprintf(stdout, "extent trace expands=%llu\n", (unsigned long long)trace_expands);
    }
    const size_t n_trace_runs = runs.size();
    for (uint32_t u = 0; u < n; ++u) {
      const uint32_t deg = nnbrs(u);
      const uint32_t* nb = nbrs(u);
      uint32_t m = 0;
      for (uint32_t j = 0; j < deg && j < hdr->R && m < 64; ++j) {
        const uint32_t v = nb[j];
        if (v < n && v != u) tmp_ids[m++] = v;
      }
      const uint32_t np = pages_of(tmp_ids, m, tmp_pg);
      if (np) runs.emplace_back(tmp_pg, tmp_pg + np);
      if (u && (u % 200000u) == 0) {
        std::fprintf(stdout, "  extent parents %u runs=%zu\n", u, runs.size());
        fflush(stdout);
      }
    }
    // Each expand's still-free pages become one contiguous run. Traces first.
    std::vector<uint32_t> old_to_new((size_t)n_pages, UINT32_MAX);
    uint32_t next_new = 0;
    auto emit = [&](uint32_t oldp) {
      if (oldp >= n_pages || old_to_new[oldp] != UINT32_MAX) return;
      old_to_new[oldp] = next_new++;
    };
    auto pack_run = [&](const std::vector<uint32_t>& pgs) {
      for (uint32_t p : pgs) emit(p);
    };
    for (const auto& r : runs) pack_run(r);
    for (uint32_t p = 0; p < n_pages; ++p) emit(p);
    if (next_new != n_pages) {
      std::fprintf(stderr, "extent placed %u != pages %u\n", next_new, n_pages);
      return 2;
    }
    std::vector<uint32_t> new_slot((size_t)n, UINT32_MAX);
    for (uint32_t id = 0; id < n; ++id) {
      const uint32_t s = id_to_slot[id];
      new_slot[id] = old_to_new[s / slots_per_page] * slots_per_page +
                     (s % slots_per_page);
    }
    id_to_slot.swap(new_slot);

    auto pred_runs = [&](size_t lo, size_t hi, const char* tag) {
      if (lo >= hi) return;
      double pages_sum = 0, stripe_sum = 0, adj_sum = 0, span_sum = 0;
      uint32_t nsamp = 0;
      for (size_t i = lo; i < hi; ++i) {
        std::vector<uint32_t> pids = runs[i];
        for (uint32_t& p : pids) p = old_to_new[p];
        std::sort(pids.begin(), pids.end());
        pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
        if (pids.empty()) continue;
        uint32_t adj = 0;
        std::unordered_map<uint32_t, uint32_t> stripes;
        for (size_t j = 0; j < pids.size(); ++j) {
          stripes[pids[j] / stripe_pages]++;
          if (j && pids[j] == pids[j - 1] + 1) adj++;
        }
        pages_sum += (double)pids.size();
        stripe_sum += (double)stripes.size();
        adj_sum += pids.size() > 1 ? (double)adj / (double)(pids.size() - 1) : 0;
        span_sum += (double)(pids.back() - pids.front() + 1);
        nsamp++;
      }
      if (!nsamp) return;
      std::fprintf(stdout,
                   "extent-%s pages/expand=%.2f stripes/expand=%.2f adj=%.2f%% "
                   "page_span=%.1f (n=%u)\n",
                   tag, pages_sum / nsamp, stripe_sum / nsamp, 100.0 * adj_sum / nsamp,
                   span_sum / nsamp, nsamp);
    };
    pred_runs(0, n_trace_runs, "trace");
    pred_runs(n_trace_runs, runs.size(), "static");
    paired = n_pairs;
    std::fprintf(stdout, "extent kept_pairs=%llu pages=%u stripe_pages=%u\n",
                 (unsigned long long)paired, n_pages, stripe_pages);
  } else if (slots_per_page > 1 && cooccur) {
    // Weight (v,w) by how often they co-appear in the same N(u) (or expand trace).
    std::unordered_map<uint64_t, uint32_t> wt;
    wt.reserve((size_t)n * 8);
    auto add_pair = [&](uint32_t a, uint32_t b, uint32_t add) {
      if (a == b) return;
      if (a > b) std::swap(a, b);
      wt[((uint64_t)a << 32) | b] += add;
    };
    uint32_t tmp[64];
    // Full-graph C(R,2) unique pairs blow up at 10M (~450 new pairs / node).
    // --trace-pairs: only query-expand co-issue weights (the page-distribution
    // rebuild). Leftover nodes are neighbor-paired after the greedy match.
    if (!trace_pairs) {
      for (uint32_t u = 0; u < n; ++u) {
        const uint32_t deg = nnbrs(u);
        const uint32_t* nb = nbrs(u);
        uint32_t m = 0;
        for (uint32_t j = 0; j < deg && j < hdr->R && m < 64; ++j) {
          const uint32_t v = nb[j];
          if (v < n && v != u) tmp[m++] = v;
        }
        std::sort(tmp, tmp + m);
        m = (uint32_t)(std::unique(tmp, tmp + m) - tmp);
        for (uint32_t i = 0; i < m; ++i)
          for (uint32_t j = i + 1; j < m; ++j) add_pair(tmp[i], tmp[j], 1);
        if (u && (u % 200000u) == 0) {
          std::fprintf(stdout, "  cooccur parents %u pairs=%zu\n", u, wt.size());
          fflush(stdout);
        }
      }
    }
    uint64_t trace_expands = 0;
    if (trace_path && trace_path[0]) {
      FILE* tf = std::fopen(trace_path, "rb");
      if (!tf) {
        perror("open --trace");
        return 2;
      }
      uint32_t cnt = 0;
      while (std::fread(&cnt, 4, 1, tf) == 1) {
        if (cnt == 0 || cnt > 256) break;
        std::vector<uint32_t> ids(cnt);
        if (std::fread(ids.data(), 4, cnt, tf) != cnt) break;
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (size_t i = 0; i < ids.size(); ++i) {
          if (ids[i] >= n) continue;
          for (size_t j = i + 1; j < ids.size(); ++j) {
            if (ids[j] >= n) continue;
            add_pair(ids[i], ids[j], 1000);
          }
        }
        trace_expands++;
      }
      std::fclose(tf);
      std::fprintf(stdout, "trace expands=%llu pairs=%zu\n",
                   (unsigned long long)trace_expands, wt.size());
    }
    struct Edge {
      uint32_t w, a, b;
    };
    std::vector<Edge> edges;
    edges.reserve(wt.size());
    for (const auto& kv : wt)
      edges.push_back({kv.second, (uint32_t)(kv.first >> 32), (uint32_t)kv.first});
    std::sort(edges.begin(), edges.end(),
              [](const Edge& x, const Edge& y) { return x.w > y.w; });
    uint32_t wsum = 0;
    for (const Edge& e : edges) {
      if (used[e.a] || used[e.b]) continue;
      used[e.a] = used[e.b] = 1;
      mate[e.a] = e.b;
      mate[e.b] = e.a;
      paired++;
      wsum += e.w;
    }
    std::fprintf(stdout, "cooccur unique_pairs=%zu matched=%llu weight_sum=%u trace_pairs=%d\n",
                 edges.size(), (unsigned long long)paired, wsum, (int)trace_pairs);
    uint64_t nbr_fill = 0;
    for (uint32_t u = 0; u < n; ++u) {
      if (used[u]) continue;
      const uint32_t deg = nnbrs(u);
      const uint32_t* nb = nbrs(u);
      uint32_t found = UINT32_MAX;
      for (uint32_t j = 0; j < deg && j < hdr->R; ++j) {
        const uint32_t v = nb[j];
        if (v < n && v != u && !used[v]) {
          found = v;
          break;
        }
      }
      if (found == UINT32_MAX) continue;
      used[u] = used[found] = 1;
      mate[u] = found;
      mate[found] = u;
      paired++;
      nbr_fill++;
    }
    std::fprintf(stdout, "cooccur leftover_neighbor_pairs=%llu total_paired=%llu\n",
                 (unsigned long long)nbr_fill, (unsigned long long)paired);
  } else if (slots_per_page > 1 && coexpand) {
    // Pair two unused members of the same N(u) so one expand issues 1 page not 2.
    for (uint32_t u = 0; u < n; ++u) {
      const uint32_t deg = nnbrs(u);
      const uint32_t* nb = nbrs(u);
      uint32_t pending = UINT32_MAX;
      for (uint32_t j = 0; j < deg && j < hdr->R; ++j) {
        const uint32_t v = nb[j];
        if (v >= n || v == u || used[v] || v == pending) continue;
        if (pending == UINT32_MAX) {
          pending = v;
          continue;
        }
        used[pending] = used[v] = 1;
        mate[pending] = v;
        mate[v] = pending;
        paired++;
        pending = UINT32_MAX;
      }
    }
  } else if (slots_per_page > 1) {
    // First unused neighbor in DiskANN order (usually the strongest edge).
    for (uint32_t u = 0; u < n; ++u) {
      if (used[u]) continue;
      const uint32_t deg = nnbrs(u);
      const uint32_t* nb = nbrs(u);
      uint32_t found = UINT32_MAX;
      for (uint32_t j = 0; j < deg && j < hdr->R; ++j) {
        const uint32_t v = nb[j];
        if (v < n && v != u && !used[v]) {
          found = v;
          break;
        }
      }
      if (found == UINT32_MAX) continue;
      used[u] = used[found] = 1;
      mate[u] = found;
      mate[found] = u;
      paired++;
    }
  }

  std::vector<uint32_t> leftover;
  if (!extent) {
    uint32_t slot = 0;
    for (uint32_t u = 0; u < n; ++u) {
      if (mate[u] == UINT32_MAX || mate[u] == u || u > mate[u]) continue;
      id_to_slot[u] = slot++;
      id_to_slot[mate[u]] = slot++;
    }
    leftover.reserve(n - 2 * paired);
    for (uint32_t u = 0; u < n; ++u) {
      if (id_to_slot[u] == UINT32_MAX) leftover.push_back(u);
    }
    for (uint32_t u : leftover) id_to_slot[u] = slot++;
    if (slot != n) {
      std::fprintf(stderr, "slot assign %u != n %u\n", slot, n);
      return 2;
    }
  }

  {
    double pages_sum = 0, two_sum = 0, occ_sum = 0;
    double stripe_sum = 0, adj_sum = 0, span_sum = 0;
    uint32_t nsamp = 0;
    const uint32_t stripe_pages = 512;
    for (uint32_t u = 0; u < n; u += 10) {
      const uint32_t deg = nnbrs(u);
      const uint32_t* nb = nbrs(u);
      std::unordered_map<uint32_t, uint32_t> pg;
      for (uint32_t j = 0; j < deg && j < hdr->R; ++j) {
        const uint32_t v = nb[j];
        if (v >= n || v == u) continue;
        pg[id_to_slot[v] / slots_per_page]++;
      }
      if (pg.empty()) continue;
      uint32_t two = 0, used_slots = 0;
      for (const auto& kv : pg) {
        used_slots += kv.second > slots_per_page ? slots_per_page : kv.second;
        if (slots_per_page == 2 && kv.second >= 2) two++;
      }
      std::vector<uint32_t> pids;
      pids.reserve(pg.size());
      for (const auto& kv : pg) pids.push_back(kv.first);
      std::sort(pids.begin(), pids.end());
      uint32_t adj = 0;
      std::unordered_map<uint32_t, uint32_t> stripes;
      for (size_t i = 0; i < pids.size(); ++i) {
        stripes[pids[i] / stripe_pages]++;
        if (i && pids[i] == pids[i - 1] + 1) adj++;
      }
      pages_sum += (double)pg.size();
      two_sum += (double)two;
      occ_sum += (double)used_slots / ((double)slots_per_page * (double)pg.size());
      stripe_sum += (double)stripes.size();
      adj_sum += pids.size() > 1 ? (double)adj / (double)(pids.size() - 1) : 0;
      span_sum += (double)(pids.back() - pids.front() + 1);
      nsamp++;
    }
    if (nsamp) {
      std::fprintf(stdout,
                   "pred pages/expand=%.2f 2-want=%.2f issue_occ=%.2f%% "
                   "stripes/expand=%.2f adj=%.2f%% page_span=%.1f (sample=%u)\n",
                   pages_sum / nsamp, two_sum / nsamp, 100.0 * occ_sum / nsamp,
                   stripe_sum / nsamp, 100.0 * adj_sum / nsamp, span_sum / nsamp, nsamp);
    }
  }

  uint64_t nbr_pages = slots_per_page == 2 ? paired : 0;
  uint64_t tot_pages = ((uint64_t)n + slots_per_page - 1) / slots_per_page;
  std::fprintf(stdout,
               "mode=%s n=%u paired=%llu leftover=%zu nbr_page_pct=%.2f%% "
               "pages=%llu stride=%zu\n",
               mode, n, (unsigned long long)paired, leftover.size(),
               tot_pages ? 100.0 * (double)nbr_pages / (double)tot_pages : 0,
               (unsigned long long)tot_pages, stride);
  fflush(stdout);

  FILE* mf = std::fopen(out_map, "wb");
  if (!mf) {
    perror("open map");
    return 2;
  }
  if (std::fwrite(id_to_slot.data(), 4, (size_t)n, mf) != (size_t)n) {
    std::fprintf(stderr, "short map write\n");
    return 2;
  }
  std::fclose(mf);

  if (map_only) {
    munmap(mp, len);
    close(fd);
    std::fprintf(stdout, "OK map-only %s (no image)\n", out_map);
    return 0;
  }

  FILE* of = std::fopen(out_img, "wb");
  if (!of) {
    perror("open out");
    return 2;
  }
  if (std::fwrite(base, 1, (size_t)off_v, of) != (size_t)off_v) {
    std::fprintf(stderr, "short header write\n");
    return 2;
  }
  std::vector<uint32_t> slot_to_id((size_t)n);
  for (uint32_t id = 0; id < n; ++id) slot_to_id[id_to_slot[id]] = id;
  std::vector<uint8_t> rec(stride);
  for (uint32_t s = 0; s < n; ++s) {
    const uint32_t id = slot_to_id[s];
    std::memcpy(rec.data(), base + off_v + (size_t)id * stride, stride);
    if (std::fwrite(rec.data(), 1, stride, of) != stride) {
      std::fprintf(stderr, "short rec write slot=%u\n", s);
      return 2;
    }
    if (s && (s % 200000u) == 0) {
      std::fprintf(stdout, "  wrote %u / %u\n", s, n);
      fflush(stdout);
    }
  }
  std::fclose(of);
  munmap(mp, len);
  close(fd);
  std::fprintf(stdout, "OK %s map=%s\n", out_img, out_map);
  return 0;
}
