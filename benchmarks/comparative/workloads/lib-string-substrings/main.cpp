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

int main() {
  const uint64_t rounds = bench_rounds(), seed = bench_seed() % 65536;
  uint64_t digest = 0;
  bench_begin();
  for (uint64_t round = 0; round < rounds; ++round) {
    const char *alphabet = "abcdefghijklmnopqrstuvwxyz";
    std::string text;
    for (int i = 0; i < 4096; ++i)
      text.push_back(alphabet[(uint64_t(i) + seed + round) % 26]);
    for (size_t i = 0; i < 2048; ++i) {
      auto piece = text.substr(i, 32);
      for (size_t j = 0; j < piece.size(); ++j)
        if (piece[j] == 'q')
          digest += j + 1;
    }
  }
  bench_end(digest);
  return 0;
}
