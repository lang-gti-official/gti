#include "gti/driver/embedding_library.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"

#include <algorithm>
#include <cerrno>
#include <set>

#if defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace lang::driver {
namespace {

struct DebugBundleCleanup {
  std::filesystem::path path;
  ~DebugBundleCleanup() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
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

ArtifactPublishResult
publishEmbeddingGeneration(const std::filesystem::path &staged,
                           const std::filesystem::path &destination) {
  // The pre-link absence check is diagnostic only. The publication operation
  // itself must refuse a generation created while the native linker ran.
#if defined(__APPLE__)
  if (::renamex_np(staged.c_str(), destination.c_str(), RENAME_EXCL) == 0)
    return {};
#elif defined(__linux__) && defined(SYS_renameat2)
  if (::syscall(SYS_renameat2, AT_FDCWD, staged.c_str(), AT_FDCWD,
                destination.c_str(), RENAME_NOREPLACE) == 0)
    return {};
#else
  return {std::make_error_code(std::errc::operation_not_supported)};
#endif
  return {std::error_code(errno, std::generic_category())};
}
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
  if (request.name.empty() || request.name == "." || request.name == ".." ||
      std::filesystem::path(request.name).filename() != request.name ||
      request.compiler.empty() ||
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
  const auto debugBundle = std::filesystem::path(library.string() + ".dSYM");
  const DebugBundleCleanup debugCleanup{debugBundle};
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
  NativeInputs inputs = request.nativeInputs;
  const auto runtime = std::filesystem::absolute(request.runtimeArchive);
  if (std::find(inputs.finalLibraryFiles.begin(),
                inputs.finalLibraryFiles.end(),
                runtime) == inputs.finalLibraryFiles.end())
    inputs.finalLibraryFiles.push_back(runtime);
  const std::vector<std::string> linkage = {darwin ? "-dynamiclib" : "-shared",
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
  inputs.linkerArguments.insert(inputs.linkerArguments.end(), linkage.begin(),
                                linkage.end());
  if (darwin) {
    inputs.linkerArguments.insert(inputs.linkerArguments.end(),
                                  {"-Xlinker", "-install_name", "-Xlinker",
                                   "@rpath/" + library.filename().string()});
  }
  const NativeLinkRequest link(request.compiler, object, library, inputs,
                               request.debugInfo);
  result.command = NativeToolchain().command(link);
  result.process =
      invokeProcess(result.command, {.captureSuccessfulOutput = true,
                                     .description = "embedding shared link"});
  if (!result.process.succeeded())
    return reject("embedding native link failed");
  if (darwin && request.debugInfo) {
    // Complete the bundle while the link's object is still available. The
    // outer driver publishes this directory with the library and header.
    if (!std::filesystem::is_directory(debugBundle, error)) {
      const auto symbols = invokeProcess(
          {"dsymutil", library.string(), "-o", debugBundle.string()},
          {.captureSuccessfulOutput = true});
      if (!symbols.succeeded())
        return reject("cannot collect embedding debug symbols: " +
                      symbols.output);
    }
    if (writeArtifact(debugBundle / ".gti-debug-symbols",
                      "gti-debug-symbols-v1\n") != ArtifactWriteStatus::Success)
      return reject("cannot mark embedding debug symbols");
  }
  if (!std::filesystem::remove(object, error) || error)
    return reject("cannot remove staged embedding object");
  if (!std::filesystem::remove(exports, error) || error)
    return reject("cannot remove staged embedding export map");
  const auto published = publishEmbeddingGeneration(staging, destination);
  if (!published.succeeded())
    return reject("embedding generation publication failed: " +
                  published.error.message());
  result.library = destination / library.filename();
  result.header = destination / header.filename();
  return result;
}
} // namespace lang::driver
