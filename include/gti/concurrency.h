#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace lang {

enum class CThreadCallbackKind : std::uint8_t {
  None,
  ThreadEntry,
  OnceInitializer,
};

// Selected only from exact compiler-private C-thread runtime declarations in
// the trusted library unit. These remain ordinary native calls, not managed
// task intrinsics: the caller retains context and manual join obligations.
enum class CThreadRuntimeOperation : std::uint8_t {
  None,
  Create,
  Join,
  Begin,
  End,
  MutexInit,
  MutexDestroy,
  MutexLock,
  MutexTryLock,
  MutexTimedLock,
  MutexUnlock,
  ConditionInit,
  ConditionDestroy,
  ConditionSignal,
  ConditionBroadcast,
  ConditionWait,
  ConditionTimedWait,
  InvokeOnceInitializer,
  Count
};

[[nodiscard]] constexpr bool
requiresCThreadConcurrency(CThreadRuntimeOperation operation) {
  return operation != CThreadRuntimeOperation::None &&
         operation != CThreadRuntimeOperation::Begin &&
         operation != CThreadRuntimeOperation::End &&
         operation < CThreadRuntimeOperation::Count;
}

// Backend-independent synchronization identities. Public library types such
// as std::thread remain ordinary GTI classes; trusted private capabilities
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
  // Wait atomically enrolls/releases and reacquires before returning. Notify
  // schedules existing waiters; shared-data ordering is through their mutex.
  ConditionWait,
  ConditionNotify,
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

[[nodiscard]] constexpr bool validAtomicMemoryOrder(AtomicMemoryOrder order) {
  return static_cast<std::size_t>(order) <
         static_cast<std::size_t>(AtomicMemoryOrder::Count);
}

[[nodiscard]] constexpr bool validAtomicLoadOrder(AtomicMemoryOrder order) {
  return order == AtomicMemoryOrder::Relaxed ||
         order == AtomicMemoryOrder::Acquire ||
         order == AtomicMemoryOrder::SequentiallyConsistent;
}

[[nodiscard]] constexpr bool validAtomicStoreOrder(AtomicMemoryOrder order) {
  return order == AtomicMemoryOrder::Relaxed ||
         order == AtomicMemoryOrder::Release ||
         order == AtomicMemoryOrder::SequentiallyConsistent;
}

[[nodiscard]] constexpr bool
validAtomicCompareExchangeFailureOrder(AtomicMemoryOrder success,
                                       AtomicMemoryOrder failure) {
  if (failure == AtomicMemoryOrder::Release ||
      failure == AtomicMemoryOrder::AcquireRelease) {
    return false;
  }
  switch (success) {
  case AtomicMemoryOrder::Relaxed:
  case AtomicMemoryOrder::Release:
    return failure == AtomicMemoryOrder::Relaxed;
  case AtomicMemoryOrder::Acquire:
  case AtomicMemoryOrder::AcquireRelease:
    return failure == AtomicMemoryOrder::Relaxed ||
           failure == AtomicMemoryOrder::Acquire;
  case AtomicMemoryOrder::SequentiallyConsistent:
    return failure == AtomicMemoryOrder::Relaxed ||
           failure == AtomicMemoryOrder::Acquire ||
           failure == AtomicMemoryOrder::SequentiallyConsistent;
  case AtomicMemoryOrder::Count:
    return false;
  }
  return false;
}

[[nodiscard]] constexpr bool
validSynchronizationOperation(const SynchronizationOperation &operation) {
  if (static_cast<std::size_t>(operation.kind) >=
      static_cast<std::size_t>(SynchronizationOperationKind::Count)) {
    return false;
  }
  if ((operation.order && !validAtomicMemoryOrder(*operation.order)) ||
      (operation.failureOrder &&
       !validAtomicMemoryOrder(*operation.failureOrder))) {
    return false;
  }
  switch (operation.kind) {
  case SynchronizationOperationKind::None:
  case SynchronizationOperationKind::ThreadSpawn:
  case SynchronizationOperationKind::ThreadJoin:
  case SynchronizationOperationKind::MutexLock:
  case SynchronizationOperationKind::MutexUnlock:
  case SynchronizationOperationKind::ConditionWait:
  case SynchronizationOperationKind::ConditionNotify:
    return !operation.order && !operation.failureOrder;
  case SynchronizationOperationKind::AtomicLoad:
    return operation.order && validAtomicLoadOrder(*operation.order) &&
           !operation.failureOrder;
  case SynchronizationOperationKind::AtomicStore:
    return operation.order && validAtomicStoreOrder(*operation.order) &&
           !operation.failureOrder;
  case SynchronizationOperationKind::AtomicReadModifyWrite:
    return operation.order && !operation.failureOrder;
  case SynchronizationOperationKind::AtomicCompareExchange:
    return operation.order && operation.failureOrder &&
           validAtomicCompareExchangeFailureOrder(*operation.order,
                                                  *operation.failureOrder);
  case SynchronizationOperationKind::Count:
    return false;
  }
  return false;
}

} // namespace lang
