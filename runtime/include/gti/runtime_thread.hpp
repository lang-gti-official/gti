#pragma once

#include "gti/runtime_thread.h"

#include <exception>
#include <utility>

namespace gti_runtime_detail {

// This exception never crosses the generated task adapter. It is only the
// in-process control transfer used after verified GTI cleanup has completed,
// or an explicit cleanup-emergency terminator has abandoned further cleanup,
// and the exact failure record has been copied into worker-owned state.
class worker_failure_handoff final {};

[[nodiscard]] bool managed_worker_active() noexcept;

// Compiler-generated per-thread owners embed this intrusive registration.
// The callback executes the exact verified MIR cleanup recipe and reports one
// ordinary failure through `failure`. A cleanup emergency may use the existing
// private worker handoff; it never escapes the runtime thread boundary.
struct thread_local_cleanup_entry {
  thread_local_cleanup_entry *previous = nullptr;
  void *storage = nullptr;
  bool (*cleanup)(
      void *, gti_failure_record_v2 *, const gti_failure_record_v2 *primary,
      const gti_failure_artifact_descriptor_v1 *primaryArtifact) = nullptr;
  const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
  bool registered = false;
};

void register_thread_local_cleanup(thread_local_cleanup_entry *entry) noexcept;
// Includes the completed shutdown state: new initialization is forbidden once
// thread exit starts, while an already-live slot may still be observed.
[[nodiscard]] bool thread_local_cleanup_active() noexcept;
void drain_thread_local_cleanup(const gti_failure_record_v2 *primary = nullptr,
                                const gti_failure_artifact_descriptor_v1
                                    *primaryArtifact = nullptr) noexcept;
void drain_thread_local_cleanup_legacy(
    const gti_failure_record_v1 *primary,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact) noexcept;

// This is not a pending-failure query. It is true only between an explicit
// cleanup-emergency handoff and its adapter catch on the same managed worker.
// Representation destructors must not execute abandoned GTI cleanup while
// the private handoff crosses their native stack frames.
[[nodiscard]] bool managed_cleanup_emergency_handoff_active() noexcept;
void clear_managed_failure_handoff() noexcept;

// Only the runtime-owned task cleanup phase exposes its pending failure.
// Generated cleanup uses it to stop immediately on a secondary failure.
[[nodiscard]] gti_thread_worker_failure_kind_v1
managed_task_cleanup_failure_kind() noexcept;

// Copy, never borrow, the pending primary into a generated cleanup frame.
// The descriptor is immutable and remains pinned by worker-owned state.
[[nodiscard]] bool copy_managed_task_cleanup_primary_v2(
    gti_failure_record_v2 &record,
    const gti_failure_artifact_descriptor_v1 *&artifact) noexcept;

[[noreturn]] void handoff_managed_worker_failure(
    const gti_failure_record_v1 &record,
    const gti_failure_artifact_descriptor_v1 *artifact);

[[noreturn]] void handoff_managed_worker_cleanup_failure(
    const gti_failure_emergency_v1 &failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact,
    const gti_failure_artifact_descriptor_v1 *secondaryArtifact);

[[noreturn]] void handoff_managed_worker_failure(
    const gti_failure_record_v2 &record,
    const gti_failure_artifact_descriptor_v1 *artifact);

[[noreturn]] void handoff_managed_worker_cleanup_failure(
    const gti_failure_emergency_v2 &failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact,
    const gti_failure_artifact_descriptor_v1 *secondaryArtifact);

// A generated GTI destructor is noexcept at the C++ representation boundary.
// While the runtime is destroying the transferred task object, this records a
// destructor failure in worker-owned state and lets native destruction finish
// without throwing through that noexcept boundary. It returns false outside
// that exact runtime cleanup phase.
[[nodiscard]] bool record_managed_task_cleanup_failure(
    const gti_failure_record_v1 &record,
    const gti_failure_artifact_descriptor_v1 *artifact) noexcept;

[[nodiscard]] bool record_managed_task_cleanup_emergency(
    const gti_failure_emergency_v1 *failure,
    const gti_failure_artifact_descriptor_v1 *primary_artifact,
    const gti_failure_artifact_descriptor_v1 *secondary_artifact) noexcept;

[[nodiscard]] bool record_managed_task_cleanup_failure(
    const gti_failure_record_v2 &record,
    const gti_failure_artifact_descriptor_v1 *artifact) noexcept;

[[nodiscard]] bool record_managed_task_cleanup_emergency(
    const gti_failure_emergency_v2 *failure,
    const gti_failure_artifact_descriptor_v1 *primary_artifact,
    const gti_failure_artifact_descriptor_v1 *secondary_artifact) noexcept;

// An automatic join has no caller-owned result channel. If the native join
// reports a recoverable failure while still owning a coherent state, generated
// code transfers that state here before propagating terminal GTI-R0012. The
// state remains pinned until process termination; this is not detach and is
// never used by explicit join.
void quarantine_managed_thread_state(
    gti_thread_state_v1 **state_address) noexcept;

template <typename Invocation>
void run_managed_task_adapter(Invocation &&invocation) noexcept {
  try {
    std::forward<Invocation>(invocation)();
  } catch (const worker_failure_handoff &) {
    clear_managed_failure_handoff();
    // The exact record was already handed to the worker state. Ordinary
    // failure proceeds to the task's explicit cleanup exactly once; cleanup
    // emergency leaves the remaining task/capture state abandoned. Generated
    // invocation closures capture references only, so clearing the marker
    // here cannot trigger delayed GTI-owned capture cleanup in this adapter.
  } catch (...) {
    // No foreign C++ unwind may cross the managed task boundary.
    std::terminate();
  }
}

} // namespace gti_runtime_detail
