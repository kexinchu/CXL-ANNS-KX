// Permute each N(u) by decreasing in-degree so late expands' leftover
// unseen neighbors share the last bundle pages (no extra scores).
//
//   g++ -O3 -std=c++17 tools/reorder_graph_degree.cpp -o tools/reorder_graph_degree
//   ./tools/reorder_graph_degree --in pagebin_graph.bin --out pagebin_graph_deg.bin --n 10000000 --R 32

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void die(const char* s) {
  fprintf(stderr, "%s\n", s);
  std::exit(2);
}

int main(int argc, char** argv) {
  const char* in_path = nullptr;
  const char* out_path = nullptr;
  uint32_t n = 0, R = 32;
  for (int i = 1; i < argc; ++i) {
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) die("missing");
      return argv[++i];
    };
    std::string a = argv[i];
    if (a == "--in") in_path = need();
    else if (a == "--out") out_path = need();
    else if (a == "--n") n = (uint32_t)atoi(need());
    else if (a == "--R") R = (uint32_t)atoi(need());
    else die("unknown");
  }
  if (!in_path || !out_path || !n || !R) die("need --in --out --n --R");

  int fd = ::open(in_path, O_RDONLY);
  if (fd < 0) die("open in");
  struct stat st {};
  if (fstat(fd, &st) != 0) die("stat");
  size_t need = (size_t)n * R * 4;
  if ((size_t)st.st_size < need) die("graph short");
  auto* g = (const uint32_t*)mmap(nullptr, need, PROT_READ, MAP_SHARED, fd, 0);
  if (g == MAP_FAILED) die("mmap in");

  std::vector<uint32_t> deg(n, 0);
  for (uint64_t i = 0; i < (uint64_t)n * R; ++i) {
    uint32_t v = g[i];
    if (v < n) deg[v]++;
  }

  FILE* out = fopen(out_path, "wb");
  if (!out) die("open out");
  std::vector<uint32_t> row(R);
  std::vector<uint32_t> ord(R);
  uint64_t moved = 0;
  for (uint32_t u = 0; u < n; ++u) {
    const uint32_t* src = g + (size_t)u * R;
    for (uint32_t k = 0; k < R; ++k) ord[k] = k;
    std::stable_sort(ord.begin(), ord.end(), [&](uint32_t a, uint32_t b) {
      uint32_t da = src[a] < n ? deg[src[a]] : 0;
      uint32_t db = src[b] < n ? deg[src[b]] : 0;
      if (da != db) return da > db;
      return src[a] < src[b];
    });
    bool ch = false;
    for (uint32_t k = 0; k < R; ++k) {
      row[k] = src[ord[k]];
      if (ord[k] != k) ch = true;
    }
    if (ch) moved++;
    if (fwrite(row.data(), 4, R, out) != R) die("write");
  }
  fclose(out);
  munmap((void*)g, need);
  ::close(fd);
  fprintf(stderr, "deg-reorder n=%u R=%u rows_changed=%llu out=%s\n", n, R,
          (unsigned long long)moved, out_path);
  return 0;
}
