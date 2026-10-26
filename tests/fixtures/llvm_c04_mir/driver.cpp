#include "carrier.h"
#include "generated.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

#if defined(GTI_C04_ALLOCATIONS)
#include "allocation_probe.h"

bool allocationControls() {
  c04_allocation_begin();
  auto *(*volatile allocate)(std::size_t) = &::operator new;
  void (*volatile release)(void *) noexcept = &::operator delete;
  release(allocate(37));
  try {
    throw 7;
  } catch (int value) {
    if (value != 7)
      return false;
  }
  const auto counted = c04_allocation_end();
  release(allocate(41));
  const auto disabled = c04_allocation_end();
  return counted.newCalls == 1 && counted.newBytes == 37 &&
         counted.deleteCalls == 1 && counted.exceptionCalls == 1 &&
         counted.exceptionBytes == sizeof(int) &&
         disabled.newCalls == counted.newCalls &&
         disabled.deleteCalls == counted.deleteCalls;
}
#endif

#if GTI_C04_NATIVE_BOUNDARY
#include "native_boundary_driver.h"
#endif

struct Observation {
  int32_t slots[3]{};
};

bool call(int32_t input, int32_t &value, Channel &failure, bool indirect,
          int32_t (&slots)[3]) {
  auto *volatile target = &GTI_C04_ENTRY;
#if GTI_C04_BORROWED_CLEANUP
#define GTI_C04_ARGUMENTS input, &slots[0], &slots[1], &slots[2]
#else
#define GTI_C04_ARGUMENTS input
#endif
#if GTI_C04_STATUS
#if GTI_C04_NATIVE_BOUNDARY
  try {
#endif
    return indirect ? target(&value, &failure, GTI_C04_ARGUMENTS)
                    : GTI_C04_ENTRY(&value, &failure, GTI_C04_ARGUMENTS);
#if GTI_C04_NATIVE_BOUNDARY
  } catch (...) {
    std::fputs("C04 foreign exception outside channel\n", stderr);
    std::_Exit(86);
  }
#endif
#else
  try {
    value =
        indirect ? target(GTI_C04_ARGUMENTS) : GTI_C04_ENTRY(GTI_C04_ARGUMENTS);
    return true;
  } catch (const c04_test::EvaluationFailure &caught) {
    if (!c04_test::unwinding || &caught != c04_test::activeCarrier)
      c04_test::foreignException();
    failure = c04_test::activeFailure;
    c04_test::unwinding = false;
    return false;
#if GTI_C04_NATIVE_BOUNDARY
  } catch (...) {
    c04_test::foreignException();
#endif
  }
#endif
#undef GTI_C04_ARGUMENTS
}

int main(int argc, char **argv) {
#if defined(GTI_C04_ALLOCATIONS)
  if (!allocationControls())
    return 9;
#endif
  const bool failureCase = argc > 1 && argv[1][0] == 'f';
  const bool mixed = argc > 1 && argv[1][0] == 'm';
  const bool indirect = argc > 2 && argv[2][0] == 'i';
  const unsigned long iterations =
      argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 1;
  if (iterations == 0)
    return 2;
  Observation *observation = nullptr;
  if (argc > 4) {
    const int fd = open(argv[4], O_RDWR);
    if (fd < 0)
      return 7;
    void *mapping = mmap(nullptr, sizeof(Observation), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED)
      return 8;
    // A parent process can inspect cleanup even after GTI's terminal _Exit.
    observation = ::new (mapping) Observation{};
  }
  Channel failure{};
  int32_t value = 0;
  std::uint64_t sum = 0;
#if defined(GTI_C04_ALLOCATIONS)
  c04_allocation_begin();
#endif
  const auto started = std::chrono::steady_clock::now();
  for (unsigned long index = 0; index < iterations; ++index) {
    const bool failing = failureCase || (mixed && index % 2 != 0);
    const auto input = failing ? std::numeric_limits<int32_t>::max()
                               : static_cast<int32_t>(index % 1024);
    int32_t localSlots[3]{};
    auto &slots = observation ? observation->slots : localSlots;
    for (auto &slot : slots)
      slot = 0;
    const bool succeeded = call(input, value, failure, indirect, slots);
#if GTI_C04_BORROWED_CLEANUP
    const auto leafDrops =
#if GTI_C04_FAILED_CONSTRUCTION
        failing ? 0 : 1;
#else
        1;
#endif
    const auto ownerDrops =
#if GTI_C04_OWNED_ROLLBACK
        failing ? 0 : 1;
#else
        1;
#endif
    if (slots[0] != leafDrops || slots[1] != 1 || slots[2] != ownerDrops)
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
#if defined(GTI_C04_ALLOCATIONS)
  const auto counted = c04_allocation_end();
  const char *report = std::getenv("GTI_C04_ALLOCATION_REPORT");
  FILE *file = report ? std::fopen(report, "w") : nullptr;
  if (!file)
    return 10;
  std::fprintf(file,
               "{\"controls\":true,\"new_calls\":%llu,\"new_bytes\":%llu,"
               "\"delete_calls\":%llu,\"exception_calls\":%llu,"
               "\"exception_bytes\":%llu}\n",
               static_cast<unsigned long long>(counted.newCalls),
               static_cast<unsigned long long>(counted.newBytes),
               static_cast<unsigned long long>(counted.deleteCalls),
               static_cast<unsigned long long>(counted.exceptionCalls),
               static_cast<unsigned long long>(counted.exceptionBytes));
  std::fclose(file);
#endif
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  if (observation)
    munmap(observation, sizeof(Observation));
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
