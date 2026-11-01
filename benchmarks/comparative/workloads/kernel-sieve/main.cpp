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
  for (uint64_t r = 0; r < rounds; r++) {
    int a[32768] = {};
    int n = 30000 + int((seed + r) % 2000);
    for (int p = 2; p * p < n; p++) {
      if (a[p] == 0) {
        for (int j = p * p; j < n; j += p) {
          a[j] = 1;
        }
      }
    }
    for (int i = 2; i < n; i++) {
      if (a[i] == 0) {
        digest += uint64_t(i);
      }
    }
  }
  bench_end(digest);
  return 0;
}
