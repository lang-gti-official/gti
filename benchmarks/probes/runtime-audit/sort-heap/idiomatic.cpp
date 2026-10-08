// Same iterative heapsort as stdlib/std/algorithm.gti, with C++ vector access.
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>
static void heap_sort(std::vector<int32_t>& values) {
  size_t end = values.size(), start = end / 2;
  while (end > 1) {
    size_t root = 0;
    if (start > 0) { start--; root = start; }
    else { end--; std::swap(values[0], values[end]); }
    while (root < end / 2) {
      size_t child = root * 2 + 1;
      if (child + 1 < end && values[child] < values[child + 1]) child++;
      if (!(values[root] < values[child])) break;
      std::swap(values[root], values[child]);
      root = child;
    }
  }
}
int main() {
  uint64_t state = 88172645463325252ull, checksum = 0;
  for (int32_t round = 0; round < 8; round++) {
    std::vector<int32_t> values;
    for (int32_t i = 0; i < 1000000; i++) {
      state ^= state << 13; state ^= state >> 7; state ^= state << 17;
      values.push_back(int32_t(state % 1000000000ull));
    }
    heap_sort(values);
    checksum += uint64_t(values[0]) + uint64_t(values[500000]);
  }
  std::printf("checksum %llu\n", (unsigned long long)checksum);
}
