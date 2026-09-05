// Isolated VMEM_IOC_PREFETCH_BATCH bandwidth vs /sys/block/*/stat sectors.
#include "serving/vmem_prefetch.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <vector>

static uint64_t one_dev_sectors(const char* name) {
  char path[160];
  snprintf(path, sizeof(path), "/sys/block/%s/stat", name);
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  unsigned long long rio = 0, rm = 0, rsect = 0;
  if (fscanf(f, "%llu %llu %llu", &rio, &rm, &rsect) != 3) rsect = 0;
  fclose(f);
  return (uint64_t)rsect;
}

static uint64_t nvme_read_sectors() {
  uint64_t total = 0;
  FILE* nf = fopen("/sys/class/vmem/vmem0/nvme_dev", "r");
  if (nf) {
    char line[256] = {};
    if (fgets(line, sizeof(line), nf)) {
      char* tok = strtok(line, ", \t\n");
      while (tok) {
        const char* base = strrchr(tok, '/');
        base = base ? base + 1 : tok;
        if (base[0]) total += one_dev_sectors(base);
        tok = strtok(nullptr, ", \t\n");
      }
    }
    fclose(nf);
    if (total) return total;
  }
  return one_dev_sectors("nvme2n1");
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: vmem_bw_probe /dev/vmem0 OFFSET_BYTES MODE(seq|rnd) [NPAGES]\n");
    return 2;
  }
  const char* dev = argv[1];
  const uint64_t off0 = strtoull(argv[2], nullptr, 10);
  const bool rnd = strcmp(argv[3], "rnd") == 0;
  const int np = argc > 4 ? atoi(argv[4]) : 16384;
  int fd = open(dev, O_RDWR);
  if (fd < 0) {
    perror("open");
    return 1;
  }
  VmemIo io;
  io.fd = fd;
  io.image_off = (off_t)off0;
  io.prefetch = true;
  std::vector<uint64_t> pages((size_t)np);
  for (int i = 0; i < np; ++i) {
    if (rnd)
      pages[(size_t)i] = (uint64_t)((i * 1103515245u + 12345u) % 1048576u) * 4096ull;
    else
      pages[(size_t)i] = (uint64_t)i * 4096ull;
  }
  const uint64_t s0 = nvme_read_sectors();
  auto t0 = std::chrono::steady_clock::now();
  int n = vmem_prefetch_pages(io, pages.data(), np, 4096);
  auto t1 = std::chrono::steady_clock::now();
  const uint64_t s1 = nvme_read_sectors();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  const uint64_t nvme_b = s1 >= s0 ? (s1 - s0) * 512ull : 0;
  const uint64_t issued_b = (uint64_t)(n > 0 ? n : 0) * 4096ull;
  printf("probe mode=%s pages=%d issued_B=%llu nvme_B=%llu wall_s=%.4f "
         "issued_GBps=%.3f nvme_GBps=%.3f\n",
         rnd ? "rnd" : "seq", n, (unsigned long long)issued_b,
         (unsigned long long)nvme_b, sec,
         sec > 0 ? issued_b / 1e9 / sec : 0, sec > 0 ? nvme_b / 1e9 / sec : 0);
  close(fd);
  return n < 0 ? 1 : 0;
}
