#include "gti/driver/compilation.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_backend.h"
#include "gti/mir_printer.h"
#include "gti/native_header.h"
#include "gti/source_loader.h"
#include "gti/support.h"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

namespace lang::driver {
namespace {

FrontendResult analyze(const CompilationRequest &request,
                       CompilationInputs inputs, ProgressObserver *progress) {
  // Compilation and `check` consume diagnostics, semantics, and symbols; only
  // editor position queries read the occurrence table, so it is not built on
  // this path.
  FrontendOptions options{.target = request.target(),
                          .configurationFlags = request.configurationFlags(),
                          .toolingOccurrences = false,
                          .requireEntryPoint = request.requiresEntryPoint(),
                          .progress = progress};
  options.rawPointerAudit = request.rawPointerAudit();
  options.program = request.program();
  FrontendResult result = Frontend(options).analyzeLoaded(
      request.entry(), std::move(inputs.sourceGraph), std::move(inputs.sources),
      std::move(inputs.diagnostics), inputs.sourceValid);
  addIncludeSuggestions(
      result, options, request.entry(), {request.standardLibrary().prelude}, {},
      {request.standardLibrary().root}, request.packageSources());
  std::erase_if(result.diagnostics, [&](const Diagnostic &diagnostic) {
    return request.warnings().suppresses(diagnostic) ||
           request.packageWarnings().suppresses(diagnostic, result.sourceGraph);
  });
  return result;
}

CompilationResult compileWithLoadedInputs(const CompilationRequest &request,
                                          CompilationInputs inputs,
                                          Backend &backend,
                                          ProgressObserver *progress) {
  FrontendResult frontend = analyze(request, std::move(inputs), progress);
  CompilationResult result;
  if (!frontend.canGenerateCode()) {
    result.status = CompilationStatus::FrontendFailure;
    result.sources = std::move(frontend.sources);
    result.diagnostics = std::move(frontend.diagnostics);
    return result;
  }

  ProgressScope optimizationProgress(progress, ProgressPhase::Optimizing);
  const OptimizationPipeline optimizationPipeline;
  const OptimizationResult optimizations = optimizationPipeline.run(
      frontend.hir, request.optimization(), request.target());
  OptimizedProgram optimizedProgram = optimizationPipeline.run(
      OptimizationRequest{.hir = frontend.hir,
                          .mir = std::move(frontend.mir),
                          .level = request.optimization(),
                          .target = request.target(),
                          .compatibility = &optimizations});
  optimizationProgress.finish();
  if (!optimizedProgram.valid()) {
    result.status = CompilationStatus::MirVerificationFailure;
    result.sources = std::move(frontend.sources);
    result.diagnostics = std::move(frontend.diagnostics);
    result.mirErrors =
        optimizedProgram.report.inputVerification.valid()
            ? std::move(optimizedProgram.report.outputVerification.errors)
            : std::move(optimizedProgram.report.inputVerification.errors);
    return result;
  }

  const OptimizationOutputRequest &optimizationOutputs =
      request.optimizationOutputs();
  if (optimizationOutputs.capturePreMir) {
    result.preOptimizationMir = MirPrinter().print(optimizedProgram.sourceMir);
  }
  if (optimizationOutputs.capturePostMir) {
    result.postOptimizationMir = MirPrinter().print(optimizedProgram.mir);
  }
  if (optimizationOutputs.captureReport) {
    result.optimizationReport = optimizedProgram.report;
  }

  std::string backendName = "unknown";
  try {
    backendName = backend.name();
  } catch (...) {
    // A backend name is presentation metadata, not permission to let a
    // backend exception escape the reusable compilation boundary.
  }
  const auto backendFailure = [&](std::string detail) {
    const SourceUnit *entry = std::as_const(frontend.sourceGraph)
                                  .findUnit(frontend.sourceGraph.entryUnit());
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-B0001", DiagnosticPhase::Backend,
        SourceSpan{entry == nullptr ? request.entry().string()
                                    : entry->path.string(),
                   0, 0, 1},
        "Internal compiler error: backend failed while generating a "
        "representation.");
    diagnostic.hints.emplace_back(
        "Report this as a GTI compiler bug and include this diagnostic, the "
        "GTI version, selected target, optimization level, and a reduced "
        "source input when possible, together with the saved backend "
        "diagnostics.");
    result.status = CompilationStatus::BackendFailure;
    result.backendFailureDetails = "Backend: " + backendName +
                                   "\nStage: representation generation\n" +
                                   "Detail: " + detail + "\n";
    result.sources = std::move(frontend.sources);
    result.diagnostics = std::move(frontend.diagnostics);
    result.diagnostics.emplace_back(std::move(diagnostic));
    return result;
  };
  const auto backendLimitation = [&](std::string detail) {
    const SourceUnit *entry = std::as_const(frontend.sourceGraph)
                                  .findUnit(frontend.sourceGraph.entryUnit());
    Diagnostic diagnostic =
        makeDiagnostic("GTI-B0003", DiagnosticPhase::Backend,
                       SourceSpan{entry == nullptr ? request.entry().string()
                                                   : entry->path.string(),
                                  0, 0, 1},
                       "The selected backend does not yet support this valid "
                       "GTI program: " +
                           std::move(detail));
    result.status = CompilationStatus::BackendFailure;
    result.sources = std::move(frontend.sources);
    result.diagnostics = std::move(frontend.diagnostics);
    result.diagnostics.emplace_back(std::move(diagnostic));
    return result;
  };

