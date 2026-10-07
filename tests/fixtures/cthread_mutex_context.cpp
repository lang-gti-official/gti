#include <cstdint>
#include <cstdlib>
#include <exception>
namespace {
struct Context {
  std::uint64_t mutex = 0;
  std::int32_t count = 0;
};
Context value;
} // namespace
extern "C" void *cthread_mutex_context(std::uint64_t handle) {
  value = {handle, 0};
  return &value;
}
extern "C" std::uint64_t cthread_mutex_handle(void *context) {
  return static_cast<Context *>(context)->mutex;
}
extern "C" std::int32_t *cthread_mutex_value(void *context) {
  return &static_cast<Context *>(context)->count;
}
extern "C" void cthread_mutex_terminal() {
  std::set_terminate([] { std::_Exit(86); });
}
