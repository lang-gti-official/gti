#include "gti/runtime_atomic_wait.h"

#include "atomic_lane.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <new>
#include <pthread.h>

extern "C" {
constinit gti_atomic_wait_bucket_v1
    gti_rt_atomic_wait_domain_v1[GTI_ATOMIC_WAIT_BUCKET_COUNT_V1]{};
}

namespace {
using gti::runtime::atomic_lane::require;

using Bucket = gti_atomic_wait_bucket_v1;
struct Parking {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
};
static_assert(sizeof(Bucket) == GTI_ATOMIC_WAIT_BUCKET_BYTES_V1);
static_assert(alignof(Bucket) == GTI_ATOMIC_WAIT_BUCKET_ALIGNMENT_V1);
static_assert(offsetof(Bucket, wake_needed) == 0 &&
              offsetof(Bucket, waiters) == 4);
static_assert(offsetof(Bucket, native_storage) == 8);
static_assert(sizeof(Parking) <= sizeof(Bucket::native_storage));
static_assert(offsetof(Bucket, native_storage) % alignof(Parking) == 0);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint32_t>::required_alignment == 4);

Parking &parking(Bucket &bucket) noexcept {
  return *std::launder(reinterpret_cast<Parking *>(bucket.native_storage));
}

auto waiters(Bucket &bucket) noexcept {
  return std::atomic_ref<std::uint32_t>(bucket.waiters);
}
#if defined(__x86_64__)
auto wakeNeeded(Bucket &bucket) noexcept {
  return std::atomic_ref<std::uint32_t>(bucket.wake_needed);
}
#endif

// Trivial process-lifetime storage: late TLS/static cleanup may be its first
// user. A failed initialization terminates before publishing a partial domain.
constexpr auto &buckets = gti_rt_atomic_wait_domain_v1;
constinit pthread_once_t initializeOnce = PTHREAD_ONCE_INIT;
constinit std::atomic<bool> initialized{false};

void initialize() noexcept {
  for (auto &bucket : buckets) {
    auto *native = ::new (static_cast<void *>(bucket.native_storage)) Parking{};
    require(pthread_mutex_init(&native->mutex, nullptr) == 0);
    require(pthread_cond_init(&native->condition, nullptr) == 0);
  }
  initialized.store(true, std::memory_order_release);
}

Bucket &bucketFor(const void *address, std::uint32_t width) noexcept {
  return buckets[((reinterpret_cast<std::uintptr_t>(address) >> 3) ^ width) %
                 GTI_ATOMIC_WAIT_BUCKET_COUNT_V1];
}

void ensureInitialized() noexcept {
  if (!initialized.load(std::memory_order_acquire))
    require(pthread_once(&initializeOnce, initialize) == 0);
}

#if defined(__GNUC__)
[[gnu::cold, gnu::noinline]]
#endif
void notifyRegistered(Bucket &bucket) noexcept {
  ensureInitialized();
  auto &native = parking(bucket);
  require(pthread_mutex_lock(&native.mutex) == 0);
  // Colliding lanes require broadcast even for source notify-one.
  require(pthread_cond_broadcast(&native.condition) == 0);
#if defined(__x86_64__)
  // Rearm under the check-to-sleep mutex, retaining coalesced publications.
  if (waiters(bucket).load(std::memory_order_relaxed) != 0)
    wakeNeeded(bucket).fetch_or(1U, std::memory_order_acq_rel);
  else
    wakeNeeded(bucket).fetch_and(~1U, std::memory_order_acq_rel);
#endif
  require(pthread_mutex_unlock(&native.mutex) == 0);
}

template <typename T>
void wait(const void *lane, std::uint64_t old, std::uint32_t order) noexcept {
  require(old <= std::numeric_limits<T>::max());
  if (gti::runtime::atomic_lane::load<T>(lane, order) != static_cast<T>(old))
    return;
  // Short handoffs often finish before entering the kernel. Bound this work:
  // an unchanged lane always proceeds to the native parking protocol below.
  using Clock = std::chrono::steady_clock;
  Clock::time_point deadline;
  for (unsigned probe = 0; probe != 8192; ++probe) {
    // Avoid clock reads on short handoffs. Slow hints/loaded hosts must not
    // turn the finite instruction budget into a long active wait.
    if (probe == 64)
      deadline = Clock::now() + std::chrono::microseconds(50);
    else if (probe > 64 && probe % 64 == 0 && Clock::now() >= deadline)
      break;
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
    if (gti::runtime::atomic_lane::load<T>(lane, order) != static_cast<T>(old))
      return;
  }
  auto &bucket = bucketFor(lane, sizeof(T));
  ensureInitialized();
  auto &native = parking(bucket);
  require(pthread_mutex_lock(&native.mutex) == 0);
  require(waiters(bucket).fetch_add(1, std::memory_order_relaxed) !=
          std::numeric_limits<unsigned>::max());
#if defined(__x86_64__)
  // Every update is an acquire/release RMW. If notification precedes this
  // enrollment, its preceding lane publication happens before our recheck.
  // Otherwise notification sees this enrollment and must take the mutex.
  wakeNeeded(bucket).fetch_or(1U, std::memory_order_acq_rel);
#else
  // Together with notify's fence, registration and the publishing lane store
  // cannot both be missed: whichever fence comes second observes the other's
  // preceding atomic write. The mutex then closes the check-to-sleep window.
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
  while (gti::runtime::atomic_lane::load<T>(lane, order) == static_cast<T>(old))
    require(pthread_cond_wait(&native.condition, &native.mutex) == 0);
  const auto previous = waiters(bucket).fetch_sub(1, std::memory_order_relaxed);
#if defined(__x86_64__)
  if (previous == 1)
    wakeNeeded(bucket).fetch_and(~1U, std::memory_order_acq_rel);
#else
  static_cast<void>(previous);
#endif
  require(pthread_mutex_unlock(&native.mutex) == 0);
}
} // namespace

extern "C" void gti_rt_atomic_wait_v1(const void *lane, std::uint64_t old_bits,
                                      std::uint32_t width_bytes,
                                      std::uint32_t load_order) noexcept {
  gti::runtime::atomic_lane::validate(lane, width_bytes);
  require(load_order <= 2);
  switch (width_bytes) {
  case 1:
    wait<std::uint8_t>(lane, old_bits, load_order);
    return;
  case 2:
    wait<std::uint16_t>(lane, old_bits, load_order);
    return;
  case 4:
    wait<std::uint32_t>(lane, old_bits, load_order);
    return;
  case 8:
    wait<std::uint64_t>(lane, old_bits, load_order);
    return;
  default:
    std::abort();
  }
}

extern "C" void gti_rt_atomic_notify_v1(const void *lane,
                                        std::uint32_t width_bytes,
                                        std::uint32_t all) noexcept {
  gti::runtime::atomic_lane::validate(lane, width_bytes);
  require(all <= 1);
  auto &bucket = bucketFor(lane, width_bytes);
#if defined(__x86_64__)
  if ((wakeNeeded(bucket).fetch_and(~1U, std::memory_order_acq_rel) & 1U) == 0)
    return;
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (waiters(bucket).load(std::memory_order_relaxed) == 0)
    return;
#endif
  notifyRegistered(bucket);
}

extern "C" void gti_rt_atomic_notify_registered_v1(const void *lane,
                                                   std::uint32_t width_bytes,
                                                   std::uint32_t all) noexcept {
  gti::runtime::atomic_lane::validate(lane, width_bytes);
  require(all <= 1);
  notifyRegistered(bucketFor(lane, width_bytes));
}
