#include "beam.hpp"
#include "common.hpp"
#include "dataset.hpp"
#include "store.hpp"
#include "vmem_cxl.hpp"
#include "vmem_findings.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <list>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static void usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  motivation gen  --dir DIR [--n N] [--dim D] [--degree R]\n"
               "  motivation f1   --dir DIR [--nq N] [--beam B] [--hops H] "
               "[--cache-mb M] [--node N]\n"
               "  motivation f2   --dir DIR [...]\n"
               "  motivation f3   --dir DIR [...]\n"
               "  motivation vmem-identity\n"
               "  motivation vmem-populate --dir DIR [--n N]\n"
               "  motivation vmem-h0  --dir DIR [--iters N]\n"
               "  motivation vmem-f1  --dir DIR [--nq N] [--beam B] [--hops H]\n"
               "  motivation vmem-f2  --dir DIR [--nq N] [--beam B] [--hops H] "
               "[--cache-mb M]\n"
               "  motivation vmem-f3  --dir DIR [--nq N] [--hops H]\n"
               "  motivation vmem-f4|f5|f6|f7|f8|f9 --dir DIR [...]\n"
               "  motivation vmem-mot12 --dir DIR  # §1.2 new exps\n"
               "  motivation vmem-mot-me --dir DIR  # M-E1/3/4/5 importance\n"
               "  motivation vmem-verify --dir DIR  # prove CXL-SSD path + counters\n"
               "  motivation vmem-mot-ef --dir DIR  # efSearch 100..1600 sweep\n"
               "  motivation vmem-mot-pin --dir DIR # efSearch pin vs nopin\n"
               "  motivation me2-f2 --dir DIR  # M-E2 small-dim record vs page\n");
}

struct Args {
  std::string cmd;
  std::string dir = "data";
  uint32_t n = 500000;
  uint32_t dim = 128;
  uint32_t degree = 32;
  uint32_t nq = 200;
  int beam = 16;
  int hops = 64;
  int cache_mb = 64;
  int node = 1;  // CXL far-memory node on gpu01 (verify with numactl -H)
  uint64_t seed = 42;
  int iters = 2000;
  uint32_t max_n = 0;  // 0 = all
};

static Args parse(int argc, char** argv) {
  Args a;
  if (argc < 2) {
    usage();
    std::exit(1);
  }
  a.cmd = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string s = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) die(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (s == "--dir") a.dir = need("--dir");
    else if (s == "--n") a.n = std::stoul(need("--n"));
    else if (s == "--dim") a.dim = std::stoul(need("--dim"));
    else if (s == "--degree") a.degree = std::stoul(need("--degree"));
    else if (s == "--nq") a.nq = std::stoul(need("--nq"));
    else if (s == "--beam") a.beam = std::stoi(need("--beam"));
    else if (s == "--hops") a.hops = std::stoi(need("--hops"));
    else if (s == "--cache-mb") a.cache_mb = std::stoi(need("--cache-mb"));
    else if (s == "--node") a.node = std::stoi(need("--node"));
    else if (s == "--seed") a.seed = std::stoull(need("--seed"));
    else if (s == "--iters") a.iters = std::stoi(need("--iters"));
    else if (s == "--max-n") a.max_n = std::stoul(need("--max-n"));
    else die("unknown arg: " + s);
  }
  return a;
}

static std::vector<float> load_query_vectors(VectorStoreSSD& ssd,
                                             const std::vector<uint32_t>& qids,
                                             Stats& st) {
  std::vector<float> out(qids.size() * ssd.meta.dim);
  for (size_t i = 0; i < qids.size(); ++i)
    ssd.pread_vec(qids[i], out.data() + i * ssd.meta.dim, st);
  return out;
}

static void run_workload(FetchCtx& ctx, const std::vector<uint32_t>& qids,
                         const std::vector<float>& qvecs, int beam, int hops,
                         const char* tag) {
  Stats& st = *ctx.st;
  st = Stats{};
  Timer t;
  for (size_t i = 0; i < qids.size(); ++i) {
    const float* q = qvecs.data() + i * ctx.g->meta.dim;
    beam_search_one(ctx, q, ctx.g->meta.entry, beam, hops, nullptr);
    st.queries++;
  }
  st.seconds = t.elapsed();
  st.print(tag);
}

static void cmd_gen(const Args& a) {
  generate_dataset(a.dir, a.n, a.dim, a.degree, a.seed);
}

static size_t cache_vecs(const Args& a, uint32_t dim) {
  size_t bytes = size_t(a.cache_mb) * 1024ull * 1024ull;
  size_t vb = size_t(dim) * sizeof(float);
  return std::max<size_t>(1, bytes / vb);
}

static void cmd_f1(const Args& a) {
  Graph g;
  g.load(a.dir);
  VectorStoreSSD ssd;
  ssd.open_file(a.dir, true);

  Stats warm_st;
  auto qids = make_queries(g.meta.n, a.nq, a.seed + 1, true);
  auto qvecs = load_query_vectors(ssd, qids, warm_st);

  size_t cap = cache_vecs(a, g.meta.dim);
  std::printf("# F1: unified-VA false friend | n=%u cache_vecs=%zu (~~%dMB) "
              "beam=%d hops=%d node=%d\n",
              g.meta.n, cap, a.cache_mb, a.beam, a.hops, a.node);

  RecordCache rec;
  rec.init(g.meta.dim, cap, a.node);
  FetchCtx ctx;
  ctx.g = &g;
  ctx.ssd = &ssd;
  ctx.rec = &rec;
  Stats st;
  ctx.st = &st;
  ctx.ensure_tmp(g.meta.dim);

  // 1) Always SSD explicit (no CXL)
  ctx.mode = FetchCtx::EXPLICIT_SSD;
  ctx.rec = nullptr;
  run_workload(ctx, qids, qvecs, a.beam, a.hops, "F1-explicit_ssd");

  // 2) Sync per-miss promote into CXL (fault-like / pointer chase)
  ctx.rec = &rec;
  // reset cache between modes
  rec.destroy();
  rec.init(g.meta.dim, cap, a.node);
  ctx.mode = FetchCtx::SYNC_PTR_FAULT;
  run_workload(ctx, qids, qvecs, a.beam, a.hops, "F1-sync_ptr_fault");

  // 3) Batch promote per beam hop (ANNS-aware)
  rec.destroy();
  rec.init(g.meta.dim, cap, a.node);
  ctx.mode = FetchCtx::BATCH_PROMOTE;
  run_workload(ctx, qids, qvecs, a.beam, a.hops, "F1-batch_promote");

  rec.destroy();
  ssd.close_file();
}

static void cmd_f2(const Args& a) {
  Graph g;
  g.load(a.dir);
  VectorStoreSSD ssd;
  ssd.open_file(a.dir, true);

  Stats warm_st;
  auto qids = make_queries(g.meta.n, a.nq, a.seed + 2, true);
  auto qvecs = load_query_vectors(ssd, qids, warm_st);

  size_t budget = size_t(a.cache_mb) * 1024ull * 1024ull;
  size_t cap_recs = std::max<size_t>(1, budget / (size_t(g.meta.dim) * 4));
  size_t cap_pages = std::max<size_t>(1, budget / PageCache::kPage);

  std::printf("# F2: page hit vs semantic next-hop hit | budget=%dMB "
              "rec_cap=%zu page_cap=%zu\n",
              a.cache_mb, cap_recs, cap_pages);

  RecordCache rec;
  PageCache page;
  FetchCtx ctx;
  ctx.g = &g;
  ctx.ssd = &ssd;
  Stats st;
  ctx.st = &st;
  ctx.ensure_tmp(g.meta.dim);

  // Page cache mode
  page.init(ssd.vec_bytes, cap_pages, a.node);
  ctx.page = &page;
  ctx.rec = nullptr;
  ctx.mode = FetchCtx::PAGE_CACHE;
  run_workload(ctx, qids, qvecs, a.beam, a.hops, "F2-page_cache");
  // semantic_next is 0 in page mode; compute proxy: reuse same queries with recording?
  page.destroy();

  // Record cache mode
  rec.init(g.meta.dim, cap_recs, a.node);
  ctx.rec = &rec;
  ctx.page = nullptr;
  ctx.mode = FetchCtx::RECORD_CACHE;
  run_workload(ctx, qids, qvecs, a.beam, a.hops, "F2-record_cache");

  // Dual metric run: page cache but also track whether next neighbors are in
  // a shadow record set mirroring page-resident vectors — approximate semantic.
  // Simpler confirmation: print ratio semantic_next_hit from record mode vs
  // cxl hit%% from page mode (already printed).

  rec.destroy();
  ssd.close_file();
}

static void cmd_f3(const Args& a) {
  Graph g;
  g.load(a.dir);
  VectorStoreSSD ssd;
  ssd.open_file(a.dir, true);

  size_t cap = cache_vecs(a, g.meta.dim);
  std::printf("# F3: beam x hit-rate coupling | cache_vecs=%zu\n", cap);
  std::printf("dist,beam,qps,hit_pct,ssd_MB,ssd_reads\n");

  RecordCache rec;
  FetchCtx ctx;
  ctx.g = &g;
  ctx.ssd = &ssd;
  ctx.rec = &rec;
  Stats st;
  ctx.st = &st;
  ctx.ensure_tmp(g.meta.dim);
  ctx.mode = FetchCtx::SYNC_PTR_FAULT;

  for (const char* dist_name : {"zipf", "uniform"}) {
    bool zipf = std::string(dist_name) == "zipf";
    Stats warm;
    auto qids = make_queries(g.meta.n, a.nq, a.seed + (zipf ? 3 : 4), zipf);
    auto qvecs = load_query_vectors(ssd, qids, warm);

    for (int beam : {4, 8, 16, 32, 64}) {
      rec.destroy();
      rec.init(g.meta.dim, cap, a.node);
      ctx.rec = &rec;
      run_workload(ctx, qids, qvecs, beam, a.hops, "F3");
      double hit = (st.cxl_hits + st.cxl_misses)
                       ? 100.0 * st.cxl_hits / (st.cxl_hits + st.cxl_misses)
                       : 0.0;
      double qps = st.queries / std::max(st.seconds, 1e-9);
      std::printf("%s,%d,%.2f,%.2f,%.2f,%lu\n", dist_name, beam, qps, hit,
                  st.ssd_bytes / (1024.0 * 1024.0),
                  (unsigned long)st.ssd_reads);
      std::fflush(stdout);
    }
  }
  rec.destroy();
  ssd.close_file();
}

static void cmd_vmem_identity(const Args&) {
  require_cxl_vmem_identity();
  VmemArena arena;
  arena.open_dev();
  arena.close_dev();
}

static void cmd_vmem_populate(const Args& a) {
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  uint32_t lim = a.max_n ? a.max_n : 0;
  // Allow --n as populate limit when not left at gen-default unused.
  if (!lim && a.n && a.n != 500000) lim = a.n;
  auto c0 = read_vmem_counters();
  arena.populate_from_file(a.dir, lim);
  auto c1 = read_vmem_counters();
  print_counter_delta("populate", c0, c1);
  arena.close_dev();
}

static void cmd_vmem_h0(const Args& a) {
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  const uint32_t dim = arena.hdr.dim;
  const uint32_t n = arena.hdr.n;
  std::mt19937_64 rng(a.seed);
  std::uniform_int_distribution<uint32_t> u(0, n - 1);

  // Warm a set into vmem page cache
  std::vector<uint32_t> warm_ids;
  for (int i = 0; i < 256; ++i) warm_ids.push_back(u(rng));
  for (uint32_t id : warm_ids) {
    volatile float sink = 0;
    const float* p = arena.vec_ptr(id, dim);
    for (uint32_t d = 0; d < dim; ++d) sink += p[d];
    (void)sink;
  }

  auto bench_touch = [&](const char* tag, bool cold) {
    std::vector<uint32_t> ids;
    if (cold) {
      // Prefer ids not in warm set
      while (ids.size() < size_t(a.iters)) {
        uint32_t id = u(rng);
        bool bad = false;
        for (uint32_t w : warm_ids)
          if (w == id) {
            bad = true;
            break;
          }
        if (!bad) ids.push_back(id);
      }
    } else {
      for (int i = 0; i < a.iters; ++i) ids.push_back(warm_ids[i % warm_ids.size()]);
    }
    auto c0 = read_vmem_counters();
    Timer t;
    double sink = 0;
    for (uint32_t id : ids) {
      const float* p = arena.vec_ptr(id, dim);
      for (uint32_t d = 0; d < dim; ++d) sink += p[d];
    }
    double sec = t.elapsed();
    auto c1 = read_vmem_counters();
    double ns = sec * 1e9 / ids.size();
    std::printf("[H0-%s] ns_per_vec=%.1f sink=%.1f\n", tag, ns, sink);
    print_counter_delta(tag, c0, c1);
  };

  bench_touch("vmem_warm_hit", false);
  arena.drop_host_ptes();
  arena.thrash_soft_cache(5ull << 30);
  arena.drop_host_ptes();
  bench_touch("vmem_cold_miss", true);

  // Compare classic O_DIRECT on host file (not unified AS)
  VectorStoreSSD ssd;
  ssd.open_file(a.dir, true);
  Stats st;
  std::vector<uint32_t> ids;
  for (int i = 0; i < a.iters; ++i) ids.push_back(u(rng));
  std::vector<float> tmp(dim);
  Timer t;
  for (uint32_t id : ids) ssd.pread_vec(id, tmp.data(), st);
  double ns = t.elapsed() * 1e9 / ids.size();
  std::printf("[H0-odirect_file] ns_per_vec=%.1f ssd_reads=%lu\n", ns,
              (unsigned long)st.ssd_reads);
  ssd.close_file();
  arena.close_dev();
}

