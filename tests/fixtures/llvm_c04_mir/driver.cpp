#include "generated.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>

// The isolated experimental edge is explicitly unwind-permitted. It does not
// alter any production C callback/runtime service contract. C++ exception
// storage/OOM/foreign exception handling are not validated by this experiment.
#if !GTI_C04_STATUS
namespace {
struct EvaluationFailure {};
thread_local Channel activeFailure{};
thread_local bool unwinding = false;
} // namespace
extern "C" [[noreturn]] void __gti_c04_evaluate_raise(const Channel *channel) {
  if (unwinding)
    std::terminate();
  activeFailure = *channel;
  unwinding = true;
  throw EvaluationFailure{};
}
extern "C" void __gti_c04_evaluate_current(Channel *channel) noexcept {
  if (!unwinding)
    std::terminate();
  *channel = activeFailure;
}
#endif

bool call(int32_t input, int32_t &value, Channel &failure, bool indirect,
          int32_t (&slots)[3]) {
  auto *volatile target = &GTI_C04_ENTRY;
#if GTI_C04_BORROWED_CLEANUP
#define GTI_C04_ARGUMENTS input, &slots[0], &slots[1], &slots[2]
#else
#define GTI_C04_ARGUMENTS input
#endif
#if GTI_C04_STATUS
  return indirect ? target(&value, &failure, GTI_C04_ARGUMENTS)
                  : GTI_C04_ENTRY(&value, &failure, GTI_C04_ARGUMENTS);
#else
  try {
    value =
        indirect ? target(GTI_C04_ARGUMENTS) : GTI_C04_ENTRY(GTI_C04_ARGUMENTS);
    return true;
  } catch (const EvaluationFailure &) {
    failure = activeFailure;
    unwinding = false;
    return false;
  }
#endif
#undef GTI_C04_ARGUMENTS
}

int main(int argc, char **argv) {
  const bool failureCase = argc > 1 && argv[1][0] == 'f';
  const bool mixed = argc > 1 && argv[1][0] == 'm';
  const bool indirect = argc > 2 && argv[2][0] == 'i';
  const unsigned long iterations =
      argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 1;
  if (iterations == 0)
    return 2;
  Channel failure{};
  int32_t value = 0;
  std::uint64_t sum = 0;
  const auto started = std::chrono::steady_clock::now();
  for (unsigned long index = 0; index < iterations; ++index) {
    const bool failing = failureCase || (mixed && index % 2 != 0);
    const auto input = failing ? std::numeric_limits<int32_t>::max()
                               : static_cast<int32_t>(index % 1024);
    int32_t slots[3]{};
    const bool succeeded = call(input, value, failure, indirect, slots);
#if GTI_C04_BORROWED_CLEANUP
    if (slots[0] != 1 || slots[1] != 1 || slots[2] != 1)
      return 6;
#endif
    if (succeeded == failing || (!failing && value != input + 1))
      return 3;
    if (failing) {
      if (failure.record.code != GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1 ||
          failure.record.detail != GTI_FAILURE_DETAIL_ADDITION_V1 ||
          gti_rt_failure_validate_v2(&failure.record, failure.artifact) !=
              GTI_FAILURE_REPORT_OK_V1)
        return 4;
      sum += failure.record.code;
    } else {
      sum += static_cast<std::uint32_t>(value);
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  if (iterations == 1) {
    if (failureCase)
      return gti_rt_failure_write_report_v2(
                 &failure.record, failure.artifact) == GTI_FAILURE_REPORT_OK_V1
                 ? 0
                 : 5;
    std::puts("1");
  } else {
    std::printf("%lu %lld %llu\n", iterations, static_cast<long long>(elapsed),
                static_cast<unsigned long long>(sum));
  }
  return 0;
}
