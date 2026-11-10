#include "../src/driver/build_source_identity.h"
#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/build_report.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"
#include "gti/lowered_program.h"
#include "gti/support.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "driver_test_support.h"

namespace {

using namespace gti_test::driver_support;

void testAnalysisConfigurationResolution() {
  const lang::TargetInfo host = lang::TargetInfo::host();
  expect(!host.runtimeCapabilities.threads,
         "raw target values should default to no runtime capabilities");

  const lang::driver::AnalysisConfiguration safeDefault(host);
  expect(safeDefault.target().executionProfile ==
                 lang::ExecutionProfile::SingleThreaded &&
             !safeDefault.target().runtimeCapabilities.threads &&
             safeDefault.runtimeProfile() == lang::driver::RuntimeProfile::None,
         "analysis configuration should fail closed without an explicit "
         "runtime profile");

  const lang::driver::AnalysisConfiguration unavailableConcurrent(
      host, lang::ExecutionProfile::Concurrent);
  expect(unavailableConcurrent.target().executionProfile ==
                 lang::ExecutionProfile::Concurrent &&
             !unavailableConcurrent.target().runtimeCapabilities.threads,
         "a concurrent execution profile must not synthesize runtime thread "
         "support");

  const lang::driver::AnalysisConfiguration hostedConcurrent(
      host, lang::ExecutionProfile::Concurrent,
      lang::driver::RuntimeProfile::Hosted, {"ZETA", "ALPHA", "ZETA"});
  expect(hostedConcurrent.target().runtimeCapabilities.threads &&
             hostedConcurrent.configurationFlags() ==
                 lang::ConfigurationFlags({"ALPHA", "ZETA"}),
         "the supported hosted runtime profile should derive threads and "
         "normalize frontend flags once");

  const lang::driver::AnalysisConfiguration unsupportedHosted(
      lang::TargetInfo{
          .os = "unsupported", .vendor = "unknown", .arch = "x86_64"},
      lang::ExecutionProfile::Concurrent, lang::driver::RuntimeProfile::Hosted);
  expect(!unsupportedHosted.target().runtimeCapabilities.threads,
         "the hosted runtime profile must not advertise threads for an "
         "unsupported target");
}

void testCompilationRequestAndTargetPropagation() {
  TemporaryDirectory temporary;
  const std::filesystem::path prelude = temporary.root() / "prelude.gti";
  const std::filesystem::path source = temporary.root() / "main.gti";
  expect(writeFile(prelude, ""), "the driver test prelude should be writable");
  expect(writeFile(source, R"(#if target.os == "linux"
int selected() { return 41; }
#else
int selected() { return 82; }
#endif
int main() {
  bool folded = 1 < 2;
  if (!folded) { return 1; }
  return selected() - 41;
}
)"),
         "the driver test source should be writable");

  expect(lang::defaultBackend(
             {.os = "macos", .vendor = "apple", .arch = "arm64"}) ==
                 lang::BackendKind::Llvm &&
             lang::defaultBackend(
                 {.os = "macos", .vendor = "apple", .arch = "x86_64"}) ==
                 lang::BackendKind::Cpp &&
             lang::defaultBackend(
                 {.os = "linux", .vendor = "unknown", .arch = "x86_64"}) ==
                 lang::BackendKind::Llvm &&
             lang::defaultBackend(
                 {.os = "linux", .vendor = "unknown", .arch = "arm64"}) ==
                 lang::BackendKind::Cpp,
         "LLVM is the default backend on the promoted macOS arm64 and Linux "
         "x86_64 targets only");
  const lang::TargetInfo target{
      .os = "linux", .vendor = "unknown", .arch = "x86_64"};
  const lang::driver::CompilationRequest request(
      source, lang::standardLibraryLayout(temporary.root()), target,
      lang::OptimizationLevel::O1, lang::CppStandard::Cpp23);
  expect(request.entry() == source &&
             request.standardLibrary().prelude == prelude,
         "compilation requests should retain resolved source and stdlib paths");
  expect(request.target().os == "linux" &&
             request.optimization() == lang::OptimizationLevel::O1 &&
             request.cppStandard() == lang::CppStandard::Cpp23 &&
             !request.optimizationOutputs().requested(),
         "compilation requests should retain target and backend policy");

  // The resolved target reaches the frontend conditionals: the verified MIR
  // for the Linux request keeps only the Linux definition's constant.
  const lang::driver::CompilationResult mir =
      lang::driver::compileToMir(request);
  expect(mir.succeeded() && mir.artifact &&
             mir.artifact->contents.find("literal=u64:41") !=
                 std::string::npos &&
             mir.artifact->contents.find("literal=u64:82") == std::string::npos,
         "one resolved target must reach frontend conditionals and the "
         "verified MIR the backends consume");

  const lang::BackendKind requested = request.backend();
  const lang::driver::CompilationRequest llvmRequest =
      request.withBackend(lang::BackendKind::Llvm);
  const lang::driver::CompilationResult llvmResult =
      lang::driver::compileToSelectedBackend(llvmRequest);
  const auto llvmDiagnostic =
      std::find_if(llvmResult.diagnostics.begin(), llvmResult.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-B0003";
                   });
  const auto host = lang::TargetInfo::host();
  const bool hostTarget =
      target.os == host.os && target.vendor == host.vendor &&
      target.arch == host.arch && target.dataLayout == host.dataLayout;
  expect(request.backend() == requested &&
             llvmRequest.backend() == lang::BackendKind::Llvm,
         "executable backend selection remains immutable");
  if (hostTarget)
    expect(llvmResult.succeeded() && llvmResult.artifact &&
               llvmResult.artifact->kind == lang::BackendArtifactKind::Object,
           "host scalar compilation returns a native object");
  else
    expect(!llvmResult.succeeded() && !llvmResult.artifact &&
               llvmDiagnostic != llvmResult.diagnostics.end() &&
               llvmDiagnostic->message.find(
                   "currently accepts only the host target") !=
                   std::string::npos,
           "foreign target selection remains a structured limitation");

  // Compilation telemetry and its optional outputs, through the shipped
  // backend on the host target.
  const lang::driver::CompilationRequest hostRequest =
      lang::driver::CompilationRequest(
          source, lang::standardLibraryLayout(temporary.root()), host,
          lang::OptimizationLevel::O1, lang::CppStandard::Cpp23)
          .withBackend(lang::BackendKind::Llvm);
  const lang::driver::CompilationResult result =
      lang::driver::compileToSelectedBackend(hostRequest);
  expect(result.succeeded() && result.artifact &&
             result.artifact->kind == lang::BackendArtifactKind::Object,
         "the driver should compile a valid whole-program request");
  RecordingProgress progress;
  const auto withProgress =
      lang::driver::compileToSelectedBackend(hostRequest, &progress);
  expect(withProgress.succeeded() && result.artifact && withProgress.artifact &&
             withProgress.artifact->contents == result.artifact->contents,
         "observing compilation must preserve byte-identical artifacts");
  expect(
      progress.phases ==
              std::vector<lang::ProgressPhase>{lang::ProgressPhase::Loading,
                                               lang::ProgressPhase::Parsing,
                                               lang::ProgressPhase::Checking,
                                               lang::ProgressPhase::HirLowering,
                                               lang::ProgressPhase::MirLowering,
                                               lang::ProgressPhase::Optimizing,
                                               lang::ProgressPhase::Emitting} &&
          progress.balanced && progress.active.empty() &&
          progress.parsed == 2 && progress.total == 2 && progress.loaded == 2,
      "live telemetry must follow the canonical phase order and exact units");
  const auto broken = temporary.root() / "broken.gti";
  expect(writeFile(broken, "int main() { return missing; }\n"),
         "the failed progress fixture should be writable");
  RecordingProgress failedProgress;
  const auto failed = lang::driver::compileToSelectedBackend(
      lang::driver::CompilationRequest(broken, hostRequest.standardLibrary(),
                                       host, hostRequest.optimization(),
                                       hostRequest.cppStandard())
          .withBackend(lang::BackendKind::Llvm),
      &failedProgress);
  expect(!failed.succeeded() && failedProgress.balanced &&
             failedProgress.active.empty() &&
             failedProgress.phases ==
                 std::vector<lang::ProgressPhase>{
                     lang::ProgressPhase::Loading, lang::ProgressPhase::Parsing,
                     lang::ProgressPhase::Checking},
         "semantic failure must end telemetry without claiming later phases");
  expect(!result.preOptimizationMir && !result.postOptimizationMir &&
             !result.optimizationReport,
         "ordinary driver compilation should not capture optimizer output");

  const lang::driver::CompilationRequest observedRequest =
      lang::driver::CompilationRequest(
          source, lang::standardLibraryLayout(temporary.root()), host,
          lang::OptimizationLevel::O1, lang::CppStandard::Cpp23, {}, {},
          {.capturePreMir = true,
           .capturePostMir = true,
           .captureReport = true})
          .withBackend(lang::BackendKind::Llvm);
  const lang::driver::CompilationResult observed =
      lang::driver::compileToSelectedBackend(observedRequest);
  expect(observed.succeeded() && observed.preOptimizationMir &&
             observed.postOptimizationMir && observed.optimizationReport &&
             *observed.preOptimizationMir != *observed.postOptimizationMir &&
             observed.optimizationReport->passes.size() == 4,
         "the reusable driver should expose opt-in pre/post MIR and the "
         "verified optimizer report");

  const lang::driver::CheckResult checked =
      lang::driver::checkCompilation(request);
  expect(checked.succeeded() && checked.diagnostics.empty(),
         "driver checks should stop successfully after the shared frontend");

  const std::filesystem::path invalidSource = temporary.root() / "invalid.gti";
  expect(writeFile(invalidSource, "int main( { return 0; }\n"),
         "the invalid driver fixture should be writable");
  const lang::driver::CompilationResult invalid =
      lang::driver::compileToSelectedBackend(
          lang::driver::CompilationRequest(
              invalidSource, lang::standardLibraryLayout(temporary.root()),
              host, lang::OptimizationLevel::O0, lang::CppStandard::Cpp23)
              .withBackend(lang::BackendKind::Llvm));
  expect(!invalid.succeeded() &&
             invalid.status ==
                 lang::driver::CompilationStatus::FrontendFailure &&
             !invalid.diagnostics.empty() &&
             invalid.sources.find(invalid.diagnostics.front().primary.source) !=
                 nullptr,
         "frontend failures should remain structured for CLI presentation");
  const lang::driver::CheckResult invalidCheck =
      lang::driver::checkCompilation(lang::driver::CompilationRequest(
          invalidSource, lang::standardLibraryLayout(temporary.root()), target,
          lang::OptimizationLevel::O3, lang::CppStandard::Cpp20));
  expect(!invalidCheck.succeeded() && !invalidCheck.diagnostics.empty(),
         "driver checks should preserve structured frontend failures without "
         "entering a backend");
}

void testDefinitionClosureWithoutEntryPoint() {
  TemporaryDirectory temporary;
  const auto source = temporary.root() / "fragment.gti";
  expect(writeFile(temporary.root() / "prelude.gti", ""),
         "definition closure prelude is writable");
  const std::string missing =
      "int missing(int value);\nint use() { return missing(4); }\n";
  expect(writeFile(source, missing), "definition closure source is writable");
  const lang::driver::CompilationRequest fragment =
      lang::driver::CompilationRequest(
          source, lang::standardLibraryLayout(temporary.root()),
          lang::TargetInfo::host(), lang::OptimizationLevel::O0,
          lang::CppStandard::Cpp23)
          .withBackend(lang::BackendKind::Llvm);
  const auto closed = fragment.withDefinitionClosure();
  expect(!fragment.requiresFunctionDefinitions() &&
             closed.requiresFunctionDefinitions() &&
             !closed.requiresEntryPoint(),
         "definition closure is immutable and independent of hosted entry");
  expect(lang::driver::checkCompilation(fragment).succeeded(),
         "open fragment analysis may resolve a bodyless signature");
  expect(lang::driver::compileToMir(fragment).succeeded(),
         "MIR inspection does not claim an executable definition closure");
  const auto checked = lang::driver::checkCompilation(closed);
  expect(!checked.succeeded(), "explicit closure rejects a missing definition");
  for (bool loaded : {false, true}) {
    const auto result =
        loaded ? lang::driver::compileToSelectedBackend(
                     fragment, lang::driver::loadCompilationInputs(fragment))
               : lang::driver::compileToSelectedBackend(fragment);
    expect(result.status == lang::driver::CompilationStatus::FrontendFailure &&
               !result.artifact && result.diagnostics.size() == 1 &&
               result.diagnostics.front().code == "GTI-S2106" &&
               result.diagnostics.front().primary.start ==
                   missing.find("missing(4)"),
           "both backend entry paths reject the selected call before backend "
           "work");
  }
  expect(writeFile(source,
                   "int unused(int value);\nint answer() { return 42; }\n"),
         "unused declaration control is writable");
  expect(lang::driver::compileToSelectedBackend(fragment).succeeded(),
         "backend emission permits unused declarations and needs no main");
  expect(writeFile(source, "extern \"C\" { int abs(int value); }\n"
                           "int answer() { return abs(-7); }\n"),
         "foreign definition control is writable");
  expect(lang::driver::compileToSelectedBackend(fragment).succeeded(),
         "native linkage retains its independent definition contract");
}

