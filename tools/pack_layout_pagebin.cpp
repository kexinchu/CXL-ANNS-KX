// A: page-bin star-pack, keep vectors densely packed (no 4K pad).
// Bin capacity C = ceil(2*page / vec_bytes): IDs that touch a 2-page fetch.
// Copies an existing CXAN layout (PQ/graph/vectors), remaps IDs, writes packed image.
//
//   g++ -O3 -std=c++17 -pthread tools/pack_layout_pagebin.cpp -o tools/pack_layout_pagebin
//
// Metrics only:
//   ./tools/pack_layout_pagebin --in-vmem-dev /dev/vmem0 --in-vmem-offset $((200<<30)) \
//       --in-vmem-len 80800071680 --metrics-only
//
// Full write:
//   ./tools/pack_layout_pagebin --in-vmem-* --out-vmem-* --out-entry --out-map --out-json

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

int main(int argc, char** argv) {
  setvbuf(stderr, nullptr, _IONBF, 0);
  std::string in_dev, out_dev, out_json, out_entry, out_map;
  off_t in_off = -1, out_off = -1;
  uint64_t in_len = 0;
  uint64_t page = 4096;
  int nthreads = 4;
  bool metrics_only = false;
  uint32_t C_override = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&]() -> std::string {
      if (i + 1 >= argc) die("missing arg");
      return argv[++i];
    };
    if (a == "--in-vmem-dev") in_dev = need();
    else if (a == "--in-vmem-offset") in_off = (off_t)std::stoll(need());
    else if (a == "--in-vmem-len") in_len = std::stoull(need());
    else if (a == "--out-vmem-dev") out_dev = need();
    else if (a == "--out-vmem-offset") out_off = (off_t)std::stoll(need());
    else if (a == "--out-json") out_json = need();
    else if (a == "--out-entry") out_entry = need();
    else if (a == "--out-map") out_map = need();
    else if (a == "--threads") nthreads = std::stoi(need());
    else if (a == "--page") page = std::stoull(need());
    else if (a == "--bin-c") C_override = (uint32_t)std::stoul(need());
    else if (a == "--metrics-only") metrics_only = true;
    else die("unknown " + a);
  }
  if (in_dev.empty() || in_off < 0 || in_len == 0) die("need --in-vmem-*");
  if (!metrics_only &&
      (out_dev.empty() || out_off < 0 || out_json.empty() || out_entry.empty() ||
       out_map.empty()))
    die("need --out-vmem-* --out-json --out-entry --out-map (or --metrics-only)");

  int ifd = ::open(in_dev.c_str(), O_RDONLY);
  if (ifd < 0) die("open in vmem");
  char* in = (char*)mmap(nullptr, in_len, PROT_READ, MAP_SHARED, ifd, in_off);
  if (in == MAP_FAILED) die("mmap in");
  auto* ih = (const LayoutHeader*)in;
  if (ih->magic != 0x314e415843ull) die("bad magic");
  uint32_t n = ih->n, dim = ih->dim, R = ih->R;
  size_t packed = (size_t)dim * ih->vec_bytes;
  size_t in_stride = packed;
  if (ih->n && ih->len_vectors) {
    size_t st = (size_t)(ih->len_vectors / ih->n);
    if (st >= packed) in_stride = st;
  }
  // Output is always densely packed.
  uint32_t C = C_override;
  if (!C) C = (uint32_t)((2 * page + packed - 1) / packed);
  if (C < 2) C = 2;
  fprintf(stderr,
          "in n=%u dim=%u R=%u packed=%zu in_stride=%zu page_touch_C=%u "
          "permute=starBFS_nbrblock threads=%d\n",
          n, dim, R, packed, in_stride, C, nthreads);

  const uint32_t* graph =
      reinterpret_cast<const uint32_t*>(in + ih->off_graph);

  std::vector<uint32_t> new_to_old(n);
  std::vector<uint32_t> old_to_new(n, UINT32_MAX);
  uint32_t assigned = 0;
  auto assign_one = [&](uint32_t u) {
    if (old_to_new[u] != UINT32_MAX) return;
    old_to_new[u] = assigned;
    new_to_old[assigned] = u;
    assigned++;
  };

  uint32_t entry_old = ih->entry_id;
  if (entry_old >= n) entry_old = 0;
  {
    std::queue<uint32_t> q;
    std::vector<uint8_t> inq(n, 0);
    auto try_push = [&](uint32_t v) {
      if (v >= n || inq[v]) return;
      inq[v] = 1;
      q.push(v);
    };
    // Star-pack BFS: when visiting u, assign all still-free neighbors as a
    // contiguous block so one expand's fetches share packed 2-page groups
    // (3 vecs / 2 pages at 768-d; more at lower dim).
    try_push(entry_old);
    while (!q.empty()) {
      uint32_t u = q.front();
      q.pop();
      assign_one(u);
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t v = graph[(size_t)u * R + k];
        if (v >= n) continue;
        if (old_to_new[v] == UINT32_MAX) assign_one(v);
        try_push(v);
      }
      if ((assigned & ((1u << 20) - 1)) == 0)
        fprintf(stderr, "  nbrblock assigned=%u/%u\n", assigned, n);
    }
  }
  for (uint32_t u = 0; u < n; ++u) assign_one(u);
  if (assigned != n) die("permute incomplete");
  uint32_t entry_new = old_to_new[entry_old];
  fprintf(stderr, "pagebin permute done entry_old=%u entry_new=%u C=%u\n", entry_old,
          entry_new, C);

  auto page_share = [&](uint32_t nu, uint32_t nv) -> bool {
    uint64_t ou = (uint64_t)nu * packed, ov = (uint64_t)nv * packed;
    uint64_t pu0 = ou / page, pu1 = (ou + packed - 1) / page;
    uint64_t pv0 = ov / page, pv1 = (ov + packed - 1) / page;
    return !(pu1 < pv0 || pv1 < pu0);
  };

  {
    uint64_t cnt = 0, d1 = 0, d2 = 0, d3 = 0, dC = 0, d64 = 0, share = 0;
    uint64_t share_old = 0, d2_old = 0;
    for (uint32_t i = 0; i < n; i += 8) {
      uint32_t old_u = new_to_old[i];
      uint64_t ooff = (uint64_t)old_u * packed;
      uint64_t op0 = ooff / page, op1 = (ooff + packed - 1) / page;
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t old_v = graph[(size_t)old_u * R + k];
        if (old_v >= n || old_v == old_u) continue;
        uint32_t nv = old_to_new[old_v];
        uint32_t dn = (nv > i) ? (nv - i) : (i - nv);
        uint32_t d_old = (old_v > old_u) ? (old_v - old_u) : (old_u - old_v);
        cnt++;
        if (dn <= 1) d1++;
        if (dn <= 2) d2++;
        if (dn <= 3) d3++;
        if (dn < C) dC++;
        if (dn <= 64) d64++;
        if (page_share(i, nv)) share++;
        uint64_t ov = (uint64_t)old_v * packed;
        uint64_t qp0 = ov / page, qp1 = (ov + packed - 1) / page;
        if (!(op1 < qp0 || qp1 < op0)) share_old++;
        if (d_old <= 2) d2_old++;
      }
    }
    auto pct = [&](uint64_t x) { return cnt ? 100.0 * x / cnt : 0.0; };
    fprintf(stderr,
            "NEW |d|<=1 %.3f%% <=2 %.3f%% <=3 %.3f%% <C %.3f%% <=64 %.3f%%  "
            "page-share %.3f%%  (cnt=%llu)\n",
            pct(d1), pct(d2), pct(d3), pct(dC), pct(d64), pct(share),
            (unsigned long long)cnt);
    fprintf(stderr, "OLD |d|<=2 %.3f%% page-share %.3f%%\n", pct(d2_old), pct(share_old));
  }
  // Search-hot expand I/O: BFS-200k from entry, unique pages to fetch all nbrs.
  {
    uint64_t N = 0, pages = 0;
    std::vector<uint8_t> seen((n + 7) / 8, 0);
    auto mark = [&](uint32_t x) { seen[x >> 3] |= (uint8_t)(1u << (x & 7)); };
    auto has = [&](uint32_t x) { return (seen[x >> 3] >> (x & 7)) & 1; };
    std::queue<uint32_t> hq;
    hq.push(entry_old);
    mark(entry_old);
    while (!hq.empty() && N < 200000) {
      uint32_t u = hq.front();
      hq.pop();
      uint64_t bits[4] = {0, 0, 0, 0};  // 256 pages enough? use simple unique via sort
      uint64_t tmp[64];
      uint32_t nt = 0;
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t v = graph[(size_t)u * R + k];
        if (v >= n || v == u) continue;
        uint32_t nv = old_to_new[v];
        uint64_t off = (uint64_t)nv * packed;
        uint64_t p0 = off / page, p1 = (off + packed - 1) / page;
        for (uint64_t p = p0; p <= p1 && nt < 64; ++p) tmp[nt++] = p;
        if (!has(v)) {
          mark(v);
          hq.push(v);
        }
      }
      std::sort(tmp, tmp + nt);
      uint32_t uniq = 0;
      for (uint32_t i = 0; i < nt; ++i)
        if (i == 0 || tmp[i] != tmp[i - 1]) uniq++;
      pages += uniq;
      N++;
    }
    fprintf(stderr, "hot200k mean_pages/expand=%.2f (N=%llu)\n", N ? pages / (double)N : 0,
            (unsigned long long)N);
  }

  if (metrics_only) {
    munmap(in, in_len);
    ::close(ifd);
    fprintf(stderr, "metrics-only, no write\n");
    return 0;
  }

  LayoutHeader hdr = *ih;
  hdr.vec_bytes = ih->vec_bytes;
  hdr.entry_id = entry_new;
  hdr.pad = C;  // hint: page-bin capacity
  uint64_t cursor = align_up(sizeof(LayoutHeader), page);
  hdr.off_pq = cursor;
  hdr.len_pq = (uint64_t)n * ih->pq_bytes;
  cursor = align_up(cursor + hdr.len_pq, page);
  hdr.off_graph = cursor;
  hdr.len_graph = (uint64_t)n * R * 4;
  cursor = align_up(cursor + hdr.len_graph, page);
  hdr.off_vectors = cursor;
  hdr.len_vectors = (uint64_t)n * packed;  // dense
  cursor = align_up(cursor + hdr.len_vectors, page);
  hdr.off_pivots = cursor;
  hdr.len_pivots = ih->len_pivots ? ih->len_pivots : 64 * 1024;
  cursor = align_up(cursor + hdr.len_pivots, page);
  hdr.checksum = hdr.n ^ hdr.dim ^ hdr.R ^ hdr.len_vectors;
  fprintf(stderr, "out packed layout_bytes=%llu (%.2f GiB)\n", (unsigned long long)cursor,
          cursor / (1024.0 * 1024 * 1024));

  int ofd = ::open(out_dev.c_str(), O_RDWR);
  if (ofd < 0) die("open out vmem");
  char* out = (char*)mmap(nullptr, cursor, PROT_READ | PROT_WRITE, MAP_SHARED, ofd, out_off);
  if (out == MAP_FAILED) die("mmap out");
  madvise(out, cursor, MADV_SEQUENTIAL);
  std::memcpy(out, &hdr, sizeof(hdr));

  // PQ permute
  {
    const char* sp = in + ih->off_pq;
    char* dp = out + hdr.off_pq;
    uint32_t pb = ih->pq_bytes;
    for (uint32_t ni = 0; ni < n; ++ni) {
      uint32_t ou = new_to_old[ni];
      std::memcpy(dp + (size_t)ni * pb, sp + (size_t)ou * pb, pb);
      if ((ni & ((1u << 20) - 1)) == 0) fprintf(stderr, "  pq %u/%u\n", ni, n);
    }
    fprintf(stderr, "pq remapped\n");
  }

  // Graph remap
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
      if ((ni & ((1u << 20) - 1)) == 0) fprintf(stderr, "  graph %u/%u\n", ni, n);
    }
    fprintf(stderr, "graph remapped\n");
  }

  // Vectors: dense packed, parallel by new-id
  {
    const char* sv = in + ih->off_vectors;
    char* dv = out + hdr.off_vectors;
    std::atomic<uint32_t> next{0};
    std::atomic<uint32_t> done{0};
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
        uint32_t d = done.fetch_add(end - begin, std::memory_order_relaxed) + (end - begin);
        if ((d & ((1u << 20) - 1)) < kChunk || d == n)
          fprintf(stderr, "  vectors %u/%u\n", d, n);
      }
    };
    std::vector<std::thread> th;
    for (int t = 0; t < nthreads; ++t) th.emplace_back(worker);
    for (auto& t : th) t.join();
    fprintf(stderr, "vectors remapped packed\n");
  }

  if (ih->len_pivots)
    std::memcpy(out + hdr.off_pivots, in + ih->off_pivots, (size_t)hdr.len_pivots);

  // Entry BFS in new graph
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
    for (uint32_t id : entry_list)
      o.write((char*)(ng + (size_t)id * R), R * 4);
  }
  {
    std::ofstream o(out_map, std::ios::binary);
    o.write((char*)new_to_old.data(), (size_t)n * 4);
  }
  {
    std::ofstream js(out_json);
    js << "{\n"
       << "  \"n\": " << n << ",\n"
       << "  \"dim\": " << dim << ",\n"
       << "  \"R\": " << R << ",\n"
       << "  \"vec_bytes\": " << hdr.vec_bytes << ",\n"
       << "  \"packed\": " << packed << ",\n"
       << "  \"page_bin_c\": " << C << ",\n"
       << "  \"entry_id_old\": " << entry_old << ",\n"
       << "  \"entry_id\": " << hdr.entry_id << ",\n"
       << "  \"entry_nodes\": " << hdr.entry_nodes << ",\n"
       << "  \"image_bytes\": " << cursor << ",\n"
       << "  \"off_vectors\": " << hdr.off_vectors << ",\n"
       << "  \"len_vectors\": " << hdr.len_vectors << ",\n"
       << "  \"vmem_offset\": " << (long long)out_off << ",\n"
       << "            \"reorder\": \"starBFS_nbrblock_packed\",\n"
       << "  \"note\": \"dense packed; BFS neighbor blocks; 2-page groups hold ~C vecs\"\n"
       << "}\n";
  }

  fprintf(stderr, "msync...\n");
  msync(out, cursor, MS_SYNC);
  munmap(out, cursor);
  ::close(ofd);
  munmap(in, in_len);
  ::close(ifd);
  fprintf(stderr, "DONE pagebin packed bytes=%llu\n", (unsigned long long)cursor);
  return 0;
}
