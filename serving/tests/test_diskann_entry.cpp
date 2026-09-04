#include "serving/placement.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
  assert(Placement::diskann_payload_bytes(200, 4, 32) == 932);
  assert(Placement::diskann_stride_for(200, 4, 32, 2048) == 2048);
  constexpr uint32_t dim = 200, R = 32;
  constexpr size_t STRIDE = 2048;
  CxanLayoutHeader h{};
  h.magic = kCxanMagic;
  h.version = 2;
  h.n = 2;
  h.dim = dim;
  h.R = R;
  h.vec_bytes = 4;
  h.off_vectors = 4096;
  h.len_vectors = 2 * STRIDE;
  h.len_graph = 0;
  std::vector<uint8_t> img(4096 + 2 * STRIDE, 0);
  std::memcpy(img.data(), &h, sizeof(h));
  uint8_t* e1 = img.data() + 4096 + STRIDE;
  reinterpret_cast<float*>(e1)[0] = 1.5f;
  *reinterpret_cast<uint32_t*>(e1 + 800) = 32;
  reinterpret_cast<uint32_t*>(e1 + 804)[0] = 7;
  Placement p;
  p.set_header(reinterpret_cast<const CxanLayoutHeader*>(img.data()));
  p.ssd_base = img.data();
  assert(p.diskann_layout);
  assert(p.vec_stride == STRIDE);
  assert(p.vec(1) == e1);
  assert(p.vec(1)[0] == 0);  // float 1.5 is bytes, check via float*
  assert(reinterpret_cast<const float*>(p.vec(1))[0] == 1.5f);
  assert(p.nnbrs(1) == 32);
  assert(p.nbrs(1)[0] == 7);
  assert(reinterpret_cast<const uint8_t*>(p.nbrs(1)) == e1 + 804);
  std::vector<uint32_t> ids;
  p.for_ids_contained_in_page(4096 + STRIDE, 2048, [&](uint32_t id) { ids.push_back(id); });
  assert(ids.size() == 1 && ids[0] == 1);
  p.id_to_slot = {1, 0};
  p.slot_to_id = {1, 0};
  assert(p.slot_of(0) == 1 && p.id_of_slot(0) == 1);
  assert(p.page_sibling(0) == 1 && p.page_sibling(1) == 0);
  assert(p.vec(0) == e1);
  std::vector<uint32_t> ids2;
  p.for_ids_contained_in_page(4096 + STRIDE, 2048, [&](uint32_t id) { ids2.push_back(id); });
  assert(ids2.size() == 1 && ids2[0] == 0);
  std::puts("test_diskann_entry OK");
  return 0;
}
