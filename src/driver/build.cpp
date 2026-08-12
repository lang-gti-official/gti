#include "gti/driver/build.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

bool pathIsWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate) {
  auto rootPart = root.begin();
  auto candidatePart = candidate.begin();
  for (; rootPart != root.end() && candidatePart != candidate.end();
       ++rootPart, ++candidatePart) {
    if (*rootPart != *candidatePart) {
      return false;
    }
  }
  return rootPart == root.end();
}

std::optional<std::string>
managedPathDiagnostic(const ManagedOutputPolicy &policy,
                      const std::filesystem::path &artifact, bool create) {
  std::error_code error;
  const std::filesystem::path trustedRoot =
      std::filesystem::absolute(policy.trustedRoot, error).lexically_normal();
  if (error || trustedRoot.empty() || trustedRoot == trustedRoot.root_path()) {
    return "gti: refusing an invalid managed output root '" +
           policy.trustedRoot.string() + "'";
  }

  error.clear();
  const std::filesystem::path outputRoot =
      std::filesystem::absolute(policy.outputRoot, error).lexically_normal();
  if (error || outputRoot == trustedRoot ||
      !pathIsWithin(trustedRoot, outputRoot)) {
    return "gti: refusing managed output root '" + policy.outputRoot.string() +
           "' outside its trusted project root";
  }

  error.clear();
  const std::filesystem::path absoluteArtifact =
      std::filesystem::absolute(artifact, error).lexically_normal();
  if (error || absoluteArtifact == outputRoot ||
      !pathIsWithin(outputRoot, absoluteArtifact)) {
    return "gti: refusing artifact path '" + artifact.string() +
           "' outside the managed project output root";
  }

  const std::filesystem::path parent = absoluteArtifact.parent_path();
  std::filesystem::path current = trustedRoot;
  const std::filesystem::path relative = parent.lexically_relative(trustedRoot);
  for (const std::filesystem::path &component : relative) {
    if (component.empty() || component == ".") {
      continue;
    }
    if (component == "..") {
      return "gti: refusing artifact path '" + artifact.string() +
             "' outside its trusted project root";
    }
    current /= component;

    error.clear();
    std::filesystem::file_status status =
        std::filesystem::symlink_status(current, error);
    const bool missing = error == std::errc::no_such_file_or_directory ||
                         (!error && !std::filesystem::exists(status));
    if (missing) {
      error.clear();
      if (!create) {
        return std::nullopt;
      }
      std::filesystem::create_directory(current, error);
      if (error) {
        return "gti: failed to create managed output directory '" +
               current.string() + "': " + error.message();
      }
      error.clear();
      status = std::filesystem::symlink_status(current, error);
    }
    if (error) {
      return "gti: failed to inspect managed output directory '" +
             current.string() + "': " + error.message();
    }
    if (std::filesystem::is_symlink(status)) {
      return "gti: refusing to traverse symbolic-link managed output "
             "directory '" +
             current.string() + "'";
    }
    if (!std::filesystem::is_directory(status)) {
      return "gti: managed output path component is not a directory: '" +
             current.string() + "'";
    }
  }
  return std::nullopt;
}

std::optional<std::string>
loadedSourceCollisionDiagnostic(const std::filesystem::path &artifact,
                                std::string_view artifactKind,
                                const SourceManager &sources) {
  const std::optional<std::filesystem::path> collision =
      findLoadedSourceCollision(artifact, sources);
  if (!collision) {
    return std::nullopt;
  }
  return "gti: refusing to overwrite loaded source '" + collision->string() +
         "' with " + std::string(artifactKind) + " '" + artifact.string() + "'";
}

NativeInputs
effectiveNativeInputs(const ToolchainLayout &toolchain, CppStandard standard,
                      const NativeInputs &additional,
                      const std::vector<std::filesystem::path> &nativeObjects) {
  NativeInputs inputs;
  inputs.includeDirectories.push_back(toolchain.runtimeInclude);
  if (standard == CppStandard::Cpp20 &&
      toolchain.vendorInclude != toolchain.runtimeInclude) {
    inputs.includeDirectories.push_back(toolchain.vendorInclude);
  }
  append(inputs.includeDirectories, additional.includeDirectories);
  append(inputs.compilerArguments, additional.compilerArguments);
  append(inputs.libraryDirectories, additional.libraryDirectories);
  if (additional.orderedLinkOperands.empty()) {
    append(inputs.libraryFiles, nativeObjects);
    inputs.libraryFiles.push_back(toolchain.runtimeLibrary);
    append(inputs.libraryFiles, additional.libraryFiles);
    append(inputs.libraries, additional.libraries);
    append(inputs.frameworks, additional.frameworks);
  } else {
    for (const std::filesystem::path &object : nativeObjects) {
      inputs.orderedLinkOperands.push_back(
          {NativeLinkOperandKind::File, object.string()});
    }
    inputs.orderedLinkOperands.push_back(
        {NativeLinkOperandKind::File, toolchain.runtimeLibrary.string()});
    append(inputs.orderedLinkOperands, additional.orderedLinkOperands);
  }
  append(inputs.linkerArguments, additional.linkerArguments);
  append(inputs.trailingArguments, additional.trailingArguments);
  return inputs;
}