static void cmd_vmem_f1(const Args& a) {
  // F1: demand serial fault vs parallel 1-hop vs graph-aware 2-hop prefetch.
  // Prefetch ioctl is sync & fills soft-cache (not PTEs). Wins come from:
  //  (1) parallelizing miss I/O across neighbors
  //  (2) prefetching next-hop neighbors of top-correlated nodes during/after
  //      current hop so subsequent expansions hit.
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  if (arena.hdr.n != g.meta.n || arena.hdr.dim != g.meta.dim)
    die("vmem header meta mismatch vs graph dir — repopulate");

  const uint32_t dim = g.meta.dim;
  const uint32_t degree = g.meta.degree;
  const int spec_top = std::min(8, a.beam);  // graph-aware: top-L neighbors

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 11, true);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                dim * sizeof(float));

  enum Mode { DEMAND = 0, PARALLEL_1HOP = 1, GRAPH_2HOP = 2 };

  auto run_mode = [&](const char* tag, Mode mode) {
    arena.set_prefetch_off();
    Stats st;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t qi = 0; qi < qids.size(); ++qi) {
      const float* query = qvecs.data() + qi * dim;
      std::priority_queue<Cand> best;
      std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
      std::unordered_set<uint32_t> visited;
      visited.reserve(size_t(a.beam) * 8);

      auto consider = [&](uint32_t id, float dist) {
        if (visited.count(id)) return;
        visited.insert(id);
        frontier.push({dist, id});
        best.push({dist, id});
        if ((int)best.size() > a.beam) best.pop();
      };

      auto touch_one = [&](uint32_t id) -> const float* {
        st.vector_fetches++;
        if (arena.page_resident(id, dim)) st.cxl_hits++;
        else st.cxl_misses++;
        return arena.touch_vec(id, dim);
      };

      touch_one(g.meta.entry);
      consider(g.meta.entry, l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));

      int hops = 0;
      while (!frontier.empty() && hops < a.hops) {
        Cand cur = frontier.top();
        frontier.pop();
        st.hops++;
        hops++;
        const uint32_t* nbrs = g.neighbors(cur.id);

        // --- ensure 1-hop neighbors resident ---
        if (mode == DEMAND) {
          for (uint32_t k = 0; k < degree; ++k) touch_one(nbrs[k]);
        } else {
          // Count residency before touch (fair hit%); parallelize I/O.
          std::vector<char> was_hit(degree, 0);
          for (uint32_t k = 0; k < degree; ++k) {
            st.vector_fetches++;
            was_hit[k] = arena.page_resident(nbrs[k], dim) ? 1 : 0;
            if (was_hit[k]) st.cxl_hits++;
            else st.cxl_misses++;
          }
#pragma omp parallel for schedule(static) if (degree > 4)
          for (int k = 0; k < (int)degree; ++k)
            arena.touch_vec(nbrs[k], dim);
        }

        // distance + frontier update; track (dist, id) for graph prefetch
        std::vector<Cand> ranked;
        ranked.reserve(degree);
        for (uint32_t k = 0; k < degree; ++k) {
          uint32_t id = nbrs[k];
          st.semantic_next_total++;
          if (arena.page_resident(id, dim)) st.semantic_next_hits++;
          float dist = l2_sq(query, arena.vec_ptr(id, dim), dim);
          ranked.push_back({dist, id});
          if ((int)best.size() < a.beam || dist < best.top().dist)
            consider(id, dist);
          else
            visited.insert(id);
        }

        // --- graph-aware 2-hop: soft-cache fill for top-nbrs' adjacency ---
        // Do NOT touch/PTE-install now — that would bloat the 4GiB cache.
        // Next hop demand faults should hit soft cache (cheaper than SSD).
        if (mode == GRAPH_2HOP) {
          std::partial_sort(
              ranked.begin(),
              ranked.begin() + std::min(spec_top, (int)ranked.size()),
              ranked.end(),
              [](const Cand& x, const Cand& y) { return x.dist < y.dist; });
          std::vector<uint32_t> spec;
          spec.reserve(size_t(spec_top) * degree);
          int lim = std::min(spec_top, (int)ranked.size());
          for (int i = 0; i < lim; ++i) {
            const uint32_t* n2 = g.neighbors(ranked[i].id);
            for (uint32_t k = 0; k < degree; ++k) {
              if (!visited.count(n2[k])) spec.push_back(n2[k]);
            }
          }
          std::sort(spec.begin(), spec.end());
          spec.erase(std::unique(spec.begin(), spec.end()), spec.end());
          arena.prefetch_ids(spec.data(), spec.size(), dim);
        }
      }
      st.queries++;
    }
    st.seconds = t.elapsed();
    auto c1 = read_vmem_counters();
    st.ssd_reads = c1.read_ios - c0.read_ios;
    st.ssd_bytes = st.ssd_reads * 4096ull;
    st.print(tag);
    print_counter_delta(tag, c0, c1);
  };

  std::printf("# VMEM-F1 graph-prefetch | n=%u dim=%u beam=%d hops=%d nq=%u "
              "spec_top=%d\n",
              g.meta.n, dim, a.beam, a.hops, a.nq, spec_top);
  arena.cold_start();
  run_mode("VMEM-F1-demand_serial", DEMAND);
  arena.cold_start();
  run_mode("VMEM-F1-parallel_1hop", PARALLEL_1HOP);
  arena.cold_start();
  run_mode("VMEM-F1-graph_2hop", GRAPH_2HOP);
  arena.close_dev();
}

static void cmd_vmem_f2(const Args& a) {
  // F2: record (1×4KiB vec) vs "page" (4 consecutive vecs = 16KiB) admission.
  // Same byte budget in a userspace residency simulator on top of vmem touches.
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  if (arena.hdr.n != g.meta.n) die("meta mismatch");

  const uint32_t dim = g.meta.dim;
  const size_t vec_bytes = size_t(dim) * 4;
  const size_t page_vecs = 4;  // artificial page = 4 records
  const size_t budget = size_t(a.cache_mb) * 1024ull * 1024ull;
  const size_t rec_cap = std::max<size_t>(1, budget / vec_bytes);
  const size_t page_cap = std::max<size_t>(1, budget / (vec_bytes * page_vecs));

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 22, true);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                dim * sizeof(float));

  enum Pol { RECORD = 0, PAGE = 1 };

  auto run = [&](const char* tag, Pol pol) {
    arena.cold_start();
    std::unordered_map<uint64_t, int> resident;  // key -> lru tick
    std::list<uint64_t> lru;
    auto evict = [&]() {
      while (!lru.empty()) {
        uint64_t k = lru.front();
        lru.pop_front();
        if (resident.erase(k)) return;
      }
    };
    auto admit = [&](uint64_t key, size_t cap) {
      if (resident.count(key)) {
        resident[key]++;
        lru.push_back(key);
        return true;  // hit
      }
      while (resident.size() >= cap) evict();
      resident[key] = 1;
      lru.push_back(key);
      return false;  // miss — need touch
    };

    Stats st;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t qi = 0; qi < qids.size(); ++qi) {
      const float* query = qvecs.data() + qi * dim;
      std::priority_queue<Cand> best;
      std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
      std::unordered_set<uint32_t> visited;
      auto consider = [&](uint32_t id, float dist) {
        if (visited.count(id)) return;
        visited.insert(id);
        frontier.push({dist, id});
        best.push({dist, id});
        if ((int)best.size() > a.beam) best.pop();
      };
      auto fetch = [&](uint32_t id) {
        st.vector_fetches++;
        bool hit = false;
        if (pol == RECORD) {
          hit = admit(id, rec_cap);
          if (!hit) {
            st.cxl_misses++;
            arena.touch_vec(id, dim);
          } else {
            st.cxl_hits++;
            if (!arena.page_resident(id, dim)) arena.touch_vec(id, dim);
          }
        } else {
          uint64_t blk = id / page_vecs;
          hit = admit(blk, page_cap);
          uint32_t base = uint32_t(blk * page_vecs);
          if (!hit) {
            st.cxl_misses++;
            st.page_misses++;
            for (size_t j = 0; j < page_vecs && base + j < g.meta.n; ++j)
              arena.touch_vec(base + uint32_t(j), dim);
          } else {
            st.cxl_hits++;
            st.page_hits++;
            if (!arena.page_resident(id, dim)) arena.touch_vec(id, dim);
          }
        }
        st.semantic_next_total++;
        // semantic: is this exact vector's key considered resident?
        if (pol == RECORD) {
          if (resident.count(id)) st.semantic_next_hits++;
        } else {
          if (resident.count(id / page_vecs)) st.semantic_next_hits++;
        }
        return arena.vec_ptr(id, dim);
      };

      fetch(g.meta.entry);
      consider(g.meta.entry, l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));
      int hops = 0;
      while (!frontier.empty() && hops < a.hops) {
        Cand cur = frontier.top();
        frontier.pop();
        st.hops++;
        hops++;
        const uint32_t* nbrs = g.neighbors(cur.id);
        for (uint32_t k = 0; k < g.meta.degree; ++k) {
          uint32_t id = nbrs[k];
          const float* v = fetch(id);
          float dist = l2_sq(query, v, dim);
          if ((int)best.size() < a.beam || dist < best.top().dist)
            consider(id, dist);
          else
            visited.insert(id);
        }
      }
      st.queries++;
    }
    st.seconds = t.elapsed();
    auto c1 = read_vmem_counters();
    st.ssd_reads = c1.read_ios - c0.read_ios;
    st.ssd_bytes = st.ssd_reads * 4096ull;
    st.print(tag);
    print_counter_delta(tag, c0, c1);
    std::printf("  policy_cap rec=%zu page_blocks=%zu budget_MB=%d\n", rec_cap,
                page_cap, a.cache_mb);
  };

  std::printf("# VMEM-F2 record vs 16KiB-page | budget=%dMB beam=%d\n",
              a.cache_mb, a.beam);
  run("VMEM-F2-record", RECORD);
  run("VMEM-F2-page16k", PAGE);
  arena.close_dev();
}

static void cmd_vmem_f3(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  if (arena.hdr.n != g.meta.n) die("meta mismatch");
  const uint32_t dim = g.meta.dim;

  std::printf("# VMEM-F3 beam x hit | hops=%d nq=%u\n", a.hops, a.nq);
  std::printf("dist,beam,qps,hit_pct,ssd_reads,faults,time\n");

  for (const char* dist_name : {"zipf", "uniform"}) {
    bool zipf = std::string(dist_name) == "zipf";
    auto qids = make_queries(g.meta.n, a.nq, a.seed + (zipf ? 31 : 32), zipf);
    std::vector<float> qvecs(qids.size() * dim);
    for (size_t i = 0; i < qids.size(); ++i)
      std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                  dim * sizeof(float));

    for (int beam : {4, 8, 16, 32, 64}) {
      arena.cold_start();
      Stats st;
      auto c0 = read_vmem_counters();
      Timer t;
      for (size_t qi = 0; qi < qids.size(); ++qi) {
        const float* query = qvecs.data() + qi * dim;
        std::priority_queue<Cand> best;
        std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
        std::unordered_set<uint32_t> visited;
        auto consider = [&](uint32_t id, float dist) {
          if (visited.count(id)) return;
          visited.insert(id);
          frontier.push({dist, id});
          best.push({dist, id});
          if ((int)best.size() > beam) best.pop();
        };
        auto touch = [&](uint32_t id) {
          st.vector_fetches++;
          if (arena.page_resident(id, dim)) st.cxl_hits++;
          else st.cxl_misses++;
          return arena.touch_vec(id, dim);
        };
        touch(g.meta.entry);
        consider(g.meta.entry,
                 l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));
        int hops = 0;
        while (!frontier.empty() && hops < a.hops) {
          Cand cur = frontier.top();
          frontier.pop();
          st.hops++;
          hops++;
          const uint32_t* nbrs = g.neighbors(cur.id);
          for (uint32_t k = 0; k < g.meta.degree; ++k) {
            uint32_t id = nbrs[k];
            const float* v = touch(id);
            float dist = l2_sq(query, v, dim);
            if ((int)best.size() < beam || dist < best.top().dist)
              consider(id, dist);
            else
              visited.insert(id);
          }
        }
        st.queries++;
      }
      st.seconds = t.elapsed();
      auto c1 = read_vmem_counters();
      double hit = (st.cxl_hits + st.cxl_misses)
                       ? 100.0 * st.cxl_hits / (st.cxl_hits + st.cxl_misses)
                       : 0.0;
      double qps = st.queries / std::max(st.seconds, 1e-9);
      std::printf("%s,%d,%.2f,%.2f,%llu,%llu,%.3f\n", dist_name, beam, qps, hit,
                  (unsigned long long)(c1.read_ios - c0.read_ios),
                  (unsigned long long)(c1.faults - c0.faults), st.seconds);
      std::fflush(stdout);
    }
  }
  arena.close_dev();
}

