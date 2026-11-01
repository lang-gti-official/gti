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
struct Result {
  bool valid;
  int32_t value;
};
Result digit(int32_t code) {
  if (code < 48 || code > 57)
    return {false, 0};
  return {true, code - 48};
}
Result two_digits(int32_t high, int32_t low) {
  auto first = digit(high);
  if (!first.valid)
    return first;
  auto second = digit(low);
  if (!second.valid)
    return second;
  return {true, first.value * 10 + second.value};
}
int main() {
  const uint64_t rounds = bench_rounds(), seed = bench_seed() % 65536;
  uint64_t digest = 0;
  bench_begin();
  for (uint64_t round = 0; round < rounds; ++round)
    for (int32_t i = 0; i < 1000000; ++i) {
      int32_t input = int32_t((uint64_t(i) + seed + round) % 1000003);
      auto value = two_digits(48 + input % 11, 48 + (input / 11) % 10);
      digest += value.valid ? uint64_t(value.value) : 1;
    }
  bench_end(digest);
  return 0;
}
