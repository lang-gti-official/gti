#include "gti/runtime_mutex.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifdef GTI_COST_LLVM
extern "C" {
void gti_test_mutex_initialize(void *, int32_t);
void gti_test_mutex_destroy(void *);
void gti_test_mutex_transfer(void *, void *);
void gti_test_mutex_cleanup(void *);
void gti_test_mutex_token(void *);
int32_t gti_test_mutex_lock(void *, void *);
void gti_test_mutex_unlock(void *, void *);
int32_t *gti_test_mutex_read_mut(void *, void *);
}
#endif

namespace {
constexpr int rounds = 9;
constexpr int iterations = 500000;
using Clock = std::chrono::steady_clock;
using Samples = std::array<double, rounds>;

void require(bool condition) {
  if (!condition)
    std::abort();
}

template <class Work> double measure(Work work) {
  const auto start = Clock::now();
  for (int index = 0; index < iterations; ++index)
    work();
  return std::chrono::duration<double, std::nano>(Clock::now() - start)
             .count() /
         iterations;
}

void report(const char *name, Samples values,
            const char *operation = "lock/increment/unlock") {
  std::sort(values.begin(), values.end());
  std::printf("%s %s ns: median=%.3f min=%.3f max=%.3f\n", name, operation,
              values[rounds / 2], values.front(), values.back());
}
} // namespace

int main() {
  // Avoid libc's process-has-never-created-a-thread optimization asymmetry.
  std::thread([] {}).join();
  std::mutex reference;
  gti_mutex_slot_v1 slot{};
  gti_rt_mutex_initialize_v1(&slot);
  int nativeValue = 0, referenceValue = 0;
  Samples native{}, standard{};
#ifdef GTI_COST_LLVM
  alignas(8) std::array<unsigned char, 72> storage{};
  void *token;
  gti_test_mutex_initialize(storage.data(), 0);
  gti_test_mutex_token(&token);
  Samples emitted{};
#endif
  const auto runNative = [&] {
    require(gti_rt_mutex_lock_v1(&slot) == 0);
    ++nativeValue;
    gti_rt_mutex_unlock_v1(&slot);
  };
  const auto runStandard = [&] {
    const std::lock_guard lock(reference);
    ++referenceValue;
  };
  for (int round = 0; round < rounds; ++round) {
    if (round % 2 == 0) {
      native[round] = measure(runNative);
      standard[round] = measure(runStandard);
    } else {
      standard[round] = measure(runStandard);
      native[round] = measure(runNative);
    }
#ifdef GTI_COST_LLVM
    emitted[round] = measure([&] {
      require(gti_test_mutex_lock(storage.data(), &token) == 0);
      ++*gti_test_mutex_read_mut(storage.data(), &token);
      gti_test_mutex_unlock(storage.data(), &token);
    });
#endif
  }
  require(nativeValue == rounds * iterations && referenceValue == nativeValue);
  report("native C service", native);
  report("std::mutex", standard);
#ifdef GTI_COST_LLVM
  require(gti_test_mutex_lock(storage.data(), &token) == 0);
  require(*gti_test_mutex_read_mut(storage.data(), &token) == nativeValue);
  gti_test_mutex_unlock(storage.data(), &token);
  gti_test_mutex_destroy(storage.data());
  report("LLVM token/payload test adapter", emitted);
  alignas(8) std::array<unsigned char, 72> relocated{};
  Samples lifetime{};
  for (auto &sample : lifetime)
    sample = measure([&] {
      gti_test_mutex_initialize(storage.data(), 7);
      gti_test_mutex_transfer(relocated.data(), storage.data());
      gti_test_mutex_cleanup(storage.data());
      gti_test_mutex_cleanup(relocated.data());
    });
  report("LLVM passive shell lifecycle", lifetime,
         "initialize/fresh-shell-transfer/retire-both");
#endif
  gti_rt_mutex_destroy_v1(&slot);
}
