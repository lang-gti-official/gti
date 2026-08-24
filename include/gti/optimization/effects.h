#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <optional>
#include <string_view>

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
// Refines an exact static call/construct/drop through independently verified
// per-instance summaries. Dynamic and unresolved boundaries remain maximally
// conservative.
[[nodiscard]] MirEffectTraits effects(const MirProgram &program,
                                      const MirInstruction &instruction);

// Returns the merged verified summary only when an invocation has a closed,
// exact static target set. Native, dynamic, unresolved, and private-runtime
// boundaries return no summary and must remain conservative in clients.
[[nodiscard]] std::optional<MirEffectSummary>
exactMirInvocationSummary(const MirProgram &program, const MirBody &body,
                          const MirInstruction &instruction);

[[nodiscard]] MirProgramEffectSummaries
deriveMirProgramEffectSummaries(const MirProgram &program);
[[nodiscard]] MirVerificationResult
verifyMirEffectSummaries(const MirProgram &program);

} // namespace lang
