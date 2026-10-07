#include <cstdint>
#include <cstring>

extern "C" std::uint64_t native_c_string_length(const char *value) {
  return static_cast<std::uint64_t>(std::strlen(value));
}

extern "C" std::uint8_t native_c_string_zero_at(const char *value,
                                                std::uint64_t index) {
  return value != nullptr && value[index] == '\0' ? std::uint8_t{1}
                                                  : std::uint8_t{0};
}
