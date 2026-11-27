// A self-join reports an error and leaves the native worker joinable by its
// owner.
#include "native_thread.h"
#include "unit_test.h"

#include <cerrno>
#include <cstdio>
#include <latch>

namespace {
struct SelfJoin {
  gti::runtime::detail::NativeThread worker;
  std::latch started{1};
  std::latch attempted{1};
  int error = 0;
};

void *trySelfJoin(void *opaque) noexcept {
  auto &task = *static_cast<SelfJoin *>(opaque);
  task.started.wait();
  task.error = task.worker.join();
  task.attempted.count_down();
  return nullptr;
}
} // namespace

GTI_UNIT_TEST(native_thread_join_errors) {
  SelfJoin task;
  if (task.worker.start(trySelfJoin, &task) != 0) {
    std::fputs("could not start native worker\n", stderr);
    return 1;
  }
  task.started.count_down();
  task.attempted.wait();
  const bool retained = task.worker.joinable();
  if (task.worker.join() != 0 || !retained || task.worker.joinable()) {
    std::fputs("owner could not consume the worker after self-join\n", stderr);
    return 1;
  }
  const int emptyError = task.worker.join();
#if defined(_WIN32)
  return task.error == 0 || emptyError == 0;
#else
  return task.error != EDEADLK || emptyError != EINVAL;
#endif
}