void testPackageWarningPolicy() {
  TemporaryDirectory temporary;
  const std::filesystem::path app = temporary.root() / "app";
  const std::filesystem::path library = temporary.root() / "library";
  const std::filesystem::path leaf = temporary.root() / "leaf";
  const std::filesystem::path source = app / "main.gti";
  const std::filesystem::path own = app / "build/gti/deps/not-a-package.gti";
  const std::filesystem::path librarySource = library / "value.gti";
  const std::filesystem::path leafSource = leaf / "value.gti";
  std::filesystem::create_directories(own.parent_path());
  std::filesystem::create_directories(library);
  std::filesystem::create_directories(leaf);
  expect(writeFile(temporary.root() / "prelude.gti", "") &&
             writeFile(source,
                       "import library.value;\n"
                       "import \"build/gti/deps/not-a-package.gti\";\n"
                       "int main() { mut int value = 0; "
                       "return library::value() + own::value() + value; "
                       "}\n") &&
             writeFile(own, "export module own;\nexport namespace own { int "
                            "value() { mut int value = 0; "
                            "return value; } }\n") &&
             writeFile(librarySource, "export module value;\nimport "
                                      "leaf.value;\nexport namespace library { "
                                      "int value() { mut int value = 0; "
                                      "return leaf::value() + value; } }\n") &&
             writeFile(leafSource, "export module value;\nexport namespace "
                                   "leaf { int value() { mut int value = 0; "
                                   "return value; } }\n"),
         "package-warning source fixtures should be writable");
  const std::vector<lang::PackageSourceRoot> roots{
      {.identity = "app@1.0.0",
       .name = "app",
       .packageRoot = app,
       .sourceRoot = app,
       .dependencies = {{"library", "library@1.0.0"}}},
      {.identity = "library@1.0.0",
       .name = "library",
       .packageRoot = library,
       .sourceRoot = library,
       .dependencies = {{"leaf", "leaf@1.0.0"}},
       .modules = {{"value", librarySource}}},
      {.identity = "leaf@1.0.0",
       .name = "leaf",
       .packageRoot = leaf,
       .sourceRoot = leaf,
       .dependencies = {},
       .modules = {{"value", leafSource}}}};
  const lang::driver::PackageWarningPolicy hidden(
      false, "app@1.0.0", {"library@1.0.0", "leaf@1.0.0", "app@1.0.0"});
  const auto request = [&](lang::driver::PackageWarningPolicy policy = {},
                           lang::WarningPolicy warnings = {}) {
    return lang::driver::CompilationRequest(
        source, lang::standardLibraryLayout(temporary.root()),
        lang::TargetInfo::host(), lang::OptimizationLevel::O0,
        lang::CppStandard::Cpp23, roots, {}, {}, std::move(warnings), false,
        std::move(policy));
  };
  const lang::driver::CheckResult visible =
      lang::driver::checkCompilation(request());
  expect(visible.succeeded() && visible.diagnostics.size() == 4 &&
             std::all_of(visible.diagnostics.begin(), visible.diagnostics.end(),
                         [](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == "GTI-S2096";
                         }),
         "dependency and selected-package warnings should remain enabled "
         "by default");
  const lang::driver::CheckResult suppressed =
      lang::driver::checkCompilation(request(hidden));
  expect(
      suppressed.succeeded() && suppressed.diagnostics.size() == 2 &&
          std::all_of(
              suppressed.diagnostics.begin(), suppressed.diagnostics.end(),
              [&](const lang::Diagnostic &diagnostic) {
                return diagnostic.primary.source ==
                           std::filesystem::canonical(source).string() ||
                       diagnostic.primary.source ==
                           std::filesystem::canonical(own).string();
              }),
      "graph-owned direct/transitive dependency warnings should be hidden "
      "without classifying a consumer's build/gti/deps spelling as a package");
  const lang::driver::CheckResult grouped = lang::driver::checkCompilation(
      request(hidden, lang::WarningPolicy({"unused-mut"})));
  expect(grouped.succeeded() && grouped.diagnostics.empty(),
         "package suppression should compose with existing warning groups");

  const lang::driver::CompilationInputs inputs =
      lang::driver::loadCompilationInputs(request(hidden));
  lang::Diagnostic diagnostic = lang::makeDiagnostic(
      "GTI-S2096", lang::DiagnosticPhase::Semantics,
      {std::filesystem::canonical(librarySource).string(), 0, 1, 1}, "warning");
  diagnostic.severity = lang::DiagnosticSeverity::Warning;
  expect(hidden.suppresses(diagnostic, inputs.sourceGraph),
         "dependency warning ownership should come from the primary source");
  diagnostic.severity = lang::DiagnosticSeverity::Error;
  expect(!hidden.suppresses(diagnostic, inputs.sourceGraph),
         "dependency errors must never be suppressed");
  diagnostic.severity = lang::DiagnosticSeverity::Warning;
  diagnostic.related.push_back(
      {{std::filesystem::canonical(librarySource).string(), 0, 1, 1},
       "dependency declaration"});
  diagnostic.primary.source = std::filesystem::canonical(source).string();
  expect(!hidden.suppresses(diagnostic, inputs.sourceGraph),
         "a dependency-related note cannot hide a consumer-primary diagnostic");
  diagnostic.primary.source =
      std::filesystem::canonical(temporary.root() / "prelude.gti").string();
  expect(!hidden.suppresses(diagnostic, inputs.sourceGraph),
         "stdlib/prelude warnings are outside dependency-package suppression");
  diagnostic.primary.source = (app / "build/gti/deps/unloaded.gti").string();
  expect(!hidden.suppresses(diagnostic, inputs.sourceGraph),
         "unloaded/unowned paths must remain visible despite directory names");

  expect(writeFile(librarySource,
                   "export module value;\nexport namespace library "
                   "{ int value() { return missing; } }\n"),
         "the dependency-error fixture should be writable");
  const lang::driver::CheckResult error =
      lang::driver::checkCompilation(request(hidden));
  expect(!error.succeeded() && lang::hasErrorDiagnostics(error.diagnostics),
         "real dependency errors must retain the frontend failure gate");
}

void testBackendExceptionBoundary() {
  TemporaryDirectory temporary;
  const std::filesystem::path prelude = temporary.root() / "prelude.gti";
  const std::filesystem::path source = temporary.root() / "main.gti";
  expect(writeFile(prelude, ""),
         "the backend-failure test prelude should be writable");
  expect(writeFile(source, "int main() { return 0; }\n"),
         "the backend-failure test source should be writable");

  const lang::driver::CompilationRequest request(
      source, lang::standardLibraryLayout(temporary.root()),
      lang::TargetInfo::host(), lang::OptimizationLevel::O0,
      lang::CppStandard::Cpp23);
  const std::filesystem::path canonicalSource =
      std::filesystem::weakly_canonical(source);

  ThrowingBackend standardBackend(true);
  const lang::driver::CompilationResult standardFailure =
      lang::driver::compileWithBackend(request, standardBackend);
  const auto standardDiagnostic = std::find_if(
      standardFailure.diagnostics.begin(), standardFailure.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-B0001";
      });
  expect(standardBackend.wasInvoked() && !standardFailure.succeeded() &&
             standardFailure.status ==
                 lang::driver::CompilationStatus::BackendFailure &&
             !standardFailure.artifact,
         "a standard backend exception should become a backend compilation "
         "failure without publishing an artifact");
  expect(standardDiagnostic != standardFailure.diagnostics.end() &&
             standardDiagnostic->phase == lang::DiagnosticPhase::Backend &&
             standardDiagnostic->primary.source == canonicalSource.string() &&
             standardDiagnostic->primary.start == 0 &&
             standardDiagnostic->primary.end == 0 &&
             standardDiagnostic->message.find("backend failed") !=
                 std::string::npos &&
             standardDiagnostic->message.find("throwing-test") ==
                 std::string::npos &&
             standardFailure.backendFailureDetails &&
             standardFailure.backendFailureDetails->find("throwing-test") !=
                 std::string::npos &&
             standardFailure.backendFailureDetails->find(
                 "deliberate backend failure") != std::string::npos &&
             !standardDiagnostic->hints.empty() &&
             standardFailure.sources.find(canonicalSource.string()) != nullptr,
         "a standard backend exception should retain the source snapshot and "
         "produce an actionable entry-anchored GTI-B0001 diagnostic");

  ThrowingBackend unknownBackend(false);
  const lang::driver::CompilationResult unknownFailure =
      lang::driver::compileWithBackend(request, unknownBackend);
  const auto unknownDiagnostic = std::find_if(
      unknownFailure.diagnostics.begin(), unknownFailure.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-B0001";
      });
  expect(unknownBackend.wasInvoked() && !unknownFailure.succeeded() &&
             unknownFailure.status ==
                 lang::driver::CompilationStatus::BackendFailure &&
             !unknownFailure.artifact &&
             unknownDiagnostic != unknownFailure.diagnostics.end() &&
             unknownFailure.backendFailureDetails &&
             unknownFailure.backendFailureDetails->find(
                 "non-standard exception") != std::string::npos &&
             unknownFailure.sources.find(canonicalSource.string()) != nullptr,
         "a non-standard backend exception should use the same structured "
         "driver failure boundary");

  ThrowingBackend unnamedBackend(true, true);
  const lang::driver::CompilationResult unnamedFailure =
      lang::driver::compileWithBackend(request, unnamedBackend);
  const auto unnamedDiagnostic = std::find_if(
      unnamedFailure.diagnostics.begin(), unnamedFailure.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-B0001";
      });
  expect(unnamedBackend.wasInvoked() && !unnamedFailure.succeeded() &&
             unnamedFailure.status ==
                 lang::driver::CompilationStatus::BackendFailure &&
             unnamedDiagnostic != unnamedFailure.diagnostics.end() &&
             unnamedFailure.backendFailureDetails &&
             unnamedFailure.backendFailureDetails->find("Backend: unknown") !=
                 std::string::npos &&
             unnamedFailure.backendFailureDetails->find(
                 "deliberate backend failure") != std::string::npos,
         "an exception from backend name metadata should not defeat the "
         "generation exception boundary");

  UnsupportedBackend unsupportedBackend;
  const lang::driver::CompilationResult unsupported =
      lang::driver::compileWithBackend(request, unsupportedBackend);
  const auto unsupportedDiagnostic = std::find_if(
      unsupported.diagnostics.begin(), unsupported.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-B0003";
      });
  expect(unsupportedBackend.wasInvoked() && !unsupported.succeeded() &&
             unsupported.status ==
                 lang::driver::CompilationStatus::BackendFailure &&
             !unsupported.artifact &&
             unsupportedDiagnostic != unsupported.diagnostics.end() &&
             unsupportedDiagnostic->phase == lang::DiagnosticPhase::Backend &&
             unsupportedDiagnostic->primary.source ==
                 canonicalSource.string() &&
             unsupportedDiagnostic->message.find("unsupported-test") ==
                 std::string::npos &&
             unsupportedDiagnostic->message.find(
                 "missing bounded test capability") != std::string::npos &&
             std::none_of(unsupported.diagnostics.begin(),
                          unsupported.diagnostics.end(),
                          [](const lang::Diagnostic &diagnostic) {
                            return diagnostic.code == "GTI-B0001";
                          }) &&
             unsupportedDiagnostic->hints.empty(),
         "a classified backend limitation should use GTI-B0003 without "
         "asking the user to report an internal compiler error");

  const lang::SourceSpan precise{canonicalSource.string(), 4, 8, 1};
  UnsupportedBackend locatedBackend(precise);
  const auto located =
      lang::driver::compileWithBackend(request, locatedBackend);
  const auto locatedDiagnostic = std::find_if(
      located.diagnostics.begin(), located.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-B0003"; });
  expect(locatedDiagnostic != located.diagnostics.end() &&
             locatedDiagnostic->primary == precise && !located.artifact,
         "a backend limitation preserves its exact source span without "
         "publication");
}

void testProcessInvocation(const std::filesystem::path &testExecutable) {
  const lang::driver::ProcessResult captured = lang::driver::invokeProcess(
      {testExecutable.string(), "--process-child", "alpha", "two words", ""},
      {.outputMode = lang::driver::ProcessOutputMode::Capture,
       .captureSuccessfulOutput = true,
       .description = "test child"});
  expect(captured.succeeded() &&
             captured.output == "process arguments preserved\n",
         "the reusable process runner should preserve exact argument-vector "
         "elements");

  const lang::driver::ProcessResult inherited = lang::driver::invokeProcess(
      {testExecutable.string(), "--process-child-exit"},
      {.outputMode = lang::driver::ProcessOutputMode::Inherit,
       .captureSuccessfulOutput = false,
       .description = "test child"});
  expect(inherited.exitCode == 23 && inherited.output.empty(),
         "inherited process execution should propagate the child exit status "
         "without capturing terminal output");

  const lang::driver::ProcessResult empty =
      lang::driver::invokeProcess({}, {.description = "test child"});
  expect(empty.exitCode == 127 && empty.driverDiagnostic &&
             empty.driverDiagnostic->find("test child command is empty") !=
                 std::string::npos,
         "empty process requests should fail before attempting execution");

#if !defined(_WIN32)
  TemporaryDirectory directory;
  const lang::driver::ProcessResult configured = lang::driver::invokeProcess(
      {std::filesystem::absolute(testExecutable).string(),
       "--process-child-context"},
      {.outputMode = lang::driver::ProcessOutputMode::Capture,
       .captureSuccessfulOutput = true,
       .description = "configured test child",
       .workingDirectory = directory.root(),
       .environment = {{"GTI_DRIVER_PROCESS_TEST", "exact value"}}});
  expect(configured.succeeded() &&
             configured.output ==
                 std::filesystem::weakly_canonical(directory.root()).string() +
                     "\nexact value\n",
         "the reusable process runner should apply an exact working directory "
         "and environment overrides in the child only");
#endif
}

