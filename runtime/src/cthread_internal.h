#pragma once

#include <cstdint>

namespace gti::runtime::detail {

// Source-private implementation behind the versioned C-thread ABI. This header
// is not installed; compiler-owned MIR policy authorizes GTI callback entry.
using CThreadHandle = std::uint64_t;
using CThreadEntry = std::int32_t (*)(void *);
using COnceInitializer = void (*)();

void invokeCOnceInitializer(COnceInitializer initializer) noexcept;
bool enterCOnceInitializer(COnceInitializer initializer) noexcept;
bool leaveCOnceInitializer(COnceInitializer initializer) noexcept;
// Balanced by the existing managed-worker runtime scope, never by user code.
void enterCThreadManagedScope() noexcept;
void leaveCThreadManagedScope() noexcept;
// An entered native adapter owns a terminating failure boundary even when
// synchronously invoked by a managed task. Do not hand its failure to join.
bool terminalCThreadCallbackActive() noexcept;

// Stable, nonzero process identity, including outside the manual service.
// Observing an identity grants neither callback attachment nor join authority.
CThreadHandle currentCThread() noexcept;

enum class CThreadStatus : std::int32_t {
  Success = 0,
  Error = 1,
  NoMemory = 2,
  Busy = 3,
  TimedOut = 4,
};

// One hosted invocation owns this process-wide service. Begin succeeds once;
// only that initial thread can shut it down.
// Shutdown closes creation immediately; Busy preserves outstanding handles so
// their owners can join and retry shutdown. Success means no worker remains.
CThreadStatus beginCThreadRuntime() noexcept;
CThreadStatus shutdownCThreadRuntime() noexcept;

// Context ownership never transfers. Its lifetime, synchronization, and any
// pointed-to objects remain the unsafe caller's obligation through join.
// Creation failure preserves the output. Success publishes a nonzero,
// never-reused identity. A successful join consumes that identity once and
// publishes the worker's integer result (or discards it for a null output).
CThreadStatus createCThread(CThreadHandle *output, CThreadEntry entry,
                            void *context) noexcept;
CThreadStatus joinCThread(CThreadHandle handle, std::int32_t *result) noexcept;

// The generated adapter must claim its exact registered function on the new
// worker before entering GTI, then leave after its verified cleanup. Initial,
// unmanaged, mismatched, recursive, and repeated claims fail closed. A worker
// registered before shutdown may complete while creation is closed.
bool enterCThreadCallback(CThreadEntry entry) noexcept;
bool leaveCThreadCallback(CThreadEntry entry) noexcept;

enum class CThreadTestFault {
  None,
  CreateNoMemory,
  CreateResourceUnavailable,
  JoinFailed,
};

void setCThreadTestFault(CThreadTestFault fault) noexcept;

} // namespace gti::runtime::detail
