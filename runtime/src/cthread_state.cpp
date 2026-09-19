#include "cthread_internal.h"

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace gti::runtime::detail {
namespace {

enum class RuntimePhase { Uninitialized, Running, Stopping, Stopped };
enum class EntryPhase { Registered, Entered, Left };

struct Worker {
  std::thread thread;
  CThreadEntry entry = nullptr;
  std::int32_t result = 0;
  bool joining = false;
};

struct Runtime {
  std::mutex mutex;
  RuntimePhase phase = RuntimePhase::Uninitialized;
  std::thread::id owner;
  CThreadHandle nextHandle = 1;
  std::unordered_map<CThreadHandle, std::unique_ptr<Worker>> workers;
};

Runtime &runtime() {
  static Runtime state;
  return state;
}

struct Attachment {
  Worker *worker = nullptr;
  EntryPhase phase = EntryPhase::Registered;
};

thread_local Attachment attachment;
std::atomic<CThreadTestFault> testFault{CThreadTestFault::None};

bool consumeFault(CThreadTestFault expected) noexcept {
  return testFault.compare_exchange_strong(expected, CThreadTestFault::None,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

void runWorker(Worker &worker, void *context) noexcept {
  if (attachment.worker != nullptr) {
    std::terminate();
  }
  attachment = {.worker = &worker};
  try {
    worker.result = worker.entry(context);
  } catch (...) {
    // This is an integrity firewall, not GTI failure transport. The generated
    // adapter must contain defined failures after its MIR cleanup completes.
    std::terminate();
  }
  if (attachment.phase == EntryPhase::Entered) {
    // A GTI adapter that entered but did not complete its boundary cannot
    // publish a successful native result.
    std::terminate();
  }
  attachment = {};
}

} // namespace

CThreadStatus beginCThreadRuntime() noexcept {
  Runtime &state = runtime();
  const std::lock_guard lock(state.mutex);
  if (state.phase != RuntimePhase::Uninitialized) {
    return CThreadStatus::Error;
  }
  state.phase = RuntimePhase::Running;
  state.owner = std::this_thread::get_id();
  return CThreadStatus::Success;
}

CThreadStatus shutdownCThreadRuntime() noexcept {
  Runtime &state = runtime();
  const std::lock_guard lock(state.mutex);
  if (state.phase == RuntimePhase::Uninitialized ||
      state.phase == RuntimePhase::Stopped ||
      state.owner != std::this_thread::get_id()) {
    return CThreadStatus::Error;
  }
  state.phase = RuntimePhase::Stopping;
  if (!state.workers.empty()) {
    return CThreadStatus::Busy;
  }
  state.phase = RuntimePhase::Stopped;
  return CThreadStatus::Success;
}

CThreadStatus createCThread(CThreadHandle *output, CThreadEntry entry,
                            void *context) noexcept {
  if (output == nullptr || entry == nullptr) {
    return CThreadStatus::Error;
  }
  Runtime &state = runtime();
  const std::lock_guard lock(state.mutex);
  if (state.phase != RuntimePhase::Running || state.nextHandle == 0) {
    return CThreadStatus::Error;
  }
  const CThreadHandle handle = state.nextHandle;
  // Zero permanently closes the identity space instead of reviving a stale
  // copied handle after unsigned wraparound.
  state.nextHandle =
      handle == std::numeric_limits<CThreadHandle>::max() ? 0 : handle + 1;
  try {
    if (consumeFault(CThreadTestFault::CreateNoMemory)) {
      throw std::bad_alloc{};
    }
    auto worker = std::make_unique<Worker>();
    worker->entry = entry;
    Worker *published = worker.get();
    state.workers.emplace(handle, std::move(worker));
    if (consumeFault(CThreadTestFault::CreateResourceUnavailable)) {
      throw std::system_error(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    // Publish in the registry before starting the native thread. No operation
    // that can throw may follow successful native creation.
    published->thread =
        std::thread([published, context] { runWorker(*published, context); });
  } catch (const std::bad_alloc &) {
    state.workers.erase(handle);
    return CThreadStatus::NoMemory;
  } catch (...) {
    state.workers.erase(handle);
    return CThreadStatus::Error;
  }
  *output = handle;
  return CThreadStatus::Success;
}

CThreadStatus joinCThread(CThreadHandle handle, std::int32_t *result) noexcept {
  Runtime &state = runtime();
  Worker *worker = nullptr;
  {
    const std::lock_guard lock(state.mutex);
    const auto found = state.workers.find(handle);
    if (found == state.workers.end()) {
      return CThreadStatus::Error;
    }
    worker = found->second.get();
    if (worker->joining) {
      return CThreadStatus::Busy;
    }
    if (worker->thread.get_id() == std::this_thread::get_id()) {
      return CThreadStatus::Error;
    }
    worker->joining = true;
  }

  try {
    if (consumeFault(CThreadTestFault::JoinFailed)) {
      throw std::system_error(
          std::make_error_code(std::errc::resource_deadlock_would_occur));
    }
    worker->thread.join();
  } catch (...) {
    const std::lock_guard lock(state.mutex);
    if (!worker->thread.joinable()) {
      // Native failure destroyed the only coherent join obligation.
      std::terminate();
    }
    worker->joining = false;
    return CThreadStatus::Error;
  }

  const std::lock_guard lock(state.mutex);
  if (result != nullptr) {
    *result = worker->result;
  }
  state.workers.erase(handle);
  return CThreadStatus::Success;
}

bool enterCThreadCallback(CThreadEntry entry) noexcept {
  if (attachment.worker == nullptr || entry == nullptr ||
      attachment.worker->entry != entry ||
      attachment.phase != EntryPhase::Registered) {
    return false;
  }
  Runtime &state = runtime();
  const std::lock_guard lock(state.mutex);
  if (state.phase != RuntimePhase::Running &&
      state.phase != RuntimePhase::Stopping) {
    return false;
  }
  attachment.phase = EntryPhase::Entered;
  return true;
}

bool leaveCThreadCallback(CThreadEntry entry) noexcept {
  if (attachment.worker == nullptr || entry == nullptr ||
      attachment.worker->entry != entry ||
      attachment.phase != EntryPhase::Entered) {
    return false;
  }
  attachment.phase = EntryPhase::Left;
  return true;
}

void setCThreadTestFault(CThreadTestFault fault) noexcept {
  testFault.store(fault, std::memory_order_release);
}

} // namespace gti::runtime::detail
