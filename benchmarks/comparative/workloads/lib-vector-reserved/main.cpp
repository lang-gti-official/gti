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
    std::vector<int32_t> values;
    values.reserve(65536);
    for (int32_t i = 0; i < 65536; ++i)
      values.push_back(int32_t((uint64_t(i) * 7919 + seed + round) % 10007));
    for (int32_t value : values)
      digest += uint64_t(value);
  }
  bench_end(digest);
  return 0;
}
