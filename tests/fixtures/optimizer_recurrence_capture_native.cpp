#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {
std::int64_t *retained;
}

extern "C" std::int32_t recurrence_mode() {
  const char *mode = std::getenv("GTI_RECURRENCE_CASE");
  return mode ? std::atoi(mode) : 0;
}
extern "C" std::int32_t recurrence_value() {
  return recurrence_mode() == 1 ? -1 : 2147483647;
}
extern "C" void recurrence_barrier() {
  if (retained)
    *retained = std::numeric_limits<std::int64_t>::max();
}
extern "C" void recurrence_stash(std::int64_t *address) { retained = address; }
extern "C" void recurrence_alter(std::int64_t *address) {
  *address = std::numeric_limits<std::int64_t>::max();
}
