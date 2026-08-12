// A: page-align vectors in-place order (keep IDs) for graph-local id neighborhoods.
// Reads packed CXAN layout from vmem, writes page-strided vectors (4096B/vec) to a new
// vmem offset. Graph/PQ/entry IDs unchanged → no id-map needed for recall.
//
// Build:
//   g++ -O3 -std=c++17 -pthread tools/pack_layout_pagealign.cpp -o tools/pack_layout_pagealign

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
  std::string in_dev, out_dev, out_json;
  off_t in_off = -1, out_off = -1;
  uint64_t in_len = 0;
  uint64_t page_stride = 4096;
  int nthreads = (int)std::thread::hardware_concurrency();
  if (nthreads < 1) nthreads = 8;
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
    else if (a == "--page-stride") page_stride = std::stoull(need());
    else if (a == "--threads") nthreads = std::stoi(need());
    else die("unknown " + a);
  }
  if (in_dev.empty() || out_dev.empty() || in_off < 0 || out_off < 0 || in_len == 0 ||
      out_json.empty())
    die("need --in-vmem-* --out-vmem-* --out-json");

  int ifd = ::open(in_dev.c_str(), O_RDONLY);
  if (ifd < 0) die("open in vmem");
  char* in = (char*)mmap(nullptr, in_len, PROT_READ, MAP_SHARED, ifd, in_off);
  if (in == MAP_FAILED) die("mmap in");
  auto* ih = (const LayoutHeader*)in;
  if (ih->magic != 0x314e415843ull) die("bad magic");
  uint32_t n = ih->n, dim = ih->dim;
  size_t packed = (size_t)dim * ih->vec_bytes;
  if (packed > page_stride) die("packed vec > page_stride");
  fprintf(stderr, "in n=%u dim=%u packed=%zu stride=%llu threads=%d\n", n, dim, packed,
          (unsigned long long)page_stride, nthreads);

  LayoutHeader hdr = *ih;
  const uint64_t page = 4096;
  uint64_t cursor = align_up(sizeof(LayoutHeader), page);
  hdr.off_pq = cursor;
  hdr.len_pq = ih->len_pq;
  cursor = align_up(cursor + hdr.len_pq, page);
  hdr.off_graph = cursor;
  hdr.len_graph = ih->len_graph;
  cursor = align_up(cursor + hdr.len_graph, page);
  hdr.off_vectors = cursor;
  hdr.len_vectors = (uint64_t)n * page_stride;
  cursor = align_up(cursor + hdr.len_vectors, page);
  hdr.off_pivots = cursor;
  hdr.len_pivots = ih->len_pivots ? ih->len_pivots : 64 * 1024;
  cursor = align_up(cursor + hdr.len_pivots, page);
  hdr.checksum = hdr.n ^ hdr.dim ^ hdr.R ^ hdr.len_vectors;
  hdr.pad = (uint32_t)page_stride;
  fprintf(stderr, "out layout_bytes=%llu (%.2f GiB)\n", (unsigned long long)cursor,
          cursor / (1024.0 * 1024 * 1024));

  int ofd = ::open(out_dev.c_str(), O_RDWR);
  if (ofd < 0) die("open out vmem");
  char* out = (char*)mmap(nullptr, cursor, PROT_READ | PROT_WRITE, MAP_SHARED, ofd, out_off);
  if (out == MAP_FAILED) die("mmap out");
  madvise(in, in_len, MADV_SEQUENTIAL);
  madvise(out, cursor, MADV_SEQUENTIAL);

  std::memcpy(out, &hdr, sizeof(hdr));

  // PQ + graph: raw copy (single-threaded; already sequential)
  std::memcpy(out + hdr.off_pq, in + ih->off_pq, (size_t)ih->len_pq);
  fprintf(stderr, "pq copied (%llu B)\n", (unsigned long long)ih->len_pq);
  std::memcpy(out + hdr.off_graph, in + ih->off_graph, (size_t)ih->len_graph);
  fprintf(stderr, "graph copied (%llu B)\n", (unsigned long long)ih->len_graph);

  // Vectors: packed → page-strided. Padding bytes left untouched (search uses packed only).
  const char* src_vec = in + ih->off_vectors;
  char* dst_vec = out + hdr.off_vectors;
  std::atomic<uint32_t> next{0};
  std::atomic<uint32_t> done{0};
  auto worker = [&]() {
    constexpr uint32_t kChunk = 4096;
    for (;;) {
      uint32_t begin = next.fetch_add(kChunk, std::memory_order_relaxed);
      if (begin >= n) break;
      uint32_t end = begin + kChunk;
      if (end > n) end = n;
      for (uint32_t i = begin; i < end; ++i) {
        std::memcpy(dst_vec + (uint64_t)i * page_stride, src_vec + (size_t)i * packed,
                    packed);
      }
      uint32_t d = done.fetch_add(end - begin, std::memory_order_relaxed) + (end - begin);
      if ((d & ((1u << 20) - 1)) < kChunk || d == n)
        fprintf(stderr, "  vectors %u/%u\n", d, n);
    }
  };
  std::vector<std::thread> th;
  th.reserve((size_t)nthreads);
  for (int t = 0; t < nthreads; ++t) th.emplace_back(worker);
  for (auto& t : th) t.join();
  fprintf(stderr, "vectors done\n");

  std::memcpy(out + hdr.off_pivots, in + ih->off_pivots, (size_t)hdr.len_pivots);
  fprintf(stderr, "msync...\n");
  if (msync(out, cursor, MS_SYNC) != 0) perror("msync");

  {
    std::ofstream js(out_json);
    js << "{\n"
       << "  \"n\": " << n << ",\n"
       << "  \"dim\": " << dim << ",\n"
       << "  \"R\": " << hdr.R << ",\n"
       << "  \"vec_bytes\": " << hdr.vec_bytes << ",\n"
       << "  \"page_stride\": " << page_stride << ",\n"
       << "  \"entry_id\": " << hdr.entry_id << ",\n"
       << "  \"entry_nodes\": " << hdr.entry_nodes << ",\n"
       << "  \"image_bytes\": " << cursor << ",\n"
       << "  \"off_vectors\": " << hdr.off_vectors << ",\n"
       << "  \"len_vectors\": " << hdr.len_vectors << ",\n"
       << "  \"vmem_offset\": " << (long long)out_off << ",\n"
       << "  \"reorder\": \"identity_pagealign\",\n"
       << "  \"note\": \"IDs unchanged; vectors 4K-strided to couple id-locality with pages\"\n"
       << "}\n";
  }

  munmap(out, cursor);
  ::close(ofd);
  munmap(in, in_len);
  ::close(ifd);
  fprintf(stderr, "DONE pagealign layout bytes=%llu\n", (unsigned long long)cursor);
  return 0;
}
