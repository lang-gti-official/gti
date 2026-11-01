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
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

int main() {
  const uint64_t rounds = bench_rounds(), seed = bench_seed() % 65536;
  uint64_t digest = 0;
  bench_begin();
  for (uint64_t round = 0; round < rounds; ++round) {
    std::mutex mutex;
    uint64_t value = seed + round;
    for (int i = 0; i < 100000; ++i) {
      std::lock_guard<std::mutex> guard(mutex);
      value += 1;
    }
    std::lock_guard<std::mutex> guard(mutex);
    digest += value;
  }
  bench_end(digest);
  return 0;
}
