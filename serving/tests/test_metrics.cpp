#include "serving/metrics.hpp"
#include <cassert>
#include <cstdio>

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
  Metrics a, b;
  a.note_promote_pages(4);
  a.note_promote_used(1);
  b.note_promote_pages(6);
  b.note_promote_used(5);
  a.add_from(b);
  assert(a.promote_pages == 10);
  assert(a.promote_used == 6);
  std::puts("test_metrics OK");
  return 0;
}
