// Driver behaviour of the frozen C++ backend (CTest cpp_mir_driver): the C++
// request default, C++ text emission through the driver, the C++ native
// whole-program build cache (schema v4) and the provenance of a generated-C++
// native compile (runtime headers, depfile argv). The backend-neutral driver
// contracts stay in driver_tests.cpp (driver_pipeline), on LLVM.
#include "../src/driver/build_source_identity.h"
#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/build_report.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"
#include "gti/driver/project.h"
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
             request.backend() == lang::BackendKind::Cpp &&
             !request.optimizationOutputs().requested(),
         "compilation requests should retain target and backend policy");

  const lang::driver::CompilationResult result =
      lang::driver::compileToCpp(request);
  expect(result.succeeded(),
         "the driver should compile a valid whole-program request");
  const lang::driver::CompilationResult selectedCpp =
      lang::driver::compileToSelectedBackend(request);
  expect(selectedCpp.succeeded() && selectedCpp.artifact && result.artifact &&
             selectedCpp.artifact->contents == result.artifact->contents,
         "the selected executable backend should default to C++");

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
  expect(request.backend() == lang::BackendKind::Cpp &&
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
  RecordingProgress progress;
  const auto withProgress = lang::driver::compileToCpp(request, &progress);
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
  const auto failed = lang::driver::compileToCpp(
      lang::driver::CompilationRequest(broken, request.standardLibrary(),
                                       target, request.optimization(),
                                       request.cppStandard()),
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
  if (result.artifact) {
    expect(
        result.artifact->contents.find(
            "// GTI verified-MIR body: scalar-cfg-v1") != std::string::npos &&
            result.artifact->contents.find("static_cast<std::int32_t>(41)") !=
                std::string::npos &&
            result.artifact->contents.find("static_cast<std::int32_t>(82)") ==
                std::string::npos,
        "one resolved target must reach frontend conditionals and backend "
        "emission");
  }

  const lang::driver::CompilationRequest observedRequest(
      source, lang::standardLibraryLayout(temporary.root()), target,
      lang::OptimizationLevel::O1, lang::CppStandard::Cpp23, {}, {},
      {.capturePreMir = true, .capturePostMir = true, .captureReport = true});
  const lang::driver::CompilationResult observed =
      lang::driver::compileToCpp(observedRequest);
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
      lang::driver::compileToCpp(lang::driver::CompilationRequest(
          invalidSource, lang::standardLibraryLayout(temporary.root()), target,
          lang::OptimizationLevel::O0, lang::CppStandard::Cpp23));
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
  const lang::driver::CompilationRequest fragment(
      source, lang::standardLibraryLayout(temporary.root()),
      lang::TargetInfo::host(), lang::OptimizationLevel::O0,
      lang::CppStandard::Cpp23);
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
        loaded ? lang::driver::compileToCpp(
                     fragment, lang::driver::loadCompilationInputs(fragment))
               : lang::driver::compileToCpp(fragment);
    expect(result.status == lang::driver::CompilationStatus::FrontendFailure &&
               !result.artifact && result.diagnostics.size() == 1 &&
               result.diagnostics.front().code == "GTI-S2106" &&
               result.diagnostics.front().primary.start ==
                   missing.find("missing(4)"),
           "both C++ entry paths reject the selected call before backend work");
  }
  expect(writeFile(source,
                   "int unused(int value);\nint answer() { return 42; }\n"),
         "unused declaration control is writable");
  expect(lang::driver::compileToCpp(fragment).succeeded(),
         "C++ emission permits unused declarations and needs no main");
  expect(writeFile(source, "extern \"C\" { int abs(int value); }\n"
                           "int answer() { return abs(-7); }\n"),
         "foreign definition control is writable");
  expect(lang::driver::compileToCpp(fragment).succeeded(),
         "native linkage retains its independent definition contract");
}