  const ProgressScope emissionProgress(progress, ProgressPhase::Emitting,
                                       backendName);
  LoweredProgramBuild loweredBuild = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir,
      optimizedProgram.sourceMir, optimizedProgram.mir,
      optimizedProgram.report.transformations, request.target(),
      request.debugInfo());
  if (!loweredBuild.valid()) {
    const std::string detail =
        loweredBuild.issues.empty()
            ? "unknown lowered-program construction failure"
            : loweredBuild.issues.front().detail;
    return backendFailure("lowered-program construction failed: " + detail);
  }
  LoweredProgram loweredProgram = std::move(*loweredBuild.program);

  try {
    result.artifact = backend.generate(loweredProgram);
  } catch (const BackendUnsupportedError &exception) {
    return backendLimitation(exception.what());
  } catch (const std::exception &exception) {
    return backendFailure(exception.what());
  } catch (...) {
    return backendFailure("a non-standard exception was thrown");
  }
  result.sources = std::move(frontend.sources);
  result.diagnostics = std::move(frontend.diagnostics);
  result.status = CompilationStatus::Success;
  return result;
}

} // namespace

AnalysisConfiguration::AnalysisConfiguration(
    TargetInfo target, ExecutionProfile executionProfile,
    RuntimeProfile runtimeProfile, ConfigurationFlags configurationFlags)
    : targetInfo(std::move(target)), selectedRuntimeProfile(runtimeProfile),
      configuredFlags(std::move(configurationFlags)) {
  targetInfo.executionProfile = executionProfile;
  targetInfo.runtimeCapabilities = {};
  if (selectedRuntimeProfile == RuntimeProfile::Hosted &&
      targetInfo.supported()) {
    targetInfo.runtimeCapabilities.threads = true;
  }
  normalizeConfigurationFlags(configuredFlags);
}

const TargetInfo &AnalysisConfiguration::target() const { return targetInfo; }

RuntimeProfile AnalysisConfiguration::runtimeProfile() const {
  return selectedRuntimeProfile;
}

const ConfigurationFlags &AnalysisConfiguration::configurationFlags() const {
  return configuredFlags;
}

PackageWarningPolicy::PackageWarningPolicy(
    bool dependencyWarnings, std::string selectedPackageIdentity,
    std::vector<std::string> dependencyPackageIdentities)
    : emitDependencyWarnings(dependencyWarnings),
      selectedIdentity(std::move(selectedPackageIdentity)),
      dependencyIdentities(std::move(dependencyPackageIdentities)) {
  std::erase_if(dependencyIdentities, [&](const std::string &identity) {
    return identity.empty() || identity == selectedIdentity;
  });
  std::ranges::sort(dependencyIdentities);
  const auto duplicate = std::ranges::unique(dependencyIdentities).begin();
  dependencyIdentities.erase(duplicate, dependencyIdentities.end());
}

bool PackageWarningPolicy::dependencyWarnings() const {
  return emitDependencyWarnings;
}

const std::string &PackageWarningPolicy::selectedPackage() const {
  return selectedIdentity;
}

const std::vector<std::string> &
PackageWarningPolicy::dependencyPackages() const {
  return dependencyIdentities;
}

bool PackageWarningPolicy::suppresses(const Diagnostic &diagnostic,
                                      const SourceGraph &sources) const {
  if (emitDependencyWarnings || selectedIdentity.empty() ||
      diagnostic.severity != DiagnosticSeverity::Warning) {
    return false;
  }
  const SourceUnit *unit =
      sources.findUnit(sources.sourceUnitForPath(diagnostic.primary.source));
  return unit != nullptr && unit->role == SourceUnitRole::Application &&
         unit->packageIdentity && *unit->packageIdentity != selectedIdentity &&
         std::ranges::binary_search(dependencyIdentities,
                                    *unit->packageIdentity);
}