void testNativeCommandConstruction() {
  lang::driver::NativeInputs inputs;
  inputs.includeDirectories = {"runtime include", "vendor"};
  inputs.compilerArguments = {"-DDEBUG=1"};
  inputs.libraryDirectories = {"native lib"};
  inputs.libraryFiles = {"libnative.a"};
  inputs.libraries = {"engine"};
  inputs.frameworks = {"Metal"};
  inputs.linkerArguments = {"-pthread"};
  inputs.trailingArguments = {"-Wl,--as-needed", "forwarded.cpp"};
  inputs.finalLibraryFiles = {"libgti_runtime.a"};

  const lang::driver::NativeCompileRequest request(
      "custom-c++", "generated source.cpp", "game", lang::CppStandard::Cpp20,
      lang::OptimizationLevel::O3, std::move(inputs));
  const std::vector<std::string> command =
      lang::driver::NativeToolchain().command(request);
  std::vector<std::string> expected{
      "custom-c++",
      "-std=c++20",
      "-O3",
      "-Iruntime include",
      "-Ivendor",
      "-DDEBUG=1",
      "generated source.cpp",
      "-Lnative lib",
      "libnative.a",
      "-lengine",
      "-framework",
      "Metal",
      "-pthread",
      "-o",
      "game",
      "-Wl,--as-needed",
      "forwarded.cpp",
      "libgti_runtime.a",
      "-fno-fast-math",
      "-ffp-contract=off",
      "-D__gti_strict_ieee754=1",
  };
#if defined(__APPLE__)
  expected.emplace_back("-Wl,-reproducible");
#endif
  expect(command == expected,
         "native command construction should preserve structured input order "
         "and link driver-owned archives after every other input, so "
         "order-sensitive linkers let forwarded sources use or replace them");
  const auto debugCommand = lang::driver::NativeToolchain().command(
      lang::driver::NativeCompileRequest(
          "custom-c++", "generated.cpp", "program", lang::CppStandard::Cpp23,
          lang::OptimizationLevel::O0, {}, std::nullopt, true));
  expect(
      std::find(debugCommand.begin(), debugCommand.end(), "-g") !=
              debugCommand.end() &&
          std::find(command.begin(), command.end(), "-g") == command.end(),
      "native debug symbols should be selected by the structured debug policy");
  const std::string rendered = lang::driver::renderCommand(command);
  expect(rendered.starts_with("+ custom-c++ -std=c++20 -O3") &&
             rendered.find("\"-Iruntime include\"") != std::string::npos &&
             rendered.find("\"generated source.cpp\"") != std::string::npos,
         "verbose command rendering should quote arguments without changing "
         "the process vector");
  expect(lang::driver::discoverNativeCompiler(std::string("selected-c++")) ==
             "selected-c++",
         "an explicitly selected compiler should have highest precedence");
  expect(lang::driver::discoverCCompiler(std::string("selected-cc")) ==
             "selected-cc",
         "an explicitly selected C compiler should have highest precedence");

  const lang::driver::NativeCCompileRequest cRequest(
      "custom-cc", "native source.c", "native object.o",
      lang::driver::CStandard::C17, lang::OptimizationLevel::O2,
      {"native include"}, {"-DNATIVE_VALUE=42"});
  expect(lang::driver::NativeToolchain().command(cRequest) ==
             std::vector<std::string>({"custom-cc", "-std=c17", "-O2",
                                       "-Inative include", "-DNATIVE_VALUE=42",
                                       "-c", "native source.c", "-o",
                                       "native object.o"}),
         "native C compilation should preserve its distinct standard, include, "
         "argument, source, and output policy");
  expect(
      lang::driver::NativeToolchain().preprocessCommand(
          cRequest, "native dependencies.d") ==
          std::vector<std::string>(
              {"custom-cc", "-std=c17", "-O2", "-Inative include",
               "-DNATIVE_VALUE=42", "-E", "-MD", "-MF", "native dependencies.d",
               "native source.c", "-o", "native object.o"}),
      "native C preprocessing should preserve the compile request while "
      "replacing object compilation with preprocessing and dependency "
      "discovery");

  const lang::driver::NativeCppCompileRequest cppRequest(
      "custom-c++", "native source.cpp", "native object.o",
      lang::CppStandard::Cpp23, lang::OptimizationLevel::O1,
      {"runtime include", "native include"}, {"-DNATIVE_CPP_VALUE=42"});
  expect(lang::driver::NativeToolchain().command(cppRequest) ==
             std::vector<std::string>(
                 {"custom-c++", "-std=c++23", "-O1", "-Iruntime include",
                  "-Inative include", "-DNATIVE_CPP_VALUE=42", "-c",
                  "native source.cpp", "-o", "native object.o"}),
         "native C++ source compilation should preserve the resolved standard, "
         "include, argument, source, and output policy");

  const lang::driver::NativeCppCompileRequest objcRequest(
      "selected-clang++", "Foundation bridge.mm", "bridge.o",
      lang::CppStandard::Cpp20, lang::OptimizationLevel::O2, {"native include"},
      {"-fobjc-arc"}, {.triple = "arm64-apple-macos", .sysroot = "SDK"});
  expect(lang::driver::NativeToolchain().command(objcRequest) ==
             std::vector<std::string>(
                 {"selected-clang++", "-std=c++20", "-O2",
                  "--target=arm64-apple-macos", "--sysroot=SDK",
                  "-Inative include", "-fobjc-arc", "-x", "objective-c++", "-c",
                  "Foundation bridge.mm", "-o", "bridge.o"}),
         "Objective-C++ bridge compilation should preserve policy and select "
         "its language without changing the final generated C++ language");
  expect(lang::driver::NativeToolchain().preprocessCommand(objcRequest,
                                                           "deps.d") ==
             std::vector<std::string>(
                 {"selected-clang++", "-std=c++20", "-O2",
                  "--target=arm64-apple-macos", "--sysroot=SDK",
                  "-Inative include", "-fobjc-arc", "-x", "objective-c++", "-E",
                  "-MD", "-MF", "deps.d", "Foundation bridge.mm", "-o",
                  "bridge.o"}),
         "Objective-C++ cache dependency discovery must use the same language, "
         "ARC, target, standard, and include policy as compilation");

  lang::driver::NativeInputs orderedInputs;
  orderedInputs.libraryDirectories = {"native lib"};
  orderedInputs.libraryFiles = {"ignored-file.a"};
  orderedInputs.libraries = {"ignored-library"};
  orderedInputs.frameworks = {"IgnoredFramework"};
  orderedInputs.orderedLinkOperands = {
      {lang::driver::NativeLinkOperandKind::File, "libgti_runtime.a"},
      {lang::driver::NativeLinkOperandKind::Library, "target"},
      {lang::driver::NativeLinkOperandKind::File, "package-provider.a"},
      {lang::driver::NativeLinkOperandKind::Framework, "CoreFoundation"},
  };
  orderedInputs.linkerArguments = {"-pthread"};
  const std::vector<std::string> orderedCommand =
      lang::driver::NativeToolchain().command(
          lang::driver::NativeCompileRequest(
              "custom-c++", "generated.cpp", "program",
              lang::CppStandard::Cpp23, lang::OptimizationLevel::O2,
              std::move(orderedInputs)));
  std::vector<std::string> expectedOrderedCommand{
      "custom-c++",
      "-std=c++23",
      "-O2",
      "generated.cpp",
      "-Lnative lib",
      "libgti_runtime.a",
      "-ltarget",
      "package-provider.a",
      "-framework",
      "CoreFoundation",
      "-pthread",
      "-o",
      "program",
      "-fno-fast-math",
      "-ffp-contract=off",
      "-D__gti_strict_ieee754=1",
  };
#if defined(__APPLE__)
  expectedOrderedCommand.emplace_back("-Wl,-reproducible");
#endif
  expect(orderedCommand == expectedOrderedCommand,
         "ordered native link operands should preserve mixed file, -l, and "
         "framework dependency order after the build-owned runtime and be "
         "authoritative over compatibility category vectors");

  lang::driver::NativeInputs objectInputs;
  objectInputs.targetPolicy = {.triple = "aarch64-unknown-linux-gnu",
                               .sysroot = "target sysroot"};
  objectInputs.compilerArguments = {"-DSOURCE_ONLY=1"};
  objectInputs.libraryDirectories = {"native lib"};
  objectInputs.orderedLinkOperands = {
      {lang::driver::NativeLinkOperandKind::File, "native-provider.o"},
      {lang::driver::NativeLinkOperandKind::Library, "target"},
  };
  objectInputs.linkerArguments = {"-pthread"};
  objectInputs.finalLibraryFiles = {"libgti_runtime.a"};
  const std::vector<std::string> objectCommand =
      lang::driver::NativeToolchain().command(lang::driver::NativeLinkRequest(
          "custom-c++", "generated.o", "program", std::move(objectInputs),
          true));
  std::vector<std::string> expectedObjectCommand{
      "custom-c++",
      "--target=aarch64-unknown-linux-gnu",
      "--sysroot=target sysroot",
      "generated.o",
      "-Lnative lib",
      "native-provider.o",
      "-ltarget",
      "-pthread",
      "-o",
      "program",
      "libgti_runtime.a",
      "-g",
  };
#if defined(__APPLE__)
  expectedObjectCommand.emplace_back("-Wl,-reproducible");
#endif
  expect(objectCommand == expectedObjectCommand,
         "backend-produced objects should link under driver-owned target and "
         "dependency policy without inheriting C++ source flags");

  // Forwarded arguments may name native sources the link compiles. They get
  // the context generated C++ would give them; the object itself gets none.
  lang::driver::NativeInputs forwardedInputs;
  forwardedInputs.includeDirectories = {"runtime include"};
  forwardedInputs.compilerArguments = {"-DSOURCE_ONLY=1"};
  forwardedInputs.trailingArguments = {"companion.cpp"};
  forwardedInputs.finalLibraryFiles = {"libgti_runtime.a"};
  const lang::driver::ForwardedSourcePolicy forwardedPolicy{
      lang::CppStandard::Cpp20, lang::OptimizationLevel::O2};
  const std::vector<std::string> forwardedCommand =
      lang::driver::NativeToolchain().command(lang::driver::NativeLinkRequest(
          "custom-c++", "generated.o", "program", forwardedInputs, false,
          forwardedPolicy));
  std::vector<std::string> expectedForwardedCommand{
      "custom-c++",
      "-std=c++20",
      "-O2",
      "-Iruntime include",
      "-DSOURCE_ONLY=1",
      "generated.o",
      "-o",
      "program",
      "companion.cpp",
      "libgti_runtime.a",
      "-fno-fast-math",
      "-ffp-contract=off",
      "-D__gti_strict_ieee754=1",
  };
#if defined(__APPLE__)
  expectedForwardedCommand.emplace_back("-Wl,-reproducible");
#endif
  expect(forwardedCommand == expectedForwardedCommand,
         "an object link should compile forwarded native sources with the "
         "language level, optimization, include directories, compiler "
         "arguments and strict floating point of a C++ backend invocation");
  forwardedInputs.trailingArguments.clear();
  const std::vector<std::string> linkOnlyCommand =
      lang::driver::NativeToolchain().command(lang::driver::NativeLinkRequest(
          "custom-c++", "generated.o", "program", forwardedInputs, false,
          forwardedPolicy));
  std::vector<std::string> expectedLinkOnlyCommand{
      "custom-c++", "generated.o", "-o", "program", "libgti_runtime.a"};
#if defined(__APPLE__)
  expectedLinkOnlyCommand.emplace_back("-Wl,-reproducible");
#endif
  expect(linkOnlyCommand == expectedLinkOnlyCommand,
         "an object link without forwarded arguments should add no source "
         "flags");

  expect(lang::driver::isReservedNativeBuildArgument("-o") &&
             lang::driver::isReservedNativeBuildArgument(
                 "--target=other-unknown-os") &&
             lang::driver::isReservedNativeBuildArgument("-target=arm64") &&
             lang::driver::isReservedNativeBuildArgument("-m32") &&
             lang::driver::isReservedNativeBuildArgument(
                 "-Wl,-syslibroot,/other-sdk") &&
             lang::driver::isReservedNativeBuildArgument(
                 "-Wl,--sysroot=/other-sdk") &&
             lang::driver::isReservedNativeBuildArgument(
                 "-Xlinker=--sysroot=/other-sdk") &&
             lang::driver::isReservedNativeBuildArgument("-Xlinker") &&
             lang::driver::isReservedNativeBuildArgument("-Xclang") &&
             lang::driver::isReservedNativeBuildArgument("-Xclang=-triple") &&
             lang::driver::isReservedNativeBuildArgument("-cc1") &&
             lang::driver::isReservedNativeBuildArgument("-cc1as") &&
             lang::driver::isReservedNativeBuildArgument("--driver-mode=cl") &&
             lang::driver::isReservedNativeBuildArgument("-Wl,-m,elf_i386") &&
             lang::driver::isReservedNativeBuildArgument("/machine:x64") &&
             lang::driver::isReservedNativeBuildArgument("/O2") &&
             !lang::driver::isReservedNativeBuildArgument(
                 "-DGTI_NATIVE_POLICY=1") &&
             !lang::driver::isReservedNativeBuildArgument(
                 "-DGTI_LIST=alpha,@beta") &&
             !lang::driver::isReservedNativeBuildArgument("-lprovider") &&
             !lang::driver::isReservedNativeBuildArgument("/openmp") &&
             !lang::driver::isReservedNativeBuildArgument("/FORCE:MULTIPLE"),
         "the shared native argument policy should reserve driver-owned "
         "output and target/data-layout settings, including raw native-driver "
         "escape modes, while preserving ordinary compiler definitions and "
         "link operands without prefix-based false positives");
}

