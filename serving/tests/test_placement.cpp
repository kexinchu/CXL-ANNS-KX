#include "serving/placement.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

int main() {
  CxanLayoutHeader h{};
  h.magic = kCxanMagic;
  h.n = 4;
  h.dim = 2;
  h.R = 2;
  h.vec_bytes = 4;
  h.pq_bytes = 1;
  h.off_graph = 128;
  h.len_graph = 4 * 2 * 4;
  h.off_vectors = 192;
  h.len_vectors = 4 * 8;

  std::vector<uint8_t> img(256, 0);
  std::memcpy(img.data(), &h, sizeof(h));
  uint32_t graph_ssd[8] = {1, 2, 0, 3, 0, 1, 2, 1};
  std::memcpy(img.data() + 128, graph_ssd, sizeof(graph_ssd));

  Placement p;
  p.set_header(reinterpret_cast<const CxanLayoutHeader*>(img.data()));
  p.ssd_base = img.data();
  assert(p.nbrs(1)[1] == 3);

  std::vector<uint8_t> host(h.len_graph);
  std::memcpy(host.data(), img.data() + 128, h.len_graph);
  uint32_t* gh = reinterpret_cast<uint32_t*>(host.data());
  gh[3] = 99;
  p.set_graph_host(host.data(), host.size());
  assert(p.nbrs(1)[1] == 99);
  assert(reinterpret_cast<const uint32_t*>(img.data() + 128)[3] == 3);
  std::vector<uint32_t> contained;
  uint64_t page = 192;
  p.for_ids_contained_in_page(page, 4096, [&](uint32_t id) { contained.push_back(id); });
  assert(contained.size() == 4);
  std::puts("test_placement OK");
  return 0;
}
