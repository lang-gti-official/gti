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
  double a[2304] = {};
  double b[2304] = {};
  double c[2304] = {};
  for (int i = 0; i < 2304; i++) {
    a[i] = double((uint64_t(i) + seed) % 17) * 0.125;
    b[i] = double(i % 13) * 0.25;
  }
  for (uint64_t r = 0; r < rounds; r++) {
    for (int i = 0; i < 48; i++) {
      for (int k = 0; k < 48; k++) {
        double v = a[i * 48 + k];
        for (int j = 0; j < 48; j++) {
          c[i * 48 + j] += v * b[k * 48 + j];
        }
      }
    }
  }
  double total = 0.0;
  for (int i = 0; i < 2304; i++) {
    total += c[i];
  }
  digest = uint64_t(total);
  bench_end(digest);
  return 0;
}
