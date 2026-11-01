#include "gti/driver/compilation.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/llvm_backend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_backend.h"
#include "gti/mir_printer.h"
#include "gti/native_header.h"
#include "gti/source_loader.h"
#include "gti/support.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace lang::driver {
namespace {

// `artifact` is set when the result is compiled. `check` reports every
// declaration's diagnostics, so it lowers library declarations completely.
FrontendResult analyze(const CompilationRequest &request,
                       CompilationInputs inputs, ProgressObserver *progress,
                       bool artifact) {
  // Compilation and `check` consume diagnostics, semantics, and symbols; only
  // editor position queries read the occurrence table, so it is not built on
  // this path.
  FrontendOptions options{
      .target = request.target(),
      .configurationFlags = request.configurationFlags(),
      .declaredConfigurationFlags = request.declaredConfigurationFlags(),
      .toolingOccurrences = false,
      .requireEntryPoint = request.requiresEntryPoint(),
      .requireFunctionDefinitions = request.requiresFunctionDefinitions(),
      .progress = progress};
  options.rawPointerAudit = request.rawPointerAudit();
  options.reportLibraryAuditFindings = request.rawPointerAuditLibraryFindings();
  options.program = request.program();
  options.sourceDebugProfile = request.debugInfo()
                                   ? SourceDebugProfile::LexicalV1
                                   : SourceDebugProfile::None;
  // Artifact analysis retains complete application diagnostics while
  // discovering compiler-trusted bodies only through application use.
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = artifact;
  options.retainApplicationBodies = artifact && request.applicationBodies();
  const auto finish = [&](FrontendResult result) {
    addIncludeSuggestions(
        result, options, request.entry(), {request.standardLibrary().prelude},
        {}, {request.standardLibrary().root}, request.packageSources());
    std::erase_if(result.diagnostics, [&](const Diagnostic &diagnostic) {
      return request.warnings().suppresses(diagnostic) ||
             request.packageWarnings().suppresses(diagnostic,
                                                  result.sourceGraph);
    });
    return result;
  };
  std::optional<std::vector<Diagnostic>> auditDiagnostics;
  if (artifact && options.rawPointerAudit != RawPointerAuditMode::Off) {
    // Auditing retains complete trusted-body coverage, including concrete
    // generic findings. Its model must not select artifact identities or code:
    // even macro isolation must be independent of optional audit policy.
    auto auditOptions = options;
    auditOptions.sourceDebugProfile = SourceDebugProfile::None;
    FrontendResult audited =
        Frontend(auditOptions)
            .analyzeLoaded(request.entry(), inputs.sourceGraph, inputs.sources,
                           inputs.diagnostics, inputs.sourceValid);
    if (!audited.canGenerateCode())
      return finish(std::move(audited));
    auditDiagnostics = std::move(audited.diagnostics);
  }
  if (artifact) {
    options.trustedBodies = TrustedBodyAnalysis::Reachable;
    options.rawPointerAudit = RawPointerAuditMode::Off;
    options.reportLibraryAuditFindings = false;
  }
  FrontendResult result = Frontend(options).analyzeLoaded(
      request.entry(), std::move(inputs.sourceGraph), std::move(inputs.sources),
      std::move(inputs.diagnostics), inputs.sourceValid);
  if (auditDiagnostics && result.canGenerateCode())
    result.diagnostics = std::move(*auditDiagnostics);
  return finish(std::move(result));
}

CompilationResult compileWithLoadedInputs(const CompilationRequest &request,
                                          CompilationInputs inputs,
                                          Backend &backend,
                                          ProgressObserver *progress) {
  FrontendResult frontend = analyze(request, std::move(inputs), progress, true);
  CompilationResult result;
  result.testCases = std::move(frontend.testCases);
  if (!frontend.canGenerateCode()) {
    result.status = CompilationStatus::FrontendFailure;
    result.sources = std::move(frontend.sources);
    result.diagnostics = std::move(frontend.diagnostics);
    return result;
  }

  ProgressScope optimizationProgress(progress, ProgressPhase::Optimizing);
  const OptimizationPipeline optimizationPipeline;
  OptimizedProgram optimizedProgram = [&] {
    const PhaseTimeScope timeScope("gti-optimize");
    const OptimizationResult optimizations = optimizationPipeline.run(
        frontend.hir, request.optimization(), request.target());
    return optimizationPipeline.run(
        OptimizationRequest{.hir = frontend.hir,
                            .mir = std::move(frontend.mir),
                            .level = request.optimization(),
                            .target = request.target(),
                            .compatibility = &optimizations});
  }();
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
  const auto backendLimitation = [&](const BackendUnsupportedError &error) {
    const SourceUnit *entry = std::as_const(frontend.sourceGraph)
                                  .findUnit(frontend.sourceGraph.entryUnit());
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-B0003", DiagnosticPhase::Backend,
        error.location().value_or(SourceSpan{
            entry == nullptr ? request.entry().string() : entry->path.string(),
            0, 0, 1}),
        "The selected backend does not yet support this valid "
        "GTI program: " +
            std::string(error.what()));
    result.status = CompilationStatus::BackendFailure;
    result.sources = std::move(frontend.sources);
    result.diagnostics = std::move(frontend.diagnostics);
    result.diagnostics.emplace_back(std::move(diagnostic));
    return result;
  };

