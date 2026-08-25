#include "gti/runtime_thread.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

struct gti_thread_state_v1 {
  std::thread worker;
  std::mutex failureMutex;
  bool workerFailed = false;
  gti_failure_record_v1 failure{};
  const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
};

namespace {

thread_local gti_thread_state_v1 *activeWorker = nullptr;

class ActiveWorkerScope final {
public:
  explicit ActiveWorkerScope(gti_thread_state_v1 &state) noexcept
      : previous(activeWorker) {
    activeWorker = &state;
  }

  ActiveWorkerScope(const ActiveWorkerScope &) = delete;
  ActiveWorkerScope &operator=(const ActiveWorkerScope &) = delete;

  ~ActiveWorkerScope() { activeWorker = previous; }

private:
  gti_thread_state_v1 *previous;
};

[[nodiscard]] gti_thread_spawn_status_v1
spawnStatus(const std::system_error &error) noexcept {
  const int value = error.code().value();
  return value == EPERM ? GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1
                        : GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
}

} // namespace

namespace gti_runtime_detail {

bool managed_worker_active() noexcept { return activeWorker != nullptr; }

[[noreturn]] void handoff_managed_worker_failure(
    const gti_failure_record_v1 &record,
    const gti_failure_artifact_descriptor_v1 *artifact) {
  gti_thread_state_v1 *state = activeWorker;
  if (state == nullptr) {
    std::terminate();
  }
  {
    const std::lock_guard lock(state->failureMutex);
    if (state->workerFailed) {
      // A worker has exactly one terminal outcome. Re-entering failure handoff
      // indicates corrupt generated containment.
      std::terminate();
    }
    state->failure = record;
    state->artifact = artifact;
    state->workerFailed = true;
  }
  throw worker_failure_handoff{};
}

} // namespace gti_runtime_detail

extern "C" gti_thread_spawn_result_v1
gti_rt_thread_spawn_v1(gti_thread_task_entry_v1 entry, void *task_context,
                       gti_thread_task_destroy_v1 destroy) {
  gti_thread_spawn_result_v1 result{.abi_version = GTI_THREAD_ABI_VERSION_V1,
                                    .status = GTI_THREAD_SPAWN_OK_V1,
                                    .state = nullptr};
  if (entry == nullptr || destroy == nullptr || task_context == nullptr) {
    if (destroy != nullptr && task_context != nullptr) {
      destroy(task_context);
    }
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    return result;
  }

  gti_thread_state_v1 *state = new (std::nothrow) gti_thread_state_v1;
  if (state == nullptr) {
    destroy(task_context);
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    return result;
  }

  try {
    state->worker = std::thread([state, entry, task_context, destroy] {
      ActiveWorkerScope scope(*state);
      try {
        entry(task_context);
      } catch (const gti_runtime_detail::worker_failure_handoff &) {
        // Generated adapters catch this. Retain a defensive runtime boundary
        // so a malformed adapter still cannot leak an exception to std::thread.
      } catch (...) {
        std::terminate();
      }
      destroy(task_context);
    });
  } catch (const std::system_error &error) {
    destroy(task_context);
    result.status = spawnStatus(error);
    delete state;
    return result;
  } catch (...) {
    destroy(task_context);
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    delete state;
    return result;
  }

  result.state = state;
  return result;
}

extern "C" gti_thread_join_result_v1
gti_rt_thread_join_v1(gti_thread_state_v1 **stateAddress) {
  gti_thread_join_result_v1 result{.abi_version = GTI_THREAD_ABI_VERSION_V1,
                                   .status = GTI_THREAD_JOIN_OK_V1,
                                   .failure = {}};
  if (stateAddress == nullptr || *stateAddress == nullptr) {
    result.status = GTI_THREAD_JOIN_NOT_JOINABLE_V1;
    return result;
  }

  gti_thread_state_v1 *state = *stateAddress;
  if (!state->worker.joinable()) {
    result.status = GTI_THREAD_JOIN_NOT_JOINABLE_V1;
    return result;
  }
  try {
    state->worker.join();
  } catch (...) {
    result.status = GTI_THREAD_JOIN_FAILED_V1;
    return result;
  }

  {
    const std::lock_guard lock(state->failureMutex);
    if (state->workerFailed) {
      result.status = GTI_THREAD_JOIN_WORKER_FAILURE_V1;
      result.failure = state->failure;
    }
  }
  *stateAddress = nullptr;
  delete state;
  return result;
}
