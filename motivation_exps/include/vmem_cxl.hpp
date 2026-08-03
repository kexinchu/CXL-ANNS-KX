#pragma once
// CXL / vmem identity + mmap arena for unified-AS ANNS motivation.
// Follows mem2nvme vmem_sw userspace ABI (shell.h + vmem_sw_layout.h).

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common.hpp"
#include "dataset.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

#include "shell.h"
#include "vmem_sw_layout.h"

static constexpr const char* kVmemDev = "/dev/vmem0";
static constexpr const char* kExpectedBdf = "0000:d8:00.0";
static constexpr const char* kExpectedNvme = "/dev/nvme3n2";
static constexpr uint64_t kExpectedSsdSize = 1920383410176ULL;
static constexpr uint64_t kExpectedLogicalSize = 1954743148544ULL;
static constexpr uint64_t kExpectedCacheLimit = 4294967296ULL;

// Reserved ANNS region inside /dev/vmem0 logical space (page/stripe aligned).
static constexpr uint64_t kAnnLogicalBase = 64ull * 1024ull * 1024ull;  // 64MiB
static constexpr char kAnnMagic[8] = {'C', 'X', 'L', 'A', 'N', 'N', '0', '1'};

inline std::string read_sysfs_text(const std::string& path) {
  std::ifstream in(path);
  if (!in) die("cannot read " + path);
  std::string s;
  std::getline(in, s);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  return s;
}

inline uint64_t read_sysfs_u64(const std::string& path) {
  return std::stoull(read_sysfs_text(path));
}

struct VmemCounters {
  uint64_t read_ios = 0;
  uint64_t write_ios = 0;
  uint64_t faults = 0;
  uint64_t cache_used = 0;
  uint64_t ram_allocated = 0;
  uint64_t dirty_bytes = 0;
  uint64_t io_errors = 0;
};

inline VmemCounters read_vmem_counters() {
  VmemCounters c;
  c.read_ios = read_sysfs_u64("/sys/class/vmem/vmem0/read_ios");
  c.write_ios = read_sysfs_u64("/sys/class/vmem/vmem0/write_ios");
  c.faults = read_sysfs_u64("/sys/class/vmem/vmem0/faults");
  c.cache_used = read_sysfs_u64("/sys/class/vmem/vmem0/cache_used");
  c.ram_allocated = read_sysfs_u64("/sys/class/vmem/vmem0/ram_allocated");
  c.dirty_bytes = read_sysfs_u64("/sys/class/vmem/vmem0/dirty_bytes");
  c.io_errors = read_sysfs_u64("/sys/class/vmem/vmem0/io_errors");
  return c;
}

inline void print_counter_delta(const char* tag, const VmemCounters& a,
                                const VmemCounters& b) {
  std::printf(
      "[%s] Δread_ios=%llu Δwrite_ios=%llu Δfaults=%llu Δcache_used=%llu "
      "Δram_allocated=%llu dirty=%llu io_errors=%llu\n",
      tag,
      (unsigned long long)(b.read_ios - a.read_ios),
      (unsigned long long)(b.write_ios - a.write_ios),
      (unsigned long long)(b.faults - a.faults),
      (unsigned long long)(b.cache_used - a.cache_used),
      (unsigned long long)(b.ram_allocated - a.ram_allocated),
      (unsigned long long)b.dirty_bytes,
      (unsigned long long)b.io_errors);
}

// Gate: prove production /dev/vmem0 identity (from vmem_sw demo design) +
// Montage Type-3 CXL memory is present.
inline void require_cxl_vmem_identity() {
  struct stat st {};
  if (stat(kVmemDev, &st) || !S_ISCHR(st.st_mode))
    die("/dev/vmem0 is not a character device");

  auto eq = [](const char* path, const char* want) {
    auto got = read_sysfs_text(path);
    if (got != want)
      die(std::string(path) + " is '" + got + "', expected '" + want + "'");
  };
  eq("/sys/class/vmem/vmem0/target_bdf", kExpectedBdf);
  eq("/sys/class/vmem/vmem0/nvme_dev", kExpectedNvme);
  if (read_sysfs_u64("/sys/class/vmem/vmem0/ssd_size") != kExpectedSsdSize)
    die("ssd_size mismatch");
  if (read_sysfs_u64("/sys/class/vmem/vmem0/size") != kExpectedLogicalSize)
    die("logical size mismatch");
  if (read_sysfs_u64("/sys/class/vmem/vmem0/cache_limit") != kExpectedCacheLimit)
    die("cache_limit mismatch");
  auto backend = read_sysfs_text("/sys/class/vmem/vmem0/backend");
  if (backend != "software") die("backend is not software");

  // Montage CXL Type-3 must be online (true CXL.mem HDM).
  if (access("/sys/bus/cxl/devices/mem0", F_OK) != 0)
    die("Montage CXL mem0 missing under /sys/bus/cxl/devices");
  if (access("/sys/bus/cxl/devices/region0", F_OK) != 0)
    die("CXL region0 missing");

  std::printf("PASS cxl-vmem-identity (vmem0 software + Montage mem0/region0)\n");
}

