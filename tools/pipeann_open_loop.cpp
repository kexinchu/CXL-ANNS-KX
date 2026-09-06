// Evaluation-only fixed-rate replay for the native PipeANN index.
// Every worker calls SSDIndex::pipe_search, as search_disk_index does. Latency
// starts at the scheduled arrival time and therefore includes queueing.

#include "linux_aligned_file_reader.h"
#include "nbr/nbr.h"
#include "ssd_index.h"
#include "utils.h"

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
                                static_cast<size_t>(std::ceil(fraction * values.size())) - 1);
  return values[index];
}

template <class T>
double mean(const std::vector<T> &values) {
  double total = 0.0;
  for (const auto &value : values) total += static_cast<double>(value);
  return values.empty() ? 0.0 : total / values.size();
}

template <class T>
void write_raw(const std::string &path, const std::vector<T> &values) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot create " + path);
  out.write(reinterpret_cast<const char *>(values.data()),
            static_cast<std::streamsize>(values.size() * sizeof(T)));
  if (!out) throw std::runtime_error("short write " + path);
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const Options o = parse_options(argc, argv);
    const pipeann::Metric metric = pipeann::get_metric(o.dist_fn);

    float *queries = nullptr;
    size_t nq = 0, query_dim = 0;
    pipeann::load_bin<float>(o.query_file, queries, nq, query_dim);
    if (nq == 0 || nq > UINT32_MAX) throw std::runtime_error("invalid query count");

    std::shared_ptr<AlignedFileReader> reader = std::make_shared<LinuxAlignedFileReader>();
    pipeann::AbstractNeighbor<float> *nbr = pipeann::get_nbr_handler<float>(metric, "pq");
    pipeann::IndexBuildParameters params;
    params.max_nthreads = std::max<uint32_t>(16, 2 * o.threads);
    auto index = std::make_unique<pipeann::SSDIndex<float>>(metric, reader, nbr, true, &params);
    if (index->load(o.index_prefix.c_str(), false) != 0)
      throw std::runtime_error("failed to load native PipeANN index");

    std::vector<uint32_t> ids(nq * o.k);
    std::vector<float> distances(nq * o.k);
    std::vector<pipeann::QueryStats> stats(nq);
    std::vector<double> latency_ms(nq, 0.0), queue_wait_ms(nq, 0.0);
    std::vector<uint64_t> latency_ns(nq, 0);
    std::vector<uint32_t> query_ids(nq);
    std::iota(query_ids.begin(), query_ids.end(), 0);
    std::mt19937 query_rng(o.shuffle_seed);
    std::shuffle(query_ids.begin(), query_ids.end(), query_rng);

    using Clock = std::chrono::steady_clock;
    const auto epoch = Clock::now();
    auto due = [&](uint32_t arrival_index) {
      return epoch + std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>(arrival_index / o.arrival_rate));
    };
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<uint32_t> pending;
    bool producer_done = false;

    std::thread producer([&] {
      for (uint32_t qi = 0; qi < static_cast<uint32_t>(nq); ++qi) {
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
          const auto scheduled = due(qi);
          const auto started = Clock::now();
          queue_wait_ms[qi] = started > scheduled
                                  ? std::chrono::duration<double, std::milli>(started - scheduled).count()
                                  : 0.0;
          const uint32_t query_id = query_ids[qi];
          index->pipe_search(queries + static_cast<size_t>(query_id) * query_dim,
                             o.k, 0, o.l,
                             ids.data() + static_cast<size_t>(qi) * o.k,
                             distances.data() + static_cast<size_t>(qi) * o.k,
                             o.beam_width, stats.data() + qi);
          const auto finished = Clock::now();
          latency_ns[qi] = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(finished - scheduled).count());
          latency_ms[qi] = static_cast<double>(latency_ns[qi]) / 1e6;
        }
      });
    }
    producer.join();
    for (auto &worker : workers) worker.join();
    const auto end = Clock::now();

    const std::string result_ids = o.result_prefix + "_" + std::to_string(o.l) + "_idx_uint32.bin";
    const std::string result_dists = o.result_prefix + "_" + std::to_string(o.l) + "_dists_float.bin";
    pipeann::save_bin<uint32_t>(result_ids, ids.data(), nq, o.k);
    pipeann::save_bin<float>(result_dists, distances.data(), nq, o.k);

    double recall = 0.0;
    unsigned *gt_ids = nullptr;
    float *gt_dists = nullptr;
    size_t gt_n = 0, gt_k = 0;
    uint32_t *gt_tags = nullptr;
    if (o.gt_file != "null" && o.gt_file != "NULL" && file_exists(o.gt_file)) {
      pipeann::load_truthset(o.gt_file, gt_ids, gt_dists, gt_n, gt_k, &gt_tags);
      if (gt_n != nq) throw std::runtime_error("ground-truth query count mismatch");
      std::vector<uint32_t> selected_gt(nq * gt_k);
      std::vector<float> selected_dists(gt_dists ? nq * gt_k : 0);
      for (size_t qi = 0; qi < nq; ++qi) {
        std::memcpy(selected_gt.data() + qi * gt_k,
                    gt_ids + static_cast<size_t>(query_ids[qi]) * gt_k,
                    gt_k * sizeof(uint32_t));
        if (gt_dists)
          std::memcpy(selected_dists.data() + qi * gt_k,
                      gt_dists + static_cast<size_t>(query_ids[qi]) * gt_k,
                      gt_k * sizeof(float));
      }
      recall = pipeann::calculate_recall(static_cast<uint32_t>(nq), selected_gt.data(),
                                         selected_dists.empty() ? nullptr : selected_dists.data(),
                                         static_cast<uint32_t>(gt_k), ids.data(), o.k, o.k);
    }

    if (!o.trace_dir.empty()) {
      std::filesystem::create_directories(o.trace_dir);
      write_raw(o.trace_dir + "/query_ids.u32", query_ids);
      write_raw(o.trace_dir + "/latency_ns.u64", latency_ns);
    }

    std::vector<double> total_us(nq), io_us(nq), ios(nq), hops(nq);
    for (size_t i = 0; i < nq; ++i) {
      total_us[i] = stats[i].total_us;
      io_us[i] = stats[i].io_us;
      ios[i] = stats[i].n_ios;
      hops[i] = stats[i].n_hops;
    }
    const double wall_s = std::chrono::duration<double>(end - epoch).count();
    const double qps = nq / wall_s;
    std::printf("arrival_mode=open-loop-periodic offered_QPS=%.3f latency_includes_queue=1\n", o.arrival_rate);
    std::printf("wall_s=%.6f throughput_QPS=%.3f\n", wall_s, qps);
    std::printf("latency_ms mean=%.3f p50=%.3f p90=%.3f p95=%.3f p99=%.3f\n",
                mean(latency_ms), percentile(latency_ms, 0.50), percentile(latency_ms, 0.90),
                percentile(latency_ms, 0.95), percentile(latency_ms, 0.99));
    std::printf("queue_wait_ms_mean=%.3f queue_wait_ms_p95=%.3f\n",
                mean(queue_wait_ms), percentile(queue_wait_ms, 0.95));
    std::printf("         L   I/O Width         QPS  AvgLat(us)     P99 Lat   Mean Hops    Mean IOs   Recall@10\n");
    std::printf("%10u%12u%12.2f%12.2f%12.2f%12.2f%12.2f%12.2f\n",
                o.l, o.beam_width, qps, mean(total_us), percentile(total_us, 0.99),
                mean(hops), mean(ios), recall);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "pipeann_open_loop: %s\n", error.what());
    return 2;
  }
}