void testRenderedCommandReplay(const std::filesystem::path &testExecutable) {
  const std::vector<std::string> arguments{
      "plain",
      "safe/path-1.2_@%+=:,",
      "two words",
      "tab\tvalue",
      "",
      "a\\b",
      "$HOME",
      "`command`",
      "x;y",
      "a'b",
      "line\nbreak",
      "*.cpp",
      "\"quoted\"",
      "left&right",
      "left|right",
      "<input>",
      "(group)",
      "question?",
  };
  const std::string rendered = lang::driver::renderCommand(arguments);
  const std::string expected =
      "+ plain safe/path-1.2_@%+=:, \"two words\" \"tab\tvalue\" '' "
      "'a\\b' '$HOME' '`command`' 'x;y' 'a'\\''b' 'line\nbreak' "
      "'*.cpp' '\"quoted\"' 'left&right' 'left|right' '<input>' "
      "'(group)' 'question?'";
  expect(rendered == expected,
         "POSIX command rendering should use readable whitespace quotes and "
         "fail-safe quoting for every other shell-sensitive argument");

#if !defined(_WIN32)
  std::vector<std::string> replayArguments{testExecutable.string(),
                                           "--render-command-child"};
  replayArguments.insert(replayArguments.end(), arguments.begin(),
                         arguments.end());
  const std::string replay = lang::driver::renderCommand(replayArguments);
  const lang::driver::ProcessResult result = lang::driver::invokeProcess(
      {"/bin/sh", "-c", replay.substr(2)},
      {.outputMode = lang::driver::ProcessOutputMode::Capture,
       .captureSuccessfulOutput = true,
       .description = "rendered POSIX command"});
  expect(result.succeeded() &&
             result.output == "rendered arguments preserved\n",
         "a POSIX shell should reconstruct every rendered argument exactly");
#endif
}

void testNativeCCompilerFailure() {
  TemporaryDirectory temporary;
  const std::filesystem::path include = temporary.root() / "include";
  std::filesystem::create_directories(include / "gti");
  const std::filesystem::path generated = temporary.root() / "generated.o";
  const std::filesystem::path nativeSource = temporary.root() / "helper.c";
#if defined(_WIN32)
  const std::filesystem::path nativeObject =
      temporary.root() / "generated.native-0-helper.obj";
#else
  const std::filesystem::path nativeObject =
      temporary.root() / "generated.native-0-helper.o";
#endif
  expect(writeFile(temporary.root() / "prelude.gti", "") &&
             writeFile(temporary.root() / "main.gti",
                       "int main() { return 0; }\n") &&
             writeFile(nativeSource, "int helper(void) { return 42; }\n") &&
             writeFile(include / "gti/runtime.hpp", "") &&
             writeFile(include / "gti/runtime.h", "") &&
             writeFile(include / "gti/c_abi.h", "") &&
             writeFile(include / "gti/runtime_failure.h", "") &&
             writeFile(include / "gti/runtime_thread.h", "") &&
             writeFile(include / "gti/runtime_thread.hpp", "") &&
             writeFile(temporary.root() / "libgti_runtime.a", "") &&
             writeFile(nativeObject, "previous object"),
         "native C compiler failure fixtures should be writable");

  lang::driver::NativeInputs inputs;
  inputs.cSources = {nativeSource};
  inputs.cStandard = lang::driver::CStandard::C17;
  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              temporary.root() / "main.gti",
              lang::standardLibraryLayout(temporary.root()),
              lang::TargetInfo::host(), lang::OptimizationLevel::O0,
              lang::CppStandard::Cpp23)
              .withBackend(lang::BackendKind::Llvm),
          {.standardLibrary = lang::standardLibraryLayout(temporary.root()),
           .runtimeInclude = include,
           .runtimeLibrary = temporary.root() / "libgti_runtime.a"},
          generated, temporary.root() / "program", "unused-c++",
          std::move(inputs), false, false, false, std::nullopt,
          (temporary.root() / "missing-c-compiler").string()));
  expect(
      result.status ==
              lang::driver::ExecutableBuildStatus::NativeCCompilerFailure &&
          result.cCompilations.size() == 1 &&
          result.cCompilations.front().source == nativeSource &&
          result.cCompilations.front().object == nativeObject &&
          result.cCompilations.front().process.exitCode != 0 &&
          result.nativeCommand.empty() && result.generatedSourceRetained &&
          std::filesystem::is_regular_file(generated) &&
          readFile(nativeObject) == "previous object" &&
          !std::filesystem::exists(temporary.root() / "program"),
      "a failed C compiler invocation should stop before final linking, "
      "retain the backend object for diagnosis, and preserve a prior object");
}

void testNativeCppCompilerFailure() {
  // Library callers can construct native inputs without manifest resolution.
  // Reject both application and composed bridges before creating artifacts or
  // invoking even an intentionally missing compiler.
  for (bool dependency : {false, true}) {
    TemporaryDirectory rejected;
    lang::driver::NativeInputs inputs;
    if (dependency) {
      lang::driver::NativeDependencyGroup group;
      group.packageIdentity = "bridge@1.0.0";
      group.cppSources = {rejected.root() / "bridge.mm"};
      inputs.dependencyGroups.push_back(std::move(group));
    } else {
      inputs.cppSources = {rejected.root() / "bridge.mm"};
    }
    const auto result =
        lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
            lang::driver::CompilationRequest(
                rejected.root() / "missing.gti",
                lang::standardLibraryLayout(rejected.root()),
                {.os = "linux", .vendor = "unknown", .arch = "x86_64"},
                lang::OptimizationLevel::O0, lang::CppStandard::Cpp23)
                .withBackend(lang::BackendKind::Llvm),
            {.standardLibrary = lang::standardLibraryLayout(rejected.root())},
            rejected.root() / "generated.o", rejected.root() / "program",
            "missing-compiler", std::move(inputs), false, false, false));
    expect(result.status == lang::driver::ExecutableBuildStatus::
                                ToolchainConfigurationFailure &&
               result.driverDiagnostic &&
               result.driverDiagnostic->find("macOS") != std::string::npos &&
               result.cppCompilations.empty() && result.nativeCommand.empty() &&
               !std::filesystem::exists(rejected.root() / "generated.o"),
           "direct driver clients must not bypass the macOS Objective-C++ "
           "boundary");
  }
  TemporaryDirectory temporary;
  const std::filesystem::path include = temporary.root() / "include";
  std::filesystem::create_directories(include / "gti");
  const std::filesystem::path generated = temporary.root() / "generated.o";
  const std::filesystem::path nativeSource = temporary.root() / "helper.cpp";
#if defined(_WIN32)
  const std::filesystem::path nativeObject =
      temporary.root() / "generated.native-0-helper.obj";
#else
  const std::filesystem::path nativeObject =
      temporary.root() / "generated.native-0-helper.o";
#endif
  expect(writeFile(temporary.root() / "prelude.gti", "") &&
             writeFile(temporary.root() / "main.gti",
                       "int main() { return 0; }\n") &&
             writeFile(nativeSource, "int helper() { return 42; }\n") &&
             writeFile(include / "gti/runtime.hpp", "") &&
             writeFile(include / "gti/runtime.h", "") &&
             writeFile(include / "gti/c_abi.h", "") &&
             writeFile(include / "gti/runtime_failure.h", "") &&
             writeFile(include / "gti/runtime_thread.h", "") &&
             writeFile(include / "gti/runtime_thread.hpp", "") &&
             writeFile(temporary.root() / "libgti_runtime.a", "") &&
             writeFile(nativeObject, "previous object"),
         "native C++ compiler failure fixtures should be writable");

  lang::driver::NativeInputs inputs;
  inputs.cppSources = {nativeSource};
  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              temporary.root() / "main.gti",
              lang::standardLibraryLayout(temporary.root()),
              lang::TargetInfo::host(), lang::OptimizationLevel::O0,
              lang::CppStandard::Cpp23)
              .withBackend(lang::BackendKind::Llvm),
          {.standardLibrary = lang::standardLibraryLayout(temporary.root()),
           .runtimeInclude = include,
           .runtimeLibrary = temporary.root() / "libgti_runtime.a"},
          generated, temporary.root() / "program",
          (temporary.root() / "missing-cpp-compiler").string(),
          std::move(inputs), false, false, false));
  expect(
      result.status ==
              lang::driver::ExecutableBuildStatus::NativeCppCompilerFailure &&
          result.cppCompilations.size() == 1 &&
          result.cppCompilations.front().source == nativeSource &&
          result.cppCompilations.front().object == nativeObject &&
          result.cppCompilations.front().process.exitCode != 0 &&
          result.nativeCommand.empty() && result.generatedSourceRetained &&
          std::filesystem::is_regular_file(generated) &&
          readFile(nativeObject) == "previous object" &&
          !std::filesystem::exists(temporary.root() / "program"),
      "a failed C++ source compiler invocation should stop before final "
      "linking, "
      "retain the backend object for diagnosis, and preserve a prior object");
}

void testOrderedExecutableBuildCommand() {
  TemporaryDirectory temporary;
  const std::filesystem::path include = temporary.root() / "include";
  std::filesystem::create_directories(include / "gti");
  expect(writeFile(temporary.root() / "prelude.gti", "") &&
             writeFile(temporary.root() / "main.gti",
                       "int main() { return 0; }\n") &&
             writeFile(include / "gti/runtime.hpp", "") &&
             writeFile(include / "gti/runtime.h", "") &&
             writeFile(include / "gti/c_abi.h", "") &&
             writeFile(include / "gti/runtime_failure.h", "") &&
             writeFile(include / "gti/runtime_thread.h", "") &&
             writeFile(include / "gti/runtime_thread.hpp", "") &&
             writeFile(temporary.root() / "libgti_runtime.a", "") &&
             writeFile(temporary.root() / "provider.c",
                       "int native_provider(void) { return 42; }\n") &&
             writeFile(temporary.root() / "provider.a", ""),
         "ordered executable-build fixtures should be writable");

  lang::driver::NativeInputs inputs;
  inputs.cSources = {temporary.root() / "provider.c"};
  inputs.cStandard = lang::driver::CStandard::C17;
  inputs.libraryFiles = {"ignored-category-file.a"};
  inputs.libraries = {"ignored-category-library"};
  inputs.orderedLinkOperands = {
      {lang::driver::NativeLinkOperandKind::Library, "target"},
      {lang::driver::NativeLinkOperandKind::File,
       (temporary.root() / "provider.a").string()},
  };
  const lang::driver::ExecutableBuildResult result =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              temporary.root() / "main.gti",
              lang::standardLibraryLayout(temporary.root()),
              lang::TargetInfo::host(), lang::OptimizationLevel::O0,
              lang::CppStandard::Cpp23)
              .withBackend(lang::BackendKind::Llvm),
          {.standardLibrary = lang::standardLibraryLayout(temporary.root()),
           .runtimeInclude = include,
           .runtimeLibrary = temporary.root() / "libgti_runtime.a"},
          temporary.root() / "generated.o", temporary.root() / "program",
          (temporary.root() / "missing-native-compiler").string(),
          std::move(inputs), false, false, false, std::nullopt,
          lang::driver::discoverCCompiler()));
  const std::vector<std::string> &command = result.nativeCommand;
#if defined(_WIN32)
  const std::filesystem::path expectedNativeObject =
      temporary.root() / "generated.native-0-provider.obj";
#else
  const std::filesystem::path expectedNativeObject =
      temporary.root() / "generated.native-0-provider.o";
