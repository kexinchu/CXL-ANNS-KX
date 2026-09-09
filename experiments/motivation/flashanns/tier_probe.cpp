#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct Args {
  std::string path;
  std::string offsets_path;
  std::string mode;
  std::string output;
  std::size_t record_stride = 0;
  std::size_t payload_bytes = 0;
};

Args parse_args(int argc, char **argv) {
  Args args;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) throw std::runtime_error("missing argument value");
    const std::string key(argv[i]);
    const std::string value(argv[i + 1]);
    if (key == "--path") args.path = value;
    else if (key == "--offsets") args.offsets_path = value;
    else if (key == "--mode") args.mode = value;
    else if (key == "--output") args.output = value;
    else if (key == "--record-stride") args.record_stride = std::stoull(value);
    else if (key == "--payload-bytes") args.payload_bytes = std::stoull(value);
    else throw std::runtime_error("unknown argument: " + key);
  }
  if (args.path.empty() || args.offsets_path.empty() || args.mode.empty() ||
      args.record_stride == 0 || args.payload_bytes == 0)
    throw std::runtime_error("required arguments are missing");
  if (args.payload_bytes > args.record_stride)
    throw std::runtime_error("payload bytes cannot exceed record stride");
  if (args.mode != "prepare-cache" && args.output.empty())
    throw std::runtime_error("--output is required for measurement");
  return args;
}

std::vector<std::uint64_t> read_offsets(const std::string &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open offsets");
  std::vector<std::uint64_t> offsets;
  std::uint64_t value;
  while (input >> value) offsets.push_back(value);
  if (offsets.empty()) throw std::runtime_error("empty offsets");
  return offsets;
}

void emit_json(const Args &args, const std::vector<std::uint64_t> &offsets,
               const std::vector<std::uint64_t> &latency_ns, std::uint64_t checksum) {
  std::ofstream output(args.output, std::ios::out | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot open output");
  output << "{\n  \"mode\": \"" << args.mode << "\",\n";
  output << "  \"record_stride\": " << args.record_stride << ",\n";
  output << "  \"payload_bytes\": " << args.payload_bytes << ",\n";
  output << "  \"checksum\": " << checksum << ",\n";
  output << "  \"offsets\": [";
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    if (i) output << ", ";
    output << offsets[i];
  }
  output << "],\n  \"latency_ns\": [";
  for (std::size_t i = 0; i < latency_ns.size(); ++i) {
    if (i) output << ", ";
    output << latency_ns[i];
  }
  output << "]\n}\n";
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Args args = parse_args(argc, argv);
    const auto offsets = read_offsets(args.offsets_path);
    const long page_size = sysconf(_SC_PAGESIZE);
    const std::uint64_t first = *std::min_element(offsets.begin(), offsets.end());
    const std::uint64_t last = *std::max_element(offsets.begin(), offsets.end());
    const std::uint64_t map_offset = first / page_size * page_size;
    const std::size_t map_length = static_cast<std::size_t>(last + args.payload_bytes - map_offset);
    const int fd = open(args.path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open source");
    void *raw = mmap(nullptr, map_length, PROT_READ, MAP_SHARED, fd, map_offset);
    close(fd);
    if (raw == MAP_FAILED) throw std::runtime_error("mmap failed");
    auto *mapped = static_cast<const std::uint8_t *>(raw);
    volatile std::uint64_t checksum = 0;

    if (args.mode == "prepare-cache") {
      std::vector<std::uint8_t> destination(args.payload_bytes);
      for (const auto offset : offsets) {
        const auto *source = mapped + (offset - map_offset);
        std::memcpy(destination.data(), source, args.payload_bytes);
        checksum += destination[offset % args.payload_bytes];
      }
      if (munmap(raw, map_length) != 0) throw std::runtime_error("munmap failed");
      std::cout << "prepared " << offsets.size() << " vectors checksum=" << checksum << "\n";
      return 0;
    }

    std::vector<std::uint8_t> host;
    if (args.mode == "host") {
      host.resize(offsets.size() * args.payload_bytes);
      for (std::size_t i = 0; i < offsets.size(); ++i) {
        const auto *source = mapped + (offsets[i] - map_offset);
        std::memcpy(host.data() + i * args.payload_bytes, source, args.payload_bytes);
      }
      for (const auto value : host) checksum += value;  // prefault the host buffer
    } else if (args.mode == "cxl_cache" || args.mode == "flash") {
      if (madvise(raw, map_length, MADV_DONTNEED) != 0)
        throw std::runtime_error("madvise failed");
    } else {
      throw std::runtime_error("unknown mode");
    }

    std::vector<std::uint64_t> latency_ns;
    latency_ns.reserve(offsets.size());
    std::vector<std::uint8_t> destination(args.payload_bytes);
    for (auto &value : destination) value = 0;  // prefault the destination buffer
    for (std::size_t i = 0; i < offsets.size(); ++i) {
      const std::uint8_t *source = args.mode == "host"
          ? host.data() + i * args.payload_bytes
          : mapped + (offsets[i] - map_offset);
      const auto begin = std::chrono::steady_clock::now();
      std::memcpy(destination.data(), source, args.payload_bytes);
      const auto end = std::chrono::steady_clock::now();
      checksum += destination[i % args.payload_bytes];
      latency_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    }
    emit_json(args, offsets, latency_ns, checksum);
    if (munmap(raw, map_length) != 0) throw std::runtime_error("munmap failed");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "tier_probe: " << error.what() << "\n";
    return 2;
  }
}
