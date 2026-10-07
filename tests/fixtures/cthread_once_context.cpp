#include "gti/cthread.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
namespace {
std::int32_t counts[4] = {};
}
extern "C" void once_arrive() {
  static std::atomic<int> arrived{0};
  arrived.fetch_add(1);
  while (arrived.load() != 8) {
    std::this_thread::yield();
  }
}
extern "C" void once_write(std::int32_t slot) { ++counts[slot]; }
extern "C" std::int32_t once_read(std::int32_t slot) { return counts[slot]; }
extern "C" std::int32_t once_zero() { return 0; }
extern "C" void once_cleanup() {
  std::fputs("once-cleaned\n", stdout);
  std::fflush(stdout);
}
extern "C" void once_owner_cleanup() {
  std::fputs("owner-cleaned\n", stdout);
  std::fflush(stdout);
}
extern "C" void once_terminal() {
  std::set_terminate([] { std::_Exit(86); });
}
extern "C" void once_unregistered(gti_once_initializer_v1 callback) {
  callback();
}
extern "C" void once_foreign(gti_once_initializer_v1 callback) {
  std::thread worker([=] { gti_rt_once_invoke_v1(callback); });
  worker.join();
}
