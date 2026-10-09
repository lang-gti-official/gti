#include <cstdint>
#include <cstdio>
#include <unordered_map>
int main() {
  std::unordered_map<int32_t, int32_t> table;
  for (int32_t i = 0; i < 1000000; i++)
    table.insert({int32_t((int64_t(i) * 7919) % 2000003), i});
  int64_t hits = 0;
  for (int32_t round = 0; round < 5; round++)
    for (int32_t i = 0; i < 2000000; i++) {
      int32_t key = i + round;
      if (table.contains(key))
        hits += table.at(key);
    }
  std::printf("checksum %lld\n", (long long)hits);
}