struct AnnHeader {
  char magic[8];
  uint32_t n;
  uint32_t dim;
  uint32_t degree;
  uint32_t entry;
  uint64_t logical_base;
  uint64_t payload_bytes;
};

struct VmemArena {
  int fd = -1;
  size_t page_size = 4096;
  struct vmem_info info {};
  struct vmem_layout_info layout_info {};
  struct vmem_sw_layout layout {};
  uint64_t logical_base = kAnnLogicalBase;
  uint64_t map_bytes = 0;
  void* map = nullptr;
  AnnHeader hdr {};
  bool hdr_ok = false;

  void open_dev() {
    require_cxl_vmem_identity();
    fd = ::open(kVmemDev, O_RDWR);
    if (fd < 0) die("open /dev/vmem0 failed");
    page_size = size_t(::sysconf(_SC_PAGESIZE));
    if (ioctl(fd, VMEM_IOC_GET_INFO, &info) < 0) die("VMEM_IOC_GET_INFO");
    if (info.mode != VMEM_MODE_SOFTWARE)
      die("vmem mode is not SOFTWARE");
    if (!(info.flags & VMEM_INFO_F_SOFTWARE))
      die("VMEM_INFO_F_SOFTWARE not set");
    if (ioctl(fd, VMEM_IOC_GET_LAYOUT, &layout_info) < 0)
      die("VMEM_IOC_GET_LAYOUT");
    if (layout_info.abi_version != VMEM_LAYOUT_ABI_VERSION ||
        layout_info.struct_size != sizeof(layout_info))
      die("layout ABI mismatch");
    layout.ram_size = layout_info.ram_size;
    layout.ssd_size = layout_info.ssd_size;
    layout.logical_size = layout_info.logical_size;
    layout.stripe_size = layout_info.stripe_size;
    layout.ram_stripes = layout.ram_size / layout.stripe_size;
    layout.ssd_stripes = layout.ssd_size / layout.stripe_size;
    layout.full_stripes = layout.ram_stripes + layout.ssd_stripes;
    layout.full_span = layout.full_stripes * layout.stripe_size;
    layout.ssd_tail = layout.ssd_size % layout.stripe_size;
    std::printf(
        "vmem info: mode=%u flags=0x%x bar=0x%llx/%llu logical=%llu ram=%llu "
        "ssd=%llu stripe=%llu cache_limit=%llu\n",
        info.mode, info.flags, (unsigned long long)info.bar_phys,
        (unsigned long long)info.bar_size,
        (unsigned long long)layout.logical_size,
        (unsigned long long)layout.ram_size,
        (unsigned long long)layout.ssd_size,
        (unsigned long long)layout.stripe_size,
        (unsigned long long)layout_info.cache_limit);
  }

  void set_prefetch_off() {
    struct vmem_prefetch_cfg cfg { VMEM_PREFETCH_OFF, 0 };
    if (ioctl(fd, VMEM_IOC_SET_PREFETCH, &cfg) < 0) die("SET_PREFETCH off");
  }

  void prefetch_logical(uint64_t off, uint64_t len) {
    struct vmem_logical_range r { off, len };
    if (ioctl(fd, VMEM_IOC_PREFETCH_LOGICAL, &r) < 0) {
      // Non-fatal for partial ranges in stress; count as miss path.
    }
  }

  void map_region(uint64_t bytes, int prot = PROT_READ | PROT_WRITE) {
    if (bytes % page_size) bytes = (bytes + page_size - 1) & ~(page_size - 1);
    if (logical_base % page_size) die("logical_base not page aligned");
    if (logical_base + bytes > layout.logical_size) die("region exceeds vmem");
    map_bytes = bytes;
    map = ::mmap(nullptr, map_bytes, prot, MAP_SHARED, fd, (off_t)logical_base);
    if (map == MAP_FAILED) die("mmap /dev/vmem0 failed");
  }

  void drop_host_ptes() {
    if (!map) return;
    if (::madvise(map, map_bytes, MADV_DONTNEED) != 0)
      std::perror("madvise(MADV_DONTNEED)");
  }

