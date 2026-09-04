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
  {
    // Admit budget full: do not burst-Fill; keep the thread alive.
    CbSt st[2] = {CbSt::Empty, CbSt::Empty};
    bool cov[2] = {false, false};
    auto d = steal_decide(st, cov, 2, true, true, /*admit_ok=*/false);
    assert(d.act == Pipe2Act::Pump);
  }
  {
    CbSt st[2] = {CbSt::Wait, CbSt::Empty};
    bool cov[2] = {true, false};
    auto d = steal_decide(st, cov, 2, true, false, /*admit_ok=*/false);
    assert(d.act == Pipe2Act::Rank && d.slot == 0);
  }
  assert(admit_gap_ok(1000, 0, 0));
  assert(admit_gap_ok(1500, 0, 1500));
  assert(!admit_gap_ok(1499, 0, 1500));
  assert(admit_gap_ok(3000, 1500, 1500));
  assert(!admit_gap_ok(2500, 1500, 1500));
  assert(!issue_qd_ok(4, 4));
  assert(issue_qd_ok(3, 4));
  assert(issue_qd_ok(0, 2));
  assert(effective_issue_qd(0, 8) == 8);
  assert(effective_issue_qd(0, 16) == 16);
  assert(effective_issue_qd(8, 16) == 8);
  assert(effective_issue_qd(0, 0) == 1);

  // Late-beam C_L: one shot at `at`, never when disabled or already fired.
  assert(!early_cl_due(256, 0, false));
  assert(!early_cl_due(255, 256, false));
  assert(early_cl_due(256, 256, false));
  assert(early_cl_due(320, 256, false));
  assert(!early_cl_due(256, 256, true));

  std::puts("test_cont_batch OK");
  return 0;
}
