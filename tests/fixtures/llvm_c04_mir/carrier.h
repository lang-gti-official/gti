#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

#if !GTI_C04_STATUS
#include <cxxabi.h>
#include <unwind.h>

namespace c04_test {
inline thread_local Channel activeFailure{};
inline thread_local bool unwinding = false;
inline thread_local const void *activeCarrier = nullptr;
inline thread_local unsigned channelReads = 0;

struct EvaluationFailure {
  explicit EvaluationFailure(bool registerCarrier) noexcept {
    if (registerCarrier)
      activeCarrier = this;
  }
  EvaluationFailure(const EvaluationFailure &) = delete;
  ~EvaluationFailure() {
    if (activeCarrier == this)
      activeCarrier = nullptr;
  }
};

[[noreturn]] inline void foreignException() noexcept {
  std::fprintf(stderr, "C04 foreign exception: active=%d reads=%u\n",
               static_cast<int>(unwinding), channelReads);
  std::_Exit(86);
}

inline bool isActiveCarrier(void *exception) noexcept {
  if (!exception || !unwinding || !activeCarrier)
    return false;
  // This is the public Level-I unwind header, not a C++ exception header.
  // Reject other runtimes and dependent exceptions before using the native
  // C++ ABI accessor; never cast or inspect the foreign payload itself.
  const auto *header = static_cast<const _Unwind_Exception *>(exception);
#if defined(_LIBCPP_VERSION)
  constexpr std::uint64_t nativeClass = 0x434c4e47432b2b00ULL;
#elif defined(__GLIBCXX__)
  constexpr std::uint64_t nativeClass = 0x474e5543432b2b00ULL;
#else
#error "C04 carrier identity requires a reviewed Itanium C++ runtime"
#endif
  return header->exception_class == nativeClass &&
         __cxxabiv1::__cxa_get_exception_ptr(exception) == activeCarrier;
}
} // namespace c04_test

extern "C" [[noreturn]] void __gti_c04_evaluate_raise(const Channel *channel) {
  if (c04_test::unwinding)
    std::terminate();
  c04_test::activeFailure = *channel;
  c04_test::unwinding = true;
  throw c04_test::EvaluationFailure{true};
}
extern "C" void __gti_c04_evaluate_current(Channel *channel,
                                           void *exception) noexcept {
  if (!c04_test::isActiveCarrier(exception))
    c04_test::foreignException();
  ++c04_test::channelReads;
  *channel = c04_test::activeFailure;
}
#endif
