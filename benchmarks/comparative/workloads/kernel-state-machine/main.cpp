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
  uint64_t x = seed;
  int state = 0;
  for (uint64_t i = 0; i < rounds * 100000; i++) {
    x = (x * 48271) % 2147483647;
    switch (state) {
    case 0:
      state = int(x % 3);
      break;
    case 1:
      state = int(x % 4);
      digest += 1;
      break;
    case 2:
      state = int(x % 2);
      digest += 2;
      break;
    default:
      state = 0;
      digest += 3;
      break;
    }
  }
  digest += uint64_t(state);
  bench_end(digest);
  return 0;
}
