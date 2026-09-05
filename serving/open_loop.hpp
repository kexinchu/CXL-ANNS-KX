#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <thread>

class OpenLoopSchedule {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit OpenLoopSchedule(double requests_per_second = 0.0)
      : rate_(requests_per_second) {
    if (!std::isfinite(rate_) || rate_ < 0.0)
      throw std::invalid_argument("arrival rate must be finite and non-negative");
  }

  bool enabled() const { return rate_ > 0.0; }
  double rate() const { return rate_; }

  TimePoint scheduled(TimePoint epoch, uint32_t sequence) const {
    if (!enabled()) return epoch;
    const auto offset = std::chrono::duration<double>((double)sequence / rate_);
    return epoch + std::chrono::duration_cast<Clock::duration>(offset);
  }

  TimePoint wait(TimePoint epoch, uint32_t sequence) const {
    if (!enabled()) return Clock::now();
    const TimePoint due = scheduled(epoch, sequence);
    std::this_thread::sleep_until(due);
    return due;
  }

  static double queue_wait_ms(TimePoint scheduled, TimePoint started) {
    if (started <= scheduled) return 0.0;
    return std::chrono::duration<double, std::milli>(started - scheduled).count();
  }

 private:
  double rate_;
};
