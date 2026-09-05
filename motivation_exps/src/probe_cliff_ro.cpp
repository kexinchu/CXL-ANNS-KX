// Read-only cliff + blocking-QD probe on the current /dev/vmem0.
// Does not populate, does not check the July d8 identity gate, does not write.
// Safe to run on the T2I-staged hide disk.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common.hpp"
#include "shell.h"
#include "vmem_cxl.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static uint64_t sysu64(const char* p) {
  return read_sysfs_u64(p);
}

static void print_identity() {
  std::printf("# probe-cliff-ro backend=%s bdf=%s nvme=%s cache_used=%llu "
              "cache_limit=%llu\n",
              read_sysfs_text("/sys/class/vmem/vmem0/backend").c_str(),
              read_sysfs_text("/sys/class/vmem/vmem0/target_bdf").c_str(),
              read_sysfs_text("/sys/class/vmem/vmem0/nvme_dev").c_str(),
              (unsigned long long)sysu64("/sys/class/vmem/vmem0/cache_used"),
              (unsigned long long)sysu64("/sys/class/vmem/vmem0/cache_limit"));
}

int main() {
  print_identity();

  int fd = ::open("/dev/vmem0", O_RDONLY);
  if (fd < 0) die("open /dev/vmem0 O_RDONLY");

  vmem_info info {};
  if (ioctl(fd, VMEM_IOC_GET_INFO, &info) < 0) die("GET_INFO");
  vmem_layout_info layout {};
  if (ioctl(fd, VMEM_IOC_GET_LAYOUT, &layout) < 0) die("GET_LAYOUT");
  if (layout.abi_version != VMEM_LAYOUT_ABI_VERSION)
    die("layout ABI mismatch");

  const size_t page = size_t(::sysconf(_SC_PAGESIZE));
  const size_t vec_bytes = 800;  // T2I record size; LAION is 4096
  const int nprobe = 800;
  // High logical offset: SSD-backed, away from any 64MiB motivation header.
  uint64_t base = 128ull << 30;
  base = (base + page - 1) & ~(uint64_t)(page - 1);
  const uint64_t map_bytes = 32ull << 20;
  if (base + map_bytes > layout.logical_size) die("map exceeds logical");

  void* map = ::mmap(nullptr, map_bytes, PROT_READ, MAP_SHARED, fd, (off_t)base);
  if (map == MAP_FAILED) die("mmap failed");

  std::vector<uint64_t> offs(nprobe);
  for (int i = 0; i < nprobe; ++i)
    offs[i] = (uint64_t(i) * 40961ull) % (map_bytes - vec_bytes);

  auto touch = [&](uint64_t off) {
    const volatile unsigned char* p =
        static_cast<unsigned char*>(map) + off;
    unsigned s = 0;
    for (size_t i = 0; i < vec_bytes; i += 64) s += p[i];
    s += p[vec_bytes - 1];
    return s;
  };

  auto measure = [&](const char* tag, bool record_gaps) {
    auto f0 = sysu64("/sys/class/vmem/vmem0/faults");
    auto r0 = sysu64("/sys/class/vmem/vmem0/read_ios");
    std::vector<double> gaps;
    gaps.reserve(nprobe);
    unsigned sink = 0;
    Timer t;
    double prev = now_sec();
    for (int i = 0; i < nprobe; ++i) {
      sink += touch(offs[i]);
      double now = now_sec();
      if (record_gaps) gaps.push_back((now - prev) * 1e9);
      prev = now;
    }
    double sec = t.elapsed();
    auto f1 = sysu64("/sys/class/vmem/vmem0/faults");
    auto r1 = sysu64("/sys/class/vmem/vmem0/read_ios");
    double ns = sec * 1e9 / nprobe;
    std::printf("[%s] n=%d bytes=%zu ns/load=%.1f Δfaults=%llu Δread_ios=%llu "
                "time=%.4fs sink=%u\n",
                tag, nprobe, vec_bytes, ns,
                (unsigned long long)(f1 - f0),
                (unsigned long long)(r1 - r0), sec, sink);
    if (record_gaps && !gaps.empty()) {
      std::sort(gaps.begin(), gaps.end());
      double mean = 0;
      for (double g : gaps) mean += g;
      mean /= double(gaps.size());
      std::printf("[QD] blocking_memcpy max_in_flight=1 mean_gap_ns=%.1f "
                  "p50=%.1f p90=%.1f (serial by construction)\n",
                  mean, gaps[gaps.size() / 2],
                  gaps[size_t(0.9 * (gaps.size() - 1))]);
    }
  };

  auto drop_ptes = [&]() {
    if (::madvise(map, map_bytes, MADV_DONTNEED) != 0)
      std::perror("madvise(MADV_DONTNEED)");
  };

  auto thrash = [&]() {
    uint64_t start = base + map_bytes + (2ull << 20);
    start = (start + page - 1) & ~(uint64_t)(page - 1);
    const uint64_t chunk = 16ull << 20;
    const uint64_t want = 5ull << 30;
    uint64_t touched = 0;
    while (touched < want && start + chunk < layout.logical_size) {
      void* p = ::mmap(nullptr, chunk, PROT_READ, MAP_SHARED, fd, (off_t)start);
      if (p == MAP_FAILED) break;
      for (uint64_t o = 0; o < chunk; o += page) {
        volatile unsigned char x = *(static_cast<unsigned char*>(p) + o);
        (void)x;
      }
      ::munmap(p, chunk);
      start += chunk;
      touched += chunk;
      if ((touched & ((512ull << 20) - 1)) == 0)
        std::printf("  thrash progress %llu MB\n",
                    (unsigned long long)(touched >> 20));
    }
    std::printf("thrashed soft-cache touch≈%llu MB\n",
                (unsigned long long)(touched >> 20));
  };

  std::printf("# VMEM-CLIFF-RO 800B loads at logical 0x%llx (no write)\n",
              (unsigned long long)base);

  drop_ptes();
  thrash();
  drop_ptes();
  measure("RO-cold_ssd", true);

  for (int i = 0; i < nprobe; ++i) touch(offs[i]);
  measure("RO-warm_pte", false);

  drop_ptes();
  measure("RO-softcache_pte_cold", false);

  thrash();
  drop_ptes();
  measure("RO-after_thrash", false);

  ::munmap(map, map_bytes);
  ::close(fd);
  return 0;
}
