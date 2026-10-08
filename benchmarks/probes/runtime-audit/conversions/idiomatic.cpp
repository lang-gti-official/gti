#include <cstdint>
#include <cstdio>
#include <vector>
int main() {
  std::vector<int64_t> wide;
  for (int64_t i = 0; i < 4096; i++) wide.push_back((i * 2654435761ll) % 2000000000ll);
  int64_t acc = 0;
  for (int32_t round = 0; round < 50000; round++)
    for (int64_t value : wide) {
      int32_t narrow = int32_t(value);
      uint8_t low = uint8_t(narrow & 255);
      acc += int64_t(narrow / 1024) + int64_t(low);
    }
  std::printf("checksum %lld\n", (long long)acc);
}
