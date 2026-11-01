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
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

int main() {
  const uint64_t rounds = bench_rounds(), seed = bench_seed() % 65536;
  uint64_t digest = 0;
  bench_begin();
  for (uint64_t round = 0; round < rounds; ++round) {
    std::vector<std::unique_ptr<uint64_t>> values;
    values.reserve(8192);
    for (int i = 0; i < 8192; ++i)
      values.push_back(
          std::make_unique<uint64_t>((uint64_t(i) + seed + round) % 65521));
    for (const auto &value : values)
      digest += *value;
  }
  bench_end(digest);
  return 0;
}