std::filesystem::path
nativeObjectPath(const std::filesystem::path &generatedSource,
                 const std::filesystem::path &nativeSource, std::size_t index) {
#if defined(_WIN32)
  constexpr std::string_view extension = ".obj";
#else
  constexpr std::string_view extension = ".o";
#endif
  const std::string generatedStem = generatedSource.stem().empty()
                                        ? "generated"
                                        : generatedSource.stem().string();
  const std::string nativeStem =
      nativeSource.stem().empty() ? "source" : nativeSource.stem().string();
  return generatedSource.parent_path() /
         (generatedStem + ".native-" + std::to_string(index) + "-" +
          nativeStem + std::string(extension));
}

} // namespace

ExecutableBuildRequest::ExecutableBuildRequest(
    CompilationRequest compilation, ToolchainLayout toolchain,
    std::filesystem::path generatedSource, std::filesystem::path output,
    std::string nativeCompiler, NativeInputs nativeInputs,
    bool keepGeneratedSource, bool createParentDirectories,
    bool captureSuccessfulNativeOutput,
    std::optional<ManagedOutputPolicy> managedOutput,
    std::optional<std::string> cCompiler)
    : compilationRequest(std::move(compilation)),
      toolchainLayout(std::move(toolchain)),
      generatedSourcePath(std::move(generatedSource)),
      outputPath(std::move(output)),
      compilerExecutable(std::move(nativeCompiler)),
      cCompilerExecutable(std::move(cCompiler)),
      additionalNativeInputs(std::move(nativeInputs)),
      retainGeneratedSource(keepGeneratedSource),
      createParents(createParentDirectories),
      captureSuccessfulOutput(captureSuccessfulNativeOutput),
      managedOutputPolicy(std::move(managedOutput)) {}

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