// ----- F4: stall-amplified recall / efficiency -----
static void cmd_vmem_f4(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  if (arena.hdr.n != g.meta.n) die("meta mismatch");
  const uint32_t dim = g.meta.dim;
  const int k = 10;

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 41, true);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                dim * sizeof(float));

  std::printf("# F4 loading host GT (brute top-%d)...\n", k);
  auto host = load_host_vectors(a.dir, g.meta.n, dim);
  std::vector<std::vector<uint32_t>> gt(qids.size());
  for (size_t i = 0; i < qids.size(); ++i)
    gt[i] = brute_topk(host.data(), g.meta.n, dim, qvecs.data() + i * dim, k);

  std::printf("# VMEM-F4 recall efficiency | nq=%u hops=%d k=%d\n", a.nq, a.hops,
              k);
  std::printf(
      "beam,qps,hit_pct,recall@10,ssd_reads,time,recall_per_s,recall_per_kio\n");

  for (int beam : {4, 8, 16, 32, 64}) {
    arena.cold_start();
    Stats agg;
    double rec_sum = 0;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t i = 0; i < qids.size(); ++i) {
      auto bo = vmem_beam_search(arena, g, qvecs.data() + i * dim, beam, a.hops,
                                 k);
      rec_sum += recall_at(bo.ids, gt[i], k);
      agg.queries++;
      agg.hops += bo.st.hops;
      agg.vector_fetches += bo.st.vector_fetches;
      agg.cxl_hits += bo.st.cxl_hits;
      agg.cxl_misses += bo.st.cxl_misses;
    }
    agg.seconds = t.elapsed();
    auto c1 = read_vmem_counters();
    uint64_t reads = c1.read_ios - c0.read_ios;
    double hit = (agg.cxl_hits + agg.cxl_misses)
                     ? 100.0 * agg.cxl_hits / (agg.cxl_hits + agg.cxl_misses)
                     : 0;
    double qps = agg.queries / std::max(agg.seconds, 1e-9);
    double recall = rec_sum / qids.size();
    double rps = recall * qps;  // recall-weighted throughput
    double rpk =
        reads ? (recall * agg.queries) / (reads / 1000.0) : 0;  // per 1k IOs
    std::printf("%d,%.2f,%.2f,%.4f,%llu,%.3f,%.3f,%.3f\n", beam, qps, hit,
                recall, (unsigned long long)reads, agg.seconds, rps, rpk);
    std::fflush(stdout);
  }
  arena.close_dev();
}

// ----- F5: promote precision vs volume -----
static void cmd_vmem_f5(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  const uint32_t dim = g.meta.dim;
  const uint32_t degree = g.meta.degree;
  const int beam = a.beam > 0 ? a.beam : 32;

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 51, true);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                dim * sizeof(float));

  enum Mode { DEMAND, TOP1_NBR, TOP2_2HOP, TOP8_2HOP };

  auto run = [&](const char* tag, Mode mode) {
    arena.cold_start();
    Stats st;
    uint64_t pref_pages = 0, pref_used = 0;
    std::unordered_set<uint32_t> pref_ids;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t qi = 0; qi < qids.size(); ++qi) {
      const float* query = qvecs.data() + qi * dim;
      std::priority_queue<Cand> best;
      std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
      std::unordered_set<uint32_t> visited;
      auto consider = [&](uint32_t id, float dist) {
        if (visited.count(id)) return;
        visited.insert(id);
        frontier.push({dist, id});
        best.push({dist, id});
        if ((int)best.size() > beam) best.pop();
      };
      auto touch = [&](uint32_t id) {
        st.vector_fetches++;
        if (pref_ids.count(id)) {
          pref_used++;
          pref_ids.erase(id);
        }
        if (arena.page_resident(id, dim)) st.cxl_hits++;
        else st.cxl_misses++;
        return arena.touch_vec(id, dim);
      };
      auto prefetch_set = [&](const std::vector<uint32_t>& ids) {
        for (uint32_t id : ids) {
          if (visited.count(id) || pref_ids.count(id)) continue;
          pref_ids.insert(id);
          pref_pages++;
          arena.prefetch_ids(&id, 1, dim);
        }
      };

      touch(g.meta.entry);
      consider(g.meta.entry,
               l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));
      int hops = 0;
      while (!frontier.empty() && hops < a.hops) {
        Cand cur = frontier.top();
        frontier.pop();
        st.hops++;
        hops++;
        const uint32_t* nbrs = g.neighbors(cur.id);
        for (uint32_t k = 0; k < degree; ++k) touch(nbrs[k]);
        std::vector<Cand> ranked;
        ranked.reserve(degree);
        for (uint32_t k = 0; k < degree; ++k) {
          uint32_t id = nbrs[k];
          float dist = l2_sq(query, arena.vec_ptr(id, dim), dim);
          ranked.push_back({dist, id});
          if ((int)best.size() < beam || dist < best.top().dist)
            consider(id, dist);
          else
            visited.insert(id);
        }
        if (mode == DEMAND) continue;
        int L = (mode == TOP1_NBR) ? 1 : (mode == TOP2_2HOP) ? 2 : 8;
        L = std::min(L, (int)ranked.size());
        std::partial_sort(ranked.begin(), ranked.begin() + L, ranked.end(),
                          [](const Cand& x, const Cand& y) {
                            return x.dist < y.dist;
                          });
        std::vector<uint32_t> spec;
        for (int i = 0; i < L; ++i) {
          const uint32_t* n2 = g.neighbors(ranked[i].id);
          for (uint32_t k = 0; k < degree; ++k) spec.push_back(n2[k]);
        }
        std::sort(spec.begin(), spec.end());
        spec.erase(std::unique(spec.begin(), spec.end()), spec.end());
        prefetch_set(spec);
      }
      st.queries++;
    }
    st.seconds = t.elapsed();
    auto c1 = read_vmem_counters();
    st.ssd_reads = c1.read_ios - c0.read_ios;
    st.ssd_bytes = st.ssd_reads * 4096ull;
    st.print(tag);
    print_counter_delta(tag, c0, c1);
    double prec = pref_pages ? 100.0 * pref_used / pref_pages : 0;
    std::printf("  promote_pages=%llu used=%llu precision%%=%.2f unused=%llu\n",
                (unsigned long long)pref_pages, (unsigned long long)pref_used,
                prec, (unsigned long long)(pref_pages - pref_used));
  };

  std::printf("# VMEM-F5 promote precision | beam=%d hops=%d nq=%u\n", beam,
              a.hops, a.nq);
  run("VMEM-F5-demand", DEMAND);
  run("VMEM-F5-top1_2hop", TOP1_NBR);
  run("VMEM-F5-top2_2hop", TOP2_2HOP);
  run("VMEM-F5-top8_2hop", TOP8_2HOP);
  arena.close_dev();
}

// ----- F6: graph / hub admission vs page / record -----
static void cmd_vmem_f6(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  const uint32_t dim = g.meta.dim;
  const int beam = a.beam > 0 ? a.beam : 32;

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 61, true);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                dim * sizeof(float));

  size_t hub_n =
      std::max<size_t>(1, (size_t(a.cache_mb) * 1024ull * 1024ull) /
                              (size_t(dim) * 4) / 4);  // 25% of record cap
  auto hubs = compute_indegree_hubs(g, hub_n);

  auto run = [&](const char* tag, AdmitPolicy::Kind kind) {
    arena.cold_start();
    AdmitPolicy pol;
    pol.kind = kind;
    pol.budget_bytes = size_t(a.cache_mb) << 20;
    pol.reset_cache(dim);
    if (kind == AdmitPolicy::HUB_PIN) {
      for (uint32_t id : hubs) pol.pinned.insert(id);
      // warm pins once
      for (uint32_t id : hubs) arena.touch_vec(id, dim);
    }
    Stats agg;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t i = 0; i < qids.size(); ++i) {
      auto bo = vmem_beam_search(arena, g, qvecs.data() + i * dim, beam, a.hops,
                                 10, &pol);
      agg.queries++;
      agg.hops += bo.st.hops;
      agg.vector_fetches += bo.st.vector_fetches;
      agg.cxl_hits += bo.st.cxl_hits;
      agg.cxl_misses += bo.st.cxl_misses;
      agg.page_hits += bo.st.page_hits;
      agg.page_misses += bo.st.page_misses;
    }
    agg.seconds = t.elapsed();
    auto c1 = read_vmem_counters();
    agg.ssd_reads = c1.read_ios - c0.read_ios;
    agg.ssd_bytes = agg.ssd_reads * 4096ull;
    agg.print(tag);
    print_counter_delta(tag, c0, c1);
    std::printf("  promote_pages=%llu cap=%zu pinned=%zu\n",
                (unsigned long long)pol.promote_pages, pol.cap,
                pol.pinned.size());
  };

  std::printf("# VMEM-F6 admission | budget=%dMB beam=%d nq=%u hubs=%zu\n",
              a.cache_mb, beam, a.nq, hubs.size());
  run("VMEM-F6-record_lru", AdmitPolicy::RECORD_LRU);
  run("VMEM-F6-page16k", AdmitPolicy::PAGE16K);
  run("VMEM-F6-graph_expand", AdmitPolicy::GRAPH_EXPAND);
  run("VMEM-F6-hub_pin", AdmitPolicy::HUB_PIN);
  arena.close_dev();
}

// ----- F7: shared hot-set vs per-query beam -----
static void cmd_vmem_f7(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  const uint32_t dim = g.meta.dim;

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 71, true);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                dim * sizeof(float));

  // Profile pass: frequency of touched IDs under beam=32 (DRAM-side count via
  // graph walk with host vectors — cheap; approximates hot set).
  auto host = load_host_vectors(a.dir, g.meta.n, dim);
  std::unordered_map<uint32_t, uint32_t> freq;
  for (size_t qi = 0; qi < qids.size(); ++qi) {
    const float* query = qvecs.data() + qi * dim;
    std::priority_queue<Cand> best;
    std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
    std::unordered_set<uint32_t> visited;
    auto consider = [&](uint32_t id, float dist) {
      if (visited.count(id)) return;
      visited.insert(id);
      frontier.push({dist, id});
      best.push({dist, id});
      if ((int)best.size() > 32) best.pop();
    };
    auto touch = [&](uint32_t id) {
      freq[id]++;
      return host.data() + size_t(id) * dim;
    };
    touch(g.meta.entry);
    consider(g.meta.entry, l2_sq(query, touch(g.meta.entry), dim));
    int hops = 0;
    while (!frontier.empty() && hops < a.hops) {
      Cand cur = frontier.top();
      frontier.pop();
      hops++;
      const uint32_t* nbrs = g.neighbors(cur.id);
      for (uint32_t k = 0; k < g.meta.degree; ++k) {
        uint32_t id = nbrs[k];
        float dist = l2_sq(query, touch(id), dim);
        if ((int)best.size() < 32 || dist < best.top().dist)
          consider(id, dist);
        else
          visited.insert(id);
      }
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> items(freq.begin(), freq.end());
  std::sort(items.begin(), items.end(),
            [](auto& x, auto& y) { return x.second > y.second; });
  size_t pin_budget =
      size_t(a.cache_mb) * 1024ull * 1024ull / (size_t(dim) * 4) / 2;
  size_t pin_n = std::min(items.size(), pin_budget);
  std::unordered_set<uint32_t> shared;
  for (size_t i = 0; i < pin_n; ++i) shared.insert(items[i].first);

  auto run = [&](const char* tag, int beam, const std::unordered_set<uint32_t>* pin) {
    arena.cold_start();
    if (pin) {
      for (uint32_t id : *pin) arena.touch_vec(id, dim);
    }
    Stats agg;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t i = 0; i < qids.size(); ++i) {
      auto bo = vmem_beam_search(arena, g, qvecs.data() + i * dim, beam, a.hops,
                                 10, nullptr, pin);
      agg.queries++;
      agg.cxl_hits += bo.st.cxl_hits;
      agg.cxl_misses += bo.st.cxl_misses;
      agg.vector_fetches += bo.st.vector_fetches;
      agg.hops += bo.st.hops;
    }
    agg.seconds = t.elapsed();
    auto c1 = read_vmem_counters();
    agg.ssd_reads = c1.read_ios - c0.read_ios;
    agg.ssd_bytes = agg.ssd_reads * 4096ull;
    agg.print(tag);
    print_counter_delta(tag, c0, c1);
  };

  std::printf("# VMEM-F7 shared pin=%zu (~%dMB/2) nq=%u | controlled beam\n",
              shared.size(), a.cache_mb, a.nq);
  // Control variable: same beam, pin vs no-pin
  run("VMEM-F7-beam8_nopin", 8, nullptr);
  run("VMEM-F7-beam8_shared", 8, &shared);
  run("VMEM-F7-beam32_nopin", 32, nullptr);
  run("VMEM-F7-beam32_shared", 32, &shared);
  arena.close_dev();
}

