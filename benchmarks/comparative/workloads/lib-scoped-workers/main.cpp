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
uint64_t work(uint64_t input) {
  uint64_t state = input % 65521;
  for (int i = 0; i < 250000; i++)
    state = (state * 17 + uint64_t(i)) % 65521;
  return state;
}
int main() {
  const uint64_t rounds = bench_rounds(), seed = bench_seed() % 65536;
  uint64_t digest = 0;
  bench_begin();
  for (uint64_t round = 0; round < rounds; ++round) {
    uint64_t first = 0, second = 0, third = 0, fourth = 0;
    std::thread a([&] { first = work(seed + round); });
    std::thread b([&] { second = work(seed + round + 1); });
    std::thread c([&] { third = work(seed + round + 2); });
    std::thread d([&] { fourth = work(seed + round + 3); });
    a.join();
    b.join();
    c.join();
    d.join();
    digest += first + second + third + fourth;
  }
  bench_end(digest);
  return 0;
}
