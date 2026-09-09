#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/ioctl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef VMEM_IOC_MAGIC
#define VMEM_IOC_MAGIC 'V'
#define VMEM_BATCH_MAX 256U
struct vmem_batch_read {
  uint32_t count;
  uint32_t _pad;
  uint64_t offsets[VMEM_BATCH_MAX];
  uint64_t dests[VMEM_BATCH_MAX];
};
struct vmem_batch_prefetch {
  uint32_t count;
  uint32_t _pad;
  uint64_t offsets[VMEM_BATCH_MAX];
};
#define VMEM_IOC_PREFETCH_BATCH _IOW(VMEM_IOC_MAGIC, 9, struct vmem_batch_prefetch)
#define VMEM_IOC_READ_BATCH _IOW(VMEM_IOC_MAGIC, 10, struct vmem_batch_read)
#endif

namespace {

struct Args {
  std::string device;
  std::string offsets_path;
  std::string pages_path;
  std::string output;
  std::uint64_t image_offset = 0;
  std::uint64_t image_bytes = 0;
  std::size_t wave_pages = 32;
  bool dry_run = false;
};

Args parse_args(int argc, char **argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string key(argv[i]);
    if (key == "--dry-run") {
      args.dry_run = true;
      continue;
    }
    if (++i >= argc) throw std::runtime_error("missing argument value");
    const std::string value(argv[i]);
    if (key == "--device") args.device = value;
    else if (key == "--image-offset") args.image_offset = std::stoull(value);
    else if (key == "--image-bytes") args.image_bytes = std::stoull(value);
    else if (key == "--wave-pages") args.wave_pages = std::stoull(value);
    else if (key == "--query-offsets") args.offsets_path = value;
    else if (key == "--pages") args.pages_path = value;
    else if (key == "--output") args.output = value;
    else throw std::runtime_error("unknown argument: " + key);
  }
  if (args.device.empty() || args.offsets_path.empty() || args.pages_path.empty() ||
      args.output.empty() || args.image_bytes == 0)
    throw std::runtime_error("required arguments are missing");
  if (args.wave_pages == 0 || args.wave_pages > VMEM_BATCH_MAX)
    throw std::runtime_error("wave-pages must be in 1..256");
  return args;
}

std::vector<std::uint64_t> read_u64(const std::string &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open input: " + path);
  const auto bytes = input.tellg();
  if (bytes < 0 || bytes % static_cast<std::streamoff>(sizeof(std::uint64_t)) != 0)
    throw std::runtime_error("unaligned u64 input: " + path);
  input.seekg(0);
  std::vector<std::uint64_t> values(static_cast<std::size_t>(bytes) / sizeof(std::uint64_t));
  input.read(reinterpret_cast<char *>(values.data()), bytes);
  if (!input) throw std::runtime_error("short input: " + path);
  return values;
}

struct IssueResult {
  std::uint64_t fallback_waves = 0;
  std::uint64_t checksum = 0;
};

