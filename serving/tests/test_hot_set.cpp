#include "serving/hot_set.hpp"
#include <cassert>
#include <cstdio>

int main() {
  HotSet hs;
  hs.aging_period = 2;
  hs.on_touch(7);
  hs.on_touch(7);
  hs.on_touch(3);
  hs.on_query_end();
  auto a = hs.top_ids(16, 8);
  assert(a.size() == 2);
  assert(a[0] == 7);
  assert(a[1] == 3);

  HotSet empty;
  assert(empty.top_ids(1 << 20, 8).empty());

  HotSet age;
  age.aging_period = 1;
  age.on_touch(1);
  age.on_query_end();
  assert(age.top_ids(64, 8).empty());

  std::puts("test_hot_set OK");
  return 0;
}
