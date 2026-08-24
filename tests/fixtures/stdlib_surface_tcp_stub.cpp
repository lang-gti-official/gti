#include <cstdint>

namespace {
std::int32_t socket_result = 17;
std::int32_t close_calls = 0;
} // namespace

extern "C" void gti_test_set_socket_result(std::int32_t value) {
  socket_result = value;
}

extern "C" std::int32_t gti_test_close_calls() { return close_calls; }

extern "C" std::int32_t socket(std::int32_t, std::int32_t, std::int32_t) {
  return socket_result;
}

extern "C" std::int32_t close(std::int32_t) {
  ++close_calls;
  return -1;
}