// ----- F8: soft-cache vs PTE two-level cliff -----
static void cmd_vmem_f8(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  const uint32_t dim = g.meta.dim;
  const int nprobe = std::min(a.iters, 800);

  auto qids = make_queries(g.meta.n, uint32_t(nprobe), a.seed + 81, false);
  // Use distinct ids as probe set
  std::vector<uint32_t> ids = qids;

  auto measure = [&](const char* tag) {
    auto c0 = read_vmem_counters();
    Timer t;
    for (int i = 0; i < nprobe; ++i) {
      volatile float s = 0;
      const float* p = arena.vec_ptr(ids[i], dim);
      for (uint32_t d = 0; d < dim; ++d) s += p[d];
      (void)s;
    }
    double sec = t.elapsed();
    auto c1 = read_vmem_counters();
    double ns = sec * 1e9 / nprobe;
    std::printf("[%s] n=%d ns/vec=%.1f Δfaults=%llu Δread_ios=%llu time=%.4fs\n",
                tag, nprobe, ns,
                (unsigned long long)(c1.faults - c0.faults),
                (unsigned long long)(c1.read_ios - c0.read_ios), sec);
  };

  std::printf("# VMEM-F8 two-level cliff | probes=%d\n", nprobe);

  // (1) full cold: thrash + drop PTEs
  arena.cold_start();
  measure("F8-cold_ssd");

  // (2) warm PTE + soft cache
  for (int i = 0; i < nprobe; ++i) arena.touch_vec(ids[i], dim);
  measure("F8-warm_pte");

  // (3) drop host PTEs only — soft cache should still hold pages
  arena.drop_host_ptes();
  measure("F8-softcache_pte_cold");

  // (4) thrash soft cache but leave... actually thrash then touch = cold again
  arena.thrash_soft_cache(5ull << 30);
  arena.drop_host_ptes();
  measure("F8-after_thrash");

  arena.close_dev();
}

// ----- F9: stall-aware adaptive beam -----
static void cmd_vmem_f9(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  const uint32_t dim = g.meta.dim;
  const int k = 10;

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 91, true);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                dim * sizeof(float));

  auto host = load_host_vectors(a.dir, g.meta.n, dim);
  std::vector<std::vector<uint32_t>> gt(qids.size());
  for (size_t i = 0; i < qids.size(); ++i)
    gt[i] = brute_topk(host.data(), g.meta.n, dim, qvecs.data() + i * dim, k);

  auto run_static = [&](const char* tag, int beam) {
    arena.cold_start();
    Stats agg;
    double rec = 0;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t i = 0; i < qids.size(); ++i) {
      auto bo = vmem_beam_search(arena, g, qvecs.data() + i * dim, beam, a.hops,
                                 k);
      rec += recall_at(bo.ids, gt[i], k);
      agg.queries++;
      agg.cxl_hits += bo.st.cxl_hits;
      agg.cxl_misses += bo.st.cxl_misses;
    }
    agg.seconds = t.elapsed();
    auto c1 = read_vmem_counters();
    double hit = (agg.cxl_hits + agg.cxl_misses)
                     ? 100.0 * agg.cxl_hits / (agg.cxl_hits + agg.cxl_misses)
                     : 0;
    double recall = rec / qids.size();
    double qps = agg.queries / std::max(agg.seconds, 1e-9);
    std::printf("[%s] beam=%d qps=%.2f hit%%=%.2f recall@10=%.4f ssd=%llu "
                "time=%.3f\n",
                tag, beam, qps, hit, recall,
                (unsigned long long)(c1.read_ios - c0.read_ios), agg.seconds);
  };

  auto run_adaptive = [&](const char* tag) {
    arena.cold_start();
    Stats agg;
    double rec = 0;
    int beam = 32;
    const double miss_hi = 0.45, miss_lo = 0.25;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t i = 0; i < qids.size(); ++i) {
      auto bo = vmem_beam_search(arena, g, qvecs.data() + i * dim, beam, a.hops,
                                 k);
      rec += recall_at(bo.ids, gt[i], k);
      agg.queries++;
      agg.cxl_hits += bo.st.cxl_hits;
      agg.cxl_misses += bo.st.cxl_misses;
      uint64_t tot = bo.st.cxl_hits + bo.st.cxl_misses;
      double miss = tot ? double(bo.st.cxl_misses) / tot : 0;
      if (miss > miss_hi) beam = std::max(4, beam / 2);
      else if (miss < miss_lo) beam = std::min(64, beam * 2);
    }
    agg.seconds = t.elapsed();
    auto c1 = read_vmem_counters();
    double hit = (agg.cxl_hits + agg.cxl_misses)
                     ? 100.0 * agg.cxl_hits / (agg.cxl_hits + agg.cxl_misses)
                     : 0;
    double recall = rec / qids.size();
    double qps = agg.queries / std::max(agg.seconds, 1e-9);
    std::printf("[%s] adaptive qps=%.2f hit%%=%.2f recall@10=%.4f ssd=%llu "
                "time=%.3f final_beam=%d\n",
                tag, qps, hit, recall,
                (unsigned long long)(c1.read_ios - c0.read_ios), agg.seconds,
                beam);
  };

  std::printf("# VMEM-F9 stall-aware beam | nq=%u hops=%d\n", a.nq, a.hops);
  run_static("VMEM-F9-static4", 4);
  run_static("VMEM-F9-static16", 16);
  run_static("VMEM-F9-static32", 32);
  run_adaptive("VMEM-F9-adaptive");
  arena.close_dev();
}

