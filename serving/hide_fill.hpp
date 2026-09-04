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

enum class HideScore : uint8_t { Window, Bounce, Vmem };

// Single-page bounce lookup. Null if missing or the range would cross a page.
inline const uint8_t* hide_bounce_lookup(const uint64_t* pages, uint8_t* const* hosts, size_t n,
                                         uint64_t page_off, size_t in_page, size_t len,
                                         size_t pb) {
  if (!pages || !hosts || in_page + len > pb) return nullptr;
  for (size_t i = 0; i < n; ++i) {
    if (pages[i] == page_off) return hosts[i] + in_page;
  }
  return nullptr;
}

struct HideInflight {
  std::vector<uint64_t> pages;
  std::unique_ptr<uint8_t[]> host_mem;
  size_t host_n = 0;
  size_t host_pb = 0;
  std::vector<uint8_t*> dests;
  std::vector<size_t> frames;
  std::unique_ptr<std::atomic<uint8_t>[]> ready;
  std::vector<uint8_t> consumed;
  size_t ready_n = 0;
  uint16_t ttl = 32;
  bool active = false;
  bool direct = false;
  uint64_t tok = 0;

  uint8_t* host_page(size_t i) { return host_mem.get() + i * host_pb; }
  const uint8_t* host_page(size_t i) const { return host_mem.get() + i * host_pb; }

  void clear() {
    pages.clear();
    host_mem.reset();
    host_n = 0;
    host_pb = 0;
    dests.clear();
    frames.clear();
    ready.reset();
    consumed.clear();
    ready_n = 0;
    active = false;
    direct = false;
    tok = 0;
  }
};

inline void hide_pump(HideInflight& inf, DramWindow& win, HideScore score = HideScore::Window) {
  if (!inf.active) return;
  if (score != HideScore::Window) {
    for (size_t i = 0; i < inf.pages.size(); ++i) {
      if (inf.consumed[i]) continue;
      if (!inf.ready[i].load(std::memory_order_acquire)) continue;
      inf.consumed[i] = 1;
      inf.ready_n++;
    }
    return;
  }
  if (inf.direct) {
    for (size_t i = 0; i < inf.pages.size(); ++i) {
      if (inf.consumed[i]) continue;
      if (!inf.ready[i].load(std::memory_order_acquire)) continue;
      inf.consumed[i] = 1;
      win.commit_fill(inf.pages[i], inf.frames[i], inf.ttl);
      inf.ready_n++;
    }
    return;
  }
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
    hosts[n] = inf.host_page(i);
    n++;
    if (n == 256) flush();
  }
  flush();
}