void testWholeProgramBuildCache(const std::filesystem::path &testExecutable) {
  ScopedEnvironment clearCpath("CPATH");
  ScopedEnvironment clearCIncludePath("C_INCLUDE_PATH");
  ScopedEnvironment clearCxxIncludePath("CPLUS_INCLUDE_PATH");
  ScopedEnvironment clearObjcIncludePath("OBJC_INCLUDE_PATH");
  ScopedEnvironment clearLibraryPath("LIBRARY_PATH");
  ScopedEnvironment clearCompilerPath("COMPILER_PATH");
  ScopedEnvironment clearGccExecPrefix("GCC_EXEC_PREFIX");
  ScopedEnvironment clearSdkRoot("SDKROOT");
  ScopedEnvironment clearDeveloperDirectory("DEVELOPER_DIR");
  ScopedEnvironment clearLdLibraryPath("LD_LIBRARY_PATH");
  ScopedEnvironment clearDyldLibraryPath("DYLD_LIBRARY_PATH");
  ScopedEnvironment clearDyldFrameworkPath("DYLD_FRAMEWORK_PATH");
  ScopedEnvironment clearDyldFallbackFrameworkPath(
      "DYLD_FALLBACK_FRAMEWORK_PATH");
  ScopedEnvironment clearMsvcIncludePath("INCLUDE");
  ScopedEnvironment clearMsvcLibraryPath("LIB");
  expect(std::getenv("COMPILER_PATH") == nullptr &&
             std::getenv("GCC_EXEC_PREFIX") == nullptr,
         "the cache fixture should remove compiler search-path variables "
         "rather than passing empty overrides to GCC");

  TemporaryDirectory temporary;
  const std::filesystem::path project = temporary.root() / "project";
  const std::filesystem::path managedRoot = project / "build/gti";
  const std::filesystem::path generated =
      managedRoot / "dev/host/intermediate/app.gti.cpp";
  const std::filesystem::path output = managedRoot / "dev/host/app";
  const std::filesystem::path source = project / "main.gti";
  const std::filesystem::path helper = project / "helper.gti";
  const std::filesystem::path leaf = project / "leaf.gti";
  std::filesystem::create_directories(project);
  expect(
      writeFile(source, "import \"helper.gti\";\n"
                        "int main() { return cached_value(); }\n") &&
          writeFile(helper,
                    "export module helper;\n"
                    "import \"leaf.gti\";\n"
                    "export int cached_value() { return leaf_value(); }\n") &&
          writeFile(
              leaf,
              "export module leaf;\nexport int leaf_value() { return 0; }\n"),
      "whole-program cache source fixtures should be writable");

  const std::string executablePath = testExecutable.string();
  const lang::driver::ToolchainLayout discoveredToolchain =
      lang::driver::discoverToolchainLayout(executablePath.c_str());
  const std::filesystem::path localRuntimeInclude =
      temporary.root() / "toolchain/include";
  const std::filesystem::path localRuntimeLibrary =
      temporary.root() / "toolchain/libgti_runtime.a";
  std::filesystem::create_directories(localRuntimeInclude / "gti");
  std::error_code copyError;
  for (const std::string_view header :
       {"runtime.hpp", "runtime.h", "c_abi.h", "runtime_failure.h",
        "runtime_thread.h", "runtime_thread.hpp"}) {
    std::filesystem::copy_file(discoveredToolchain.runtimeInclude / "gti" /
                                   header,
                               localRuntimeInclude / "gti" / header, copyError);
    expect(!copyError, "runtime cache fixture headers should be copyable");
    copyError.clear();
  }
  std::filesystem::copy_file(discoveredToolchain.runtimeLibrary,
                             localRuntimeLibrary, copyError);
  expect(!copyError, "the runtime cache fixture archive should be copyable");
  const lang::driver::ToolchainLayout toolchain{
      .standardLibrary = discoveredToolchain.standardLibrary,
      .runtimeInclude = localRuntimeInclude,
      .runtimeLibrary = localRuntimeLibrary,
  };
  const lang::driver::ManagedOutputPolicy managed{
      .trustedRoot = project,
      .outputRoot = managedRoot,
  };
  const lang::driver::BuildCachePolicy cache{
      .root = managedRoot / "cache",
      .sourceRoot = project,
      .compilerIdentity = "driver-cache-test-v1",
      .projectModelIdentity = "driver-project-model-v1",
  };
  std::string nativeCompiler = "c++";
  if (!activeSanitizerArguments().empty()) {
    // Keep sanitizer runtime linkage in the compiler identity rather than in
    // NativeInputs: opaque user arguments correctly bypass cache lookup, while
    // sanitizer CI still needs to exercise the cache implementation itself.
    std::filesystem::path wrapper = temporary.root() / "native-cxx-wrapper";
#if defined(_WIN32)
    wrapper += ".exe";
#endif
    std::error_code wrapperError;
    std::filesystem::create_symlink(testExecutable, wrapper, wrapperError);
    if (wrapperError) {
      wrapperError.clear();
      std::filesystem::copy_file(testExecutable, wrapper, wrapperError);
    }
    expect(!wrapperError,
           "the sanitizer-aware native compiler wrapper should be creatable");
    if (!wrapperError) {
      nativeCompiler = wrapper.string();
    }
  }
  const auto build =
      [&](lang::OptimizationLevel optimization,
          lang::driver::NativeInputs nativeInputs = {}, bool keepCpp = false,
          lang::CppStandard standard = lang::CppStandard::Cpp23,
          lang::TargetInfo target = lang::TargetInfo::host(),
          lang::driver::PackageWarningPolicy packageWarnings = {},
          lang::WarningPolicy warnings = {},
          lang::RawPointerAuditMode rawPointerAudit =
              lang::RawPointerAuditMode::Off,
          bool debugInfo = false) {
        return lang::driver::buildExecutable(
            lang::driver::ExecutableBuildRequest(
                lang::driver::CompilationRequest(
                    source, toolchain.standardLibrary, std::move(target),
                    optimization, standard, {}, {}, {}, std::move(warnings),
                    debugInfo, std::move(packageWarnings), rawPointerAudit),
                toolchain, generated, output, nativeCompiler,
                std::move(nativeInputs), keepCpp, true, false, managed,
                std::nullopt, cache));
      };

  const lang::driver::ExecutableBuildResult first =
      build(lang::OptimizationLevel::O0);
  if (!first.succeeded()) {
    if (first.driverDiagnostic) {
      std::cerr << "whole-program cache first-build diagnostic: "
                << *first.driverDiagnostic << '\n';
    }
    if (first.nativeProcess) {
      std::cerr << "whole-program cache first-build native output:\n"
                << first.nativeProcess->output;
    }
    for (const lang::Diagnostic &diagnostic : first.compilation.diagnostics) {
      std::cerr << "whole-program cache first-build compiler diagnostic: "
                << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  if (first.cache.status != lang::driver::BuildCacheStatus::Miss) {
    std::cerr << "whole-program cache first-build status: "
              << static_cast<int>(first.cache.status) << '\n';
    if (first.cache.detail) {
      std::cerr << "whole-program cache first-build detail: "
                << *first.cache.detail << '\n';
    }
  }
  if (first.cache.warning) {
    std::cerr << "whole-program cache first-build warning: "
              << *first.cache.warning << '\n';
  }
  expect(first.succeeded() &&
             first.cache.status == lang::driver::BuildCacheStatus::Miss &&
             first.nativeProcess && first.nativeProcess->succeeded() &&
             !first.cache.key.empty() &&
             std::filesystem::is_regular_file(first.cache.entry / "metadata") &&
             std::filesystem::is_regular_file(first.cache.entry /
                                              "generated.cpp") &&
             std::filesystem::is_regular_file(first.cache.entry / "executable"),
         "the first project build should compile natively and atomically "
         "publish a complete content-addressed cache entry");
  const std::string firstKey = first.cache.key;
  const std::filesystem::path firstEntry = first.cache.entry;
  expect(
      firstEntry.parent_path() == cache.root / "v4" &&
          readFile(firstEntry / "metadata").starts_with("gti-build-cache-v4\n"),
      "the current one-root driver request should publish only the v4 "
      "symbol- and test-aware ordered-prelude cache schema");

  std::error_code removeError;
  std::filesystem::remove(output, removeError);
  const lang::driver::ExecutableBuildResult hit =
      build(lang::OptimizationLevel::O0);
  expect(hit.succeeded() &&
             hit.cache.status == lang::driver::BuildCacheStatus::Hit &&
             hit.cache.key == firstKey && !hit.nativeProcess &&
             hit.cCompilations.empty() && hit.cppCompilations.empty() &&
             std::filesystem::is_regular_file(output),
         "an unchanged build should verify and restore the cached executable "
         "without invoking any native compilation");

  const auto debugBuild = [&] {
    return build(lang::OptimizationLevel::O0, {}, false,
                 lang::CppStandard::Cpp23, lang::TargetInfo::host(), {}, {},
                 lang::RawPointerAuditMode::Off, true);
  };
  const lang::driver::ExecutableBuildResult debugFirst = debugBuild();
  if (debugFirst.cache.status != lang::driver::BuildCacheStatus::Miss) {
    std::cerr << "whole-program cache debug-build status: "
              << static_cast<int>(debugFirst.cache.status)
              << " detail: " << debugFirst.cache.detail.value_or("none")
              << '\n';
  }
  expect(debugFirst.succeeded() &&
             debugFirst.cache.status == lang::driver::BuildCacheStatus::Miss &&
             !debugFirst.cache.key.empty() &&
             debugFirst.cache.key != firstKey &&
             std::filesystem::is_regular_file(debugFirst.cache.entry /
                                              "metadata") &&
             readFile(debugFirst.cache.entry / "metadata").find("\nsymbols ") !=
                 std::string::npos,
         "a source-level debug build should use the cache under its own key "
         "and record its symbol bundle in the entry metadata");
  const std::filesystem::path debugBundle =
      std::filesystem::path(output.string() + ".dSYM");
#if defined(__APPLE__)
  expect(
      std::filesystem::is_directory(debugFirst.cache.entry / "symbols.dSYM") &&
          std::filesystem::is_regular_file(debugBundle / ".gti-debug-symbols"),
      "a Darwin debug build should publish its owned symbol bundle and "
      "keep a copy of it in the cache entry");
  std::error_code bundleRemoveError;
  std::filesystem::remove_all(debugBundle, bundleRemoveError);
#endif
  std::error_code debugRemoveError;
  std::filesystem::remove(output, debugRemoveError);
  const lang::driver::ExecutableBuildResult debugHit = debugBuild();
  expect(debugHit.succeeded() &&
             debugHit.cache.status == lang::driver::BuildCacheStatus::Hit &&
             debugHit.cache.key == debugFirst.cache.key &&
             !debugHit.nativeProcess && debugHit.cCompilations.empty() &&
             debugHit.cppCompilations.empty() &&
             std::filesystem::is_regular_file(output),
         "an unchanged debug build should restore the cached executable "
         "without native compilation");
#if defined(__APPLE__)
  expect(std::filesystem::is_regular_file(debugBundle / ".gti-debug-symbols"),
         "a Darwin debug cache hit should restore the owned symbol bundle "
         "next to the executable");
#endif
  const lang::driver::ExecutableBuildResult plainAfterDebug =
      build(lang::OptimizationLevel::O0);
  expect(plainAfterDebug.succeeded() &&
             plainAfterDebug.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             plainAfterDebug.cache.key == firstKey,
         "the non-debug entry should remain valid beside the debug entry");

  std::string warnAuditKey;
  for (const auto mode :
       {lang::RawPointerAuditMode::Warn, lang::RawPointerAuditMode::Strict}) {
    const auto audited =
        build(lang::OptimizationLevel::O0, {}, false, lang::CppStandard::Cpp23,
              lang::TargetInfo::host(), {}, {}, mode);
    expect(audited.succeeded() &&
               audited.cache.status == lang::driver::BuildCacheStatus::Miss &&
               audited.cache.key != firstKey &&
               audited.cache.key != warnAuditKey,
           "each audit policy requires its own frontend/cache identity");
    warnAuditKey = audited.cache.key;
    const auto auditHit =
        build(lang::OptimizationLevel::O0, {}, false, lang::CppStandard::Cpp23,
              lang::TargetInfo::host(), {}, {}, mode);
    expect(auditHit.succeeded() &&
               auditHit.cache.status == lang::driver::BuildCacheStatus::Hit &&
               auditHit.cache.key == audited.cache.key,
           "unchanged audit policy may reuse its verified artifact");
  }

  const lang::driver::ExecutableBuildResult warningPolicyChange = build(
      lang::OptimizationLevel::O0, {}, false, lang::CppStandard::Cpp23,
      lang::TargetInfo::host(),
      lang::driver::PackageWarningPolicy(false, "app@1.0.0", {"dep@1.0.0"}));
  expect(warningPolicyChange.succeeded() &&
             warningPolicyChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             warningPolicyChange.cache.key != firstKey,
         "changing dependency-warning policy must perform frontend analysis "
         "instead of reusing a cached diagnostic presentation");
  const lang::driver::ExecutableBuildResult groupPolicyChange =
      build(lang::OptimizationLevel::O0, {}, false, lang::CppStandard::Cpp23,
            lang::TargetInfo::host(), {}, lang::WarningPolicy({"unused-mut"}));
  expect(groupPolicyChange.succeeded() &&
             groupPolicyChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             groupPolicyChange.cache.key != firstKey,
         "warning-group policy must invalidate the same cached presentation");

  expect(
      writeFile(leaf,
                "export module leaf;\nexport int leaf_value() { return 1; }\n"),
      "the transitive cache invalidation fixture should be writable");
  const lang::driver::ExecutableBuildResult transitiveChange =
      build(lang::OptimizationLevel::O0);
  expect(transitiveChange.succeeded() &&
             transitiveChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             transitiveChange.cache.key != firstKey &&
             transitiveChange.nativeProcess,
         "changing a transitive GTI include should produce a different cache "
         "identity and invoke native compilation");

  expect(
      writeFile(leaf,
                "export module leaf;\nexport int leaf_value() { return 0; }\n"),
      "the original transitive source should be restorable");
  const lang::driver::ExecutableBuildResult restoredSource =
      build(lang::OptimizationLevel::O0);
  expect(restoredSource.succeeded() &&
             restoredSource.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             restoredSource.cache.key == firstKey,
         "cache identity should depend on source content rather than file "
         "timestamps");

  const lang::driver::ExecutableBuildResult optimizationChange =
      build(lang::OptimizationLevel::O1);
  expect(optimizationChange.succeeded() &&
             optimizationChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             optimizationChange.cache.key != firstKey,
         "optimization policy should participate in whole-program cache "
         "identity");

  const lang::driver::ExecutableBuildResult standardChange =
      build(lang::OptimizationLevel::O0, {}, false, lang::CppStandard::Cpp20);
  expect(standardChange.succeeded() &&
             standardChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             standardChange.cache.key != firstKey,
         "the selected backend C++ standard should participate in cache "
         "identity");

  lang::TargetInfo concurrentTarget = lang::TargetInfo::host();
  concurrentTarget.executionProfile = lang::ExecutionProfile::Concurrent;
  const lang::driver::ExecutableBuildResult executionProfileChange =
      build(lang::OptimizationLevel::O0, {}, false, lang::CppStandard::Cpp23,
            std::move(concurrentTarget));
  expect(executionProfileChange.succeeded() &&
             executionProfileChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             executionProfileChange.cache.key != firstKey,
         "the selected GTI execution profile should participate in cache "
         "identity");

  const std::filesystem::path runtimeHeader =
      localRuntimeInclude / "gti/runtime.hpp";
  const std::string originalRuntimeHeader = readFile(runtimeHeader);
  expect(writeFile(runtimeHeader,
                   originalRuntimeHeader + "\n// cache identity change\n"),
         "the runtime invalidation fixture should be writable");
  const lang::driver::ExecutableBuildResult runtimeChange =
      build(lang::OptimizationLevel::O0);
  expect(runtimeChange.succeeded() &&
             runtimeChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             runtimeChange.cache.key != firstKey,
         "runtime header content should participate in cache identity");
  expect(writeFile(runtimeHeader, originalRuntimeHeader),
         "the original runtime header should be restorable");

  const std::filesystem::path forcedHeader = project / "forced-cache.hpp";
  expect(writeFile(forcedHeader, "#define GTI_FORCED_CACHE_VALUE 1\n"),
         "the forced native header fixture should be writable");
  const auto buildWithForcedHeader = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.compilerArguments = {"-include", forcedHeader.string()};
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult forcedHeaderBuild =
      buildWithForcedHeader();
  expect(forcedHeaderBuild.succeeded() &&
             forcedHeaderBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             forcedHeaderBuild.cache.detail &&
             forcedHeaderBuild.cache.detail->find(
                 "opaque native argument vectors") != std::string::npos &&
             forcedHeaderBuild.nativeProcess,
         "opaque compiler arguments should bypass cache lookup because they "
         "can introduce dependencies such as forced native headers");
  expect(writeFile(forcedHeader, "#define GTI_FORCED_CACHE_VALUE 2\n"),
         "the forced native header should remain mutable between builds");
  const lang::driver::ExecutableBuildResult forcedHeaderChange =
      buildWithForcedHeader();
  expect(forcedHeaderChange.succeeded() &&
             forcedHeaderChange.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             forcedHeaderChange.nativeProcess,
         "changing a header introduced through an opaque compiler argument "
         "should force native compilation rather than restore a stale entry");

  const auto buildWithExactLinkFile = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.libraryFiles.push_back(localRuntimeLibrary);
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult exactNativeLinkBuild =
      buildWithExactLinkFile();
  expect(exactNativeLinkBuild.succeeded() &&
             exactNativeLinkBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             exactNativeLinkBuild.cache.key != firstKey &&
             exactNativeLinkBuild.nativeProcess,
         "a content-complete exact link archive should join cache identity "
         "with its own key instead of bypassing");
  const lang::driver::ExecutableBuildResult exactNativeLinkHit =
      buildWithExactLinkFile();
  expect(exactNativeLinkHit.succeeded() &&
             exactNativeLinkHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             exactNativeLinkHit.cache.key == exactNativeLinkBuild.cache.key &&
             !exactNativeLinkHit.nativeProcess,
         "an unchanged exact link archive should verify and restore the "
         "cached executable");

  const std::filesystem::path linkerScript = project / "script.a";
  expect(writeFile(linkerScript, "INPUT(missing.o)\n"),
         "the linker-script link-input fixture should be writable");
  lang::driver::NativeInputs scriptLinkInput;
  scriptLinkInput.libraryFiles.push_back(linkerScript);
  const lang::driver::ExecutableBuildResult scriptLinkBuild =
      build(lang::OptimizationLevel::O0, std::move(scriptLinkInput));
  expect(scriptLinkBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             scriptLinkBuild.cache.detail &&
             scriptLinkBuild.cache.detail->find(
                 "not a content-complete archive or object") !=
                 std::string::npos,
         "a link input that is not a content-complete archive or object "
         "should bypass the whole-program cache with its reason");

  const std::filesystem::path nativeSearchDirectory = project / "native-search";
  std::filesystem::create_directories(nativeSearchDirectory);
  const auto buildWithIncludeDirectory = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.includeDirectories.push_back(nativeSearchDirectory);
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult nativeIncludeDirectoryBuild =
      buildWithIncludeDirectory();
  expect(nativeIncludeDirectoryBuild.succeeded() &&
             nativeIncludeDirectoryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeIncludeDirectoryBuild.cache.key != firstKey &&
             nativeIncludeDirectoryBuild.nativeProcess,
         "declared native include directories should join cache identity "
         "through full-tree content instead of bypassing");
  const lang::driver::ExecutableBuildResult nativeIncludeDirectoryHit =
      buildWithIncludeDirectory();
  expect(nativeIncludeDirectoryHit.succeeded() &&
             nativeIncludeDirectoryHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             nativeIncludeDirectoryHit.cache.key ==
                 nativeIncludeDirectoryBuild.cache.key,
         "an unchanged declared include tree should verify and restore the "
         "cached executable");
  expect(writeFile(nativeSearchDirectory / "added.h", "#define ADDED 1\n"),
         "the include-tree invalidation fixture should be writable");
  const lang::driver::ExecutableBuildResult nativeIncludeDirectoryChange =
      buildWithIncludeDirectory();
  expect(nativeIncludeDirectoryChange.succeeded() &&
             nativeIncludeDirectoryChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeIncludeDirectoryChange.cache.key !=
                 nativeIncludeDirectoryBuild.cache.key,
         "adding any file below a declared include directory should change "
         "cache identity so header shadowing can never restore a stale "
         "executable");

  const std::filesystem::path nativeLibrarySearchDirectory =
      project / "native-library-search";
  std::filesystem::create_directories(nativeLibrarySearchDirectory);
  const auto buildWithLibraryDirectory = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.libraryDirectories.push_back(nativeLibrarySearchDirectory);
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult nativeLibraryDirectoryBuild =
      buildWithLibraryDirectory();
  expect(nativeLibraryDirectoryBuild.succeeded() &&
             nativeLibraryDirectoryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeLibraryDirectoryBuild.cache.key != firstKey &&
             nativeLibraryDirectoryBuild.nativeProcess,
         "declared native library search directories should join cache "
         "identity through full-tree content instead of bypassing");
  const lang::driver::ExecutableBuildResult nativeLibraryDirectoryHit =
      buildWithLibraryDirectory();
  expect(nativeLibraryDirectoryHit.succeeded() &&
             nativeLibraryDirectoryHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             nativeLibraryDirectoryHit.cache.key ==
                 nativeLibraryDirectoryBuild.cache.key,
         "an unchanged declared library search tree should verify and restore "
         "the cached executable");
  expect(writeFile(nativeLibrarySearchDirectory / "added.txt", "added\n"),
         "the library-tree invalidation fixture should be writable");
  const lang::driver::ExecutableBuildResult nativeLibraryDirectoryChange =
      buildWithLibraryDirectory();
  expect(nativeLibraryDirectoryChange.succeeded() &&
             nativeLibraryDirectoryChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeLibraryDirectoryChange.cache.key !=
                 nativeLibraryDirectoryBuild.cache.key,
         "adding any file below a declared library search directory should "
         "change cache identity, so a file dropped there can never shadow an "
         "implicitly resolved library behind a stale entry");
  std::filesystem::remove(nativeLibrarySearchDirectory / "added.txt");

  lang::driver::NativeInputs namedLibraryInput;
  namedLibraryInput.libraries.emplace_back("m");
  const lang::driver::ExecutableBuildResult namedLibraryBuild =
      build(lang::OptimizationLevel::O0, std::move(namedLibraryInput));
  expect(namedLibraryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             namedLibraryBuild.cache.detail &&
             namedLibraryBuild.cache.detail->find(
                 "does not resolve inside the declared native library search "
                 "directories") != std::string::npos,
         "a named library with no declared search directory holding it would "
         "resolve through the linker's implicit directories and must bypass");

  // A `-l` name that resolves to exactly one content-complete archive inside
  // a declared search directory participates with that file's content.
  const std::filesystem::path resolvedArchive =
      nativeLibrarySearchDirectory / "libgtiruntimealias.a";
  copyError.clear();
  std::filesystem::copy_file(localRuntimeLibrary, resolvedArchive, copyError);
  expect(!copyError, "the resolvable library fixture should be copyable");
  const auto buildWithResolvedLibrary = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.libraryDirectories.push_back(nativeLibrarySearchDirectory);
    inputs.libraries.emplace_back("gtiruntimealias");
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult resolvedLibraryBuild =
      buildWithResolvedLibrary();
  expect(resolvedLibraryBuild.succeeded() &&
             resolvedLibraryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             resolvedLibraryBuild.nativeProcess,
         "a named library resolving to one content-complete archive in a "
         "declared search directory should join cache identity");
  const lang::driver::ExecutableBuildResult resolvedLibraryHit =
      buildWithResolvedLibrary();
  expect(resolvedLibraryHit.succeeded() &&
             resolvedLibraryHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             resolvedLibraryHit.cache.key == resolvedLibraryBuild.cache.key &&
             !resolvedLibraryHit.nativeProcess,
         "an unchanged resolved library should verify and restore the cached "
         "executable");

  // Two modeled candidates in the same directory: which one the linker would
  // prefer is a guess, so resolution must refuse.
  const std::filesystem::path ambiguousCandidate =
      nativeLibrarySearchDirectory /
#if defined(_WIN32)
      "libgtiruntimealias.dll.a";
#elif defined(__APPLE__)
      "libgtiruntimealias.dylib";
#else
      "libgtiruntimealias.so";
#endif
  expect(writeFile(ambiguousCandidate, "not a real dynamic library\n"),
         "the ambiguous library fixture should be writable");
  const lang::driver::ExecutableBuildResult ambiguousLibraryBuild =
      buildWithResolvedLibrary();
  expect(ambiguousLibraryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             ambiguousLibraryBuild.cache.detail &&
             ambiguousLibraryBuild.cache.detail->find("resolves ambiguously") !=
                 std::string::npos,
         "two modeled candidates in one search directory should bypass rather "
         "than guess the linker's preference");
  std::filesystem::remove(ambiguousCandidate);

  // An unambiguous resolution whose bytes do not determine its contribution
  // to the link still bypasses.
  std::filesystem::remove(resolvedArchive);
  expect(writeFile(resolvedArchive, "INPUT(missing.o)\n"),
         "the content-incomplete resolved library fixture should be writable");
  const lang::driver::ExecutableBuildResult incompleteLibraryBuild =
      buildWithResolvedLibrary();
  expect(incompleteLibraryBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             incompleteLibraryBuild.cache.detail &&
             incompleteLibraryBuild.cache.detail->find(
                 "not a content-complete archive or object") !=
                 std::string::npos,
         "a resolved library that is not content-complete should bypass with "
         "the link-input reason");
  std::filesystem::remove(resolvedArchive);

  const auto buildWithFramework = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.frameworks.emplace_back("CoreFoundation");
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
#if defined(__APPLE__)
  const lang::driver::ExecutableBuildResult frameworkBuild =
      buildWithFramework();
  expect(
      frameworkBuild.succeeded() &&
          frameworkBuild.cache.status == lang::driver::BuildCacheStatus::Miss &&
          frameworkBuild.cache.key != firstKey && frameworkBuild.nativeProcess,
      "a framework selected by the Apple linker should join cache identity "
      "through its exact traced link input");
  const lang::driver::ExecutableBuildResult frameworkHit = buildWithFramework();
  expect(frameworkHit.succeeded() &&
             frameworkHit.cache.status == lang::driver::BuildCacheStatus::Hit &&
             frameworkHit.cache.key == frameworkBuild.cache.key &&
             !frameworkHit.nativeProcess,
         "an unchanged traced Apple framework should restore the cached "
         "executable without relinking");

  lang::driver::NativeInputs missingFrameworkInputs;
  missingFrameworkInputs.frameworks.emplace_back(
      "GTIFrameworkThatMustNotExistForCacheTesting");
  const lang::driver::ExecutableBuildResult missingFrameworkBuild =
      build(lang::OptimizationLevel::O0, std::move(missingFrameworkInputs));
  expect(missingFrameworkBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             missingFrameworkBuild.cache.detail &&
             missingFrameworkBuild.cache.detail->find(
                 "framework resolution probe failed") != std::string::npos,
         "a framework whose exact linker input cannot be resolved should "
         "bypass rather than trusting its name");

  // A compiler wrapper supplies a private framework search root that the
  // driver cannot infer. The real linker map remains authoritative: changing
  // the selected text stub must change the cache key even though the framework
  // name, wrapper executable, and GTI sources are unchanged.
  const lang::driver::ProcessResult sdkProbe = lang::driver::invokeProcess(
      {"/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"},
      {.outputMode = lang::driver::ProcessOutputMode::Capture,
       .captureSuccessfulOutput = true,
       .description = "test macOS SDK discovery"});
  std::string sdkPath = sdkProbe.output;
  while (!sdkPath.empty() &&
         std::isspace(static_cast<unsigned char>(sdkPath.back())) != 0) {
    sdkPath.pop_back();
  }
  const std::filesystem::path systemFrameworkStub =
      std::filesystem::path(sdkPath) /
      "System/Library/Frameworks/CoreFoundation.framework/CoreFoundation.tbd";
  const std::filesystem::path privateFrameworkStub =
      temporary.root() /
      "Frameworks/CoreFoundation.framework/CoreFoundation.tbd";
  std::filesystem::create_directories(privateFrameworkStub.parent_path());
  copyError.clear();
  if (sdkProbe.succeeded() && !sdkPath.empty()) {
    std::filesystem::copy_file(systemFrameworkStub, privateFrameworkStub,
                               copyError);
  }
  const bool frameworkFixtureReady =
      sdkProbe.succeeded() && !sdkPath.empty() && !copyError &&
      std::filesystem::is_regular_file(privateFrameworkStub);
  expect(frameworkFixtureReady,
         "the framework cache test should copy the selected SDK text stub");
  const std::filesystem::path frameworkWrapper =
      temporary.root() / "native-framework-cxx-wrapper";
  std::error_code wrapperError;
  std::filesystem::create_symlink(testExecutable, frameworkWrapper,
                                  wrapperError);
  if (wrapperError) {
    wrapperError.clear();
    std::filesystem::copy_file(testExecutable, frameworkWrapper, wrapperError);
  }
  expect(!wrapperError,
         "the framework-selecting native compiler wrapper should be creatable");
  const std::string previousNativeCompiler = nativeCompiler;
  if (frameworkFixtureReady && !wrapperError) {
    nativeCompiler = frameworkWrapper.string();
    const lang::driver::ExecutableBuildResult privateFrameworkBuild =
        buildWithFramework();
    const lang::driver::ExecutableBuildResult privateFrameworkHit =
        buildWithFramework();
    expect(privateFrameworkBuild.succeeded() &&
               privateFrameworkBuild.cache.status ==
                   lang::driver::BuildCacheStatus::Miss &&
               privateFrameworkHit.succeeded() &&
               privateFrameworkHit.cache.status ==
                   lang::driver::BuildCacheStatus::Hit &&
               privateFrameworkHit.cache.key == privateFrameworkBuild.cache.key,
           "the linker-traced private framework should be reusable while its "
           "selected text stub is unchanged");
    const std::string originalStub = readFile(privateFrameworkStub);
    std::string changedStub = originalStub;
    const std::size_t firstLineEnd = changedStub.find('\n');
    if (firstLineEnd != std::string::npos) {
      changedStub.insert(firstLineEnd + 1, "# GTI cache identity change\n");
    }
    expect(firstLineEnd != std::string::npos &&
               writeFile(privateFrameworkStub, changedStub),
           "the private framework text stub should remain mutable");
    const lang::driver::ExecutableBuildResult privateFrameworkChange =
        buildWithFramework();
    if (!(privateFrameworkChange.succeeded() &&
          privateFrameworkChange.cache.status ==
              lang::driver::BuildCacheStatus::Miss &&
          privateFrameworkChange.cache.key !=
              privateFrameworkBuild.cache.key)) {
      std::cerr << "framework mutation status="
                << static_cast<int>(privateFrameworkChange.status) << " cache="
                << static_cast<int>(privateFrameworkChange.cache.status)
                << " old-key=" << privateFrameworkBuild.cache.key
                << " new-key=" << privateFrameworkChange.cache.key << " detail="
                << privateFrameworkChange.cache.detail.value_or("none")
                << " diagnostic="
                << privateFrameworkChange.driverDiagnostic.value_or("none")
                << " native="
                << (privateFrameworkChange.nativeProcess
                        ? privateFrameworkChange.nativeProcess->output
                        : "none")
                << '\n';
    }
    expect(privateFrameworkChange.succeeded() &&
               privateFrameworkChange.cache.status ==
                   lang::driver::BuildCacheStatus::Miss &&
               privateFrameworkChange.cache.key !=
                   privateFrameworkBuild.cache.key,
           "changing the exact framework link input selected by the linker "
           "should invalidate the whole-program cache");
  }
  nativeCompiler = previousNativeCompiler;
#else
  const lang::driver::ExecutableBuildResult frameworkBuild =
      buildWithFramework();
  expect(
      frameworkBuild.cache.status == lang::driver::BuildCacheStatus::Bypassed &&
          frameworkBuild.cache.detail &&
          frameworkBuild.cache.detail->find("Apple linker resolution trace") !=
              std::string::npos,
      "framework cache identity should remain unavailable without an Apple "
      "linker trace");
#endif

  {
    ScopedEnvironment injectedIncludePath("CPATH",
                                          nativeSearchDirectory.string());
    const lang::driver::ExecutableBuildResult nativeEnvironmentBuild =
        build(lang::OptimizationLevel::O0);
    expect(nativeEnvironmentBuild.succeeded() &&
               nativeEnvironmentBuild.cache.status ==
                   lang::driver::BuildCacheStatus::Bypassed &&
               nativeEnvironmentBuild.cache.detail &&
               nativeEnvironmentBuild.cache.detail->find(
                   "environment search paths") != std::string::npos &&
               nativeEnvironmentBuild.nativeProcess,
           "dependency-injecting native environment search paths should "
           "bypass cache lookup until their compiler/linker-discovered "
           "transitive inputs can be modeled");
  }

  const std::filesystem::path nativeHeader = project / "cache-native.hpp";
  const std::filesystem::path nativeSource = project / "cache-native.cpp";
  expect(writeFile(nativeHeader, "#define GTI_CACHE_NATIVE_VALUE 1\n") &&
             writeFile(nativeSource,
                       "#include \"cache-native.hpp\"\n"
                       "extern \"C\" int gti_cache_native_value() { "
                       "return GTI_CACHE_NATIVE_VALUE; }\n"),
         "native source cache fixtures should be writable");
  const auto buildWithNativeSource = [&]() {
    lang::driver::NativeInputs inputs;
    inputs.cppSources.push_back(nativeSource);
    return build(lang::OptimizationLevel::O0, std::move(inputs));
  };
  const lang::driver::ExecutableBuildResult nativeSourceBuild =
      buildWithNativeSource();
  expect(nativeSourceBuild.succeeded() &&
             nativeSourceBuild.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeSourceBuild.cache.key != firstKey &&
             nativeSourceBuild.cppCompilations.size() == 1,
         "a declared native source should join cache identity through "
         "compiler dependency discovery instead of bypassing");
  const lang::driver::ExecutableBuildResult nativeSourceHit =
      buildWithNativeSource();
  expect(nativeSourceHit.succeeded() &&
             nativeSourceHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             nativeSourceHit.cache.key == nativeSourceBuild.cache.key &&
             nativeSourceHit.cppCompilations.empty() &&
             !nativeSourceHit.nativeProcess,
         "an unchanged declared native source should verify and restore the "
         "cached executable without native compilation");

  // Discovery memo: the probe's outcome is persisted beneath the cache root
  // and answers an unchanged source without the preprocessor. The record's
  // `written` line moves when stamps are repaired, so compare the rest.
  const std::filesystem::path discoveryRoot = cache.root / "native-discovery";
  std::vector<std::filesystem::path> discoveryRecords;
  std::error_code discoveryError;
  for (const auto &entry :
       std::filesystem::directory_iterator(discoveryRoot, discoveryError)) {
    discoveryRecords.push_back(entry.path());
  }
  const auto recordWithoutWritten = [](const std::string &text) {
    std::string kept;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.starts_with("written ")) {
        kept += line + "\n";
      }
    }
    return kept;
  };
  const auto recordDirectoryLine = [](const std::string &text,
                                      const std::filesystem::path &directory) {
    const std::string suffix =
        " " + std::filesystem::weakly_canonical(directory).generic_string();
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.starts_with("directory ") && line.ends_with(suffix)) {
        return line;
      }
    }
    return std::string();
  };
  expect(!discoveryError && discoveryRecords.size() == 1 &&
             readFile(discoveryRecords.front())
                 .starts_with("gti-native-discovery-v1\n") &&
             readFile(discoveryRecords.front())
                     .find(" " +
                           std::filesystem::weakly_canonical(nativeHeader)
                               .generic_string() +
                           "\n") != std::string::npos &&
             !recordDirectoryLine(readFile(discoveryRecords.front()), project)
                  .empty(),
         "a declared native source's discovery should persist one memo record "
         "naming its discovered header and watching the source directory");
  const std::string memoAfterHit = discoveryRecords.empty()
                                       ? std::string()
                                       : readFile(discoveryRecords.front());
  const lang::driver::ExecutableBuildResult nativeSourceMemoHit =
      buildWithNativeSource();
  expect(nativeSourceMemoHit.succeeded() &&
             nativeSourceMemoHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             nativeSourceMemoHit.cache.key == nativeSourceBuild.cache.key &&
             !discoveryRecords.empty() &&
             recordWithoutWritten(readFile(discoveryRecords.front())) ==
                 recordWithoutWritten(memoAfterHit),
         "a memoized discovery should reproduce the probe's identity exactly "
         "and keep its dependency and watch lines");
  expect(writeFile(project / "cache-native-neighbour.hpp", "// unused\n"),
         "the neighbouring native header fixture should be writable");
  const lang::driver::ExecutableBuildResult nativeNeighbourHit =
      buildWithNativeSource();
  expect(nativeNeighbourHit.succeeded() &&
             nativeNeighbourHit.cache.status ==
                 lang::driver::BuildCacheStatus::Hit &&
             nativeNeighbourHit.cache.key == nativeSourceBuild.cache.key &&
             !discoveryRecords.empty() &&
             recordDirectoryLine(readFile(discoveryRecords.front()), project) !=
                 recordDirectoryLine(memoAfterHit, project) &&
             !recordDirectoryLine(readFile(discoveryRecords.front()), project)
                  .empty(),
         "a new file beside a discovered input should re-run the probe (the "
         "watched directory changed) and, resolving identically, still hit");

  expect(writeFile(nativeHeader, "#define GTI_CACHE_NATIVE_VALUE 2\n"),
         "the native transitive-header fixture should be writable");
  const lang::driver::ExecutableBuildResult nativeHeaderChange =
      buildWithNativeSource();
  expect(nativeHeaderChange.succeeded() &&
             nativeHeaderChange.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             nativeHeaderChange.cache.key != nativeSourceBuild.cache.key &&
             nativeHeaderChange.cppCompilations.size() == 1,
         "changing a compiler-discovered native header must change cache "
         "identity and force native recompilation");

  expect(
      writeFile(nativeHeader, "#define GTI_CACHE_NATIVE_VALUE (__TIME__[0])\n"),
      "the time-sensitive native header fixture should be writable");
  const lang::driver::ExecutableBuildResult timeSensitiveNative =
      buildWithNativeSource();
  expect(timeSensitiveNative.succeeded() &&
             timeSensitiveNative.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             timeSensitiveNative.cache.detail &&
             timeSensitiveNative.cache.detail->find(
                 "time-and-date preprocessor macros") != std::string::npos,
         "a discovered native input using time-and-date preprocessor macros "
         "should bypass the cache under the deterministic preprocessing "
         "policy");
  expect(writeFile(nativeHeader, "#define GTI_CACHE_NATIVE_VALUE 1\n"),
         "the original native header should be restorable");

  expect(writeFile(firstEntry / "executable", "corrupt"),
         "the cache corruption fixture should be writable");
  const lang::driver::ExecutableBuildResult recovered =
      build(lang::OptimizationLevel::O0);
  expect(recovered.succeeded() &&
             recovered.cache.status ==
                 lang::driver::BuildCacheStatus::RecoveredCorruption &&
             recovered.cache.warning && recovered.nativeProcess &&
             recovered.nativeProcess->succeeded(),
         "a digest mismatch should never be used and should be replaced only "
         "after a successful native rebuild");

  const lang::driver::ExecutableBuildResult retainedCpp =
      build(lang::OptimizationLevel::O0, {}, true);
  expect(retainedCpp.succeeded() &&
             retainedCpp.cache.status == lang::driver::BuildCacheStatus::Hit &&
             retainedCpp.generatedSourceRetained &&
             std::filesystem::is_regular_file(generated) &&
             readFile(generated) == retainedCpp.compilation.artifact->contents,
         "keep-cpp should restore verified generated source on a cache hit "
         "without changing cache identity");

  const std::string publishedExecutable = readFile(output);
  std::filesystem::remove_all(firstEntry, removeError);
  expect(!removeError && readFile(output) == publishedExecutable,
         "deleting a cache entry should not damage the published executable");
  const std::filesystem::path legacyEntry = cache.root / "v3" / firstKey;
  std::filesystem::create_directories(legacyEntry);
  expect(writeFile(legacyEntry / "metadata", "gti-build-cache-v3\n") &&
             writeFile(legacyEntry / "generated.cpp", "stale") &&
             writeFile(legacyEntry / "executable", "stale"),
         "the legacy cache-schema fixture should be writable");
  const lang::driver::ExecutableBuildResult afterDeletion =
      build(lang::OptimizationLevel::O0);
  expect(afterDeletion.succeeded() &&
             afterDeletion.cache.status ==
                 lang::driver::BuildCacheStatus::Miss &&
             afterDeletion.nativeProcess &&
             afterDeletion.cache.entry.parent_path() == cache.root / "v4",
         "a deleted v4 cache entry should ignore a same-key v3 entry and "
         "degrade to a clean rebuild");

  const lang::driver::ExecutableBuildResult unmanagedCache =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              source, toolchain.standardLibrary, lang::TargetInfo::host(),
              lang::OptimizationLevel::O0, lang::CppStandard::Cpp23),
          toolchain, temporary.root() / "unmanaged.gti.cpp",
          temporary.root() / "unmanaged-program", nativeCompiler, {}, false,
          true, false, std::nullopt, std::nullopt, cache));
  expect(unmanagedCache.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             unmanagedCache.cache.detail &&
             unmanagedCache.cache.detail->find("managed project output") !=
                 std::string::npos,
         "a cache policy without a managed project output should bypass with "
         "its reason rather than publish unchecked paths");

  const lang::driver::ExecutableBuildResult unresolvableIdentity =
      lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
          lang::driver::CompilationRequest(
              source, toolchain.standardLibrary, lang::TargetInfo::host(),
              lang::OptimizationLevel::O0, lang::CppStandard::Cpp23),
          toolchain, generated, output,
          (temporary.root() / "missing-native-compiler").string(), {}, false,
          true, false, managed, std::nullopt, cache));
  expect(unresolvableIdentity.cache.status ==
                 lang::driver::BuildCacheStatus::Bypassed &&
             unresolvableIdentity.cache.detail &&
             unresolvableIdentity.cache.detail->find("identity unavailable") !=
                 std::string::npos,
         "an unresolvable native compiler identity should bypass the cache "
         "with its reason instead of guessing a key");
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
        temporary.root() / ("generated-" + std::string(suffix) + ".cpp");
    const std::filesystem::path output =
        temporary.root() / ("program-" + std::string(suffix));
    const std::filesystem::path report =
        temporary.root() / ("report-" + std::string(suffix) + ".json");
    return lang::driver::buildExecutable(lang::driver::ExecutableBuildRequest(
        lang::driver::CompilationRequest(
            buildEntry, buildToolchain.standardLibrary,
            lang::TargetInfo::host(), lang::OptimizationLevel::O0, standard, {},
            std::move(flags)),
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
  const bool hasRuntimeHeader = std::any_of(
      firstProvenance.runtime.headers.begin(),
      firstProvenance.runtime.headers.end(),
      [](const lang::driver::BuildRuntimeHeaderProvenance &header) {
        return header.kind == lang::driver::BuildRuntimeHeaderKind::Runtime &&
               !header.material.sha256.empty();
      });
  expect(hasDependency && hasRuntimeHeader &&
             firstProvenance.runtime.archive.canonicalPath ==
                 std::filesystem::weakly_canonical(runtimeLibrary) &&
             firstProvenance.producerExecutable.canonicalPath ==
                 std::filesystem::weakly_canonical(testExecutable) &&
             !firstProvenance.nativeCompilerExecutable.sha256.empty() &&
             std::find(firstProvenance.nativeCommand.begin(),
                       firstProvenance.nativeCommand.end(),
                       "-MD") != firstProvenance.nativeCommand.end() &&
             std::find(firstProvenance.nativeCommand.begin(),
                       firstProvenance.nativeCommand.end(),
                       "-MF") != firstProvenance.nativeCommand.end(),
         "build provenance should retain exact loaded source, used runtime, "
         "producer, native compiler, and depfile-bearing argv facts");
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

  // GTI owns its expected representation, so a C++20 build consumes exactly
  // the runtime materials of the C++23 build and no standard-specific header.
  const lang::driver::ExecutableBuildResult cpp20 =
      build("cpp20", entry, toolchain, lang::CppStandard::Cpp20);
  const auto runtimeSummary = [](const lang::driver::BuildProvenance &value) {
    std::string summary = value.runtime.materialsSha256;
    for (const lang::driver::BuildRuntimeHeaderProvenance &header :
         value.runtime.headers) {
      summary +=
          " [" + header.logicalIdentity + ":" + header.material.sha256 + "]";
    }
    return summary;
  };
  expect(cpp20.succeeded() && cpp20.provenance &&
             cpp20.provenance->runtime.materialsSha256 ==
                 firstProvenance.runtime.materialsSha256 &&
             std::all_of(
                 cpp20.provenance->runtime.headers.begin(),
                 cpp20.provenance->runtime.headers.end(),
                 [](const lang::driver::BuildRuntimeHeaderProvenance &header) {
                   return header.logicalIdentity.starts_with("gti/");
                 }),
         "a C++20 build should consume the C++23 build's runtime materials "
         "(status=" +
             std::to_string(static_cast<int>(cpp20.status)) + ", detail=" +
             cpp20.driverDiagnostic.value_or("none") + ", C++20 materials=" +
             (cpp20.provenance ? runtimeSummary(*cpp20.provenance) : "none") +
             ", C++23 materials=" + runtimeSummary(firstProvenance) + ")");

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

  if (firstProvenance.runtime.headers.empty()) {
    return;
  }
  const std::filesystem::path runtimeHeader =
      firstProvenance.runtime.headers.front().material.canonicalPath;
  expect(writeFile(runtimeHeader,
                   readFile(runtimeHeader) + "\n// provenance change\n"),
         "the copied used runtime header should be editable");
  const lang::driver::ExecutableBuildResult runtimeChanged =
      build("runtime", entry, toolchain, lang::CppStandard::Cpp23);
  expect(runtimeChanged.succeeded() && runtimeChanged.provenance &&
             sourceChanged.provenance &&
             runtimeChanged.provenance->runtime.materialsSha256 !=
                 sourceChanged.provenance->runtime.materialsSha256,
         "changing a used runtime header should alter runtime provenance "
         "(status=" +
             std::to_string(static_cast<int>(runtimeChanged.status)) +
             ", detail=" + runtimeChanged.driverDiagnostic.value_or("none") +
             ")");
}

// The sidecar is the interchange format `gti verify` reads back, so writing
// and reading it must round-trip exactly and comparison must name the exact
// fact that moved.
// The C++-backend part of project_tests.cpp's testBackendProfileAuthority:
// C++ retention and an explicit C++ override plan the C++ backend.
bool writeProjectFile(const std::filesystem::path &path,
                      std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  return writeFile(path, contents);
}

void testBackendProfileAuthority() {
  using namespace lang;
  using namespace lang::driver;
  TemporaryDirectory temporary;
  const auto manifest = temporary.root() / "gti.toml";
  const std::string prefix =
      "manifest-version = 1\n[package]\nname = \"backend-policy\"\n"
      "version = \"0.1.0\"\n[targets.app]\nkind = \"executable\"\n"
      "root = \"src/main.gti\"\n[profiles.dev]\n";
  expect(writeProjectFile(temporary.root() / "src/main.gti",
                          "int main() { return 0; }\n") &&
             writeProjectFile(manifest, prefix),
         "backend policy fixture is writable");
  const auto resolve = [&](ProjectBuildOverrides overrides = {}) {
    return resolveProjectBuild(ProjectBuildRequest(
        temporary.root(), "app", "dev", TargetInfo::host(), overrides));
  };
  expect(writeProjectFile(manifest, prefix + "keep-cpp = true\n"),
         "C++ retention profile is writable");
  const auto retained = resolve();
  expect(retained.succeeded() && retained.plan->backend() == BackendKind::Cpp &&
             retained.plan->generatedSource().extension() == ".cpp",
         "C++ retention without a backend keeps the C++ backend it configures");
  expect(writeProjectFile(manifest, prefix + "backend = \"llvm\"\n"),
         "LLVM profile is writable");
  const auto profile = resolve();
  ProjectBuildOverrides cpp;
  cpp.backend = BackendKind::Cpp;
  const auto overridden = resolve(cpp);
  expect(profile.succeeded() && overridden.succeeded(),
         "profile and explicit executable backend policies resolve");
  if (profile.plan && overridden.plan) {
    expect(profile.plan->backend() == BackendKind::Llvm &&
               profile.plan->generatedSource().extension() == ".o" &&
               overridden.plan->backend() == BackendKind::Cpp &&
               overridden.plan->generatedSource().extension() == ".cpp",
           "explicit backend overrides the profile before artifact planning");
    expect(profile.plan->projectModelIdentity() !=
               overridden.plan->projectModelIdentity(),
           "cache policy binds the effective backend, not whether it was "
           "explicit");
  }
  expect(writeProjectFile(manifest,
                          prefix + "backend = \"llvm\"\nkeep-cpp = true\n"),
         "conflicting profile is writable");
  expect(
      !resolve().succeeded() && resolve(cpp).succeeded(),
      "effective backend authority decides whether profile retention is valid");
}

// compileToCpp rejects an LLVM diagnostic-instrumentation request before
// compilation (formerly in llvm_cache_tests.cpp).
void testCppRejectsLlvmInstrumentation() {
  TemporaryDirectory temporary;
  expect(writeFile(temporary.root() / "prelude.gti", "") &&
             writeFile(temporary.root() / "main.gti",
                       "int main() { return 0; }\n"),
         "instrumentation rejection fixture is writable");
  const auto addressRequest =
      lang::driver::CompilationRequest(
          temporary.root() / "main.gti",
          lang::standardLibraryLayout(temporary.root()),
          lang::TargetInfo::host(), lang::OptimizationLevel::O3,
          lang::CppStandard::Cpp23)
          .withBackend(lang::BackendKind::Llvm)
          .withLlvmInstrumentation(lang::LlvmInstrumentationMode::Address);
  const auto invalidRequest = [&](const auto &action) {
    bool rejected = false;
    try {
      action();
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    expect(rejected,
           "invalid diagnostic request must reject before compilation");
  };
  invalidRequest(
      [&] { static_cast<void>(lang::driver::compileToCpp(addressRequest)); });
  invalidRequest([&] {
    static_cast<void>(lang::driver::compileToCpp(
        addressRequest, lang::driver::CompilationInputs{}));
  });
}

// The C++ backend's schema-v4 whole-program cache stays isolated from the
// LLVM native-object (v5) cache of the same project (formerly in
// llvm_cache_tests.cpp, on its tests/fixtures/llvm_cache project).
void testLlvmAndCppCacheIsolation(const std::filesystem::path &self) {
  using namespace lang;
  using namespace lang::driver;
  TemporaryDirectory temporary;
  const auto project = temporary.root() / "project";
  const auto managedRoot = project / "build/gti";
  const auto output = managedRoot / "app";
  const auto representation = managedRoot / "app.o";
  std::filesystem::create_directories(managedRoot);
  std::filesystem::copy(GTI_LLVM_CACHE_FIXTURE, project,
                        std::filesystem::copy_options::recursive);
  const auto edges = project / "edges";
  std::filesystem::remove(edges / "left-leaf.gti");
  std::filesystem::remove(edges / "right-leaf.gti");
  std::filesystem::create_symlink("first.gti", edges / "left-leaf.gti");
  std::filesystem::create_symlink("second.gti", edges / "right-leaf.gti");
  const auto toolchain = discoverToolchainLayout(self.c_str());
  const ManagedOutputPolicy managed{.trustedRoot = project,
                                    .outputRoot = managedRoot};
  const auto build = [&](BackendKind backend, bool keep) {
    return buildExecutable(ExecutableBuildRequest(
        CompilationRequest(project / "main.gti", toolchain.standardLibrary,
                           TargetInfo::host(), OptimizationLevel::O3,
                           CppStandard::Cpp23)
            .withBackend(backend),
        toolchain,
        backend == BackendKind::Cpp ? managedRoot / "app.cpp" : representation,
        output, "c++", {}, keep, true, false, managed, std::nullopt,
        BuildCachePolicy{.root = managedRoot / "cache",
                         .sourceRoot = project,
                         .compilerIdentity = "llvm-cache-test-v1",
                         .projectModelIdentity = "llvm-cache-project-v1"}));
  };
  const auto execute = [&](int expected) {
    const auto result = invokeProcess({output.string()},
                                      {.outputMode = ProcessOutputMode::Capture,
                                       .captureSuccessfulOutput = true});
    expect(result.exitCode == expected && result.output.empty(),
           "cached executable has incorrect observable behavior");
  };
  const auto first = build(BackendKind::Llvm, false);
  expect(first.succeeded() && first.cache.status == BuildCacheStatus::Miss &&
             first.cache.entry.parent_path().filename() == "v5",
         "first native-object build must miss and publish a v5 entry");
  if (!first.succeeded())
    return;
  const auto executable = readFile(output);
  execute(17);
  const auto cpp = build(BackendKind::Cpp, true);
  expect(
      cpp.succeeded() && cpp.cache.status == BuildCacheStatus::Miss &&
          cpp.cache.key != first.cache.key &&
          cpp.cache.entry.parent_path().filename() == "v4" &&
          readFile(cpp.cache.entry / "metadata")
              .starts_with("gti-build-cache-v4\n") &&
          std::filesystem::is_regular_file(cpp.cache.entry / "generated.cpp"),
      "C++ artifacts must remain isolated in unchanged schema v4");
  const auto cppHit = build(BackendKind::Cpp, true);
  expect(cppHit.succeeded() && cppHit.cache.status == BuildCacheStatus::Hit &&
             !cppHit.nativeProcess && cppHit.compilation.artifact &&
             cppHit.compilation.artifact->kind == BackendArtifactKind::Source &&
             cppHit.compilation.artifact->extension == ".cpp",
         "C++ v4 cache must still restore generated source");
  execute(17);
  const auto nativeAgain = build(BackendKind::Llvm, false);
  expect(nativeAgain.succeeded() &&
             nativeAgain.cache.status == BuildCacheStatus::Hit &&
             nativeAgain.cache.key == first.cache.key &&
             readFile(output) == executable,
         "C++ build must not replace a native-object cache entry");
}

} // namespace

int main(int argc, char *argv[]) {
  lang::installCrashHandlers(argc > 0 ? argv[0] : "gti_cpp_mir_driver_tests");
  if (const std::optional<int> wrapped = runNativeCompilerWrapper(argc, argv))
    return *wrapped;

  testCompilationRequestAndTargetPropagation();
  testDefinitionClosureWithoutEntryPoint();
  testWholeProgramBuildCache(std::filesystem::absolute(argv[0]));
  testBuildProvenance(std::filesystem::absolute(argv[0]));
  testBackendProfileAuthority();
  testCppRejectsLlvmInstrumentation();
  testLlvmAndCppCacheIsolation(std::filesystem::absolute(argv[0]));

  if (failures != 0) {
    std::cerr << failures << " C++ backend driver test(s) failed\n";
    return 1;
  }
  std::cout << "All C++ backend driver tests passed\n";
  return 0;
}
