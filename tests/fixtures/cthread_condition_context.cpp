#include <cstdint>
#include <cstdlib>
#include <exception>
namespace {
struct Context {
  std::uint64_t mutex = 0;
  std::uint64_t condition = 0;
  std::int32_t turn = 0;
  std::int32_t count = 0;
};
Context value;
} // namespace
extern "C" void *cthread_condition_context(std::uint64_t mutex,
                                           std::uint64_t condition) {
  value = {mutex, condition, 0, 0};
  return &value;
}
extern "C" std::uint64_t cthread_condition_mutex(void *context) {
  return static_cast<Context *>(context)->mutex;
}
extern "C" std::uint64_t cthread_condition_handle(void *context) {
  return static_cast<Context *>(context)->condition;
}
extern "C" std::int32_t *cthread_condition_turn(void *context) {
  return &static_cast<Context *>(context)->turn;
}
extern "C" std::int32_t *cthread_condition_count(void *context) {
  return &static_cast<Context *>(context)->count;
}
extern "C" void cthread_condition_terminal() {
  std::set_terminate([] { std::_Exit(86); });
}
