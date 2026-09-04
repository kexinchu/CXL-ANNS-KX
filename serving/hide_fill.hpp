#pragma once
// Fill SSD pages into DramWindow off the scoring path. Score only after install.

#include "dram_window.hpp"
#include "metrics.hpp"
#include "page_copy_pool.hpp"
#include "placement.hpp"
#include "vmem_prefetch.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

inline void hide_collect_vec_pages(Placement& pl, size_t vb, size_t pb, uint32_t id,
                                   std::vector<uint64_t>& pages,
                                   std::unordered_set<uint64_t>* seen) {
  const uint8_t* src = pl.diskann_layout ? pl.entry(id) : pl.vec(id);
  const size_t len = pl.diskann_layout && pl.vec_stride ? pl.vec_stride : vb;
  uint64_t off = (uint64_t)(src - pl.ssd_base);
  uint64_t end = off + len;
  uint64_t first = off & ~(uint64_t)(pb - 1);
  uint64_t last = (end - 1) & ~(uint64_t)(pb - 1);
  for (uint64_t p = first; p <= last; p += pb) {
    if (seen && !seen->insert(p).second) continue;
    pages.push_back(p);
  }
}

// Pages of N(src) that hold any id in `want`. One expand = ≤7 sequential pages.
// min_use>0: skip a bundle page whose want/contained is below the threshold and
// prefetch those want IDs via pagebin (hide_collect_vec_pages). 0 = always issue.
inline void hide_collect_bundle_pages(Placement& pl, size_t vb, size_t pb, uint32_t src,
                                      const uint32_t* nbrs, uint32_t n_nbr,
                                      const std::unordered_set<uint32_t>& want,
                                      std::vector<uint64_t>& pages,
                                      std::unordered_set<uint64_t>* seen,
                                      Metrics* m = nullptr, float min_use = 0.f) {
  if (!pl.has_bundle() || !n_nbr) return;
  const size_t packed = pl.packed_vec_bytes();
  for (uint32_t k = 0; k < n_nbr; ++k) {
    if (!want.empty() && !want.count(nbrs[k])) continue;
    uint64_t off = pl.bundle_off + (uint64_t)src * pl.bundle_stride + (uint64_t)k * packed;
    uint64_t end = off + packed;
    uint64_t first = off & ~(uint64_t)(pb - 1);
    uint64_t last = (end - 1) & ~(uint64_t)(pb - 1);
    for (uint64_t p = first; p <= last; p += pb) {
      if (seen && !seen->insert(p).second) continue;
      uint32_t on = 0, w = 0;
      if (!want.empty() && (m || min_use > 0.f)) {
        pl.for_ids_contained_in_page(p, pb, [&](uint32_t id) {
          ++on;
          if (want.count(id)) ++w;
        });
      }
      if (min_use > 0.f && on > 0 && (float)w / (float)on < min_use) {
        pl.for_ids_contained_in_page(p, pb, [&](uint32_t id) {
          if (want.count(id)) hide_collect_vec_pages(pl, vb, pb, id, pages, seen);
        });
        continue;
      }
      pages.push_back(p);
      if (m && !want.empty()) {
        m->pf_issue_slots += on;
        m->pf_issue_want_slots += w;
        if (on > 0) {
          const float u = (float)w / (float)on;
          int bin = 4;
          if (u < 0.2f) bin = 0;
          else if (u < 0.4f) bin = 1;
          else if (u < 0.6f) bin = 2;
          else if (u < 0.8f) bin = 3;
          m->issue_use_hist[bin]++;
        }
      }
    }
  }
}

inline void hide_collect_entry_pages(Placement& pl, size_t vb, size_t pb, uint32_t id,
                                     std::vector<uint64_t>& pages,
                                     std::unordered_set<uint64_t>* seen) {
  hide_collect_vec_pages(pl, vb, pb, id, pages, seen);
}

inline void hide_read_nbrs(Placement& pl, uint32_t id, uint32_t* out, uint32_t R) {
  std::memcpy(out, pl.nbrs(id), (size_t)R * sizeof(uint32_t));
}

