#pragma once

#include "gti/mir.h"
#include "gti/optimization/alias.h"

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
};

// Performs a conservative body-local abstract interpretation over GTI MIR.
// It proves only checks whose exact outcome follows from integer intervals,
// branch predicates, fixed-array extents, or tracked private-storage state.
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
  case MirSafetyCheckFamily::StorageState:
    return MirTransformProofKind::StorageInitialization;
  case MirSafetyCheckFamily::ArithmeticOverflow:
  case MirSafetyCheckFamily::ArithmeticDomain:
    return MirTransformProofKind::ArithmeticRange;
  case MirSafetyCheckFamily::Count:
    return MirTransformProofKind::Count;
  }
  return MirTransformProofKind::Count;
}

} // namespace lang
