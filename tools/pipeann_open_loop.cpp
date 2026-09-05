// Evaluation-only open-loop request replay for the exact DiskANN/PipeANN index.
// It uses the same PQFlashIndex API as search_disk_index, but measures response
// time from a fixed-rate arrival timestamp so overload includes queueing delay.

#include "common_includes.h"
#include "disk_utils.h"
#include "linux_aligned_file_reader.h"
#include "math_utils.h"
#include "pq_flash_index.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
  std::string data_type = "float";
  std::string dist_fn;
  std::string index_prefix;
  std::string result_prefix;
  std::string query_file;
  std::string gt_file = "null";
  std::string trace_dir;
  uint32_t k = 0;
  uint32_t l = 0;
  uint32_t beam_width = 8;
  uint32_t threads = 8;
  uint32_t shuffle_seed = 42;
  double arrival_rate = 0.0;
};

const char *need(int &i, int argc, char **argv) {
  if (++i >= argc) throw std::invalid_argument("missing option value");
  return argv[i];
}

Options parse_options(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--data_type") o.data_type = need(i, argc, argv);
    else if (a == "--dist_fn") o.dist_fn = need(i, argc, argv);
    else if (a == "--index_path_prefix") o.index_prefix = need(i, argc, argv);
    else if (a == "--result_path") o.result_prefix = need(i, argc, argv);
    else if (a == "--query_file") o.query_file = need(i, argc, argv);
    else if (a == "--gt_file") o.gt_file = need(i, argc, argv);
    else if (a == "--trace-dir") o.trace_dir = need(i, argc, argv);
    else if (a == "-K" || a == "--recall_at") o.k = std::stoul(need(i, argc, argv));
    else if (a == "-L" || a == "--search_list") o.l = std::stoul(need(i, argc, argv));
    else if (a == "-W" || a == "--beamwidth") o.beam_width = std::stoul(need(i, argc, argv));
    else if (a == "-T" || a == "--num_threads") o.threads = std::stoul(need(i, argc, argv));
    else if (a == "--arrival-rate") o.arrival_rate = std::stod(need(i, argc, argv));
    else if (a == "--shuffle-seed") o.shuffle_seed = std::stoul(need(i, argc, argv));
    else throw std::invalid_argument("unknown option " + a);
  }
  if (o.data_type != "float" || o.index_prefix.empty() || o.result_prefix.empty() ||
      o.query_file.empty() || o.k == 0 || o.l < o.k || o.threads == 0 ||
      !std::isfinite(o.arrival_rate) || o.arrival_rate <= 0.0)
    throw std::invalid_argument("invalid or incomplete open-loop options");
  return o;
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t index = std::min(values.size() - 1,
                                (size_t)std::ceil(fraction * values.size()) - 1);
  return values[index];
}

template <class T>
double mean(const std::vector<T> &values) {
  double total = 0.0;
  for (const auto &value : values) total += (double)value;
  return values.empty() ? 0.0 : total / values.size();
}

