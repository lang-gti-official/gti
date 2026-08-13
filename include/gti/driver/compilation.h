#pragma once

#include "gti/backend.h"
#include "gti/cpp_emitter.h"
#include "gti/diagnostic.h"
#include "gti/mir.h"
#include "gti/optimizer.h"
#include "gti/source_graph.h"
#include "gti/standard_library.h"
#include "gti/target.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace lang::driver {

class CompilationRequest final {
public:
  CompilationRequest(std::filesystem::path entry,
                     StandardLibraryLayout standardLibrary, TargetInfo target,
                     OptimizationLevel optimization, CppStandard cppStandard);

  [[nodiscard]] const std::filesystem::path &entry() const;
  [[nodiscard]] const StandardLibraryLayout &standardLibrary() const;
  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] CppStandard cppStandard() const;

private:
  std::filesystem::path entryPath;
  StandardLibraryLayout standardLibraryLayout;
  TargetInfo targetInfo;
  OptimizationLevel optimizationLevel;
  CppStandard backendStandard;
};

enum class CompilationStatus {
  Success,
  FrontendFailure,
  MirVerificationFailure,
};

// The exact source state consumed by one whole-program compilation. Project
// builds may inspect this immutable-at-the-boundary snapshot to derive cache
// identity, then move it into the canonical frontend without loading source a
// second time.
struct CompilationInputs {
  SourceGraph sourceGraph;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;
  bool sourceValid = false;

  [[nodiscard]] bool succeeded() const { return sourceValid; }
};

struct CompilationResult {
  CompilationStatus status = CompilationStatus::FrontendFailure;
  std::optional<BackendArtifact> artifact;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;
  std::vector<MirVerificationError> mirErrors;

  [[nodiscard]] bool succeeded() const {
    return status == CompilationStatus::Success && artifact.has_value();
  }
};

enum class CheckStatus {
  Success,
  FrontendFailure,
};

struct CheckResult {
  CheckStatus status = CheckStatus::FrontendFailure;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == CheckStatus::Success;
  }
};

[[nodiscard]] CheckResult checkCompilation(const CompilationRequest &request);

[[nodiscard]] CompilationInputs
loadCompilationInputs(const CompilationRequest &request);

[[nodiscard]] CompilationResult compileToCpp(const CompilationRequest &request);

[[nodiscard]] CompilationResult compileToCpp(const CompilationRequest &request,
                                             CompilationInputs inputs);

} // namespace lang::driver
