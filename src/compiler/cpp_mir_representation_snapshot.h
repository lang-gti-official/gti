#pragma once

#include "cpp_mir_body_emitter.h"
#include "cpp_mir_program_plan.h"

#include "gti/ast.h"
#include "gti/cpp_emitter.h"
#include "gti/hir.h"
#include "gti/semantic_analyzer.h"
#include "gti/target.h"

#include <optional>
#include <string>
#include <vector>

namespace lang {

// Private C++-representation boundary. Public backend callers provide only
// frontend snapshots; they cannot author body/data/thunk support claims.
enum class CppMirRepresentationSnapshotIssueKind {
  CrossPhaseMismatch,
  MissingProgramDeclaration,
  MissingSemanticDeclaration,
  MissingHirDeclaration,
  MissingMirBodyIdentity,
  InvalidHostedEntry,
  InvalidProgramInitialization,
};

struct CppMirRepresentationSnapshotIssue {
  CppMirRepresentationSnapshotIssueKind kind =
      CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch;
  std::string detail;
};

struct CppMirRepresentationSnapshotBuild {
  std::optional<CppMirRepresentationSnapshot> snapshot;
  std::vector<CppMirRepresentationSnapshotIssue> issues;

  [[nodiscard]] bool valid() const {
    return snapshot.has_value() && issues.empty();
  }
};

// Checks the exact SemanticModel/HIR analyzed-Program/target seal, verified
// HIR-to-MIR program-initialization plan/storage/provenance identity, and MIR
// header/owner identities consumed by source-MIR admission and the
// representation snapshot builder.
[[nodiscard]] bool
cppMirFrontendSnapshotsMatch(const SemanticModel &semantics,
                             const HirProgram &hir, const MirProgram &mir,
                             std::string *mismatch = nullptr);

// Builds and privately seals the complete copied representation inventory for
// one exact Program/SemanticModel/HIR/MIR/Target tuple. Support and thunk
// claims are derived here; no boolean/capability input can bless a row.
[[nodiscard]] CppMirRepresentationSnapshotBuild
buildCppMirRepresentationSnapshot(const Program &program,
                                  const SemanticModel &semantics,
                                  const HirProgram &hir, const MirProgram &mir,
                                  const TargetInfo &target);

// Builds the deterministic copied representation rows the generic MIR body
// emitter consumes (ADR 016 phase 4). Spellings come only from the extracted
// cpp_representation authorities, so a row can never drift from an emitted
// byte. The walk is MIR-ordered and pointer-free: types referenced by any MIR
// body (with their argument closures), field rows for every concrete class
// instance, qualified call-target names for source free functions, and enum
// rows for every non-payload enum MIR references. Facts outside this
// inventory are deliberately absent so unsupported shapes stay fail-closed.
[[nodiscard]] CppMirBodyEmissionMapRows
buildCppMirBodyEmissionMapRows(const SemanticModel &semantics,
                               const MirProgram &mir, CppStandard standard);

enum class CppMirBackendProgramRoute {
  Compatibility,
};

// Selects exactly one route for the complete plan. Incoherent plans are never
// executable. The compatibility route is whole-program during this tranche.
[[nodiscard]] CppMirBackendProgramRoute
selectCppMirBackendProgramRoute(const CppMirProgramPlan &plan);

} // namespace lang