CompilationRequest::CompilationRequest(
    std::filesystem::path entry, StandardLibraryLayout standardLibrary,
    TargetInfo target, OptimizationLevel optimization, CppStandard cppStandard,
    std::vector<PackageSourceRoot> packageSourceRoots,
    ConfigurationFlags configurationFlags,
    OptimizationOutputRequest optimizationOutputs, WarningPolicy warnings,
    bool debugInfo, PackageWarningPolicy packageWarnings,
    RawPointerAuditMode rawPointerAudit, ProgramRequest program)
    : entryPath(std::move(entry)),
      standardLibraryLayout(std::move(standardLibrary)),
      targetInfo(std::move(target)), optimizationLevel(optimization),
      backendStandard(cppStandard),
      packageSourceRootSet(std::move(packageSourceRoots)),
      configuredFlags(std::move(configurationFlags)),
      requestedOptimizationOutputs(optimizationOutputs),
      requestedProgram(std::move(program)), warningPolicy(std::move(warnings)),
      packageWarningPolicy(std::move(packageWarnings)),
      sourceDebugInfo(debugInfo), rawPointerAuditMode(rawPointerAudit) {
  normalizeConfigurationFlags(configuredFlags);
}

const std::filesystem::path &CompilationRequest::entry() const {
  return entryPath;
}

const StandardLibraryLayout &CompilationRequest::standardLibrary() const {
  return standardLibraryLayout;
}

const TargetInfo &CompilationRequest::target() const { return targetInfo; }

OptimizationLevel CompilationRequest::optimization() const {
  return optimizationLevel;
}

CppStandard CompilationRequest::cppStandard() const { return backendStandard; }

bool CompilationRequest::debugInfo() const { return sourceDebugInfo; }

RawPointerAuditMode CompilationRequest::rawPointerAudit() const {
  return rawPointerAuditMode;
}

const std::vector<PackageSourceRoot> &
CompilationRequest::packageSources() const {
  return packageSourceRootSet;
}

const ConfigurationFlags &CompilationRequest::configurationFlags() const {
  return configuredFlags;
}

const OptimizationOutputRequest &
CompilationRequest::optimizationOutputs() const {
  return requestedOptimizationOutputs;
}

const ProgramRequest &CompilationRequest::program() const {
  return requestedProgram;
}

const WarningPolicy &CompilationRequest::warnings() const {
  return warningPolicy;
}

const PackageWarningPolicy &CompilationRequest::packageWarnings() const {
  return packageWarningPolicy;
}

bool CompilationRequest::requiresEntryPoint() const {
  return entryPointRequired;
}

CompilationRequest CompilationRequest::forExecutable() const {
  CompilationRequest result = *this;
  result.entryPointRequired = requestedProgram.kind == ProgramKind::Executable;
  return result;
}

CompilationInputs loadCompilationInputs(const CompilationRequest &request,
                                        ProgressObserver *progress) {
  SourceLoader sourceLoader;
  CompilationInputs inputs;
  {
    const PhaseTimeScope timeScope("gti-source-load");
    const ProgressScope loadProgress(progress, ProgressPhase::Loading);
    inputs.sourceGraph = sourceLoader.load(
        request.entry(), std::nullopt, {request.standardLibrary().prelude}, {},
        {request.standardLibrary().root}, std::nullopt,
        request.packageSources(), request.target(),
        request.configurationFlags());
    loadProgress.update({}, inputs.sourceGraph.sourceUnits().size(),
                        inputs.sourceGraph.sourceUnits().size());
  }
  inputs.sources = sourceLoader.sources();
  inputs.diagnostics = sourceLoader.errors();
  inputs.sourceValid = !sourceLoader.hadError();
  std::erase_if(inputs.diagnostics, [&](const Diagnostic &diagnostic) {
    return request.warnings().suppresses(diagnostic) ||
           request.packageWarnings().suppresses(diagnostic, inputs.sourceGraph);
  });
  return inputs;
}

CompilationResult compileWithBackend(const CompilationRequest &request,
                                     Backend &backend,
                                     ProgressObserver *progress) {
  return compileWithLoadedInputs(
      request, loadCompilationInputs(request, progress), backend, progress);
}

CheckResult checkCompilation(const CompilationRequest &request,
                             ProgressObserver *progress) {
  FrontendResult frontend =
      analyze(request, loadCompilationInputs(request, progress), progress);
  CheckResult result;
  result.sources = std::move(frontend.sources);
  result.diagnostics = std::move(frontend.diagnostics);
  result.status = frontend.canGenerateCode() ? CheckStatus::Success
                                             : CheckStatus::FrontendFailure;
  return result;
}

CompilationResult compileToCpp(const CompilationRequest &request,
                               ProgressObserver *progress) {
  CppBackend backend(request.cppStandard(), request.debugInfo());
  return compileWithBackend(request, backend, progress);
}

CompilationResult compileToCpp(const CompilationRequest &request,
                               CompilationInputs inputs,
                               ProgressObserver *progress) {
  CppBackend backend(request.cppStandard(), request.debugInfo());
  return compileWithLoadedInputs(request, std::move(inputs), backend, progress);
}

CompilationResult compileToNativeHeader(const CompilationRequest &request,
                                        ProgressObserver *progress) {
  NativeHeaderBackend backend;
  return compileWithBackend(request, backend, progress);
}

CompilationResult compileToMir(const CompilationRequest &request,
                               ProgressObserver *progress) {
  MirBackend backend;
  return compileWithBackend(request, backend, progress);
}

} // namespace lang::driver
