#pragma once

#include <cerrno>
#include <exception>

#if defined(_WIN32)
#include <system_error>
#include <thread>
#else
#include <pthread.h>
#if defined(_LIBCPP_VERSION)
#include <memory>
#include <thread>
#endif
#endif

namespace gti::runtime::detail {

// The owner stays at a stable address from start through successful join.
// A failed create leaves it empty; a failed join preserves the obligation.
class NativeThread final {
public:
  using Entry = void *(*)(void *) noexcept;

  NativeThread() = default;
  NativeThread(const NativeThread &) = delete;
  NativeThread &operator=(const NativeThread &) = delete;
  ~NativeThread() {
    if (joinable())
      std::terminate();
  }

  // Native operations return zero or their error number. Setup can still
  // throw before a worker exists (for example, allocating libc++ exit state).
  [[nodiscard]] int start(Entry entry, void *context) {
    if (joinable())
      std::terminate();
#if defined(_WIN32)
    try {
      handle = std::thread(entry, context);
      return 0;
    } catch (const std::system_error &error) {
      return error.code().value();
    }
#else
#if defined(_LIBCPP_VERSION)
    // libc++ callbacks such as promise::set_value_at_thread_exit expect the
    // exit state normally installed by its std::thread launcher. Allocate on
    // the creator so allocation failure still precedes successful creation.
    exitState = std::make_unique<std::__thread_struct>();
    userEntry = entry;
    userContext = context;
    entry = enterLibcxx;
    context = this;
#endif
    const int error = pthread_create(&handle, nullptr, entry, context);
    started = error == 0;
    return error;
#endif
  }

  [[nodiscard]] bool joinable() const noexcept {
#if defined(_WIN32)
    return handle.joinable();
#else
    return started;
#endif
  }

  [[nodiscard]] int join() noexcept {
#if defined(_WIN32)
    try {
      handle.join();
      return 0;
    } catch (const std::system_error &error) {
      return error.code().value();
    } catch (...) {
      return EIO;
    }
#else
    if (!started)
      return EINVAL;
    if (pthread_equal(handle, pthread_self()))
      return EDEADLK;
    const int error = pthread_join(handle, nullptr);
    if (error == 0)
      started = false;
    return error;
#endif
  }

private:
#if defined(_WIN32)
  std::thread handle;
#else
  pthread_t handle{};
  bool started = false;
#if defined(_LIBCPP_VERSION)
  static void *enterLibcxx(void *opaque) noexcept {
    auto &worker = *static_cast<NativeThread *>(opaque);
    std::__thread_local_data().set_pointer(worker.exitState.release());
    return worker.userEntry(worker.userContext);
  }
  std::unique_ptr<std::__thread_struct> exitState;
  Entry userEntry = nullptr;
  void *userContext = nullptr;
#endif
#endif
};

} // namespace gti::runtime::detail
