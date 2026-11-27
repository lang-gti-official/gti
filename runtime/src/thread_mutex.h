#pragma once

#if defined(_WIN32)
#include <mutex>
#else
#include <exception>
#include <pthread.h>
#include <system_error>
#endif

namespace gti::runtime::detail {

#if defined(_WIN32)
using ThreadMutex = std::mutex;
using StaticThreadMutex = std::mutex;
#else
// BasicLockable for the thread runtime's own bookkeeping. A live pthread
// mutex has a stable address until destruction.
class ThreadMutex {
public:
  ThreadMutex() {
    if (const int error = pthread_mutex_init(&mutex, nullptr))
      throwError(error);
  }
  ThreadMutex(const ThreadMutex &) = delete;
  ThreadMutex &operator=(const ThreadMutex &) = delete;
  ~ThreadMutex() {
    if (pthread_mutex_destroy(&mutex) != 0)
      std::terminate();
  }

  void lock() {
    if (const int error = pthread_mutex_lock(&mutex))
      throwError(error);
  }
  void unlock() noexcept {
    if (pthread_mutex_unlock(&mutex) != 0)
      std::terminate();
  }

protected:
  struct StaticStorage {};
  constexpr explicit ThreadMutex(StaticStorage) noexcept
      : mutex PTHREAD_MUTEX_INITIALIZER {}

private:
#if defined(__GNUC__)
  [[gnu::cold, gnu::noinline]]
#endif
  [[noreturn]] static void throwError(int error) {
    throw std::system_error(error, std::generic_category());
  }

  pthread_mutex_t mutex;
};

// The registry and quarantine locks have static storage. In particular,
// retaining a worker after a failed join must not need a fallible lock setup.
class StaticThreadMutex final : public ThreadMutex {
public:
  constexpr StaticThreadMutex() noexcept : ThreadMutex(StaticStorage{}) {}
};
#endif

} // namespace gti::runtime::detail
