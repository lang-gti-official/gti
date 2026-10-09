#include <cstdint>
#include <cstdio>
static int32_t step(int32_t x, int32_t k) { return (x * 3 + k) % 1000003; }
int main() {
  int32_t acc = 1;
  for (int32_t i = 0; i < 200000000; i++)
    acc = step(acc, i % 7);
  std::printf("checksum %d\n", acc);
}
