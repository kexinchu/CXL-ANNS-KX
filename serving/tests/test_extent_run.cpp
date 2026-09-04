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

  std::puts("test_extent_run OK");
  return 0;
}
