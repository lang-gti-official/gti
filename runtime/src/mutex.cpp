#include "gti/runtime_mutex.h"

#include <cstdlib>
#include <memory>
#include <new>
#include <pthread.h>
#include <type_traits>

#if !(defined(__APPLE__) && defined(__MACH__) && defined(__aarch64__)) &&      \
    !(defined(__linux__) && defined(__x86_64__))
#error GTI_MUTEX_UNREVIEWED_TARGET
#endif

namespace {
using Native = pthread_mutex_t;
static_assert(sizeof(gti_mutex_slot_v1) == 64);
static_assert(alignof(gti_mutex_slot_v1) == 8);
static_assert(sizeof(Native) <= sizeof(gti_mutex_slot_v1));
static_assert(alignof(Native) <= alignof(gti_mutex_slot_v1));
static_assert(std::is_trivially_destructible_v<Native>);

void require(bool condition) noexcept {
  if (!condition)
    std::abort();
}

Native *native(gti_mutex_slot_v1 *slot) noexcept {
  require(slot != nullptr);
  return std::launder(reinterpret_cast<Native *>(slot->bytes));
}
} // namespace

extern "C" void gti_rt_mutex_initialize_v1(gti_mutex_slot_v1 *slot) noexcept {
  require(slot != nullptr);
  auto *mutex = std::construct_at(reinterpret_cast<Native *>(slot->bytes));
  require(pthread_mutex_init(mutex, nullptr) == 0);
}

extern "C" void gti_rt_mutex_destroy_v1(gti_mutex_slot_v1 *slot) noexcept {
  auto *mutex = native(slot);
  require(pthread_mutex_destroy(mutex) == 0);
  std::destroy_at(mutex);
}

extern "C" uint32_t gti_rt_mutex_lock_v1(gti_mutex_slot_v1 *slot) noexcept {
  return pthread_mutex_lock(native(slot)) == 0 ? 0 : 1;
}

extern "C" void gti_rt_mutex_unlock_v1(gti_mutex_slot_v1 *slot) noexcept {
  require(pthread_mutex_unlock(native(slot)) == 0);
}
