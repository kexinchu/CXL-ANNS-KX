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
  pq.centroid = {0.f, 0.f};
  pq.codes = {1, 2, 0, 0};
  pq.lut.assign((size_t)pq.nchunks * PqTable::kCentroids, 0.f);
  const float q[2] = {1.f, 1.f};
  pq.begin_query(q, DistanceMetric::Mips);
  assert(pq.dist(0) == -2.f);
  assert(pq.dist(1) == 0.f);
  std::vector<float> ext((size_t)pq.nchunks * PqTable::kCentroids, 0.f);
  pq.fill_lut(q, ext.data(), DistanceMetric::Mips);
  assert(pq.dist(0, ext.data()) == -2.f);
  assert(pq.dist(1, ext.data()) == 0.f);

  PqTable one_dim;
  one_dim.n = 2;
  one_dim.dim = 1;
  one_dim.nchunks = 1;
  one_dim.chunk_off = {0, 1};
  one_dim.centroid = {10.f};
  one_dim.tables_T.assign(PqTable::kCentroids, 0.f);
  one_dim.tables_T[1] = 0.f;
  one_dim.tables_T[2] = 3.f;
  one_dim.codes = {1, 2};
  one_dim.lut.assign(PqTable::kCentroids, 0.f);
  const float q1[1] = {11.f};

  one_dim.begin_query(q1, DistanceMetric::Mips);
  assert(one_dim.dist(1) < one_dim.dist(0));
  assert(one_dim.dist(0) == 0.f);
  assert(one_dim.dist(1) == -33.f);

  one_dim.begin_query(q1, DistanceMetric::L2);
  assert(one_dim.dist(0) < one_dim.dist(1));
  assert(one_dim.dist(0) == 1.f);
  assert(one_dim.dist(1) == 4.f);
  std::puts("test_pq_table OK");
  return 0;
}
