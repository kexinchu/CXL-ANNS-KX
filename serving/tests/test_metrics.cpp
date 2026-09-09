#include "serving/metrics.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
  Metrics m;
  m.note_promote_pages(10);
  m.note_promote_used(7);
  m.note_promote_pages(5);
  m.note_promote_used(1);
  assert(m.promote_pages == 15);
  assert(m.promote_used == 8);
  double p = m.precision_pct();
  assert(p > 53.3 && p < 53.4);

  m.note_score_from_window(3);
  m.note_score_from_bounce(7);
  m.note_fetched_pages(10);
  assert(m.score_from_window == 3);
  assert(m.score_from_cxl_dram == 0);  // numa / HOST path must stay 0
  assert(m.score_from_bounce == 7);
  assert(m.score_from_cxl_dram_pct() == 0.0);

  Metrics cxl;
  cxl.window_is_cxl_dram = true;
  cxl.note_score_from_window(4);
  cxl.note_score_from_bounce(1);
  assert(cxl.score_from_cxl_dram == 4);
  assert(cxl.score_from_cxl_dram_pct() > 79.9 && cxl.score_from_cxl_dram_pct() < 80.1);
  cxl.reset();
  assert(cxl.window_is_cxl_dram);
  assert(cxl.score_from_window == 0);
  assert(cxl.score_from_cxl_dram == 0);
  cxl.note_score_from_window(2);
  assert(cxl.score_from_cxl_dram == 2);
  double hide = m.score_from_window_pct();
  assert(hide > 29.9 && hide < 30.1);
  double prec = m.hide_precision_pct();
  assert(prec > 29.9 && prec < 30.1);
  m.crit_wait_ns = 1234;
  m.device_fill_ns = 5000;
  m.note_coverage_wait(700);
  m.note_slot_backpressure_wait(300);
  assert(m.coverage_wait_ns == 700);
  assert(m.slot_backpressure_wait_ns == 300);
  assert(m.host_data_stall_ns() == 1000);
  m.wall_ns = 2000;
  assert(m.overlap_ratio() > 2.4 && m.overlap_ratio() < 2.6);

  Metrics a, b;
  a.note_score_from_window(1);
  b.note_score_from_window(2);
  b.note_score_from_bounce(4);
  a.add_from(b);
  assert(a.score_from_window == 3);
  assert(a.score_from_bounce == 4);

  Metrics stall_more;
  stall_more.note_coverage_wait(11);
  stall_more.note_slot_backpressure_wait(13);
  a.add_from(stall_more);
  assert(a.coverage_wait_ns == 11);
  assert(a.slot_backpressure_wait_ns == 13);
  assert(a.host_data_stall_ns() == 24);

  Metrics score_contract;
  score_contract.note_score_from_window(8);
  score_contract.note_score_from_cache(1);
  score_contract.note_score_from_bounce(1);
  assert(score_contract.score_triggered_flash_fills == 0);
  assert(score_contract.score_prematerialized_pct() == 100.0);
  score_contract.note_score_triggered_flash_fill();
  assert(score_contract.score_triggered_flash_fills == 1);
  assert(score_contract.score_prematerialized_pct() == 0.0);

  Metrics events;
  events.note_pf_issue_event(7, 10);
  events.note_pf_issue_event(3, 3);
  assert(events.pf_requested_page_events == 10);
  assert(events.pf_issued_page_events == 13);
  assert(events.pf_extent_extra_page_events == 3);
  assert(events.issue_command_events == 2);
  events.note_inflight_depth(0);
  events.note_inflight_depth(8);
  assert(events.inflight_depth_sum == 8);
  assert(events.inflight_depth_samples == 2);
  assert(events.inflight_depth_max == 8);

  Metrics more_events;
  more_events.note_pf_issue_event(4, 6);
  more_events.note_inflight_depth(5);
  events.add_from(more_events);
  assert(events.pf_requested_page_events == 14);
  assert(events.pf_issued_page_events == 19);
  assert(events.pf_extent_extra_page_events == 5);
  assert(events.issue_command_events == 3);
  assert(events.inflight_depth_sum == 13);
  assert(events.inflight_depth_samples == 3);
  assert(events.inflight_depth_max == 8);

  Metrics u;
  u.note_pf_issue(std::vector<uint64_t>{4096, 8192, 12288}, true);
  u.note_pf_issue(std::vector<uint64_t>{16384}, false);
  u.note_pf_used(std::vector<uint64_t>{4096, 16384});
  u.note_pf_score_vec(800);
  u.note_pf_score_vec(800);
  assert(u.pf_issued.size() == 4);
  assert(u.pf_used.size() == 2);
  assert(u.prefetch_page_use_pct() > 49.9 && u.prefetch_page_use_pct() < 50.1);
  assert(u.prefetch_look_used() == 1);
  u.note_pf_slot_id(1);
  u.note_pf_slot_id(2);
  u.note_pf_slot_id(3);
  u.note_pf_slot_id(4);
  u.note_pf_slot_id(5);
  u.note_pf_scored_id(1);
  assert(u.prefetch_slot_use_pct() > 19.9 && u.prefetch_slot_use_pct() < 20.1);
  Metrics occ;
  occ.note_page_occ_slots(0, 2);
  occ.note_page_occ_slots(1, 2);
  occ.note_page_occ_slots(2, 2);
  assert(occ.page_occ_pages == 3);
  assert(occ.page_occ_n0 == 1 && occ.page_occ_n50 == 1 && occ.page_occ_n100 == 1);
  assert(occ.page_occ_pct() > 49.9 && occ.page_occ_pct() < 50.1);
  std::puts("test_metrics OK");
  return 0;
}
