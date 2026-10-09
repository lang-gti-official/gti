#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
int main() {
  std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz";
  uint64_t checksum = 0;
  for (int32_t round = 0; round < 400; round++) {
    std::string text;
    for (int32_t i = 0; i < 50000; i++)
      text.push_back(alphabet[std::size_t((i * 7 + round) % 26)]);
    std::size_t position = text.find('q');
    while (position != std::string::npos) {
      checksum += position;
      position = text.find('q', position + 1);
    }
    checksum += text.size();
  }
  std::printf("checksum %llu\n", (unsigned long long)checksum);
}
