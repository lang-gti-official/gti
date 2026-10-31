#include <cstddef>
#include <cstdint>
extern "C" {
uint64_t bench_rounds();
uint64_t bench_seed();
void bench_begin();
void bench_end(uint64_t digest);
}
uint64_t tree(int depth, uint64_t x) {
  if (depth == 0) {
    return x % 251;
  }
  return tree(depth - 1, (x * 17 + 3) % 1000003) +
         tree(depth - 1, (x * 13 + 7) % 1000003);
}
int main() {
  uint64_t rounds = bench_rounds();
  uint64_t seed = bench_seed();
  bench_begin();
  uint64_t digest = 0;
  for (uint64_t r = 0; r < rounds * 16; r++) {
    digest += tree(16 + int((seed + r) % 3), seed + r);
  }
  bench_end(digest);
  return 0;
}
