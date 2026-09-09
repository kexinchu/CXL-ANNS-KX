#pragma once
#include <cstdint>
#include <cstdio>
#include <unordered_set>
#include <vector>

enum class HostStallCause : uint8_t { None, Coverage, SlotBackpressure };

struct Metrics {
  uint64_t dram_hits = 0;
  uint64_t ssd_misses = 0;
  uint64_t promote_bytes = 0;
  uint64_t promote_ns = 0;
  uint64_t prefetch_pages = 0;
  uint64_t evicts = 0;
  uint64_t queries = 0;
  uint64_t distance_comps = 0;
  uint64_t promote_pages = 0;
  uint64_t promote_used = 0;
  uint64_t hotset_pins = 0;
  uint64_t score_from_window = 0;
  uint64_t score_from_cxl_dram = 0;
  uint64_t score_from_bounce = 0;
  uint64_t score_from_cache = 0;  // vmem software cache ioctl, no window install
  // Set only when the scoring window is the vmem BAR (/dev/vmem*).
  // Never for anon+mbind HOST DRAM or /dev/dax*.
  bool window_is_cxl_dram = false;
  uint64_t fetched_pages = 0;
  uint64_t crit_wait_ns = 0;
  uint64_t device_fill_ns = 0;
  uint64_t coverage_wait_ns = 0;
  uint64_t slot_backpressure_wait_ns = 0;
  uint64_t score_triggered_flash_fills = 0;
  uint64_t wall_ns = 0;
  // Unique NAND pages issued by hide vs later covering a score (not hide_prec).
  std::unordered_set<uint64_t> pf_issued;
  std::unordered_set<uint64_t> pf_used;
  std::unordered_set<uint64_t> pf_look;
  uint64_t pf_scored_vec_bytes = 0;
  uint64_t nvme_read_bytes = 0;
  std::unordered_set<uint32_t> pf_ids_on_issued;
  std::unordered_set<uint32_t> pf_ids_scored;
  // This-issue: slots on the page vs slots in `want` (unseen of this expand).
  // Query-level slot_use can be 100% while this is 20% (4 siblings scored earlier).
  uint64_t pf_issue_slots = 0;
  uint64_t pf_issue_want_slots = 0;
  // Issued-page u=want/contained: [0,.2) [.2,.4) [.4,.6) [.6,.8) [.8,1]
  uint64_t issue_use_hist[5] = {};
  uint64_t spec_issue_pages = 0;
  uint64_t spec_uniq_ids = 0;
  // Issued 4K page occupancy this run: 2-in-4K → used 0/1/2 = 0%/50%/100%.
  uint64_t page_occ_n0 = 0, page_occ_n50 = 0, page_occ_n100 = 0;
  uint64_t page_occ_pages = 0;
  uint64_t page_occ_used_slots = 0;
  uint64_t page_occ_slots = 0;
  uint64_t pf_requested_page_events = 0;
  uint64_t pf_issued_page_events = 0;
  uint64_t pf_extent_extra_page_events = 0;
  uint64_t issue_command_events = 0;
  uint64_t inflight_depth_sum = 0;
  uint64_t inflight_depth_samples = 0;
  uint64_t inflight_depth_max = 0;

  void reset() {
    const bool keep_cxl = window_is_cxl_dram;
    *this = Metrics{};
    window_is_cxl_dram = keep_cxl;
  }

  void note_promote_pages(uint64_t n) { promote_pages += n; }
  void note_promote_used(uint64_t n) { promote_used += n; }
  void note_score_from_window(uint64_t n = 1) {
    score_from_window += n;
    if (window_is_cxl_dram) score_from_cxl_dram += n;
  }
  void note_score_from_bounce(uint64_t n = 1) { score_from_bounce += n; }
  void note_score_from_cache(uint64_t n = 1) { score_from_cache += n; }
  void note_score_triggered_flash_fill(uint64_t n = 1) {
    score_triggered_flash_fills += n;
  }
  void note_coverage_wait(uint64_t ns) { coverage_wait_ns += ns; }
  void note_slot_backpressure_wait(uint64_t ns) { slot_backpressure_wait_ns += ns; }
  void note_host_stall(HostStallCause cause, uint64_t ns) {
    if (cause == HostStallCause::Coverage) note_coverage_wait(ns);
    else if (cause == HostStallCause::SlotBackpressure) note_slot_backpressure_wait(ns);
  }
  uint64_t host_data_stall_ns() const {
    return coverage_wait_ns + slot_backpressure_wait_ns;
  }
  void note_fetched_pages(uint64_t n) { fetched_pages += n; }
  void note_pf_issue_event(uint64_t requested, uint64_t issued) {
    pf_requested_page_events += requested;
    pf_issued_page_events += issued;
    pf_extent_extra_page_events += issued > requested ? issued - requested : 0;
    if (issued) issue_command_events++;
  }
  void note_inflight_depth(uint64_t depth) {
    inflight_depth_sum += depth;
    inflight_depth_samples++;
    if (depth > inflight_depth_max) inflight_depth_max = depth;
  }

