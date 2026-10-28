// Test-only entry to the existing typed driver instrumentation API. This is
// not a new public CLI or an alternate GTI compilation path.
#include "gti/driver/build.h"
#include "gti/driver/native_toolchain.h"
#include "gti/llvm_backend.h"
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  using namespace lang;
  using namespace lang::driver;
  if (argc != 6)
    return 2;
  const auto level = std::string(argv[4]);
  if (level != "O0" && level != "O3")
    return 2;
  auto toolchain = discoverToolchainLayout(argv[0]);
  toolchain.standardLibrary = standardLibraryLayout(argv[1]);
  auto target = TargetInfo::host();
  target.executionProfile = ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = true;
  auto request = CompilationRequest(argv[2], toolchain.standardLibrary, target,
                                    level == "O0" ? OptimizationLevel::O0
                                                  : OptimizationLevel::O3,
                                    CppStandard::Cpp23)
                     .withBackend(BackendKind::Llvm)
                     .withLlvmInstrumentation(
                         LlvmInstrumentationMode::AddressAndUndefinedMemory);
  const std::filesystem::path output(argv[3]);
  auto object = output;
  object += ".o";
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
