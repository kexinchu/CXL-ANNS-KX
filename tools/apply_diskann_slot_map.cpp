// Apply an already-frozen logical-id -> physical-slot permutation to a DiskANN image.
#include "serving/placement.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

[[noreturn]] void fail(const char* message) {
  std::perror(message);
  std::exit(1);
}

[[noreturn]] void reject(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  std::exit(2);
}

struct ReadMapping {
  int fd = -1;
  size_t size = 0;
  const uint8_t* data = nullptr;

  explicit ReadMapping(const char* path) {
    fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) fail(path);
    struct stat st {};
    if (::fstat(fd, &st) != 0) fail("fstat input");
    if (st.st_size <= 0) reject("empty input");
    size = static_cast<size_t>(st.st_size);
    void* p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) fail("mmap input");
    data = static_cast<const uint8_t*>(p);
  }

  ~ReadMapping() {
    if (data) ::munmap(const_cast<uint8_t*>(data), size);
    if (fd >= 0) ::close(fd);
  }

  ReadMapping(const ReadMapping&) = delete;
  ReadMapping& operator=(const ReadMapping&) = delete;
};

void write_all(int fd, const void* data, size_t bytes) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (bytes) {
    const ssize_t wrote = ::write(fd, p, bytes);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      fail("write output");
    }
    if (!wrote) reject("zero-byte output write");
    p += static_cast<size_t>(wrote);
    bytes -= static_cast<size_t>(wrote);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const char* source_path = nullptr;
  const char* map_path = nullptr;
  const char* output_path = nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> const char* {
      if (++i >= argc) reject("missing option value");
      return argv[i];
    };
    if (arg == "--src") source_path = value();
    else if (arg == "--slot-map") map_path = value();
    else if (arg == "--out") output_path = value();
    else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return 2;
    }
  }
  if (!source_path || !map_path || !output_path) {
    std::fprintf(stderr,
                 "usage: apply_diskann_slot_map --src logical.bin --slot-map id_to_slot.bin "
                 "--out extent.bin\n");
    return 2;
  }

  ReadMapping source(source_path);
  if (source.size < sizeof(CxanLayoutHeader)) reject("source header is too short");
  const auto* header = reinterpret_cast<const CxanLayoutHeader*>(source.data);
  if (header->magic != kCxanMagic || header->version < 2 || !header->n)
    reject("source has invalid DiskANN header");
  if (!header->off_vectors || header->len_vectors % header->n)
    reject("source has invalid vector region");
  const uint64_t declared_size = header->off_vectors + header->len_vectors;
  if (declared_size != source.size) reject("source size differs from declared vector region");
  const size_t stride = static_cast<size_t>(header->len_vectors / header->n);
  const size_t payload =
      Placement::diskann_payload_bytes(header->dim, header->vec_bytes, header->R);
  if (stride < payload) reject("source stride is smaller than its record payload");

  ReadMapping id_to_slot_file(map_path);
  const uint32_t n = header->n;
  if (id_to_slot_file.size != static_cast<uint64_t>(n) * sizeof(uint32_t))
    reject("slot map size is not exactly n uint32 values");
  const auto* id_to_slot = reinterpret_cast<const uint32_t*>(id_to_slot_file.data);
  std::vector<uint32_t> slot_to_id(static_cast<size_t>(n), UINT32_MAX);
  for (uint32_t id = 0; id < n; ++id) {
    const uint32_t slot = id_to_slot[id];
    if (slot >= n || slot_to_id[slot] != UINT32_MAX) {
      std::fprintf(stderr, "slot map is not a permutation at id=%u slot=%u\n", id, slot);
      return 2;
    }
    slot_to_id[slot] = id;
  }

  const int output =
      ::open(output_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (output < 0) fail("create output (refusing overwrite)");
  if (::ftruncate(output, static_cast<off_t>(declared_size)) != 0) fail("size output");
  const int alloc_rc = ::posix_fallocate(output, 0, static_cast<off_t>(declared_size));
  if (alloc_rc != 0) {
    errno = alloc_rc;
    fail("allocate output");
  }
  write_all(output, source.data, static_cast<size_t>(header->off_vectors));

  constexpr size_t kTargetBufferBytes = 16u << 20;
  const size_t records_per_chunk = std::max<size_t>(1, kTargetBufferBytes / stride);
  std::vector<uint8_t> buffer(records_per_chunk * stride);
  const uint8_t* records = source.data + header->off_vectors;
  for (uint32_t first_slot = 0; first_slot < n;) {
    const size_t count =
        std::min<size_t>(records_per_chunk, static_cast<size_t>(n - first_slot));
    for (size_t j = 0; j < count; ++j) {
      const uint32_t slot = first_slot + static_cast<uint32_t>(j);
      const uint32_t logical_id = slot_to_id[slot];
      std::memcpy(buffer.data() + j * stride,
                  records + static_cast<uint64_t>(logical_id) * stride, stride);
    }
    write_all(output, buffer.data(), count * stride);
    first_slot += static_cast<uint32_t>(count);
    if (first_slot == n || first_slot % 1048576u < count) {
      std::fprintf(stderr, "remapped %u / %u\n", first_slot, n);
      std::fflush(stderr);
    }
  }
  if (::fsync(output) != 0) fail("fsync output");
  if (::close(output) != 0) fail("close output");
  std::fprintf(stderr, "OK n=%u stride=%zu bytes=%llu out=%s\n", n, stride,
               static_cast<unsigned long long>(declared_size), output_path);
  return 0;
}
