#pragma once

#include "gti/mir_verifier.h"

#include <string>
#include <string_view>

namespace lang {

enum class MirAnalysisKind : std::uint8_t;

// A maintenance gate, not a second effects model or permission to transform.
// Supported names the existing bounded consumer; Barrier means that consumer
// must retain/forget the operation, and NotApplicable denotes neutral metadata.
// Actual legality still comes from mir_effects, typed analysis and proof
// replay.
enum class MirOptimizationHandling {
  Supported,
  Barrier,
  NotApplicable,
  Unknown
};

struct MirOptimizationCoverage {
  MirOptimizationHandling handling = MirOptimizationHandling::Unknown;
  std::string_view consumer = "unclassified MIR vocabulary";

  [[nodiscard]] bool classified() const {
    return handling != MirOptimizationHandling::Unknown;
  }
};

[[nodiscard]] MirOptimizationCoverage optimizationCoverage(MirInstructionKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(MirOperation);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(IntrinsicKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(MirTerminatorKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(MirProjectionKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(MirPlaceRootKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(MirBodyKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(MirOperandKind);
[[nodiscard]] MirOptimizationCoverage
    optimizationCoverage(MirLifecycleEventKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(DefinedFailureCode);
[[nodiscard]] MirOptimizationCoverage
    optimizationCoverage(DefinedFailureDetail);
[[nodiscard]] MirOptimizationCoverage
    optimizationCoverage(FailurePropagationKind);
[[nodiscard]] MirOptimizationCoverage
    optimizationCoverage(SynchronizationOperationKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(AtomicMemoryOrder);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(UnsafeOperationKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(MirAnalysisKind);
[[nodiscard]] MirOptimizationCoverage optimizationCoverage(MirTransformKind);
[[nodiscard]] MirOptimizationCoverage
    optimizationCoverage(MirTransformProofKind);
[[nodiscard]] MirOptimizationCoverage
    optimizationCoverage(MirSafetyCheckFamily);

// This cheap structural preflight is shared by the MIR verifier, analyses and
// the pipeline even when optional full verification is disabled. Unknown enum
// values never enter CFG/replay as implicit exits or harmless operations.
[[nodiscard]] MirVerificationResult
verifyMirOptimizationVocabulary(const MirBody &body, std::size_t owner = 0);

// Deterministic, factual explanations from authoritative effects and schedules.
// Empty means no general barrier was found, NOT that a rewrite is legal.
[[nodiscard]] std::string
mirOptimizationBarrierReason(const MirProgram &program,
                             const MirInstruction &instruction,
                             const MirBody *body = nullptr);

} // namespace lang
