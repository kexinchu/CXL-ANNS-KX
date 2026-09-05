#include "serving/nav_graph.hpp"
#include "serving/placement.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

int main() {
  const uint32_t n0 = 4, dim = 2, R = 2;
  const size_t rec = Placement::diskann_payload_bytes(dim, 4, R);
  char path[] = "/tmp/nav_test_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  FILE* f = fdopen(fd, "wb");
  assert(f);
  fwrite(&n0, 4, 1, f);
  fwrite(&dim, 4, 1, f);
  fwrite(&R, 4, 1, f);
  uint32_t ids[4] = {0, 1, 2, 3};
  fwrite(ids, 4, 4, f);
  std::vector<uint8_t> recs(n0 * rec, 0);
  // Point 0 is (1,0); others near origin. Query (1,0) must return 0.
  reinterpret_cast<float*>(recs.data())[0] = 1.f;
  reinterpret_cast<float*>(recs.data())[1] = 0.f;
  *reinterpret_cast<uint32_t*>(recs.data() + 8) = 2;
  fwrite(recs.data(), 1, recs.size(), f);
  fclose(f);

  NavGraph nav;
  assert(nav.load(path));
  assert(nav.n0 == 4);
  assert(nav.loaded());
  float q[2] = {1.f, 0.f};
  assert(nav.search_entry(q, 64) == 0);
  unlink(path);
  assert((size_t)n0 * rec < 64ull << 20);
  std::puts("test_nav_graph OK");
  return 0;
}
