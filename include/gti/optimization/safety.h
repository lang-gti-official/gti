#pragma once

#include "gti/checked_integer.h"
#include "gti/mir.h"
#include "gti/optimization/alias.h"

#include <optional>
#include <vector>

namespace lang {

// Canonical, replayable evidence for one removable source-level check. The
// related identities are deliberately MIR-only: clients cannot justify a
// discharge from a source wrapper name, emitted C++ shape, or a native
// optimizer's behavior.
struct MirSafetyProof {
  MirBodyAddress body;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  MirSafetyDischarge discharge;
  std::vector<MirBlockId> relatedBlocks;
  std::vector<MirInstructionId> relatedInstructions;
  std::vector<MirValueId> relatedValues;
  std::vector<MirPlaceId> relatedPlaces;

  friend bool operator==(const MirSafetyProof &,
                         const MirSafetyProof &) = default;
};

struct MirSafetyAnalysis {
  std::vector<MirSafetyProof> proofs;

  [[nodiscard]] const MirSafetyProof *find(MirInstructionId instruction,
                                           MirSafetyDischarge discharge) const;

  friend bool operator==(const MirSafetyAnalysis &,
                         const MirSafetyAnalysis &) = default;
};

// Value facts derived from the complete, concrete static-call graph. These
// facts are analysis results, not call effects: effects continue to describe
// observable behavior only. The analysis manager revisions this result as one
// program-wide unit whenever any participating MIR body changes.
struct MirClosedCallIntegerInterval {
  CheckedIntegerDomain domain;
  CheckedIntegerValue lower;
  CheckedIntegerValue upper;

  friend bool operator==(const MirClosedCallIntegerInterval &,
                         const MirClosedCallIntegerInterval &) = default;
};

struct MirClosedCallBodyAnalysis {
  MirBodyAddress body;
  bool reached = false;
  // True only when successful exits have both complete fact transport and no
  // opaque effects. Internally a known result may still be transported across
  // an opaque call, after invalidating caller memory; that is not effect
  // closure and cannot justify recurrence invariance.
  bool closed = false;
  std::optional<MirClosedCallIntegerInterval> returnInterval;
  MirSafetyAnalysis safety;

  friend bool operator==(const MirClosedCallBodyAnalysis &,
                         const MirClosedCallBodyAnalysis &) = default;
};

struct MirClosedCallSafetyAnalysis {
  bool converged = false;
  // Maximum worklist analyses of any one body, not whole-program scans.
  std::size_t iterations = 0;
  std::vector<MirClosedCallBodyAnalysis> bodies;

  [[nodiscard]] const MirClosedCallBodyAnalysis *
  find(MirBodyAddress body) const;
};

// Computes one bounded, non-recursive contextual summary over exact static
// GTI calls and constructors. Unsupported, open, native, virtual, recursive,
// owning-argument, or synchronization-capable edges fail closed. Body-local
// facts remain available even when no contextual summary is admitted. Once
// the whole analysis converges, contextual proofs established before a
// conservative call barrier remain usable even when that barrier prevents the
// enclosing body from exporting a complete summary.
// Actual raw pointee/lifetime operations and unsupported unsafe calls are
// per-instruction opaque barriers, not a reason to discard earlier proofs.
// Inert address computations/markers preserve unrelated facts without proving
// pointee invariants. Selected unsafe source functions use actual body effects;
// a successful path with a raw barrier cannot export effect closure.
[[nodiscard]] MirClosedCallSafetyAnalysis
analyzeMirClosedCallSafetyChecks(const MirProgram &program);

// Performs conservative MIR abstract interpretation. Body-local facts are
// always considered; facts from a closed concrete static-call context are
// admitted only when the whole-program analysis above converges.
[[nodiscard]] MirSafetyAnalysis
analyzeMirSafetyChecks(const MirProgram &program, MirBodyAddress address);

// Reuses an analysis-manager-owned alias snapshot when one is already
// available. The uncached entry point above remains the canonical replay API.
[[nodiscard]] MirSafetyAnalysis
analyzeMirSafetyChecks(const MirProgram &program, MirBodyAddress address,
                       const MirPlaceAliasEscapeAnalysis &aliases);

[[nodiscard]] constexpr MirTransformProofKind
mirSafetyTransformProof(MirSafetyCheckFamily family) {
  switch (family) {
  case MirSafetyCheckFamily::Bounds:
    return MirTransformProofKind::BoundsRange;
  case MirSafetyCheckFamily::ArithmeticOverflow:
  case MirSafetyCheckFamily::ArithmeticDomain:
    return MirTransformProofKind::ArithmeticRange;
  case MirSafetyCheckFamily::Count:
    return MirTransformProofKind::Count;
  }
  return MirTransformProofKind::Count;
}

} // namespace lang
