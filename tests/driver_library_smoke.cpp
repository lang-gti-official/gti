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
      lang::TargetInfo{.os = "test", .vendor = "test", .arch = "test"},
      lang::OptimizationLevel::O2, lang::CppStandard::Cpp23);
  if (compilation.entry() != "main.gti" || compilation.target().os != "test" ||
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
  if (command.size() != 12 || command.front() != "c++" ||
      command[1] != "-std=c++23" || command[2] != "-O2" ||
      command[3] != "-Iruntime include" || command[4] != "generated.cpp" ||
      command[5] != "libgti_runtime.a" || command[6] != "-o" ||
      command[7] != "program" || command[8] != "-pthread" ||
      command[9] != "-fno-fast-math" || command[10] != "-ffp-contract=off" ||
      command[11] != "-D__gti_strict_binary32=1") {
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
      lang::driver::targetTriple(
          {.os = "test", .vendor = "vendor", .arch = "arch"}) !=
          "arch-vendor-test") {
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