IssueResult issue_pages(int fd, const std::uint8_t *mapped_image,
                        std::uint64_t image_offset, const std::uint64_t *pages,
                        std::size_t count, std::vector<std::uint8_t> &buffer,
                        std::size_t wave, bool dry_run) {
  constexpr std::size_t page_bytes = 4096;
  IssueResult result;
  for (std::size_t begin = 0; begin < count; begin += wave) {
    vmem_batch_read batch{};
    const std::size_t n = std::min(wave, count - begin);
    batch.count = static_cast<std::uint32_t>(n);
    for (std::size_t i = 0; i < n; ++i) {
      batch.offsets[i] = image_offset + pages[begin + i];
      batch.dests[i] = reinterpret_cast<std::uint64_t>(buffer.data() + (begin + i) * page_bytes);
    }
    const bool read_failed = dry_run || ioctl(fd, VMEM_IOC_READ_BATCH, &batch) != 0;
    if (read_failed) {
      ++result.fallback_waves;
      if (!dry_run) {
        vmem_batch_prefetch prefetch{};
        prefetch.count = static_cast<std::uint32_t>(n);
        for (std::size_t i = 0; i < n; ++i)
          prefetch.offsets[i] = image_offset + pages[begin + i];
        if (ioctl(fd, VMEM_IOC_PREFETCH_BATCH, &prefetch) != 0)
          throw std::runtime_error("VMEM_IOC_READ_BATCH and VMEM_IOC_PREFETCH_BATCH failed: " +
                                   std::string(std::strerror(errno)));
      }
      for (std::size_t i = 0; i < n; ++i) {
        std::memcpy(buffer.data() + (begin + i) * page_bytes,
                    mapped_image + pages[begin + i], page_bytes);
      }
    }
    for (std::size_t i = 0; i < n; ++i)
      result.checksum += buffer[(begin + i) * page_bytes];
  }
  return result;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Args args = parse_args(argc, argv);
    const auto offsets = read_u64(args.offsets_path);
    const auto pages = read_u64(args.pages_path);
    if (offsets.size() < 2 || offsets.front() != 0 || offsets.back() != pages.size())
      throw std::runtime_error("query offsets do not bound pages");
    for (std::size_t i = 1; i < offsets.size(); ++i) {
      if (offsets[i] < offsets[i - 1]) throw std::runtime_error("query offsets are not monotonic");
    }
    constexpr std::uint64_t page_bytes = 4096;
    if (args.image_offset % page_bytes != 0)
      throw std::runtime_error("image offset is not page aligned");
    for (const std::uint64_t page : pages) {
      if (page % page_bytes != 0 || page > args.image_bytes ||
          args.image_bytes - page < page_bytes)
        throw std::runtime_error("page is unaligned or outside the image");
    }
    const int fd = open(args.device.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open device");
    void *mapping = mmap(nullptr, args.image_bytes, PROT_READ, MAP_SHARED, fd,
                         static_cast<off_t>(args.image_offset));
    if (mapping == MAP_FAILED) {
      const std::string error = std::strerror(errno);
      close(fd);
      throw std::runtime_error("cannot mmap image: " + error);
    }
    std::vector<std::uint64_t> latency_ns;
    latency_ns.reserve(offsets.size() - 1);
    IssueResult total;
    for (std::size_t query = 0; query + 1 < offsets.size(); ++query) {
      const std::size_t begin = offsets[query];
      const std::size_t count = offsets[query + 1] - begin;
      std::vector<std::uint8_t> buffer(count * 4096);
      for (std::size_t page = 0; page < count; ++page) buffer[page * 4096] = 0;
      const auto start = std::chrono::steady_clock::now();
      const IssueResult issued = issue_pages(
          fd, static_cast<const std::uint8_t *>(mapping), args.image_offset,
          pages.data() + begin, count, buffer, args.wave_pages, args.dry_run);
      const auto end = std::chrono::steady_clock::now();
      total.fallback_waves += issued.fallback_waves;
      total.checksum += issued.checksum;
      latency_ns.push_back(
          std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    munmap(mapping, args.image_bytes);
    close(fd);
    std::ofstream output(args.output, std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open output");
    output << "{\n  \"dry_run\": " << (args.dry_run ? "true" : "false")
           << ",\n  \"completed_queries\": " << latency_ns.size()
           << ",\n  \"logical_pages\": " << pages.size() << ",\n  \"latency_ns\": [";
    for (std::size_t i = 0; i < latency_ns.size(); ++i) {
      if (i) output << ", ";
      output << latency_ns[i];
    }
    output << "],\n  \"wave_pages\": " << args.wave_pages
           << ",\n  \"fallback_waves\": " << total.fallback_waves
           << ",\n  \"checksum\": " << total.checksum << "\n}\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "replay_probe: " << error.what() << "\n";
    return 2;
  }
}
