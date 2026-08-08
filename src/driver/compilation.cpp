#include "gti/driver/compilation.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"

#include <utility>

namespace lang::driver {

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

CompilationResult compileToCpp(const CompilationRequest &request) {
  FrontendResult frontend = Frontend({.target = request.target()})
                                .analyze(request.entry(), std::nullopt,
                                         {request.standardLibrary().prelude},
                                         {}, {request.standardLibrary().root});

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
                          .target = request.target()});
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

  CppBackend backend(request.cppStandard());
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

} // namespace lang::driver
