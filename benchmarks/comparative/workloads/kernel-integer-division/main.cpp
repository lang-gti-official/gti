#include <cstddef>
#include <cstdint>
extern "C" {
uint64_t bench_rounds();
uint64_t bench_seed();
void bench_begin();
void bench_end(uint64_t digest);
}

int main() {
  uint64_t rounds = bench_rounds();
  uint64_t seed = bench_seed();
  bench_begin();
  uint64_t digest = 0;
  uint64_t x = seed + 1000000007;
  for (uint64_t i = 0; i < rounds * 100000; i++) {
    uint64_t d = 3 + (i + seed) % 251;
    x = (x / d + x % d + 1234567) % 1000000007;
  }
  digest = x;
  bench_end(digest);
  return 0;
}
