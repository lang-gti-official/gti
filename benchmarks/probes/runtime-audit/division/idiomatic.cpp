#include <cstdint>
#include <cstdio>
#include <vector>
int main() {
  std::vector<int32_t> divisors;
  for (int32_t i = 0; i < 1024; i++) divisors.push_back((i * 37) % 97 + 1);
  int64_t acc = 0;
  for (int32_t i = 1; i < 150000000; i++) {
    int32_t proved = (i % 97) + 1;
    int32_t loaded = divisors[std::size_t(i & 1023)];
    int32_t value = i ^ 1540483477;
    acc += int64_t(value / proved) + int64_t(value % loaded);
  }
  std::printf("checksum %lld\n", (long long)acc);
}