  void note_pf_issue(const std::vector<uint64_t>& pages, bool lookahead) {
    for (uint64_t p : pages) {
      if (pf_issued.insert(p).second && lookahead) pf_look.insert(p);
    }
  }
  void note_pf_used(const std::vector<uint64_t>& pages) {
    for (uint64_t p : pages) {
      if (pf_issued.count(p)) pf_used.insert(p);
    }
  }
  void note_pf_score_vec(uint64_t vec_bytes) { pf_scored_vec_bytes += vec_bytes; }
  void note_pf_slot_id(uint32_t id) { pf_ids_on_issued.insert(id); }
  void note_pf_scored_id(uint32_t id) { pf_ids_scored.insert(id); }
  void note_page_occ_slots(uint32_t used, uint32_t slots) {
    if (!slots) return;
    page_occ_pages++;
    page_occ_slots += slots;
    page_occ_used_slots += used < slots ? used : slots;
    if (used == 0) page_occ_n0++;
    else if (used >= slots) page_occ_n100++;
    else page_occ_n50++;
  }
  double page_occ_pct() const {
    return page_occ_slots ? 100.0 * (double)page_occ_used_slots / (double)page_occ_slots : 0.0;
  }

  uint64_t prefetch_slots_used() const {
    uint64_t n = 0;
    for (uint32_t id : pf_ids_scored)
      if (pf_ids_on_issued.count(id)) ++n;
    return n;
  }
  // IDs living on issued pages that we later scored (5-on-a-page → 20% if only 1).
  double prefetch_slot_use_pct() const {
    return pf_ids_on_issued.empty()
               ? 0.0
               : 100.0 * (double)prefetch_slots_used() / (double)pf_ids_on_issued.size();
  }
  double prefetch_issue_use_pct() const {
    return pf_issue_slots
               ? 100.0 * (double)pf_issue_want_slots / (double)pf_issue_slots
               : 0.0;
  }

  double prefetch_page_use_pct() const {
    return pf_issued.empty() ? 0.0
                             : 100.0 * (double)pf_used.size() / (double)pf_issued.size();
  }
  uint64_t prefetch_look_used() const {
    uint64_t n = 0;
    for (uint64_t p : pf_look)
      if (pf_used.count(p)) ++n;
    return n;
  }
  double prefetch_look_use_pct() const {
    return pf_look.empty() ? 0.0
                           : 100.0 * (double)prefetch_look_used() / (double)pf_look.size();
  }
  double prefetch_miss_use_pct() const {
    const uint64_t mi = pf_issued.size() - pf_look.size();
    uint64_t mu = 0;
    for (uint64_t p : pf_used)
      if (!pf_look.count(p)) ++mu;
    return mi ? 100.0 * (double)mu / (double)mi : 0.0;
  }
  double prefetch_byte_use_pct() const {
    const uint64_t iss = (uint64_t)pf_issued.size() * 4096ull;
    return iss ? 100.0 * (double)pf_scored_vec_bytes / (double)iss : 0.0;
  }

  double precision_pct() const {
    return promote_pages ? 100.0 * (double)promote_used / (double)promote_pages : 0.0;
  }
  double score_from_window_pct() const {
    uint64_t tot = score_from_window + score_from_bounce;
    return tot ? 100.0 * (double)score_from_window / (double)tot : 0.0;
  }
  double score_from_cxl_dram_pct() const {
    uint64_t tot = score_from_window + score_from_bounce;
    return tot ? 100.0 * (double)score_from_cxl_dram / (double)tot : 0.0;
  }
  double score_prematerialized_pct() const {
    const uint64_t scored = score_from_window + score_from_bounce + score_from_cache;
    if (!scored) return 0.0;
    return score_triggered_flash_fills == 0 ? 100.0 : 0.0;
  }
  // Hide precision: scores whose bytes were already in the window / pages fetched.
  double hide_precision_pct() const {
    return fetched_pages ? 100.0 * (double)score_from_window / (double)fetched_pages : 0.0;
  }
  double overlap_ratio() const {
    return wall_ns ? (double)device_fill_ns / (double)wall_ns : 0.0;
  }

