#include "gti/driver/embedding_library.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"

#include <algorithm>
#include <set>

namespace lang::driver {
namespace {
bool identifier(std::string_view name) {
  const auto letter = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
  };
  return !name.empty() && letter(name.front()) &&
         std::all_of(name.begin(), name.end(), [&](char c) {
           return letter(c) || (c >= '0' && c <= '9');
         });
}
} // namespace

EmbeddingLibraryResult
publishEmbeddingLibrary(const LlvmEmbeddingArtifacts &artifacts,
                        const EmbeddingLibraryRequest &request) {
  EmbeddingLibraryResult result;
  const auto reject = [&](std::string message) {
    result.error = std::move(message);
    return result;
  };
  const auto host = TargetInfo::host();
  const bool darwin =
      artifacts.target.os == "macos" && artifacts.target.arch == "arm64";
  const bool sysv =
      artifacts.target.os == "linux" && artifacts.target.arch == "x86_64";
  if ((!darwin && !sysv) || artifacts.target.os != host.os ||
      artifacts.target.arch != host.arch)
    return reject(
        "embedding publication requires the supported native host target");
  if (!identifier(request.name) || request.compiler.empty() ||
      artifacts.object.kind != BackendArtifactKind::Object ||
      artifacts.object.contents.empty() ||
      artifacts.header.kind != BackendArtifactKind::Source ||
      artifacts.header.contents.empty())
    return reject("invalid embedding artifact or generation name");
  std::set<std::string> symbols;
  for (const auto &name : artifacts.publicSymbols)
    if (!identifier(name) || !symbols.insert(name).second)
      return reject("invalid or duplicate embedding public symbol");
  if (!symbols.contains("gti_context_create_v1") ||
      !symbols.contains("gti_context_destroy_v1") ||
      !symbols.contains("gti_get_module_descriptor_v1") || symbols.size() <= 3)
    return reject(
        "embedding publication requires the lifecycle and export symbols");
  std::error_code error;
  if (!std::filesystem::is_regular_file(request.runtimeArchive, error) || error)
    return reject("embedding runtime archive is not a regular file");
  if (request.generationDirectory.empty() ||
      request.generationDirectory.filename().empty() ||
      request.generationDirectory.filename() == "." ||
      request.generationDirectory.filename() == "..")
    return reject("embedding generation directory must name a new child");
  const auto parent = std::filesystem::canonical(
      request.generationDirectory.parent_path().empty()
          ? "."
          : request.generationDirectory.parent_path(),
      error);
  if (error)
    return reject("embedding generation parent is unavailable: " +
                  error.message());
  const auto destination = parent / request.generationDirectory.filename();
  const auto existing = std::filesystem::symlink_status(destination, error);
  if (existing.type() != std::filesystem::file_type::not_found)
    return reject("embedding generation already exists; select a new "
                  "generation directory");
  error.clear();
  const auto staging = stagedArtifactPath(destination);
  if (!std::filesystem::create_directory(staging, error) || error)
    return reject("cannot create embedding staging directory: " +
                  error.message());
  TemporaryArtifact directoryCleanup(staging, true);
  const auto object = staging / "module.o";
  const auto header = staging / (request.name + ".h");
  const auto library =
      staging / ("lib" + request.name + (darwin ? ".dylib" : ".so"));
  const auto exports = staging / "exports.txt";
  TemporaryArtifact objectCleanup(object, true), headerCleanup(header, true),
      libraryCleanup(library, true), exportsCleanup(exports, true);
  std::string visibility;
  if (!darwin)
    visibility = "{ global:\n";
  for (const auto &name : symbols)
    visibility += darwin ? "_" + name + "\n" : name + ";\n";
  if (!darwin)
    visibility += "local: *; };\n";
  if (writeArtifact(object, artifacts.object.contents) !=
          ArtifactWriteStatus::Success ||
      writeArtifact(header, artifacts.header.contents) !=
          ArtifactWriteStatus::Success ||
      writeArtifact(exports, visibility) != ArtifactWriteStatus::Success)
    return reject("cannot write staged embedding artifacts");
  NativeInputs inputs;
  inputs.finalLibraryFiles.push_back(
      std::filesystem::absolute(request.runtimeArchive));
  inputs.linkerArguments = {darwin ? "-dynamiclib" : "-shared",
                            "-pthread",
                            "-Xlinker",
                            darwin ? "-exported_symbols_list"
                                   : "--version-script",
                            "-Xlinker",
                            exports.string(),
                            "-Xlinker",
                            darwin ? "-undefined" : "-z",
                            "-Xlinker",
                            darwin ? "error" : "defs"};
  if (darwin) {
    inputs.linkerArguments.insert(inputs.linkerArguments.end(),
                                  {"-Xlinker", "-install_name", "-Xlinker",
                                   "@rpath/" + library.filename().string()});
  }
  const NativeLinkRequest link(request.compiler, object, library, inputs);
  result.command = NativeToolchain().command(link);
  result.process =
      invokeProcess(result.command, {.captureSuccessfulOutput = true,
                                     .description = "embedding shared link"});
  if (!result.process.succeeded())
    return reject("embedding native link failed");
  if (!std::filesystem::remove(object, error) || error)
    return reject("cannot remove staged embedding object");
  if (!std::filesystem::remove(exports, error) || error)
    return reject("cannot remove staged embedding export map");
  const auto published = publishArtifact(staging, destination);
  if (!published.succeeded())
    return reject("embedding generation publication failed: " +
                  published.error.message());
  result.library = destination / library.filename();
  result.header = destination / header.filename();
  return result;
}
} // namespace lang::driver
