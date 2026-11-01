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
    a[i] = uint64_t(i) * 3 + seed % 3;
  }
  uint64_t x = seed;
  for (uint64_t q = 0; q < rounds * 10000; q++) {
    x = (x * 48271) % 2147483647;
    uint64_t key = x % 12288;
    int lo = 0;
    int hi = 4096;
    while (lo < hi) {
      int mid = lo + (hi - lo) / 2;
      if (a[mid] < key) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    digest += uint64_t(lo);
  }
  bench_end(digest);
  return 0;
}
