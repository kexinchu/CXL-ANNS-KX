#include "serving/distance_metric.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>

int main() {
  const float a[] = {1.f, 2.f, 3.f};
  const float b[] = {3.f, 2.f, 1.f};
  assert(parse_distance_metric("mips") == DistanceMetric::Mips);
  assert(parse_distance_metric("l2") == DistanceMetric::L2);
  assert(std::fabs(distance_f32(a, b, 3, DistanceMetric::Mips) + 10.f) < 1e-6f);
  assert(std::fabs(distance_f32(a, b, 3, DistanceMetric::L2) - 8.f) < 1e-6f);

  bool rejected = false;
  try {
    (void)parse_distance_metric("cosine");
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
  std::puts("test_distance_metric OK");
  return 0;
}
