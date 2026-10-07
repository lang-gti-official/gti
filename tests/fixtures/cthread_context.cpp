#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>

namespace {
std::int32_t value = 0;
}

extern "C" void *cthread_context() { return &value; }
extern "C" void cthread_write(void *context) {
  *static_cast<std::int32_t *>(context) = 91;
}
extern "C" std::int32_t cthread_read() { return value; }
extern "C" void cthread_reset() { value = 0; }
extern "C" std::int32_t cthread_zero() { return 0; }
extern "C" void cthread_cleanup() {
  std::fputs("worker-cleaned\n", stdout);
  std::fflush(stdout);
}
extern "C" void cthread_install_terminal() {
  std::set_terminate([] { std::_Exit(86); });
}
extern "C" void cthread_block() {
  // The missing-join test holds a live worker until process termination.
  // No timing assumption or sanitizer suppression selects this state.
  static std::mutex mutex;
  static std::condition_variable condition;
  std::unique_lock lock(mutex);
  condition.wait(lock, [] { return false; });
}
extern "C" std::int32_t cthread_foreign(std::int32_t (*callback)(void *)) {
  std::thread worker([=] { (void)callback(nullptr); });
  worker.join();
  return 0;
}
extern "C" std::int32_t cthread_initial(std::int32_t (*callback)(void *)) {
  return callback(nullptr);
}
