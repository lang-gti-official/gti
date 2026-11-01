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
  uint64_t a[4096] = {};
  for (int i = 0; i < 4096; i++) {
    a[i] = (uint64_t(i) + seed) % 251;
  }
  for (uint64_t r = 0; r < rounds * 32; r++) {
    for (int i = 0; i < 4096; i++) {
      a[i] = (a[i] * 17 + r % 251) % 251;
    }
  }
  for (int i = 0; i < 4096; i++) {
    digest += a[i];
  }
  bench_end(digest);
  return 0;
}
