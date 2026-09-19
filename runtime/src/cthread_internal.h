#pragma once

#include <cstdint>

namespace gti::runtime::detail {

// Source-private prerequisite for the C-style thread MIR boundary. These are
// not installed APIs and do not authorize concurrent GTI callback conversion.
using CThreadHandle = std::uint64_t;
using CThreadEntry = std::int32_t (*)(void *);

enum class CThreadStatus : std::int32_t {
  Success,
  Error,
  NoMemory,
  Busy,
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
