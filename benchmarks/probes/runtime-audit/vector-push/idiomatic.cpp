#include <cstdint>
#include <cstdio>
#include <vector>
int main() {
  uint64_t checksum = 0;
  for (int32_t round = 0; round < 300; round++) {
    std::vector<int32_t> values;
    for (int32_t i = 0; i < 200000; i++) values.push_back(i ^ round);
    for (int32_t& value : values) checksum += uint64_t(value);
  }
  std::printf("checksum %llu\n", (unsigned long long)checksum);
}
