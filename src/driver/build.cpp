#include "gti/driver/build.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace lang::driver {
namespace {

template <typename Value>
void append(std::vector<Value> &destination, const std::vector<Value> &source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

std::optional<std::string> createParent(const std::filesystem::path &artifact) {
  const std::filesystem::path parent = artifact.parent_path();
  if (parent.empty()) {
    return std::nullopt;
  }
  std::error_code error;
  std::filesystem::create_directories(parent, error);
  if (!error) {
    return std::nullopt;
  }
  return "gti: failed to create output directory '" + parent.string() +
         "': " + error.message();
}

NativeInputs effectiveNativeInputs(const ToolchainLayout &toolchain,
                                   CppStandard standard,
                                   const NativeInputs &additional) {
  NativeInputs inputs;
  inputs.includeDirectories.push_back(toolchain.runtimeInclude);
  if (standard == CppStandard::Cpp20 &&
      toolchain.vendorInclude != toolchain.runtimeInclude) {
    inputs.includeDirectories.push_back(toolchain.vendorInclude);
  }
  append(inputs.includeDirectories, additional.includeDirectories);
  append(inputs.compilerArguments, additional.compilerArguments);
  append(inputs.libraryDirectories, additional.libraryDirectories);
  inputs.libraryFiles.push_back(toolchain.runtimeLibrary);
  append(inputs.libraryFiles, additional.libraryFiles);
  append(inputs.libraries, additional.libraries);
  append(inputs.frameworks, additional.frameworks);
  append(inputs.linkerArguments, additional.linkerArguments);
  append(inputs.trailingArguments, additional.trailingArguments);
  return inputs;
}

} // namespace

ExecutableBuildRequest::ExecutableBuildRequest(
    CompilationRequest compilation, ToolchainLayout toolchain,
    std::filesystem::path generatedSource, std::filesystem::path output,
    std::string nativeCompiler, NativeInputs nativeInputs,
    bool keepGeneratedSource, bool createParentDirectories,
    bool captureSuccessfulNativeOutput)
    : compilationRequest(std::move(compilation)),
      toolchainLayout(std::move(toolchain)),
      generatedSourcePath(std::move(generatedSource)),
      outputPath(std::move(output)),
      compilerExecutable(std::move(nativeCompiler)),
      additionalNativeInputs(std::move(nativeInputs)),
      retainGeneratedSource(keepGeneratedSource),
      createParents(createParentDirectories),
      captureSuccessfulOutput(captureSuccessfulNativeOutput) {}

const CompilationRequest &ExecutableBuildRequest::compilation() const {
  return compilationRequest;
}

const ToolchainLayout &ExecutableBuildRequest::toolchain() const {
  return toolchainLayout;
}

const std::filesystem::path &ExecutableBuildRequest::generatedSource() const {
  return generatedSourcePath;
}

const std::filesystem::path &ExecutableBuildRequest::output() const {
  return outputPath;
}

const std::string &ExecutableBuildRequest::nativeCompiler() const {
  return compilerExecutable;
}

const NativeInputs &ExecutableBuildRequest::nativeInputs() const {
  return additionalNativeInputs;
}

bool ExecutableBuildRequest::keepGeneratedSource() const {
  return retainGeneratedSource;
}

bool ExecutableBuildRequest::createParentDirectories() const {
  return createParents;
}

bool ExecutableBuildRequest::captureSuccessfulNativeOutput() const {
  return captureSuccessfulOutput;
}

ExecutableBuildResult buildExecutable(const ExecutableBuildRequest &request) {
  ExecutableBuildResult result;
  result.generatedSource = request.generatedSource();
  result.compilation = compileToCpp(request.compilation());
  if (!result.compilation.succeeded()) {
    result.status = ExecutableBuildStatus::CompilationFailure;
    return result;
  }

  if (request.createParentDirectories()) {
    if (const std::optional<std::string> diagnostic =
            createParent(request.generatedSource())) {
      result.status = ExecutableBuildStatus::OutputDirectoryFailure;
      result.driverDiagnostic = diagnostic;
      return result;
    }
    if (const std::optional<std::string> diagnostic =
            createParent(request.output())) {
      result.status = ExecutableBuildStatus::OutputDirectoryFailure;
      result.driverDiagnostic = diagnostic;
      return result;
    }
  }

  result.artifactWriteStatus = writeArtifact(
      request.generatedSource(), result.compilation.artifact->contents);
  if (*result.artifactWriteStatus != ArtifactWriteStatus::Success) {
    result.status = ExecutableBuildStatus::GeneratedArtifactFailure;
    return result;
  }

  TemporaryArtifact generatedArtifact(request.generatedSource(),
                                      !request.keepGeneratedSource());
  result.resourceError = validateToolchainLayout(
      request.toolchain(), request.compilation().cppStandard());
  if (result.resourceError) {
    result.status = ExecutableBuildStatus::ToolchainConfigurationFailure;
    return result;
  }

  const NativeCompileRequest nativeRequest(
      request.nativeCompiler(), request.generatedSource(), request.output(),
      request.compilation().cppStandard(), request.compilation().optimization(),
      effectiveNativeInputs(request.toolchain(),
                            request.compilation().cppStandard(),
                            request.nativeInputs()));
  const NativeToolchain nativeToolchain;
  result.nativeCommand = nativeToolchain.command(nativeRequest);
  result.nativeProcess = nativeToolchain.invoke(
      nativeRequest,
      {.captureSuccessfulOutput = request.captureSuccessfulNativeOutput()});
  if (result.nativeProcess->driverDiagnostic) {
    result.driverDiagnostic = result.nativeProcess->driverDiagnostic;
  }
  if (!result.nativeProcess->succeeded()) {
    generatedArtifact.keep();
    result.generatedSourceRetained = true;
    result.status = ExecutableBuildStatus::NativeCompilerFailure;
    return result;
  }

  result.generatedSourceRetained = request.keepGeneratedSource();
  result.status = ExecutableBuildStatus::Success;
  return result;
}

} // namespace lang::driver
