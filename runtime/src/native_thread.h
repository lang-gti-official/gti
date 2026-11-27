#pragma once

#include <exception>
#include <system_error>

#if defined(_WIN32)
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

  void start(Entry entry, void *context) {
    if (joinable())
      std::terminate();
#if defined(_WIN32)
    handle = std::thread(entry, context);
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
    if (error != 0)
      throw std::system_error(error, std::generic_category(), "pthread_create");
    started = true;
#endif
  }

  [[nodiscard]] bool joinable() const noexcept {
#if defined(_WIN32)
    return handle.joinable();
#else
    return started;
#endif
  }

  void join() {
#if defined(_WIN32)
    handle.join();
#else
    if (!started)
      throw std::system_error(
          std::make_error_code(std::errc::invalid_argument));
    if (pthread_equal(handle, pthread_self()))
      throw std::system_error(
          std::make_error_code(std::errc::resource_deadlock_would_occur));
    const int error = pthread_join(handle, nullptr);
    if (error != 0)
      throw std::system_error(error, std::generic_category(), "pthread_join");
    started = false;
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