  // Touch unrelated SSD pages to pressure the 4GiB software cache.
  void thrash_soft_cache(uint64_t bytes_to_touch) {
    uint64_t start = logical_base + map_bytes + layout.stripe_size;
    start = (start + page_size - 1) & ~(uint64_t)(page_size - 1);
    const uint64_t chunk = 16ull << 20;  // 16MiB mmap chunks
    uint64_t touched = 0;
    while (touched < bytes_to_touch && start + chunk < layout.logical_size) {
      void* p = ::mmap(nullptr, chunk, PROT_READ, MAP_SHARED, fd, (off_t)start);
      if (p == MAP_FAILED) break;
      // touch every page
      for (uint64_t o = 0; o < chunk; o += page_size) {
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
  }

  void unmap_region() {
    if (map && map != MAP_FAILED) ::munmap(map, map_bytes);
    map = nullptr;
    map_bytes = 0;
  }

  float* vec_ptr(uint32_t id, uint32_t dim) {
    size_t vb = size_t(dim) * sizeof(float);
    // header occupies first page
    uint64_t off = page_size + uint64_t(id) * vb;
    if (off + vb > map_bytes) die("vector id out of mapped range");
    return reinterpret_cast<float*>(static_cast<char*>(map) + off);
  }

  uint64_t vec_logical(uint32_t id, uint32_t dim) const {
    size_t vb = size_t(dim) * sizeof(float);
    return logical_base + page_size + uint64_t(id) * vb;
  }

  bool page_resident(uint32_t id, uint32_t dim) {
    size_t vb = size_t(dim) * sizeof(float);
    uint64_t off = page_size + uint64_t(id) * vb;
    size_t span = (vb + page_size - 1) & ~(page_size - 1);
    std::vector<unsigned char> vec(span / page_size);
    if (::mincore(static_cast<char*>(map) + off, span, vec.data()) != 0)
      return false;
    for (unsigned char b : vec)
      if ((b & 1) == 0) return false;
    return true;
  }

  // Touch vector body (force fault / cache fill). Returns pointer.
  const float* touch_vec(uint32_t id, uint32_t dim) {
    const float* p = vec_ptr(id, dim);
    volatile float s = 0;
    for (uint32_t d = 0; d < dim; ++d) s += p[d];
    (void)s;
    return p;
  }

  // Prefetch SSD pages for ids into soft cache (sync ioctl; no PTE install).
  void prefetch_ids(const uint32_t* ids, size_t n, uint32_t dim) {
    for (size_t i = 0; i < n; ++i) {
      uint64_t loff = vec_logical(ids[i], dim);
      size_t vb = size_t(dim) * sizeof(float);
      prefetch_logical(loff & ~(uint64_t)(page_size - 1),
                       (vb + page_size - 1) & ~(page_size - 1));
    }
  }

  void cold_start(uint64_t thrash_bytes = 5ull << 30) {  // ~5GiB > 4GiB cache
    drop_host_ptes();
    thrash_soft_cache(thrash_bytes);
    drop_host_ptes();
  }

  void flush() {
    if (ioctl(fd, VMEM_IOC_FLUSH) < 0) die("VMEM_IOC_FLUSH");
  }

  void close_dev() {
    unmap_region();
    if (fd >= 0) ::close(fd);
    fd = -1;
  }

  // Populate payload from host vectors.bin; writes SSD-backed pages via mmap.
  void populate_from_file(const std::string& dir, uint32_t max_n = 0) {
    DatasetMeta meta = read_meta(dir);
    if (max_n && max_n < meta.n) meta.n = max_n;
    size_t vb = size_t(meta.dim) * sizeof(float);
    uint64_t payload = uint64_t(meta.n) * vb;
    uint64_t need = page_size + payload;
    map_region(need);

    // header
    AnnHeader h {};
    std::memcpy(h.magic, kAnnMagic, 8);
    h.n = meta.n;
    h.dim = meta.dim;
    h.degree = meta.degree;
    h.entry = meta.entry;
    h.logical_base = logical_base;
    h.payload_bytes = payload;
    std::memcpy(map, &h, sizeof(h));

    int src = ::open(vec_path(dir).c_str(), O_RDONLY);
    if (src < 0) die("open host vectors.bin");
    std::vector<char> buf(1 << 20);
    uint64_t done = 0;
    while (done < payload) {
      size_t chunk = std::min(buf.size(), size_t(payload - done));
      ssize_t r = ::pread(src, buf.data(), chunk, off_t(done));
      if (r <= 0) die("pread host vectors short");
      std::memcpy(static_cast<char*>(map) + page_size + done, buf.data(),
                  size_t(r));
      done += size_t(r);
      if ((done & ((64ull << 20) - 1)) == 0)
        std::printf("  populate %.1f/%llu MB\n", done / 1e6,
                    (unsigned long long)(payload / 1e6));
    }
    ::close(src);
    if (::msync(map, map_bytes, MS_SYNC) != 0) die("msync failed");
    if (::fsync(fd) != 0) die("fsync vmem failed");
    flush();
    hdr = h;
    hdr_ok = true;
    std::printf("PASS populate n=%u dim=%u bytes=%llu at logical 0x%llx\n",
                meta.n, meta.dim, (unsigned long long)payload,
                (unsigned long long)logical_base);
  }

  void load_header(bool writable = false) {
    int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
    // Map just header page first
    map_region(page_size, PROT_READ | PROT_WRITE);
    AnnHeader h {};
    std::memcpy(&h, map, sizeof(h));
    if (std::memcmp(h.magic, kAnnMagic, 8) != 0)
      die("ANNS magic missing on vmem — run populate first");
    unmap_region();
    map_region(page_size + h.payload_bytes, prot);
    std::memcpy(&hdr, map, sizeof(hdr));
    hdr_ok = true;
    std::printf("loaded header n=%u dim=%u base=0x%llx\n", hdr.n, hdr.dim,
                (unsigned long long)hdr.logical_base);
  }
};
