#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

enum class DistanceMetric : uint8_t { Mips, L2 };

inline DistanceMetric parse_distance_metric(const std::string& value) {
  if (value == "mips") return DistanceMetric::Mips;
  if (value == "l2") return DistanceMetric::L2;
  throw std::invalid_argument("metric must be mips or l2");
}

inline const char* distance_metric_name(DistanceMetric value) {
  return value == DistanceMetric::Mips ? "mips" : "l2";
}

inline float distance_f32(const float* x, const float* q, uint32_t dim,
                          DistanceMetric metric) {
  float sum = 0.f;
  if (metric == DistanceMetric::Mips) {
    for (uint32_t i = 0; i < dim; ++i) sum -= x[i] * q[i];
  } else {
    for (uint32_t i = 0; i < dim; ++i) {
      const float delta = x[i] - q[i];
      sum += delta * delta;
    }
  }
  return sum;
}
