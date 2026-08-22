#pragma once

#include "cpp_mir_body_emitter.h"
#include "cpp_mir_program_plan.h"

#include "gti/cpp_emitter.h"
#include "gti/lowered_program.h"

#include <optional>
#include <string>
#include <vector>

namespace lang {

// Private C++-representation boundary. Backend callers provide only the sealed
// lowered program; they cannot author body/data/thunk support claims.
enum class CppMirRepresentationSnapshotIssueKind {
  InvalidLoweredProgram,
  UnsupportedMirEmission,
};

struct CppMirRepresentationSnapshotIssue {
  CppMirRepresentationSnapshotIssueKind kind =
      CppMirRepresentationSnapshotIssueKind::InvalidLoweredProgram;
  std::string detail;
};

struct CppMirRepresentationSnapshotBuild {
  std::optional<CppMirRepresentationSnapshot> snapshot;
  std::vector<CppMirRepresentationSnapshotIssue> issues;

  [[nodiscard]] bool valid() const {
    return snapshot.has_value() && issues.empty();
  }
};

// Builds and privately seals the complete C++ representation inventory from
// the compiler-owned backend contract. Support and generated-item claims are
// derived here; no boolean/capability input can bless a row.
[[nodiscard]] CppMirRepresentationSnapshotBuild
buildCppMirRepresentationSnapshot(const LoweredProgram &program,
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
buildCppMirBodyEmissionMapRows(const LoweredProgram &program,
                               CppStandard standard);

// Applies the declaration-level type spellings required when one concrete MIR
// instance must be emitted through a source C++ template (most importantly an
// unnameable closure type represented by its GTI generic parameter). The
// returned count is zero when no overlay was needed; nullopt means the exact
// lowered declaration/MIR identities cannot produce an unambiguous overlay.
[[nodiscard]] std::optional<std::size_t>
cppMirApplyCallableTemplateTypeOverlays(
    CppMirBodyEmissionMapRows &rows, const LoweredProgram &program,
    CppStandard standard, const LoweredFunctionDeclaration &declaration,
    const MirFunctionInstance &instance);

[[nodiscard]] std::optional<std::size_t>
cppMirApplyGenericOwnerConstructorTypeOverlays(
    CppMirBodyEmissionMapRows &rows, const LoweredProgram &program,
    const LoweredConstructorDeclaration &declaration,
    const MirConstructorInstance &instance);

enum class CppMirBackendProgramRoute {
  VerifiedMir,
};

// Selects the verified-MIR route only for a complete whole-program plan.
// Incoherent or unsupported plans fail before any emitter is constructed.
[[nodiscard]] CppMirBackendProgramRoute
selectCppMirBackendProgramRoute(const CppMirProgramPlan &plan);

} // namespace lang
