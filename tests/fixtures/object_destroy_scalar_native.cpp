#include <cstdint>
#include <new>

extern "C" std::int32_t *lifetime_test_integer(std::int32_t value) {
  return new (std::nothrow) std::int32_t(value);
}

extern "C" void lifetime_test_release(std::int32_t *address) {
  // The GTI operation ended the scalar's lifetime; only bytes remain to
  // release.
  ::operator delete(address);
}
