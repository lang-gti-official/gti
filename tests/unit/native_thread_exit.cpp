#include "gti/cthread.h"
#include "gti/runtime_thread.h"
#include "unit_test.h"

#include <chrono>
#include <cstdio>
#include <future>

namespace {
struct Publication {
  std::promise<int> promise;
  std::future<int> result = promise.get_future();
  bool destroyed = false;

  bool readyAfterJoin() {
    return destroyed &&
           result.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready &&
           result.get() == 17;
  }
};

struct ExitMarker {
  bool &destroyed;
  ~ExitMarker() { destroyed = true; }
};

void publish(void *opaque) {
  auto &publication = *static_cast<Publication *>(opaque);
  thread_local ExitMarker marker{publication.destroyed};
  publication.promise.set_value_at_thread_exit(17);
}

int32_t manualEntry(void *context) {
  publish(context);
  return 42;
}
} // namespace

GTI_UNIT_TEST(native_thread_exit_publication) {
  Publication scoped;
  gti_thread_state_v1 *scratch = nullptr;
  const gti_thread_scoped_task_v1 task{publish, &scoped};
  const auto scope = gti_rt_thread_scope_v1(&task, &scratch, 1);
  if (scope.spawn_status != GTI_THREAD_SPAWN_OK_V1 || scope.started != 1 ||
      scope.worker.status != GTI_THREAD_JOIN_OK_V1 ||
      !scoped.readyAfterJoin()) {
    std::fputs("scoped join missed native exit publication\n", stderr);
    return 1;
  }

  Publication manual;
  uint64_t handle = 0;
  int32_t result = 0;
  gti_rt_cthread_begin_v1();
  if (gti_rt_cthread_create_v1(&handle, manualEntry, &manual) != 0 ||
      gti_rt_cthread_join_v1(handle, &result) != 0 || result != 42 ||
      !manual.readyAfterJoin()) {
    std::fputs("manual join missed native exit publication\n", stderr);
    return 1;
  }
  gti_rt_cthread_end_v1();
  return 0;
}
