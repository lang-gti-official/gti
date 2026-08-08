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

[[nodiscard]] CompilationResult compileToCpp(const CompilationRequest &request);

} // namespace lang::driver
