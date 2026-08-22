#pragma once

#include "cpp_mir_body_emitter.h"
#include "cpp_mir_program_plan.h"

#include "gti/ast.h"
#include "gti/cpp_emitter.h"
#include "gti/hir.h"
#include "gti/lowered_program.h"
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
  InvalidNativeCallbackAdapter,
  UnsupportedMirEmission,
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
                                  const TargetInfo &target,
                                  CppStandard standard = CppStandard::Cpp23);

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

[[nodiscard]] CppMirBodyEmissionMapRows
buildCppMirBodyEmissionMapRows(const LoweredProgram &program,
                               CppStandard standard);

// Applies the declaration-level type spellings required when one concrete MIR
// instance must be emitted through a source C++ template (most importantly an
// unnameable closure type represented by its GTI generic parameter). The
// returned count is zero when no overlay was needed; nullopt means the exact
// semantic/HIR/MIR identities cannot produce an unambiguous overlay.
[[nodiscard]] std::optional<std::size_t>
cppMirApplyCallableTemplateTypeOverlays(CppMirBodyEmissionMapRows &rows,
                                        const SemanticModel &semantics,
                                        CppStandard standard,
                                        const FunctionInfo &declaration,
                                        const MirFunctionInstance &instance);

[[nodiscard]] std::optional<std::size_t>
cppMirApplyGenericOwnerConstructorTypeOverlays(
    CppMirBodyEmissionMapRows &rows, const SemanticModel &semantics,
    const HirProgram &hir, const MirProgram &mir,
    const ConstructorDecl &declaration, const MirConstructorInstance &instance);

enum class CppMirBackendProgramRoute {
  VerifiedMir,
};

// Selects the verified-MIR route only for a complete whole-program plan.
// Incoherent or unsupported plans fail before any emitter is constructed.
[[nodiscard]] CppMirBackendProgramRoute
selectCppMirBackendProgramRoute(const CppMirProgramPlan &plan);

} // namespace lang
