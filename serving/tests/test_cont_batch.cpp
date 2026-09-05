#include "serving/cont_batch.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
  std::vector<uint64_t> a{4096, 8192, 8192};
  std::vector<uint64_t> b{8192, 12288};
  const std::vector<uint64_t>* parts[2] = {&a, &b};
  auto m = merge_unique_pages(parts, 2);
  assert(m.size() == 3);
  assert(m[0] == 4096 && m[1] == 8192 && m[2] == 12288);

  CbSt st[4] = {CbSt::Wait, CbSt::Ready, CbSt::Wait, CbSt::Empty};
  assert(pick_ready(st, 4) == 1);
  st[1] = CbSt::Wait;
  assert(pick_ready(st, 4) == -1);
  st[3] = CbSt::Ready;
  assert(pick_ready(st, 4) == 3);

  CbSt st2[4] = {CbSt::Wait, CbSt::Busy, CbSt::Ready, CbSt::Wait};
  assert(pick_steal(st2, 4, 0) == 2);
  st2[2] = CbSt::Busy;
  assert(pick_steal(st2, 4, 3) == 3);
  st2[0] = CbSt::Busy;
  st2[3] = CbSt::Busy;
  assert(pick_steal(st2, 4, 0) == -1);

  assert(pipe2_stagger_us(0, 2000) == 0);
  assert(pipe2_stagger_us(3, 0) == 0);
  assert(pipe2_stagger_us(3, 2000) == 6000);
  assert(pipe2_stagger_us(7, 250) == 1750);

  // Per-thread 2-deep: fill next query before waiting, so NAND stays fed.
  {
    CbSt st[2] = {CbSt::Empty, CbSt::Empty};
    bool cov[2] = {false, false};
    auto d = pipe2_decide(st, cov, 2, true);
    assert(d.act == Pipe2Act::Fill && d.slot == 0);
  }
  {
    CbSt st[2] = {CbSt::Wait, CbSt::Empty};
    bool cov[2] = {false, false};
    auto d = pipe2_decide(st, cov, 2, true);
    assert(d.act == Pipe2Act::Fill && d.slot == 1);
  }
  {
    // Covering Wait must not starve Fill — that re-creates PQ/PQ lockstep.
    CbSt st[2] = {CbSt::Wait, CbSt::Empty};
    bool cov[2] = {true, false};
    auto d = pipe2_decide(st, cov, 2, true);
    assert(d.act == Pipe2Act::Fill && d.slot == 1);
  }
  {
    CbSt st[2] = {CbSt::Wait, CbSt::Wait};
    bool cov[2] = {false, true};
    auto d = pipe2_decide(st, cov, 2, false);
    assert(d.act == Pipe2Act::Rank && d.slot == 1);
  }
  {
    CbSt st[2] = {CbSt::Wait, CbSt::Wait};
    bool cov[2] = {false, false};
    auto d = pipe2_decide(st, cov, 2, false);
    assert(d.act == Pipe2Act::Wait && d.slot == 0);
  }
  {
    CbSt st[2] = {CbSt::Ready, CbSt::Wait};
    bool cov[2] = {true, false};
    auto d = pipe2_decide(st, cov, 2, true);
    assert(d.act == Pipe2Act::Rank && d.slot == 0);
  }
  {
    CbSt st[2] = {CbSt::Empty, CbSt::Empty};
    bool cov[2] = {false, false};
    auto d = pipe2_decide(st, cov, 2, false);
    assert(d.act == Pipe2Act::Done && d.slot < 0);
  }

  // Dual-queue steal: never block. Issue Hold if QD has room; else Fill/Rank/Pump.
  {
    CbSt st[2] = {CbSt::Hold, CbSt::Empty};
    bool cov[2] = {false, false};
    auto d = steal_decide(st, cov, 2, true, /*qd_ok=*/true);
    assert(d.act == Pipe2Act::Issue && d.slot == 0);
  }
  {
    CbSt st[2] = {CbSt::Hold, CbSt::Empty};
    bool cov[2] = {false, false};
    auto d = steal_decide(st, cov, 2, true, /*qd_ok=*/false);
    assert(d.act == Pipe2Act::Fill && d.slot == 1);
  }
  {
    CbSt st[2] = {CbSt::Wait, CbSt::Empty};
    bool cov[2] = {true, false};
    auto d = steal_decide(st, cov, 2, true, false);
    assert(d.act == Pipe2Act::Fill && d.slot == 1);
  }
  {
    CbSt st[2] = {CbSt::Wait, CbSt::Wait};
    bool cov[2] = {false, true};
    auto d = steal_decide(st, cov, 2, false, false);
    assert(d.act == Pipe2Act::Rank && d.slot == 1);
  }
  {
    CbSt st[2] = {CbSt::Wait, CbSt::Wait};
    bool cov[2] = {false, false};
    auto d = steal_decide(st, cov, 2, false, false);
    assert(d.act == Pipe2Act::Pump && d.slot == 0);
  }
  {
    CbSt st[2] = {CbSt::Hold, CbSt::Hold};
    bool cov[2] = {false, false};
    auto d = steal_decide(st, cov, 2, false, false);
    assert(d.act == Pipe2Act::Pump && d.slot == 0);
  }
  {
    CbSt st[2] = {CbSt::Empty, CbSt::Empty};
    bool cov[2] = {false, false};
    auto d = steal_decide(st, cov, 2, false, true);
    assert(d.act == Pipe2Act::Done);
  }
  assert(!issue_qd_ok(4, 4));
  assert(issue_qd_ok(3, 4));
  assert(issue_qd_ok(0, 2));
  assert(effective_issue_qd(0, 8) == 8);
  assert(effective_issue_qd(0, 16) == 16);
  assert(effective_issue_qd(8, 16) == 8);
  assert(effective_issue_qd(0, 0) == 1);

  // A per-thread hub partitions one declared global I/O-worker budget; it
  // must not replicate the complete pool for every query thread.
  assert(per_thread_pool_workers(16, 1, 0) == 16);
  assert(per_thread_pool_workers(16, 8, 0) == 2);
  assert(per_thread_pool_workers(16, 8, 7) == 2);
  assert(per_thread_pool_workers(10, 4, 0) == 3);
  assert(per_thread_pool_workers(10, 4, 1) == 3);
  assert(per_thread_pool_workers(10, 4, 2) == 2);
  assert(per_thread_pool_workers(10, 4, 3) == 2);

  // A fully resident committed set legitimately creates no new fill token.
  // It must proceed to rank without consuming QD instead of cycling in Hold.
  assert(!issue_consumes_qd(false, true, false));
  assert(!issue_consumes_qd(true, true, false));
  assert(issue_consumes_qd(false, false, true));
  assert(!issue_consumes_qd(false, false, false));

  // QD describes a physical NAND wave, not the lifetime of its query.  Once
  // the wave has completed, a query that is still waiting for full coverage
  // must release the permit so a refill can make forward progress.
  assert(should_release_qd(/*held=*/true, /*wave_pending=*/false));
  assert(!should_release_qd(/*held=*/true, /*wave_pending=*/true));
  assert(!should_release_qd(/*held=*/false, /*wave_pending=*/false));

  // A worker must keep pumping a detached physical wave before exiting;
  // otherwise its unreleased global QD permit can starve surviving workers.
  assert(should_wait_for_detached_qd(Pipe2Act::Done, 1));
  assert(!should_wait_for_detached_qd(Pipe2Act::Done, 0));
  assert(!should_wait_for_detached_qd(Pipe2Act::Pump, 1));

  // A committed query whose pages were evicted after its I/O drained must
  // refill instead of spinning forever in Wait/Pump.
  assert(should_refill_missing(CbSt::Wait, false, false));
  assert(!should_refill_missing(CbSt::Wait, false, true));
  assert(!should_refill_missing(CbSt::Wait, true, false));
  assert(!should_refill_missing(CbSt::Ready, false, false));

  Metrics scheduler_metrics;
  note_scheduler_inflight(&scheduler_metrics, 3);
  note_scheduler_inflight(&scheduler_metrics, 7);
  note_scheduler_inflight(nullptr, 99);
  assert(scheduler_metrics.inflight_depth_samples == 2);
  assert(scheduler_metrics.inflight_depth_sum == 10);
  assert(scheduler_metrics.inflight_depth_max == 7);

  std::puts("test_cont_batch OK");
  return 0;
}
