#include <cstdint>
#include <cstdlib>

extern "C" std::int32_t square_mode() {
  const char *value = std::getenv("GTI_SQUARE_CASE");
  return value ? std::atoi(value) : 0;
}
extern "C" std::int64_t square_input() {
  constexpr std::int64_t inputs[] = {
      46340,      -46340,     46341,      -46341,      26754,      -26754,
      26755,      -26755,     3037000499, -3037000499, 3037000500, -3037000500,
      4294967295, 4294967296, 2147483647, 1431655765,  715827882,  65535,
      65536,      0,          -1,         -1,          0};
  const auto mode = square_mode();
  return mode >= 0 && mode < 23 ? inputs[mode] : 0;
}
