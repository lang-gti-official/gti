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
struct shape {
  virtual int32_t area() const = 0;
  virtual ~shape() = default;
};
struct square : shape {
  int32_t side;
  square(int32_t v) : side(v) {}
  int32_t area() const override { return side * side; }
};
struct circle : shape {
  int32_t radius;
  circle(int32_t v) : radius(v) {}
  int32_t area() const override { return 3 * radius * radius; }
};
uint64_t sum_areas(const shape &item, int32_t count) {
  uint64_t total = 0;
  for (int32_t i = 0; i < count; i++)
    total += uint64_t(item.area() + i % 7);
  return total;
}
int main() {
  const uint64_t rounds = bench_rounds(), seed = bench_seed() % 65536;
  uint64_t digest = 0;
  bench_begin();
  for (uint64_t round = 0; round < rounds; ++round) {
    square s(int32_t((seed + round) % 16) + 1);
    circle c(int32_t((seed + round) % 7) + 1);
    digest += sum_areas(s, 500000);
    digest += sum_areas(c, 500000);
  }
  bench_end(digest);
  return 0;
}
