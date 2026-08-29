#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct HotSet {
  uint32_t aging_period = 32;
  uint32_t queries = 0;

  struct Ent {
    uint32_t freq = 0;
    uint32_t last_q = 0;
  };
  std::unordered_map<uint32_t, Ent> tab;

  void on_touch(uint32_t id) {
    Ent& e = tab[id];
    e.freq++;
    e.last_q = queries;
  }

  void on_query_end() {
    queries++;
    if (!aging_period || (queries % aging_period) != 0) return;
    for (auto it = tab.begin(); it != tab.end();) {
      it->second.freq >>= 1;
      if (it->second.freq == 0) it = tab.erase(it);
      else ++it;
    }
  }

  std::vector<uint32_t> top_ids(size_t nbytes, size_t vec_bytes) const {
    std::vector<uint32_t> out;
    if (nbytes == 0 || vec_bytes == 0) return out;
    std::vector<std::pair<Ent, uint32_t>> rows;
    rows.reserve(tab.size());
    for (const auto& kv : tab) rows.push_back({kv.second, kv.first});
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      if (a.first.freq != b.first.freq) return a.first.freq > b.first.freq;
      return a.first.last_q > b.first.last_q;
    });
    size_t used = 0;
    for (const auto& r : rows) {
      if (used + vec_bytes > nbytes) break;
      out.push_back(r.second);
      used += vec_bytes;
    }
    return out;
  }
};
