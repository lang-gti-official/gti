#include <cstdint>
#include <cstdio>
int main() {
  uint64_t state = 88172645463325252ull, acc = 0;
  for (int32_t i = 0; i < 400000000; i++) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    acc ^= state >> uint64_t(i & 31);
  }
  std::printf("checksum %llu\n", (unsigned long long)acc);
}
