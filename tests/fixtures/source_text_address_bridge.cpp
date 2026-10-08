#include <cstdint>
#include <cstring>

extern "C" std::int32_t source_bridge_byte(const char *text,
                                           std::uint64_t index) {
  return static_cast<unsigned char>(text[index]);
}
extern "C" std::uint64_t source_bridge_length(const char *text) {
  return std::strlen(text);
}
