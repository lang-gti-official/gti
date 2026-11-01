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
    std::vector<uint64_t> values;
    values.reserve(32768);
    for (int32_t i = 0; i < 32768; ++i)
      values.push_back(uint64_t(i) + seed + round);
    std::sort(values.begin(), values.end());
    for (size_t i = 0; i < values.size(); ++i)
      digest += (values[i] * (i + 1)) % 1000000007;
  }
  bench_end(digest);
  return 0;
}