#endif
  const auto nativeObject =
      std::find(command.begin(), command.end(), expectedNativeObject.string());
  const auto runtime =
      std::find(command.begin(), command.end(),
                (temporary.root() / "libgti_runtime.a").string());
  const auto target = std::find(command.begin(), command.end(), "-ltarget");
  const auto provider = std::find(command.begin(), command.end(),
                                  (temporary.root() / "provider.a").string());
  expect(result.status ==
                 lang::driver::ExecutableBuildStatus::NativeCompilerFailure &&
             runtime != command.end() && target != command.end() &&
             provider != command.end() && nativeObject != command.end() &&
             result.cCompilations.size() == 1 && nativeObject < target &&
             target < provider && provider < runtime &&
             std::find(command.begin(), command.end(),
                       "ignored-category-file.a") == command.end() &&
             std::find(command.begin(), command.end(),
                       "-lignored-category-library") == command.end(),
         "executable builds should place compiled C objects and authoritative "
         "ordered project operands before the runtime, which links last, "
         "without duplicating split metadata categories");
  expect(result.backendDiagnosticLog &&
             result.backendDiagnosticLog->succeeded() &&
             readFile(result.backendDiagnosticLog->path).find("Command:") !=
                 std::string::npos &&
             readFile(result.backendDiagnosticLog->path)
                     .find("missing-native-compiler") != std::string::npos &&
             result.generatedSourceRetained,
         "a native invocation failure should preserve its exact command, "
         "driver failure and representation as developer evidence");
  if (result.backendDiagnosticLog && result.backendDiagnosticLog->succeeded()) {
    std::filesystem::remove(result.backendDiagnosticLog->path);
    std::filesystem::remove(result.backendDiagnosticLog->path.parent_path());
  }
}

void testDiagnosticLogs() {
  constexpr std::string_view evidence("raw\0evidence", 12);
  const auto first = lang::driver::writeDiagnosticLog(evidence);
  const auto second = lang::driver::writeDiagnosticLog("other evidence");
  expect(
      first.succeeded() && second.succeeded() && first.path != second.path &&
          readFile(first.path) == evidence &&
          readFile(second.path) == "other evidence",
      "diagnostic logs should preserve exact bytes in distinct retained paths");
#if !defined(_WIN32)
  if (first.succeeded()) {
    const auto otherPermissions =
        std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    expect(
        (std::filesystem::status(first.path).permissions() &
         otherPermissions) == std::filesystem::perms::none &&
            (std::filesystem::status(first.path.parent_path()).permissions() &
             otherPermissions) == std::filesystem::perms::none,
        "backend evidence directories and logs should be owner-only");
  }
  TemporaryDirectory temporary;
  const auto blocker = temporary.root() / "not-a-directory";
  expect(writeFile(blocker, "preserve this file"),
         "the log-failure fixture should be writable");
  {
    ScopedEnvironment badTemporaryRoot("TMPDIR", blocker.string());
    const auto failed = lang::driver::writeDiagnosticLog("unsaved evidence");
    expect(!failed.succeeded() && failed.path.empty() && failed.error &&
               readFile(blocker) == "preserve this file",
           "log preservation failure must report failure without an invented "
           "saved path or overwriting unrelated files");
  }
#endif
  for (const auto *log : {&first, &second}) {
    if (log->succeeded()) {
      std::filesystem::remove(log->path);
      std::filesystem::remove(log->path.parent_path());
    }
  }
}

void testManagedOutputSafety() {
  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.root() / "project";
  const std::filesystem::path managedRoot = project / "build/gti";
  const std::filesystem::path generated =
      managedRoot / "dev/host/intermediate/program.o";
  const std::filesystem::path externalOutput =
      temporary.root() / "external/program";
  std::filesystem::create_directory(project);
  const lang::driver::ManagedOutputPolicy managed{.trustedRoot = project,
                                                  .outputRoot = managedRoot};
  const std::filesystem::path existingExecutable =
      managedRoot / "dev/host/existing";
  const std::optional<std::string> missingExecutable =
      lang::driver::managedExecutableDiagnostic(managed, existingExecutable);
  expect(missingExecutable &&
             missingExecutable->find("does not exist") != std::string::npos,
         "a build-skipping run should reject a missing managed executable");
  std::filesystem::create_directories(existingExecutable.parent_path());
  expect(writeFile(existingExecutable, "existing executable\n"),
         "the existing managed executable fixture should be writable");
#if !defined(_WIN32)
  const std::optional<std::string> nonExecutableDiagnostic =
      lang::driver::managedExecutableDiagnostic(managed, existingExecutable);
  expect(nonExecutableDiagnostic && nonExecutableDiagnostic->find(
                                        "not executable") != std::string::npos,
         "a build-skipping run should reject a regular file without execute "
         "permissions");
  std::error_code permissionError;
  std::filesystem::permissions(
      existingExecutable, std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::add, permissionError);
  expect(!permissionError,
         "the existing managed executable fixture should be executable");
#endif
  expect(
      !lang::driver::managedExecutableDiagnostic(managed, existingExecutable),
      "a regular executable inside the symlink-free managed subtree should "
      "be runnable without rebuilding");

  const std::filesystem::path directoryArtifact =
      managedRoot / "dev/host/not-an-executable";
  std::filesystem::create_directory(directoryArtifact);
  const std::optional<std::string> directoryDiagnostic =
      lang::driver::managedExecutableDiagnostic(managed, directoryArtifact);
  expect(directoryDiagnostic && directoryDiagnostic->find(
                                    "not a regular file") != std::string::npos,
         "a build-skipping run should reject a directory in place of its "
         "managed executable");

  const std::filesystem::path externalExecutable =
      temporary.root() / "external-existing";
  expect(writeFile(externalExecutable, "external executable\n"),
         "the external executable fixture should be writable");
  const std::filesystem::path symlinkedExecutable =
      managedRoot / "dev/host/symlinked";
  std::error_code executableSymlinkError;
  std::filesystem::create_symlink(externalExecutable, symlinkedExecutable,
                                  executableSymlinkError);
  if (!executableSymlinkError) {
    const std::optional<std::string> symlinkedDiagnostic =
        lang::driver::managedExecutableDiagnostic(managed, symlinkedExecutable);
    expect(symlinkedDiagnostic &&
               symlinkedDiagnostic->find("symbolic-link managed output ") !=
                   std::string::npos,
           "a build-skipping run should reject a symbolic-link managed "
           "executable rather than following it outside the project");
  }

  const lang::driver::ExecutableBuildResult outside =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              project / "missing.gti",
              lang::standardLibraryLayout(temporary.root()),
              lang::TargetInfo::host(), lang::OptimizationLevel::O0,
              lang::CppStandard::Cpp23)
              .withBackend(lang::BackendKind::Llvm),
          {}, managedRoot / "intermediate/program.o", externalOutput,
          "unused-c++", {}, false, true, false,
          lang::driver::ManagedOutputPolicy{.trustedRoot = project,
                                            .outputRoot = managedRoot}));
  expect(outside.status ==
                 lang::driver::ExecutableBuildStatus::OutputDirectoryFailure &&
             outside.driverDiagnostic &&
             outside.driverDiagnostic->find(
                 "outside the managed project output root") !=
                 std::string::npos &&
             !std::filesystem::exists(externalOutput.parent_path()),
         "managed builds should reject output paths outside their declared "
         "project subtree before compiling or mutating the filesystem");

  const std::filesystem::path externalGenerated =
      temporary.root() / "external-generated.o";
  expect(writeFile(externalGenerated, "preserve me\n"),
         "the external generated-source fixture should be writable");
  std::filesystem::create_directories(generated.parent_path());
  std::error_code symlinkError;
  std::filesystem::create_symlink(externalGenerated, generated, symlinkError);
  if (!symlinkError) {
    const lang::driver::ExecutableBuildResult symlinkedGenerated =
        lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
            lang::driver::CompilationRequest(
                project / "missing.gti",
                lang::standardLibraryLayout(temporary.root()),
                lang::TargetInfo::host(), lang::OptimizationLevel::O0,
                lang::CppStandard::Cpp23)
                .withBackend(lang::BackendKind::Llvm),
            {}, generated, managedRoot / "dev/host/program", "unused-c++", {},
            true, true, false,
            lang::driver::ManagedOutputPolicy{.trustedRoot = project,
                                              .outputRoot = managedRoot}));
    expect(
        symlinkedGenerated.status ==
                lang::driver::ExecutableBuildStatus::OutputDirectoryFailure &&
            symlinkedGenerated.driverDiagnostic &&
            symlinkedGenerated.driverDiagnostic->find(
                "symbolic-link managed output artifact") != std::string::npos &&
            readFile(externalGenerated) == "preserve me\n",
        "managed builds should reject a symbolic-link generated-source "
        "leaf before loading source or overwriting its external target");
  }

  const std::filesystem::path externalProject =
      temporary.root() / "external-project";
  const std::filesystem::path linkedProject = temporary.root() / "project-link";
  std::filesystem::create_directory(externalProject);
  symlinkError.clear();
  std::filesystem::create_directory_symlink(externalProject, linkedProject,
                                            symlinkError);
  if (!symlinkError) {
    const std::filesystem::path linkedManagedRoot = linkedProject / "build/gti";
    const lang::driver::ExecutableBuildResult symlinkedTrustRoot =
        lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
            lang::driver::CompilationRequest(
                linkedProject / "missing.gti",
                lang::standardLibraryLayout(temporary.root()),
                lang::TargetInfo::host(), lang::OptimizationLevel::O0,
                lang::CppStandard::Cpp23)
                .withBackend(lang::BackendKind::Llvm),
            {}, linkedManagedRoot / "intermediate/program.o",
            linkedManagedRoot / "program", "unused-c++", {}, false, true, false,
            lang::driver::ManagedOutputPolicy{.trustedRoot = linkedProject,
                                              .outputRoot =
                                                  linkedManagedRoot}));
    expect(
        symlinkedTrustRoot.status ==
                lang::driver::ExecutableBuildStatus::OutputDirectoryFailure &&
            symlinkedTrustRoot.driverDiagnostic &&
            symlinkedTrustRoot.driverDiagnostic->find(
                "symbolic-link managed output trust root") !=
                std::string::npos &&
            !std::filesystem::exists(externalProject / "build"),
        "managed builds should reject a symbolic-link trust root before "
        "creating output through its external target");
  }
}

void testNativeDependencyDiscoveryPrimitives() {
  std::string errorMessage;
  const auto parsed = [&](std::string_view contents) {
    errorMessage.clear();
    return lang::driver::parseNativeDependencyFile(contents, errorMessage);
  };

  const auto simple = parsed("main.o: main.c include/support.h\n");
  expect(simple && simple->size() == 2 &&
             (*simple)[0] == std::filesystem::path("main.c") &&
             (*simple)[1] == std::filesystem::path("include/support.h"),
         "a plain dependency rule should list every prerequisite in order");

  const auto continued = parsed("main.o: main.c \\\n  a.h \\\r\n  b.h\n");
  expect(continued && continued->size() == 3 &&
             (*continued)[1] == std::filesystem::path("a.h") &&
             (*continued)[2] == std::filesystem::path("b.h"),
         "backslash line continuations should join one dependency rule");

  const auto escaped =
      parsed("out.o: with\\ space.h dollar$$sign.h back\\\\slash.h\n");
  expect(escaped && escaped->size() == 3 &&
             (*escaped)[0] == std::filesystem::path("with space.h") &&
             (*escaped)[1] == std::filesystem::path("dollar$sign.h") &&
             (*escaped)[2] == std::filesystem::path("back\\slash.h"),
         "escaped spaces, doubled dollars, and escaped backslashes should "
         "decode to literal path characters");

  const auto driveLetters = parsed("C:/build/out.obj: C:/src/main.c\n");
  expect(driveLetters && driveLetters->size() == 1 &&
             (*driveLetters)[0] == std::filesystem::path("C:/src/main.c"),
         "Windows drive colons should not terminate the dependency rule");

  const auto noDependencies = parsed("main.o:\n");
  expect(noDependencies && noDependencies->empty(),
         "a rule without prerequisites should parse to an empty list");

  expect(!parsed("no separator here\n") && !errorMessage.empty(),
         "a report without a rule separator should be rejected with a "
         "diagnostic");
  expect(!parsed("main.o: a.h\nphony.h:\n"),
         "an unexpected second rule should be rejected rather than guessed "
         "at");

  TemporaryDirectory temporary;
  const auto classify = [&](std::string_view name, std::string_view contents) {
    const std::filesystem::path path = temporary.root() / name;
    writeFile(path, contents);
    return lang::driver::classifyNativeLinkInput(path);
  };
  expect(classify("regular.a", "!<arch>\ncontents") ==
             lang::driver::NativeLinkInputClass::StaticArchive,
         "a regular ar archive should classify as content-complete");
  expect(classify("thin.a", "!<thin>\ncontents") ==
             lang::driver::NativeLinkInputClass::RequiresDependencyDiscovery,
         "a thin archive references external members and requires discovery");
  const std::string elfRelocatable{
      '\x7f', 'E', 'L', 'F', 2, 1, 1, 0, 0,
      0,      0,   0,   0,   0, 0, 0, 1, 0, /* ET_REL little-endian */
      0x3e,   0};
  expect(classify("object.o", elfRelocatable) ==
             lang::driver::NativeLinkInputClass::RelocatableObject,
         "an ELF relocatable object should classify as content-complete");
  const std::string elfShared{'\x7f', 'E', 'L', 'F', 2, 1, 1, 0, 0,
                              0,      0,   0,   0,   0, 0, 0, 3, 0, /* ET_DYN */
                              0x3e,   0};
  expect(classify("shared.so", elfShared) ==
             lang::driver::NativeLinkInputClass::RequiresDependencyDiscovery,
         "an ELF shared library requires transitive dependency discovery");
  const std::string machOObject{
      '\xcf', '\xfa', '\xed', '\xfe', 12, 0, 0, 1,
      0,      0,      0,      0,      1,  0, 0, 0, /* MH_OBJECT */
      2,      0,      0,      0};
  expect(classify("object.mach.o", machOObject) ==
             lang::driver::NativeLinkInputClass::RelocatableObject,
         "a Mach-O relocatable object should classify as content-complete");
  const std::string machODylib{
      '\xcf', '\xfa', '\xed', '\xfe', 12, 0, 0, 1,
      0,      0,      0,      0,      6,  0, 0, 0, /* MH_DYLIB */
      2,      0,      0,      0};
  expect(classify("shared.dylib", machODylib) ==
             lang::driver::NativeLinkInputClass::RequiresDependencyDiscovery,
         "a Mach-O dynamic library requires transitive dependency discovery");
  expect(classify("script.a", "INPUT(other.o)\n") ==
             lang::driver::NativeLinkInputClass::RequiresDependencyDiscovery,
         "a text linker script must never classify as content-complete");
  expect(
      lang::driver::classifyNativeLinkInput(temporary.root() / "missing.a") ==
          lang::driver::NativeLinkInputClass::Unreadable,
      "a missing link input should classify as unreadable");

  expect(lang::driver::containsTimeSensitivePreprocessorUse(
             "const char *when = __DATE__;") &&
             lang::driver::containsTimeSensitivePreprocessorUse(
                 "#define STAMP __TIMESTAMP__") &&
             lang::driver::containsTimeSensitivePreprocessorUse(
                 "int t = __TIME__[0];") &&
             !lang::driver::containsTimeSensitivePreprocessorUse(
                 "int date = 0; // date and time words alone\n"),
         "the deterministic preprocessing policy should detect exactly the "
         "time-and-date macro spellings");
}

