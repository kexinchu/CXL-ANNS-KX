#include "serving/score_cache.hpp"
#include "serving/placement.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
  // ids 10,11 share page 0 (hit); 20 on page 1 (miss); 30 on page 2 (hit).
  uint32_t ids[] = {10, 20, 11, 30};
  uint32_t page_ix[] = {0, 1, 0, 2};
  uint8_t hit[] = {1, 0, 1};
  std::vector<uint32_t> now, later;
  score_cache_split(ids, page_ix, 4, hit, 3, now, later);
  assert(now.size() == 3);
  assert(later.size() == 1);
  assert(now[0] == 10 && now[1] == 11 && now[2] == 30);
  assert(later[0] == 20);

  now.clear();
  later.clear();
  uint8_t none[] = {0, 0, 0};
  score_cache_split(ids, page_ix, 4, none, 3, now, later);
  assert(now.empty());
  assert(later.size() == 4);

  // DiskANN: host graph_host wins over mmap entry() so N(u) never faults.
  CxanLayoutHeader h{};
  h.magic = kCxanMagic;
  h.version = 2;
  h.n = 2;
  h.dim = 1;
  h.R = 2;
  h.vec_bytes = 4;
  h.off_vectors = 64;
  h.len_vectors = 2 * 2048;
  h.len_graph = 0;
  std::vector<uint8_t> img(64 + 2 * 2048, 0);
  std::memcpy(img.data(), &h, sizeof(h));
  Placement p;
  p.set_header(reinterpret_cast<const CxanLayoutHeader*>(img.data()));
  p.ssd_base = img.data();
  p.vec_stride = 2048;
  p.diskann_layout = true;
  uint32_t host_g[4] = {1, 0, 0, 1};
  p.set_graph_host(reinterpret_cast<const uint8_t*>(host_g), sizeof(host_g));
  assert(p.nbrs(0)[0] == 1);
  assert(p.nbrs(1)[1] == 1);
  assert(p.nnbrs(0) == 2);

  std::puts("test_score_cache OK");
  return 0;
}
