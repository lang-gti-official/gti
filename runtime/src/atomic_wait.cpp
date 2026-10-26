#include "gti/runtime_atomic_wait.h"

#include "atomic_lane.h"

#include <array>
#include <pthread.h>

namespace {
using gti::runtime::atomic_lane::require;

struct Bucket {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
};

// Trivial process-lifetime storage: late TLS/static cleanup may be its first
// user. A failed initialization terminates before publishing a partial domain.
constinit std::array<Bucket, 256> buckets{};
constinit pthread_once_t initializeOnce = PTHREAD_ONCE_INIT;

void initialize() noexcept {
  for (auto &bucket : buckets) {
    require(pthread_mutex_init(&bucket.mutex, nullptr) == 0);
    require(pthread_cond_init(&bucket.condition, nullptr) == 0);
  }
}

Bucket &bucketFor(const void *address, std::uint32_t width) noexcept {
  require(pthread_once(&initializeOnce, initialize) == 0);
  return buckets[((reinterpret_cast<std::uintptr_t>(address) >> 3) ^ width) %
                 buckets.size()];
}

template <typename T>
void wait(const void *lane, std::uint64_t old, std::uint32_t order) noexcept {
  require(old <= std::numeric_limits<T>::max());
  if (gti::runtime::atomic_lane::load<T>(lane, order) != static_cast<T>(old))
    return;
  auto &bucket = bucketFor(lane, sizeof(T));
  require(pthread_mutex_lock(&bucket.mutex) == 0);
  while (gti::runtime::atomic_lane::load<T>(lane, order) == static_cast<T>(old))
    require(pthread_cond_wait(&bucket.condition, &bucket.mutex) == 0);
  require(pthread_mutex_unlock(&bucket.mutex) == 0);
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
  require(pthread_mutex_lock(&bucket.mutex) == 0);
  // A single native wake could choose a different colliding lane and strand
  // an eligible waiter. Broadcast preserves both source notification contracts.
  require(pthread_cond_broadcast(&bucket.condition) == 0);
  require(pthread_mutex_unlock(&bucket.mutex) == 0);
}
