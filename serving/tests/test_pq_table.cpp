#include "serving/pq_table.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
  PqTable pq;
  pq.n = 2;
  pq.dim = 2;
  pq.nchunks = 2;
  pq.chunk_off = {0, 1, 2};
  pq.tables_T.assign((size_t)pq.dim * PqTable::kCentroids, 0.f);
  pq.tables_T[(size_t)0 * PqTable::kCentroids + 1] = 1.f;
  pq.tables_T[(size_t)1 * PqTable::kCentroids + 2] = 1.f;
  pq.codes = {1, 2, 0, 0};
  pq.lut.assign((size_t)pq.nchunks * PqTable::kCentroids, 0.f);
  const float q[2] = {1.f, 1.f};
  pq.begin_query_ip(q);
  assert(pq.dist(0) == -2.f);
  assert(pq.dist(1) == 0.f);
  std::vector<float> ext((size_t)pq.nchunks * PqTable::kCentroids, 0.f);
  pq.fill_lut_ip(q, ext.data());
  assert(pq.dist(0, ext.data()) == -2.f);
  assert(pq.dist(1, ext.data()) == 0.f);

  // dest[i] = file[new_to_old[i]] via in-place cycle permute.
  {
    const char* path = "/tmp/test_pq_codes.bin";
    const uint32_t npts = 4, nc = 2;
    const uint8_t file_codes[] = {10, 11, 20, 21, 30, 31, 40, 41};
    FILE* f = std::fopen(path, "wb");
    assert(f);
    assert(std::fwrite(&npts, 4, 1, f) == 1);
    assert(std::fwrite(&nc, 4, 1, f) == 1);
    assert(std::fwrite(file_codes, 1, sizeof(file_codes), f) == sizeof(file_codes));
    std::fclose(f);

    PqTable p2;
    p2.nchunks = nc;
    const uint32_t n2o[] = {2, 0, 3, 1};
    assert(p2.load_codes(path, n2o, npts));
    const uint8_t want[] = {30, 31, 10, 11, 40, 41, 20, 21};
    assert(p2.n == npts);
    assert(p2.codes.size() == sizeof(want));
    assert(std::memcmp(p2.codes.data(), want, sizeof(want)) == 0);

    PqTable p3;
    p3.nchunks = nc;
    assert(p3.load_codes(path, nullptr, 0));
    assert(std::memcmp(p3.codes.data(), file_codes, sizeof(file_codes)) == 0);
    std::remove(path);
  }

  std::puts("test_pq_table OK");
  return 0;
}
