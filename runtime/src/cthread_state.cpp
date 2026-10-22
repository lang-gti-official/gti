#include "cthread_internal.h"
#include "gti/cthread.h"
#include "gti/runtime_thread.hpp"

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
  CThreadHandle handle = 0;
  CThreadEntry entry = nullptr;
  std::int32_t result = 0;
  bool joining = false;
};

struct Runtime {
  std::mutex mutex;
  std::atomic<RuntimePhase> phase{RuntimePhase::Uninitialized};
  std::thread::id owner;
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
struct OnceInvocation {
  COnceInitializer initializer;
  EntryPhase phase = EntryPhase::Registered;
  OnceInvocation *previous = nullptr;
};
thread_local OnceInvocation *onceInvocation = nullptr;
thread_local std::uint64_t managedDepth = 0;
thread_local std::uint64_t confinedDepth = 0;
thread_local bool initialRuntimeThread = false;
thread_local CThreadHandle currentHandle = 0;
std::atomic<CThreadHandle> nextHandle{1};
std::atomic<CThreadTestFault> testFault{CThreadTestFault::None};

// One identity space for initial, managed, foreign, and manual workers. Native
// thread IDs can be reused after exit and therefore cannot be the public ID.
// Relaxed ordering allocates identities only; it publishes no user memory.
CThreadHandle allocateHandle() noexcept {
  CThreadHandle candidate = nextHandle.load(std::memory_order_relaxed);
  while (candidate != 0) {
    const CThreadHandle next =
        candidate == std::numeric_limits<CThreadHandle>::max() ? 0
                                                               : candidate + 1;
    if (nextHandle.compare_exchange_weak(candidate, next,
                                         std::memory_order_relaxed)) {
      return candidate;
    }
  }
  return 0;
}

bool consumeFault(CThreadTestFault expected) noexcept {
  return testFault.compare_exchange_strong(expected, CThreadTestFault::None,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire);
}

void runWorker(Worker &worker, void *context) noexcept {
  if (attachment.worker != nullptr || currentHandle != 0) {
    std::terminate();
  }
  currentHandle = worker.handle;
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

void enterCThreadManagedScope() noexcept {
  if (managedDepth == std::numeric_limits<std::uint64_t>::max()) {
    std::terminate();
  }
  ++managedDepth;
}
void leaveCThreadManagedScope() noexcept {
  if (managedDepth == 0) {
    std::terminate();
  }
  --managedDepth;
}

bool terminalCThreadCallbackActive() noexcept {
  if (confinedDepth != 0)
    return true;
  if (attachment.phase == EntryPhase::Entered) {
    return true;
  }
  for (auto *invocation = onceInvocation; invocation != nullptr;
       invocation = invocation->previous) {
    if (invocation->phase == EntryPhase::Entered) {
      return true;
    }
  }
  return false;
}

void invokeCOnceInitializer(COnceInitializer initializer) noexcept {
  if (initializer == nullptr) {
    std::terminate();
  }
  OnceInvocation invocation{.initializer = initializer,
                            .previous = onceInvocation};
  onceInvocation = &invocation;
  try {
    initializer();
  } catch (...) {
    std::terminate();
  }
  if (invocation.phase == EntryPhase::Entered) {
    std::terminate();
  }
  onceInvocation = invocation.previous;
}

bool enterCOnceInitializer(COnceInitializer initializer) noexcept {
  if (onceInvocation == nullptr || initializer == nullptr ||
      onceInvocation->initializer != initializer ||
      onceInvocation->phase != EntryPhase::Registered) {
    return false;
  }
  Runtime &state = runtime();
  const std::lock_guard lock(state.mutex);
  if ((state.phase != RuntimePhase::Running &&
       state.phase != RuntimePhase::Stopping) ||
      (state.owner != std::this_thread::get_id() && managedDepth == 0 &&
       (attachment.worker == nullptr ||
        attachment.phase != EntryPhase::Entered))) {
    return false;
  }
  onceInvocation->phase = EntryPhase::Entered;
  return true;
}

bool leaveCOnceInitializer(COnceInitializer initializer) noexcept {
  if (onceInvocation == nullptr || initializer == nullptr ||
      onceInvocation->initializer != initializer ||
      onceInvocation->phase != EntryPhase::Entered) {
    return false;
  }
  onceInvocation->phase = EntryPhase::Left;
  return true;
}

CThreadHandle currentCThread() noexcept {
  if (currentHandle == 0) {
    currentHandle = allocateHandle();
    if (currentHandle == 0) {
      // Identity exhaustion cannot return a valid identity or revive an old
      // one. This terminal integrity boundary does not raise a GTI failure.
      std::terminate();
    }
  }
  return currentHandle;
}

CThreadStatus beginCThreadRuntime() noexcept {
  Runtime &state = runtime();
  const std::lock_guard lock(state.mutex);
  if (state.phase != RuntimePhase::Uninitialized) {
    return CThreadStatus::Error;
  }
  state.phase = RuntimePhase::Running;
  state.owner = std::this_thread::get_id();
  initialRuntimeThread = true;
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
  initialRuntimeThread = false;
  return CThreadStatus::Success;
}

CThreadStatus createCThread(CThreadHandle *output, CThreadEntry entry,
                            void *context) noexcept {
  if (output == nullptr || entry == nullptr) {
    return CThreadStatus::Error;
  }
  Runtime &state = runtime();
  const std::lock_guard lock(state.mutex);
  if (state.phase != RuntimePhase::Running) {
    return CThreadStatus::Error;
  }
  const CThreadHandle handle = allocateHandle();
  if (handle == 0) {
    return CThreadStatus::Error;
  }
  try {
    if (consumeFault(CThreadTestFault::CreateNoMemory)) {
      throw std::bad_alloc{};
    }
    auto worker = std::make_unique<Worker>();
    worker->handle = handle;
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
  gti_runtime_detail::drain_thread_local_cleanup();
  attachment.phase = EntryPhase::Left;
  return true;
}

void setCThreadTestFault(CThreadTestFault fault) noexcept {
  testFault.store(fault, std::memory_order_release);
}

CThreadHandle confinedCallbackThread() noexcept {
  const auto phase = runtime().phase.load(std::memory_order_relaxed);
  if ((phase != RuntimePhase::Running && phase != RuntimePhase::Stopping) ||
      (!initialRuntimeThread && managedDepth == 0 &&
       (attachment.worker == nullptr ||
        attachment.phase != EntryPhase::Entered))) {
    std::terminate();
  }
  return currentCThread();
}

void enterConfinedCallback(CThreadHandle owner) noexcept {
  if (owner == 0 || owner != confinedCallbackThread() ||
      confinedDepth == std::numeric_limits<std::uint64_t>::max()) {
    std::terminate();
  }
  ++confinedDepth;
}

void leaveConfinedCallback() noexcept {
  if (confinedDepth == 0)
    std::terminate();
  --confinedDepth;
}

} // namespace gti::runtime::detail

extern "C" uint64_t gti_rt_confined_callback_thread_v1(void) {
  return gti::runtime::detail::confinedCallbackThread();
}
extern "C" void gti_rt_confined_callback_enter_v1(uint64_t owner) {
  gti::runtime::detail::enterConfinedCallback(owner);
}
extern "C" void gti_rt_confined_callback_leave_v1(void) {
  gti::runtime::detail::leaveConfinedCallback();
}

extern "C" uint64_t gti_rt_cthread_current_v1(void) {
  return gti::runtime::detail::currentCThread();
}

extern "C" void gti_rt_once_invoke_v1(gti_once_initializer_v1 initializer) {
  gti::runtime::detail::invokeCOnceInitializer(initializer);
}
extern "C" void gti_rt_once_enter_v1(gti_once_initializer_v1 initializer) {
  if (!gti::runtime::detail::enterCOnceInitializer(initializer)) {
    std::terminate();
  }
}
extern "C" void gti_rt_once_leave_v1(gti_once_initializer_v1 initializer) {
  if (!gti::runtime::detail::leaveCOnceInitializer(initializer)) {
    std::terminate();
  }
}

extern "C" int32_t gti_rt_cthread_create_v1(uint64_t *output,
                                            gti_cthread_entry_v1 entry,
                                            void *context) {
  return static_cast<int32_t>(
      gti::runtime::detail::createCThread(output, entry, context));
}

extern "C" int32_t gti_rt_cthread_join_v1(uint64_t handle, int32_t *result) {
  return static_cast<int32_t>(
      gti::runtime::detail::joinCThread(handle, result));
}

extern "C" void gti_rt_cthread_begin_v1(void) {
  if (gti::runtime::detail::beginCThreadRuntime() !=
      gti::runtime::detail::CThreadStatus::Success) {
    std::terminate();
  }
}

extern "C" void gti_rt_cthread_end_v1(void) {
  if (gti::runtime::detail::shutdownCThreadRuntime() !=
      gti::runtime::detail::CThreadStatus::Success) {
    std::terminate();
  }
}

extern "C" void gti_rt_cthread_enter_v1(gti_cthread_entry_v1 entry) {
  if (!gti::runtime::detail::enterCThreadCallback(entry)) {
    std::terminate();
  }
}

extern "C" void gti_rt_cthread_leave_v1(gti_cthread_entry_v1 entry) {
  if (!gti::runtime::detail::leaveCThreadCallback(entry)) {
    std::terminate();
  }
}
