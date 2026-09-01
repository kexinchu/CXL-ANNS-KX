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
  m.wall_ns = 2000;
  assert(m.overlap_ratio() > 2.4 && m.overlap_ratio() < 2.6);

  Metrics a, b;
  a.note_score_from_window(1);
  b.note_score_from_window(2);
  b.note_score_from_bounce(4);
  a.add_from(b);
  assert(a.score_from_window == 3);
  assert(a.score_from_bounce == 4);

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
  std::puts("test_metrics OK");
  return 0;
}
