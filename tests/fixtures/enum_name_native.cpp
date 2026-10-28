#include <cstdint>
#include <cstring>

extern "C" void enum_name_unknown(void *address) {
  const std::int8_t value = 7;
  std::memcpy(address, &value, sizeof(value));
}
