#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace lang {

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
[[nodiscard]] MirEffectTraits effects(const MirProgram &program,
                                      const MirInstruction &instruction);

[[nodiscard]] std::optional<MirEffectSummary>
exactMirInvocationSummary(const MirProgram &program, const MirBody &body,
                          const MirInstruction &instruction);

[[nodiscard]] MirProgramEffectSummaries
deriveMirProgramEffectSummaries(const MirProgram &program);

struct MirDefinedFailureEffects {
  std::vector<bool> functions;
  std::vector<bool> constructors;
  std::vector<bool> destructors;

  friend bool operator==(const MirDefinedFailureEffects &,
                         const MirDefinedFailureEffects &) = default;
};

// Failure-dimension slice of O-MIR-02. Each result is indexed by its concrete
// instance ID and remains conservative (`true`) unless an acyclic, closed,
// bounded scalar/static-call/construction/normal-cleanup graph is proved from
// MIR alone.
[[nodiscard]] MirDefinedFailureEffects
deriveMirDefinedFailureEffects(const MirProgram &program);

// Proves that moving a value cannot execute a GTI defined-failure edge. For a
// class this requires an exact concrete MIR instance, a generated move member,
// and recursively failure-free structural state; declared move bodies remain
// outside the proof.
[[nodiscard]] bool mirTypeMoveIsDefinedFailureFree(const MirProgram &program,
                                                   const SemanticType &type);

// Compatibility accessor for clients of MIR v20's function-only slice.
[[nodiscard]] std::vector<bool>
deriveMirScalarDefinedFailureEffects(const MirProgram &program);

} // namespace lang
