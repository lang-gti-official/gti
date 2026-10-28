#include "gti/runtime_atomic_wait.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned iterations = 500000;

template <typename Operation>
void measure(const char *name, Operation operation) {
  std::array<double, 9> samples{};
  for (auto &sample : samples) {
    const auto start = Clock::now();
    for (unsigned index = 0; index < iterations; ++index)
      operation();
    sample =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count() /
        iterations;
  }
  std::sort(samples.begin(), samples.end());
  std::cout << name << " median_ns=" << samples[4]
            << " min_ns=" << samples.front() << " max_ns=" << samples.back()
            << " iterations=" << iterations << " samples=" << samples.size()
            << '\n';
}

template <typename Wait, typename Publish>
void roundTrips(const char *name, Wait wait, Publish publish) {
  constexpr unsigned rounds = 5000;
  std::array<double, 5> samples{};
  for (auto &sample : samples) {
    const auto start = Clock::now();
    std::thread consumer([&] {
      for (unsigned index = 0; index < rounds; ++index) {
        wait(0);
        publish(0);
      }
    });
    for (unsigned index = 0; index < rounds; ++index) {
      publish(1);
      wait(1);
    }
    consumer.join();
    sample =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count() /
        rounds;
  }
  std::sort(samples.begin(), samples.end());
  std::cout << name << " median_ns=" << samples[2]
            << " min_ns=" << samples.front() << " max_ns=" << samples.back()
            << " roundtrips=" << rounds << " samples=" << samples.size()
            << '\n';
}
} // namespace

int main() {
  alignas(8) std::uint64_t lane = 1;
  std::atomic<std::uint64_t> standard{1};
  const auto start = Clock::now();
  gti_rt_atomic_notify_v1(&lane, 8, 1);
  std::cout
      << "native_notify_cold_ns="
      << std::chrono::duration<double, std::nano>(Clock::now() - start).count()
      << '\n';
  measure("native_ready_acquire",
          [&] { gti_rt_atomic_wait_v1(&lane, 0, 8, 1); });
  measure("standard_ready_acquire",
          [&] { standard.wait(0, std::memory_order_acquire); });
  measure("native_notify_one_empty",
          [&] { gti_rt_atomic_notify_v1(&lane, 8, 0); });
  measure("standard_notify_one_empty", [&] { standard.notify_one(); });
  measure("native_notify_all_empty",
          [&] { gti_rt_atomic_notify_v1(&lane, 8, 1); });
  measure("standard_notify_all_empty", [&] { standard.notify_all(); });
  lane = 0;
  standard.store(0, std::memory_order_relaxed);
  roundTrips(
      "native_roundtrip",
      [&](std::uint64_t old) { gti_rt_atomic_wait_v1(&lane, old, 8, 1); },
      [&](std::uint64_t value) {
        __atomic_store_n(&lane, value, __ATOMIC_RELEASE);
        gti_rt_atomic_notify_v1(&lane, 8, 0);
      });
  roundTrips(
      "standard_roundtrip",
      [&](std::uint64_t old) { standard.wait(old, std::memory_order_acquire); },
      [&](std::uint64_t value) {
        standard.store(value, std::memory_order_release);
        standard.notify_one();
      });
}
