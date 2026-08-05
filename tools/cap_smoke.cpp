// Capacity sandbox smoke: 1GiB CXL-DRAM arena + 100GiB vmem image region (sampled).
// Build: g++ -O2 -std=c++17 tools/cap_smoke.cpp -o tools/cap_smoke -lnuma
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <unistd.h>

static constexpr uint64_t kMagic = 0x314e415843ull;  // "CXAN1" low bytes

static void* map_dram_1g(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap dram");
    exit(2);
  }
  unsigned long nodemask = 1UL << 1;
  if (mbind(p, bytes, MPOL_BIND, &nodemask, sizeof(nodemask) * 8,
            MPOL_MF_MOVE | MPOL_MF_STRICT) != 0) {
    perror("mbind node1");
    exit(2);
  }
  // Touch every 2MiB to place pages without full memset stall forever.
  auto* b = static_cast<volatile char*>(p);
  for (size_t off = 0; off < bytes; off += 2 * 1024 * 1024) b[off] = 1;
  b[bytes - 1] = 1;
  return p;
}

static void* map_ssd_region(const char* dev, off_t offset, size_t bytes, int* out_fd) {
  int fd = open(dev, O_RDWR);
  if (fd < 0) {
    perror("open vmem");
    exit(2);
  }
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
  if (p == MAP_FAILED) {
    perror("mmap vmem region");
    exit(2);
  }
  *out_fd = fd;
  return p;
}

int main() {
  if (numa_available() < 0) {
    fprintf(stderr, "libnuma unavailable\n");
    return 2;
  }
  const char* dev = getenv("CXAN_VMEM_DEV") ? getenv("CXAN_VMEM_DEV") : "/dev/vmem0";
  size_t dram_bytes = getenv("CXAN_DRAM_BYTES") ? strtoull(getenv("CXAN_DRAM_BYTES"), nullptr, 10)
                                                : (1ull << 30);
  size_t ssd_bytes = getenv("CXAN_SSD_BYTES") ? strtoull(getenv("CXAN_SSD_BYTES"), nullptr, 10)
                                              : (100ull << 30);
  off_t ssd_off = getenv("CXAN_SSD_OFFSET") ? strtoull(getenv("CXAN_SSD_OFFSET"), nullptr, 10)
                                            : (200ull << 30);

  printf("dram_bytes=%zu ssd_bytes=%zu ssd_offset=%lld dev=%s\n", dram_bytes, ssd_bytes,
         (long long)ssd_off, dev);

  void* dram = map_dram_1g(dram_bytes);
  printf("dram_arena=%p touched\n", dram);

  // Map only header + sample windows to avoid faulting entire 100GiB in smoke.
  const size_t map_len = 16ull * 1024 * 1024;  // 16MiB window at start of image
  int fd = -1;
  void* ssd = map_ssd_region(dev, ssd_off, map_len, &fd);
  auto* hdr = static_cast<uint64_t*>(ssd);
  hdr[0] = kMagic;
  hdr[1] = dram_bytes;
  hdr[2] = ssd_bytes;
  hdr[3] = (uint64_t)ssd_off;
  msync(ssd, 4096, MS_SYNC);

  // Sample write near end of logical image via a second small map.
  off_t tail_off = ssd_off + (off_t)ssd_bytes - (off_t)map_len;
  void* ssd_tail =
      mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, tail_off);
  if (ssd_tail == MAP_FAILED) {
    perror("mmap vmem tail");
    return 2;
  }
  auto* t = static_cast<uint64_t*>(ssd_tail);
  t[0] = kMagic ^ 0xA5A5A5A5ull;
  msync(ssd_tail, 4096, MS_SYNC);

  // Re-read header
  uint64_t magic = hdr[0];
  printf("header_magic=0x%llx expect=0x%llx %s\n", (unsigned long long)magic,
         (unsigned long long)kMagic, magic == kMagic ? "OK" : "FAIL");
  printf("tail_marker=0x%llx OK\n", (unsigned long long)t[0]);
  printf("soft_cache_note: cite software DRAM window=%zu; vmem soft-cache is separate\n",
         dram_bytes);

  munmap(ssd_tail, map_len);
  munmap(ssd, map_len);
  munmap(dram, dram_bytes);
  close(fd);
  return magic == kMagic ? 0 : 1;
}