// ----- §1.2 new motivation experiments -----
static void cmd_vmem_mot12(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  if (arena.hdr.n != g.meta.n) die("meta mismatch");
  const uint32_t dim = g.meta.dim;
  const uint32_t degree = g.meta.degree;
  const int beam_default = a.beam > 0 ? a.beam : 32;

  // -------- Part1a: single-vector latency vs O_DIRECT SSD --------
  {
    VectorStoreSSD ssd;
    ssd.open_file(a.dir, true);
    Stats st_ssd;
    const int nprobe = std::min(a.iters > 0 ? a.iters : 400, 800);
    auto ids = make_queries(g.meta.n, uint32_t(nprobe), a.seed + 121, false);
    std::vector<float> tmp(dim);

    // O_DIRECT from host file (CPU read SSD path)
    {
      auto c0 = read_vmem_counters();
      Timer t;
      for (int i = 0; i < nprobe; ++i) ssd.pread_vec(ids[i], tmp.data(), st_ssd);
      double sec = t.elapsed();
      auto c1 = read_vmem_counters();
      std::printf("[MOT12-1a-odirect_ssd] n=%d ns/vec=%.1f ssd_reads_app=%llu "
                  "Δvmem_read_ios=%llu time=%.4fs\n",
                  nprobe, sec * 1e9 / nprobe,
                  (unsigned long long)st_ssd.ssd_reads,
                  (unsigned long long)(c1.read_ios - c0.read_ios), sec);
    }

    arena.cold_start();
    {
      auto c0 = read_vmem_counters();
      Timer t;
      for (int i = 0; i < nprobe; ++i) arena.touch_vec(ids[i], dim);
      double sec = t.elapsed();
      auto c1 = read_vmem_counters();
      std::printf("[MOT12-1a-vmem_cold_ssd] n=%d ns/vec=%.1f Δfaults=%llu "
                  "Δread_ios=%llu time=%.4fs\n",
                  nprobe, sec * 1e9 / nprobe,
                  (unsigned long long)(c1.faults - c0.faults),
                  (unsigned long long)(c1.read_ios - c0.read_ios), sec);
    }
    // warm PTE
    {
      auto c0 = read_vmem_counters();
      Timer t;
      for (int i = 0; i < nprobe; ++i) arena.touch_vec(ids[i], dim);
      double sec = t.elapsed();
      auto c1 = read_vmem_counters();
      std::printf("[MOT12-1a-vmem_warm_pte] n=%d ns/vec=%.1f Δfaults=%llu "
                  "Δread_ios=%llu time=%.4fs\n",
                  nprobe, sec * 1e9 / nprobe,
                  (unsigned long long)(c1.faults - c0.faults),
                  (unsigned long long)(c1.read_ios - c0.read_ios), sec);
    }
    // soft-cache hot, PTE cold
    arena.drop_host_ptes();
    {
      auto c0 = read_vmem_counters();
      Timer t;
      for (int i = 0; i < nprobe; ++i) arena.touch_vec(ids[i], dim);
      double sec = t.elapsed();
      auto c1 = read_vmem_counters();
      std::printf("[MOT12-1a-vmem_soft_pte_cold] n=%d ns/vec=%.1f Δfaults=%llu "
                  "Δread_ios=%llu time=%.4fs\n",
                  nprobe, sec * 1e9 / nprobe,
                  (unsigned long long)(c1.faults - c0.faults),
                  (unsigned long long)(c1.read_ios - c0.read_ios), sec);
    }
    ssd.close_file();
  }

  // -------- Part1b: full search under warm / soft / cold --------
  {
    auto qids = make_queries(g.meta.n, a.nq, a.seed + 122, true);
    std::vector<float> qvecs(qids.size() * dim);
    // load queries from host to avoid polluting residency state later
    auto host = load_host_vectors(a.dir, g.meta.n, dim);
    for (size_t i = 0; i < qids.size(); ++i)
      std::memcpy(qvecs.data() + i * dim, host.data() + size_t(qids[i]) * dim,
                  dim * sizeof(float));

    struct PathAcc {
      double sec_hit = 0, sec_soft = 0, sec_ssd = 0;
      uint64_t n_hit = 0, n_soft = 0, n_ssd = 0;
      uint64_t unique = 0, fetches = 0;
    };

    auto run_search = [&](const char* tag, int prep) {
      // prep: 0=cold thrash, 1=warm all touched ids from a dry host walk,
      //       2=warm then drop PTE (soft only)
      arena.cold_start();
      std::unordered_set<uint32_t> touch_set;
      // dry walk on host to know working set for warm/soft prep
      for (size_t qi = 0; qi < qids.size(); ++qi) {
        const float* query = qvecs.data() + qi * dim;
        std::priority_queue<Cand> best;
        std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
        std::unordered_set<uint32_t> visited;
        auto consider = [&](uint32_t id, float dist) {
          if (visited.count(id)) return;
          visited.insert(id);
          frontier.push({dist, id});
          best.push({dist, id});
          if ((int)best.size() > beam_default) best.pop();
        };
        auto ht = [&](uint32_t id) {
          touch_set.insert(id);
          return host.data() + size_t(id) * dim;
        };
        ht(g.meta.entry);
        consider(g.meta.entry, l2_sq(query, ht(g.meta.entry), dim));
        int hops = 0;
        while (!frontier.empty() && hops < a.hops) {
          Cand cur = frontier.top();
          frontier.pop();
          hops++;
          const uint32_t* nbrs = g.neighbors(cur.id);
          for (uint32_t k = 0; k < degree; ++k) {
            uint32_t id = nbrs[k];
            float dist = l2_sq(query, ht(id), dim);
            if ((int)best.size() < beam_default || dist < best.top().dist)
              consider(id, dist);
            else
              visited.insert(id);
          }
        }
      }
      if (prep >= 1) {
        for (uint32_t id : touch_set) arena.touch_vec(id, dim);
      }
      if (prep == 2) arena.drop_host_ptes();
      if (prep == 0) {
        // already cold_start
      }

      PathAcc acc;
      Stats st;
      auto c0 = read_vmem_counters();
      Timer t_all;
      std::unordered_set<uint32_t> seen;
      for (size_t qi = 0; qi < qids.size(); ++qi) {
        const float* query = qvecs.data() + qi * dim;
        std::priority_queue<Cand> best;
        std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
        std::unordered_set<uint32_t> visited;
        auto consider = [&](uint32_t id, float dist) {
          if (visited.count(id)) return;
          visited.insert(id);
          frontier.push({dist, id});
          best.push({dist, id});
          if ((int)best.size() > beam_default) best.pop();
        };
        auto touch = [&](uint32_t id) {
          st.vector_fetches++;
          seen.insert(id);
          bool pte = arena.page_resident(id, dim);
          auto t0 = now_sec();
          arena.touch_vec(id, dim);
          double dt = now_sec() - t0;
          // classify: if was PTE -> hit; else if read_ios later... use timing
          // heuristic + counters: after touch, if was not pte, check if soft
          // by comparing dt to thresholds is fragile; use pre-touch state:
          // not resident + we will look at global counters. Per-touch:
          // !pte: could be soft or ssd — distinguish via dt vs 20us cutoff
          // after we've measured 1a. Better: read counters delta per touch
          // is too heavy. Use: !pte && dt > 20e-6 => ssd else if !pte => soft
          if (pte) {
            acc.sec_hit += dt;
            acc.n_hit++;
            st.cxl_hits++;
          } else if (dt > 20e-6) {
            acc.sec_ssd += dt;
            acc.n_ssd++;
            st.cxl_misses++;
          } else {
            acc.sec_soft += dt;
            acc.n_soft++;
            st.cxl_misses++;  // PTE miss but soft
          }
          return arena.vec_ptr(id, dim);
        };
        touch(g.meta.entry);
        consider(g.meta.entry,
                 l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));
        int hops = 0;
        while (!frontier.empty() && hops < a.hops) {
          Cand cur = frontier.top();
          frontier.pop();
          st.hops++;
          hops++;
          const uint32_t* nbrs = g.neighbors(cur.id);
          for (uint32_t k = 0; k < degree; ++k) {
            uint32_t id = nbrs[k];
            const float* v = touch(id);
            float dist = l2_sq(query, v, dim);
            if ((int)best.size() < beam_default || dist < best.top().dist)
              consider(id, dist);
            else
              visited.insert(id);
          }
        }
        st.queries++;
      }
      st.seconds = t_all.elapsed();
      auto c1 = read_vmem_counters();
      acc.unique = seen.size();
      acc.fetches = st.vector_fetches;
      double qps = st.queries / std::max(st.seconds, 1e-9);
      std::printf(
          "[%s] q=%lu qps=%.2f wall_s=%.3f unique=%llu fetches=%llu "
          "Δread_ios=%llu Δfaults=%llu | "
          "path_hit:n=%llu,s=%.4f path_soft:n=%llu,s=%.4f "
          "path_ssd:n=%llu,s=%.4f\n",
          tag, (unsigned long)st.queries, qps, st.seconds,
          (unsigned long long)acc.unique, (unsigned long long)acc.fetches,
          (unsigned long long)(c1.read_ios - c0.read_ios),
          (unsigned long long)(c1.faults - c0.faults),
          (unsigned long long)acc.n_hit, acc.sec_hit,
          (unsigned long long)acc.n_soft, acc.sec_soft,
          (unsigned long long)acc.n_ssd, acc.sec_ssd);
    };

    std::printf("# MOT12-1b full search beam=%d hops=%d nq=%u\n", beam_default,
                a.hops, a.nq);
    run_search("MOT12-1b-search_cold_ssd", 0);
    run_search("MOT12-1b-search_warm_pte", 1);
    run_search("MOT12-1b-search_soft_pte_cold", 2);
  }

  // -------- Part3: why graph prefetch is slower --------
  {
    auto qids = make_queries(g.meta.n, a.nq, a.seed + 123, true);
    std::vector<float> qvecs(qids.size() * dim);
    for (size_t i = 0; i < qids.size(); ++i)
      std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                  dim * sizeof(float));

    auto run_pref = [&](const char* tag, int L) {
      arena.cold_start();
      double sec_pref = 0, sec_touch = 0, sec_dist = 0;
      uint64_t pref_pages = 0, pref_used = 0, pref_unused = 0;
      std::unordered_set<uint32_t> pref_ids;
      Stats st;
      auto c0 = read_vmem_counters();
      Timer t_all;
      for (size_t qi = 0; qi < qids.size(); ++qi) {
        const float* query = qvecs.data() + qi * dim;
        std::priority_queue<Cand> best;
        std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
        std::unordered_set<uint32_t> visited;
        auto consider = [&](uint32_t id, float dist) {
          if (visited.count(id)) return;
          visited.insert(id);
          frontier.push({dist, id});
          best.push({dist, id});
          if ((int)best.size() > beam_default) best.pop();
        };
        auto touch = [&](uint32_t id) {
          st.vector_fetches++;
          if (pref_ids.count(id)) {
            pref_used++;
            pref_ids.erase(id);
          }
          if (arena.page_resident(id, dim)) st.cxl_hits++;
          else st.cxl_misses++;
          auto t0 = now_sec();
          arena.touch_vec(id, dim);
          sec_touch += now_sec() - t0;
          return arena.vec_ptr(id, dim);
        };
        touch(g.meta.entry);
        consider(g.meta.entry,
                 l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));
        int hops = 0;
        while (!frontier.empty() && hops < a.hops) {
          Cand cur = frontier.top();
          frontier.pop();
          st.hops++;
          hops++;
          const uint32_t* nbrs = g.neighbors(cur.id);
          for (uint32_t k = 0; k < degree; ++k) touch(nbrs[k]);
          std::vector<Cand> ranked;
          for (uint32_t k = 0; k < degree; ++k) {
            uint32_t id = nbrs[k];
            auto t0 = now_sec();
            float dist = l2_sq(query, arena.vec_ptr(id, dim), dim);
            sec_dist += now_sec() - t0;
            ranked.push_back({dist, id});
            if ((int)best.size() < beam_default || dist < best.top().dist)
              consider(id, dist);
            else
              visited.insert(id);
          }
          if (L <= 0) continue;
          int lim = std::min(L, (int)ranked.size());
          std::partial_sort(ranked.begin(), ranked.begin() + lim, ranked.end(),
                            [](const Cand& x, const Cand& y) {
                              return x.dist < y.dist;
                            });
          std::vector<uint32_t> spec;
          for (int i = 0; i < lim; ++i) {
            const uint32_t* n2 = g.neighbors(ranked[i].id);
            for (uint32_t k = 0; k < degree; ++k) {
              if (!visited.count(n2[k])) spec.push_back(n2[k]);
            }
          }
          std::sort(spec.begin(), spec.end());
          spec.erase(std::unique(spec.begin(), spec.end()), spec.end());
          auto t0 = now_sec();
          for (uint32_t id : spec) {
            if (pref_ids.count(id)) continue;
            pref_ids.insert(id);
            pref_pages++;
            arena.prefetch_ids(&id, 1, dim);
          }
          sec_pref += now_sec() - t0;
        }
        st.queries++;
      }
      st.seconds = t_all.elapsed();
      pref_unused = pref_ids.size();  // never demanded
      auto c1 = read_vmem_counters();
      double prec =
          pref_pages ? 100.0 * pref_used / double(pref_pages) : 0;
      double qps = st.queries / std::max(st.seconds, 1e-9);
      std::printf(
          "[%s] qps=%.2f wall=%.3f Δread_ios=%llu hit%%=%.2f | "
          "sec_pref=%.3f sec_touch=%.3f sec_dist=%.3f | "
          "promote=%llu used=%llu leftover_unused=%llu precision%%=%.1f\n",
          tag, qps, st.seconds,
          (unsigned long long)(c1.read_ios - c0.read_ios),
          (st.cxl_hits + st.cxl_misses)
              ? 100.0 * st.cxl_hits / (st.cxl_hits + st.cxl_misses)
              : 0,
          sec_pref, sec_touch, sec_dist, (unsigned long long)pref_pages,
          (unsigned long long)pref_used, (unsigned long long)pref_unused, prec);
    };

    std::printf("# MOT12-3 prefetch slowdown analysis beam=%d\n", beam_default);
    run_pref("MOT12-3-demand_L0", 0);
    run_pref("MOT12-3-pref_L1", 1);
    run_pref("MOT12-3-pref_L2", 2);
    run_pref("MOT12-3-pref_L8", 8);
  }

  // -------- Part4: efSearch sweep (HNSW-style search width) --------
  // hops := efSearch so larger ef can actually explore (old hops=32 saturated).
  {
    auto qids = make_queries(g.meta.n, a.nq, a.seed + 124, true);
    std::vector<float> qvecs(qids.size() * dim);
    auto host = load_host_vectors(a.dir, g.meta.n, dim);
    for (size_t i = 0; i < qids.size(); ++i)
      std::memcpy(qvecs.data() + i * dim, host.data() + size_t(qids[i]) * dim,
                  dim * sizeof(float));
    const int k = 10;
    std::vector<std::vector<uint32_t>> gt(qids.size());
    for (size_t i = 0; i < qids.size(); ++i)
      gt[i] = brute_topk(host.data(), g.meta.n, dim, qvecs.data() + i * dim, k);

    std::printf("# MOT12-4 efSearch sweep | hops=efSearch nq=%u k=%d "
                "(CXL-SSD /dev/vmem0)\n",
                a.nq, k);
    std::printf(
        "efSearch,qps,hit_pct,recall@10,unique_ids,fetches,hops_done,"
        "ssd_reads,faults,fetches_per_q,unique_per_q,wall_s\n");
    for (int ef : {100, 200, 400, 800, 1600}) {
      const int hops_lim = ef;  // allow exploration to scale with efSearch
      arena.cold_start();
      Stats st;
      std::unordered_set<uint32_t> uniq;
      double rec_sum = 0;
      auto c0 = read_vmem_counters();
      Timer t;
      for (size_t qi = 0; qi < qids.size(); ++qi) {
        const float* query = qvecs.data() + qi * dim;
        std::priority_queue<Cand> best;
        std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
        std::unordered_set<uint32_t> visited;
        auto consider = [&](uint32_t id, float dist) {
          if (visited.count(id)) return;
          visited.insert(id);
          frontier.push({dist, id});
          best.push({dist, id});
          if ((int)best.size() > ef) best.pop();
        };
        auto touch = [&](uint32_t id) {
          st.vector_fetches++;
          uniq.insert(id);
          if (arena.page_resident(id, dim)) st.cxl_hits++;
          else st.cxl_misses++;
          return arena.touch_vec(id, dim);
        };
        touch(g.meta.entry);
        consider(g.meta.entry,
                 l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));
        int hops = 0;
        while (!frontier.empty() && hops < hops_lim) {
          Cand cur = frontier.top();
          frontier.pop();
          st.hops++;
          hops++;
          const uint32_t* nbrs = g.neighbors(cur.id);
          for (uint32_t k = 0; k < degree; ++k) {
            uint32_t id = nbrs[k];
            const float* v = touch(id);
            float dist = l2_sq(query, v, dim);
            if ((int)best.size() < ef || dist < best.top().dist)
              consider(id, dist);
            else
              visited.insert(id);
          }
        }
        // top-k from best for recall
        std::vector<Cand> sorted;
        while (!best.empty()) {
          sorted.push_back(best.top());
          best.pop();
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const Cand& x, const Cand& y) { return x.dist < y.dist; });
        std::vector<uint32_t> pred;
        for (int i = 0; i < k && i < (int)sorted.size(); ++i)
          pred.push_back(sorted[i].id);
        rec_sum += recall_at(pred, gt[qi], k);
        st.queries++;
      }
      st.seconds = t.elapsed();
      auto c1 = read_vmem_counters();
      double hit = (st.cxl_hits + st.cxl_misses)
                       ? 100.0 * st.cxl_hits / (st.cxl_hits + st.cxl_misses)
                       : 0;
      double qps = st.queries / std::max(st.seconds, 1e-9);
      double recall = rec_sum / qids.size();
      std::printf(
          "%d,%.2f,%.2f,%.4f,%zu,%llu,%llu,%llu,%llu,%.1f,%.1f,%.3f\n", ef, qps,
          hit, recall, uniq.size(), (unsigned long long)st.vector_fetches,
          (unsigned long long)st.hops,
          (unsigned long long)(c1.read_ios - c0.read_ios),
          (unsigned long long)(c1.faults - c0.faults),
          double(st.vector_fetches) / st.queries,
          double(uniq.size()) / st.queries, st.seconds);
      std::fflush(stdout);
    }
  }

  // -------- Part5: pin vs nopin, same efSearch --------
  {
    auto qids = make_queries(g.meta.n, a.nq, a.seed + 125, true);
    std::vector<float> qvecs(qids.size() * dim);
    auto host = load_host_vectors(a.dir, g.meta.n, dim);
    for (size_t i = 0; i < qids.size(); ++i)
      std::memcpy(qvecs.data() + i * dim, host.data() + size_t(qids[i]) * dim,
                  dim * sizeof(float));

    // Frequency from host walk at ef=400 (representative mid-range search)
    const int freq_ef = 400;
    std::unordered_map<uint32_t, uint32_t> freq;
    for (size_t qi = 0; qi < qids.size(); ++qi) {
      const float* query = qvecs.data() + qi * dim;
      std::priority_queue<Cand> best;
      std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
      std::unordered_set<uint32_t> visited;
      auto consider = [&](uint32_t id, float dist) {
        if (visited.count(id)) return;
        visited.insert(id);
        frontier.push({dist, id});
        best.push({dist, id});
        if ((int)best.size() > freq_ef) best.pop();
      };
      auto touch = [&](uint32_t id) {
        freq[id]++;
        return host.data() + size_t(id) * dim;
      };
      touch(g.meta.entry);
      consider(g.meta.entry, l2_sq(query, touch(g.meta.entry), dim));
      int hops = 0;
      while (!frontier.empty() && hops < freq_ef) {
        Cand cur = frontier.top();
        frontier.pop();
        hops++;
        const uint32_t* nbrs = g.neighbors(cur.id);
        for (uint32_t j = 0; j < degree; ++j) {
          uint32_t id = nbrs[j];
          float dist = l2_sq(query, touch(id), dim);
          if ((int)best.size() < freq_ef || dist < best.top().dist)
            consider(id, dist);
          else
            visited.insert(id);
        }
      }
    }
    std::vector<std::pair<uint32_t, uint32_t>> items(freq.begin(), freq.end());
    std::sort(items.begin(), items.end(),
              [](auto& x, auto& y) { return x.second > y.second; });
    size_t pin_budget =
        size_t(a.cache_mb) * 1024ull * 1024ull / (size_t(dim) * 4) / 2;
    size_t pin_n = std::min(items.size(), pin_budget);
    std::unordered_set<uint32_t> shared;
    for (size_t i = 0; i < pin_n; ++i) shared.insert(items[i].first);

    std::printf("# MOT12-5 pin vs nopin SAME efSearch | pin=%zu "
                "(~%dMB/2) freq_ef=%d\n",
                shared.size(), a.cache_mb, freq_ef);
    std::printf("efSearch,mode,qps,hit_pct,ssd_reads,fetches,wall_s\n");
    for (int ef : {200, 400, 800, 1600}) {
      for (bool do_pin : {false, true}) {
        arena.cold_start();
        const std::unordered_set<uint32_t>* pin = nullptr;
        if (do_pin) {
          for (uint32_t id : shared) arena.touch_vec(id, dim);
          pin = &shared;
        }
        Stats agg;
        auto c0 = read_vmem_counters();
        Timer t;
        for (size_t i = 0; i < qids.size(); ++i) {
          auto bo = vmem_beam_search(arena, g, qvecs.data() + i * dim, ef, ef,
                                     10, nullptr, pin);
          agg.queries++;
          agg.cxl_hits += bo.st.cxl_hits;
          agg.cxl_misses += bo.st.cxl_misses;
          agg.vector_fetches += bo.st.vector_fetches;
        }
        agg.seconds = t.elapsed();
        auto c1 = read_vmem_counters();
        uint64_t reads = c1.read_ios - c0.read_ios;
        double hit =
            (agg.cxl_hits + agg.cxl_misses)
                ? 100.0 * agg.cxl_hits / (agg.cxl_hits + agg.cxl_misses)
                : 0;
        double qps = agg.queries / std::max(agg.seconds, 1e-9);
        std::printf("%d,%s,%.2f,%.2f,%llu,%llu,%.3f\n", ef,
                    do_pin ? "pin" : "nopin", qps, hit,
                    (unsigned long long)reads,
                    (unsigned long long)agg.vector_fetches, agg.seconds);
        std::fflush(stdout);
      }
    }
  }

  arena.close_dev();
}

