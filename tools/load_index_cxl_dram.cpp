// Map DiskANN-style base.bin (+ optional memory index files) into CXL-DRAM (mbind node1)
// and run a tiny search smoke using DiskANN search_memory_index via system(), OR
// just verify residency / touch bandwidth.
//
// Build: g++ -O2 -std=c++17 tools/load_index_cxl_dram.cpp -o tools/load_index_cxl_dram -lnuma
//
// Usage:
//   ./tools/load_index_cxl_dram --base /path/base.bin --index-prefix /path/mem_R32 \
//       --touch --hold-s 5

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void die(const char* s) {
  perror(s);
  std::exit(2);
}

static void* map_file(const char* path, size_t* len, int prot) {
  int fd = open(path, (prot & PROT_WRITE) ? O_RDWR : O_RDONLY);
  if (fd < 0) die(path);
  struct stat st {};
  if (fstat(fd, &st) != 0) die("fstat");
  int flags = (prot & PROT_WRITE) ? MAP_SHARED : MAP_PRIVATE;
  void* p = mmap(nullptr, st.st_size, prot, flags, fd, 0);
  if (p == MAP_FAILED) die("mmap");
  close(fd);
  *len = (size_t)st.st_size;
  return p;
}

static void* alloc_cxl(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) die("mmap anon");
  unsigned long nodemask = 1UL << 1;
  if (mbind(p, bytes, MPOL_BIND, &nodemask, sizeof(nodemask) * 8,
            MPOL_MF_MOVE | MPOL_MF_STRICT) != 0) {
    perror("mbind node1 STRICT failed");
    exit(2);
  }
  return p;
}

int main(int argc, char** argv) {
  const char* base_path = nullptr;
  const char* index_prefix = nullptr;
  int touch = 0;
  int hold_s = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) exit(2);
      return argv[++i];
    };
    if (a == "--base") base_path = need();
    else if (a == "--index-prefix") index_prefix = need();
    else if (a == "--touch") touch = 1;
    else if (a == "--hold-s") hold_s = atoi(need());
    else {
      fprintf(stderr, "unknown %s\n", a.c_str());
      return 2;
    }
  }
  if (!base_path) {
    fprintf(stderr, "need --base\n");
    return 2;
  }
  if (numa_available() < 0) {
    fprintf(stderr, "libnuma required\n");
    return 2;
  }

  size_t base_len = 0;
  void* base_file = map_file(base_path, &base_len, PROT_READ);
  auto* hdr = (uint32_t*)base_file;
  uint32_t n = hdr[0], dim = hdr[1];
  printf("base file n=%u dim=%u bytes=%zu\n", n, dim, base_len);

  // Copy into CXL-DRAM arena (true residency on node1)
  void* cxl_base = alloc_cxl(base_len);
  printf("copying base -> CXL-DRAM %p ...\n", cxl_base);
  memcpy(cxl_base, base_file, base_len);
  munmap(base_file, base_len);
  printf("base resident on CXL-DRAM\n");

  std::vector<std::pair<void*, size_t>> held;
  held.push_back({cxl_base, base_len});

  if (index_prefix) {
    // DiskANN memory index typically writes: <prefix> (data) and related files.
    // Copy any existing prefix* files into CXL-DRAM.
    std::vector<std::string> cands = {
        std::string(index_prefix),
        std::string(index_prefix) + "_mem.index",
        std::string(index_prefix) + ".data",
        std::string(index_prefix) + "_vectors.bin",
    };
    // also scan dirname for prefix*
    for (const auto& path : cands) {
      struct stat st {};
      if (stat(path.c_str(), &st) != 0) continue;
      if (!S_ISREG(st.st_mode)) continue;
      size_t len = 0;
      void* f = map_file(path.c_str(), &len, PROT_READ);
      void* cxl = alloc_cxl(len);
      printf("copy index file %s (%zu bytes) -> CXL-DRAM\n", path.c_str(), len);
      memcpy(cxl, f, len);
      munmap(f, len);
      held.push_back({cxl, len});
    }
  }

  if (touch) {
    volatile uint64_t sink = 0;
    for (auto& h : held) {
      auto* b = (const uint8_t*)h.first;
      for (size_t off = 0; off < h.second; off += 4096) sink += b[off];
    }
    printf("touch checksum=%llu\n", (unsigned long long)sink);
  }

  // Report node1 free after placement
  FILE* f = popen("numactl -H | head -12", "r");
  if (f) {
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) fputs(buf, stdout);
    pclose(f);
  }

  if (hold_s > 0) {
    printf("holding %d s (arena remains mapped)\n", hold_s);
    sleep(hold_s);
  }

  size_t total = 0;
  for (auto& h : held) {
    total += h.second;
    munmap(h.first, h.second);
  }
  printf("released %.2f GiB from CXL-DRAM\n", total / (1024.0 * 1024 * 1024));
  return 0;
}
