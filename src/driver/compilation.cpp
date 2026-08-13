#include "gti/driver/compilation.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/native_header.h"

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

CompilationResult compileWithBackend(const CompilationRequest &request,
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

  result.artifact = backend.generate({.program = frontend.program,
                                      .semantics = frontend.semantics,
                                      .hir = frontend.hir,
                                      .mir = optimizedProgram.mir,
                                      .optimizations = optimizations,
                                      .target = request.target()});
  result.sources = std::move(frontend.sources);
  result.diagnostics = std::move(frontend.diagnostics);
  result.status = CompilationStatus::Success;
  return result;
}

} // namespace

CompilationRequest::CompilationRequest(std::filesystem::path entry,
                                       StandardLibraryLayout standardLibrary,
                                       TargetInfo target,
                                       OptimizationLevel optimization,
                                       CppStandard cppStandard)
    : entryPath(std::move(entry)),
      standardLibraryLayout(std::move(standardLibrary)),
      targetInfo(std::move(target)), optimizationLevel(optimization),
      backendStandard(cppStandard) {}

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

CompilationInputs loadCompilationInputs(const CompilationRequest &request) {
  SourceLoader sourceLoader;
  CompilationInputs inputs;
  {
    const PhaseTimeScope timeScope("gti-source-load");
    inputs.sourceGraph = sourceLoader.load(
        request.entry(), std::nullopt, {request.standardLibrary().prelude}, {},
        {request.standardLibrary().root});
  }
  inputs.sources = sourceLoader.sources();
  inputs.diagnostics = sourceLoader.errors();
  inputs.sourceValid = !sourceLoader.hadError();
  return inputs;
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
  return compileToCpp(request, loadCompilationInputs(request));
}

CompilationResult compileToCpp(const CompilationRequest &request,
                               CompilationInputs inputs) {
  CppBackend backend(request.cppStandard());
  return compileWithBackend(request, std::move(inputs), backend);
}

CompilationResult compileToNativeHeader(const CompilationRequest &request) {
  NativeHeaderBackend backend;
  return compileWithBackend(request, loadCompilationInputs(request), backend);
}

} // namespace lang::driver
