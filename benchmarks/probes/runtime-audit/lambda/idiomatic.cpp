#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
int main() {
  std::vector<int32_t> values;
  for (int32_t i = 0; i < 1000000; i++)
    values.push_back(int32_t((int64_t(i) * 7919) % 10007));
  int64_t total = 0;
  for (int32_t round = 0; round < 300; round++) {
    int32_t threshold = round * 33;
    total += std::count_if(
        values.begin(), values.end(),
        [threshold](const int32_t &value) { return value > threshold; });
  }
  std::printf("checksum %lld\n", (long long)total);
}
