#pragma once

#include <cstring>
#include <limits>
#include <unwind.h>

namespace c04_native_test {
struct ForeignFailure {};
inline bool selected(const char *name) {
  const auto *value = std::getenv("GTI_C04_NATIVE_CASE");
  return value && std::strcmp(value, name) == 0;
}
[[noreturn]] inline void opaqueException() {
  auto *exception = new _Unwind_Exception{};
  exception->exception_class = 0x4754495445535400ULL;
  exception->exception_cleanup = [](_Unwind_Reason_Code,
                                    _Unwind_Exception *value) { delete value; };
  _Unwind_RaiseException(exception);
  std::_Exit(89);
}
} // namespace c04_native_test

extern "C" int32_t native_bridge(int32_t input, const int32_t *slot) {
  if (!slot || *slot != 0)
    std::_Exit(90);
  if (c04_native_test::selected("foreign-before"))
    throw c04_native_test::ForeignFailure{};
  try {
    if (c04_native_test::selected("translated"))
      throw c04_native_test::ForeignFailure{};
    return input;
  } catch (const c04_native_test::ForeignFailure &) {
    return input;
  }
}
extern "C" int32_t native_cleanup(int32_t input, const int32_t *slot) {
  if (!slot || *slot != 1)
    std::_Exit(91);
  if (input == std::numeric_limits<int32_t>::max()) {
    if (c04_native_test::selected("foreign-cleanup"))
      throw c04_native_test::ForeignFailure{};
    if (c04_native_test::selected("opaque-cleanup"))
      c04_native_test::opaqueException();
    if (c04_native_test::selected("impostor-cleanup")) {
#if GTI_C04_STATUS
      throw c04_native_test::ForeignFailure{};
#else
      throw c04_test::EvaluationFailure{false};
#endif
    }
  }
  return input;
}