template <class T>
void write_raw(const std::string &path, const std::vector<T> &values) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot create " + path);
  out.write(reinterpret_cast<const char *>(values.data()),
            (std::streamsize)(values.size() * sizeof(T)));
  if (!out) throw std::runtime_error("short write " + path);
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Options o = parse_options(argc, argv);
    diskann::Metric metric;
    if (o.dist_fn == "mips") metric = diskann::Metric::INNER_PRODUCT;
    else if (o.dist_fn == "l2") metric = diskann::Metric::L2;
    else if (o.dist_fn == "cosine") metric = diskann::Metric::COSINE;
    else throw std::invalid_argument("unsupported distance function");

    float *queries = nullptr;
    size_t nq = 0, dim = 0, aligned_dim = 0;
    diskann::load_aligned_bin<float>(o.query_file, queries, nq, dim, aligned_dim);
    if (nq == 0 || nq > UINT32_MAX) throw std::runtime_error("invalid query count");

    std::shared_ptr<AlignedFileReader> reader = std::make_shared<LinuxAlignedFileReader>();
    auto index = std::make_unique<diskann::PQFlashIndex<float, uint32_t>>(reader, metric);
    if (index->load(o.threads, o.index_prefix.c_str()) != 0)
      throw std::runtime_error("failed to load PipeANN index");
    std::vector<uint32_t> cache_nodes;
    index->cache_bfs_levels(0, cache_nodes);
    index->load_cache_list(cache_nodes);

    std::vector<uint64_t> ids64(nq * o.k);
    std::vector<uint32_t> ids32(nq * o.k);
    std::vector<float> distances(nq * o.k);
    std::vector<diskann::QueryStats> stats(nq);
    std::vector<double> latency_ms(nq, 0.0), queue_wait_ms(nq, 0.0);
    std::vector<uint64_t> latency_ns(nq, 0);
    std::vector<uint32_t> query_ids(nq);
    std::iota(query_ids.begin(), query_ids.end(), 0);
    std::mt19937 query_rng(o.shuffle_seed);
    std::shuffle(query_ids.begin(), query_ids.end(), query_rng);

    using Clock = std::chrono::steady_clock;
    const auto epoch = Clock::now();
    auto due = [&](uint32_t qi) {
      return epoch + std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>((double)qi / o.arrival_rate));
    };
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<uint32_t> pending;
    bool producer_done = false;

    std::thread producer([&] {
      for (uint32_t qi = 0; qi < (uint32_t)nq; ++qi) {
        std::this_thread::sleep_until(due(qi));
        {
          std::lock_guard<std::mutex> lock(mutex);
          pending.push_back(qi);
        }
        ready.notify_one();
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        producer_done = true;
      }
      ready.notify_all();
    });

    std::vector<std::thread> workers;
    workers.reserve(o.threads);
    for (uint32_t worker = 0; worker < o.threads; ++worker) {
      workers.emplace_back([&] {
        for (;;) {
          uint32_t qi = 0;
          {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [&] { return !pending.empty() || producer_done; });
            if (pending.empty()) return;
            qi = pending.front();
            pending.pop_front();
          }
          const auto started = Clock::now();
          const auto scheduled = due(qi);
          queue_wait_ms[qi] = started > scheduled
                                  ? std::chrono::duration<double, std::milli>(started - scheduled).count()
                                  : 0.0;
          const uint32_t query_id = query_ids[qi];
          index->cached_beam_search(
              queries + (size_t)query_id * aligned_dim, o.k, o.l,
              ids64.data() + (size_t)qi * o.k,
              distances.data() + (size_t)qi * o.k, o.beam_width,
              false, stats.data() + qi);
          const auto finished = Clock::now();
          latency_ns[qi] = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                               finished - scheduled).count();
          latency_ms[qi] = (double)latency_ns[qi] / 1e6;
        }
      });
    }
    producer.join();
    for (auto &worker : workers) worker.join();
    const auto end = Clock::now();

    for (size_t i = 0; i < ids64.size(); ++i) ids32[i] = (uint32_t)ids64[i];
    const std::string result_ids = o.result_prefix + "_" + std::to_string(o.l) + "_idx_uint32.bin";
    const std::string result_dists = o.result_prefix + "_" + std::to_string(o.l) + "_dists_float.bin";
    diskann::save_bin<uint32_t>(result_ids, ids32.data(), nq, o.k);
    diskann::save_bin<float>(result_dists, distances.data(), nq, o.k);

    double recall = 0.0;
    uint32_t *gt_ids = nullptr;
    float *gt_dists = nullptr;
    size_t gt_n = 0, gt_k = 0;
    if (o.gt_file != "null" && o.gt_file != "NULL" && file_exists(o.gt_file)) {
      diskann::load_truthset(o.gt_file, gt_ids, gt_dists, gt_n, gt_k);
      if (gt_n != nq) throw std::runtime_error("ground-truth query count mismatch");
      std::vector<uint32_t> selected_gt(nq * gt_k);
      std::vector<float> selected_dists(gt_dists ? nq * gt_k : 0);
      for (size_t qi = 0; qi < nq; ++qi) {
        std::memcpy(selected_gt.data() + qi * gt_k,
                    gt_ids + (size_t)query_ids[qi] * gt_k, gt_k * sizeof(uint32_t));
        if (gt_dists)
          std::memcpy(selected_dists.data() + qi * gt_k,
                      gt_dists + (size_t)query_ids[qi] * gt_k, gt_k * sizeof(float));
      }
      recall = diskann::calculate_recall((uint32_t)nq, selected_gt.data(),
                                         selected_dists.empty() ? nullptr : selected_dists.data(),
                                         (uint32_t)gt_k, ids32.data(), o.k, o.k);
    }

    if (!o.trace_dir.empty()) {
      std::filesystem::create_directories(o.trace_dir);
      write_raw(o.trace_dir + "/query_ids.u32", query_ids);
      write_raw(o.trace_dir + "/latency_ns.u64", latency_ns);
    }

    std::vector<double> total_us(nq), io_us(nq), ios(nq), cpu_us(nq);
    for (size_t i = 0; i < nq; ++i) {
      total_us[i] = stats[i].total_us;
      io_us[i] = stats[i].io_us;
      ios[i] = stats[i].n_ios;
      cpu_us[i] = stats[i].cpu_us;
    }
    const double wall_s = std::chrono::duration<double>(end - epoch).count();
    const double qps = nq / wall_s;
    std::printf("arrival_mode=open-loop-periodic offered_QPS=%.3f latency_includes_queue=1\n",
                o.arrival_rate);
    std::printf("wall_s=%.6f throughput_QPS=%.3f\n", wall_s, qps);
    std::printf("latency_ms mean=%.3f p50=%.3f p90=%.3f p95=%.3f p99=%.3f\n",
                mean(latency_ms), percentile(latency_ms, 0.50), percentile(latency_ms, 0.90),
                percentile(latency_ms, 0.95), percentile(latency_ms, 0.99));
    std::printf("queue_wait_ms_mean=%.3f queue_wait_ms_p95=%.3f\n",
                mean(queue_wait_ms), percentile(queue_wait_ms, 0.95));
    std::printf(" L Beamwidth QPS Mean Latency 99.9 Latency Mean IOs Mean IO (us) CPU (s) Recall@10\n");
    std::printf(" %u %u %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", o.l, o.beam_width,
                qps, mean(total_us), percentile(total_us, 0.999), mean(ios), mean(io_us),
                mean(cpu_us) / 1e6, recall);

    diskann::aligned_free(queries);
    if (gt_ids) delete[] gt_ids;
    if (gt_dists) delete[] gt_dists;
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "pipeann_open_loop: %s\n", error.what());
    return 2;
  }
}
