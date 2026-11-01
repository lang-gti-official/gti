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
    for (int i = 0; i < 65536; ++i)
      text.push_back(alphabet[(uint64_t(i) * 7 + seed + round) % 26]);
    size_t position = text.find('q');
    while (position != std::string::npos) {
      digest += position;
      position = text.find('q', position + 1);
    }
    digest += text.size();
  }
  bench_end(digest);
  return 0;
}
