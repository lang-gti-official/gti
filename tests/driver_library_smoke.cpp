#include "gti/driver/artifact.h"
#include "gti/driver/build.h"
#include "gti/driver/compilation.h"
#include "gti/driver/manifest.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/project.h"
#include "gti/driver/project_facts.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(!std::is_default_constructible_v<lang::driver::ProjectFacts>);
static_assert(std::is_copy_constructible_v<lang::driver::ProjectFacts>);
static_assert(std::is_move_constructible_v<lang::driver::ProjectFacts>);

int main() {
  const lang::driver::CompilationRequest compilation(
      "main.gti", lang::standardLibraryLayout("stdlib"),
      lang::TargetInfo{.os = "linux", .vendor = "unknown", .arch = "x86_64"},
      lang::OptimizationLevel::O2, lang::CppStandard::Cpp23, {}, {},
      {.capturePreMir = true, .capturePostMir = true, .captureReport = true});
  if (compilation.entry() != "main.gti" || compilation.target().os != "linux" ||
      compilation.standardLibrary().prelude != "stdlib/prelude.gti" ||
      !compilation.optimizationOutputs().requested()) {
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
  const lang::driver::CompilerBuildIdentity buildIdentity =
      lang::driver::currentCompilerBuildIdentity();

  // The read-only project-facts surface is part of the published driver
  // library: an installed consumer must be able to ask for a project that is
  // not there and receive a discovery failure rather than a partial answer.
  const lang::driver::ProjectFactsResult facts =
      lang::driver::resolveProjectFacts("gti-absent-project-directory");
  if (facts.succeeded() || facts.facts.has_value() ||
      facts.status != lang::driver::ProjectFactsStatus::DiscoveryFailure) {
    return 6;
  }

  return buildRequest.output() == "program" &&
                 !buildRequest.keepGeneratedSource() &&
                 lang::driver::currentBuildProvenanceSchema == 1 &&
                 !lang::driver::compilerSourceStateName(buildIdentity.state)
                      .empty()
             ? 0
             : 5;
}
