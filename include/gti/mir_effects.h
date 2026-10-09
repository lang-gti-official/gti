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
// Exact integer-scalar final-address construction has no selected constructor,
// cleanup or adapter. It still writes raw storage and requires the ordinary
// unsafe vacancy/extent/alignment contract; this is not a validity proof.
[[nodiscard]] bool
isMirScalarFinalAddressConstruction(const MirInstruction &instruction);
// The same vacant-storage transfer for an exact generated scalar-record
// copy/move. Uses the verifier's construction shape and the structural
// transport classification below; no source constructor or replacement.
[[nodiscard]] bool
isMirStructuralFinalAddressConstruction(const MirProgram &program,
                                        const MirInstruction &instruction);
// Exact generated copy/move of ordinary scalar-field records. This proves
// transport, not cleanup, replacement, raw-pointee validity or alias freedom.
// Unsupported capabilities, borrowed state and open/overlapping layouts fail
// closed. Shared effects and optimizer transfer use the same classification.
[[nodiscard]] bool
isMirStructuralValueTransfer(const MirProgram &program,
                             const MirInstruction &instruction);
[[nodiscard]] bool isMirStructuralValueDrop(const MirProgram &program,
                                            const MirInstruction &instruction);
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

// Generated relocation into an empty slot, including lifecycle-guarded
// source-destructor classes and their complete base/field state. This does
// not prove assignment safe: replacement must still run the old cleanup.
[[nodiscard]] bool
mirTypeRelocationIsDefinedFailureFree(const MirProgram &program,
                                      const SemanticType &type);

// Replays semantic structural relocation eligibility over concrete MIR type
// identities. Verification compares published nominal traits with this proof;
// backend selection must consume it, never infer eligibility from C++ names.
[[nodiscard]] RelocationKind mirTypeRelocationKind(const MirProgram &program,
                                                   const SemanticType &type);

// Exact ordered class/unique-owner/collection cleanup: the class's guarded
// source body, if any, precedes its active subobjects. All are represented;
// recursive ownership uses exact class back-edges; unsupported subobjects
// have no descriptor.
[[nodiscard]] std::optional<MirDropType>
mirUniqueCleanupType(const MirProgram &program, const SemanticType &type);

// All exact source-destructor effects of a represented cleanup schedule, not
// one representative leaf standing in for a whole aggregate. The argument
// must be a complete root recipe, not a projected child containing back-edges.
[[nodiscard]] std::vector<HirDestructorInstanceId>
mirDropDestructorTargets(const MirDropType &type);
[[nodiscard]] bool mirDropHasPolymorphicCleanup(const MirDropType &type);
[[nodiscard]] bool mirDropMayRaiseDefinedFailure(const MirProgram &program,
                                                 const MirDropType &type);

// Compatibility accessor for clients of MIR v20's function-only slice.
[[nodiscard]] std::vector<bool>
deriveMirScalarDefinedFailureEffects(const MirProgram &program);

} // namespace lang
