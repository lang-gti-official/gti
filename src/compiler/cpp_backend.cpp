#include "gti/cpp_backend.h"

#include "cpp_mir_body_emitter.h"
#include "cpp_mir_program_plan.h"
#include "cpp_mir_representation_snapshot.h"

#include "gti/cpp_emitter.h"

#include <stdexcept>

namespace lang {

CppBackend::CppBackend(CppStandard standard) : standard(standard) {}

std::string_view CppBackend::name() const { return "cpp"; }

BackendArtifact CppBackend::generate(const BackendInput &input) {
  if (input.sourceMir == nullptr) {
    throw std::logic_error(
        "C++ backend requires the verified source MIR snapshot");
  }
  const FailureMetadataVerificationResult failureMetadata =
      verifyFailureMetadata(input.mir.failureMetadata());
  if (!failureMetadata.valid()) {
    const std::string detail = failureMetadata.errors.empty()
                                   ? "unknown verification failure"
                                   : failureMetadata.errors.front();
    throw std::logic_error("C++ backend requires coherent failure metadata: " +
                           detail);
  }
  const MirVerificationResult verification = verifyMirProgram(input.mir);
  if (!verification.valid()) {
    const std::string detail = verification.errors.empty()
                                   ? "unknown verification failure"
                                   : verification.errors.front().message;
    throw std::logic_error("C++ backend requires a verified MIR program: " +
                           detail);
  }
  std::string sourceSnapshotMismatch;
  if (!cppMirFrontendSnapshotsMatch(input.semantics, input.hir,
                                    *input.sourceMir,
                                    &sourceSnapshotMismatch)) {
    throw std::logic_error(
        "C++ backend received a canonical source MIR snapshot from a "
        "different frontend analysis: " +
        sourceSnapshotMismatch);
  }
  const MirVerificationResult optimizationCoherence =
      verifyMirOptimizationCoherence(*input.sourceMir, input.mir);
  if (!optimizationCoherence.valid()) {
    const std::string detail =
        optimizationCoherence.errors.empty()
            ? "unknown optimization-coherence failure"
            : optimizationCoherence.errors.front().message;
    throw std::logic_error(
        "C++ backend requires optimized MIR coherent with its verified "
        "source snapshot: " +
        detail);
  }
  std::string snapshotMismatch;
  if (!cppMirFrontendSnapshotsMatch(input.semantics, input.hir, input.mir,
                                    &snapshotMismatch)) {
    throw std::logic_error(
        "C++ backend received semantics, HIR, and MIR from different frontend "
        "snapshots: " +
        snapshotMismatch);
  }

  CppMirRepresentationSnapshotBuild snapshot =
      buildCppMirRepresentationSnapshot(input.program, input.semantics,
                                        input.hir, input.mir, input.target,
                                        standard);
  if (!snapshot.valid()) {
    const std::string detail = snapshot.issues.empty()
                                   ? "unknown snapshot-builder failure"
                                   : snapshot.issues.front().detail;
    throw std::logic_error(
        "C++ backend requires a coherent representation snapshot: " + detail);
  }
  CppMirProgramPlan plan =
      planCppMirProgram(input.mir, std::move(*snapshot.snapshot));
  const CppMirBackendProgramRoute route = selectCppMirBackendProgramRoute(plan);
  if (route != CppMirBackendProgramRoute::VerifiedMir) {
    throw std::logic_error("C++ backend selected an unavailable whole-program "
                           "representation route");
  }

  // Copied representation rows for the generic body emitter are built and
  // owned at this boundary, beside the program plan, from the same verified
  // inputs (ADR 016). Production uses only LoweredProgram for these rows;
  // direct legacy test construction remains until BackendInput itself narrows.
  CppMirBodyEmissionMap generalRows(
      input.loweredProgram != nullptr
          ? buildCppMirBodyEmissionMapRows(*input.loweredProgram, standard)
          : buildCppMirBodyEmissionMapRows(input.semantics, input.mir,
                                           standard));

  // The complete plan above proves every executable body has generic MIR text
  // coverage before this one whole-program representation emitter is built.
  return {.kind = BackendArtifactKind::Source,
          .contents = CppEmitter(input.semantics, input.hir, input.mir,
                                 std::move(plan), std::move(generalRows),
                                 standard, input.target, &input.optimizations)
                          .emit(input.program),
          .extension = ".cpp"};
}

} // namespace lang
