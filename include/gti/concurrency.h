#pragma once

#include <cstdint>
#include <optional>

namespace lang {

// Backend-independent synchronization identities. Public library types such
// as std::jthread remain ordinary GTI classes; trusted private capabilities
// attach these records to their resolved calls before MIR lowering.
enum class SynchronizationOperationKind : std::uint8_t {
  None,
  ThreadSpawn,
  ThreadJoin,
  AtomicLoad,
  AtomicStore,
  AtomicReadModifyWrite,
  AtomicCompareExchange,
  MutexLock,
  MutexUnlock,
  Count,
};

enum class AtomicMemoryOrder : std::uint8_t {
  Relaxed,
  Acquire,
  Release,
  AcquireRelease,
  SequentiallyConsistent,
  Count,
};

struct SynchronizationOperation {
  SynchronizationOperationKind kind = SynchronizationOperationKind::None;
  std::optional<AtomicMemoryOrder> order;
  std::optional<AtomicMemoryOrder> failureOrder;

  friend bool operator==(const SynchronizationOperation &,
                         const SynchronizationOperation &) = default;
};

} // namespace lang
