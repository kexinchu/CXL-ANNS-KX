#include "serving/inflight_trace.hpp"
#include <cassert>
#include <cstdio>
#include <string>

int main() {
  InflightTrace tr;
  tr.max_samples = 8;
  tr.note_query_fill(/*qi=*/3, /*cand=*/400, /*pages=*/210, /*miss=*/180, /*issued=*/192,
                     /*extent_extra=*/12, /*wait_ns=*/12345);
  assert(tr.fills.size() == 1);
  assert(tr.fills[0].qi == 3);
  assert(tr.fills[0].miss_n == 180);
  assert(tr.fills[0].issue_n == 192);

  tr.sample(/*t_ns=*/1000, /*sw_pages=*/180, /*sw_slots=*/1, /*q_wait=*/1);
  tr.sample(/*t_ns=*/2000, /*sw_pages=*/40, /*sw_slots=*/1, /*q_wait=*/1);
  assert(tr.samples.size() == 2);
  assert(tr.peak_sw_pages() == 180);

  char path[] = "/tmp/c3_trace_test.csv";
  assert(tr.dump_csv(path));
  FILE* f = fopen(path, "r");
  assert(f);
  char buf[256];
  assert(fgets(buf, sizeof(buf), f));
  fclose(f);
  std::remove(path);
  printf("test_inflight_trace ok\n");
  return 0;
}