void testNativeLibraryNameResolution() {
  using lang::driver::NativeLibraryResolutionStatus;
  TemporaryDirectory temporary;
  const std::filesystem::path first = temporary.root() / "first";
  const std::filesystem::path second = temporary.root() / "second";
  std::filesystem::create_directories(first);
  std::filesystem::create_directories(second);
  const std::vector<std::filesystem::path> directories{first, second};

#if defined(_WIN32)
  const std::string dynamicName = "libsupport.dll.a";
#elif defined(__APPLE__)
  const std::string dynamicName = "libsupport.dylib";
#else
  const std::string dynamicName = "libsupport.so";
#endif

  const auto resolve = [&](std::string_view name) {
    return lang::driver::resolveNativeLibraryName(name, directories);
  };

  expect(resolve("support").status == NativeLibraryResolutionStatus::Unresolved,
         "a name with no candidate in any declared directory should resolve "
         "outside them and refuse to participate");

  writeFile(second / "libsupport.a", "!<arch>\ncontents");
  const lang::driver::NativeLibraryResolution resolved = resolve("support");
  expect(resolved.status == NativeLibraryResolutionStatus::Resolved &&
             resolved.file == second / "libsupport.a",
         "a single modeled candidate should resolve to that exact file, "
         "skipping earlier directories that hold none");

  writeFile(first / dynamicName, "dynamic");
  const lang::driver::NativeLibraryResolution shadowed = resolve("support");
  expect(shadowed.status == NativeLibraryResolutionStatus::Resolved &&
             shadowed.file == first / dynamicName,
         "the first declared directory holding a candidate should win, as it "
         "does on the link command line");

  writeFile(first / "libsupport.a", "!<arch>\ncontents");
  const lang::driver::NativeLibraryResolution ambiguous = resolve("support");
  expect(ambiguous.status == NativeLibraryResolutionStatus::Ambiguous &&
             ambiguous.directory == first,
         "two modeled candidates in one directory should refuse rather than "
         "guess the linker's preference");

  std::filesystem::remove(first / dynamicName);
  writeFile(first / "libsupport.la", "# libtool archive\n");
  expect(resolve("support").status == NativeLibraryResolutionStatus::Ambiguous,
         "a candidate spelling the driver does not model should make "
         "resolution ambiguous, never invisible");
  std::filesystem::remove(first / "libsupport.la");

  // A neighbouring name must not be mistaken for a candidate.
  writeFile(second / "libsupport_extra.a", "!<arch>\ncontents");
  expect(resolve("support_extra").status ==
                 NativeLibraryResolutionStatus::Resolved &&
             resolve("support").status ==
                 NativeLibraryResolutionStatus::Resolved,
         "a longer library name sharing a prefix should not disturb either "
         "resolution");

  expect(resolve(":libsupport.a").status ==
                 NativeLibraryResolutionStatus::UnsupportedSpelling &&
             resolve("sub/support").status ==
                 NativeLibraryResolutionStatus::UnsupportedSpelling &&
             resolve("").status ==
                 NativeLibraryResolutionStatus::UnsupportedSpelling,
         "exact-name selections and path-bearing spellings mean different "
         "things to different linkers and must not be resolved");

  const std::vector<std::filesystem::path> missingDirectory{temporary.root() /
                                                            "absent"};
  expect(lang::driver::resolveNativeLibraryName("support", missingDirectory)
                 .status == NativeLibraryResolutionStatus::DirectoryUnreadable,
         "a search directory that cannot be enumerated must not be reported "
         "as holding no candidate");
}

void testBuildSourceMaterialIdentityProjection() {
  lang::driver::detail::BuildSourceMaterialProjection baseline;
  baseline.units = {
      {.id = 41,
       .role = lang::SourceUnitRole::Application,
       .logicalIdentity = "package:main.gti",
       .entry = true,
       .bytes = 20,
       .sha256 = std::string(64, '1')},
      {.id = 17,
       .role = lang::SourceUnitRole::Prelude,
       .logicalIdentity = "prelude:first",
       .prelude = true,
       .bytes = 10,
       .sha256 = std::string(64, '2')},
      {.id = 23,
       .role = lang::SourceUnitRole::Prelude,
       .logicalIdentity = "prelude:second",
       .prelude = true,
       .bytes = 11,
       .sha256 = std::string(64, '3')},
  };
  baseline.preludeRoots = {"prelude:first", "prelude:second"};
  baseline.dependencyEdges = {
      "package:main.gti\nprelude:first\n1",
      "package:main.gti\nprelude:second\n1",
  };
  const std::string baselineIdentity =
      lang::driver::detail::buildSourceMaterialsSha256(baseline);

  lang::driver::detail::BuildSourceMaterialProjection relocated = baseline;
  relocated.units[0].id = 1;
  relocated.units[1].id = 2;
  relocated.units[2].id = 3;
  expect(lang::driver::detail::buildSourceMaterialsSha256(relocated) ==
             baselineIdentity,
         "source material identity should exclude ephemeral unit identifiers");

  lang::driver::detail::BuildSourceMaterialProjection contentChanged = baseline;
  contentChanged.units[0].sha256 = std::string(64, '4');
  expect(lang::driver::detail::buildSourceMaterialsSha256(contentChanged) !=
             baselineIdentity,
         "source material identity should change with consumed content");

  lang::driver::detail::BuildSourceMaterialProjection rootsReordered = baseline;
  std::reverse(rootsReordered.preludeRoots.begin(),
               rootsReordered.preludeRoots.end());
  expect(lang::driver::detail::buildSourceMaterialsSha256(rootsReordered) !=
             baselineIdentity,
         "source material identity should preserve prelude-root order");

  lang::driver::detail::BuildSourceMaterialProjection edgeChanged = baseline;
  edgeChanged.dependencyEdges.back() = "prelude:first\nprelude:second\n0";
  std::sort(edgeChanged.dependencyEdges.begin(),
            edgeChanged.dependencyEdges.end());
  expect(lang::driver::detail::buildSourceMaterialsSha256(edgeChanged) !=
             baselineIdentity,
         "source material identity should retain logical dependency edges");
}

void testBuildProvenance(const std::filesystem::path &testExecutable) {
  TemporaryDirectory temporary;
  const lang::driver::ToolchainLayout discovered =
      lang::driver::discoverToolchainLayout(testExecutable.string().c_str());
  const std::filesystem::path runtimeInclude = temporary.root() / "runtime";
  const std::filesystem::path runtimeLibrary =
      temporary.root() / discovered.runtimeLibrary.filename();
  std::error_code copyError;
  std::filesystem::copy(discovered.runtimeInclude, runtimeInclude,
                        std::filesystem::copy_options::recursive, copyError);
  if (!copyError) {
    std::filesystem::copy_file(discovered.runtimeLibrary, runtimeLibrary,
                               copyError);
  }
  if (copyError) {
    expect(false, "build-provenance runtime fixtures should be copyable: " +
                      copyError.message());
    return;
  }

  const std::filesystem::path project = temporary.root() / "project";
  const std::filesystem::path dependency = project / "dependency.gti";
  const std::filesystem::path entry = project / "main.gti";
  std::filesystem::create_directories(project);
  expect(writeFile(dependency, "export module dependency;\nexport int "
                               "dependency_value() { return 1; }\n") &&
             writeFile(entry,
                       "import \"dependency.gti\";\n"
                       "expected<int, int> calculated() { return 1; }\n"
                       "int main() {\n"
                       "  expected<int, int> value = calculated();\n"
                       "  if (!value) { return value.error(); }\n"
                       "  return dependency_value() + value.value() - 2;\n"
                       "}\n"),
         "build-provenance GTI sources should be writable");

  const lang::driver::ToolchainLayout toolchain{
      .standardLibrary = discovered.standardLibrary,
      .runtimeInclude = runtimeInclude,
      .runtimeLibrary = runtimeLibrary,
  };
  lang::driver::NativeInputs nestedNativeInputs;
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
  nestedNativeInputs.compilerArguments = {"-fsanitize=address,undefined",
                                          "-fno-omit-frame-pointer"};
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
  if (nestedNativeInputs.compilerArguments.empty()) {
    nestedNativeInputs.compilerArguments = {"-fsanitize=address,undefined",
                                            "-fno-omit-frame-pointer"};
  }
#endif
  const auto build = [&](std::string_view suffix,
                         const std::filesystem::path &buildEntry,
                         const lang::driver::ToolchainLayout &buildToolchain,
                         lang::CppStandard standard,
                         lang::ConfigurationFlags flags = {}) {
    const std::filesystem::path generated =
        temporary.root() / ("generated-" + std::string(suffix) + ".o");
    const std::filesystem::path output =
        temporary.root() / ("program-" + std::string(suffix));
    const std::filesystem::path report =
        temporary.root() / ("report-" + std::string(suffix) + ".json");
    return lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
        lang::driver::CompilationRequest(
            buildEntry, buildToolchain.standardLibrary,
            lang::TargetInfo::host(), lang::OptimizationLevel::O0, standard, {},
            std::move(flags))
            .withBackend(lang::BackendKind::Llvm),
        buildToolchain, generated, output,
        lang::driver::discoverNativeCompiler(), nestedNativeInputs, false, true,
        false, std::nullopt, std::nullopt, std::nullopt,
        lang::driver::BuildReportRequest{
            .output = report,
            .producerExecutable = testExecutable,
        }));
  };

  const lang::driver::ExecutableBuildResult first =
      build("first", entry, toolchain, lang::CppStandard::Cpp23);
  expect(first.succeeded() && first.provenance.has_value(),
         "an opted-in executable build should capture structured provenance "
         "(status=" +
             std::to_string(static_cast<int>(first.status)) + ", detail=" +
             first.driverDiagnostic.value_or("none") + ", native output=" +
             (first.nativeProcess ? first.nativeProcess->output : "none") +
             ")");
  if (!first.provenance) {
    return;
  }
  const lang::driver::BuildProvenance &firstProvenance = *first.provenance;
  const bool hasDependency = std::any_of(
      firstProvenance.source.units.begin(), firstProvenance.source.units.end(),
      [&](const lang::driver::BuildSourceUnitProvenance &unit) {
        return unit.material.canonicalPath ==
                   std::filesystem::weakly_canonical(dependency) &&
               !unit.material.sha256.empty();
      });
  // The LLVM object consumes the runtime archive only; runtime-header
  // provenance belongs to the C++ backend (cpp_mir_driver).
  expect(hasDependency &&
             firstProvenance.runtime.archive.canonicalPath ==
                 std::filesystem::weakly_canonical(runtimeLibrary) &&
             firstProvenance.producerExecutable.canonicalPath ==
                 std::filesystem::weakly_canonical(testExecutable) &&
             !firstProvenance.nativeCompilerExecutable.sha256.empty() &&
             !firstProvenance.nativeCommand.empty(),
         "build provenance should retain exact loaded source, used runtime, "
         "producer, native compiler, and native argv facts");
  expect((firstProvenance.compilerBuild.state ==
          lang::driver::CompilerSourceState::Unavailable) ==
             !firstProvenance.compilerBuild.commit.has_value(),
         "compiler build identity should claim a commit only with established "
         "clean/dirty state");

  const std::filesystem::path relocatedProject =
      temporary.root() / "relocated/project";
  const std::filesystem::path relocatedDependency =
      relocatedProject / dependency.filename();
  const std::filesystem::path relocatedEntry =
      relocatedProject / entry.filename();
  std::filesystem::create_directories(relocatedProject);
  copyError.clear();
  std::filesystem::copy_file(dependency, relocatedDependency, copyError);
  if (!copyError) {
    std::filesystem::copy_file(entry, relocatedEntry, copyError);
  }
  expect(!copyError, "relocated provenance sources should be copyable");
  const lang::driver::ExecutableBuildResult relocatedSource = build(
      "relocated-source", relocatedEntry, toolchain, lang::CppStandard::Cpp23);
  const bool relocatedPathRecorded =
      relocatedSource.provenance &&
      std::any_of(relocatedSource.provenance->source.units.begin(),
                  relocatedSource.provenance->source.units.end(),
                  [&](const lang::driver::BuildSourceUnitProvenance &unit) {
                    return unit.material.canonicalPath ==
                           std::filesystem::weakly_canonical(relocatedEntry);
                  });
  expect(relocatedSource.succeeded() && relocatedSource.provenance &&
             relocatedSource.provenance->source.materialsSha256 ==
                 firstProvenance.source.materialsSha256 &&
             relocatedPathRecorded,
         "moving an identical logical source package should preserve its "
         "aggregate while recording the new canonical paths");

  const std::filesystem::path relocatedRuntimeInclude =
      temporary.root() / "relocated-toolchain/include";
  const std::filesystem::path relocatedRuntimeLibrary =
      temporary.root() / "relocated-toolchain" / runtimeLibrary.filename();
  std::filesystem::create_directories(relocatedRuntimeLibrary.parent_path());
  copyError.clear();
  std::filesystem::copy(runtimeInclude, relocatedRuntimeInclude,
                        std::filesystem::copy_options::recursive, copyError);
  if (!copyError) {
    std::filesystem::copy_file(runtimeLibrary, relocatedRuntimeLibrary,
                               copyError);
  }
  expect(!copyError, "relocated runtime materials should be copyable");
  const lang::driver::ToolchainLayout relocatedToolchain{
      .standardLibrary = toolchain.standardLibrary,
      .runtimeInclude = relocatedRuntimeInclude,
      .runtimeLibrary = relocatedRuntimeLibrary,
  };
  const lang::driver::ExecutableBuildResult relocatedRuntime = build(
      "relocated-runtime", entry, relocatedToolchain, lang::CppStandard::Cpp23);
  expect(relocatedRuntime.succeeded() && relocatedRuntime.provenance &&
             relocatedRuntime.provenance->runtime.materialsSha256 ==
                 firstProvenance.runtime.materialsSha256 &&
             relocatedRuntime.provenance->runtime.archive.canonicalPath ==
                 std::filesystem::weakly_canonical(relocatedRuntimeLibrary),
         "moving identical runtime headers and archive should preserve their "
         "aggregate while recording new canonical paths");

  expect(writeFile(dependency, "export module dependency;\nexport int "
                               "dependency_value() { return 2; }\n"),
         "the included provenance source should be editable");
  const lang::driver::ExecutableBuildResult sourceChanged =
      build("source", entry, toolchain, lang::CppStandard::Cpp23);
  expect(sourceChanged.succeeded() && sourceChanged.provenance &&
             sourceChanged.provenance->source.materialsSha256 !=
                 firstProvenance.source.materialsSha256 &&
             sourceChanged.provenance->runtime.materialsSha256 ==
                 firstProvenance.runtime.materialsSha256,
         "changing a consumed included GTI source should alter only source "
         "material provenance");
}

