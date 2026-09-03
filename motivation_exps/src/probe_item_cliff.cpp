// One-item latency: Host load from CXL-DRAM (dax0.0) vs CXL-SSD cache miss
// (vmem0 NAND fill). Read-only on vmem. Does not populate or restage T2I.
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
#include <x86intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

static constexpr uint64_t kCxanMagic = 0x314e415843ull;
static constexpr uint64_t kT2IOff = 450971566080ull;  // 420 GiB pagebin

struct ItemFile {
  const char* name;
  const char* path;
  uint32_t dim;
  uint32_t n_use;
  size_t header;
  size_t item;
};

static uint64_t nsec() {
  timespec ts {};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

static void clflush_item(void* p, size_t n) {
  auto* c = static_cast<unsigned char*>(p);
  for (size_t o = 0; o < n; o += 64) _mm_clflushopt(c + o);
  _mm_sfence();
}

static unsigned load_item(const void* p, size_t n) {
  const auto* c = static_cast<const unsigned char*>(p);
  unsigned s = 0;
  for (size_t o = 0; o < n; o += 64) s += c[o];
  if (n) s += c[n - 1];
  return s;
}

static void stats(const char* tag, const std::vector<double>& ns, unsigned sink,
                  uint64_t df, uint64_t dr) {
  if (ns.empty()) return;
  std::vector<double> v = ns;
  std::sort(v.begin(), v.end());
  double mean = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
  auto pct = [&](double p) {
    size_t i = size_t(p * (v.size() - 1));
    return v[i];
  };
  std::printf("[%s] n=%zu mean_ns=%.1f p50=%.1f p90=%.1f p99=%.1f "
              "Δfaults=%llu Δread_ios=%llu sink=%u\n",
              tag, v.size(), mean, pct(0.50), pct(0.90), pct(0.99),
              (unsigned long long)df, (unsigned long long)dr, sink);
}

static std::vector<uint32_t> pick_ids(uint32_t n, int k, uint32_t seed) {
  std::vector<uint32_t> ids(k);
  uint64_t x = seed * 6364136223846793005ull + 1;
  for (int i = 0; i < k; ++i) {
    x = x * 6364136223846793005ull + 1;
    ids[i] = uint32_t((x >> 33) % n);
  }
  return ids;
}

static void* mmap_rw(int fd, size_t bytes, off_t off, int prot) {
  void* p = ::mmap(nullptr, bytes, prot, MAP_SHARED, fd, off);
  if (p == MAP_FAILED) die("mmap failed");
  return p;
}

static void drop_ptes(void* p, size_t n) {
  if (p && ::madvise(p, n, MADV_DONTNEED) != 0) std::perror("madvise");
}

static void thrash_vmem(int fd, uint64_t avoid_lo, uint64_t avoid_hi,
                        uint64_t logical_size, size_t page) {
  uint64_t start = 128ull << 30;
  if (start >= avoid_lo && start < avoid_hi) start = avoid_hi + (2ull << 20);
  start = (start + page - 1) & ~(uint64_t)(page - 1);
  const uint64_t chunk = 16ull << 20;
  const uint64_t want = 5ull << 30;
  uint64_t touched = 0;
  while (touched < want && start + chunk < logical_size) {
    if (start + chunk > avoid_lo && start < avoid_hi) {
      start = avoid_hi + (2ull << 20);
      continue;
    }
    void* p = ::mmap(nullptr, chunk, PROT_READ, MAP_SHARED, fd, (off_t)start);
    if (p == MAP_FAILED) break;
    for (uint64_t o = 0; o < chunk; o += page)
      (void)*(static_cast<volatile unsigned char*>(p) + o);
    ::munmap(p, chunk);
    start += chunk;
    touched += chunk;
    if ((touched & ((512ull << 20) - 1)) == 0)
      std::printf("  thrash %llu MB\n", (unsigned long long)(touched >> 20));
  }
  std::printf("thrashed ≈%llu MB\n", (unsigned long long)(touched >> 20));
}

static void bench_dax(int dax_fd, const ItemFile& ds, const std::vector<uint32_t>& ids) {
  const size_t align = 2ull << 20;
  const size_t slot = (ds.item + 63) & ~size_t(63);
  const size_t need = (ids.size() * slot + align - 1) & ~(align - 1);
  void* dax = mmap_rw(dax_fd, need, 0, PROT_READ | PROT_WRITE);
  int host = ::open(ds.path, O_RDONLY);
  if (host < 0) die(std::string("open ") + ds.path);
  std::vector<char> buf(ds.item);
  for (size_t i = 0; i < ids.size(); ++i) {
    off_t off = off_t(ds.header + uint64_t(ids[i]) * ds.item);
    if (::pread(host, buf.data(), ds.item, off) != ssize_t(ds.item))
      die("pread item");
    std::memcpy(static_cast<char*>(dax) + i * slot, buf.data(), ds.item);
  }
  ::close(host);

  // Uncached CXL.mem: flush the item, then load it.
  std::vector<double> ns;
  ns.reserve(ids.size());
  unsigned sink = 0;
  for (size_t i = 0; i < ids.size(); ++i) {
    void* p = static_cast<char*>(dax) + i * slot;
    clflush_item(p, ds.item);
    uint64_t t0 = nsec();
    sink += load_item(p, ds.item);
    uint64_t t1 = nsec();
    ns.push_back(double(t1 - t0));
  }
  stats((std::string(ds.name) + "/CXL-DRAM").c_str(), ns, sink, 0, 0);
  ::munmap(dax, need);
}

static void bench_vmem_t2i(int vmem_fd, const ItemFile& ds,
                           const std::vector<uint32_t>& ids,
                           uint64_t logical_size, size_t page) {
  const size_t map_bytes = 10ull << 30;
  void* img = mmap_rw(vmem_fd, map_bytes, off_t(kT2IOff), PROT_READ);
  uint64_t magic = 0;
  std::memcpy(&magic, img, 8);
  if (magic != kCxanMagic) die("T2I CXAN1 magic missing");
  uint64_t off_vectors = 0;
  std::memcpy(&off_vectors, static_cast<char*>(img) + 76, 8);
  std::printf("# T2I vmem magic=CXAN1 off_vectors=%llu\n",
              (unsigned long long)off_vectors);

  drop_ptes(img, map_bytes);
  thrash_vmem(vmem_fd, kT2IOff, kT2IOff + map_bytes, logical_size, page);
  drop_ptes(img, map_bytes);

  auto f0 = read_sysfs_u64("/sys/class/vmem/vmem0/faults");
  auto r0 = read_sysfs_u64("/sys/class/vmem/vmem0/read_ios");
  std::vector<double> ns;
  ns.reserve(ids.size());
  unsigned sink = 0;
  for (uint32_t id : ids) {
    const void* p = static_cast<char*>(img) + off_vectors + uint64_t(id) * ds.item;
    uint64_t t0 = nsec();
    sink += load_item(p, ds.item);
    uint64_t t1 = nsec();
    ns.push_back(double(t1 - t0));
  }
  auto f1 = read_sysfs_u64("/sys/class/vmem/vmem0/faults");
  auto r1 = read_sysfs_u64("/sys/class/vmem/vmem0/read_ios");
  stats((std::string(ds.name) + "/CXL-SSD-miss").c_str(), ns, sink, f1 - f0,
        r1 - r0);
  ::munmap(img, map_bytes);
}

static void bench_vmem_sized(int vmem_fd, const ItemFile& ds,
                             const std::vector<uint32_t>& ids,
                             uint64_t logical_size, size_t page) {
  // LAION is not staged on this SSD. Match the item size on the same
  // NAND image as T2I (read-only) so the miss is a real fill, not a
  // device-DRAM cache hit on unused addresses.
  const size_t map_bytes = 10ull << 30;
  void* img = mmap_rw(vmem_fd, map_bytes, off_t(kT2IOff), PROT_READ);
  uint64_t off_vectors = 0;
  std::memcpy(&off_vectors, static_cast<char*>(img) + 76, 8);
  drop_ptes(img, map_bytes);
  thrash_vmem(vmem_fd, kT2IOff, kT2IOff + map_bytes, logical_size, page);
  drop_ptes(img, map_bytes);

  auto f0 = read_sysfs_u64("/sys/class/vmem/vmem0/faults");
  auto r0 = read_sysfs_u64("/sys/class/vmem/vmem0/read_ios");
  std::vector<double> ns;
  ns.reserve(ids.size());
  unsigned sink = 0;
  const uint64_t vec_bytes = 8000000000ull;
  for (uint32_t id : ids) {
    uint64_t rel = (uint64_t(id) * ds.item) % (vec_bytes - ds.item);
    rel &= ~uint64_t(63);
    const void* p = static_cast<char*>(img) + off_vectors + rel;
    uint64_t t0 = nsec();
    sink += load_item(p, ds.item);
    uint64_t t1 = nsec();
    ns.push_back(double(t1 - t0));
  }
  auto f1 = read_sysfs_u64("/sys/class/vmem/vmem0/faults");
  auto r1 = read_sysfs_u64("/sys/class/vmem/vmem0/read_ios");
  stats((std::string(ds.name) + "/CXL-SSD-miss").c_str(), ns, sink, f1 - f0,
        r1 - r0);
  ::munmap(img, map_bytes);
}

int main() {
  std::printf("# probe-item-cliff dax=/dev/dax0.0 vmem=/dev/vmem0 "
              "bdf=%s nvme=%s\n",
              read_sysfs_text("/sys/class/vmem/vmem0/target_bdf").c_str(),
              read_sysfs_text("/sys/class/vmem/vmem0/nvme_dev").c_str());

  int dax = ::open("/dev/dax0.0", O_RDWR);
  if (dax < 0) die("open /dev/dax0.0");
  int vmem = ::open("/dev/vmem0", O_RDWR);
  if (vmem < 0) die("open /dev/vmem0");

  vmem_info info {};
  if (ioctl(vmem, VMEM_IOC_GET_INFO, &info) < 0) die("GET_INFO");
  vmem_layout_info layout {};
  if (ioctl(vmem, VMEM_IOC_GET_LAYOUT, &layout) < 0) die("GET_LAYOUT");
  vmem_prefetch_cfg cfg { VMEM_PREFETCH_OFF, 0 };
  ioctl(vmem, VMEM_IOC_SET_PREFETCH, &cfg);
  const size_t page = size_t(::sysconf(_SC_PAGESIZE));
  const int nprobe = 400;

  ItemFile t2i {
      "T2I-10M",
      "/mnt/disk0/chukexin_motivation/data/text2image_10m/base.10M.fbin",
      200, 10000000, 8, 800};
  ItemFile laion {
      "LAION-10M",
      "/mnt/disk0/chukexin_motivation/diskann_data_laion25m/base.bin",
      512, 10000000, 8, 2048};

  for (ItemFile* ds : {&laion, &t2i}) {
    auto ids = pick_ids(ds->n_use, nprobe, 42);
    std::printf("# %s dim=%u item=%zu n_use=%u probes=%d\n", ds->name, ds->dim,
                ds->item, ds->n_use, nprobe);
    bench_dax(dax, *ds, ids);
    if (std::strcmp(ds->name, "T2I-10M") == 0)
      bench_vmem_t2i(vmem, *ds, ids, layout.logical_size, page);
    else
      bench_vmem_sized(vmem, *ds, ids, layout.logical_size, page);
  }
  ::close(dax);
  ::close(vmem);
  return 0;
}
