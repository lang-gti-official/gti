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
std::optional<int32_t> lookup(int32_t index) {
  if (index % 4 == 0)
    return std::nullopt;
  return index;
}
int main() {
  const uint64_t rounds = bench_rounds(), seed = bench_seed() % 65536;
  uint64_t digest = 0;
  bench_begin();
  for (uint64_t round = 0; round < rounds; ++round)
    for (int32_t i = 0; i < 1000000; ++i) {
      auto value = lookup(int32_t((uint64_t(i) + seed + round) % 1000003));
      if (value)
        digest += uint64_t(*value);
    }
  bench_end(digest);
  return 0;
}
