#include "serving/eval_trace.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

template <typename T>
static std::vector<T> read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  assert(in);
  const auto bytes = in.tellg();
  assert(bytes >= 0);
  assert((size_t)bytes % sizeof(T) == 0);
  std::vector<T> out((size_t)bytes / sizeof(T));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(out.data()), bytes);
  assert(in);
  return out;
}

int main() {
  const auto dir = std::filesystem::temp_directory_path() / "flashanns-eval-trace-test";
  std::filesystem::remove_all(dir);

  EvalTrace trace(3, 2);
  trace.record(2, 102, 300, {20, 21, 22}, {202, 203});
  trace.record(0, 100, 100, {0}, {200, 201});
  trace.record(1, 101, 200, {10, 11}, {201, 202});
  assert(trace.finish(dir));

  assert(std::filesystem::file_size(dir / "query_ids.u32") == 3 * 4);
  assert(std::filesystem::file_size(dir / "latency_ns.u64") == 3 * 8);
  assert(std::filesystem::file_size(dir / "candidate_offsets.u64") == 4 * 8);
  assert(std::filesystem::file_size(dir / "candidate_ids.u32") == 6 * 4);
  assert(std::filesystem::file_size(dir / "result_ids.u32") == 3 * 2 * 4);

  assert((read_all<uint32_t>(dir / "query_ids.u32") == std::vector<uint32_t>{100, 101, 102}));
  assert((read_all<uint64_t>(dir / "latency_ns.u64") == std::vector<uint64_t>{100, 200, 300}));
  assert((read_all<uint64_t>(dir / "candidate_offsets.u64") ==
          std::vector<uint64_t>{0, 1, 3, 6}));
  assert((read_all<uint32_t>(dir / "candidate_ids.u32") ==
          std::vector<uint32_t>{0, 10, 11, 20, 21, 22}));
  assert((read_all<uint32_t>(dir / "result_ids.u32") ==
          std::vector<uint32_t>{200, 201, 201, 202, 202, 203}));

  EvalTrace incomplete(2, 1);
  incomplete.record(0, 7, 9, {1}, {2});
  assert(!incomplete.finish(dir / "incomplete"));

  std::filesystem::remove_all(dir);
  std::puts("test_eval_trace OK");
  return 0;
}