  void add_from(const Metrics& o) {
    dram_hits += o.dram_hits;
    ssd_misses += o.ssd_misses;
    promote_bytes += o.promote_bytes;
    promote_ns += o.promote_ns;
    prefetch_pages += o.prefetch_pages;
    evicts += o.evicts;
    queries += o.queries;
    distance_comps += o.distance_comps;
    promote_pages += o.promote_pages;
    promote_used += o.promote_used;
    hotset_pins += o.hotset_pins;
    score_from_window += o.score_from_window;
    score_from_cxl_dram += o.score_from_cxl_dram;
    score_from_bounce += o.score_from_bounce;
    score_from_cache += o.score_from_cache;
    window_is_cxl_dram = window_is_cxl_dram || o.window_is_cxl_dram;
    fetched_pages += o.fetched_pages;
    crit_wait_ns += o.crit_wait_ns;
    device_fill_ns += o.device_fill_ns;
    coverage_wait_ns += o.coverage_wait_ns;
    slot_backpressure_wait_ns += o.slot_backpressure_wait_ns;
    score_triggered_flash_fills += o.score_triggered_flash_fills;
    wall_ns += o.wall_ns;
    pf_issued.insert(o.pf_issued.begin(), o.pf_issued.end());
    pf_used.insert(o.pf_used.begin(), o.pf_used.end());
    pf_look.insert(o.pf_look.begin(), o.pf_look.end());
    pf_scored_vec_bytes += o.pf_scored_vec_bytes;
    nvme_read_bytes += o.nvme_read_bytes;
    pf_ids_on_issued.insert(o.pf_ids_on_issued.begin(), o.pf_ids_on_issued.end());
    pf_ids_scored.insert(o.pf_ids_scored.begin(), o.pf_ids_scored.end());
    pf_issue_slots += o.pf_issue_slots;
    pf_issue_want_slots += o.pf_issue_want_slots;
    for (int i = 0; i < 5; ++i) issue_use_hist[i] += o.issue_use_hist[i];
    spec_issue_pages += o.spec_issue_pages;
    spec_uniq_ids += o.spec_uniq_ids;
    page_occ_n0 += o.page_occ_n0;
    page_occ_n50 += o.page_occ_n50;
    page_occ_n100 += o.page_occ_n100;
    page_occ_pages += o.page_occ_pages;
    page_occ_used_slots += o.page_occ_used_slots;
    page_occ_slots += o.page_occ_slots;
    pf_requested_page_events += o.pf_requested_page_events;
    pf_issued_page_events += o.pf_issued_page_events;
    pf_extent_extra_page_events += o.pf_extent_extra_page_events;
    issue_command_events += o.issue_command_events;
    inflight_depth_sum += o.inflight_depth_sum;
    inflight_depth_samples += o.inflight_depth_samples;
    if (o.inflight_depth_max > inflight_depth_max) inflight_depth_max = o.inflight_depth_max;
  }

