#include "serving/page_copy_pool.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <vector>

int main() {
  PageCopyPool pool;
  pool.start(4);
  std::atomic<int> n{0};
  std::vector<std::function<void()>> jobs;
  jobs.reserve(8);
  for (int i = 0; i < 8; ++i) {
    jobs.emplace_back([&n]() { n.fetch_add(1, std::memory_order_relaxed); });
  }
  pool.submit_fns(std::move(jobs));
  pool.wait_idle();
  assert(n.load() == 8);
  pool.stop_join();
  std::puts("test_page_copy_pool OK");
  return 0;
}
