// Sample 10k nodes (seed=42) from a version-2 DiskANN image (file or dax).
#include "serving/placement.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <string>

static void die(const char* m) {
  perror(m);
  std::exit(1);
}

int main(int argc, char** argv) {
  const char* src = nullptr;
  const char* out = nullptr;
  uint32_t n0 = 10000;
  int seed = 42;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--src") src = argv[++i];
    else if (a == "--out") out = argv[++i];
    else if (a == "--n0") n0 = (uint32_t)atoi(argv[++i]);
    else if (a == "--seed") seed = atoi(argv[++i]);
    else {
      fprintf(stderr, "unknown %s\n", a.c_str());
      return 2;
    }
  }
  if (!src || !out) {
    fprintf(stderr, "usage: build_nav_graph --src diskann.bin|/dev/dax0.0 --out nav_10k.bin\n");
    return 2;
  }
  int fd = open(src, O_RDONLY);
  if (fd < 0) die("open src");
  struct stat st{};
  if (fstat(fd, &st) != 0) die("stat src");
  size_t map_len = (size_t)st.st_size;
  if (map_len == 0) {
    FILE* sf = fopen("/sys/bus/dax/devices/dax0.0/size", "r");
    unsigned long long ds = 0;
    if (sf && fscanf(sf, "%llu", &ds) == 1 && ds > 0) map_len = (size_t)ds;
    if (sf) fclose(sf);
    if (map_len == 0) map_len = 32ull << 30;
  }
  void* m = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) die("mmap src");
  auto* hdr = reinterpret_cast<const CxanLayoutHeader*>(m);
  if (hdr->magic != kCxanMagic || hdr->n == 0) {
    fprintf(stderr, "bad src header\n");
    return 1;
  }
  Placement pl;
  pl.set_header(hdr);
  pl.ssd_base = static_cast<const uint8_t*>(m);
  if (!pl.diskann_layout) {
    fprintf(stderr, "src is not diskann version-2\n");
    return 1;
  }
  if (n0 > hdr->n) n0 = hdr->n;
  std::vector<uint32_t> all(hdr->n);
  for (uint32_t i = 0; i < hdr->n; ++i) all[i] = i;
  std::mt19937 rng((uint32_t)seed);
  std::shuffle(all.begin(), all.end(), rng);
  all.resize(n0);
  std::sort(all.begin(), all.end());
  const size_t payload = Placement::diskann_payload_bytes(hdr->dim, hdr->vec_bytes, hdr->R);
  FILE* f = fopen(out, "wb");
  if (!f) die("fopen out");
  fwrite(&n0, 4, 1, f);
  fwrite(&hdr->dim, 4, 1, f);
  fwrite(&hdr->R, 4, 1, f);
  fwrite(all.data(), 4, n0, f);
  std::vector<uint8_t> rec(payload);
  for (uint32_t i = 0; i < n0; ++i) {
    const uint8_t* e = pl.entry(all[i]);
    std::memcpy(rec.data(), e, payload);
    fwrite(rec.data(), 1, payload, f);
  }
  fclose(f);
  munmap(m, map_len);
  close(fd);
  fprintf(stderr, "OK nav n0=%u seed=%d out=%s\n", n0, seed, out);
  return 0;
}
