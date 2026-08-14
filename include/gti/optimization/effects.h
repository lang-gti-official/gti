#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <string_view>

namespace lang {

struct MirEffectTraits {
  bool readsPlace = false;
  bool writesPlace = false;
  bool readsUnknownMemory = false;
  bool writesUnknownMemory = false;
  bool allocates = false;
  bool invokesRuntime = false;
  bool mayTrap = false;
  bool startsLoan = false;
  bool endsLoan = false;
  bool dependsOnLoan = false;
  bool copiesValue = false;
  bool movesValue = false;
  bool initializesValue = false;
  bool dropsValue = false;
  bool invokesUserCode = false;
  bool targetDependent = false;
  bool maySynchronize = false;
  bool speculatable = false;
  bool removableWhenUnused = false;
  bool reorderable = false;
};

inline constexpr std::size_t mirInstructionKindCount =
    static_cast<std::size_t>(MirInstructionKind::Count);
inline constexpr std::size_t mirOperationCount =
    static_cast<std::size_t>(MirOperation::Count);
inline constexpr std::size_t intrinsicKindCount =
    static_cast<std::size_t>(IntrinsicKind::Count);
inline constexpr std::size_t synchronizationOperationKindCount =
    static_cast<std::size_t>(SynchronizationOperationKind::Count);
inline constexpr std::size_t atomicMemoryOrderCount =
    static_cast<std::size_t>(AtomicMemoryOrder::Count);

[[nodiscard]] std::string_view name(MirInstructionKind kind);
[[nodiscard]] std::string_view name(MirOperation operation);
[[nodiscard]] std::string_view name(IntrinsicKind intrinsic);
[[nodiscard]] std::string_view name(SynchronizationOperationKind kind);
[[nodiscard]] std::string_view name(AtomicMemoryOrder order);

[[nodiscard]] MirEffectTraits effects(MirInstructionKind kind);
[[nodiscard]] MirEffectTraits effects(MirOperation operation);
[[nodiscard]] MirEffectTraits effects(IntrinsicKind intrinsic);
[[nodiscard]] MirEffectTraits effects(SynchronizationOperationKind kind);
[[nodiscard]] MirEffectTraits effects(const MirInstruction &instruction);

} // namespace lang
