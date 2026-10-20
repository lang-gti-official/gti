#include <cstdint>

namespace {
std::int32_t value = 19;
const std::int32_t readonlyValue = 23;
std::int32_t *pointerGetter() { return &value; }
const std::int32_t *readonlyGetter() { return &readonlyValue; }
std::int32_t scalarGetter() { return 42; }
void voidGetter() {}
} // namespace

extern "C" auto projection_pointer_getter() -> std::int32_t *(*)() {
  return pointerGetter;
}
extern "C" auto projection_readonly_getter() -> const std::int32_t *(*)() {
  return readonlyGetter;
}
extern "C" auto projection_scalar_getter() -> std::int32_t (*)() {
  return scalarGetter;
}
extern "C" auto projection_void_getter() -> void (*)() { return voidGetter; }
