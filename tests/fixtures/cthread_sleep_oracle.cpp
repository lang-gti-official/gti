#include <cstdint>
#include <cstdlib>

namespace {
std::int32_t mode = 0;
std::int32_t calls = 0;
} // namespace

extern "C" void cthread_sleep_mode(std::int32_t value) { mode = value; }
extern "C" std::int32_t cthread_sleep_calls() { return calls; }
extern "C" std::int32_t
gti_rt_cthread_sleep_v1(std::int64_t seconds, std::int64_t nanoseconds,
                        std::int64_t *remainingSeconds,
                        std::int64_t *remainingNanoseconds) {
  ++calls;
  if (seconds != 8 || nanoseconds != 900000000 || remainingSeconds == nullptr ||
      remainingNanoseconds == nullptr ||
      remainingSeconds == remainingNanoseconds) {
    std::abort();
  }
  *remainingSeconds = 2;
  *remainingNanoseconds = 250000000;
  return mode;
}
