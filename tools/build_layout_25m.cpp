// Build CXL-ANNS SSD layout image (+ host entry subgraph).
// Supports motivation-dir (meta/vectors/graph) or DiskANN base.bin + raw graph.bin.
//
// Build:
//   g++ -O2 -std=c++17 tools/build_layout_25m.cpp -o tools/build_layout_25m
//
// Example (200k / 1M path):
//   ./tools/build_layout_25m --motivation-dir /mnt/disk0/chukexin_motivation/data/laion1m \
//       --out-image /mnt/disk0/chukexin_motivation/serving_layout.bin \
//       --out-entry /mnt/disk0/chukexin_motivation/serving_entry.bin \
//       --out-json  /root/chukexin/CXL-ANNS-KX/results/layout.json \
//       --pq-bytes 32 --vec-bytes 2 --R 32 --entry-nodes 4096 --max-n 200000

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>
#include <cmath>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#pragma pack(push, 1)
struct LayoutHeader {
  uint64_t magic = 0x314e415843ull;  // CXAN1
  uint32_t version = 1;
  uint32_t n = 0;
  uint32_t dim = 0;
  uint32_t R = 0;
  uint32_t pq_bytes = 0;
  uint32_t vec_bytes = 0;  // 2 or 4
  uint32_t entry_id = 0;
  uint32_t entry_nodes = 0;
  uint32_t pad = 0;
  uint64_t off_pq = 0, len_pq = 0;
  uint64_t off_graph = 0, len_graph = 0;
  uint64_t off_vectors = 0, len_vectors = 0;
  uint64_t off_pivots = 0, len_pivots = 0;
  uint64_t checksum = 0;
};
#pragma pack(pop)

struct MetaMot {
  uint32_t n, dim, degree, entry;
};

static uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) / a * a; }

static void die(const std::string& s) {
  std::cerr << s << "\n";
  std::exit(2);
}

static std::vector<char> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) die("cannot read " + path);
  in.seekg(0, std::ios::end);
  size_t n = (size_t)in.tellg();
  in.seekg(0);
  std::vector<char> b(n);
  in.read(b.data(), n);
  return b;
}

static void write_all(int fd, const void* p, size_t n) {
  const char* c = (const char*)p;
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd, c + off, n - off);
    if (w <= 0) die("write failed");
    off += (size_t)w;
  }
}

// Naive PQ: take first pq_bytes dimensions, scale to uint8 (MVP placeholder).
static void make_pq_u8(const float* v, uint32_t dim, uint8_t* out, uint32_t pq_bytes) {
  for (uint32_t i = 0; i < pq_bytes; ++i) {
    float x = (i < dim) ? v[i] : 0.f;
    int q = (int)std::lround((x * 0.5f + 0.5f) * 255.f);
    if (q < 0) q = 0;
    if (q > 255) q = 255;
    out[i] = (uint8_t)q;
  }
}

static void to_f16_bits(const float* v, uint32_t dim, uint16_t* out) {
  // Prefer F16C if available; fallback: store truncated via bit cast approximation
  // using standard conversion.
  for (uint32_t i = 0; i < dim; ++i) {
    // portable half: use _Float16 if available
#if defined(__FLT16_MAX__)
    _Float16 h = (_Float16)v[i];
    std::memcpy(out + i, &h, 2);
#else
    // crude: store as float32 bytes truncated — should not happen on this Xeon
    union {
      float f;
      uint32_t u;
    } x{v[i]};
    uint32_t sign = (x.u >> 16) & 0x8000;
    int32_t exp = ((x.u >> 23) & 0xff) - 127 + 15;
    uint32_t mant = (x.u >> 13) & 0x3ff;
    uint16_t h;
    if (exp <= 0)
      h = (uint16_t)sign;
    else if (exp >= 31)
      h = (uint16_t)(sign | 0x7c00);
    else
      h = (uint16_t)(sign | (exp << 10) | mant);
    out[i] = h;
#endif
  }
}

