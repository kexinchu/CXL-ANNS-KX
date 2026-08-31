// Write per-node neighbor-vector bundles so one expand is 7 sequential pages
// whose slots are exactly N(u). No extra scoring.
//
//   g++ -O3 -std=c++17 -pthread tools/pack_nbr_bundle.cpp -o tools/pack_nbr_bundle
//
//   ./tools/pack_nbr_bundle --in-file layout_t2i_10m.bin \
//       --graph-file pagebin_graph.bin --id-map new_to_old_pagebin.bin \
//       --out-vmem-dev /dev/vmem0 --out-vmem-offset $((460571635712))

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#pragma pack(push, 1)
struct LayoutHeader {
  uint64_t magic;
  uint32_t version, n, dim, R, pq_bytes, vec_bytes;
  uint32_t entry_id, entry_nodes, pad;
  uint64_t off_pq, len_pq, off_graph, len_graph, off_vectors, len_vectors;
  uint64_t off_pivots, len_pivots, checksum;
};
#pragma pack(pop)

static void die(const std::string& s) {
  fprintf(stderr, "%s\n", s.c_str());
  std::exit(2);
}

int main(int argc, char** argv) {
  setvbuf(stderr, nullptr, _IONBF, 0);
  std::string in_file, graph_file, id_map, out_dev, out_json;
  off_t out_off = -1;
  int nthreads = 8;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&]() -> std::string {
      if (i + 1 >= argc) die("missing");
      return argv[++i];
    };
    if (a == "--in-file") in_file = need();
    else if (a == "--graph-file") graph_file = need();
    else if (a == "--id-map") id_map = need();
    else if (a == "--out-vmem-dev") out_dev = need();
    else if (a == "--out-vmem-offset") out_off = (off_t)std::stoll(need());
    else if (a == "--out-json") out_json = need();
    else if (a == "--threads") nthreads = std::stoi(need());
    else die("unknown " + a);
  }
  if (in_file.empty() || graph_file.empty() || id_map.empty() || out_dev.empty() ||
      out_off < 0)
    die("need --in-file --graph-file --id-map --out-vmem-dev --out-vmem-offset");

  int ifd = ::open(in_file.c_str(), O_RDONLY);
  if (ifd < 0) die("open in");
  struct stat st {};
  if (fstat(ifd, &st) != 0) die("stat in");
  auto* in = (const char*)mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, ifd, 0);
  if (in == MAP_FAILED) die("mmap in");
  auto* ih = (const LayoutHeader*)in;
  if (ih->magic != 0x314e415843ull) die("bad magic");
  uint32_t n = ih->n, R = ih->R;
  size_t packed = (size_t)ih->dim * ih->vec_bytes;
  size_t in_stride = packed;
  if (ih->len_vectors && ih->n) {
    size_t s = (size_t)(ih->len_vectors / ih->n);
    if (s >= packed) in_stride = s;
  }
  uint64_t stride = ((uint64_t)R * packed + 4095ull) & ~4095ull;
  uint64_t bytes = (uint64_t)n * stride;
  fprintf(stderr, "n=%u R=%u packed=%zu bundle_stride=%llu bytes=%.2f GiB\n", n, R, packed,
          (unsigned long long)stride, bytes / (1024.0 * 1024 * 1024));

  std::vector<uint32_t> new_to_old(n);
  {
    FILE* f = fopen(id_map.c_str(), "rb");
    if (!f) die("open id-map");
    if (fread(new_to_old.data(), 4, n, f) != n) die("read id-map");
    fclose(f);
  }
  int gfd = ::open(graph_file.c_str(), O_RDONLY);
  if (gfd < 0) die("open graph");
  struct stat gst {};
  if (fstat(gfd, &gst) != 0) die("stat graph");
  if ((size_t)gst.st_size < (size_t)n * R * 4) die("graph short");
  auto* graph = (const uint32_t*)mmap(nullptr, (size_t)gst.st_size, PROT_READ, MAP_SHARED, gfd, 0);
  if (graph == MAP_FAILED) die("mmap graph");

  int ofd = ::open(out_dev.c_str(), O_RDWR);
  if (ofd < 0) die("open vmem");
  char* out = (char*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, ofd, out_off);
  if (out == MAP_FAILED) die("mmap out vmem");
  madvise(out, bytes, MADV_SEQUENTIAL);

  const char* vecs = in + ih->off_vectors;
  std::atomic<uint32_t> next{0};
  std::atomic<uint64_t> done{0};
  auto worker = [&]() {
    constexpr uint32_t kChunk = 512;
    for (;;) {
      uint32_t b = next.fetch_add(kChunk, std::memory_order_relaxed);
      if (b >= n) break;
      uint32_t e = b + kChunk;
      if (e > n) e = n;
      for (uint32_t u = b; u < e; ++u) {
        char* dst = out + (uint64_t)u * stride;
        const uint32_t* nbr = graph + (size_t)u * R;
        for (uint32_t k = 0; k < R; ++k) {
          uint32_t nv = nbr[k];
          uint32_t ov = (nv < n) ? new_to_old[nv] : 0;
          if (ov >= n) ov = 0;
          std::memcpy(dst + (size_t)k * packed, vecs + (uint64_t)ov * in_stride, packed);
        }
        if ((uint32_t)(R * packed) < stride)
          std::memset(dst + (size_t)R * packed, 0, (size_t)(stride - R * packed));
      }
      uint64_t d = done.fetch_add(e - b, std::memory_order_relaxed) + (e - b);
      if ((d & ((1u << 16) - 1)) < kChunk || d == n)
        fprintf(stderr, "  bundle %llu/%u (%.1f%%)\n", (unsigned long long)d, n,
                100.0 * d / n);
    }
  };
  std::vector<std::thread> th;
  for (int t = 0; t < nthreads; ++t) th.emplace_back(worker);
  for (auto& t : th) t.join();
  fprintf(stderr, "msync bundle...\n");
  msync(out, bytes, MS_SYNC);
  if (!out_json.empty()) {
    FILE* js = fopen(out_json.c_str(), "w");
    fprintf(js,
            "{\n  \"n\": %u,\n  \"R\": %u,\n  \"packed\": %zu,\n  \"stride\": %llu,\n  "
            "\"bytes\": %llu,\n  \"vmem_offset\": %lld\n}\n",
            n, R, packed, (unsigned long long)stride, (unsigned long long)bytes,
            (long long)out_off);
    fclose(js);
  }
  munmap(out, bytes);
  ::close(ofd);
  munmap((void*)graph, (size_t)gst.st_size);
  ::close(gfd);
  munmap((void*)in, (size_t)st.st_size);
  ::close(ifd);
  fprintf(stderr, "DONE nbr-bundle bytes=%llu off=%lld\n", (unsigned long long)bytes,
          (long long)out_off);
  return 0;
}
