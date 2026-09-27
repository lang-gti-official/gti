#pragma once

#include "gti/diagnostic.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/project.h"

#include <optional>
#include <string>
#include <vector>

namespace lang::driver {

struct BuildScriptExecutionOptions {
  std::string cCompiler;
  std::string cppCompiler;
  unsigned jobs = 1;
  bool verbose = false;
};

struct BuildScriptOutcome {
  std::string packageIdentity;
  bool reused = false;
  std::string output;
};

struct BuildScriptExecutionResult {
  std::optional<NativeInputs> nativeInputs;
  std::vector<BuildScriptOutcome> outcomes;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const { return nativeInputs.has_value(); }
};

// Runs approved package build programs and folds their structured native
// outputs into the already-resolved project inputs. No frontend or backend is
// entered until this operation succeeds.
[[nodiscard]] BuildScriptExecutionResult
executeBuildScripts(const ProjectBuildPlan &plan,
                    const BuildScriptExecutionOptions &options);

} // namespace lang::driver
