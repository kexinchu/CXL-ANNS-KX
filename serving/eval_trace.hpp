#pragma once

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

struct EvalTraceRow {
  uint32_t query_id = UINT32_MAX;
  uint64_t latency_ns = 0;
  std::vector<uint32_t> candidates;
  std::vector<uint32_t> results;
  bool valid = false;
};

class EvalTrace {
 public:
  EvalTrace(size_t nq, uint32_t k) : rows_(nq), k_(k) {}

  void record(size_t ordinal, uint32_t query_id, uint64_t latency_ns,
              std::vector<uint32_t> candidates, std::vector<uint32_t> results) {
    if (ordinal >= rows_.size()) return;
    EvalTraceRow& row = rows_[ordinal];
    row.query_id = query_id;
    row.latency_ns = latency_ns;
    row.candidates = std::move(candidates);
    row.results = std::move(results);
    row.valid = row.results.size() == k_;
  }

  bool finish(const std::filesystem::path& dir) const {
    for (const EvalTraceRow& row : rows_) {
      if (!row.valid || row.results.size() != k_) return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;

    std::vector<uint32_t> query_ids;
    std::vector<uint64_t> latency_ns;
    std::vector<uint64_t> candidate_offsets;
    std::vector<uint32_t> candidate_ids;
    std::vector<uint32_t> result_ids;
    query_ids.reserve(rows_.size());
    latency_ns.reserve(rows_.size());
    candidate_offsets.reserve(rows_.size() + 1);
    result_ids.reserve(rows_.size() * k_);
    candidate_offsets.push_back(0);
    for (const EvalTraceRow& row : rows_) {
      query_ids.push_back(row.query_id);
      latency_ns.push_back(row.latency_ns);
      candidate_ids.insert(candidate_ids.end(), row.candidates.begin(), row.candidates.end());
      candidate_offsets.push_back(candidate_ids.size());
      result_ids.insert(result_ids.end(), row.results.begin(), row.results.end());
    }

    const std::vector<std::filesystem::path> outputs = {
        dir / "query_ids.u32", dir / "latency_ns.u64", dir / "candidate_offsets.u64",
        dir / "candidate_ids.u32", dir / "result_ids.u32"};
    bool ok = write_atomic(outputs[0], query_ids.data(), query_ids.size() * sizeof(uint32_t)) &&
              write_atomic(outputs[1], latency_ns.data(), latency_ns.size() * sizeof(uint64_t)) &&
              write_atomic(outputs[2], candidate_offsets.data(),
                           candidate_offsets.size() * sizeof(uint64_t)) &&
              write_atomic(outputs[3], candidate_ids.data(),
                           candidate_ids.size() * sizeof(uint32_t)) &&
              write_atomic(outputs[4], result_ids.data(), result_ids.size() * sizeof(uint32_t));
    if (!ok) return false;

    int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) return false;
    ok = ::fsync(dir_fd) == 0;
    ok = (::close(dir_fd) == 0) && ok;
    return ok;
  }

 private:
  static bool write_atomic(const std::filesystem::path& path, const void* data, size_t bytes) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    size_t done = 0;
    bool ok = true;
    while (done < bytes) {
      ssize_t n = ::write(fd, src + done, bytes - done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        ok = false;
        break;
      }
      done += static_cast<size_t>(n);
    }
    if (ok) ok = ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    if (ok) {
      std::error_code ec;
      std::filesystem::rename(tmp, path, ec);
      ok = !ec;
    }
    if (!ok) {
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
    }
    return ok;
  }

  std::vector<EvalTraceRow> rows_;
  uint32_t k_ = 0;
};