int main(int argc, char** argv) {
  std::string mot_dir, base_bin, graph_bin, out_image, out_entry, out_json;
  uint32_t pq_bytes = 32, vec_bytes = 2, R = 32, entry_nodes = 4096;
  uint32_t max_n = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* k) -> std::string {
      if (i + 1 >= argc) die(std::string("missing ") + k);
      return argv[++i];
    };
    if (a == "--motivation-dir") mot_dir = need(a.c_str());
    else if (a == "--base-bin") base_bin = need(a.c_str());
    else if (a == "--graph-bin") graph_bin = need(a.c_str());
    else if (a == "--out-image") out_image = need(a.c_str());
    else if (a == "--out-entry") out_entry = need(a.c_str());
    else if (a == "--out-json") out_json = need(a.c_str());
    else if (a == "--pq-bytes") pq_bytes = (uint32_t)std::stoul(need(a.c_str()));
    else if (a == "--vec-bytes") vec_bytes = (uint32_t)std::stoul(need(a.c_str()));
    else if (a == "--R") R = (uint32_t)std::stoul(need(a.c_str()));
    else if (a == "--entry-nodes") entry_nodes = (uint32_t)std::stoul(need(a.c_str()));
    else if (a == "--max-n") max_n = (uint32_t)std::stoul(need(a.c_str()));
    else die("unknown arg " + a);
  }
  if (out_image.empty() || out_entry.empty() || out_json.empty())
    die("need --out-image --out-entry --out-json");

  uint32_t n = 0, dim = 0, degree = 0, entry_id = 0;
  std::vector<float> vectors_f;
  std::vector<uint32_t> graph;

  if (!mot_dir.empty()) {
    MetaMot m{};
    {
      auto b = read_file(mot_dir + "/meta.bin");
      if (b.size() < sizeof(m)) die("meta.bin short");
      std::memcpy(&m, b.data(), sizeof(m));
    }
    n = m.n;
    dim = m.dim;
    degree = m.degree;
    entry_id = m.entry;
    if (max_n && max_n < n) n = max_n;
    if (R == 0) R = degree;
    if (degree < R) die("graph degree < R");
    vectors_f.resize((size_t)n * dim);
    {
      int fd = ::open((mot_dir + "/vectors.bin").c_str(), O_RDONLY);
      if (fd < 0) die("open vectors");
      size_t needb = (size_t)n * dim * sizeof(float);
      if (::read(fd, vectors_f.data(), needb) != (ssize_t)needb) die("read vectors");
      ::close(fd);
    }
    graph.resize((size_t)n * R);
    {
      int fd = ::open((mot_dir + "/graph.bin").c_str(), O_RDONLY);
      if (fd < 0) die("open graph");
      std::vector<uint32_t> row(degree);
      for (uint32_t i = 0; i < n; ++i) {
        if (::read(fd, row.data(), degree * 4) != (ssize_t)(degree * 4)) die("read graph row");
        for (uint32_t k = 0; k < R; ++k) {
          uint32_t nb = row[k];
          if (nb >= n) nb = i;  // clamp when subsetting
          graph[(size_t)i * R + k] = nb;
        }
      }
      ::close(fd);
    }
  } else {
    if (base_bin.empty() || graph_bin.empty()) die("need --motivation-dir or --base-bin+--graph-bin");
    {
      int fd = ::open(base_bin.c_str(), O_RDONLY);
      if (fd < 0) die("open base");
      if (::read(fd, &n, 4) != 4 || ::read(fd, &dim, 4) != 4) die("base header");
      if (max_n && max_n < n) {
        // still need to read only max_n
      } else {
        max_n = n;
      }
      uint32_t use_n = max_n ? max_n : n;
      vectors_f.resize((size_t)use_n * dim);
      size_t needb = (size_t)use_n * dim * sizeof(float);
      if (::read(fd, vectors_f.data(), needb) != (ssize_t)needb) die("read base vecs");
      ::close(fd);
      n = use_n;
    }
    auto gb = read_file(graph_bin);
    if (gb.size() < (size_t)n * R * 4) die("graph.bin too small for n*R");
    graph.resize((size_t)n * R);
    std::memcpy(graph.data(), gb.data(), graph.size() * 4);
    entry_id = 0;
  }

  if (entry_id >= n) entry_id = 0;
  if (entry_nodes > n) entry_nodes = n;

  // BFS entry closure
  std::vector<uint32_t> entry_list;
  {
    std::unordered_set<uint32_t> seen;
    std::queue<uint32_t> q;
    q.push(entry_id);
    seen.insert(entry_id);
    while (!q.empty() && entry_list.size() < entry_nodes) {
      uint32_t u = q.front();
      q.pop();
      entry_list.push_back(u);
      for (uint32_t k = 0; k < R; ++k) {
        uint32_t v = graph[(size_t)u * R + k];
        if (v < n && !seen.count(v)) {
          seen.insert(v);
          q.push(v);
        }
      }
    }
  }

  LayoutHeader hdr;
  hdr.n = n;
  hdr.dim = dim;
  hdr.R = R;
  hdr.pq_bytes = pq_bytes;
  hdr.vec_bytes = vec_bytes;
  hdr.entry_id = entry_id;
  hdr.entry_nodes = (uint32_t)entry_list.size();

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
  hdr.len_pivots = 64 * 1024;  // placeholder
  cursor = align_up(cursor + hdr.len_pivots, page);
  hdr.checksum = hdr.n ^ hdr.dim ^ hdr.R ^ hdr.len_vectors;

  int fd = ::open(out_image.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) die("open out-image");
  // Write sparse-ish by seeking sections
  write_all(fd, &hdr, sizeof(hdr));

  // PQ
  if (::lseek(fd, (off_t)hdr.off_pq, SEEK_SET) < 0) die("seek pq");
  std::vector<uint8_t> pqrow(pq_bytes);
  for (uint32_t i = 0; i < n; ++i) {
    make_pq_u8(vectors_f.data() + (size_t)i * dim, dim, pqrow.data(), pq_bytes);
    write_all(fd, pqrow.data(), pq_bytes);
  }
  // graph
  if (::lseek(fd, (off_t)hdr.off_graph, SEEK_SET) < 0) die("seek graph");
  write_all(fd, graph.data(), graph.size() * 4);
  // vectors
  if (::lseek(fd, (off_t)hdr.off_vectors, SEEK_SET) < 0) die("seek vectors");
  if (vec_bytes == 4) {
    write_all(fd, vectors_f.data(), vectors_f.size() * 4);
  } else if (vec_bytes == 2) {
    std::vector<uint16_t> row(dim);
    for (uint32_t i = 0; i < n; ++i) {
      to_f16_bits(vectors_f.data() + (size_t)i * dim, dim, row.data());
      write_all(fd, row.data(), dim * 2);
    }
  } else
    die("vec_bytes must be 2 or 4");

  // pivots zero
  if (::lseek(fd, (off_t)hdr.off_pivots, SEEK_SET) < 0) die("seek pivots");
  std::vector<char> z((size_t)hdr.len_pivots, 0);
  write_all(fd, z.data(), z.size());
  // final size
  if (::ftruncate(fd, (off_t)cursor) != 0) die("ftruncate");
  ::close(fd);

  // entry file: list of node ids + their adjacency rows (host-resident)
  {
    std::ofstream out(out_entry, std::ios::binary);
    uint32_t en = (uint32_t)entry_list.size();
    out.write((char*)&en, 4);
    out.write((char*)&R, 4);
    out.write((char*)&entry_id, 4);
    out.write((char*)entry_list.data(), en * 4);
    for (uint32_t id : entry_list) {
      out.write((char*)(graph.data() + (size_t)id * R), R * 4);
    }
  }

  {
    std::ofstream js(out_json);
    js << "{\n"
       << "  \"n\": " << n << ",\n"
       << "  \"dim\": " << dim << ",\n"
       << "  \"R\": " << R << ",\n"
       << "  \"pq_bytes\": " << pq_bytes << ",\n"
       << "  \"vec_bytes\": " << vec_bytes << ",\n"
       << "  \"entry_id\": " << entry_id << ",\n"
       << "  \"entry_nodes\": " << entry_list.size() << ",\n"
       << "  \"image_bytes\": " << cursor << ",\n"
       << "  \"off_pq\": " << hdr.off_pq << ",\n"
       << "  \"off_graph\": " << hdr.off_graph << ",\n"
       << "  \"off_vectors\": " << hdr.off_vectors << ",\n"
       << "  \"out_image\": \"" << out_image << "\",\n"
       << "  \"out_entry\": \"" << out_entry << "\"\n"
       << "}\n";
  }

  std::cerr << "wrote image=" << out_image << " bytes=" << cursor
            << " entry_nodes=" << entry_list.size() << " n=" << n << "\n";
  return 0;
}
