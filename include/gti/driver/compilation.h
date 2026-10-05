#pragma once

#include "gti/backend.h"
#include "gti/configuration_flags.h"
#include "gti/cpp_standard.h"
#include "gti/diagnostic.h"
#include "gti/mir.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"
#include "gti/progress.h"
#include "gti/source_graph.h"
#include "gti/standard_library.h"
#include "gti/target.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lang::driver {

enum class RuntimeProfile {
  None,
  Hosted,
};

// Immutable frontend-facing configuration resolved by driver adapters before
// source loading. Runtime capabilities are derived from the selected runtime
// profile and target; compiler phases only consume the resulting TargetInfo.
class AnalysisConfiguration final {
public:
  AnalysisConfiguration(
      TargetInfo target = TargetInfo::host(),
      ExecutionProfile executionProfile = ExecutionProfile::SingleThreaded,
      RuntimeProfile runtimeProfile = RuntimeProfile::None,
      ConfigurationFlags configurationFlags = {});

  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] RuntimeProfile runtimeProfile() const;
  [[nodiscard]] const ConfigurationFlags &configurationFlags() const;

private:
  TargetInfo targetInfo;
  RuntimeProfile selectedRuntimeProfile;
  ConfigurationFlags configuredFlags;
};

struct OptimizationOutputRequest {
  bool capturePreMir = false;
  bool capturePostMir = false;
  bool captureReport = false;

  [[nodiscard]] bool requested() const {
    return capturePreMir || capturePostMir || captureReport;
  }
};

// Consumer-owned diagnostic presentation policy. Package identities come from
// the resolved workspace, never from directory names or a dependency's profile.
// Unowned, selected-package, standard-library and native diagnostics remain
// visible. The compiler still analyzes every package under unchanged semantics.
class PackageWarningPolicy final {
public:
  PackageWarningPolicy() = default;
  PackageWarningPolicy(bool dependencyWarnings,
                       std::string selectedPackageIdentity,
                       std::vector<std::string> dependencyPackageIdentities);

  [[nodiscard]] bool dependencyWarnings() const;
  [[nodiscard]] const std::string &selectedPackage() const;
  [[nodiscard]] const std::vector<std::string> &dependencyPackages() const;
  [[nodiscard]] bool suppresses(const Diagnostic &diagnostic,
                                const SourceGraph &sources) const;

private:
  bool emitDependencyWarnings = true;
  std::string selectedIdentity;
  std::vector<std::string> dependencyIdentities;
};

class CompilationRequest final {
public:
  CompilationRequest(std::filesystem::path entry,
                     StandardLibraryLayout standardLibrary, TargetInfo target,
                     OptimizationLevel optimization, CppStandard cppStandard,
                     std::vector<PackageSourceRoot> packageSourceRoots = {},
                     ConfigurationFlags configurationFlags = {},
                     OptimizationOutputRequest optimizationOutputs = {},
                     WarningPolicy warningPolicy = {}, bool debugInfo = false,
                     PackageWarningPolicy packageWarnings = {});

  [[nodiscard]] const std::filesystem::path &entry() const;
  [[nodiscard]] const StandardLibraryLayout &standardLibrary() const;
  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] CppStandard cppStandard() const;
  [[nodiscard]] bool debugInfo() const;
  [[nodiscard]] const std::vector<PackageSourceRoot> &packageSources() const;
  [[nodiscard]] const ConfigurationFlags &configurationFlags() const;
  [[nodiscard]] const OptimizationOutputRequest &optimizationOutputs() const;
  [[nodiscard]] const WarningPolicy &warnings() const;
  [[nodiscard]] const PackageWarningPolicy &packageWarnings() const;
  [[nodiscard]] bool requiresEntryPoint() const;
  [[nodiscard]] CompilationRequest forExecutable() const;

private:
  std::filesystem::path entryPath;
  StandardLibraryLayout standardLibraryLayout;
  TargetInfo targetInfo;
  OptimizationLevel optimizationLevel;
  CppStandard backendStandard;
  std::vector<PackageSourceRoot> packageSourceRootSet;
  ConfigurationFlags configuredFlags;
  OptimizationOutputRequest requestedOptimizationOutputs;
  WarningPolicy warningPolicy;
  PackageWarningPolicy packageWarningPolicy;
  bool entryPointRequired = false;
  bool sourceDebugInfo = false;
};

enum class CompilationStatus {
  Success,
  FrontendFailure,
  MirVerificationFailure,
  BackendFailure,
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
  std::optional<std::string> preOptimizationMir;
  std::optional<std::string> postOptimizationMir;
  std::optional<OptimizationReport> optimizationReport;

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

[[nodiscard]] CheckResult
checkCompilation(const CompilationRequest &request,
                 ProgressObserver *progress = nullptr);

[[nodiscard]] CompilationInputs
loadCompilationInputs(const CompilationRequest &request,
                      ProgressObserver *progress = nullptr);

// Run the canonical whole-program pipeline with a caller-provided backend.
// Backend exceptions are translated into structured compilation failures at
// this driver boundary rather than escaping into individual CLI/library
// callers.
[[nodiscard]] CompilationResult
compileWithBackend(const CompilationRequest &request, Backend &backend,
                   ProgressObserver *progress = nullptr);

[[nodiscard]] CompilationResult
compileToCpp(const CompilationRequest &request,
             ProgressObserver *progress = nullptr);

[[nodiscard]] CompilationResult
compileToCpp(const CompilationRequest &request, CompilationInputs inputs,
             ProgressObserver *progress = nullptr);

[[nodiscard]] CompilationResult
compileToNativeHeader(const CompilationRequest &request,
                      ProgressObserver *progress = nullptr);

// Serializes the verified optimized MIR snapshot that executable backends
// consume, in the deterministic versioned text format owned by `MirPrinter`.
[[nodiscard]] CompilationResult
compileToMir(const CompilationRequest &request,
             ProgressObserver *progress = nullptr);

} // namespace lang::driver
