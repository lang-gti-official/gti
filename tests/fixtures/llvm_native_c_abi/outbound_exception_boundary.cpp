#include "outbound_exception_boundary.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {
struct ForeignFailure {};
struct ForeignGuard {
  ~ForeignGuard() { std::fputs("foreign-cleanup\n", stderr); }
};
int32_t foreign_operation(int32_t mode) {
  ForeignGuard guard;
  if (mode != 0)
    throw ForeignFailure{};
  return 41;
}
} // namespace

extern "C" int32_t native_boundary_call(int32_t mode) {
  // Mode 3 deliberately violates the documented C shim contract. It is a
  // fault-injection observation, never a supported foreign-call example.
  if (mode == 3)
    return foreign_operation(mode);
  try {
    return foreign_operation(mode);
  } catch (const ForeignFailure &) {
    if (mode == 1 || mode == 4) {
      std::fputs("translated-in-shim\n", stderr);
      return mode == 1 ? -7 : 0;
    }
    std::fputs("terminal-in-shim\n", stderr);
    std::abort();
  } catch (...) {
    std::abort();
  }
}
extern "C" Operation native_boundary_select() { return native_boundary_call; }
extern "C" void native_boundary_cleanup(int32_t id) {
  std::fprintf(stderr, "gti-cleanup-%d\n", static_cast<int>(id));
}
extern "C" int32_t native_boundary_host(Operation direct, Operation indirect) {
  const char *mode_text = std::getenv("GTI_NATIVE_BOUNDARY_MODE");
  const char *path_text = std::getenv("GTI_NATIVE_BOUNDARY_PATH");
  if (!mode_text || !path_text)
    return 64;
  const int mode = std::atoi(mode_text);
  if (mode < 0 || mode > 4)
    return 64;
  std::signal(SIGABRT, [](int) { std::_Exit(86); });
  std::set_terminate([] { std::abort(); });
  try {
    const int32_t result = (path_text[0] == 'd' ? direct : indirect)(mode);
    std::fprintf(stderr, "returned-through-C:%d\n", static_cast<int>(result));
    return result == (mode == 0 ? 41 : -7) ? 0 : 88;
  } catch (...) {
    std::fputs("escaped-callback\n", stderr);
    return 87;
  }
}
