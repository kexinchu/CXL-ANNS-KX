// BFS-reorder CXAN layout for graph locality (25M-scale, stream to vmem).
// Assigns new contiguous IDs in BFS order from entry_id, remaps graph neighbors,
// and streams vectors/PQ in new-id order. Writes new_to_old map for recall.
//
// Build:
//   g++ -O2 -std=c++17 tools/reorder_layout_bfs.cpp -o tools/reorder_layout_bfs
//
// Example:
//   ./tools/reorder_layout_bfs \
//     --base .../base.bin --graph .../graph_R32.bin \
//     --vmem-dev /dev/vmem0 --vmem-offset $((300<<30)) \
//     --out-entry .../serving_entry_25m_bfs.bin \
//     --out-map .../new_to_old_bfs.bin \
//     --out-json .../layout_25m_bfs.json \
//     --entry-id 6681339 --entry-nodes 8192 --R 32 --vec-bytes 4 --pq-bytes 32

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <queue>
#include <string>
#include <utility>
#include <vector>
#include <cmath>

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

static void make_pq_u8(const float* v, uint32_t dim, uint8_t* out, uint32_t pq_bytes) {
  for (uint32_t i = 0; i < pq_bytes; ++i) {
    float x = (i < dim) ? v[i] : 0.f;
    int q = (int)std::lround((x * 0.5f + 0.5f) * 255.f);
    if (q < 0) q = 0;
    if (q > 255) q = 255;
    out[i] = (uint8_t)q;
  }
}

