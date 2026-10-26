#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>
using Operation = int (*)(int);
extern "C" void native_callback_cleanup() { std::fputs("gti-cleanup\n", stderr); }
extern "C" int native_callback_failure_invoke(Operation operation) {
  int result = operation(0);
  std::fputs("returned-through-C\n", stderr);
  return result;
}
extern "C" int native_callback_exception_invoke(Operation operation) {
  std::signal(SIGABRT, [](int) { std::_Exit(86); });
  std::set_terminate([] { std::abort(); });
  try {
    return operation(1);
  } catch (...) {
    std::fputs("escaped-through-C\n", stderr);
    return 87;
  }
}
extern "C" int native_callback_throw(int) {
  struct Cleanup { ~Cleanup() { std::fputs("foreign-cleanup\n", stderr); } } cleanup;
  throw 42;
}