const std::optional<std::string> &ExecutableBuildRequest::cCompiler() const {
  return cCompilerExecutable;
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

const std::optional<ManagedOutputPolicy> &
ExecutableBuildRequest::managedOutput() const {
  return managedOutputPolicy;
}

ExecutableBuildResult buildExecutable(const ExecutableBuildRequest &request) {
  ExecutableBuildResult result;
  result.generatedSource = request.generatedSource();
  std::vector<std::filesystem::path> nativeObjects;
  nativeObjects.reserve(request.nativeInputs().cSources.size());
  for (std::size_t index = 0; index < request.nativeInputs().cSources.size();
       ++index) {
    nativeObjects.push_back(
        nativeObjectPath(request.generatedSource(),
                         request.nativeInputs().cSources[index], index));
  }
  if (!nativeObjects.empty() &&
      (!request.cCompiler() || request.cCompiler()->empty())) {
    result.status = ExecutableBuildStatus::ToolchainConfigurationFailure;
    result.driverDiagnostic =
        "gti: native C sources require a selected C compiler";
    return result;
  }
  if (request.managedOutput()) {
    if (const std::optional<std::string> diagnostic = managedPathDiagnostic(
            *request.managedOutput(), request.generatedSource(), false)) {
      result.status = ExecutableBuildStatus::OutputDirectoryFailure;
      result.driverDiagnostic = diagnostic;
      return result;
    }
    if (const std::optional<std::string> diagnostic = managedPathDiagnostic(
            *request.managedOutput(), request.output(), false)) {
      result.status = ExecutableBuildStatus::OutputDirectoryFailure;
      result.driverDiagnostic = diagnostic;
      return result;
    }
    for (const std::filesystem::path &nativeObject : nativeObjects) {
      if (const std::optional<std::string> diagnostic = managedPathDiagnostic(
              *request.managedOutput(), nativeObject, false)) {
        result.status = ExecutableBuildStatus::OutputDirectoryFailure;
        result.driverDiagnostic = diagnostic;
        return result;
      }
    }
  }

  result.compilation = compileToCpp(request.compilation());
  if (!result.compilation.succeeded()) {
    result.status = ExecutableBuildStatus::CompilationFailure;
    return result;
  }

  if (const std::optional<std::string> diagnostic =
          loadedSourceCollisionDiagnostic(request.output(), "executable output",
                                          result.compilation.sources)) {
    result.status = ExecutableBuildStatus::ArtifactPathConflict;
    result.driverDiagnostic = diagnostic;
    return result;
  }
  if (const std::optional<std::string> diagnostic =
          loadedSourceCollisionDiagnostic(request.generatedSource(),
                                          "generated C++ output",
                                          result.compilation.sources)) {
    result.status = ExecutableBuildStatus::ArtifactPathConflict;
    result.driverDiagnostic = diagnostic;
    return result;
  }

  if (request.createParentDirectories()) {
    const std::optional<std::string> generatedDiagnostic =
        request.managedOutput()
            ? managedPathDiagnostic(*request.managedOutput(),
                                    request.generatedSource(), true)
            : createParent(request.generatedSource());
    if (generatedDiagnostic) {
      result.status = ExecutableBuildStatus::OutputDirectoryFailure;
      result.driverDiagnostic = generatedDiagnostic;
      return result;
    }
    const std::optional<std::string> outputDiagnostic =
        request.managedOutput()
            ? managedPathDiagnostic(*request.managedOutput(), request.output(),
                                    true)
            : createParent(request.output());
    if (outputDiagnostic) {
      result.status = ExecutableBuildStatus::OutputDirectoryFailure;
      result.driverDiagnostic = outputDiagnostic;
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

  const NativeToolchain nativeToolchain;
  std::vector<std::filesystem::path> cIncludeDirectories{
      request.toolchain().runtimeInclude};
  append(cIncludeDirectories, request.nativeInputs().includeDirectories);
  for (std::size_t index = 0; index < nativeObjects.size(); ++index) {
    const std::filesystem::path stagedObject =
        stagedArtifactPath(nativeObjects[index]);
    TemporaryArtifact stagedNativeObject(stagedObject, true);
    const NativeCCompileRequest cRequest(
        *request.cCompiler(), request.nativeInputs().cSources[index],
        stagedObject, request.nativeInputs().cStandard.value_or(CStandard::C17),
        request.compilation().optimization(), cIncludeDirectories,
        request.nativeInputs().cCompilerArguments);
    NativeCCompilationResult cCompilation{
        .source = request.nativeInputs().cSources[index],
        .object = nativeObjects[index],
        .command = nativeToolchain.command(cRequest),
        .process = nativeToolchain.invoke(
            cRequest, {.captureSuccessfulOutput =
                           request.captureSuccessfulNativeOutput()}),
    };
    if (!cCompilation.process.succeeded()) {
      result.cCompilations.push_back(std::move(cCompilation));
      generatedArtifact.keep();
      result.generatedSourceRetained = true;
      result.status = ExecutableBuildStatus::NativeCCompilerFailure;
      return result;
    }
    cCompilation.artifactPublishResult =
        publishArtifact(stagedObject, nativeObjects[index]);
    const bool published = cCompilation.artifactPublishResult->succeeded();
    if (!published) {
      result.driverDiagnostic =
          "gti: failed to publish native object '" +
          nativeObjects[index].string() +
          "': " + cCompilation.artifactPublishResult->error.message();
    }
    result.cCompilations.push_back(std::move(cCompilation));
    if (!published) {
      generatedArtifact.keep();
      result.generatedSourceRetained = true;
      result.status = ExecutableBuildStatus::NativeObjectPublicationFailure;
      return result;
    }
  }

  const std::filesystem::path stagedOutput =
      stagedArtifactPath(request.output());
  TemporaryArtifact stagedArtifact(stagedOutput, true);
  const NativeCompileRequest nativeRequest(
      request.nativeCompiler(), request.generatedSource(), stagedOutput,
      request.compilation().cppStandard(), request.compilation().optimization(),
      effectiveNativeInputs(request.toolchain(),
                            request.compilation().cppStandard(),
                            request.nativeInputs(), nativeObjects));
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

  result.artifactPublishResult =
      publishArtifact(stagedOutput, request.output());
  if (!result.artifactPublishResult->succeeded()) {
    generatedArtifact.keep();
    result.generatedSourceRetained = true;
    result.driverDiagnostic =
        "gti: failed to publish executable '" + request.output().string() +
        "': " + result.artifactPublishResult->error.message();
    result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
    return result;
  }

  result.generatedSourceRetained = request.keepGeneratedSource();
  result.status = ExecutableBuildStatus::Success;
  return result;
}

} // namespace lang::driver