int main(int argc, char** argv) {
  std::string base_bin, graph_bin, vmem_dev, out_entry, out_map, out_json;
  uint32_t pq_bytes = 32, vec_bytes = 4, R = 32, entry_nodes = 8192;
  uint32_t entry_id = 0;
  off_t vmem_off = -1;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&]() -> std::string {
      if (i + 1 >= argc) die("missing arg");
      return argv[++i];
    };
    if (a == "--base") base_bin = need();
    else if (a == "--graph") graph_bin = need();
    else if (a == "--vmem-dev") vmem_dev = need();
    else if (a == "--vmem-offset") vmem_off = (off_t)std::stoll(need());
    else if (a == "--out-entry") out_entry = need();
    else if (a == "--out-map") out_map = need();
    else if (a == "--out-json") out_json = need();
    else if (a == "--pq-bytes") pq_bytes = (uint32_t)std::stoul(need());
    else if (a == "--vec-bytes") vec_bytes = (uint32_t)std::stoul(need());
    else if (a == "--R") R = (uint32_t)std::stoul(need());
    else if (a == "--entry-id") entry_id = (uint32_t)std::stoul(need());
    else if (a == "--entry-nodes") entry_nodes = (uint32_t)std::stoul(need());
    else die("unknown " + a);
  }
  if (base_bin.empty() || graph_bin.empty() || vmem_dev.empty() || vmem_off < 0 ||
      out_entry.empty() || out_map.empty() || out_json.empty())
    die("need --base --graph --vmem-dev --vmem-offset --out-entry --out-map --out-json");
  if (vec_bytes != 4) die("only vec_bytes=4 supported");

  int bfd = ::open(base_bin.c_str(), O_RDONLY);
  if (bfd < 0) die("open base");
  uint32_t n = 0, dim = 0;
  if (::read(bfd, &n, 4) != 4 || ::read(bfd, &dim, 4) != 4) die("base hdr");
  fprintf(stderr, "base n=%u dim=%u\n", n, dim);

  struct stat gst {};
  if (stat(graph_bin.c_str(), &gst) != 0) die("stat graph");
  if ((size_t)gst.st_size != (size_t)n * R * 4) die("graph size != n*R*4");
  int gfd = ::open(graph_bin.c_str(), O_RDONLY);
  if (gfd < 0) die("open graph");
  void* gmap = mmap(nullptr, (size_t)gst.st_size, PROT_READ, MAP_PRIVATE, gfd, 0);
  if (gmap == MAP_FAILED) die("mmap graph");
  const uint32_t* graph = (const uint32_t*)gmap;

  if (entry_id >= n) entry_id = 0;
  if (entry_nodes > n) entry_nodes = n;

  // Star-pack BFS: when processing u, assign contiguous new IDs to all still-unassigned
  // neighbors (packs 1-hop stars). Reduces mean |Δnew_id| on edges vs classic BFS.
  std::vector<uint32_t> new_to_old(n);
  std::vector<uint32_t> old_to_new(n, UINT32_MAX);
  uint32_t assigned = 0;
  auto assign = [&](uint32_t u) {
    if (old_to_new[u] != UINT32_MAX) return;
    old_to_new[u] = assigned;
    new_to_old[assigned] = u;
    assigned++;
  };

  {
    std::queue<uint32_t> q;
    assign(entry_id);
    q.push(entry_id);
    while (!q.empty()) {
      uint32_t u = q.front();
      q.pop();
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t v = graph[(size_t)u * R + k];
        if (v >= n || old_to_new[v] != UINT32_MAX) continue;
        assign(v);
        q.push(v);
      }
      if ((assigned & ((1u << 20) - 1)) == 0)
        fprintf(stderr, "  star-pack assigned=%u/%u\n", assigned, n);
    }
  }
  for (uint32_t u = 0; u < n; ++u) {
    if (old_to_new[u] != UINT32_MAX) continue;
    assign(u);
  }
  if (assigned != n) die("permute incomplete");
  uint32_t entry_id_new = old_to_new[entry_id];
  fprintf(stderr, "star-pack permute done entry_old=%u entry_new=%u\n", entry_id,
          entry_id_new);

  // Edge locality metrics (new id space).
  {
    uint64_t sum = 0, cnt = 0, within64 = 0, within256 = 0, within4k = 0;
    uint64_t sum_old = 0;
    for (uint32_t i = 0; i < n; i += 64) {
      uint32_t old_u = new_to_old[i];
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t old_v = graph[(size_t)old_u * R + k];
        if (old_v >= n) continue;
        uint32_t nv = old_to_new[old_v];
        uint32_t dn = (nv > i) ? (nv - i) : (i - nv);
        uint32_t do_ = (old_v > old_u) ? (old_v - old_u) : (old_u - old_v);
        sum += dn;
        sum_old += do_;
        cnt++;
        if (dn <= 64) within64++;
        if (dn <= 256) within256++;
        if (dn <= 4096) within4k++;
      }
    }
    fprintf(stderr,
            "edge locality: mean|Δnew|=%.1f mean|Δold|=%.1f  "
            "frac|Δnew|<=64: %.4f <=256: %.4f <=4096: %.4f (cnt=%llu)\n",
            cnt ? (double)sum / cnt : 0.0, cnt ? (double)sum_old / cnt : 0.0,
            cnt ? (double)within64 / cnt : 0.0, cnt ? (double)within256 / cnt : 0.0,
            cnt ? (double)within4k / cnt : 0.0, (unsigned long long)cnt);
  }

  LayoutHeader hdr;
  hdr.n = n;
  hdr.dim = dim;
  hdr.R = R;
  hdr.pq_bytes = pq_bytes;
  hdr.vec_bytes = vec_bytes;
  hdr.entry_id = entry_id_new;
  hdr.entry_nodes = entry_nodes;
  const uint64_t page = 4096;
  uint64_t cursor = align_up(sizeof(LayoutHeader), page);
  hdr.off_pq = cursor;
  hdr.len_pq = (uint64_t)n * pq_bytes;
  cursor = align_up(cursor + hdr.len_pq, page);
  hdr.off_graph = cursor;
  hdr.len_graph = (uint64_t)n * R * 4;
  cursor = align_up(cursor + hdr.len_graph, page);
  hdr.off_vectors = cursor;
  hdr.len_vectors = (uint64_t)n * dim * vec_bytes;
  cursor = align_up(cursor + hdr.len_vectors, page);
  hdr.off_pivots = cursor;
  hdr.len_pivots = 64 * 1024;
  cursor = align_up(cursor + hdr.len_pivots, page);
  hdr.checksum = hdr.n ^ hdr.dim ^ hdr.R ^ hdr.len_vectors;
  fprintf(stderr, "layout_bytes=%llu (%.2f GiB) vmem_off=%lld\n", (unsigned long long)cursor,
          cursor / (1024.0 * 1024 * 1024), (long long)vmem_off);

  int vfd = ::open(vmem_dev.c_str(), O_RDWR);
  if (vfd < 0) die("open vmem");
  char* vmem = (char*)mmap(nullptr, cursor, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, vmem_off);
  if (vmem == MAP_FAILED) die("mmap vmem");

  auto emit = [&](uint64_t off, const void* p, size_t len) {
    std::memcpy(vmem + off, p, len);
  };
  emit(0, &hdr, sizeof(hdr));

  // Remapped graph in new-id order.
  {
    std::vector<uint32_t> row(R);
    const size_t batch = 4096;
    std::vector<uint32_t> batch_buf(batch * R);
    uint64_t goff = hdr.off_graph;
    for (uint32_t ni = 0; ni < n;) {
      uint32_t b = std::min<uint32_t>(batch, n - ni);
      for (uint32_t j = 0; j < b; ++j) {
        uint32_t old_u = new_to_old[ni + j];
        const uint32_t* src = graph + (size_t)old_u * R;
        for (uint32_t k = 0; k < R; ++k) {
          uint32_t ov = src[k];
          batch_buf[j * R + k] = (ov < n) ? old_to_new[ov] : 0;
        }
      }
      emit(goff, batch_buf.data(), (size_t)b * R * 4);
      goff += (uint64_t)b * R * 4;
      ni += b;
      if (ni % 1000000 == 0 || ni == n) fprintf(stderr, "  graph %u/%u\n", ni, n);
    }
  }

  // Vectors + PQ in new-id order. Chunk by new_id, sort by old_id for sequential-ish pread.
  {
    const uint32_t CHUNK = 65536;
    std::vector<float> rows((size_t)CHUNK * dim);
    std::vector<uint8_t> pq_batch((size_t)CHUNK * pq_bytes);
    std::vector<std::pair<uint32_t, uint32_t>> order;
    order.reserve(CHUNK);
    const off_t base_payload = 8;
    uint64_t pq_off = hdr.off_pq;
    uint64_t vec_off = hdr.off_vectors;
    for (uint32_t ni0 = 0; ni0 < n; ni0 += CHUNK) {
      uint32_t b = std::min(CHUNK, n - ni0);
      order.clear();
      for (uint32_t j = 0; j < b; ++j) order.push_back({new_to_old[ni0 + j], j});
      std::sort(order.begin(), order.end());
      for (auto [old_u, j] : order) {
        off_t pos = base_payload + (off_t)old_u * (off_t)dim * 4;
        if (::pread(bfd, rows.data() + (size_t)j * dim, dim * 4, pos) != (ssize_t)(dim * 4))
          die("pread vec");
        make_pq_u8(rows.data() + (size_t)j * dim, dim, pq_batch.data() + (size_t)j * pq_bytes,
                   pq_bytes);
      }
      emit(pq_off, pq_batch.data(), (size_t)b * pq_bytes);
      pq_off += (uint64_t)b * pq_bytes;
      emit(vec_off, rows.data(), (size_t)b * dim * 4);
      vec_off += (uint64_t)b * dim * 4;
      if ((ni0 / CHUNK) % 16 == 0 || ni0 + b == n)
        fprintf(stderr, "  vectors/pq %u/%u\n", ni0 + b, n);
    }
  }
  ::close(bfd);

  std::vector<char> z((size_t)hdr.len_pivots, 0);
  emit(hdr.off_pivots, z.data(), z.size());
  msync(vmem, cursor, MS_SYNC);

  // Entry: first entry_nodes in BFS (= new ids 0..entry_nodes-1 if entry is root).
  std::vector<uint32_t> entry_list(entry_nodes);
  for (uint32_t i = 0; i < entry_nodes; ++i) entry_list[i] = i;
  // If entry_id_new != 0, rebuild entry BFS in new graph on vmem.
  if (entry_id_new != 0) {
    entry_list.clear();
    std::vector<uint8_t> eseen((n + 7) / 8, 0);
    auto emark = [&](uint32_t x) { eseen[x >> 3] |= (uint8_t)(1u << (x & 7)); };
    auto ehas = [&](uint32_t x) { return (eseen[x >> 3] >> (x & 7)) & 1; };
    std::queue<uint32_t> q;
    q.push(entry_id_new);
    emark(entry_id_new);
    const uint32_t* ng =
        reinterpret_cast<const uint32_t*>(vmem + hdr.off_graph);
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
    emit(0, &hdr, sizeof(hdr));
  }

  {
    std::ofstream out(out_entry, std::ios::binary);
    uint32_t en = (uint32_t)entry_list.size();
    uint32_t eid = hdr.entry_id;
    out.write((char*)&en, 4);
    out.write((char*)&R, 4);
    out.write((char*)&eid, 4);
    out.write((char*)entry_list.data(), en * 4);
    const uint32_t* ng = reinterpret_cast<const uint32_t*>(vmem + hdr.off_graph);
    for (uint32_t id : entry_list)
      out.write((char*)(ng + (size_t)id * R), R * 4);
  }

  {
    std::ofstream out(out_map, std::ios::binary);
    out.write((char*)new_to_old.data(), (size_t)n * 4);
  }

  {
    std::ofstream js(out_json);
    js << "{\n"
       << "  \"n\": " << n << ",\n"
       << "  \"dim\": " << dim << ",\n"
       << "  \"R\": " << R << ",\n"
       << "  \"pq_bytes\": " << pq_bytes << ",\n"
       << "  \"vec_bytes\": " << vec_bytes << ",\n"
       << "  \"entry_id_old\": " << entry_id << ",\n"
       << "  \"entry_id\": " << hdr.entry_id << ",\n"
       << "  \"entry_nodes\": " << hdr.entry_nodes << ",\n"
       << "  \"image_bytes\": " << cursor << ",\n"
       << "  \"off_pq\": " << hdr.off_pq << ",\n"
       << "  \"off_graph\": " << hdr.off_graph << ",\n"
       << "  \"off_vectors\": " << hdr.off_vectors << ",\n"
       << "  \"vmem_dev\": \"" << vmem_dev << "\",\n"
       << "  \"vmem_offset\": " << (long long)vmem_off << ",\n"
       << "  \"out_entry\": \"" << out_entry << "\",\n"
       << "  \"out_map\": \"" << out_map << "\",\n"
       << "  \"reorder\": \"star_pack_bfs\"\n"
       << "}\n";
  }

  munmap(vmem, cursor);
  ::close(vfd);
  munmap(gmap, (size_t)gst.st_size);
  ::close(gfd);
  fprintf(stderr, "DONE bfs layout @ %s off=%lld bytes=%llu\n", vmem_dev.c_str(),
          (long long)vmem_off, (unsigned long long)cursor);
  return 0;
}