void testBuildReportRecordRoundTrip() {
  lang::driver::BuildProvenance recorded;
  recorded.debugInfo = true;
  recorded.rawPointerAudit = lang::RawPointerAuditMode::Strict;
  recorded.producerExecutable = {.canonicalPath = "/opt/gti/bin/gti",
                                 .bytes = 128,
                                 .sha256 = std::string(64, 'a')};
  recorded.compilerBuild = {.state = lang::driver::CompilerSourceState::Dirty,
                            .commit = std::string(40, 'b')};
  recorded.project = lang::driver::BuildProjectProvenance{
      .packageName = "sample",
      .packageVersion = "0.1.0",
      .targetName = "sample",
      .targetKind = "executable",
      .profileName = "dev",
      .targetTriple = "arm64-apple-macos",
      // A multi-line identity exercises string escaping in both directions.
      .modelIdentity = "gti-workspace-v1\npackage:sample@0.1.0"};
  recorded.source.entryRoot = 1;
  recorded.source.preludeRoots = {2};
  recorded.source.compilationOrder = {2, 1};
  recorded.source.materialsSha256 = std::string(64, 'c');
  recorded.source.units.push_back(
      {.id = 1,
       .role = lang::SourceUnitRole::Application,
       .material = {.canonicalPath = "/work/sample/src/main.gti",
                    .bytes = 25,
                    .sha256 = std::string(64, 'd')},
       .logicalIdentity = "package:sample@0.1.0:src/main.gti",
       .standardLibraryName = std::nullopt,
       .packageIdentity = "sample@0.1.0",
       .packageRelativePath = "src/main.gti",
       .entry = true,
       .prelude = false});
  recorded.source.units.push_back(
      {.id = 2,
       .role = lang::SourceUnitRole::Prelude,
       .material = {.canonicalPath = "/opt/gti/stdlib/prelude.gti",
                    .bytes = 64,
                    .sha256 = std::string(64, 'e')},
       .logicalIdentity = "prelude",
       .standardLibraryName = std::nullopt,
       .packageIdentity = std::nullopt,
       .packageRelativePath = std::nullopt,
       .entry = false,
       .prelude = true});
  recorded.source.dependencies.push_back(
      {.source = 1,
       .target = 2,
       .kind = lang::SourceDependencyKind::Prelude,
       .includeSpelling = "prelude.gti",
       .includeOccurrence = 0});
  for (const auto kind : {lang::SourceDependencyKind::Import,
                          lang::SourceDependencyKind::ExportImport}) {
    recorded.source.dependencies.push_back(
        {.source = 1,
         .target = 2,
         .kind = kind,
         .includeSpelling = "dependency.main",
         .includeOccurrence = 1});
  }
  recorded.nativeCompilerExecutable = {.canonicalPath = "/usr/bin/c++",
                                       .bytes = 512,
                                       .sha256 = std::string(64, 'f')};
  recorded.nativeCommand = {
      "c++",
      "-std=c++23",
      "-O0",
      "-MD",
      "-MF",
      "/tmp/gti-1-2-3-sample.build-report.d",
      "/work/sample/build/gti/sample.gti.cpp",
      "-o",
      "/work/sample/build/gti/.sample.gti-stage-1-2-3/sample"};
  recorded.runtime.materialsSha256 = std::string(64, '0');
  recorded.runtime.headers.push_back(
      {.kind = lang::driver::BuildRuntimeHeaderKind::Runtime,
       .logicalIdentity = "gti/runtime.h",
       .material = {.canonicalPath = "/opt/gti/include/gti/runtime.h",
                    .bytes = 900,
                    .sha256 = std::string(64, '1')}});
  recorded.runtime.headers.push_back(
      {.kind = lang::driver::BuildRuntimeHeaderKind::Runtime,
       .logicalIdentity = "gti/runtime_failure.h",
       .material = {.canonicalPath = "/opt/gti/include/gti/runtime_failure.h",
                    .bytes = 300,
                    .sha256 = std::string(64, '2')}});
  recorded.runtime.archive = {.canonicalPath = "/opt/gti/lib/libgti_runtime.a",
                              .bytes = 4096,
                              .sha256 = std::string(64, '3')};
  recorded.artifact = {.canonicalPath = "/work/sample/build/gti/sample",
                       .bytes = 58008,
                       .sha256 = std::string(64, '4')};

  const std::string serialized =
      lang::driver::serializeBuildProvenanceJson(recorded);
  expect(serialized.find("\"target_kind\":\"executable\"") != std::string::npos,
         "a project record should serialize its resolved target kind");
  expect(serialized.find("gti-workspace-v1\\npackage:sample@0.1.0") !=
             std::string::npos,
         "a multi-line model identity should be escaped, not truncated");
  std::string parseError;
  const std::optional<lang::driver::BuildProvenance> parsed =
      lang::driver::parseBuildProvenanceJson(serialized, parseError);
  expect(parsed.has_value(),
         "a serialized build report should parse back: " + parseError);
  if (!parsed) {
    return;
  }
  expect(lang::driver::compareBuildProvenance(recorded, *parsed).empty(),
         "a round-tripped record should compare equal to its source");
  expect(lang::driver::serializeBuildProvenanceJson(*parsed) == serialized,
         "re-serializing a parsed record should reproduce the sidecar");
  for (const auto *retired :
       {"include", "export-include", "standard-library", "package"}) {
    std::string legacy = serialized;
    const std::string currentKind = "\"kind\":\"import\"";
    const auto position = legacy.find(currentKind);
    expect(position != std::string::npos,
           "the report fixture contains an import edge");
    if (position == std::string::npos)
      continue;
    legacy.replace(position, currentKind.size(),
                   "\"kind\":\"" + std::string(retired) + "\"");
    expect(!lang::driver::parseBuildProvenanceJson(legacy, parseError) &&
               parseError.find("unknown source dependency kind") !=
                   std::string::npos,
           "retired source-edge spellings are not valid module build records");
  }
  auto embedding = recorded;
  embedding.project->targetKind = "library";
  embedding.embeddingHeader = recorded.artifact;
  embedding.embeddingHeader->canonicalPath = "/work/sample/build/gti/module.h";
  const auto embeddingText =
      lang::driver::serializeBuildProvenanceJson(embedding);
  const auto embeddingParsed =
      lang::driver::parseBuildProvenanceJson(embeddingText, parseError);
  expect(embeddingParsed &&
             lang::driver::compareBuildProvenance(embedding, *embeddingParsed)
                 .empty(),
         "library reports preserve the matching header identity");
  auto changedHeader = embedding;
  changedHeader.embeddingHeader->bytes += 1;
  expect(
      !lang::driver::compareBuildProvenance(embedding, changedHeader).empty(),
      "verification detects embedding companion drift");
  changedHeader.embeddingHeader.reset();
  expect(!lang::driver::parseBuildProvenanceJson(
             lang::driver::serializeBuildProvenanceJson(changedHeader),
             parseError),
         "library reports cannot omit their required header companion");
  embedding.nativeCommand = {"clang++",
                             "/stage/one/module.o",
                             "-dynamiclib",
                             "-Xlinker",
                             "-exported_symbols_list",
                             "-Xlinker",
                             "/stage/one/exports.txt",
                             "-o",
                             "/stage/one/libmodule.dylib"};
  auto relocatedEmbedding = embedding;
  relocatedEmbedding.nativeCommand[1] = "/stage/two/module.o";
  relocatedEmbedding.nativeCommand[6] = "/stage/two/exports.txt";
  relocatedEmbedding.nativeCommand[8] = "/stage/two/libmodule.dylib";
  expect(
      lang::driver::compareBuildProvenance(embedding, relocatedEmbedding)
          .empty(),
      "embedding report replay normalizes only its private generation inputs");
  relocatedEmbedding.nativeCommand[6] = "/user/exports.txt";
  expect(!lang::driver::compareBuildProvenance(embedding, relocatedEmbedding)
              .empty(),
         "embedding replay preserves user export-map identity");
  relocatedEmbedding = embedding;
  relocatedEmbedding.nativeCommand[1] = "/stage/one/other.o";
  expect(!lang::driver::compareBuildProvenance(embedding, relocatedEmbedding)
              .empty(),
         "embedding replay preserves other native object operands");
  relocatedEmbedding = embedding;
  relocatedEmbedding.nativeCommand[4] = "--other-linker-policy";
  expect(!lang::driver::compareBuildProvenance(embedding, relocatedEmbedding)
              .empty(),
         "embedding replay preserves linker policy");
  expect(parsed->debugInfo, "build reports should preserve debug policy");
  expect(parsed->rawPointerAudit == lang::RawPointerAuditMode::Strict,
         "build reports preserve raw pointer audit policy");
  auto changedAudit = *parsed;
  changedAudit.rawPointerAudit = lang::RawPointerAuditMode::Off;
  expect(!lang::driver::compareBuildProvenance(recorded, changedAudit).empty(),
         "verification detects audit policy changes");
  std::string oldReport = serialized;
  const auto auditStart = oldReport.find("  \"raw_pointer_audit\":");
  oldReport.erase(auditStart,
                  oldReport.find('\n', auditStart) - auditStart + 1);
  const auto oldParsed =
      lang::driver::parseBuildProvenanceJson(oldReport, parseError);
  expect(oldParsed &&
             oldParsed->rawPointerAudit == lang::RawPointerAuditMode::Off,
         "reports without the additive audit field default off");
  auto changedDebug = *parsed;
  changedDebug.debugInfo = false;
  expect(!lang::driver::compareBuildProvenance(recorded, changedDebug).empty(),
         "debug policy drift must be observable to verification");

  // The driver owns the staged link output and the dependency-file operand,
  // and both carry a per-invocation identity. Comparing them literally would
  // report a difference for every verification.
  lang::driver::BuildProvenance restaged = *parsed;
  restaged.nativeCommand[5] = "/tmp/gti-9-9-9-sample.build-report.d";
  restaged.nativeCommand[8] =
      "/work/sample/build/gti/.sample.gti-stage-9-9-9/sample";
  expect(lang::driver::compareBuildProvenance(recorded, restaged).empty(),
         "per-invocation staging paths should not read as a divergence");

  lang::driver::BuildProvenance drifted = *parsed;
  drifted.source.units[0].material.sha256 = std::string(64, '9');
  drifted.source.units[0].material.bytes = 59;
  drifted.source.materialsSha256 = std::string(64, '8');
  drifted.runtime.headers.pop_back();
  drifted.nativeCommand[2] = "-O2";
  drifted.artifact.sha256 = std::string(64, '7');
  drifted.project->profileName = "release";
  // A record built for another target describes another build, even when
  // every source unit and command operand is unchanged.
  drifted.project->targetTriple = "x86_64-unknown-linux-gnu";
  const std::vector<lang::driver::BuildProvenanceDifference> differences =
      lang::driver::compareBuildProvenance(recorded, drifted);
  const auto named = [&](std::string_view subject, std::string_view field) {
    return std::any_of(
        differences.begin(), differences.end(),
        [&](const lang::driver::BuildProvenanceDifference &difference) {
          return difference.subject.find(subject) != std::string::npos &&
                 difference.field == field;
        });
  };
  expect(named("package:sample@0.1.0:src/main.gti", "content"),
         "a changed source unit should be named with its content identity");
  expect(named("project", "target triple"),
         "a record built for another target should diverge on its triple");
  expect(named("source materials", "aggregate sha256"),
         "the source aggregate should be compared");
  expect(named("gti/runtime_failure.h", "presence"),
         "a runtime header the rebuild no longer consumes should be named");
  expect(named("native argv[2]", "operand"),
         "a changed native operand should be named by position");
  expect(named("built artifact", "content"),
         "the produced artifact's bytes should be compared");
  expect(named("project", "profile name"),
         "the recorded profile selection should be compared");
  const bool preludeUntouched = std::none_of(
      differences.begin(), differences.end(),
      [](const lang::driver::BuildProvenanceDifference &difference) {
        return difference.subject.find("prelude") != std::string::npos;
      });
  expect(preludeUntouched,
         "an unchanged source unit should not be reported as a difference");

  // Canonical material paths are context rather than identity. This does not
  // relax path operands in the separately compared native command.
  lang::driver::BuildProvenance relocated = *parsed;
  relocated.source.units[0].material.canonicalPath =
      "/elsewhere/sample/src/main.gti";
  relocated.artifact.canonicalPath = "/elsewhere/sample/build/gti/sample";
  expect(lang::driver::compareBuildProvenance(recorded, relocated).empty(),
         "identical content at a new canonical path should compare equal");

  std::string rejectedError;
  std::string duplicateSchema = serialized;
  duplicateSchema.insert(duplicateSchema.find('\n') + 1, "  \"schema\": 2,\n");
  expect(!lang::driver::parseBuildProvenanceJson(duplicateSchema, rejectedError)
                 .has_value() &&
             rejectedError.find("duplicate object member 'schema'") !=
                 std::string::npos,
         "an ambiguous report with duplicate object members should be "
         "rejected");

  expect(
      !lang::driver::parseBuildProvenanceJson("{\"schema\": 1}", rejectedError)
              .has_value() &&
          rejectedError.find("schema") != std::string::npos,
      "an older schema should be refused rather than partially read");
  expect(
      !lang::driver::parseBuildProvenanceJson(serialized + "{", rejectedError)
           .has_value(),
      "trailing content should be refused");
  expect(!lang::driver::parseBuildProvenanceJson("", rejectedError).has_value(),
         "an empty report should be refused");
}

