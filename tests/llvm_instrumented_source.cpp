// Test-only access to existing driver and native-emitter instrumentation.
// This adds no public CLI or source-language rule.
#include "../src/compiler/llvm/body.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/build.h"
#include "gti/driver/native_toolchain.h"
#include "gti/llvm_backend.h"
#include <filesystem>
#include <iostream>
#include <llvm/Support/raw_ostream.h>
#include <string>

int main(int argc, char **argv) {
  using namespace lang;
  using namespace lang::driver;
  if (argc != 6 && argc != 7 && argc != 8)
    return 2;
  const auto level = std::string(argv[4]);
  if (level != "O0" && level != "O1" && level != "O3")
    return 2;
  const auto mode = argc >= 7 ? std::string(argv[6]) : "address,undefined";
  if (mode != "address,undefined" && mode != "thread")
    return 2;
  const auto optimization = level == "O0"   ? OptimizationLevel::O0
                            : level == "O1" ? OptimizationLevel::O1
                                            : OptimizationLevel::O3;
  auto toolchain = discoverToolchainLayout(argv[0]);
  toolchain.standardLibrary = standardLibraryLayout(argv[1]);
  auto target = TargetInfo::host();
  const auto profile = argc == 8 ? std::string(argv[7]) : "concurrent";
  if (profile != "concurrent" && profile != "single-threaded")
    return 2;
  target.executionProfile = profile == "concurrent"
                                ? ExecutionProfile::Concurrent
                                : ExecutionProfile::SingleThreaded;
  target.runtimeCapabilities.threads = true;
  auto request =
      CompilationRequest(argv[2], toolchain.standardLibrary, target,
                         optimization, CppStandard::Cpp23)
          .withBackend(BackendKind::Llvm)
          .withLlvmInstrumentation(
              mode == "thread"
                  ? LlvmInstrumentationMode::Thread
                  : LlvmInstrumentationMode::AddressAndUndefinedMemory);
  const std::filesystem::path output(argv[3]);
  auto object = output;
  object += ".o";
  if (mode == "thread") {
    // Test-only sanitizer composition, like llvm_mutex_contract_tests. Keep
    // the public driver's provenance rejection. Instrument a second emission
    // of the same publicly admitted verified program and link only runtime
    // sources compiled by the harness with the matching native sanitizer.
    FrontendOptions frontendOptions;
    frontendOptions.target = target;
    frontendOptions.requireEntryPoint = true;
    frontendOptions.lowerReachableOnly = true;
    frontendOptions.lowerLibraryReachableOnly = true;
    const std::filesystem::path library(argv[1]);
    const auto frontend =
        Frontend(frontendOptions)
            .analyze(argv[2], std::nullopt, {library / "prelude.gti"}, {},
                     {library});
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      return 1;
    }
    std::vector<LoweredProgramIssue> issues;
    const auto input = verifiedLoweredProgram(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, target),
        issues);
    if (!input)
      return 1;
    const auto admission = planLlvmAdmission(*input);
    if (!admission.admitted()) {
      std::cerr << "Thread sanitizer fixture must pass public LLVM admission\n";
      return 1;
    }
    native_llvm::Session session(*input, {});
    native_llvm::lowerScalarProgram(session, admission);
    for (auto &function : session.module())
      if (!function.isDeclaration())
        function.addFnAttr(llvm::Attribute::SanitizeThread);
    session.verify();
    std::error_code error;
    llvm::raw_fd_ostream stream(output.string() + ".ll", error);
    if (error)
      return 1;
    session.module().print(stream, nullptr);
    stream.close();
    return stream.has_error() ? 1 : 0;
  }
  const auto result = buildExecutable(ExecutableBuildRequest(
      request, toolchain, object, output, argv[5], {}, true, true, true));
  if (!result.succeeded()) {
    std::cerr << result.driverDiagnostic.value_or("") << '\n';
    if (result.nativeProcess)
      std::cerr << result.nativeProcess->output;
    for (const auto &diagnostic : result.compilation.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  return 0;
}