  const ProgressScope emissionProgress(progress, ProgressPhase::Emitting,
                                       backendName);
  LoweredProgramBuild loweredBuild = [&] {
    const PhaseTimeScope timeScope("gti-lowered-program");
    // The pipeline verified its input and the coherence of its output on
    // exactly these programs and records; the builder need not repeat them.
    const OptimizationReport &report = optimizedProgram.report;
    const LoweredProgramVerifiedInputs verified{
        .sourceMirVerified =
            report.verificationEnabled && report.inputVerification.valid(),
        .optimizationCoherenceVerified =
            report.verificationEnabled && report.outputVerification.valid()};
    return LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir,
        optimizedProgram.sourceMir, optimizedProgram.mir,
        optimizedProgram.report.transformations, request.target(), verified);
  }();
  if (!loweredBuild.valid()) {
    const std::string detail =
        loweredBuild.issues.empty()
            ? "unknown lowered-program construction failure"
            : loweredBuild.issues.front().detail;
    return backendFailure("lowered-program construction failed: " + detail);
  }
  LoweredProgram loweredProgram = std::move(*loweredBuild.program);

  try {
    const PhaseTimeScope timeScope("gti-backend");
    result.artifact = backend.generate(std::move(loweredProgram));
  } catch (const BackendUnsupportedError &exception) {
    return backendLimitation(exception);
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
    RuntimeProfile runtimeProfile, ConfigurationFlags configurationFlags,
    ConfigurationFlags declaredConfigurationFlags)
    : targetInfo(std::move(target)), selectedRuntimeProfile(runtimeProfile),
      configuredFlags(std::move(configurationFlags)),
      declaredFlags(std::move(declaredConfigurationFlags)) {
  targetInfo.executionProfile = executionProfile;
  targetInfo.runtimeCapabilities = {};
  if (selectedRuntimeProfile == RuntimeProfile::Hosted &&
      targetInfo.supported()) {
    targetInfo.runtimeCapabilities.threads = true;
  }
  normalizeConfigurationFlags(configuredFlags);
  normalizeConfigurationFlags(declaredFlags);
}

const TargetInfo &AnalysisConfiguration::target() const { return targetInfo; }

RuntimeProfile AnalysisConfiguration::runtimeProfile() const {
  return selectedRuntimeProfile;
}

const ConfigurationFlags &AnalysisConfiguration::configurationFlags() const {
  return configuredFlags;
}

