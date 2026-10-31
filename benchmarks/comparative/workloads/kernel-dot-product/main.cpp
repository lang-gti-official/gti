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
  double a[4096] = {};
  double b[4096] = {};
  for (int i = 0; i < 4096; i++) {
    a[i] = double((uint64_t(i) + seed) % 17) * 0.125;
    b[i] = double(i % 13) * 0.25;
  }
  double total = 0.0;
  for (uint64_t r = 0; r < rounds * 64; r++) {
    a[r % 4096] += 0.125;
    for (int i = 0; i < 4096; i++) {
      total += a[i] * b[i];
    }
  }
  digest = uint64_t(total);
  bench_end(digest);
  return 0;
}
