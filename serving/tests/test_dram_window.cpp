#include "serving/dram_window.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
  const size_t pb = 4096;
  std::vector<uint8_t> arena(8 * pb, 0);
  DramWindow w;
  w.init(arena.data(), arena.size(), nullptr, pb);

  uint8_t* dest = nullptr;
  size_t fr = SIZE_MAX;
  assert(w.reserve_fill(8192, &dest, &fr));
  assert(dest && fr < 8);
  dest[0] = 0xAB;
  dest[800] = 0xCD;
  w.commit_fill(8192, fr, 0);
  assert(w.is_resident(nullptr, reinterpret_cast<const uint8_t*>(8192), 1));

  const uint8_t* p = w.try_ptr_resident(nullptr, reinterpret_cast<const uint8_t*>(8192), 800);
  assert(p);
  assert(p[0] == 0xAB && p[800] == 0xCD);

  uint8_t buf[800];
  assert(w.try_copy_resident(nullptr, reinterpret_cast<const uint8_t*>(8192), 800, buf));
  assert(buf[0] == 0xAB && buf[799] == 0);

  // Cross-page vector cannot return a single pointer.
  uint8_t* d2 = nullptr;
  size_t fr2 = SIZE_MAX;
  assert(w.reserve_fill(12288, &d2, &fr2));
  w.commit_fill(12288, fr2, 0);
  const uint8_t* span =
      w.try_ptr_resident(nullptr, reinterpret_cast<const uint8_t*>(8192 + 4000), 200);
  assert(span == nullptr);

  std::puts("test_dram_window OK");
  return 0;
}
