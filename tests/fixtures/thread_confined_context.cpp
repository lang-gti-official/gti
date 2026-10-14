#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
using Transform = std::int32_t (*)(std::int32_t);
static Transform saved = nullptr;
extern "C" std::int32_t confined_mode() {
  std::set_terminate([] { std::_Exit(86); });
  const char *mode = std::getenv("GTI_CONFINED_TEST_MODE");
  return mode ? std::atoi(mode) : 0;
}
extern "C" std::int32_t confined_invoke(Transform callback,
                                        std::int32_t value) {
  return callback(value);
}
extern "C" std::int32_t confined_foreign(Transform callback) {
  std::thread worker([=] { (void)callback(1); });
  worker.join();
  return 9;
}
extern "C" void confined_save(Transform callback) { saved = callback; }
extern "C" std::int32_t confined_saved() { return saved(1); }
extern "C" std::int32_t confined_zero() { return 0; }
extern "C" void confined_clean() {
  std::puts("callback-cleaned");
  std::fflush(stdout);
}