struct HideInflight {
  std::vector<uint64_t> pages;
  std::vector<std::vector<uint8_t>> host;
  std::unique_ptr<std::atomic<uint8_t>[]> ready;
  std::vector<uint8_t> consumed;
  size_t ready_n = 0;
  uint16_t ttl = 32;
  bool active = false;

  void clear() {
    pages.clear();
    host.clear();
    ready.reset();
    consumed.clear();
    ready_n = 0;
    active = false;
  }
};

inline void hide_pump(HideInflight& inf, DramWindow& win) {
  if (!inf.active) return;
  uint64_t offs[256];
  uint8_t* hosts[256];
  size_t n = 0;
  auto flush = [&]() {
    if (!n) return;
    win.install_full_pages(offs, hosts, n, inf.ttl);
    inf.ready_n += n;
    n = 0;
  };
  for (size_t i = 0; i < inf.pages.size(); ++i) {
    if (inf.consumed[i]) continue;
    if (!inf.ready[i].load(std::memory_order_acquire)) continue;
    inf.consumed[i] = 1;
    offs[n] = inf.pages[i];
    hosts[n] = inf.host[i].data();
    n++;
    if (n == 256) flush();
  }
  flush();
}

inline uint64_t hide_wait(HideInflight& inf, DramWindow& win, PageCopyPool& /*pool*/,
                          const std::function<void()>* after = nullptr) {
  if (!inf.active) return 0;
  auto t0 = std::chrono::steady_clock::now();
  while (inf.ready_n < inf.pages.size()) {
    hide_pump(inf, win);
    if (after && *after) (*after)();
    if (inf.ready_n < inf.pages.size()) std::this_thread::yield();
  }
  hide_pump(inf, win);
  if (after && *after) (*after)();
  auto t1 = std::chrono::steady_clock::now();
  inf.clear();
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

inline void hide_stripe_fill(std::vector<uint64_t>& pages, size_t pb, uint32_t max_span = 64) {
  if (pages.size() < 2 || !pb) return;
  const uint64_t stripe = 2ull << 20;
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  std::vector<uint64_t> extra;
  size_t i = 0;
  while (i < pages.size()) {
    const uint64_t s0 = pages[i] / stripe;
    size_t j = i + 1;
    while (j < pages.size() && pages[j] / stripe == s0) j++;
    const uint64_t lo = pages[i], hi = pages[j - 1];
    const uint64_t span = (hi - lo) / pb + 1;
    if (span > 1 && span <= max_span) {
      for (uint64_t p = lo; p <= hi; p += pb) extra.push_back(p);
    }
    i = j;
  }
  if (extra.empty()) return;
  pages.insert(pages.end(), extra.begin(), extra.end());
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
}

inline void hide_issue(HideInflight& inf, DramWindow& win, Placement& pl, PageCopyPool& pool,
                       VmemIo* vio, const std::vector<uint64_t>& pages_in, uint16_t ttl,
                       Metrics* m, bool lookahead = false, bool /*direct_install*/ = false,
                       bool stripe_fill = false) {
  const size_t pb = win.page_bytes;
  std::vector<uint64_t> miss;
  miss.reserve(pages_in.size());
  std::unordered_set<uint64_t> uniq;
  uniq.reserve(pages_in.size() * 2);
  for (uint64_t p : pages_in) {
    if (!uniq.insert(p).second) continue;
    if (win.is_resident(pl.ssd_base, pl.ssd_base + p, 1)) continue;
    miss.push_back(p);
  }
  if (stripe_fill) hide_stripe_fill(miss, pb, 64);
  if (miss.empty()) return;
  inf.clear();
  inf.pages = std::move(miss);
  inf.host.resize(inf.pages.size());
  inf.ready.reset(new std::atomic<uint8_t>[inf.pages.size()]);
  inf.consumed.assign(inf.pages.size(), 0);
  inf.ttl = ttl;
  inf.active = true;
  if (m) {
    m->note_fetched_pages(inf.pages.size());
    m->promote_bytes += inf.pages.size() * pb;
    m->prefetch_pages += inf.pages.size();
    m->note_pf_issue(inf.pages, lookahead);
    for (uint64_t p : inf.pages)
      pl.for_ids_contained_in_page(p, pb, [&](uint32_t id) { m->note_pf_slot_id(id); });
  }
  for (size_t i = 0; i < inf.pages.size(); ++i) {
    inf.host[i].resize(pb);
    inf.ready[i].store(0, std::memory_order_relaxed);
  }
  const uint8_t* base = pl.ssd_base;
  auto* pages = &inf.pages;
  auto* host = &inf.host;
  std::atomic<uint8_t>* ready = inf.ready.get();
  const size_t n = inf.pages.size();
  VmemIo* vio_c = vio;
  const size_t chunk = 32;
  for (size_t off = 0; off < n; off += chunk) {
    const size_t n1 = n - off < chunk ? n - off : chunk;
    pool.submit_fn([=]() {
      uint8_t* dests[256];
      const size_t ncopy = n1 < 256 ? n1 : 256;
      for (size_t i = 0; i < ncopy; ++i) dests[i] = (*host)[off + i].data();
      int rd = -1;
      if (vio_c && vio_c->fd >= 0)
        rd = vmem_read_pages(*vio_c, pages->data() + off, dests, (int)n1, pb);
      if (rd <= 0) {
        if (vio_c && vio_c->fd >= 0)
          vmem_prefetch_pages(*vio_c, pages->data() + off, (int)n1, pb);
        for (size_t i = 0; i < n1; ++i) {
          const size_t j = off + i;
          std::memcpy((*host)[j].data(), base + (*pages)[j], pb);
        }
      }
      for (size_t i = 0; i < n1; ++i) ready[off + i].store(1, std::memory_order_release);
    });
  }
}

// In-flight fills. Miss issues must not drop when full (that collapses QD).
struct HidePipe {
  static constexpr int kSlots = 32;
  HideInflight slot[kSlots];
  PageCopyPool* pool = nullptr;
  DramWindow* win = nullptr;
  Placement* pl = nullptr;
  VmemIo* vio = nullptr;
  Metrics* m = nullptr;
  bool direct_install = false;
  bool stripe_fill = false;
  std::function<void()> after_pump;

  void pump() {
    for (int i = 0; i < kSlots; ++i) {
      hide_pump(slot[i], *win);
      if (slot[i].active && slot[i].ready_n >= slot[i].pages.size()) slot[i].clear();
    }
  }

  uint64_t wait_all() {
    uint64_t ns = 0;
    const std::function<void()>* ap = after_pump ? &after_pump : nullptr;
    for (int i = 0; i < kSlots; ++i) ns += hide_wait(slot[i], *win, *pool, ap);
    return ns;
  }

  // Wait only until `need` pages are in the window. Do not drain lookahead
  // pages that share a slot — that put NAND of hop i+1 on hop i's score path.
  uint64_t wait_covering(const std::vector<uint64_t>& need) {
    if (need.empty()) return 0;
    std::unordered_set<uint64_t> nset(need.begin(), need.end());
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      pump();
      for (auto it = nset.begin(); it != nset.end();) {
        if (win->is_resident(pl->ssd_base, pl->ssd_base + *it, 1))
          it = nset.erase(it);
        else
          ++it;
      }
      if (nset.empty()) break;
      if (after_pump) after_pump();
      bool infl = false;
      for (int s = 0; s < kSlots; ++s) {
        if (slot[s].active) {
          infl = true;
          break;
        }
      }
      if (!infl) break;
      std::this_thread::yield();
    }
    pump();
    auto t1 = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  }

  HideInflight& free_or_older() {
    if (!slot[0].active) return slot[0];
    if (!slot[1].active) return slot[1];
    return slot[0].ready_n <= slot[1].ready_n ? slot[0] : slot[1];
  }

  void issue(const std::vector<uint64_t>& pages, uint16_t ttl, bool stall_if_full = false,
             bool lookahead = false) {
    pump();
    std::vector<uint64_t> miss;
    miss.reserve(pages.size());
    std::unordered_set<uint64_t> uniq;
    uniq.reserve(pages.size() * 2);
    for (uint64_t p : pages) {
      if (!uniq.insert(p).second) continue;
      if (win->is_resident(pl->ssd_base, pl->ssd_base + p, 1)) continue;
      bool infl = false;
      for (int s = 0; s < kSlots && !infl; ++s) {
        if (!slot[s].active) continue;
        for (uint64_t q : slot[s].pages) {
          if (q == p) {
            infl = true;
            break;
          }
        }
      }
      if (!infl) miss.push_back(p);
    }
    if (miss.empty()) return;
    HideInflight* dst = nullptr;
    for (int i = 0; i < kSlots; ++i) {
      if (!slot[i].active) {
        dst = &slot[i];
        break;
      }
    }
    if (!dst && stall_if_full) {
      int best = 0;
      for (int i = 1; i < kSlots; ++i)
        if (slot[i].ready_n >= slot[best].ready_n) best = i;
      hide_wait(slot[best], *win, *pool, after_pump ? &after_pump : nullptr);
      dst = &slot[best];
    }
    if (!dst) return;
    if (miss.size() > 1024) miss.resize(1024);
    hide_issue(*dst, *win, *pl, *pool, vio, miss, ttl, m, lookahead, direct_install,
               stripe_fill);
  }
};

