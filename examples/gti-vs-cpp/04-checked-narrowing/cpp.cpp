#include <cstdint>

int main() {
  std::int32_t value = 300;
  std::uint8_t narrowed = static_cast<std::uint8_t>(value);
  return narrowed == 44 ? 0 : 1;
}