// Dedicated pin vs nopin at efSearch ∈ {200,400,800,1600}.
static void cmd_vmem_mot_pin(const Args& a) {
  // Reuse mot12 part5 by calling the same logic via a thin wrapper:
  // run only after opening — duplicate minimal path for a fast dedicated cmd.
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  if (arena.hdr.n != g.meta.n) die("meta mismatch");
  const uint32_t dim = g.meta.dim;
  const uint32_t degree = g.meta.degree;

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 125, true);
  auto host = load_host_vectors(a.dir, g.meta.n, dim);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, host.data() + size_t(qids[i]) * dim,
                dim * sizeof(float));

  const int freq_ef = 400;
  std::unordered_map<uint32_t, uint32_t> freq;
  for (size_t qi = 0; qi < qids.size(); ++qi) {
    const float* query = qvecs.data() + qi * dim;
    std::priority_queue<Cand> best;
    std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
    std::unordered_set<uint32_t> visited;
    auto consider = [&](uint32_t id, float dist) {
      if (visited.count(id)) return;
      visited.insert(id);
      frontier.push({dist, id});
      best.push({dist, id});
      if ((int)best.size() > freq_ef) best.pop();
    };
    auto ht = [&](uint32_t id) {
      freq[id]++;
      return host.data() + size_t(id) * dim;
    };
    ht(g.meta.entry);
    consider(g.meta.entry, l2_sq(query, ht(g.meta.entry), dim));
    int hops = 0;
    while (!frontier.empty() && hops < freq_ef) {
      Cand cur = frontier.top();
      frontier.pop();
      hops++;
      const uint32_t* nbrs = g.neighbors(cur.id);
      for (uint32_t j = 0; j < degree; ++j) {
        uint32_t id = nbrs[j];
        float dist = l2_sq(query, ht(id), dim);
        if ((int)best.size() < freq_ef || dist < best.top().dist)
          consider(id, dist);
        else
          visited.insert(id);
      }
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> items(freq.begin(), freq.end());
  std::sort(items.begin(), items.end(),
            [](auto& x, auto& y) { return x.second > y.second; });
  size_t pin_budget =
      size_t(a.cache_mb) * 1024ull * 1024ull / (size_t(dim) * 4) / 2;
  size_t pin_n = std::min(items.size(), pin_budget);
  std::unordered_set<uint32_t> shared;
  for (size_t i = 0; i < pin_n; ++i) shared.insert(items[i].first);

  std::printf("# MOT-PIN pin vs nopin SAME efSearch | pin=%zu (~%dMB/2) "
              "freq_ef=%d nq=%u (CXL-SSD)\n",
              shared.size(), a.cache_mb, freq_ef, a.nq);
  std::printf("efSearch,mode,qps,hit_pct,ssd_reads,fetches,wall_s\n");
  for (int ef : {200, 400, 800, 1600}) {
    for (bool do_pin : {false, true}) {
      arena.cold_start();
      const std::unordered_set<uint32_t>* pin = nullptr;
      if (do_pin) {
        for (uint32_t id : shared) arena.touch_vec(id, dim);
        pin = &shared;
      }
      Stats agg;
      auto c0 = read_vmem_counters();
      Timer t;
      for (size_t i = 0; i < qids.size(); ++i) {
        auto bo = vmem_beam_search(arena, g, qvecs.data() + i * dim, ef, ef, 10,
                                   nullptr, pin);
        agg.queries++;
        agg.cxl_hits += bo.st.cxl_hits;
        agg.cxl_misses += bo.st.cxl_misses;
        agg.vector_fetches += bo.st.vector_fetches;
      }
      agg.seconds = t.elapsed();
      auto c1 = read_vmem_counters();
      uint64_t reads = c1.read_ios - c0.read_ios;
      double hit = (agg.cxl_hits + agg.cxl_misses)
                       ? 100.0 * agg.cxl_hits / (agg.cxl_hits + agg.cxl_misses)
                       : 0;
      double qps = agg.queries / std::max(agg.seconds, 1e-9);
      std::printf("%d,%s,%.2f,%.2f,%llu,%llu,%.3f\n", ef,
                  do_pin ? "pin" : "nopin", qps, hit,
                  (unsigned long long)reads,
                  (unsigned long long)agg.vector_fetches, agg.seconds);
      std::fflush(stdout);
    }
  }
  arena.close_dev();
}

// ----- M-E1/3/4/5: Motivation "importance" hardening -----
static void cmd_vmem_mot_me(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  if (arena.hdr.n != g.meta.n) die("meta mismatch");
  const uint32_t dim = g.meta.dim;
  const uint32_t degree = g.meta.degree;
  const int beam = a.beam > 0 ? a.beam : 32;
  const int hops = a.hops > 0 ? a.hops : 32;

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 201, true);
  auto host = load_host_vectors(a.dir, g.meta.n, dim);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, host.data() + size_t(qids[i]) * dim,
                dim * sizeof(float));

  // Dry-walk working set (host) for ME1 pin control
  std::unordered_set<uint32_t> workset;
  for (size_t qi = 0; qi < qids.size(); ++qi) {
    const float* query = qvecs.data() + qi * dim;
    std::priority_queue<Cand> best;
    std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
    std::unordered_set<uint32_t> visited;
    auto consider = [&](uint32_t id, float dist) {
      if (visited.count(id)) return;
      visited.insert(id);
      frontier.push({dist, id});
      best.push({dist, id});
      if ((int)best.size() > beam) best.pop();
    };
    auto ht = [&](uint32_t id) {
      workset.insert(id);
      return host.data() + size_t(id) * dim;
    };
    ht(g.meta.entry);
    consider(g.meta.entry, l2_sq(query, ht(g.meta.entry), dim));
    int h = 0;
    while (!frontier.empty() && h < hops) {
      Cand cur = frontier.top();
      frontier.pop();
      h++;
      const uint32_t* nbrs = g.neighbors(cur.id);
      for (uint32_t k = 0; k < degree; ++k) {
        uint32_t id = nbrs[k];
        float dist = l2_sq(query, ht(id), dim);
        if ((int)best.size() < beam || dist < best.top().dist)
          consider(id, dist);
        else
          visited.insert(id);
      }
    }
  }
  std::vector<uint32_t> ws(workset.begin(), workset.end());
  std::sort(ws.begin(), ws.end());

  // ===== M-E1: controlled miss% → latency breakdown =====
  std::printf("# M-E1 miss%% → path breakdown | workset=%zu beam=%d hops=%d "
              "nq=%u\n",
              ws.size(), beam, hops, a.nq);
  std::printf(
      "target_miss_pct,pin_n,qps,wall,miss_obs_pct,sec_dist,sec_hit,sec_soft,"
      "sec_ssd,ssd_time_frac,read_ios\n");
  for (int tgt : {0, 10, 25, 50, 100}) {
    arena.cold_start();
    std::unordered_set<uint32_t> pin;
    size_t pin_n = size_t((100 - tgt) / 100.0 * double(ws.size()));
    for (size_t i = 0; i < pin_n; ++i) pin.insert(ws[i]);
    for (uint32_t id : pin) arena.touch_vec(id, dim);

    struct Acc {
      double sec_hit = 0, sec_soft = 0, sec_ssd = 0, sec_dist = 0;
      uint64_t n_hit = 0, n_soft = 0, n_ssd = 0;
    } acc;
    Stats st;
    auto c0 = read_vmem_counters();
    Timer t_all;
    for (size_t qi = 0; qi < qids.size(); ++qi) {
      const float* query = qvecs.data() + qi * dim;
      std::priority_queue<Cand> best;
      std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
      std::unordered_set<uint32_t> visited;
      auto consider = [&](uint32_t id, float dist) {
        if (visited.count(id)) return;
        visited.insert(id);
        frontier.push({dist, id});
        best.push({dist, id});
        if ((int)best.size() > beam) best.pop();
      };
      auto touch = [&](uint32_t id) {
        st.vector_fetches++;
        bool pte = arena.page_resident(id, dim);
        auto t0 = now_sec();
        arena.touch_vec(id, dim);
        double dt = now_sec() - t0;
        if (pte) {
          acc.sec_hit += dt;
          acc.n_hit++;
        } else if (dt > 20e-6) {
          acc.sec_ssd += dt;
          acc.n_ssd++;
        } else {
          acc.sec_soft += dt;
          acc.n_soft++;
        }
        return arena.vec_ptr(id, dim);
      };
      touch(g.meta.entry);
      {
        auto t0 = now_sec();
        float d0 = l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim);
        acc.sec_dist += now_sec() - t0;
        consider(g.meta.entry, d0);
      }
      int h = 0;
      while (!frontier.empty() && h < hops) {
        Cand cur = frontier.top();
        frontier.pop();
        h++;
        const uint32_t* nbrs = g.neighbors(cur.id);
        for (uint32_t k = 0; k < degree; ++k) {
          uint32_t id = nbrs[k];
          const float* v = touch(id);
          auto t0 = now_sec();
          float dist = l2_sq(query, v, dim);
          acc.sec_dist += now_sec() - t0;
          if ((int)best.size() < beam || dist < best.top().dist)
            consider(id, dist);
          else
            visited.insert(id);
        }
      }
      st.queries++;
    }
    double wall = t_all.elapsed();
    auto c1 = read_vmem_counters();
    double path = acc.sec_hit + acc.sec_soft + acc.sec_ssd;
    double qps = st.queries / std::max(wall, 1e-9);
    double miss_obs =
        (acc.n_hit + acc.n_soft + acc.n_ssd)
            ? 100.0 * double(acc.n_soft + acc.n_ssd) /
                  double(acc.n_hit + acc.n_soft + acc.n_ssd)
            : 0;
    std::printf("%d,%zu,%.2f,%.3f,%.1f,%.4f,%.4f,%.4f,%.4f,%.1f,%llu\n", tgt,
                pin_n, qps, wall, miss_obs, acc.sec_dist, acc.sec_hit,
                acc.sec_soft, acc.sec_ssd,
                path > 0 ? 100.0 * acc.sec_ssd / path : 0.0,
                (unsigned long long)(c1.read_ios - c0.read_ios));
    std::fflush(stdout);
  }

  // ===== M-E3: promote budget (top-L 2hop) scan =====
  std::printf("# M-E3 promote top-L 2hop budget scan | beam=%d hops=%d\n", beam,
              hops);
  std::printf(
      "L,qps,hit_pct,read_ios,promote,used,precision_pct,leftover,wall\n");
  for (int L : {0, 1, 2, 4, 8, 16}) {
    arena.cold_start();
    Stats st;
    uint64_t pref_pages = 0, pref_used = 0;
    std::unordered_set<uint32_t> pref_ids;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t qi = 0; qi < qids.size(); ++qi) {
      const float* query = qvecs.data() + qi * dim;
      std::priority_queue<Cand> best;
      std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
      std::unordered_set<uint32_t> visited;
      auto consider = [&](uint32_t id, float dist) {
        if (visited.count(id)) return;
        visited.insert(id);
        frontier.push({dist, id});
        best.push({dist, id});
        if ((int)best.size() > beam) best.pop();
      };
      auto touch = [&](uint32_t id) {
        st.vector_fetches++;
        if (pref_ids.count(id)) {
          pref_used++;
          pref_ids.erase(id);
        }
        if (arena.page_resident(id, dim)) st.cxl_hits++;
        else st.cxl_misses++;
        return arena.touch_vec(id, dim);
      };
      touch(g.meta.entry);
      consider(g.meta.entry,
               l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));
      int h = 0;
      while (!frontier.empty() && h < hops) {
        Cand cur = frontier.top();
        frontier.pop();
        st.hops++;
        h++;
        const uint32_t* nbrs = g.neighbors(cur.id);
        for (uint32_t k = 0; k < degree; ++k) touch(nbrs[k]);
        std::vector<Cand> ranked;
        ranked.reserve(degree);
        for (uint32_t k = 0; k < degree; ++k) {
          uint32_t id = nbrs[k];
          float dist = l2_sq(query, arena.vec_ptr(id, dim), dim);
          ranked.push_back({dist, id});
          if ((int)best.size() < beam || dist < best.top().dist)
            consider(id, dist);
          else
            visited.insert(id);
        }
        if (L <= 0) continue;
        int LL = std::min(L, (int)ranked.size());
        std::partial_sort(ranked.begin(), ranked.begin() + LL, ranked.end(),
                          [](const Cand& x, const Cand& y) {
                            return x.dist < y.dist;
                          });
        for (int i = 0; i < LL; ++i) {
          const uint32_t* n2 = g.neighbors(ranked[i].id);
          for (uint32_t k = 0; k < degree; ++k) {
            uint32_t id = n2[k];
            if (visited.count(id) || pref_ids.count(id)) continue;
            pref_ids.insert(id);
            pref_pages++;
            arena.prefetch_ids(&id, 1, dim);
          }
        }
      }
      st.queries++;
    }
    double wall = t.elapsed();
    auto c1 = read_vmem_counters();
    double hit = (st.cxl_hits + st.cxl_misses)
                     ? 100.0 * st.cxl_hits / (st.cxl_hits + st.cxl_misses)
                     : 0;
    double qps = st.queries / std::max(wall, 1e-9);
    double prec = pref_pages ? 100.0 * pref_used / pref_pages : 0;
    std::printf("%d,%.2f,%.2f,%llu,%llu,%llu,%.1f,%llu,%.3f\n", L, qps, hit,
                (unsigned long long)(c1.read_ios - c0.read_ios),
                (unsigned long long)pref_pages, (unsigned long long)pref_used,
                prec, (unsigned long long)pref_ids.size(), wall);
    std::fflush(stdout);
  }

  // ===== M-E4: higher hops beam×recall =====
  {
    const int hops4 = std::max(hops, 128);
    const int k = 10;
    std::printf("# M-E4 beam×recall (hops=%d) | loading GT...\n", hops4);
    std::vector<std::vector<uint32_t>> gt(qids.size());
    for (size_t i = 0; i < qids.size(); ++i)
      gt[i] = brute_topk(host.data(), g.meta.n, dim, qvecs.data() + i * dim, k);
    std::printf("beam,qps,hit_pct,recall@10,read_ios,wall,recall_per_kio\n");
    for (int b : {4, 8, 16, 32, 64}) {
      arena.cold_start();
      Stats agg;
      double rec_sum = 0;
      auto c0 = read_vmem_counters();
      Timer t;
      for (size_t i = 0; i < qids.size(); ++i) {
        auto bo = vmem_beam_search(arena, g, qvecs.data() + i * dim, b, hops4,
                                   k);
        rec_sum += recall_at(bo.ids, gt[i], k);
        agg.queries++;
        agg.vector_fetches += bo.st.vector_fetches;
        agg.cxl_hits += bo.st.cxl_hits;
        agg.cxl_misses += bo.st.cxl_misses;
      }
      agg.seconds = t.elapsed();
      auto c1 = read_vmem_counters();
      uint64_t reads = c1.read_ios - c0.read_ios;
      double hit = (agg.cxl_hits + agg.cxl_misses)
                       ? 100.0 * agg.cxl_hits / (agg.cxl_hits + agg.cxl_misses)
                       : 0;
      double qps = agg.queries / std::max(agg.seconds, 1e-9);
      double recall = rec_sum / qids.size();
      double rpk = reads ? (recall * agg.queries) / (reads / 1000.0) : 0;
      std::printf("%d,%.2f,%.2f,%.4f,%llu,%.3f,%.3f\n", b, qps, hit, recall,
                  (unsigned long long)reads, agg.seconds, rpk);
      std::fflush(stdout);
    }
  }

  // ===== M-E5: concurrent queries, pin vs nopin =====
  {
    size_t pin_budget =
        std::max<size_t>(1, (size_t(a.cache_mb) * 1024ull * 1024ull) /
                                (size_t(dim) * 4));
    std::unordered_map<uint32_t, uint32_t> freq;
    for (size_t qi = 0; qi < qids.size(); ++qi) {
      const float* query = qvecs.data() + qi * dim;
      std::priority_queue<Cand> best;
      std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
      std::unordered_set<uint32_t> visited;
      auto consider = [&](uint32_t id, float dist) {
        if (visited.count(id)) return;
        visited.insert(id);
        frontier.push({dist, id});
        best.push({dist, id});
        if ((int)best.size() > 8) best.pop();
      };
      auto ht = [&](uint32_t id) {
        freq[id]++;
        return host.data() + size_t(id) * dim;
      };
      consider(g.meta.entry, l2_sq(query, ht(g.meta.entry), dim));
      int h = 0;
      while (!frontier.empty() && h < hops) {
        Cand cur = frontier.top();
        frontier.pop();
        h++;
        const uint32_t* nbrs = g.neighbors(cur.id);
        for (uint32_t k = 0; k < degree; ++k) {
          uint32_t id = nbrs[k];
          float dist = l2_sq(query, ht(id), dim);
          if ((int)best.size() < 8 || dist < best.top().dist)
            consider(id, dist);
          else
            visited.insert(id);
        }
      }
    }
    std::vector<std::pair<uint32_t, uint32_t>> items(freq.begin(), freq.end());
    std::sort(items.begin(), items.end(),
              [](auto& x, auto& y) { return x.second > y.second; });
    std::unordered_set<uint32_t> shared;
    size_t pin_n = std::min(items.size(), pin_budget);
    for (size_t i = 0; i < pin_n; ++i) shared.insert(items[i].first);

    std::printf("# M-E5 concurrent pin vs nopin | pin=%zu threads∈{1,4,16}\n",
                shared.size());
    std::printf("threads,mode,beam,qps,hit_pct,read_ios,wall\n");

    auto run_conc = [&](int nthreads, int b, bool do_pin) {
      arena.cold_start();
      if (do_pin)
        for (uint32_t id : shared) arena.touch_vec(id, dim);
      auto c0 = read_vmem_counters();
      std::vector<Stats> local{size_t(nthreads)};
      Timer t;
      std::vector<std::thread> th;
      th.reserve(size_t(nthreads));
      auto worker = [&](int tid) {
        for (size_t i = size_t(tid); i < qids.size(); i += size_t(nthreads)) {
          auto bo =
              vmem_beam_search(arena, g, qvecs.data() + i * dim, b, hops, 10,
                               nullptr, do_pin ? &shared : nullptr);
          local[size_t(tid)].queries++;
          local[size_t(tid)].cxl_hits += bo.st.cxl_hits;
          local[size_t(tid)].cxl_misses += bo.st.cxl_misses;
          local[size_t(tid)].vector_fetches += bo.st.vector_fetches;
        }
      };
      for (int t_i = 0; t_i < nthreads; ++t_i) th.emplace_back(worker, t_i);
      for (auto& x : th) x.join();
      double wall = t.elapsed();
      auto c1 = read_vmem_counters();
      Stats agg;
      for (auto& s : local) {
        agg.queries += s.queries;
        agg.cxl_hits += s.cxl_hits;
        agg.cxl_misses += s.cxl_misses;
      }
      double hit = (agg.cxl_hits + agg.cxl_misses)
                       ? 100.0 * agg.cxl_hits / (agg.cxl_hits + agg.cxl_misses)
                       : 0;
      double qps = agg.queries / std::max(wall, 1e-9);
      std::printf("%d,%s,%d,%.2f,%.2f,%llu,%.3f\n", nthreads,
                  do_pin ? "pin" : "nopin", b, qps, hit,
                  (unsigned long long)(c1.read_ios - c0.read_ios), wall);
      std::fflush(stdout);
    };

    for (int nt : {1, 4, 16}) {
      run_conc(nt, 32, false);
      run_conc(nt, 8, true);
    }
  }

  arena.close_dev();
}