// Entry + 1-hop + 2-hop vectors into the window (once). Call before the timed loop.
inline size_t hide_warm_entry_ball(Placement& pl, DramWindow& win, PageCopyPool& pool,
                                   VmemIo* vio, const std::vector<uint32_t>& extra_ids,
                                   uint32_t entry_id, size_t page_byte_cap = 0) {
  const size_t vb = (size_t)pl.hdr->dim * pl.hdr->vec_bytes;
  const size_t pb = win.page_bytes;
  uint32_t R = pl.hdr->R;
  if (R > 64) R = 64;
  std::unordered_set<uint32_t> ids;
  ids.insert(entry_id);
  uint32_t nbrs[64];
  auto add_nbrs = [&](uint32_t id) {
    if (id >= pl.hdr->n) return;
    hide_read_nbrs(pl, id, nbrs, R);
    for (uint32_t j = 0; j < R; ++j) {
      if (nbrs[j] < pl.hdr->n) ids.insert(nbrs[j]);
    }
  };
  add_nbrs(entry_id);
  std::vector<uint32_t> hop1(ids.begin(), ids.end());
  for (uint32_t id : hop1) add_nbrs(id);
  for (uint32_t id : extra_ids) ids.insert(id);

  std::vector<uint64_t> pages;
  std::unordered_set<uint64_t> pseen;
  pages.reserve(ids.size() * 8);
  if (pl.has_bundle()) {
    std::unordered_set<uint32_t> seeds;
    seeds.insert(entry_id);
    for (uint32_t id : hop1) seeds.insert(id);
    uint32_t nbuf[64];
    for (uint32_t id : seeds) {
      hide_read_nbrs(pl, id, nbuf, R);
      hide_collect_bundle_pages(pl, vb, pb, id, nbuf, R, {}, pages, &pseen);
    }
  }
  for (uint32_t id : ids) hide_collect_vec_pages(pl, vb, pb, id, pages, &pseen);
  if (page_byte_cap && pb && pages.size() * pb > page_byte_cap)
    pages.resize(page_byte_cap / pb);

  HideInflight inf;
  hide_issue(inf, win, pl, pool, vio, pages, /*ttl=*/128, win.metrics);
  uint64_t ns = hide_wait(inf, win, pool);
  if (tls_metrics) tls_metrics->device_fill_ns += ns;
  else if (win.metrics) win.metrics->device_fill_ns += ns;

  for (uint32_t id : ids) {
    if (pl.diskann_layout && pl.vec_stride)
      win.pin(pl.ssd_base, pl.entry(id), pl.vec_stride);
    else
      win.pin(pl.ssd_base, pl.vec(id), vb);
  }
  return ids.size();
}