  void print(FILE* f = stdout) const {
    double hit = dram_hits + ssd_misses
                     ? 100.0 * (double)dram_hits / (double)(dram_hits + ssd_misses)
                     : 0.0;
    double ns_miss =
        ssd_misses ? (double)promote_ns / (double)ssd_misses : 0.0;
    fprintf(f,
            "metrics queries=%llu dram_hits=%llu ssd_misses=%llu hit_pct=%.2f "
            "promote_bytes=%llu avg_promote_ns=%.1f prefetch_pages=%llu "
            "evicts=%llu dist=%llu\n",
            (unsigned long long)queries, (unsigned long long)dram_hits,
            (unsigned long long)ssd_misses, hit,
            (unsigned long long)promote_bytes, ns_miss,
            (unsigned long long)prefetch_pages, (unsigned long long)evicts,
            (unsigned long long)distance_comps);
    fprintf(f,
            "hide from_win=%llu from_cxl_dram=%llu from_bounce=%llu from_cache=%llu "
            "from_win_pct=%.2f from_cxl_dram_pct=%.2f hide_prec=%.2f "
            "crit_wait_ns=%llu device_fill_ns=%llu overlap=%.2f "
            "coverage_wait_ns=%llu slot_backpressure_wait_ns=%llu "
            "host_data_stall_ns=%llu score_triggered_flash_fills=%llu "
            "score_prematerialized_pct=%.2f\n",
            (unsigned long long)score_from_window, (unsigned long long)score_from_cxl_dram,
            (unsigned long long)score_from_bounce, (unsigned long long)score_from_cache,
            score_from_window_pct(), score_from_cxl_dram_pct(), hide_precision_pct(),
            (unsigned long long)crit_wait_ns, (unsigned long long)device_fill_ns,
            overlap_ratio(), (unsigned long long)coverage_wait_ns,
            (unsigned long long)slot_backpressure_wait_ns,
            (unsigned long long)host_data_stall_ns(),
            (unsigned long long)score_triggered_flash_fills,
            score_prematerialized_pct());
    const uint64_t lu = prefetch_look_used();
    fprintf(f,
            "prefetch_use issued=%zu used=%zu page_use_pct=%.2f look_issued=%zu "
            "look_used=%llu look_use_pct=%.2f miss_use_pct=%.2f "
            "byte_use_pct=%.2f slot_use_pct=%.2f issue_use_pct=%.2f "
            "slots_on_pages=%zu slots_scored=%llu issue_slots=%llu issue_want=%llu "
            "scored_vec_B=%llu nvme_read_B=%llu\n",
            pf_issued.size(), pf_used.size(), prefetch_page_use_pct(), pf_look.size(),
            (unsigned long long)lu, prefetch_look_use_pct(), prefetch_miss_use_pct(),
            prefetch_byte_use_pct(), prefetch_slot_use_pct(), prefetch_issue_use_pct(),
            pf_ids_on_issued.size(), (unsigned long long)prefetch_slots_used(),
            (unsigned long long)pf_issue_slots, (unsigned long long)pf_issue_want_slots,
            (unsigned long long)pf_scored_vec_bytes, (unsigned long long)nvme_read_bytes);
    fprintf(f,
            "issue_use_hist=[0,.2):%llu [.2,.4):%llu [.4,.6):%llu [.6,.8):%llu [.8,1]:%llu\n",
            (unsigned long long)issue_use_hist[0], (unsigned long long)issue_use_hist[1],
            (unsigned long long)issue_use_hist[2], (unsigned long long)issue_use_hist[3],
            (unsigned long long)issue_use_hist[4]);
    fprintf(f, "spec_beam pages=%llu ids=%llu\n",
            (unsigned long long)spec_issue_pages, (unsigned long long)spec_uniq_ids);
    const double p0 = page_occ_pages ? 100.0 * (double)page_occ_n0 / (double)page_occ_pages : 0;
    const double p50 = page_occ_pages ? 100.0 * (double)page_occ_n50 / (double)page_occ_pages : 0;
    const double p100 = page_occ_pages ? 100.0 * (double)page_occ_n100 / (double)page_occ_pages : 0;
    fprintf(f,
            "page_occ mean=%.2f%% pages=%llu 0%%=%.2f 50%%=%.2f 100%%=%.2f "
            "(n0=%llu n50=%llu n100=%llu slots=%llu used=%llu)\n",
            page_occ_pct(), (unsigned long long)page_occ_pages, p0, p50, p100,
            (unsigned long long)page_occ_n0, (unsigned long long)page_occ_n50,
            (unsigned long long)page_occ_n100, (unsigned long long)page_occ_slots,
            (unsigned long long)page_occ_used_slots);
    const double inflight_mean = inflight_depth_samples
                                     ? (double)inflight_depth_sum / (double)inflight_depth_samples
                                     : 0.0;
    fprintf(f,
            "eval_events requested_pages=%llu issued_pages=%llu extent_extra_pages=%llu "
            "issue_commands=%llu inflight_depth_sum=%llu inflight_depth_samples=%llu "
            "inflight_depth_max=%llu inflight_depth_mean=%.3f\n",
            (unsigned long long)pf_requested_page_events,
            (unsigned long long)pf_issued_page_events,
            (unsigned long long)pf_extent_extra_page_events,
            (unsigned long long)issue_command_events,
            (unsigned long long)inflight_depth_sum,
            (unsigned long long)inflight_depth_samples,
            (unsigned long long)inflight_depth_max, inflight_mean);
  }
};