static void cmd_me2_f2(const Args& a) { cmd_f2(a); }

// Dedicated efSearch sweep (HNSW search-width knob) on CXL-SSD.
static void cmd_vmem_mot_ef(const Args& a) {
  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  if (arena.hdr.n != g.meta.n) die("meta mismatch");
  const uint32_t dim = g.meta.dim;
  const uint32_t degree = g.meta.degree;
  const int k = 10;

  auto qids = make_queries(g.meta.n, a.nq, a.seed + 124, true);
  auto host = load_host_vectors(a.dir, g.meta.n, dim);
  std::vector<float> qvecs(qids.size() * dim);
  for (size_t i = 0; i < qids.size(); ++i)
    std::memcpy(qvecs.data() + i * dim, host.data() + size_t(qids[i]) * dim,
                dim * sizeof(float));
  std::vector<std::vector<uint32_t>> gt(qids.size());
  std::printf("# loading GT brute top-%d for %u queries...\n", k, a.nq);
  for (size_t i = 0; i < qids.size(); ++i)
    gt[i] = brute_topk(host.data(), g.meta.n, dim, qvecs.data() + i * dim, k);

  std::printf("# MOT-EF efSearch sweep | hops=efSearch nq=%u "
              "(CXL-SSD /dev/vmem0)\n",
              a.nq);
  std::printf(
      "efSearch,qps,hit_pct,recall@10,unique_ids,fetches,hops_done,"
      "ssd_reads,faults,fetches_per_q,unique_per_q,wall_s\n");

  for (int ef : {100, 200, 400, 800, 1600}) {
    const int hops_lim = ef;
    arena.cold_start();
    Stats st;
    std::unordered_set<uint32_t> uniq;
    double rec_sum = 0;
    auto c0 = read_vmem_counters();
    Timer t;
    for (size_t qi = 0; qi < qids.size(); ++qi) {
      const float* query = qvecs.data() + qi * dim;
      std::priority_queue<Cand> best;
      std::priority_queue<Cand, std::vector<Cand>, ByDistGreat> frontier;
      std::unordered_set<uint32_t> visited;
      auto consider = [&](uint32_t id, float dist) {
        if (visited.count(id)) return;
        visited.insert(id);
        frontier.push({dist, id});
        best.push({dist, id});
        if ((int)best.size() > ef) best.pop();
      };
      auto touch = [&](uint32_t id) {
        st.vector_fetches++;
        uniq.insert(id);
        if (arena.page_resident(id, dim)) st.cxl_hits++;
        else st.cxl_misses++;
        return arena.touch_vec(id, dim);
      };
      touch(g.meta.entry);
      consider(g.meta.entry,
               l2_sq(query, arena.vec_ptr(g.meta.entry, dim), dim));
      int hops = 0;
      while (!frontier.empty() && hops < hops_lim) {
        Cand cur = frontier.top();
        frontier.pop();
        st.hops++;
        hops++;
        const uint32_t* nbrs = g.neighbors(cur.id);
        for (uint32_t j = 0; j < degree; ++j) {
          uint32_t id = nbrs[j];
          const float* v = touch(id);
          float dist = l2_sq(query, v, dim);
          if ((int)best.size() < ef || dist < best.top().dist)
            consider(id, dist);
          else
            visited.insert(id);
        }
      }
      std::vector<Cand> sorted;
      while (!best.empty()) {
        sorted.push_back(best.top());
        best.pop();
      }
      std::sort(sorted.begin(), sorted.end(),
                [](const Cand& x, const Cand& y) { return x.dist < y.dist; });
      std::vector<uint32_t> pred;
      for (int i = 0; i < k && i < (int)sorted.size(); ++i)
        pred.push_back(sorted[i].id);
      rec_sum += recall_at(pred, gt[qi], k);
      st.queries++;
    }
    st.seconds = t.elapsed();
    auto c1 = read_vmem_counters();
    double hit = (st.cxl_hits + st.cxl_misses)
                     ? 100.0 * st.cxl_hits / (st.cxl_hits + st.cxl_misses)
                     : 0;
    double qps = st.queries / std::max(st.seconds, 1e-9);
    double recall = rec_sum / qids.size();
    std::printf("%d,%.2f,%.2f,%.4f,%zu,%llu,%llu,%llu,%llu,%.1f,%.1f,%.3f\n", ef,
                qps, hit, recall, uniq.size(),
                (unsigned long long)st.vector_fetches,
                (unsigned long long)st.hops,
                (unsigned long long)(c1.read_ios - c0.read_ios),
                (unsigned long long)(c1.faults - c0.faults),
                double(st.vector_fetches) / st.queries,
                double(uniq.size()) / st.queries, st.seconds);
    std::fflush(stdout);
  }
  arena.close_dev();
}