void testResourcesAndArtifactOwnership() {
  TemporaryDirectory temporary;
  const std::filesystem::path include = temporary.root() / "include";
  const std::filesystem::path runtime = temporary.root() / "libgti_runtime.a";
  std::filesystem::create_directories(include / "gti");
  expect(writeFile(include / "gti/runtime.hpp", ""),
         "the runtime header fixture should be writable");
  expect(writeFile(include / "gti/runtime.h", ""),
         "the C runtime header fixture should be writable");
  expect(writeFile(runtime, ""),
         "the runtime library fixture should be writable");

  lang::driver::ToolchainLayout layout{
      .standardLibrary = lang::standardLibraryLayout(temporary.root()),
      .runtimeInclude = include,
      .runtimeLibrary = runtime,
  };
  expect(lang::driver::validateToolchainLayout(layout) ==
             lang::driver::ToolchainResourceError::RuntimeFilesMissing,
         "runtime validation should require the installed public C ABI header");
  expect(writeFile(include / "gti/c_abi.h", ""),
         "the C ABI header fixture should be writable");
  expect(lang::driver::validateToolchainLayout(layout) ==
             lang::driver::ToolchainResourceError::RuntimeFilesMissing,
         "runtime validation should require the defined-failure ABI header");
  expect(writeFile(include / "gti/runtime_failure.h", ""),
         "the defined-failure ABI header fixture should be writable");
  expect(lang::driver::validateToolchainLayout(layout) ==
             lang::driver::ToolchainResourceError::RuntimeFilesMissing,
         "runtime validation should require the managed-thread C ABI header");
  expect(writeFile(include / "gti/runtime_thread.h", ""),
         "the managed-thread C ABI header fixture should be writable");
  expect(lang::driver::validateToolchainLayout(layout) ==
             lang::driver::ToolchainResourceError::RuntimeFilesMissing,
         "runtime validation should require the managed-thread C++ bridge");
  expect(writeFile(include / "gti/runtime_thread.hpp", ""),
         "the managed-thread C++ bridge fixture should be writable");
  expect(!lang::driver::validateToolchainLayout(layout),
         "resource validation should accept the complete runtime surface");

  std::error_code resourceError;
  std::filesystem::remove(runtime, resourceError);
  resourceError.clear();
  std::filesystem::create_directory(runtime, resourceError);
  expect(!resourceError &&
             lang::driver::validateToolchainLayout(layout) ==
                 lang::driver::ToolchainResourceError::RuntimeFilesMissing,
         "runtime validation should reject a directory where the runtime "
         "archive must be a regular file");

  const std::filesystem::path removed = temporary.root() / "removed.cpp";
  expect(lang::driver::writeArtifact(removed, "generated") ==
                 lang::driver::ArtifactWriteStatus::Success &&
             readFile(removed) == "generated",
         "artifact writes should report success and preserve contents");
  lang::SourceManager loadedSources;
  loadedSources.set(removed.string(), "generated");
  const std::filesystem::path hardLink = temporary.root() / "source-alias";
  std::error_code hardLinkError;
  std::filesystem::create_hard_link(removed, hardLink, hardLinkError);
  if (!hardLinkError) {
    expect(lang::driver::findLoadedSourceCollision(hardLink, loadedSources) ==
               std::optional<std::filesystem::path>(removed),
           "artifact collision checks should recognize hard-link aliases of "
           "loaded sources");
  }
  {
    lang::driver::TemporaryArtifact artifact(removed, true);
  }
  expect(!std::filesystem::exists(removed),
         "temporary artifacts should be removed at scope exit");

  const std::filesystem::path retained = temporary.root() / "retained.cpp";
  expect(writeFile(retained, "generated"),
         "the retained artifact fixture should be writable");
  {
    lang::driver::TemporaryArtifact artifact(retained, true);
    artifact.keep();
  }
  expect(std::filesystem::exists(retained),
         "explicitly retained artifacts should survive scope exit");

  const std::filesystem::path firstTemporary =
      lang::driver::temporaryCppPath(temporary.root() / "main.gti");
  const std::filesystem::path secondTemporary =
      lang::driver::temporaryCppPath(temporary.root() / "main.gti");
  expect(firstTemporary != secondTemporary,
         "temporary artifact paths should be process-unique");
  expect(firstTemporary.extension() == ".cpp",
         "temporary C++ artifacts should retain their backend extension");
  const std::filesystem::path preprocessedTemporary =
      lang::driver::temporaryArtifactPath(temporary.root() / "native.h", ".i");
  expect(preprocessedTemporary.extension() == ".i" &&
             preprocessedTemporary != firstTemporary,
         "generic temporary artifacts should be unique and retain the "
         "requested extension");
  std::error_code firstError;
  std::error_code temporaryError;
  const std::filesystem::path canonicalParent =
      std::filesystem::weakly_canonical(firstTemporary.parent_path(),
                                        firstError);
  const std::filesystem::path canonicalTemporary =
      std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),
                                        temporaryError);
  expect(!firstError && !temporaryError &&
             canonicalParent == canonicalTemporary,
         "temporary artifacts should stay in the system temporary directory");

  const std::filesystem::path published = temporary.root() / "program.exe";
  const std::filesystem::path firstStaged =
      lang::driver::stagedArtifactPath(published);
  const std::filesystem::path secondStaged =
      lang::driver::stagedArtifactPath(published);
  expect(firstStaged != secondStaged,
         "staged artifact paths should be process-unique");
  expect(firstStaged.parent_path() == published.parent_path() &&
             firstStaged.extension() == published.extension() &&
             firstStaged.filename().string().starts_with(".program.gti-stage-"),
         "native outputs should stage beside the destination and preserve its "
         "extension");
  expect(writeFile(published, "previous") &&
             writeFile(firstStaged, "replacement"),
         "publication fixtures should be writable");
  const lang::driver::ArtifactPublishResult publication =
      lang::driver::publishArtifact(firstStaged, published);
  expect(publication.succeeded() && readFile(published) == "replacement" &&
             !std::filesystem::exists(firstStaged),
         "successful publication should atomically replace the destination");

  const lang::driver::ArtifactPublishResult missingPublication =
      lang::driver::publishArtifact(temporary.root() / "missing-stage",
                                    published);
  expect(!missingPublication.succeeded() &&
             readFile(published) == "replacement",
         "failed publication should preserve the previous destination");
}

} // namespace

int main(int argc, char *argv[]) {
  lang::installCrashHandlers(argc > 0 ? argv[0] : "gti_driver_tests");
  if (const std::optional<int> wrapped = runNativeCompilerWrapper(argc, argv))
    return *wrapped;
  if (argc > 1 && std::string_view(argv[1]) == "--process-child") {
    if (argc != 5 || std::string_view(argv[2]) != "alpha" ||
        std::string_view(argv[3]) != "two words" ||
        std::string_view(argv[4]) != "") {
      return 31;
    }
    std::cout << "process arguments preserved\n";
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--process-child-exit") {
    return 23;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--process-child-context") {
    std::cout << std::filesystem::current_path().string() << '\n';
    const char *value = std::getenv("GTI_DRIVER_PROCESS_TEST");
    std::cout << (value != nullptr ? value : "<missing>") << '\n';
    return 0;
  }
  if (argc > 1 && std::string_view(argv[1]) == "--render-command-child") {
    const std::vector<std::string_view> expected{
        "plain",
        "safe/path-1.2_@%+=:,",
        "two words",
        "tab\tvalue",
        "",
        "a\\b",
        "$HOME",
        "`command`",
        "x;y",
        "a'b",
        "line\nbreak",
        "*.cpp",
        "\"quoted\"",
        "left&right",
        "left|right",
        "<input>",
        "(group)",
        "question?",
    };
    if (argc != static_cast<int>(expected.size()) + 2) {
      return 32;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
      if (std::string_view(argv[index + 2]) != expected[index]) {
        return 33;
      }
    }
    std::cout << "rendered arguments preserved\n";
    return 0;
  }

  testAnalysisConfigurationResolution();
  testCompilationRequestAndTargetPropagation();
  testDefinitionClosureWithoutEntryPoint();
  testPackageWarningPolicy();
  testBackendExceptionBoundary();
  testProcessInvocation(std::filesystem::absolute(argv[0]));
  testNativeCommandConstruction();
  testRenderedCommandReplay(std::filesystem::absolute(argv[0]));
  testNativeCCompilerFailure();
  testNativeCppCompilerFailure();
  testOrderedExecutableBuildCommand();
  testDiagnosticLogs();
  testManagedOutputSafety();
  testNativeDependencyDiscoveryPrimitives();
  testNativeLibraryNameResolution();
  testBuildSourceMaterialIdentityProjection();
  testBuildProvenance(std::filesystem::absolute(argv[0]));
  testBuildReportRecordRoundTrip();
  testResourcesAndArtifactOwnership();

  if (failures != 0) {
    std::cerr << failures << " driver test(s) failed\n";
    return 1;
  }
  std::cout << "All driver tests passed\n";
  return 0;
}
