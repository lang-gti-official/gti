#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
extern "C" {
uint64_t bench_rounds();
uint64_t bench_seed();
void bench_begin();
void bench_end(uint64_t);
}

int main() {
  const uint64_t rounds = bench_rounds(), seed = bench_seed() % 65536;
  uint64_t digest = 0;
  bench_begin();
  for (uint64_t round = 0; round < rounds; ++round) {
    std::unordered_map<int32_t, int32_t> table;
    table.reserve(16384);
    for (int32_t i = 0; i < 16384; ++i)
      table.insert({int32_t((uint64_t(i) * 7919 + seed + round) % 32771), i});
    for (int32_t key = 0; key < 32768; ++key)
      if (table.contains(key))
        digest += uint64_t(table.at(key));
  }
  bench_end(digest);
  return 0;
}
