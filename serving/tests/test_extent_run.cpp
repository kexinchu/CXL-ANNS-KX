#include "serving/hide_fill.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <memory>
#include <vector>

static void prime_slot(HideInflight& inf, uint64_t page, const uint8_t* data,
                       bool ready) {
  inf.pages = {page};
  inf.host_n = 1;
  inf.host_pb = DramWindow::kPage;
  inf.host_mem.reset(new uint8_t[DramWindow::kPage]);
  std::memcpy(inf.host_mem.get(), data, DramWindow::kPage);
  inf.ready.reset(new std::atomic<uint8_t>[1]);
  inf.ready[0].store(ready ? 1 : 0, std::memory_order_release);
  inf.consumed = {0};
  inf.ready_n = 0;
  inf.active = true;
  inf.ttl = 0;
}

int main() {
  const size_t pb = 4096;
  std::vector<uint64_t> far = {0, 64 * pb};
  hide_extent_run(far, pb, 32);
  assert(far.size() == 2);

  std::vector<uint64_t> tight = {0, 2 * pb, 3 * pb};
  hide_extent_run(tight, pb, 32);
  assert(tight.size() == 4);
  assert(tight[1] == pb);

  std::vector<uint64_t> wide = {0, 31 * pb};
  hide_extent_run(wide, pb, 32);
  assert(wide.size() == 2);

  uint64_t pages[2] = {0, 4096};
  uint8_t a[4096], b[4096];
  a[100] = 0x11;
  b[200] = 0x22;
  uint8_t* hosts[2] = {a, b};
  assert(hide_bounce_lookup(pages, hosts, 2, 0, 100, 800, 4096) == a + 100);
  assert(hide_bounce_lookup(pages, hosts, 2, 4096, 200, 800, 4096) == b + 200);
  assert(hide_bounce_lookup(pages, hosts, 2, 8192, 0, 800, 4096) == nullptr);
  assert(hide_bounce_lookup(pages, hosts, 2, 0, 4000, 200, 4096) == nullptr);

  // A page that was once covered may be evicted by a later completion before
  // the full committed set is resident. The barrier must recheck and refill it.
  std::vector<uint8_t> source(4 * pb, 0);
  source[0] = 0xa0;
  source[pb] = 0xb0;
  uint8_t arena[2 * pb] = {};
  Metrics metrics;
  DramWindow win;
  win.init(arena, sizeof(arena), &metrics, pb);
  Placement placement;
  placement.ssd_base = source.data();
  PageCopyPool pool;
  pool.start(1);
  HidePipe pipe;
  pipe.pool = &pool;
  pipe.win = &win;
  pipe.pl = &placement;
  pipe.m = &metrics;

  // Token state follows the physical copy wave.  A ready wave is complete
  // even before its owning query reaches rerank/finish.
  prime_slot(pipe.slot[2], 2 * pb, source.data() + 2 * pb, false);
  pipe.slot[2].tok = 77;
  assert(pipe.token_pending(77));
  pipe.slot[2].ready[0].store(1, std::memory_order_release);
  assert(!pipe.token_pending(77));
  pipe.slot[2].clear();
  assert(!pipe.token_pending(77));

  prime_slot(pipe.slot[0], 0, source.data(), true);
  prime_slot(pipe.slot[1], pb, source.data() + pb, false);
  bool displaced = false;
  pipe.after_pump = [&] {
    if (displaced) return;
    displaced = true;
    uint64_t extra_pages[2] = {2 * pb, 3 * pb};
    uint8_t c[pb] = {}, d[pb] = {};
    uint8_t* extra_data[2] = {c, d};
    win.install_full_pages(extra_pages, extra_data, 2, 0);
    pipe.slot[1].ready[0].store(1, std::memory_order_release);
  };
  std::vector<uint64_t> committed = {0, pb};
  pipe.wait_covering(committed);
  assert(displaced);
  assert(pipe.covers(committed));
  assert(metrics.coverage_wait_ns > 0);
  assert(metrics.host_data_stall_ns() == metrics.coverage_wait_ns +
                                             metrics.slot_backpressure_wait_ns);
  pool.stop_join();

  std::puts("test_extent_run OK");
  return 0;
}