inline uint64_t hide_wait(HideInflight& inf, DramWindow& win, PageCopyPool& /*pool*/,
                          const std::function<void()>* after = nullptr,
                          HideScore score = HideScore::Window) {
  if (!inf.active) return 0;
  auto t0 = std::chrono::steady_clock::now();
  while (inf.ready_n < inf.pages.size()) {
    hide_pump(inf, win, score);
    if (after && *after) (*after)();
    if (inf.ready_n < inf.pages.size()) std::this_thread::yield();
  }
  hide_pump(inf, win, score);
  if (after && *after) (*after)();
  auto t1 = std::chrono::steady_clock::now();
  if (score == HideScore::Window) inf.clear();
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

// Fill holes only between this issue's min/max page, and only if the span is
// tight (density >= 50%, span <= max_span). Not a 2 MiB stripe dump.
inline void hide_extent_run(std::vector<uint64_t>& pages, size_t pb, uint32_t max_span = 32) {
  if (pages.size() < 2 || !pb) return;
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  const uint64_t lo = pages.front(), hi = pages.back();
  const uint64_t span = (hi - lo) / pb + 1;
  if (span <= 1 || span > max_span) return;
  if (pages.size() * 2 < span) return;
  pages.clear();
  pages.reserve((size_t)span);
  for (uint64_t p = lo; p <= hi; p += pb) pages.push_back(p);
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
                       Metrics* m, bool lookahead = false, bool extent_run = false,
                       bool direct = false, bool prefetch_only = false,
                       bool use_window = true) {
  const size_t pb = win.page_bytes;
  std::vector<uint64_t> miss;
  miss.reserve(pages_in.size());
  std::unordered_set<uint64_t> uniq;
  uniq.reserve(pages_in.size() * 2);
  for (uint64_t p : pages_in) {
    if (!uniq.insert(p).second) continue;
    if (use_window && win.is_resident(pl.ssd_base, pl.ssd_base + p, 1)) continue;
    miss.push_back(p);
  }
  const uint64_t requested_pages = miss.size();
  if (extent_run) hide_extent_run(miss, pb, 32);
  if (miss.empty()) {
    if (m) m->note_pf_issue_event(requested_pages, 0);
    return;
  }
  inf.clear();
  inf.direct = direct && !prefetch_only;
  if (prefetch_only) {
    inf.pages = std::move(miss);
  } else if (direct) {
    inf.pages.reserve(miss.size());
    inf.dests.reserve(miss.size());
    inf.frames.reserve(miss.size());
    for (uint64_t p : miss) {
      uint8_t* d = nullptr;
      size_t fr = SIZE_MAX;
      if (!win.reserve_fill(p, &d, &fr) || !d) continue;
      inf.pages.push_back(p);
      inf.dests.push_back(d);
      inf.frames.push_back(fr);
    }
    if (inf.pages.empty()) {
      if (m) m->note_pf_issue_event(requested_pages, 0);
      return;
    }
  } else {
    inf.pages = std::move(miss);
    inf.host_n = inf.pages.size();
    inf.host_pb = pb;
    inf.host_mem.reset(new uint8_t[inf.host_n * pb]);
    // Fault dest pages before ioctl. copy_to_user under cache_lock deadlocks on
    // a minor fault (same as unprefaulted --direct-install).
    for (size_t i = 0; i < inf.host_n * pb; i += pb) inf.host_mem[i] = 0;
  }
  inf.ready.reset(new std::atomic<uint8_t>[inf.pages.size()]);
  inf.consumed.assign(inf.pages.size(), 0);
  inf.ttl = ttl;
  inf.active = true;
  for (size_t i = 0; i < inf.pages.size(); ++i)
    inf.ready[i].store(0, std::memory_order_relaxed);
  const uint8_t* base = pl.ssd_base;
  auto* pages = &inf.pages;
  uint8_t* slab = inf.host_mem.get();
  const size_t hpb = inf.host_pb;
  auto* dests_v = &inf.dests;
  const bool use_direct = direct && !prefetch_only;
  const bool only_pf = prefetch_only;
  std::atomic<uint8_t>* ready = inf.ready.get();
  const size_t n = inf.pages.size();
  VmemIo* vio_c = vio;
  const size_t chunk = 32;
  std::vector<std::function<void()>> jobs;
  jobs.reserve((n + chunk - 1) / chunk);
  for (size_t off = 0; off < n; off += chunk) {
    const size_t n1 = n - off < chunk ? n - off : chunk;
    jobs.emplace_back([=]() {
      if (only_pf) {
        if (vio_c && vio_c->fd >= 0)
          vmem_prefetch_pages(*vio_c, pages->data() + off, (int)n1, pb);
        for (size_t i = 0; i < n1; ++i) ready[off + i].store(1, std::memory_order_release);
        return;
      }
      uint8_t* dests[256];
      const size_t ncopy = n1 < 256 ? n1 : 256;
      for (size_t i = 0; i < ncopy; ++i)
        dests[i] = use_direct ? (*dests_v)[off + i] : (slab + (off + i) * hpb);
      int rd = -1;
      if (vio_c && vio_c->fd >= 0)
        rd = vmem_read_pages(*vio_c, pages->data() + off, dests, (int)n1, pb);
      if (rd <= 0) {
        if (vio_c && vio_c->fd >= 0)
          vmem_prefetch_pages(*vio_c, pages->data() + off, (int)n1, pb);
        for (size_t i = 0; i < n1; ++i) {
          const size_t j = off + i;
          std::memcpy(dests[i], base + (*pages)[j], pb);
        }
      }
      for (size_t i = 0; i < n1; ++i) ready[off + i].store(1, std::memory_order_release);
    });
  }
  pool.submit_fns(std::move(jobs));
  if (m) {
    m->note_pf_issue_event(requested_pages, inf.pages.size());
    m->note_fetched_pages(inf.pages.size());
    m->promote_bytes += inf.pages.size() * pb;
    m->prefetch_pages += inf.pages.size();
    m->note_pf_issue(inf.pages, lookahead);
    for (uint64_t p : inf.pages)
      pl.for_ids_contained_in_page(p, pb, [&](uint32_t id) { m->note_pf_slot_id(id); });
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
  bool extent_run = false;
  bool direct_install = false;
  HideScore score = HideScore::Window;
  uint64_t next_tok = 1;
  std::function<void()> after_pump;

  bool use_window() const { return score == HideScore::Window; }

  bool page_in_slot(uint64_t p) const {
    for (int s = 0; s < kSlots; ++s) {
      if (!slot[s].active) continue;
      for (uint64_t q : slot[s].pages) {
        if (q == p) return true;
      }
    }
    return false;
  }

  bool page_ready(uint64_t p) const {
    for (int s = 0; s < kSlots; ++s) {
      if (!slot[s].active) continue;
      for (size_t i = 0; i < slot[s].pages.size(); ++i) {
        if (slot[s].pages[i] != p) continue;
        if (slot[s].ready && slot[s].ready[i].load(std::memory_order_acquire)) return true;
      }
    }
    return false;
  }

  bool covers(uint64_t p) const {
    if (use_window()) return win->is_resident(pl->ssd_base, pl->ssd_base + p, 1);
    return page_ready(p);
  }

  bool covers(const std::vector<uint64_t>& need) const {
    for (uint64_t p : need) {
      if (!covers(p)) return false;
    }
    return true;
  }

  const uint8_t* bounce_page(uint64_t page) const {
    for (int s = 0; s < kSlots; ++s) {
      if (!slot[s].active) continue;
      for (size_t i = 0; i < slot[s].pages.size(); ++i) {
        if (slot[s].pages[i] != page) continue;
        if (slot[s].host_mem && i < slot[s].host_n) return slot[s].host_page(i);
      }
    }
    return nullptr;
  }

  // In-place pointer when the vector lives on one page. Null if spanning / missing.
  const uint8_t* vec_src(const uint8_t* ssd_vec, size_t vb) const {
    if (score == HideScore::Vmem) return ssd_vec;
    const size_t pb = win->page_bytes;
    const uint64_t off = (uint64_t)(ssd_vec - pl->ssd_base);
    const uint64_t page = off & ~(uint64_t)(pb - 1);
    const size_t in_page = (size_t)(off - page);
    if (in_page + vb > pb) return nullptr;
    if (score == HideScore::Bounce) {
      const uint8_t* base = bounce_page(page);
      return base ? base + in_page : nullptr;
    }
    return win->try_ptr_resident(pl->ssd_base, ssd_vec, vb);
  }

  bool copy_vec(const uint8_t* ssd_vec, size_t vb, void* dst) const {
    if (score == HideScore::Vmem) {
      std::memcpy(dst, ssd_vec, vb);
      return true;
    }
    if (score == HideScore::Bounce) {
      const size_t pb = win->page_bytes;
      const uint64_t off = (uint64_t)(ssd_vec - pl->ssd_base);
      auto* out = static_cast<uint8_t*>(dst);
      size_t copied = 0;
      while (copied < vb) {
        const uint64_t cur = off + copied;
        const uint64_t page = cur & ~(uint64_t)(pb - 1);
        const size_t in_page = (size_t)(cur - page);
        const size_t k = std::min(vb - copied, pb - in_page);
        const uint8_t* base = bounce_page(page);
        if (!base) return false;
        std::memcpy(out + copied, base + in_page, k);
        copied += k;
      }
      return true;
    }
    return win->try_copy_resident(pl->ssd_base, ssd_vec, vb, dst);
  }

  void release_tok(uint64_t tok) {
    if (use_window() || !tok) return;
    for (int i = 0; i < kSlots; ++i) {
      if (slot[i].active && slot[i].tok == tok) slot[i].clear();
    }
  }

  void release_pages(const std::vector<uint64_t>& need) {
    if (use_window() || need.empty()) return;
    std::unordered_set<uint64_t> nset(need.begin(), need.end());
    for (int i = 0; i < kSlots; ++i) {
      if (!slot[i].active || slot[i].pages.empty()) continue;
      // Owner slot: every C_L page is here (extent_run extras allowed).
      bool owns = true;
      for (uint64_t p : nset) {
        bool hit = false;
        for (uint64_t q : slot[i].pages) {
          if (q == p) {
            hit = true;
            break;
          }
        }
        if (!hit) {
          owns = false;
          break;
        }
      }
      if (owns) slot[i].clear();
    }
  }

  void pump() {
    for (int i = 0; i < kSlots; ++i) {
      hide_pump(slot[i], *win, score);
      if (use_window() && slot[i].active && slot[i].ready_n >= slot[i].pages.size())
        slot[i].clear();
    }
  }

  uint64_t wait_all() {
    uint64_t ns = 0;
    const std::function<void()>* ap = after_pump ? &after_pump : nullptr;
    for (int i = 0; i < kSlots; ++i) ns += hide_wait(slot[i], *win, *pool, ap, score);
    return ns;
  }

  // Wait only until `need` pages cover. Bounce/vmem keep the slot until release_pages.
  uint64_t wait_covering(const std::vector<uint64_t>& need,
                         std::vector<uint64_t>* extra_toks = nullptr) {
    if (need.empty()) return 0;
    std::unordered_set<uint64_t> nset(need.begin(), need.end());
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      pump();
      for (auto it = nset.begin(); it != nset.end();) {
        if (covers(*it))
          it = nset.erase(it);
        else
          ++it;
      }
      if (nset.empty()) break;
      if (!use_window()) {
        std::vector<uint64_t> orphans;
        orphans.reserve(nset.size());
        for (uint64_t p : nset) {
          if (!page_in_slot(p)) orphans.push_back(p);
        }
        if (!orphans.empty()) {
          uint64_t t = issue(orphans, /*ttl=*/128, /*stall_if_full=*/true);
          if (t && extra_toks) extra_toks->push_back(t);
          continue;
        }
      }
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

  uint64_t issue(const std::vector<uint64_t>& pages, uint16_t ttl, bool stall_if_full = false,
                 bool lookahead = false) {
    pump();
    std::vector<uint64_t> miss;
    miss.reserve(pages.size());
    std::unordered_set<uint64_t> uniq;
    uniq.reserve(pages.size() * 2);
    std::unordered_set<uint64_t> inflight;
    for (int s = 0; s < kSlots; ++s) {
      if (!slot[s].active) continue;
      inflight.insert(slot[s].pages.begin(), slot[s].pages.end());
    }
    for (uint64_t p : pages) {
      if (!uniq.insert(p).second) continue;
      if (use_window() && win->is_resident(pl->ssd_base, pl->ssd_base + p, 1)) continue;
      if (inflight.count(p)) continue;
      miss.push_back(p);
    }
    if (miss.empty()) return 0;
    HideInflight* dst = nullptr;
    for (int i = 0; i < kSlots; ++i) {
      if (!slot[i].active) {
        dst = &slot[i];
        break;
      }
    }
    if (!dst && stall_if_full) {
      if (!use_window()) {
        while (!dst) {
          pump();
          for (int i = 0; i < kSlots; ++i) {
            if (!slot[i].active) {
              dst = &slot[i];
              break;
            }
          }
          if (!dst) {
            if (after_pump) after_pump();
            std::this_thread::yield();
          }
        }
      } else {
        int best = 0;
        for (int i = 1; i < kSlots; ++i)
          if (slot[i].ready_n >= slot[best].ready_n) best = i;
        hide_wait(slot[best], *win, *pool, after_pump ? &after_pump : nullptr, score);
        dst = &slot[best];
      }
    }
    if (!dst) return 0;
    if (miss.size() > 1024) miss.resize(1024);
    hide_issue(*dst, *win, *pl, *pool, vio, miss, ttl, m, lookahead, extent_run,
               use_window() && direct_install, score == HideScore::Vmem, use_window());
    if (!dst->active) return 0;
    dst->tok = next_tok++;
    return dst->tok;
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
