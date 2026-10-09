#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
int main() {
  uint64_t state = 88172645463325252ull, checksum = 0;
  for (int32_t round = 0; round < 8; round++) {
    std::vector<int32_t> values;
    for (int32_t i = 0; i < 1000000; i++) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      values.push_back(int32_t(state % 1000000000ull));
    }
    std::sort(values.begin(), values.end());
    checksum += uint64_t(values[0]) + uint64_t(values[500000]);
  }
  std::printf("checksum %llu\n", (unsigned long long)checksum);
}
