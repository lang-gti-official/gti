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
  double x = double(seed % 97) * 0.125;
  double total = 0.0;
  for (uint64_t i = 0; i < rounds * 200000; i++) {
    x = x * 0.9999 + 0.0001;
    total += x;
  }
  digest = uint64_t(total * 1000.0);
  bench_end(digest);
  return 0;
}