// Strict proof that ANNS touches go through /dev/vmem0 → soft-cache → NVMe.
static void cmd_vmem_verify(const Args& a) {
  require_cxl_vmem_identity();
  auto nvme = read_sysfs_text("/sys/class/vmem/vmem0/nvme_dev");
  auto bdf = read_sysfs_text("/sys/class/vmem/vmem0/target_bdf");
  auto backend = read_sysfs_text("/sys/class/vmem/vmem0/backend");
  std::printf("VERIFY identity: backend=%s nvme=%s bdf=%s\n", backend.c_str(),
              nvme.c_str(), bdf.c_str());
  if (backend != "software") die("VERIFY fail: backend not software");
  if (nvme != kExpectedNvme) die("VERIFY fail: unexpected nvme backend");

  // NVMe device must exist and be the vmem backend.
  struct stat st {};
  if (stat(nvme.c_str(), &st) || !S_ISBLK(st.st_mode))
    die("VERIFY fail: nvme backend is not a block device");

  Graph g;
  g.load(a.dir);
  VmemArena arena;
  arena.open_dev();
  arena.set_prefetch_off();
  arena.load_header();
  if (arena.hdr.n != g.meta.n || arena.hdr.dim != g.meta.dim)
    die("VERIFY fail: vmem header vs graph meta mismatch — was populate run?");
  const uint32_t dim = g.meta.dim;
  std::printf("VERIFY map: fd=/dev/vmem0 logical_base=0x%llx n=%u dim=%u "
              "vec_logical(0)=0x%llx\n",
              (unsigned long long)arena.logical_base, arena.hdr.n, dim,
              (unsigned long long)arena.vec_logical(0, dim));

  // Confirm pointer is inside mmap of /dev/vmem0
  const float* p0 = arena.vec_ptr(0, dim);
  if (p0 < (float*)arena.map ||
      (char*)p0 >= (char*)arena.map + arena.map_bytes)
    die("VERIFY fail: vec_ptr not inside vmem mmap");

  auto read_nvme_ios = [&]() -> uint64_t {
    // /sys/block/nvmeXnY/stat field 0 = reads completed
    std::string path = nvme;
    // /dev/nvme3n2 -> /sys/block/nvme3n2/stat
    auto slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::ifstream in("/sys/block/" + name + "/stat");
    if (!in) return 0;
    uint64_t reads = 0;
    in >> reads;
    return reads;
  };

  auto ids = make_queries(g.meta.n, 256, a.seed + 901, false);
  int fails = 0;

  // --- Test A: cold SSD miss must bump vmem read_ios + faults + NVMe reads ---
  arena.cold_start();
  {
    auto c0 = read_vmem_counters();
    uint64_t n0 = read_nvme_ios();
    Timer t;
    for (uint32_t id : ids) arena.touch_vec(id, dim);
    double sec = t.elapsed();
    auto c1 = read_vmem_counters();
    uint64_t n1 = read_nvme_ios();
    uint64_t d_read = c1.read_ios - c0.read_ios;
    uint64_t d_fault = c1.faults - c0.faults;
    uint64_t d_nvme = n1 - n0;
    double ns = sec * 1e9 / ids.size();
    std::printf("VERIFY-A cold_touch: n=%zu ns/vec=%.1f Δvmem_read_ios=%llu "
                "Δfaults=%llu Δnvme_reads=%llu\n",
                ids.size(), ns, (unsigned long long)d_read,
                (unsigned long long)d_fault, (unsigned long long)d_nvme);
    if (d_read < ids.size() / 2) {
      std::printf("FAIL-A: expected many vmem read_ios on cold touch\n");
      fails++;
    }
    if (d_fault < ids.size() / 2) {
      std::printf("FAIL-A: expected many faults on cold touch\n");
      fails++;
    }
    if (d_nvme == 0) {
      std::printf("FAIL-A: NVMe read counter did not increase — not hitting SSD?\n");
      fails++;
    } else {
      std::printf("PASS-A: cold path hit vmem soft-miss → NVMe\n");
    }
  }

  // --- Test B: warm PTE must NOT bump read_ios ---
  {
    auto c0 = read_vmem_counters();
    uint64_t n0 = read_nvme_ios();
    Timer t;
    for (uint32_t id : ids) arena.touch_vec(id, dim);
    double sec = t.elapsed();
    auto c1 = read_vmem_counters();
    uint64_t n1 = read_nvme_ios();
    uint64_t d_read = c1.read_ios - c0.read_ios;
    uint64_t d_fault = c1.faults - c0.faults;
    double ns = sec * 1e9 / ids.size();
    std::printf("VERIFY-B warm_pte: n=%zu ns/vec=%.1f Δvmem_read_ios=%llu "
                "Δfaults=%llu Δnvme_reads=%llu\n",
                ids.size(), ns, (unsigned long long)d_read,
                (unsigned long long)d_fault, (unsigned long long)(n1 - n0));
    if (d_read != 0 || d_fault != 0) {
      std::printf("FAIL-B: warm path should have 0 read_ios/faults\n");
      fails++;
    } else if (ns > 20000) {
      std::printf("FAIL-B: warm ns/vec=%.1f looks too slow for PTE hit\n", ns);
      fails++;
    } else {
      std::printf("PASS-B: warm path is PTE hit on vmem mmap (no SSD)\n");
    }
  }

  // --- Test C: soft-cache hit + PTE cold: faults>0, read_ios≈0 ---
  arena.drop_host_ptes();
  {
    auto c0 = read_vmem_counters();
    uint64_t n0 = read_nvme_ios();
    Timer t;
    for (uint32_t id : ids) arena.touch_vec(id, dim);
    double sec = t.elapsed();
    auto c1 = read_vmem_counters();
    uint64_t n1 = read_nvme_ios();
    uint64_t d_read = c1.read_ios - c0.read_ios;
    uint64_t d_fault = c1.faults - c0.faults;
    double ns = sec * 1e9 / ids.size();
    std::printf("VERIFY-C soft_pte_cold: n=%zu ns/vec=%.1f Δvmem_read_ios=%llu "
                "Δfaults=%llu Δnvme_reads=%llu\n",
                ids.size(), ns, (unsigned long long)d_read,
                (unsigned long long)d_fault, (unsigned long long)(n1 - n0));
    if (d_fault < ids.size() / 2) {
      std::printf("FAIL-C: expected faults to reinstall PTEs\n");
      fails++;
    } else if (d_read > ids.size() / 10) {
      std::printf("FAIL-C: soft path should mostly avoid SSD reads "
                  "(Δread_ios=%llu)\n",
                  (unsigned long long)d_read);
      fails++;
    } else {
      std::printf("PASS-C: soft-cache hit without NVMe (PTE rebuild only)\n");
    }
  }

  // --- Test D: cold beam search must generate vmem SSD IOs ---
  {
    arena.cold_start();
    auto qids = make_queries(g.meta.n, std::min(a.nq, 8u), a.seed + 902, true);
    std::vector<float> qvecs(qids.size() * dim);
    for (size_t i = 0; i < qids.size(); ++i)
      std::memcpy(qvecs.data() + i * dim, arena.touch_vec(qids[i], dim),
                  dim * sizeof(float));
    // queries loaded — cold again so search misses
    arena.cold_start();
    auto c0 = read_vmem_counters();
    uint64_t n0 = read_nvme_ios();
    Timer t;
    Stats agg;
    int beam = a.beam > 0 ? a.beam : 32;
    int hops = a.hops > 0 ? a.hops : 32;
    for (size_t i = 0; i < qids.size(); ++i) {
      auto bo = vmem_beam_search(arena, g, qvecs.data() + i * dim, beam, hops,
                                 10);
      agg.queries++;
      agg.vector_fetches += bo.st.vector_fetches;
      agg.cxl_hits += bo.st.cxl_hits;
      agg.cxl_misses += bo.st.cxl_misses;
    }
    double wall = t.elapsed();
    auto c1 = read_vmem_counters();
    uint64_t n1 = read_nvme_ios();
    uint64_t d_read = c1.read_ios - c0.read_ios;
    uint64_t d_nvme = n1 - n0;
    std::printf("VERIFY-D cold_search: q=%u fetches=%llu qps=%.2f "
                "Δvmem_read_ios=%llu Δnvme_reads=%llu hits=%llu misses=%llu\n",
                a.nq < 8 ? a.nq : 8, (unsigned long long)agg.vector_fetches,
                agg.queries / std::max(wall, 1e-9),
                (unsigned long long)d_read, (unsigned long long)d_nvme,
                (unsigned long long)agg.cxl_hits,
                (unsigned long long)agg.cxl_misses);
    if (d_read < 100 || d_nvme == 0) {
      std::printf("FAIL-D: cold search did not drive vmem/NVMe IO\n");
      fails++;
    } else if (agg.cxl_misses == 0) {
      std::printf("FAIL-D: cold search reported 0 misses\n");
      fails++;
    } else {
      std::printf("PASS-D: cold ANNS search uses CXL-SSD miss path\n");
    }
  }

  // --- Test E: magic header proves payload is the ANNS region on vmem ---
  {
    if (std::memcmp(arena.hdr.magic, kAnnMagic, 8) != 0) {
      std::printf("FAIL-E: ANNS magic mismatch on vmem header\n");
      fails++;
    } else {
      std::printf("PASS-E: ANNS header magic on /dev/vmem0 region\n");
    }
  }

  arena.close_dev();
  if (fails) {
    std::printf("VERIFY RESULT: %d FAILURE(S) — CXL-SSD path NOT confirmed\n",
                fails);
    std::exit(2);
  }
  std::printf("VERIFY RESULT: ALL PASS — experiments use /dev/vmem0 → "
              "NVMe (%s) with Montage CXL present\n",
              kExpectedNvme);
}

int main(int argc, char** argv) {
  if (numa_available() < 0) die("numa not available");
  Args a = parse(argc, argv);
  if (a.cmd == "gen") cmd_gen(a);
  else if (a.cmd == "f1") cmd_f1(a);
  else if (a.cmd == "f2") cmd_f2(a);
  else if (a.cmd == "f3") cmd_f3(a);
  else if (a.cmd == "vmem-identity") cmd_vmem_identity(a);
  else if (a.cmd == "vmem-populate") cmd_vmem_populate(a);
  else if (a.cmd == "vmem-h0") cmd_vmem_h0(a);
  else if (a.cmd == "vmem-f1") cmd_vmem_f1(a);
  else if (a.cmd == "vmem-f2") cmd_vmem_f2(a);
  else if (a.cmd == "vmem-f3") cmd_vmem_f3(a);
  else if (a.cmd == "vmem-f4") cmd_vmem_f4(a);
  else if (a.cmd == "vmem-f5") cmd_vmem_f5(a);
  else if (a.cmd == "vmem-f6") cmd_vmem_f6(a);
  else if (a.cmd == "vmem-f7") cmd_vmem_f7(a);
  else if (a.cmd == "vmem-f8") cmd_vmem_f8(a);
  else if (a.cmd == "vmem-f9") cmd_vmem_f9(a);
  else if (a.cmd == "vmem-mot12") cmd_vmem_mot12(a);
  else if (a.cmd == "vmem-mot-me") cmd_vmem_mot_me(a);
  else if (a.cmd == "vmem-mot-ef") cmd_vmem_mot_ef(a);
  else if (a.cmd == "vmem-mot-pin") cmd_vmem_mot_pin(a);
  else if (a.cmd == "vmem-verify") cmd_vmem_verify(a);
  else if (a.cmd == "me2-f2") cmd_me2_f2(a);
  else {
    usage();
    return 1;
  }
  return 0;
}
