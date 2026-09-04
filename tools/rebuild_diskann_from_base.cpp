// Rebuild a fixed-stride DiskANN image from the original fbin vectors.
// Logical record i contains base[new_to_old[i]] and logical graph row i.
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

uint64_t parse_u64(const char* text, const char* name) {
  if (!text || !text[0] || text[0] == '-') reject(name);
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (errno || !end || *end) reject(name);
  return static_cast<uint64_t>(value);
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
  const char* base_path = nullptr;
  const char* map_path = nullptr;
  const char* graph_path = nullptr;
  const char* out_path = nullptr;
  uint64_t entry_id64 = 0;
  uint64_t entry_nodes64 = 0;
  uint64_t r64 = 0;
  uint64_t stride64 = kDiskannStride;
  bool have_entry_id = false;
  bool have_entry_nodes = false;
  bool have_r = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> const char* {
      if (++i >= argc) reject("missing option value");
      return argv[i];
    };
    if (arg == "--base") base_path = value();
    else if (arg == "--id-map") map_path = value();
    else if (arg == "--graph") graph_path = value();
    else if (arg == "--out") out_path = value();
    else if (arg == "--entry-id") {
      entry_id64 = parse_u64(value(), "invalid --entry-id");
      have_entry_id = true;
    } else if (arg == "--entry-nodes") {
      entry_nodes64 = parse_u64(value(), "invalid --entry-nodes");
      have_entry_nodes = true;
    } else if (arg == "--R") {
      r64 = parse_u64(value(), "invalid --R");
      have_r = true;
    } else if (arg == "--stride") {
      stride64 = parse_u64(value(), "invalid --stride");
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return 2;
    }
  }
  if (!base_path || !map_path || !graph_path || !out_path || !have_entry_id ||
      !have_entry_nodes || !have_r) {
    std::fprintf(stderr,
                 "usage: rebuild_diskann_from_base --base base.fbin --id-map new_to_old.bin "
                 "--graph logical_graph.bin --out new.bin --entry-id ID --entry-nodes N "
                 "--R R [--stride 2048]\n");
    return 2;
  }
  if (!r64 || r64 > UINT32_MAX || entry_id64 > UINT32_MAX || entry_nodes64 > UINT32_MAX)
    reject("numeric option out of range");

  ReadMapping base(base_path);
  if (base.size < 8) reject("base fbin is shorter than its header");
  uint32_t n = 0, dim = 0;
  std::memcpy(&n, base.data, 4);
  std::memcpy(&dim, base.data + 4, 4);
  if (!n || !dim) reject("base fbin has zero rows or dimension");
  const uint64_t vector_bytes = static_cast<uint64_t>(dim) * sizeof(float);
  const uint64_t expected_base = 8 + static_cast<uint64_t>(n) * vector_bytes;
  if (expected_base != base.size) reject("base fbin size does not match header");

  const uint32_t R = static_cast<uint32_t>(r64);
  const size_t payload = Placement::diskann_payload_bytes(dim, sizeof(float), R);
  if (stride64 < payload || stride64 > SIZE_MAX) reject("stride cannot hold a record");
  const size_t stride = static_cast<size_t>(stride64);

  ReadMapping id_map(map_path);
  ReadMapping graph(graph_path);
  if (id_map.size != static_cast<uint64_t>(n) * sizeof(uint32_t))
    reject("id map size is not exactly n uint32 values");
  if (graph.size != static_cast<uint64_t>(n) * R * sizeof(uint32_t))
    reject("graph size is not exactly n * R uint32 values");
  const auto* new_to_old = reinterpret_cast<const uint32_t*>(id_map.data);
  const auto* logical_graph = reinterpret_cast<const uint32_t*>(graph.data);

  std::vector<uint8_t> seen(static_cast<size_t>(n), 0);
  for (uint32_t logical = 0; logical < n; ++logical) {
    const uint32_t old = new_to_old[logical];
    if (old >= n || seen[old]) {
      std::fprintf(stderr, "id map is not a permutation at logical=%u old=%u\n", logical,
                   old);
      return 2;
    }
    seen[old] = 1;
  }
  const uint64_t neighbor_count = static_cast<uint64_t>(n) * R;
  for (uint64_t i = 0; i < neighbor_count; ++i) {
    if (logical_graph[i] >= n) {
      std::fprintf(stderr, "graph neighbor out of range at index=%llu id=%u\n",
                   static_cast<unsigned long long>(i), logical_graph[i]);
      return 2;
    }
  }

  const uint64_t output_bytes = 4096 + static_cast<uint64_t>(n) * stride;
  if (output_bytes > static_cast<uint64_t>(INT64_MAX)) reject("output is too large");
  const int out = ::open(out_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (out < 0) fail("create output (refusing overwrite)");
  if (::ftruncate(out, static_cast<off_t>(output_bytes)) != 0) fail("size output");
  const int alloc_rc = ::posix_fallocate(out, 0, static_cast<off_t>(output_bytes));
  if (alloc_rc != 0) {
    errno = alloc_rc;
    fail("allocate output");
  }

  CxanLayoutHeader header {};
  header.magic = kCxanMagic;
  header.version = 2;
  header.n = n;
  header.dim = dim;
  header.R = R;
  header.vec_bytes = sizeof(float);
  header.entry_id = static_cast<uint32_t>(entry_id64);
  header.entry_nodes = static_cast<uint32_t>(entry_nodes64);
  header.off_vectors = 4096;
  header.len_vectors = static_cast<uint64_t>(n) * stride;
  std::vector<uint8_t> header_page(4096, 0);
  std::memcpy(header_page.data(), &header, sizeof(header));
  if (::lseek(out, 0, SEEK_SET) < 0) fail("seek output header");
  write_all(out, header_page.data(), header_page.size());

  constexpr size_t kTargetBufferBytes = 16u << 20;
  const size_t records_per_chunk = std::max<size_t>(1, kTargetBufferBytes / stride);
  std::vector<uint8_t> buffer(records_per_chunk * stride, 0);
  const uint8_t* source_vectors = base.data + 8;
  for (uint32_t first = 0; first < n;) {
    const size_t count = std::min<size_t>(records_per_chunk, static_cast<size_t>(n - first));
    std::fill(buffer.begin(), buffer.begin() + count * stride, 0);
    for (size_t j = 0; j < count; ++j) {
      const uint32_t logical = first + static_cast<uint32_t>(j);
      uint8_t* record = buffer.data() + j * stride;
      std::memcpy(record, source_vectors + static_cast<uint64_t>(new_to_old[logical]) * vector_bytes,
                  static_cast<size_t>(vector_bytes));
      std::memcpy(record + vector_bytes, &R, sizeof(R));
      std::memcpy(record + vector_bytes + sizeof(R),
                  logical_graph + static_cast<uint64_t>(logical) * R,
                  static_cast<size_t>(R) * sizeof(uint32_t));
    }
    write_all(out, buffer.data(), count * stride);
    first += static_cast<uint32_t>(count);
    if (first == n || first % 1048576u < count) {
      std::fprintf(stderr, "rebuilt %u / %u\n", first, n);
      std::fflush(stderr);
    }
  }
  if (::fsync(out) != 0) fail("fsync output");
  if (::close(out) != 0) fail("close output");
  std::fprintf(stderr, "OK n=%u dim=%u R=%u stride=%zu bytes=%llu out=%s\n", n, dim, R,
               stride, static_cast<unsigned long long>(output_bytes), out_path);
  return 0;
}
