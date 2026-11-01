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
    std::vector<uint64_t> values(32768);
    for (size_t i = 0; i < values.size(); ++i)
      values[i] = (i + seed + round) % 65521;
    for (int pass = 0; pass < 16; ++pass)
      for (size_t i = 0; i < values.size(); ++i)
        values[i] = (values[i] * 17 + uint64_t(pass)) % 65521;
    for (auto value : values)
      digest += value;
  }
  bench_end(digest);
  return 0;
}
