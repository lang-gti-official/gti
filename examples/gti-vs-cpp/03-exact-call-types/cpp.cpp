#include <cstdint>

int consume(std::uint8_t value) {
  return value;
}

int main() {
  std::int32_t value = 300;
  return consume(value) == 44 ? 0 : 1;
}
