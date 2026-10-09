#include <cstdint>
#include <cstdio>
#include <expected>
enum class parse_error { invalid };
static std::expected<int32_t, parse_error> digit(int32_t code) {
  if (code < 48 || code > 57)
    return std::unexpected(parse_error::invalid);
  return code - 48;
}
static std::expected<int32_t, parse_error> two_digits(int32_t high,
                                                      int32_t low) {
  auto first = digit(high);
  if (!first)
    return std::unexpected(first.error());
  auto second = digit(low);
  if (!second)
    return std::unexpected(second.error());
  return *first * 10 + *second;
}
int main() {
  int64_t total = 0, errors = 0;
  for (int32_t i = 0; i < 100000000; i++) {
    int32_t high = 48 + i % 11;
    int32_t low = 48 + (i / 11) % 10;
    auto parsed = two_digits(high, low);
    if (parsed)
      total += *parsed;
    else
      errors++;
  }
  std::printf("checksum %lld\n", (long long)(total + errors));
}