const ConfigurationFlags &
AnalysisConfiguration::declaredConfigurationFlags() const {
  return declaredFlags;
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
    RawPointerAuditMode rawPointerAudit, ProgramRequest program,
    ConfigurationFlags declaredConfigurationFlags,
    bool rawPointerAuditLibraryFindings)
    : entryPath(std::move(entry)),
      standardLibraryLayout(std::move(standardLibrary)),
      targetInfo(std::move(target)), optimizationLevel(optimization),
      backendStandard(cppStandard),
      packageSourceRootSet(std::move(packageSourceRoots)),
      configuredFlags(std::move(configurationFlags)),
      declaredFlags(std::move(declaredConfigurationFlags)),
      requestedOptimizationOutputs(optimizationOutputs),
      requestedProgram(std::move(program)), warningPolicy(std::move(warnings)),
      packageWarningPolicy(std::move(packageWarnings)),
      sourceDebugInfo(debugInfo), rawPointerAuditMode(rawPointerAudit),
      reportLibraryAuditFindings(rawPointerAuditLibraryFindings) {
  normalizeConfigurationFlags(configuredFlags);
  normalizeConfigurationFlags(declaredFlags);
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

BackendKind CompilationRequest::backend() const { return selectedBackend; }

bool CompilationRequest::debugInfo() const { return sourceDebugInfo; }

LlvmInstrumentationMode CompilationRequest::llvmInstrumentation() const {
  return instrumentationMode;
}

RawPointerAuditMode CompilationRequest::rawPointerAudit() const {
  return rawPointerAuditMode;
}

bool CompilationRequest::rawPointerAuditLibraryFindings() const {
  return reportLibraryAuditFindings;
}

const std::vector<PackageSourceRoot> &
CompilationRequest::packageSources() const {
  return packageSourceRootSet;
}

const ConfigurationFlags &CompilationRequest::configurationFlags() const {
  return configuredFlags;
}

const ConfigurationFlags &
CompilationRequest::declaredConfigurationFlags() const {
  return declaredFlags;
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

bool CompilationRequest::requiresFunctionDefinitions() const {
  return functionDefinitionsRequired;
}

CompilationRequest CompilationRequest::withDefinitionClosure() const {
  CompilationRequest result = *this;
  result.functionDefinitionsRequired = true;
  return result;
}

CompilationRequest CompilationRequest::withBackend(BackendKind backend) const {
  if (backend != BackendKind::Llvm &&
      instrumentationMode != LlvmInstrumentationMode::None)
    throw std::invalid_argument(
        "LLVM instrumentation requires the LLVM backend");
  CompilationRequest result = *this;
  result.selectedBackend = backend;
  return result;
}

CompilationRequest CompilationRequest::withLlvmInstrumentation(
    LlvmInstrumentationMode mode) const {
  static_cast<void>(llvmInstrumentationModeName(mode));
  if (mode != LlvmInstrumentationMode::None &&
      selectedBackend != BackendKind::Llvm)
    throw std::invalid_argument(
        "LLVM instrumentation requires the LLVM backend");
  CompilationRequest result = *this;
  result.instrumentationMode = mode;
  return result;
}

CompilationInputs loadCompilationInputs(
    const CompilationRequest &request, ProgressObserver *progress,
    const std::unordered_map<std::string, std::string> &overrides) {
  SourceLoader sourceLoader;
  CompilationInputs inputs;
  std::vector<std::filesystem::path> preludes{
      request.standardLibrary().prelude};
  if (request.program().testSuite) {
    preludes.push_back(request.standardLibrary().root / "std/string.gti");
    preludes.push_back(request.standardLibrary().root / "std/vector.gti");
  }
  {
    const PhaseTimeScope timeScope("gti-source-load");
    const ProgressScope loadProgress(progress, ProgressPhase::Loading);
    inputs.sourceGraph = sourceLoader.load(
        request.entry(), std::nullopt, preludes, overrides,
        {request.standardLibrary().root}, std::nullopt,
        request.packageSources(), request.target(),
        request.configurationFlags(), request.declaredConfigurationFlags());
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

CompilationResult compileWithBackend(const CompilationRequest &request,
                                     CompilationInputs inputs, Backend &backend,
                                     ProgressObserver *progress) {
  return compileWithLoadedInputs(request, std::move(inputs), backend, progress);
}

CheckResult checkCompilation(const CompilationRequest &request,
                             ProgressObserver *progress) {
  FrontendResult frontend = analyze(
      request, loadCompilationInputs(request, progress), progress, false);
  CheckResult result;
  result.sources = std::move(frontend.sources);
  result.diagnostics = std::move(frontend.diagnostics);
  result.status = frontend.canGenerateCode() ? CheckStatus::Success
                                             : CheckStatus::FrontendFailure;
  return result;
}

CompilationResult compileToCpp(const CompilationRequest &request,
                               ProgressObserver *progress) {
  if (request.llvmInstrumentation() != LlvmInstrumentationMode::None)
    throw std::invalid_argument(
        "LLVM instrumentation requires the LLVM backend");
  CppBackend backend(request.cppStandard(), request.debugInfo());
  return compileWithBackend(request.withDefinitionClosure(), backend, progress);
}

namespace {

LlvmCodegenOptimization
llvmCodegenOptimization(OptimizationLevel optimization) {
  switch (optimization) {
  case OptimizationLevel::O0:
    return LlvmCodegenOptimization::None;
  case OptimizationLevel::O1:
    return LlvmCodegenOptimization::Less;
  case OptimizationLevel::O2:
    return LlvmCodegenOptimization::Default;
  case OptimizationLevel::O3:
    return LlvmCodegenOptimization::Aggressive;
  }
  throw std::logic_error("unknown optimization level");
}

} // namespace

LlvmBackendOptions llvmBackendOptions(const CompilationRequest &request) {
  return {.optimization = llvmCodegenOptimization(request.optimization()),
          .debugInfo = request.debugInfo(),
          .instrumentation = request.llvmInstrumentation()};
}

namespace {

std::unique_ptr<Backend> selectedBackend(const CompilationRequest &request) {
  switch (request.backend()) {
  case BackendKind::Cpp:
    return std::make_unique<CppBackend>(request.cppStandard(),
                                        request.debugInfo());
  case BackendKind::Llvm:
    return std::make_unique<LlvmBackend>(llvmBackendOptions(request));
  }
  throw std::logic_error("unknown executable backend selection");
}

} // namespace

CompilationResult compileToSelectedBackend(const CompilationRequest &request,
                                           ProgressObserver *progress) {
  std::unique_ptr<Backend> backend = selectedBackend(request);
  return compileWithBackend(request.withDefinitionClosure(), *backend,
                            progress);
}

CompilationResult compileToSelectedBackend(const CompilationRequest &request,
                                           CompilationInputs inputs,
                                           ProgressObserver *progress) {
  std::unique_ptr<Backend> backend = selectedBackend(request);
  return compileWithLoadedInputs(request.withDefinitionClosure(),
                                 std::move(inputs), *backend, progress);
}

CompilationResult compileToCpp(const CompilationRequest &request,
                               CompilationInputs inputs,
                               ProgressObserver *progress) {
  if (request.llvmInstrumentation() != LlvmInstrumentationMode::None)
    throw std::invalid_argument(
        "LLVM instrumentation requires the LLVM backend");
  CppBackend backend(request.cppStandard(), request.debugInfo());
  return compileWithLoadedInputs(request.withDefinitionClosure(),
                                 std::move(inputs), backend, progress);
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
