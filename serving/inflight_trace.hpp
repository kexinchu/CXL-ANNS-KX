#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

struct FillRec {
  uint32_t qi = 0;
  uint32_t cand_n = 0;
  uint32_t page_n = 0;
  uint32_t miss_n = 0;
  uint32_t issue_n = 0;
  uint32_t extent_extra_n = 0;
  uint64_t fill_wait_ns = 0;
};

struct InflightSample {
  uint64_t t_ns = 0;
  uint32_t sw_pages = 0;
  uint32_t sw_slots = 0;
  uint32_t q_wait = 0;
};

struct InflightTrace {
  uint32_t max_samples = 200000;
  std::vector<FillRec> fills;
  std::vector<InflightSample> samples;

  void note_query_fill(uint32_t qi, uint32_t cand, uint32_t pages, uint32_t miss, uint32_t issued,
                       uint32_t extra, uint64_t wait_ns) {
    fills.push_back(FillRec{qi, cand, pages, miss, issued, extra, wait_ns});
  }

  void sample(uint64_t t_ns, uint32_t sw_pages, uint32_t sw_slots, uint32_t q_wait) {
    if (samples.size() >= max_samples) return;
    samples.push_back(InflightSample{t_ns, sw_pages, sw_slots, q_wait});
  }

  uint32_t peak_sw_pages() const {
    uint32_t p = 0;
    for (const auto& s : samples) p = std::max(p, s.sw_pages);
    return p;
  }

  bool dump_csv(const char* path) const {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f,
            "kind,qi,t_ns,cand_n,page_n,miss_n,issue_n,extent_extra_n,fill_wait_ns,sw_pages,sw_"
            "slots,q_wait\n");
    for (const auto& r : fills) {
      fprintf(f, "fill,%u,0,%u,%u,%u,%u,%u,%llu,0,0,0\n", r.qi, r.cand_n, r.page_n, r.miss_n,
              r.issue_n, r.extent_extra_n, (unsigned long long)r.fill_wait_ns);
    }
    for (const auto& s : samples) {
      fprintf(f, "sample,0,%llu,0,0,0,0,0,0,%u,%u,%u\n", (unsigned long long)s.t_ns, s.sw_pages,
              s.sw_slots, s.q_wait);
    }
    fclose(f);
    return true;
  }
};
