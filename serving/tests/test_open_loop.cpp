#include "serving/open_loop.hpp"

#include <cassert>
#include <chrono>
#include <stdexcept>

int main() {
  using Clock = OpenLoopSchedule::Clock;
  const Clock::time_point epoch{};

  OpenLoopSchedule disabled;
  assert(!disabled.enabled());
  assert(disabled.scheduled(epoch, 100) == epoch);

  OpenLoopSchedule periodic(250.0);
  assert(periodic.enabled());
  assert(periodic.rate() == 250.0);
  const auto fourth = periodic.scheduled(epoch, 4);
  assert(std::chrono::duration_cast<std::chrono::milliseconds>(fourth - epoch).count() == 16);

  const auto started = fourth + std::chrono::microseconds(1750);
  assert(OpenLoopSchedule::queue_wait_ms(fourth, started) == 1.75);
  assert(OpenLoopSchedule::queue_wait_ms(fourth, fourth - std::chrono::microseconds(1)) == 0.0);

  bool rejected = false;
  try {
    OpenLoopSchedule invalid(-1.0);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
}
