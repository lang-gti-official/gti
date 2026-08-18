#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/compilation.h"
#include "gti/driver/manifest.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/project.h"

#include <string>
#include <utility>
#include <vector>

int main() {
  const lang::driver::CompilationRequest compilation(
      "main.gti", lang::standardLibraryLayout("stdlib"),
      lang::TargetInfo{.os = "linux", .vendor = "unknown", .arch = "x86_64"},
      lang::OptimizationLevel::O2, lang::CppStandard::Cpp23);
  if (compilation.entry() != "main.gti" || compilation.target().os != "linux" ||
      compilation.standardLibrary().prelude != "stdlib/prelude.gti") {
    return 1;
  }

  lang::driver::NativeInputs inputs;
  inputs.includeDirectories.emplace_back("runtime include");
  inputs.libraryFiles.emplace_back("libgti_runtime.a");
  inputs.trailingArguments.emplace_back("-pthread");

  const lang::driver::NativeCompileRequest request(
      "c++", "generated.cpp", "program", lang::CppStandard::Cpp23,
      lang::OptimizationLevel::O2, std::move(inputs));
  const std::vector<std::string> command =
      lang::driver::NativeToolchain().command(request);
  std::vector<std::string> expectedCommand{"c++",
                                           "-std=c++23",
                                           "-O2",
                                           "-Iruntime include",
                                           "generated.cpp",
                                           "libgti_runtime.a",
                                           "-o",
                                           "program",
                                           "-pthread",
                                           "-fno-fast-math",
                                           "-ffp-contract=off",
                                           "-D__gti_strict_ieee754=1"};
#if defined(__APPLE__)
  expectedCommand.emplace_back("-Wl,-reproducible");
#endif
  if (command != expectedCommand) {
    return 2;
  }

  const std::string rendered = lang::driver::renderCommand(command);
  const std::filesystem::path temporary =
      lang::driver::temporaryCppPath("main.gti");
  if (rendered.find("\"-Iruntime include\"") == std::string::npos ||
      temporary.extension() != ".cpp") {
    return 3;
  }

  const lang::driver::ManifestLoadResult missingManifest =
      lang::driver::loadProjectManifest("missing-gti.toml");
  if (missingManifest.succeeded() ||
      lang::driver::projectTargetKindName(
          lang::driver::ProjectTargetKind::Test) != "test" ||
      lang::driver::targetTriple(
          {.os = "macos", .vendor = "apple", .arch = "arm64"}) !=
          "arm64-apple-macos") {
    return 4;
  }

  const lang::driver::ToolchainLayout toolchain{
      .standardLibrary = lang::standardLibraryLayout("stdlib"),
      .runtimeInclude = "include",
      .runtimeLibrary = "libgti_runtime.a",
      .vendorInclude = "vendor",
  };
  const lang::driver::ExecutableBuildRequest buildRequest(
      compilation, toolchain, "generated.cpp", "program", "c++", {}, false,
      false, false);
  return buildRequest.output() == "program" &&
                 !buildRequest.keepGeneratedSource()
             ? 0
             : 5;
}
