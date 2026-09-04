#include "serving/pq_table.hpp"
#include <cassert>
#include <cstdio>
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
  std::puts("test_pq_table OK");
  return 0;
}
