#include "serving/hide_fill.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

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

  std::puts("test_extent_run OK");
  return 0;
}
