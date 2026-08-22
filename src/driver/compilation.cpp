#include "gti/driver/compilation.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lowered_program.h"
#include "gti/mir_backend.h"
#include "gti/native_header.h"
#include "gti/source_loader.h"
#include "gti/support.h"

#include <exception>
#include <string>
#include <utility>

namespace lang::driver {
namespace {

FrontendResult analyze(const CompilationRequest &request,
                       CompilationInputs inputs) {
  // Compilation and `check` consume diagnostics, semantics, and symbols; only
  // editor position queries read the occurrence table, so it is not built on
  // this path.
  return Frontend({.target = request.target(), .toolingOccurrences = false})
      .analyzeLoaded(request.entry(), std::move(inputs.sourceGraph),
                     std::move(inputs.sources), std::move(inputs.diagnostics),
                     inputs.sourceValid);
}

CompilationResult compileWithBackendInputs(const CompilationRequest &request,
                                           CompilationInputs inputs,
                                           Backend &backend) {
  FrontendResult frontend = analyze(request, std::move(inputs));
  CompilationResult result;
  if (!frontend.canGenerateCode()) {
    result.status = CompilationStatus::FrontendFailure;
    result.sources = std::move(frontend.sources);
    result.diagnostics = std::move(frontend.diagnostics);
    return result;
  }

  const OptimizationPipeline optimizationPipeline;
  const OptimizationResult optimizations = optimizationPipeline.run(
      frontend.hir, request.optimization(), request.target());
  OptimizedProgram optimizedProgram = optimizationPipeline.run(
      OptimizationRequest{.hir = frontend.hir,
                          .mir = std::move(frontend.mir),
                          .level = request.optimization(),
                          .target = request.target(),
                          .compatibility = &optimizations});
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
        "Internal compiler error: backend '" + backendName +
            "' failed while generating an artifact: " + std::move(detail));
    diagnostic.hints.emplace_back(
        "Report this as a GTI compiler bug and include this diagnostic, the "
        "GTI version, selected target, optimization level, and a reduced "
        "source input when possible.");
    result.status = CompilationStatus::BackendFailure;
    result.sources = std::move(frontend.sources);
    result.diagnostics = std::move(frontend.diagnostics);
    result.diagnostics.emplace_back(std::move(diagnostic));
    return result;
  };

  LoweredProgramBuild loweredBuild = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir,
      optimizedProgram.sourceMir, optimizedProgram.mir, request.target());
  if (!loweredBuild.valid()) {
    const std::string detail =
        loweredBuild.issues.empty()
            ? "unknown lowered-program construction failure"
            : loweredBuild.issues.front().detail;
    return backendFailure("lowered-program construction failed: " + detail);
  }
  LoweredProgram loweredProgram = std::move(*loweredBuild.program);

  try {
    result.artifact =
        backend.generate({.program = frontend.program,
                          .semantics = frontend.semantics,
                          .hir = frontend.hir,
                          .mir = optimizedProgram.mir,
                          .sourceMir = &optimizedProgram.sourceMir,
                          .optimizations = optimizations,
                          .target = request.target(),
                          .loweredProgram = &loweredProgram});
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

CompilationRequest::CompilationRequest(
    std::filesystem::path entry, StandardLibraryLayout standardLibrary,
    TargetInfo target, OptimizationLevel optimization, CppStandard cppStandard,
    std::vector<PackageSourceRoot> packageSourceRoots)
    : entryPath(std::move(entry)),
      standardLibraryLayout(std::move(standardLibrary)),
      targetInfo(std::move(target)), optimizationLevel(optimization),
      backendStandard(cppStandard),
      packageSourceRootSet(std::move(packageSourceRoots)) {}

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

const std::vector<PackageSourceRoot> &
CompilationRequest::packageSources() const {
  return packageSourceRootSet;
}

CompilationInputs loadCompilationInputs(const CompilationRequest &request) {
  SourceLoader sourceLoader;
  CompilationInputs inputs;
  {
    const PhaseTimeScope timeScope("gti-source-load");
    inputs.sourceGraph = sourceLoader.load(
        request.entry(), std::nullopt, {request.standardLibrary().prelude}, {},
        {request.standardLibrary().root}, std::nullopt,
        request.packageSources());
  }
  inputs.sources = sourceLoader.sources();
  inputs.diagnostics = sourceLoader.errors();
  inputs.sourceValid = !sourceLoader.hadError();
  return inputs;
}

CompilationResult compileWithBackend(const CompilationRequest &request,
                                     Backend &backend) {
  return compileWithBackendInputs(request, loadCompilationInputs(request),
                                  backend);
}

CheckResult checkCompilation(const CompilationRequest &request) {
  FrontendResult frontend = analyze(request, loadCompilationInputs(request));
  CheckResult result;
  result.sources = std::move(frontend.sources);
  result.diagnostics = std::move(frontend.diagnostics);
  result.status = frontend.canGenerateCode() ? CheckStatus::Success
                                             : CheckStatus::FrontendFailure;
  return result;
}

CompilationResult compileToCpp(const CompilationRequest &request) {
  CppBackend backend(request.cppStandard());
  return compileWithBackend(request, backend);
}

CompilationResult compileToCpp(const CompilationRequest &request,
                               CompilationInputs inputs) {
  CppBackend backend(request.cppStandard());
  return compileWithBackendInputs(request, std::move(inputs), backend);
}

CompilationResult compileToNativeHeader(const CompilationRequest &request) {
  NativeHeaderBackend backend;
  return compileWithBackend(request, backend);
}

CompilationResult compileToMir(const CompilationRequest &request) {
  MirBackend backend;
  return compileWithBackend(request, backend);
}

} // namespace lang::driver
