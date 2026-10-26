#include "gti/driver/build.h"

#include "build_source_identity.h"
#include "gti/build_identity_config.h"
#include "gti/llvm_backend.h"
#include "sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace lang::driver {
namespace {

struct StagedDebugBundleCleanup {
  std::filesystem::path path;
  ~StagedDebugBundleCleanup() {
    // This exact path is inside the fresh driver-owned link staging directory.
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

// Keep the previous GTI-owned companion until its executable commits. Each
// directory move is atomic; failure before executable commit restores the pair.
class DebugBundlePublication final {
public:
  DebugBundlePublication() = default;
  DebugBundlePublication(const DebugBundlePublication &) = delete;
  DebugBundlePublication &operator=(const DebugBundlePublication &) = delete;
  ~DebugBundlePublication() { static_cast<void>(rollback()); }

  static std::optional<std::string>
  validateDestination(const std::filesystem::path &destination) {
    constexpr std::string_view markerName = ".gti-debug-symbols";
    constexpr std::string_view markerContents = "gti-debug-symbols-v1\n";
    std::error_code error;
    const auto status = std::filesystem::symlink_status(destination, error);
    if (error && error != std::errc::no_such_file_or_directory)
      return error.message();
    const bool exists = std::filesystem::exists(status);
    if (exists) {
      if (!std::filesystem::is_directory(status))
        return "refusing to replace a non-directory or symlink debug bundle";
      const auto marker = destination / markerName;
      error.clear();
      if (!std::filesystem::is_regular_file(
              std::filesystem::symlink_status(marker, error)) ||
          error)
        return "refusing to replace a debug bundle not owned by GTI";
      std::ifstream input(marker);
      std::string contents(markerContents.size(), '\0');
      input.read(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
      if (!input || contents != markerContents ||
          input.peek() != std::char_traits<char>::eof())
        return "refusing to replace a debug bundle not owned by GTI";
    }
    return std::nullopt;
  }

  std::optional<std::string> publish(const std::filesystem::path &staged,
                                     const std::filesystem::path &destination) {
    constexpr std::string_view markerName = ".gti-debug-symbols";
    constexpr std::string_view markerContents = "gti-debug-symbols-v1\n";
    std::error_code error;
    if (!std::filesystem::is_directory(
            std::filesystem::symlink_status(staged, error)) ||
        error)
      return "staged debug bundle is not a regular directory";
    if (const auto failure = validateDestination(destination))
      return failure;
    error.clear();
    const bool exists = std::filesystem::exists(destination, error);
    if (error)
      return error.message();
    if (writeArtifact(staged / markerName, markerContents) !=
        ArtifactWriteStatus::Success)
      return "cannot mark staged debug bundle as GTI-owned";
#if !defined(_WIN32)
    struct stat identity{};
    if (::lstat(staged.c_str(), &identity) != 0 || !S_ISDIR(identity.st_mode))
      return "cannot identify staged debug bundle directory";
    device = static_cast<std::uintmax_t>(identity.st_dev);
    inode = static_cast<std::uintmax_t>(identity.st_ino);
#else
    return "debug companion publication requires native directory identity "
           "support";
#endif
    destinationPath = destination;
    stagedPath = staged;
    backupPath = stagedArtifactPath(destination);
    if (exists) {
      error.clear();
      std::filesystem::rename(destination, backupPath, error);
      if (error) {
        finished = true;
        return error.message();
      }
      previous = true;
    }
    error.clear();
    std::filesystem::rename(staged, destination, error);
    if (error) {
      const auto detail = error.message();
      const auto restore = rollback();
      return restore ? detail + "; " + *restore : detail;
    }
    installed = true;
    return std::nullopt;
  }

  std::optional<std::string> rollback() {
    if (finished || destinationPath.empty())
      return std::nullopt;
    finished = true;
    std::error_code error;
    if (installed) {
#if !defined(_WIN32)
      struct stat current{};
      if (::lstat(destinationPath.c_str(), &current) != 0 ||
          !S_ISDIR(current.st_mode) ||
          static_cast<std::uintmax_t>(current.st_dev) != device ||
          static_cast<std::uintmax_t>(current.st_ino) != inode)
        return "debug bundle changed during publication; previous bundle "
               "retained at '" +
               backupPath.string() + "'";
#endif
      std::filesystem::rename(destinationPath, stagedPath, error);
      if (error)
        return "cannot retire failed debug publication; previous bundle "
               "retained at '" +
               backupPath.string() + "': " + error.message();
      installed = false;
    }
    if (previous) {
      error.clear();
      std::filesystem::rename(backupPath, destinationPath, error);
      if (error)
        return "previous debug bundle retained at '" + backupPath.string() +
               "' (restore failed: " + error.message() + ")";
    }
    return std::nullopt;
  }

  std::optional<std::string> commit() {
    finished = true;
    if (!previous)
      return std::nullopt;
    std::error_code error;
    std::filesystem::remove_all(backupPath, error);
    if (error)
      return "published debug pair, but previous GTI-owned bundle remains at "
             "'" +
             backupPath.string() + "': " + error.message();
    return std::nullopt;
  }

private:
  std::filesystem::path destinationPath;
  std::filesystem::path stagedPath;
  std::filesystem::path backupPath;
  std::uintmax_t device = 0;
  std::uintmax_t inode = 0;
  bool previous = false;
  bool installed = false;
  bool finished = false;
};

std::optional<std::string>
debugBundleSourceCollision(const std::filesystem::path &symbols,
                           const SourceManager &sources,
                           const std::filesystem::path &representation) {
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(symbols, error);
  if (error)
    return "cannot inspect debug bundle path: " + error.message();
  const auto generated =
      std::filesystem::weakly_canonical(representation, error);
  if (error)
    return "cannot establish backend representation containment for debug "
           "publication";
  const auto relativeGenerated = generated.lexically_relative(canonical);
  if (!relativeGenerated.empty() && !relativeGenerated.is_absolute() &&
      *relativeGenerated.begin() != "..")
    return "debug bundle conflicts with the requested backend representation";
  for (const auto &name : sources.names()) {
    const auto source = std::filesystem::weakly_canonical(name, error);
    if (error)
      return "cannot establish loaded source containment for debug publication";
    const auto relative = source.lexically_relative(canonical);
    if (!relative.empty() && !relative.is_absolute() &&
        *relative.begin() != "..")
      return "refusing to replace debug bundle containing loaded source '" +
             name + "'";
  }
  return std::nullopt;
}

constexpr std::string_view cacheSchema = "gti-build-cache-v4";
constexpr std::string_view objectCacheSchema = "gti-build-cache-v5";

bool nativeObjectCache(const ExecutableBuildRequest &request) {
  return request.compilation().backend() == BackendKind::Llvm;
}

bool darwinDeploymentArgument(std::string_view argument) {
  const auto changesDeployment = [](std::string_view component) {
    const auto option = component.substr(0, component.find('='));
    return (option.starts_with("-m") && option.ends_with("-version-min")) ||
           option == "-macosx_version_min" || option == "-macos_version_min" ||
           option == "-iphoneos_version_min" ||
           option == "-ios_simulator_version_min" ||
           option == "-tvos_version_min" ||
           option == "-tvos_simulator_version_min" ||
           option == "-watchos_version_min" ||
           option == "-watchos_simulator_version_min" ||
           option == "-mtargetos" || option == "-platform_version" ||
           option == "-sdk_version" || option == "-target-sdk-version" ||
           option == "-darwin-target-variant" ||
           option == "-darwin-target-variant-triple" ||
           option == "-darwin-target-variant-sdk-version";
  };
  if (argument.starts_with("-Xlinker="))
    return changesDeployment(
        argument.substr(std::string_view("-Xlinker=").size()));
  if (!argument.starts_with("-Wl,"))
    return changesDeployment(argument);
  argument.remove_prefix(std::string_view("-Wl,").size());
  while (true) {
    const auto separator = argument.find(',');
    if (changesDeployment(argument.substr(0, separator)))
      return true;
    if (separator == std::string_view::npos)
      return false;
    argument.remove_prefix(separator + 1);
  }
}

std::string_view buildCacheSchema(const ExecutableBuildRequest &request) {
  return nativeObjectCache(request) ? objectCacheSchema : cacheSchema;
}

std::string_view cacheRepresentationFile(bool object) {
  return object ? "artifact.o" : "generated.cpp";
}
using detail::IdentityBuilder;

struct FileDigest {
  std::string hash;
  std::uintmax_t size = 0;

  bool operator==(const FileDigest &) const = default;
};

std::optional<FileDigest> digestFile(const std::filesystem::path &path,
                                     std::string &errorMessage) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    errorMessage = "cannot inspect '" + path.string() + "': " + error.message();
    return std::nullopt;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    errorMessage = "cannot read '" + path.string() + "'";
    return std::nullopt;
  }
  Sha256 hash;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      hash.update(
          std::string_view(buffer.data(), static_cast<std::size_t>(count)));
    }
  }
  if (!input.eof()) {
    errorMessage = "cannot finish reading '" + path.string() + "'";
    return std::nullopt;
  }
  return FileDigest{.hash = hash.finishHex(), .size = size};
}

std::string digestText(std::string_view text) {
  Sha256 hash;
  hash.update(text);
  return hash.finishHex();
}

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
  const std::filesystem::file_status trustedRootStatus =
      std::filesystem::symlink_status(trustedRoot, error);
  if (error || !std::filesystem::exists(trustedRootStatus)) {
    return "gti: refusing unavailable managed output trust root '" +
           policy.trustedRoot.string() + "'";
  }
  if (std::filesystem::is_symlink(trustedRootStatus)) {
    return "gti: refusing symbolic-link managed output trust root '" +
           policy.trustedRoot.string() + "'";
  }
  if (!std::filesystem::is_directory(trustedRootStatus)) {
    return "gti: managed output trust root is not a directory: '" +
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

  error.clear();
  const std::filesystem::file_status artifactStatus =
      std::filesystem::symlink_status(absoluteArtifact, error);
  const bool artifactMissing =
      error == std::errc::no_such_file_or_directory ||
      (!error && !std::filesystem::exists(artifactStatus));
  if (artifactMissing) {
    error.clear();
  } else if (error) {
    return "gti: failed to inspect managed output artifact '" +
           absoluteArtifact.string() + "': " + error.message();
  } else if (std::filesystem::is_symlink(artifactStatus)) {
    return "gti: refusing symbolic-link managed output artifact '" +
           absoluteArtifact.string() + "'";
  }
  return std::nullopt;
}

std::filesystem::path normalizedAbsolute(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) {
    absolute = path;
    error.clear();
  }
  std::filesystem::path canonical =
      std::filesystem::weakly_canonical(absolute, error);
  return error ? absolute.lexically_normal() : canonical;
}

std::optional<BuildFileProvenance>
captureFileProvenance(const std::filesystem::path &path,
                      std::string &errorMessage) {
  const std::filesystem::path canonical = normalizedAbsolute(path);
  const std::optional<FileDigest> digest = digestFile(canonical, errorMessage);
  if (!digest) {
    return std::nullopt;
  }
  return BuildFileProvenance{.canonicalPath = canonical,
                             .bytes = digest->size,
                             .sha256 = digest->hash};
}

bool addFileIdentity(IdentityBuilder &identity, std::string_view label,
                     const std::filesystem::path &path, bool includePath,
                     std::string &errorMessage) {
  const std::optional<FileDigest> digest = digestFile(path, errorMessage);
  if (!digest) {
    return false;
  }
  identity.add(std::string(label) + ".begin", "file");
  if (includePath) {
    identity.add(std::string(label) + ".path",
                 normalizedAbsolute(path).generic_string());
  }
  identity.add(std::string(label) + ".size",
               static_cast<std::uint64_t>(digest->size));
  identity.add(std::string(label) + ".sha256", digest->hash);
  return true;
}

struct DirectoryIdentityEntry {
  std::string relativePath;
  std::string kind;
  std::filesystem::path path;
  std::string symlinkTarget;
};

bool addDirectoryIdentity(IdentityBuilder &identity, std::string_view label,
                          const std::filesystem::path &directory,
                          bool includePath, std::string &errorMessage) {
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error) {
    errorMessage = "cannot inspect directory '" + directory.string() +
                   "': " + (error ? error.message() : "not a directory");
    return false;
  }

  std::vector<DirectoryIdentityEntry> entries;
  std::filesystem::recursive_directory_iterator iterator(directory, error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    errorMessage = "cannot enumerate directory '" + directory.string() +
                   "': " + error.message();
    return false;
  }
  while (iterator != end) {
    const std::filesystem::directory_entry entry = *iterator;
    const std::filesystem::path relative =
        entry.path().lexically_relative(directory);
    error.clear();
    const std::filesystem::file_status status = entry.symlink_status(error);
    if (error) {
      errorMessage =
          "cannot inspect '" + entry.path().string() + "': " + error.message();
      return false;
    }

    DirectoryIdentityEntry record{.relativePath = relative.generic_string(),
                                  .path = entry.path()};
    if (std::filesystem::is_symlink(status)) {
      record.kind = "symlink";
      error.clear();
      record.symlinkTarget =
          std::filesystem::read_symlink(entry.path(), error).generic_string();
      if (error) {
        errorMessage = "cannot inspect symbolic link '" +
                       entry.path().string() + "': " + error.message();
        return false;
      }
      error.clear();
      if (std::filesystem::is_directory(entry.path(), error) && !error) {
        errorMessage = "directory '" + directory.string() +
                       "' contains a symbolic link to a directory ('" +
                       entry.path().string() +
                       "'), which cannot be tracked safely";
        return false;
      }
    } else if (std::filesystem::is_regular_file(status)) {
      record.kind = "file";
    } else if (std::filesystem::is_directory(status)) {
      record.kind = "directory";
    } else {
      record.kind = "other";
    }
    entries.push_back(std::move(record));
    iterator.increment(error);
    if (error) {
      errorMessage = "cannot enumerate directory '" + directory.string() +
                     "': " + error.message();
      return false;
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const DirectoryIdentityEntry &left,
               const DirectoryIdentityEntry &right) {
              return left.relativePath < right.relativePath;
            });
  identity.add(std::string(label) + ".begin", "directory");
  if (includePath) {
    identity.add(std::string(label) + ".path",
                 normalizedAbsolute(directory).generic_string());
  }
  identity.add(std::string(label) + ".entries",
               static_cast<std::uint64_t>(entries.size()));
  for (const DirectoryIdentityEntry &entry : entries) {
    identity.add(std::string(label) + ".entry.path", entry.relativePath);
    identity.add(std::string(label) + ".entry.kind", entry.kind);
    if (entry.kind == "symlink") {
      identity.add(std::string(label) + ".entry.target", entry.symlinkTarget);
      error.clear();
      if (std::filesystem::is_regular_file(entry.path, error) && !error &&
          !addFileIdentity(identity, std::string(label) + ".entry.contents",
                           entry.path, false, errorMessage)) {
        return false;
      }
    } else if (entry.kind == "file" &&
               !addFileIdentity(identity,
                                std::string(label) + ".entry.contents",
                                entry.path, false, errorMessage)) {
      return false;
    }
  }
  return true;
}

std::optional<std::filesystem::path>
resolveExecutableForIdentity(std::string_view command) {
  const std::filesystem::path requested(command);
  const auto usable = [](const std::filesystem::path &candidate) {
    std::error_code error;
    return std::filesystem::is_regular_file(candidate, error) && !error;
  };
  if (requested.is_absolute() || requested.has_parent_path()) {
    return usable(requested) ? std::optional<std::filesystem::path>(
                                   normalizedAbsolute(requested))
                             : std::nullopt;
  }

  const char *configuredPath = std::getenv("PATH");
  if (configuredPath == nullptr) {
    return std::nullopt;
  }
#if defined(_WIN32)
  constexpr char separator = ';';
#else
  constexpr char separator = ':';
#endif
  std::string_view searchPath(configuredPath);
  while (true) {
    const std::size_t next = searchPath.find(separator);
    const std::string_view component = searchPath.substr(0, next);
    const std::filesystem::path directory =
        component.empty() ? std::filesystem::path(".")
                          : std::filesystem::path(component);
    std::filesystem::path candidate = directory / requested;
    if (usable(candidate)) {
      return normalizedAbsolute(candidate);
    }
#if defined(_WIN32)
    if (candidate.extension().empty()) {
      candidate += ".exe";
      if (usable(candidate)) {
        return normalizedAbsolute(candidate);
      }
    }
#endif
    if (next == std::string_view::npos) {
      break;
    }
    searchPath.remove_prefix(next + 1);
  }
  return std::nullopt;
}

// Adds a compiler's command, executable content and version report to the
// identity. `mirror`, when given, receives the same facts, so a second
// identity (the native discovery memo context) can share one version probe.
bool addCompilerIdentity(IdentityBuilder &identity, std::string_view label,
                         const std::string &compiler, std::string &errorMessage,
                         IdentityBuilder *mirror = nullptr) {
  const std::optional<std::filesystem::path> executable =
      resolveExecutableForIdentity(compiler);
  if (!executable) {
    errorMessage = "cannot resolve compiler executable '" + compiler + "'";
    return false;
  }
  const std::optional<FileDigest> digest =
      digestFile(*executable, errorMessage);
  if (!digest) {
    return false;
  }
  const ProcessResult version =
      invokeProcess({compiler, "--version"},
                    {.outputMode = ProcessOutputMode::Capture,
                     .captureSuccessfulOutput = true,
                     .description = std::string(label) + " identity probe"});
  if (!version.succeeded()) {
    errorMessage = version.driverDiagnostic.value_or(
        "compiler identity probe failed for '" + compiler + "'");
    return false;
  }
  for (IdentityBuilder *builder : {&identity, mirror}) {
    if (builder == nullptr) {
      continue;
    }
    builder->add(std::string(label) + ".command", compiler);
    builder->add(std::string(label) + ".executable.begin", "file");
    builder->add(std::string(label) + ".executable.path",
                 normalizedAbsolute(*executable).generic_string());
    builder->add(std::string(label) + ".executable.size",
                 static_cast<std::uint64_t>(digest->size));
    builder->add(std::string(label) + ".executable.sha256", digest->hash);
    builder->add(std::string(label) + ".version", version.output);
  }
  return true;
}

std::optional<std::string>
logicalSourceName(const SourceUnit &unit,
                  const std::filesystem::path &sourceRoot) {
  if (unit.standardLibraryName) {
    return "stdlib:" + *unit.standardLibraryName;
  }
  if (unit.prelude || unit.role == SourceUnitRole::Prelude) {
    return "prelude";
  }
  if (unit.packageIdentity && unit.packageRelativePath) {
    return "package:" + *unit.packageIdentity + ":" + *unit.packageRelativePath;
  }

  const std::filesystem::path root = normalizedAbsolute(sourceRoot);
  const std::filesystem::path source = normalizedAbsolute(unit.path);
  if (pathIsWithin(root, source)) {
    const std::filesystem::path relative = source.lexically_relative(root);
    if (!relative.empty() && !relative.is_absolute()) {
      return "package:" + relative.generic_string();
    }
  }
  // An application include outside the package is an explicit path-semantic
  // input. Keeping its canonical path avoids unsafe reuse after it moves.
  return "external:" + source.generic_string();
}

} // namespace

std::optional<std::string>
managedExecutableDiagnostic(const ManagedOutputPolicy &policy,
                            const std::filesystem::path &executable) {
  if (const std::optional<std::string> diagnostic =
          managedPathDiagnostic(policy, executable, false)) {
    return diagnostic;
  }

  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(executable, error);
  const bool missing = error == std::errc::no_such_file_or_directory ||
                       (!error && !std::filesystem::exists(status));
  if (missing) {
    return "gti: managed executable does not exist: '" + executable.string() +
           "'";
  }
  if (error) {
    return "gti: failed to inspect managed executable '" + executable.string() +
           "': " + error.message();
  }
  if (!std::filesystem::is_regular_file(status)) {
    return "gti: managed executable is not a regular file: '" +
           executable.string() + "'";
  }
#if !defined(_WIN32)
  constexpr std::filesystem::perms executableBits =
      std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
      std::filesystem::perms::others_exec;
  if ((status.permissions() & executableBits) == std::filesystem::perms::none) {
    return "gti: managed executable is not executable: '" +
           executable.string() + "'";
  }
#endif
  return std::nullopt;
}

namespace detail {

std::optional<BuildSourceMaterialProjection>
projectBuildSourceMaterials(const CompilationInputs &inputs,
                            const std::filesystem::path &sourceRoot,
                            std::string &errorMessage) {
  BuildSourceMaterialProjection projection;
  projection.units.reserve(inputs.sourceGraph.sourceUnits().size());
  projection.logicalNamesById.resize(inputs.sourceGraph.sourceUnits().size() +
                                     1);
  for (const SourceUnit &unit : inputs.sourceGraph.sourceUnits()) {
    std::optional<std::string> name = logicalSourceName(unit, sourceRoot);
    if (name && *name == "prelude") {
      const auto &roots = inputs.sourceGraph.preludeRoots();
      const auto position = std::find(roots.begin(), roots.end(), unit.id);
      if (position != roots.end() && position != roots.begin()) {
        *name += ":" + std::to_string(std::distance(roots.begin(), position));
      }
    }
    if (!name) {
      errorMessage = "cannot derive a logical source name for '" +
                     unit.path.string() + "'";
      return std::nullopt;
    }
    if (unit.id >= projection.logicalNamesById.size()) {
      errorMessage = "source graph contains an invalid unit identifier";
      return std::nullopt;
    }
    const std::string *source = inputs.sources.find(unit.path.string());
    if (source == nullptr) {
      errorMessage =
          "loaded source text is missing for '" + unit.path.string() + "'";
      return std::nullopt;
    }
    projection.logicalNamesById[unit.id] = *name;
    projection.units.push_back({.id = unit.id,
                                .role = unit.role,
                                .logicalIdentity = *name,
                                .entry = unit.entry,
                                .prelude = unit.prelude,
                                .bytes = source->size(),
                                .sha256 = digestText(*source)});
  }
  std::sort(projection.units.begin(), projection.units.end(),
            [](const BuildSourceMaterialRecord &left,
               const BuildSourceMaterialRecord &right) {
              return left.logicalIdentity < right.logicalIdentity;
            });
  for (std::size_t index = 1; index < projection.units.size(); ++index) {
    if (projection.units[index - 1].logicalIdentity ==
        projection.units[index].logicalIdentity) {
      errorMessage = "source graph contains duplicate logical unit '" +
                     projection.units[index].logicalIdentity + "'";
      return std::nullopt;
    }
  }

  const std::vector<SourceUnitId> &preludeRoots =
      inputs.sourceGraph.preludeRoots();
  projection.preludeRoots.reserve(preludeRoots.size());
  for (const SourceUnitId root : preludeRoots) {
    if (root == 0 || root >= projection.logicalNamesById.size() ||
        projection.logicalNamesById[root].empty()) {
      errorMessage = "source graph contains an invalid prelude root";
      return std::nullopt;
    }
    const SourceUnit *unit = inputs.sourceGraph.findUnit(root);
    if (unit == nullptr ||
        (unit->role != SourceUnitRole::Prelude && !unit->prelude)) {
      errorMessage = "source graph prelude root does not name a prelude unit";
      return std::nullopt;
    }
    projection.preludeRoots.push_back(projection.logicalNamesById[root]);
  }

  projection.dependencyEdges.reserve(
      inputs.sourceGraph.dependencyEdges().size());
  for (const SourceDependency &dependency :
       inputs.sourceGraph.dependencyEdges()) {
    if (dependency.source >= projection.logicalNamesById.size() ||
        dependency.target >= projection.logicalNamesById.size() ||
        projection.logicalNamesById[dependency.source].empty() ||
        projection.logicalNamesById[dependency.target].empty()) {
      errorMessage = "source graph contains an invalid dependency edge";
      return std::nullopt;
    }
    projection.dependencyEdges.push_back(
        projection.logicalNamesById[dependency.source] + "\n" +
        projection.logicalNamesById[dependency.target] + "\n" +
        std::to_string(static_cast<unsigned>(dependency.kind)));
  }
  std::sort(projection.dependencyEdges.begin(),
            projection.dependencyEdges.end());
  return projection;
}

void appendBuildSourceMaterialsIdentity(
    IdentityBuilder &identity,
    const BuildSourceMaterialProjection &projection) {
  identity.add("source.units",
               static_cast<std::uint64_t>(projection.units.size()));
  for (const BuildSourceMaterialRecord &record : projection.units) {
    identity.add("source.name", record.logicalIdentity);
    identity.add("source.role", static_cast<std::uint64_t>(record.role));
    identity.add("source.entry", static_cast<std::uint64_t>(record.entry));
    identity.add("source.prelude", static_cast<std::uint64_t>(record.prelude));
    identity.add("source.size", static_cast<std::uint64_t>(record.bytes));
    identity.add("source.sha256", record.sha256);
  }
  identity.add("source.prelude-roots",
               static_cast<std::uint64_t>(projection.preludeRoots.size()));
  for (const std::string &root : projection.preludeRoots) {
    identity.add("source.prelude-root", root);
  }
  identity.add("source.edges",
               static_cast<std::uint64_t>(projection.dependencyEdges.size()));
  for (const std::string &edge : projection.dependencyEdges) {
    identity.add("source.edge", edge);
  }
}

std::string
buildSourceMaterialsSha256(const BuildSourceMaterialProjection &projection) {
  IdentityBuilder identity;
  appendBuildSourceMaterialsIdentity(identity, projection);
  return identity.finish();
}

} // namespace detail

namespace {

bool addSourceIdentity(IdentityBuilder &identity,
                       const CompilationInputs &inputs,
                       const std::filesystem::path &sourceRoot,
                       std::string &errorMessage) {
  const std::optional<detail::BuildSourceMaterialProjection> projection =
      detail::projectBuildSourceMaterials(inputs, sourceRoot, errorMessage);
  if (!projection) {
    return false;
  }
  detail::appendBuildSourceMaterialsIdentity(identity, *projection);
  return true;
}

std::optional<BuildSourceProvenance>
captureBuildSourceProvenance(const CompilationRequest &request,
                             const CompilationInputs &inputs,
                             std::string &errorMessage) {
  BuildSourceProvenance provenance;
  provenance.entryRoot = inputs.sourceGraph.entryUnit();
  provenance.preludeRoots = inputs.sourceGraph.preludeRoots();
  provenance.compilationOrder = inputs.sourceGraph.compilationOrder();

  const std::filesystem::path logicalRoot = request.entry().parent_path();
  const std::optional<detail::BuildSourceMaterialProjection> projection =
      detail::projectBuildSourceMaterials(inputs, logicalRoot, errorMessage);
  if (!projection) {
    return std::nullopt;
  }
  std::vector<const detail::BuildSourceMaterialRecord *> projectedById(
      projection->logicalNamesById.size());
  for (const detail::BuildSourceMaterialRecord &record : projection->units) {
    projectedById[record.id] = &record;
  }
  for (const SourceUnit &unit : inputs.sourceGraph.sourceUnits()) {
    if (unit.id >= projectedById.size() || projectedById[unit.id] == nullptr) {
      errorMessage = "source material projection is missing a loaded unit";
      return std::nullopt;
    }
    const detail::BuildSourceMaterialRecord &projected =
        *projectedById[unit.id];
    BuildSourceUnitProvenance record{
        .id = unit.id,
        .role = unit.role,
        .material = {.canonicalPath = normalizedAbsolute(unit.path),
                     .bytes = projected.bytes,
                     .sha256 = projected.sha256},
        .logicalIdentity = projected.logicalIdentity,
        .standardLibraryName = unit.standardLibraryName,
        .packageIdentity = unit.packageIdentity,
        .packageRelativePath = unit.packageRelativePath,
        .entry = unit.entry,
        .prelude = unit.prelude,
    };
    provenance.units.push_back(std::move(record));
  }

  for (const SourceDependency &dependency :
       inputs.sourceGraph.dependencyEdges()) {
    if (inputs.sourceGraph.findUnit(dependency.source) == nullptr ||
        inputs.sourceGraph.findUnit(dependency.target) == nullptr) {
      errorMessage = "source graph contains an invalid dependency edge";
      return std::nullopt;
    }
    provenance.dependencies.push_back(
        {.source = dependency.source,
         .target = dependency.target,
         .kind = dependency.kind,
         .includeSpelling = dependency.includeSpelling,
         .includeOccurrence = dependency.includeOccurrence});
  }
  provenance.materialsSha256 = detail::buildSourceMaterialsSha256(*projection);
  return provenance;
}

template <typename Value>
void addScalarSequence(IdentityBuilder &identity, std::string_view label,
                       const std::vector<Value> &values) {
  identity.add(std::string(label) + ".count",
               static_cast<std::uint64_t>(values.size()));
  for (const Value &value : values) {
    identity.add(label, std::string_view(value));
  }
}

bool addPathFileSequence(IdentityBuilder &identity, std::string_view label,
                         const std::vector<std::filesystem::path> &paths,
                         bool includePath, std::string &errorMessage) {
  identity.add(std::string(label) + ".count",
               static_cast<std::uint64_t>(paths.size()));
  for (const std::filesystem::path &path : paths) {
    if (!addFileIdentity(identity, label, path, includePath, errorMessage)) {
      return false;
    }
  }
  return true;
}

bool addPathDirectorySequence(IdentityBuilder &identity, std::string_view label,
                              const std::vector<std::filesystem::path> &paths,
                              bool includePath, std::string &errorMessage) {
  identity.add(std::string(label) + ".count",
               static_cast<std::uint64_t>(paths.size()));
  for (const std::filesystem::path &path : paths) {
    if (!addDirectoryIdentity(identity, label, path, includePath,
                              errorMessage)) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> readTextFile(const std::filesystem::path &path,
                                        std::string &errorMessage);
bool publishTextAtomically(const std::filesystem::path &destination,
                           std::string_view contents,
                           std::string &errorMessage);

std::vector<std::filesystem::path>
nativeCppIncludeDirectories(const ToolchainLayout &toolchain,
                            const NativeInputs &additional);

struct ResolvedNativeLibrary {
  std::string name;
  std::filesystem::path file;
};

struct ResolvedNativeFramework {
  std::string name;
  std::filesystem::path file;
};

// The concrete files a native link configuration resolved to, established
// before any cache key exists. `nativeCacheBypassDetail` fills this in while
// deciding whether every link input has an exact identity; `buildCacheKey`
// then records the resolution so a `-l` name that would answer with a
// different file cannot reuse an entry.
struct NativeLinkIdentityPlan {
  std::vector<ResolvedNativeLibrary> libraries;
  std::vector<ResolvedNativeFramework> frameworks;
};

constexpr std::string_view nativeDiscoverySchema = "gti-native-discovery-v1";
constexpr std::string_view nativeDiscoveryDirectory = "native-discovery";

// The stat facts under which a memoized discovery may trust a recorded
// content digest without rereading the file. Any difference rehashes the
// file; equal content then repairs the stamp.
struct FileStamp {
  std::uint64_t size = 0;
  std::uint64_t modifiedNs = 0;
  std::uint64_t inode = 0;

  bool operator==(const FileStamp &) const = default;
};

std::optional<FileStamp> stampFile(const std::filesystem::path &path) {
#if defined(_WIN32)
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    return std::nullopt;
  }
  const std::filesystem::file_time_type written =
      std::filesystem::last_write_time(path, error);
  if (error) {
    return std::nullopt;
  }
  return FileStamp{.size = static_cast<std::uint64_t>(size),
                   .modifiedNs = static_cast<std::uint64_t>(
                       written.time_since_epoch().count()),
                   .inode = 0};
#else
  struct stat info{};
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return std::nullopt;
  }
#if defined(__APPLE__)
  const timespec &modified = info.st_mtimespec;
#else
  const timespec &modified = info.st_mtim;
#endif
  return FileStamp{.size = static_cast<std::uint64_t>(info.st_size),
                   .modifiedNs = static_cast<std::uint64_t>(modified.tv_sec) *
                                     1000000000ULL +
                                 static_cast<std::uint64_t>(modified.tv_nsec),
                   .inode = static_cast<std::uint64_t>(info.st_ino)};
#endif
}

std::uint64_t wallClockNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// The entry names of a directory, which decide whether a file could now
// shadow an include the compiler resolved elsewhere. A directory that cannot
// be listed has the fixed identity "absent".
std::string directoryNamesDigest(const std::filesystem::path &directory) {
  std::error_code error;
  std::vector<std::string> names;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      break;
    }
    names.push_back(iterator->path().filename().string());
  }
  if (error) {
    return "absent";
  }
  std::sort(names.begin(), names.end());
  IdentityBuilder identity;
  identity.add("names", static_cast<std::uint64_t>(names.size()));
  for (const std::string &name : names) {
    identity.add("name", name);
  }
  return identity.finish();
}

struct DiscoveredDependency {
  std::string path;
  FileDigest digest;
  FileStamp stamp;
};

struct WatchedDirectory {
  std::string path;
  std::string namesDigest;
};

// One probe's outcome: the preprocessed unit's digest, every file the
// preprocessor opened (sorted by path) and the directories whose entry names
// decided include resolution. Persisted beneath the cache root so an unchanged
// source skips the preprocessor on later lookups.
struct NativeDiscoveryRecord {
  std::uint64_t writtenNs = 0;
  FileDigest preprocessed;
  std::vector<DiscoveredDependency> dependencies;
  std::vector<WatchedDirectory> directories;
};

std::string renderNativeDiscoveryRecord(const NativeDiscoveryRecord &record) {
  std::string text = std::string(nativeDiscoverySchema) + "\n" + "written " +
                     std::to_string(record.writtenNs) + "\n" + "preprocessed " +
                     record.preprocessed.hash + " " +
                     std::to_string(record.preprocessed.size) + "\n";
  for (const WatchedDirectory &directory : record.directories) {
    text += "directory " + directory.namesDigest + " " + directory.path + "\n";
  }
  for (const DiscoveredDependency &dependency : record.dependencies) {
    text += "dependency " + dependency.digest.hash + " " +
            std::to_string(dependency.digest.size) + " " +
            std::to_string(dependency.stamp.modifiedNs) + " " +
            std::to_string(dependency.stamp.inode) + " " + dependency.path +
            "\n";
  }
  return text;
}

// Parses a record strictly; anything unexpected is an absent record.
std::optional<NativeDiscoveryRecord>
readNativeDiscoveryRecord(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return std::nullopt;
  }
  std::string ignored;
  const std::optional<std::string> text = readTextFile(path, ignored);
  if (!text) {
    return std::nullopt;
  }
  std::istringstream input(*text);
  std::string line;
  if (!std::getline(input, line) || line != nativeDiscoverySchema) {
    return std::nullopt;
  }
  const auto validDigest = [](std::string_view hash) {
    return hash.size() == 64 &&
           std::all_of(hash.begin(), hash.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
  };
  NativeDiscoveryRecord record;
  bool sawWritten = false;
  bool sawPreprocessed = false;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string kind;
    fields >> kind;
    if (kind == "written") {
      if (sawWritten || !(fields >> record.writtenNs)) {
        return std::nullopt;
      }
      sawWritten = true;
    } else if (kind == "preprocessed") {
      if (sawPreprocessed ||
          !(fields >> record.preprocessed.hash >> record.preprocessed.size) ||
          !validDigest(record.preprocessed.hash)) {
        return std::nullopt;
      }
      sawPreprocessed = true;
    } else if (kind == "directory") {
      WatchedDirectory directory;
      if (!(fields >> directory.namesDigest) || fields.get() != ' ' ||
          !std::getline(fields, directory.path) || directory.path.empty()) {
        return std::nullopt;
      }
      record.directories.push_back(std::move(directory));
    } else if (kind == "dependency") {
      DiscoveredDependency dependency;
      if (!(fields >> dependency.digest.hash >> dependency.digest.size >>
            dependency.stamp.modifiedNs >> dependency.stamp.inode) ||
          !validDigest(dependency.digest.hash) || fields.get() != ' ' ||
          !std::getline(fields, dependency.path) || dependency.path.empty()) {
        return std::nullopt;
      }
      dependency.stamp.size = dependency.digest.size;
      record.dependencies.push_back(std::move(dependency));
    } else {
      return std::nullopt;
    }
  }
  if (!sawWritten || !sawPreprocessed) {
    return std::nullopt;
  }
  return record;
}

// Checks a memoized discovery against the file system: every watched
// directory must list the same names and every dependency must have the same
// content. A stamp equal to the recorded one, settled well before the record
// was written, vouches for the recorded digest; anything else is rehashed,
// and equal content repairs the stamp (`repaired`). Returns false when the
// record no longer describes the inputs.
bool nativeDiscoveryRecordHolds(NativeDiscoveryRecord &record, bool &repaired) {
  repaired = false;
  for (const WatchedDirectory &directory : record.directories) {
    if (directoryNamesDigest(directory.path) != directory.namesDigest) {
      return false;
    }
  }
  constexpr std::uint64_t stampSettleNs = 2000000000ULL;
  for (DiscoveredDependency &dependency : record.dependencies) {
    const std::optional<FileStamp> stamp = stampFile(dependency.path);
    if (!stamp) {
      return false;
    }
    if (*stamp == dependency.stamp &&
        stamp->modifiedNs + stampSettleNs < record.writtenNs) {
      continue;
    }
    std::string ignored;
    const std::optional<FileDigest> digest =
        digestFile(dependency.path, ignored);
    if (!digest || *digest != dependency.digest) {
      return false;
    }
    dependency.stamp = *stamp;
    repaired = true;
  }
  return true;
}

// Folds a discovery outcome into the cache identity. Fresh probes and valid
// memoized records go through this one function, so a memo hit contributes
// exactly the bytes the probe would have.
void addNativeDiscoveryIdentity(IdentityBuilder &identity,
                                std::string_view label,
                                const std::filesystem::path &source,
                                const NativeDiscoveryRecord &record) {
  identity.add(std::string(label) + ".source",
               normalizedAbsolute(source).generic_string());
  identity.add(std::string(label) + ".deps",
               static_cast<std::uint64_t>(record.dependencies.size()));
  for (const DiscoveredDependency &dependency : record.dependencies) {
    identity.add(std::string(label) + ".dep.path", dependency.path);
    identity.add(std::string(label) + ".dep.size",
                 static_cast<std::uint64_t>(dependency.digest.size));
    identity.add(std::string(label) + ".dep.sha256", dependency.digest.hash);
  }
  identity.add(std::string(label) + ".preprocessed.size",
               static_cast<std::uint64_t>(record.preprocessed.size));
  identity.add(std::string(label) + ".preprocessed.sha256",
               record.preprocessed.hash);
}

// Best effort: a memo that cannot be written only costs the next lookup a
// probe.
void writeNativeDiscoveryRecord(const std::filesystem::path &memo,
                                NativeDiscoveryRecord &record) {
  record.writtenNs = wallClockNs();
  std::error_code error;
  std::filesystem::create_directories(memo.parent_path(), error);
  if (error) {
    return;
  }
  std::string ignored;
  publishTextAtomically(memo, renderNativeDiscoveryRecord(record), ignored);
}

// Runs one native preprocessor probe and folds the compiler's own dependency
// report into the cache identity: every file the preprocessor opened joins
// with content identity, and the preprocessed translation unit itself is
// hashed so include resolution changes (for example a newly added shadowing
// header) always change the key. `memo` names this probe's persisted record;
// a record whose watched directories and dependency contents still hold
// answers without running the preprocessor. Policy violations set
// `policyBypassDetail`; mechanical failures set `errorMessage`.
bool addNativeSourceDiscovery(
    IdentityBuilder &identity, std::string_view label,
    const std::vector<std::string> &probeCommand,
    const std::filesystem::path &source,
    const std::filesystem::path &preprocessed,
    const std::filesystem::path &depfile,
    const std::vector<std::filesystem::path> &includeDirectories,
    const std::optional<std::filesystem::path> &memo, std::string &errorMessage,
    std::string &policyBypassDetail) {
  if (memo) {
    if (std::optional<NativeDiscoveryRecord> record =
            readNativeDiscoveryRecord(*memo)) {
      bool repaired = false;
      if (nativeDiscoveryRecordHolds(*record, repaired)) {
        addNativeDiscoveryIdentity(identity, label, source, *record);
        if (repaired) {
          writeNativeDiscoveryRecord(*memo, *record);
        }
        return true;
      }
    }
  }

  const ProcessResult probe = invokeProcess(
      probeCommand, {.outputMode = ProcessOutputMode::Capture,
                     .captureSuccessfulOutput = false,
                     .description = "native dependency discovery"});
  if (!probe.succeeded()) {
    errorMessage = "native dependency discovery failed for '" +
                   source.string() + "' with exit code " +
                   std::to_string(probe.exitCode);
    return false;
  }
  const std::optional<std::string> report = readTextFile(depfile, errorMessage);
  if (!report) {
    return false;
  }
  const std::optional<std::vector<std::filesystem::path>> dependencies =
      parseNativeDependencyFile(*report, errorMessage);
  if (!dependencies) {
    errorMessage = "cannot interpret the native dependency report for '" +
                   source.string() + "': " + errorMessage;
    return false;
  }

  std::vector<std::string> discovered;
  discovered.reserve(dependencies->size() + 1);
  discovered.push_back(normalizedAbsolute(source).generic_string());
  for (const std::filesystem::path &dependency : *dependencies) {
    discovered.push_back(normalizedAbsolute(dependency).generic_string());
  }
  std::sort(discovered.begin(), discovered.end());
  discovered.erase(std::unique(discovered.begin(), discovered.end()),
                   discovered.end());

  NativeDiscoveryRecord record;
  std::vector<std::string> watched;
  for (const std::filesystem::path &directory : includeDirectories) {
    watched.push_back(normalizedAbsolute(directory).generic_string());
  }
  for (const std::string &dependency : discovered) {
    // Stamp before reading so a write racing the probe is rehashed later.
    const std::optional<FileStamp> stamp = stampFile(dependency);
    const std::optional<std::string> contents =
        readTextFile(dependency, errorMessage);
    if (!contents) {
      return false;
    }
    if (containsTimeSensitivePreprocessorUse(*contents)) {
      policyBypassDetail = "time-and-date preprocessor macros in '" +
                           dependency + "' prevent deterministic native reuse";
      return false;
    }
    record.dependencies.push_back(
        DiscoveredDependency{.path = dependency,
                             .digest = FileDigest{.hash = digestText(*contents),
                                                  .size = contents->size()},
                             .stamp = stamp.value_or(FileStamp{})});
    watched.push_back(
        std::filesystem::path(dependency).parent_path().generic_string());
  }

  const std::optional<FileDigest> preprocessedDigest =
      digestFile(preprocessed, errorMessage);
  if (!preprocessedDigest) {
    return false;
  }
  record.preprocessed = *preprocessedDigest;
  addNativeDiscoveryIdentity(identity, label, source, record);

  if (memo) {
    std::sort(watched.begin(), watched.end());
    watched.erase(std::unique(watched.begin(), watched.end()), watched.end());
    for (const std::string &directory : watched) {
      record.directories.push_back(WatchedDirectory{
          .path = directory, .namesDigest = directoryNamesDigest(directory)});
    }
    writeNativeDiscoveryRecord(*memo, record);
  }
  return true;
}

// The scalar native-toolchain environment that affects compiler policy
// without injecting a mutable search root.
void addNativeEnvironmentIdentity(IdentityBuilder &identity) {
  constexpr std::array<std::string_view, 19> nativeEnvironment{
      "PATH",
      "CPATH",
      "C_INCLUDE_PATH",
      "CPLUS_INCLUDE_PATH",
      "OBJC_INCLUDE_PATH",
      "LIBRARY_PATH",
      "COMPILER_PATH",
      "GCC_EXEC_PREFIX",
      "SDKROOT",
      "DEVELOPER_DIR",
      "MACOSX_DEPLOYMENT_TARGET",
      "IPHONEOS_DEPLOYMENT_TARGET",
      "SOURCE_DATE_EPOCH",
      "LD_LIBRARY_PATH",
      "DYLD_LIBRARY_PATH",
      "DYLD_FRAMEWORK_PATH",
      "DYLD_FALLBACK_FRAMEWORK_PATH",
      "INCLUDE",
      "LIB",
  };
  for (const std::string_view name : nativeEnvironment) {
    const std::string ownedName(name);
    const char *value = std::getenv(ownedName.c_str());
    identity.add("native.environment.name", name);
    identity.add("native.environment.present",
                 static_cast<std::uint64_t>(value != nullptr));
    if (value != nullptr) {
      identity.add("native.environment.value", value);
    }
  }
}

std::vector<std::string>
nativeSourceArguments(const std::vector<std::string> &arguments,
                      const NativeInputs &inputs, bool debugInfo) {
  auto result = arguments;
  if (debugInfo)
    result.emplace_back("-g");
  if (inputs.instrumentation)
    append(result, inputs.instrumentation->compileArguments);
  return result;
}

std::optional<std::string> buildCacheKey(const ExecutableBuildRequest &request,
                                         const CompilationInputs &inputs,
                                         const BuildCachePolicy &policy,
                                         const NativeLinkIdentityPlan &linkPlan,
                                         std::string &errorMessage,
                                         std::string &policyBypassDetail) {
  IdentityBuilder identity;
  identity.add("cache.schema", buildCacheSchema(request));
  identity.add("compiler.identity", policy.compilerIdentity);
  identity.add("project-model.identity", policy.projectModelIdentity);
  if (request.nativeInputs().instrumentation) {
    identity.add("native.instrumentation.identity",
                 request.nativeInputs().instrumentation->identity);
  }
  if (nativeObjectCache(request)) {
    const LlvmBackendOptions options =
        llvmBackendOptions(request.compilation());
    const auto backend = queryLlvmBackendIdentity(
        request.compilation().target(), options, errorMessage);
    if (!backend || backend->schema.empty() || backend->llvmVersion.empty() ||
        backend->triple.empty() || backend->dataLayout.empty() ||
        backend->cpu.empty() || backend->privateAbi == 0 ||
        backend->instrumentation.empty() ||
        backend->debugInfo != options.debugInfo ||
        backend->instrumentation !=
            llvmInstrumentationModeName(options.instrumentation)) {
      if (errorMessage.empty())
        errorMessage = "incomplete LLVM backend identity";
      return std::nullopt;
    }
    // CMake requires a static LLVM closure. The containing executable's bytes
    // identify the actual GTI + LLVM implementation, including dirty builds.
    if (!addFileIdentity(identity, "llvm.compiler-image",
                         backend->compilerExecutable, false, errorMessage))
      return std::nullopt;
    identity.add("llvm.schema", backend->schema);
    identity.add("llvm.version", backend->llvmVersion);
    identity.add("llvm.private-abi", backend->privateAbi);
    identity.add("llvm.triple", backend->triple);
    identity.add("llvm.data-layout", backend->dataLayout);
    identity.add("llvm.cpu", backend->cpu);
    identity.add("llvm.features", backend->features);
    identity.add("llvm.relocation", backend->relocation);
    identity.add("llvm.code-model", backend->codeModel);
    identity.add("llvm.codegen-optimization", backend->codegenOptimization);
    identity.add("llvm.debug-info",
                 static_cast<std::uint64_t>(backend->debugInfo));
    identity.add("llvm.instrumentation", backend->instrumentation);
  }
  addNativeEnvironmentIdentity(identity);
  identity.add("backend.kind",
               static_cast<std::uint64_t>(request.compilation().backend()));
  identity.add("backend.standard",
               static_cast<std::uint64_t>(request.compilation().cppStandard()));
  identity.add(
      "frontend.requires-entry-point",
      static_cast<std::uint64_t>(request.compilation().requiresEntryPoint()));
  identity.add(
      "frontend.test-suite",
      static_cast<std::uint64_t>(request.compilation().program().testSuite));
  identity.add("optimization", static_cast<std::uint64_t>(
                                   request.compilation().optimization()));
  // Source-level debug artifacts embed the loaded units' paths (the
  // backend's line directives, native DWARF, the symbol bundle), so a debug
  // build is keyed on them where a plain build is keyed on logical names and
  // may move with its checkout. Darwin's symbol bundle is produced by
  // dsymutil, which joins the identity like the compilers do.
  const bool debugInfo = request.compilation().debugInfo();
  identity.add("debug-info", static_cast<std::uint64_t>(debugInfo));
  if (debugInfo) {
    std::vector<std::string> sourcePaths;
    for (const SourceUnit &unit : inputs.sourceGraph.sourceUnits()) {
      sourcePaths.push_back(unit.path.string());
    }
    std::sort(sourcePaths.begin(), sourcePaths.end());
    identity.add("debug.source-paths",
                 static_cast<std::uint64_t>(sourcePaths.size()));
    for (const std::string &path : sourcePaths) {
      identity.add("debug.source-path", path);
    }
    if (request.compilation().target().os == "macos" &&
        !addCompilerIdentity(identity, "debug.dsymutil", "dsymutil",
                             errorMessage)) {
      return std::nullopt;
    }
  }
  identity.add(
      "raw-pointer-audit",
      rawPointerAuditModeName(request.compilation().rawPointerAudit()));
  identity.add("raw-pointer-audit-library-findings",
               static_cast<std::uint64_t>(
                   request.compilation().rawPointerAuditLibraryFindings()));
  for (const std::string &group :
       request.compilation().warnings().disabledGroups()) {
    identity.add("warning.disabled-group", group);
  }
  const PackageWarningPolicy &packageWarnings =
      request.compilation().packageWarnings();
  identity.add(
      "warning.dependency-warnings",
      static_cast<std::uint64_t>(packageWarnings.dependencyWarnings()));
  identity.add("warning.selected-package", packageWarnings.selectedPackage());
  for (const std::string &package : packageWarnings.dependencyPackages()) {
    identity.add("warning.dependency-package", package);
  }
  for (const std::string &flag : request.compilation().configurationFlags()) {
    identity.add("configuration.flag", flag);
  }
  for (const auto &flag : request.compilation().declaredConfigurationFlags()) {
    identity.add("configuration.declared-flag", flag);
  }

  const TargetInfo &target = request.compilation().target();
  identity.add("target.os", target.os);
  identity.add("target.vendor", target.vendor);
  identity.add("target.arch", target.arch);
  identity.add("target.execution-profile",
               static_cast<std::uint64_t>(target.executionProfile));
  identity.add("target.runtime.threads",
               static_cast<std::uint64_t>(target.runtimeCapabilities.threads));
  identity.add("target.layout.supported",
               static_cast<std::uint64_t>(target.dataLayout.supported()));
  identity.add("target.layout.endianness",
               static_cast<std::uint64_t>(target.dataLayout.endianness()));
  identity.add("target.layout.pointer-width",
               target.dataLayout.pointerWidthBits());
  for (std::size_t index = 0; index < targetScalarKindCount; ++index) {
    const auto kind = static_cast<TargetScalarKind>(index);
    const std::optional<TargetTypeLayout> layout =
        target.dataLayout.scalarLayout(kind);
    identity.add("target.layout.scalar.kind",
                 static_cast<std::uint64_t>(index));
    identity.add("target.layout.scalar.present",
                 static_cast<std::uint64_t>(layout.has_value()));
    if (layout) {
      identity.add("target.layout.scalar.size", layout->sizeBytes);
      identity.add("target.layout.scalar.abi-align", layout->abiAlignmentBytes);
      identity.add("target.layout.scalar.preferred-align",
                   layout->preferredAlignmentBytes);
    }
  }

  if (!addSourceIdentity(identity, inputs, policy.sourceRoot, errorMessage)) {
    return std::nullopt;
  }

  // Native discovery memos are keyed on the facts that decide what a
  // preprocessor run can see: the environment, the compilers and the exact
  // probe argument vector.
  IdentityBuilder probeContext;
  probeContext.add("discovery.schema", nativeDiscoverySchema);
  addNativeEnvironmentIdentity(probeContext);
  if (!addCompilerIdentity(identity, "native-cxx", request.nativeCompiler(),
                           errorMessage, &probeContext)) {
    return std::nullopt;
  }
  const bool anyNativeCSources =
      !request.nativeInputs().cSources.empty() ||
      std::any_of(request.nativeInputs().dependencyGroups.begin(),
                  request.nativeInputs().dependencyGroups.end(),
                  [](const NativeDependencyGroup &group) {
                    return !group.cSources.empty();
                  });
  if (anyNativeCSources) {
    if (!request.cCompiler() || request.cCompiler()->empty()) {
      errorMessage = "native C inputs have no selected C compiler";
      return std::nullopt;
    }
    if (!addCompilerIdentity(identity, "native-cc", *request.cCompiler(),
                             errorMessage, &probeContext)) {
      return std::nullopt;
    }
  }
  const std::string probeContextDigest = probeContext.finish();

  if (!addDirectoryIdentity(identity, "toolchain.runtime-include",
                            request.toolchain().runtimeInclude, false,
                            errorMessage) ||
      !addFileIdentity(identity, "toolchain.runtime-library",
                       request.toolchain().runtimeLibrary, false,
                       errorMessage)) {
    return std::nullopt;
  }

  const NativeInputs &native = request.nativeInputs();
  if (!addPathDirectorySequence(identity, "native.include-directory",
                                native.includeDirectories, true,
                                errorMessage) ||
      !addPathFileSequence(identity, "native.c-source", native.cSources, true,
                           errorMessage) ||
      !addPathFileSequence(identity, "native.cpp-source", native.cppSources,
                           true, errorMessage) ||
      !addPathDirectorySequence(identity, "native.library-directory",
                                native.libraryDirectories, true,
                                errorMessage) ||
      !addPathFileSequence(identity, "native.library-file", native.libraryFiles,
                           true, errorMessage)) {
    return std::nullopt;
  }

  // Cross-compilation policy. The sysroot contributes path identity, not
  // content: it is a platform SDK tree, and the same non-hermetic toolchain
  // boundary that covers implicit system headers covers it.
  identity.add("native.target-policy.triple", native.targetPolicy.triple);
  identity.add("native.target-policy.sysroot",
               native.targetPolicy.sysroot.string());
  addScalarSequence(identity, "native.compiler-argument",
                    native.compilerArguments);
  addScalarSequence(identity, "native.c-compiler-argument",
                    native.cCompilerArguments);
  addScalarSequence(identity, "native.library", native.libraries);
  addScalarSequence(identity, "native.framework", native.frameworks);
  addScalarSequence(identity, "native.linker-argument", native.linkerArguments);
  addScalarSequence(identity, "native.trailing-argument",
                    native.trailingArguments);
  identity.add("native.c-standard.present",
               static_cast<std::uint64_t>(native.cStandard.has_value()));
  if (native.cStandard) {
    identity.add("native.c-standard",
                 static_cast<std::uint64_t>(*native.cStandard));
  }
  identity.add("native.ordered-link-operands",
               static_cast<std::uint64_t>(native.orderedLinkOperands.size()));
  for (const NativeLinkOperand &operand : native.orderedLinkOperands) {
    identity.add("native.link-operand.kind",
                 static_cast<std::uint64_t>(operand.kind));
    identity.add("native.link-operand.value", operand.value);
    if (operand.kind == NativeLinkOperandKind::File &&
        !addFileIdentity(identity, "native.link-operand.file",
                         std::filesystem::path(operand.value), true,
                         errorMessage)) {
      return std::nullopt;
    }
  }

  // The file each `-l<name>` operand resolved to inside the declared search
  // directories. Those directories already join as full tree identities, so
  // this records which of their hashed files the linker selects.
  identity.add("native.resolved-library.count",
               static_cast<std::uint64_t>(linkPlan.libraries.size()));
  for (const ResolvedNativeLibrary &library : linkPlan.libraries) {
    identity.add("native.resolved-library.name", library.name);
    if (!addFileIdentity(identity, "native.resolved-library.file", library.file,
                         true, errorMessage)) {
      return std::nullopt;
    }
  }

  // Apple framework names join only after a real linker-map probe established
  // the exact text stub or binary selected by this compiler/target/SDK policy.
  identity.add("native.resolved-framework.count",
               static_cast<std::uint64_t>(linkPlan.frameworks.size()));
  for (const ResolvedNativeFramework &framework : linkPlan.frameworks) {
    identity.add("native.resolved-framework.name", framework.name);
    if (!addFileIdentity(identity, "native.resolved-framework.file",
                         framework.file, true, errorMessage)) {
      return std::nullopt;
    }
  }

  identity.add("native.dependency-group.count",
               static_cast<std::uint64_t>(native.dependencyGroups.size()));
  for (const NativeDependencyGroup &group : native.dependencyGroups) {
    identity.add("native.dependency-group.package", group.packageIdentity);
    if (!addPathDirectorySequence(
            identity, "native.dependency-group.include-directory",
            group.includeDirectories, true, errorMessage) ||
        !addPathFileSequence(identity, "native.dependency-group.c-source",
                             group.cSources, true, errorMessage) ||
        !addPathFileSequence(identity, "native.dependency-group.cpp-source",
                             group.cppSources, true, errorMessage) ||
        !addPathDirectorySequence(
            identity, "native.dependency-group.library-directory",
            group.libraryDirectories, true, errorMessage)) {
      return std::nullopt;
    }
    addScalarSequence(identity, "native.dependency-group.c-macro",
                      group.cMacroDefinitions);
    addScalarSequence(identity, "native.dependency-group.cpp-macro",
                      group.cppMacroDefinitions);
    identity.add("native.dependency-group.c-standard",
                 static_cast<std::uint64_t>(group.cStandard));
    identity.add("native.dependency-group.link-operands",
                 static_cast<std::uint64_t>(group.linkOperands.size()));
    for (const NativeLinkOperand &operand : group.linkOperands) {
      identity.add("native.dependency-group.link-operand.kind",
                   static_cast<std::uint64_t>(operand.kind));
      identity.add("native.dependency-group.link-operand.value", operand.value);
      if (operand.kind == NativeLinkOperandKind::File &&
          !addFileIdentity(
              identity, "native.dependency-group.link-operand.file",
              std::filesystem::path(operand.value), true, errorMessage)) {
        return std::nullopt;
      }
    }
  }

  const bool anyNativeSources =
      !native.cSources.empty() || !native.cppSources.empty() ||
      std::any_of(native.dependencyGroups.begin(),
                  native.dependencyGroups.end(),
                  [](const NativeDependencyGroup &group) {
                    return !group.cSources.empty() || !group.cppSources.empty();
                  });
  if (anyNativeSources) {
    std::error_code temporaryError;
    const std::filesystem::path temporaryRoot =
        std::filesystem::temp_directory_path(temporaryError);
    if (temporaryError) {
      errorMessage = "cannot resolve a temporary directory for native "
                     "dependency discovery: " +
                     temporaryError.message();
      return std::nullopt;
    }
    const NativeToolchain nativeToolchain;
    // Every probe must repeat the exact include search and argument vector
    // the object compile will use, or discovery would report a different
    // input set than the build consumes.
    const auto discover =
        [&](const std::filesystem::path &source, std::string_view label,
            bool cSource,
            const std::vector<std::filesystem::path> &includeDirectories,
            const std::vector<std::string> &arguments, CStandard cStandard) {
          const std::filesystem::path preprocessed = stagedArtifactPath(
              temporaryRoot / (source.stem().string() + ".gti-discovery.i"));
          const std::filesystem::path depfile = stagedArtifactPath(
              temporaryRoot / (source.stem().string() + ".gti-discovery.d"));
          TemporaryArtifact preprocessedCleanup(preprocessed, true);
          TemporaryArtifact depfileCleanup(depfile, true);
          const auto effectiveArguments = nativeSourceArguments(
              arguments, native, request.compilation().debugInfo());
          const std::vector<std::string> probeCommand =
              cSource ? nativeToolchain.preprocessCommand(
                            NativeCCompileRequest(
                                *request.cCompiler(), source, preprocessed,
                                cStandard, request.compilation().optimization(),
                                includeDirectories, effectiveArguments,
                                native.targetPolicy),
                            depfile)
                      : nativeToolchain.preprocessCommand(
                            NativeCppCompileRequest(
                                request.nativeCompiler(), source, preprocessed,
                                request.compilation().cppStandard(),
                                request.compilation().optimization(),
                                includeDirectories, effectiveArguments,
                                native.targetPolicy),
                            depfile);
          std::optional<std::filesystem::path> memo;
          if (!policy.root.empty()) {
            IdentityBuilder key;
            key.add("discovery.context", probeContextDigest);
            key.add("discovery.label", label);
            key.add("discovery.source",
                    normalizedAbsolute(source).generic_string());
            key.add("discovery.argv",
                    static_cast<std::uint64_t>(probeCommand.size()));
            for (const std::string &argument : probeCommand) {
              // The staged output paths carry a per-run identity.
              key.add("discovery.arg", argument == preprocessed.string() ||
                                               argument == depfile.string()
                                           ? std::string_view("<staged>")
                                           : std::string_view(argument));
            }
            memo = policy.root / nativeDiscoveryDirectory / key.finish();
          }
          return addNativeSourceDiscovery(
              identity, label, probeCommand, source, preprocessed, depfile,
              includeDirectories, memo, errorMessage, policyBypassDetail);
        };

    std::vector<std::filesystem::path> cIncludeDirectories{
        request.toolchain().runtimeInclude};
    append(cIncludeDirectories, native.includeDirectories);
    const std::vector<std::filesystem::path> cppIncludeDirectories =
        nativeCppIncludeDirectories(request.toolchain(), native);
    for (const std::filesystem::path &source : native.cSources) {
      if (!discover(source, "native.c-source.discovery", true,
                    cIncludeDirectories, native.cCompilerArguments,
                    native.cStandard.value_or(CStandard::C17))) {
        return std::nullopt;
      }
    }
    for (const std::filesystem::path &source : native.cppSources) {
      if (!discover(source, "native.cpp-source.discovery", false,
                    cppIncludeDirectories, native.compilerArguments,
                    CStandard::C17)) {
        return std::nullopt;
      }
    }

    // Dependency-group sources compile with only their own include
    // directories and validated macro definitions, so their probes use the
    // same scoping the object compiles do.
    for (const NativeDependencyGroup &group : native.dependencyGroups) {
      identity.add("native.dependency-group.discovery.package",
                   group.packageIdentity);
      std::vector<std::filesystem::path> groupCIncludes{
          request.toolchain().runtimeInclude};
      append(groupCIncludes, group.includeDirectories);
      for (const std::filesystem::path &source : group.cSources) {
        if (!discover(source, "native.dependency-group.c-source.discovery",
                      true, groupCIncludes, group.cMacroDefinitions,
                      group.cStandard)) {
          return std::nullopt;
        }
      }
      std::vector<std::filesystem::path> groupCppIncludes{
          request.toolchain().runtimeInclude};
      append(groupCppIncludes, group.includeDirectories);
      for (const std::filesystem::path &source : group.cppSources) {
        if (!discover(source, "native.dependency-group.cpp-source.discovery",
                      false, groupCppIncludes, group.cppMacroDefinitions,
                      CStandard::C17)) {
          return std::nullopt;
        }
      }
    }
  }
  return identity.finish();
}

struct CacheMetadata {
  std::string key;
  bool object = false;
  FileDigest generated;
  FileDigest executable;
  std::uint32_t executablePermissions = 0;
  std::vector<std::string> testCases;
  // The debug symbol bundle published beside a source-level debug
  // executable (Darwin's `.dSYM`), as a tree digest over its files, or
  // absent when the build published none.
  std::optional<FileDigest> symbols;
};

constexpr std::string_view cacheSymbolsDirectory = "symbols.dSYM";

std::string testCatalogText(const std::vector<std::string> &names) {
  std::string text;
  for (const std::string &name : names)
    text += name + '\n';
  return text;
}

// A content digest of a directory tree: every regular file by its relative
// path, content hash and size, in sorted path order. Symlinks and other
// entries make the tree unidentifiable.
std::optional<FileDigest> digestTree(const std::filesystem::path &root,
                                     std::string &errorMessage) {
  std::error_code error;
  std::vector<std::filesystem::path> files;
  for (std::filesystem::recursive_directory_iterator
           iterator(root, std::filesystem::directory_options::none, error),
       end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      break;
    }
    const auto status = iterator->symlink_status(error);
    if (error) {
      break;
    }
    if (std::filesystem::is_directory(status)) {
      continue;
    }
    if (!std::filesystem::is_regular_file(status)) {
      errorMessage = "debug symbol bundle contains a non-regular entry '" +
                     iterator->path().string() + "'";
      return std::nullopt;
    }
    files.push_back(iterator->path());
  }
  if (error) {
    errorMessage =
        "cannot enumerate '" + root.string() + "': " + error.message();
    return std::nullopt;
  }
  std::sort(files.begin(), files.end());
  IdentityBuilder identity;
  std::uintmax_t total = 0;
  for (const std::filesystem::path &file : files) {
    const std::optional<FileDigest> digest = digestFile(file, errorMessage);
    if (!digest) {
      return std::nullopt;
    }
    identity.add("tree.path", file.lexically_relative(root).generic_string());
    identity.add("tree.sha256", digest->hash);
    identity.add("tree.size", static_cast<std::uint64_t>(digest->size));
    total += digest->size;
  }
  return FileDigest{.hash = identity.finish(), .size = total};
}

// Copies a directory tree of regular files, failing on anything else.
bool copyTree(const std::filesystem::path &source,
              const std::filesystem::path &destination,
              std::string &errorMessage) {
  std::error_code error;
  std::filesystem::remove_all(destination, error);
  if (error) {
    errorMessage =
        "cannot clear '" + destination.string() + "': " + error.message();
    return false;
  }
  std::filesystem::copy(source, destination,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::copy_symlinks,
                        error);
  if (error) {
    errorMessage = "cannot copy '" + source.string() + "' to '" +
                   destination.string() + "': " + error.message();
    return false;
  }
  return true;
}

bool validDigest(std::string_view digest) {
  return digest.size() == 64 &&
         std::all_of(digest.begin(), digest.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::optional<CacheMetadata>
readCacheMetadata(const std::filesystem::path &path, bool object,
                  std::string &errorMessage) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    errorMessage = "metadata is missing or unreadable";
    return std::nullopt;
  }

  std::string schema;
  CacheMetadata metadata;
  metadata.object = object;
  if (object) {
    std::string label, kind, extension;
    if (!(input >> schema >> metadata.key >> label >> kind >> extension) ||
        schema != objectCacheSchema || label != "artifact" ||
        kind != "object" || extension != ".o") {
      errorMessage = "metadata has an invalid native artifact schema";
      return std::nullopt;
    }
  } else if (!(input >> schema >> metadata.key) || schema != cacheSchema) {
    errorMessage = "metadata has an invalid source artifact schema";
    return std::nullopt;
  }
  std::string generatedLabel;
  std::string executableLabel;
  std::string symbolsLabel;
  std::string symbolsHash;
  if (!(input >> generatedLabel >> metadata.generated.hash >>
        metadata.generated.size >> executableLabel >>
        metadata.executable.hash >> metadata.executable.size >>
        metadata.executablePermissions >> symbolsLabel >> symbolsHash) ||
      generatedLabel != (object ? "representation" : "generated") ||
      executableLabel != "executable" || symbolsLabel != "symbols" ||
      !validDigest(metadata.key) || !validDigest(metadata.generated.hash) ||
      !validDigest(metadata.executable.hash)) {
    errorMessage = "metadata has an invalid schema or digest";
    return std::nullopt;
  }
  if (symbolsHash != "none") {
    std::uintmax_t symbolsSize = 0;
    if (!validDigest(symbolsHash) || !(input >> symbolsSize)) {
      errorMessage = "metadata has an invalid symbol bundle digest";
      return std::nullopt;
    }
    metadata.symbols = FileDigest{.hash = symbolsHash, .size = symbolsSize};
  }
  std::string testsLabel;
  std::size_t testCount = 0;
  std::string testDigest;
  if (!(input >> testsLabel >> testCount >> testDigest) ||
      testsLabel != "tests" || testCount > 100000 || !validDigest(testDigest)) {
    errorMessage = "metadata has an invalid test catalog";
    return std::nullopt;
  }
  for (std::size_t i = 0; i < testCount; ++i) {
    std::string label, name;
    if (!(input >> label >> name) || label != "case" || name.empty() ||
        name.size() > 4096 ||
        !std::all_of(name.begin(), name.end(),
                     [](unsigned char c) {
                       return (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == ':';
                     }) ||
        (!metadata.testCases.empty() && name <= metadata.testCases.back())) {
      errorMessage = "metadata has an invalid or unordered test name";
      return std::nullopt;
    }
    metadata.testCases.push_back(std::move(name));
  }
  if (digestText(testCatalogText(metadata.testCases)) != testDigest) {
    errorMessage = "test catalog digest does not match metadata";
    return std::nullopt;
  }
  std::string trailing;
  if (input >> trailing) {
    errorMessage = "metadata contains unexpected trailing fields";
    return std::nullopt;
  }
  return metadata;
}

std::string renderCacheMetadata(const CacheMetadata &metadata) {
  std::string text =
      std::string(metadata.object ? objectCacheSchema : cacheSchema) + "\n" +
      metadata.key + "\n" +
      (metadata.object ? "artifact object .o\nrepresentation " : "generated ") +
      metadata.generated.hash + " " + std::to_string(metadata.generated.size) +
      "\n" + "executable " + metadata.executable.hash + " " +
      std::to_string(metadata.executable.size) + " " +
      std::to_string(metadata.executablePermissions) + "\n" + "symbols " +
      (metadata.symbols ? metadata.symbols->hash + " " +
                              std::to_string(metadata.symbols->size)
                        : std::string("none")) +
      "\n";
  text += "tests " + std::to_string(metadata.testCases.size()) + " " +
          digestText(testCatalogText(metadata.testCases)) + "\n";
  for (const std::string &name : metadata.testCases)
    text += "case " + name + '\n';
  return text;
}

enum class CacheLookupStatus {
  Miss,
  Hit,
  Corrupt,
};

struct CacheLookupResult {
  CacheLookupStatus status = CacheLookupStatus::Miss;
  std::string generated;
  FileDigest executableDigest;
  std::filesystem::perms executablePermissions = std::filesystem::perms::none;
  // The verified debug symbol bundle inside the entry, when it holds one.
  std::optional<std::filesystem::path> symbols;
  std::string detail;
  std::vector<std::string> testCases;
};

std::optional<std::string> readTextFile(const std::filesystem::path &path,
                                        std::string &errorMessage) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    errorMessage = "cannot read '" + path.string() + "'";
    return std::nullopt;
  }
  std::string contents;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    contents.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
  }
  if (input.bad()) {
    errorMessage = "cannot finish reading '" + path.string() + "'";
    return std::nullopt;
  }
  return contents;
}

CacheLookupResult lookupCacheEntry(const std::filesystem::path &entry,
                                   std::string_view expectedKey, bool object,
                                   bool requiresSymbols) {
  const std::filesystem::path metadataPath = entry / "metadata";
  const std::filesystem::path generatedPath =
      entry / cacheRepresentationFile(object);
  const std::filesystem::path executablePath = entry / "executable";
  std::error_code error;
  const bool entryExists = std::filesystem::exists(entry, error);
  if (error) {
    return {.status = CacheLookupStatus::Corrupt,
            .detail = "cannot inspect cache entry: " + error.message()};
  }
  if (!entryExists) {
    return {.status = CacheLookupStatus::Miss,
            .detail = "no entry for this input identity"};
  }

  std::string detail;
  if (object) {
    for (const auto &path : {metadataPath, generatedPath, executablePath}) {
      error.clear();
      if (!std::filesystem::is_regular_file(
              std::filesystem::symlink_status(path, error)) ||
          error) {
        return {.status = CacheLookupStatus::Corrupt,
                .detail = "native cache payload is not a regular file"};
      }
    }
  }
  const std::optional<CacheMetadata> metadata =
      readCacheMetadata(metadataPath, object, detail);
  if (!metadata) {
    return {.status = CacheLookupStatus::Corrupt, .detail = std::move(detail)};
  }
  if (metadata->symbols.has_value() != requiresSymbols)
    return {.status = CacheLookupStatus::Corrupt,
            .detail =
                "debug companion presence does not match the requested policy"};

  if (metadata->key != expectedKey) {
    return {.status = CacheLookupStatus::Corrupt,
            .detail = "metadata key does not match its cache directory"};
  }

  std::optional<std::string> generated = readTextFile(generatedPath, detail);
  if (!generated) {
    return {.status = CacheLookupStatus::Corrupt, .detail = std::move(detail)};
  }
  const FileDigest generatedDigest{.hash = digestText(*generated),
                                   .size = generated->size()};
  if (generatedDigest != metadata->generated) {
    return {.status = CacheLookupStatus::Corrupt,
            .detail = "backend representation digest does not match metadata"};
  }
  const std::optional<FileDigest> executableDigest =
      digestFile(executablePath, detail);
  if (!executableDigest || *executableDigest != metadata->executable) {
    return {.status = CacheLookupStatus::Corrupt,
            .detail = executableDigest
                          ? "executable digest does not match metadata"
                          : std::move(detail)};
  }
  std::error_code permissionError;
  const std::filesystem::perms executablePermissions =
      std::filesystem::status(executablePath, permissionError).permissions() &
      std::filesystem::perms::mask;
  if (permissionError || static_cast<std::uint32_t>(executablePermissions) !=
                             metadata->executablePermissions) {
    return {.status = CacheLookupStatus::Corrupt,
            .detail = permissionError
                          ? "cannot inspect executable permissions: " +
                                permissionError.message()
                          : "executable permissions do not match metadata"};
  }
  std::optional<std::filesystem::path> symbols;
  if (metadata->symbols) {
    const std::filesystem::path symbolsPath = entry / cacheSymbolsDirectory;
    const std::optional<FileDigest> symbolsDigest =
        digestTree(symbolsPath, detail);
    if (!symbolsDigest || *symbolsDigest != *metadata->symbols) {
      return {.status = CacheLookupStatus::Corrupt,
              .detail = symbolsDigest
                            ? "debug symbol bundle digest does not match "
                              "metadata"
                            : std::move(detail)};
    }
    symbols = symbolsPath;
  }
  const std::string detailText =
      symbols ? "verified backend representation, executable and debug "
                "symbol digests"
              : "verified backend representation and executable digests";
  return {.status = CacheLookupStatus::Hit,
          .generated = std::move(*generated),
          .executableDigest = *executableDigest,
          .executablePermissions = executablePermissions,
          .symbols = std::move(symbols),
          .detail = detailText + ", and test catalog digest",
          .testCases = metadata->testCases};
}

bool writeBinaryFile(const std::filesystem::path &path,
                     std::string_view contents, std::string &errorMessage) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    errorMessage = "cannot open '" + path.string() + "' for writing";
    return false;
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  if (!output) {
    errorMessage = "cannot finish writing '" + path.string() + "'";
    return false;
  }
  return true;
}

bool publishTextAtomically(const std::filesystem::path &destination,
                           std::string_view contents,
                           std::string &errorMessage) {
  const std::filesystem::path staged = stagedArtifactPath(destination);
  TemporaryArtifact stagedArtifact(staged, true);
  if (!writeBinaryFile(staged, contents, errorMessage)) {
    return false;
  }
  const ArtifactPublishResult published = publishArtifact(staged, destination);
  if (!published.succeeded()) {
    errorMessage = "cannot publish '" + destination.string() +
                   "': " + published.error.message();
    return false;
  }
  return true;
}

ArtifactPublishResult copyFileAtomically(
    const std::filesystem::path &source,
    const std::filesystem::path &destination, std::string &errorMessage,
    const std::optional<FileDigest> &expected = std::nullopt,
    const std::optional<std::filesystem::perms> &expectedPermissions =
        std::nullopt) {
  const std::filesystem::path staged = stagedArtifactPath(destination);
  TemporaryArtifact stagedArtifact(staged, true);
  std::error_code error;
  std::filesystem::copy_file(source, staged, error);
  if (error) {
    errorMessage = "cannot copy '" + source.string() +
                   "' to staged artifact '" + staged.string() +
                   "': " + error.message();
    return {error};
  }
  error.clear();
  std::filesystem::perms permissions =
      std::filesystem::status(source, error).permissions();
  if (expectedPermissions) {
    permissions = *expectedPermissions;
  }
  if (!error) {
    std::filesystem::permissions(staged, permissions,
                                 std::filesystem::perm_options::replace, error);
  }
  if (error) {
    errorMessage = "cannot preserve permissions for staged artifact '" +
                   staged.string() + "': " + error.message();
    return {error};
  }
  if (expected) {
    const std::optional<FileDigest> stagedDigest =
        digestFile(staged, errorMessage);
    if (!stagedDigest || *stagedDigest != *expected) {
      if (stagedDigest) {
        errorMessage =
            "staged artifact digest changed during cache restoration";
      }
      return {std::make_error_code(std::errc::io_error)};
    }
  }
  const ArtifactPublishResult published = publishArtifact(staged, destination);
  if (!published.succeeded()) {
    errorMessage = "cannot publish '" + destination.string() +
                   "': " + published.error.message();
  }
  return published;
}

bool storeCacheEntry(const ExecutableBuildRequest &request,
                     const std::filesystem::path &entry, std::string_view key,
                     const BackendArtifact &artifact,
                     const std::vector<std::string> &testCases,
                     std::string &errorMessage) {
  if (!request.managedOutput()) {
    errorMessage = "cache publication requires a managed project output";
    return false;
  }
  const std::filesystem::path metadataPath = entry / "metadata";
  if (const std::optional<std::string> diagnostic =
          managedPathDiagnostic(*request.managedOutput(), metadataPath, true)) {
    errorMessage = *diagnostic;
    return false;
  }

  const bool object = nativeObjectCache(request);
  if (artifact.kind != (object ? BackendArtifactKind::Object
                               : BackendArtifactKind::Source) ||
      artifact.extension != (object ? ".o" : ".cpp")) {
    errorMessage = "backend artifact does not match the cache schema";
    return false;
  }
  const std::string_view generated = artifact.contents;
  const std::filesystem::path generatedPath =
      entry / cacheRepresentationFile(object);
  const std::filesystem::path executablePath = entry / "executable";
  const FileDigest generatedDigest{.hash = digestText(generated),
                                   .size = generated.size()};
  if (!publishTextAtomically(generatedPath, generated, errorMessage)) {
    return false;
  }
  const ArtifactPublishResult executablePublished =
      copyFileAtomically(request.output(), executablePath, errorMessage);
  if (!executablePublished.succeeded()) {
    return false;
  }
  const std::optional<FileDigest> executableDigest =
      digestFile(executablePath, errorMessage);
  if (!executableDigest) {
    return false;
  }
  std::error_code permissionError;
  const std::filesystem::perms executablePermissions =
      std::filesystem::status(executablePath, permissionError).permissions() &
      std::filesystem::perms::mask;
  if (permissionError) {
    errorMessage = "cannot inspect cached executable permissions: " +
                   permissionError.message();
    return false;
  }

  CacheMetadata metadata{.key = std::string(key),
                         .object = object,
                         .generated = generatedDigest,
                         .executable = *executableDigest,
                         .executablePermissions =
                             static_cast<std::uint32_t>(executablePermissions),
                         .testCases = testCases};
  // A source-level debug build on Darwin published its symbol bundle beside
  // the executable; the entry keeps a copy so a hit can restore both.
  const std::filesystem::path symbolsPath = entry / cacheSymbolsDirectory;
  std::error_code symbolsError;
  std::filesystem::remove_all(symbolsPath, symbolsError);
  if (request.compilation().debugInfo() &&
      request.compilation().target().os == "macos") {
    const std::filesystem::path published(request.output().string() + ".dSYM");
    if (std::filesystem::is_directory(published, symbolsError)) {
      if (!copyTree(published, symbolsPath, errorMessage)) {
        return false;
      }
      metadata.symbols = digestTree(symbolsPath, errorMessage);
      if (!metadata.symbols) {
        return false;
      }
    }
  }
  // Metadata is the commit marker. Readers ignore incomplete entries until it
  // has been atomically replaced after both payloads have been published.
  return publishTextAtomically(metadataPath, renderCacheMetadata(metadata),
                               errorMessage);
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

// The declared native library search directories in the exact order the link
// command emits them, which is the order the linker searches them: the
// selected package's own directories first, then each dependency group's in
// composed order. `effectiveNativeInputs` builds the same sequence.
std::vector<std::filesystem::path>
nativeLibrarySearchDirectories(const NativeInputs &inputs) {
  std::vector<std::filesystem::path> directories = inputs.libraryDirectories;
  for (const NativeDependencyGroup &group : inputs.dependencyGroups) {
    append(directories, group.libraryDirectories);
  }
  return directories;
}

#if defined(__APPLE__)
std::optional<std::filesystem::path>
frameworkSelectionFromLinkMap(std::string_view linkMap, std::string_view name,
                              std::string &detail) {
  const std::string marker = "/" + std::string(name) + ".framework/";
  const std::string binaryName(name);
  const std::string stubName = binaryName + ".tbd";
  std::optional<std::filesystem::path> selected;
  while (!linkMap.empty()) {
    const std::size_t newline = linkMap.find('\n');
    std::string_view line = linkMap.substr(0, newline);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line == "# Sections:") {
      break;
    }
    const std::size_t fieldEnd = line.find("] ");
    if (fieldEnd != std::string_view::npos) {
      const std::string_view pathText = line.substr(fieldEnd + 2);
      if (pathText.find(marker) != std::string_view::npos) {
        const std::filesystem::path candidate(pathText);
        const std::string filename = candidate.filename().string();
        if (filename == binaryName || filename == stubName) {
          const std::filesystem::path normalized =
              normalizedAbsolute(candidate);
          if (selected && *selected != normalized) {
            detail = "framework '" + std::string(name) +
                     "' resolved to multiple link inputs";
            return std::nullopt;
          }
          selected = normalized;
        }
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    linkMap.remove_prefix(newline + 1);
  }
  if (!selected) {
    detail = "framework '" + std::string(name) +
             "' did not appear in the native linker's map";
  }
  return selected;
}
#endif

// Uses the selected native compiler and linker to resolve every `-framework`
// name exactly as the real link will. The emitted Apple link map names the
// chosen framework stub or binary; that file then joins the cache key by path
// and content. A failed, missing, or ambiguous trace remains a conservative
// cache bypass and never prevents the ordinary native build from running.
std::optional<std::string>
resolveNativeFrameworks(const ExecutableBuildRequest &request,
                        const std::vector<std::string> &frameworkNames,
                        NativeLinkIdentityPlan &plan) {
  if (frameworkNames.empty()) {
    return std::nullopt;
  }
#if !defined(__APPLE__)
  (void)request;
  (void)plan;
  return "name-resolved frameworks require an Apple linker resolution trace";
#else
  std::vector<std::string> uniqueNames;
  uniqueNames.reserve(frameworkNames.size());
  for (const std::string &name : frameworkNames) {
    if (std::find(uniqueNames.begin(), uniqueNames.end(), name) ==
        uniqueNames.end()) {
      uniqueNames.push_back(name);
    }
  }

  const std::filesystem::path source =
      temporaryArtifactPath(request.output(), ".framework-probe.cpp");
  const std::filesystem::path output =
      temporaryArtifactPath(request.output(), ".framework-probe");
  const std::filesystem::path linkMap =
      temporaryArtifactPath(request.output(), ".framework-probe.map");
  TemporaryArtifact sourceCleanup(source, true);
  TemporaryArtifact outputCleanup(output, true);
  TemporaryArtifact linkMapCleanup(linkMap, true);
  std::string writeError;
  if (!publishTextAtomically(source, "int main() { return 0; }\n",
                             writeError)) {
    return "framework resolution probe could not create its source: " +
           writeError;
  }

  std::vector<std::string> command{request.nativeCompiler(), "-x", "c++"};
  appendNativeTargetPolicy(command, request.nativeInputs().targetPolicy);
  command.push_back(source.string());
  for (const std::string &name : uniqueNames) {
    command.emplace_back("-framework");
    command.push_back(name);
  }
  command.emplace_back("-Xlinker");
  command.emplace_back("-map");
  command.emplace_back("-Xlinker");
  command.push_back(linkMap.string());
  command.emplace_back("-o");
  command.push_back(output.string());
  const ProcessResult probe =
      invokeProcess(command, {.outputMode = ProcessOutputMode::Capture,
                              .captureSuccessfulOutput = false,
                              .description = "native framework resolution"});
  if (!probe.succeeded()) {
    return "framework resolution probe failed with exit code " +
           std::to_string(probe.exitCode);
  }

  std::string readError;
  const std::optional<std::string> contents = readTextFile(linkMap, readError);
  if (!contents) {
    return "framework resolution probe produced no readable linker map: " +
           readError;
  }
  for (const std::string &name : uniqueNames) {
    std::string detail;
    const std::optional<std::filesystem::path> selected =
        frameworkSelectionFromLinkMap(*contents, name, detail);
    if (!selected) {
      return detail;
    }
    std::error_code statusError;
    if (!std::filesystem::is_regular_file(*selected, statusError) ||
        statusError) {
      return "framework '" + name + "' resolved to unreadable link input '" +
             selected->string() + "'";
    }
    plan.frameworks.push_back({.name = name, .file = *selected});
  }
  return std::nullopt;
#endif
}

// Returns the reason a native configuration must bypass the cache, or
// nullopt when every native input can join the identity exactly, filling
// `plan` with the concrete files that named libraries resolved to. Declared
// C and C++ sources and include directories are cacheable through dependency
// discovery and full-tree identity, including a dependency group's, because a
// group composes only validated exact `-D`/`-U` macro definitions. Declared
// library search directories join as full tree identities, so the linker's
// choice among them is a function of hashed content; a `-l` name joins only
// when it resolves to exactly one content-complete file inside them. The
// remaining bypasses are inputs the driver cannot resolve to exact content
// identities. Narrowing a bypass here must never widen what the key trusts:
// an unidentifiable input bypasses.
std::optional<std::string>
nativeCacheBypassDetail(const ExecutableBuildRequest &request,
                        NativeLinkIdentityPlan &plan) {
  plan = {};
  if (request.compilation().backend() != BackendKind::Cpp &&
      request.compilation().backend() != BackendKind::Llvm) {
    return "unknown backend artifact identity";
  }
  const NativeInputs &inputs = request.nativeInputs();
  if (!inputs.compilerArguments.empty() || !inputs.cCompilerArguments.empty() ||
      !inputs.linkerArguments.empty() || !inputs.trailingArguments.empty()) {
    return "opaque native argument vectors can introduce undeclared "
           "compiler and linker inputs";
  }

  // Mirror the link composition `effectiveNativeInputs` performs: the ordered
  // operand vector is authoritative once anything populates it, and the
  // category vectors are the link only when nothing does.
  const bool groupLinkContribution = std::any_of(
      inputs.dependencyGroups.begin(), inputs.dependencyGroups.end(),
      [](const NativeDependencyGroup &group) {
        return !group.linkOperands.empty() || !group.libraryDirectories.empty();
      });
  std::vector<std::filesystem::path> linkFiles;
  std::vector<std::string> namedLibraries;
  std::vector<std::string> namedFrameworks;
  const auto collectOperands =
      [&](const std::vector<NativeLinkOperand> &operands) {
        for (const NativeLinkOperand &operand : operands) {
          switch (operand.kind) {
          case NativeLinkOperandKind::File:
            linkFiles.emplace_back(operand.value);
            break;
          case NativeLinkOperandKind::Library:
            namedLibraries.push_back(operand.value);
            break;
          case NativeLinkOperandKind::Framework:
            namedFrameworks.push_back(operand.value);
            break;
          }
        }
      };
  if (inputs.orderedLinkOperands.empty() && !groupLinkContribution) {
    linkFiles = inputs.libraryFiles;
    namedLibraries = inputs.libraries;
    namedFrameworks = inputs.frameworks;
  } else {
    collectOperands(inputs.orderedLinkOperands);
    for (const NativeDependencyGroup &group : inputs.dependencyGroups) {
      collectOperands(group.linkOperands);
    }
  }

  const std::vector<std::filesystem::path> searchDirectories =
      nativeLibrarySearchDirectories(inputs);
  for (const std::string &library : namedLibraries) {
    const NativeLibraryResolution resolution =
        resolveNativeLibraryName(library, searchDirectories);
    switch (resolution.status) {
    case NativeLibraryResolutionStatus::Resolved:
      plan.libraries.push_back({.name = library, .file = resolution.file});
      linkFiles.push_back(resolution.file);
      break;
    case NativeLibraryResolutionStatus::Unresolved:
      return "native library '-l" + library +
             "' does not resolve inside the declared native library search "
             "directories";
    case NativeLibraryResolutionStatus::Ambiguous:
      return "native library '-l" + library +
             "' resolves ambiguously in native library search directory '" +
             resolution.directory.string() + "'";
    case NativeLibraryResolutionStatus::UnsupportedSpelling:
      return "native library name '" + library +
             "' is not a plain library name the driver can resolve";
    case NativeLibraryResolutionStatus::DirectoryUnreadable:
      return "native library search directory '" +
             resolution.directory.string() +
             "' cannot be enumerated for cache identity";
    }
  }

  for (const std::filesystem::path &linkFile : linkFiles) {
    switch (classifyNativeLinkInput(linkFile)) {
    case NativeLinkInputClass::StaticArchive:
    case NativeLinkInputClass::RelocatableObject:
      break;
    case NativeLinkInputClass::RequiresDependencyDiscovery:
      return "link input '" + linkFile.string() +
             "' is not a content-complete archive or object (thin archives, "
             "linker scripts, and shared libraries require link-input "
             "discovery)";
    case NativeLinkInputClass::Unreadable:
      return "link input '" + linkFile.string() +
             "' cannot be read for cache identity";
    }
  }
  if (const std::optional<std::string> frameworkBypass =
          resolveNativeFrameworks(request, namedFrameworks, plan)) {
    return frameworkBypass;
  }
  return std::nullopt;
}

bool hasDependencyInjectingNativeEnvironment() {
  // These variables name mutable search roots or toolchain resources. Hashing
  // only the variable spelling cannot identify headers, linker scripts,
  // archives, plugins, or sub-tools reached through the named location.
  constexpr std::array<std::string_view, 15> variables{
      "CPATH",
      "C_INCLUDE_PATH",
      "CPLUS_INCLUDE_PATH",
      "OBJC_INCLUDE_PATH",
      "LIBRARY_PATH",
      "COMPILER_PATH",
      "GCC_EXEC_PREFIX",
      "SDKROOT",
      "DEVELOPER_DIR",
      "LD_LIBRARY_PATH",
      "DYLD_LIBRARY_PATH",
      "DYLD_FRAMEWORK_PATH",
      "DYLD_FALLBACK_FRAMEWORK_PATH",
      "INCLUDE",
      "LIB",
  };
  return std::any_of(variables.begin(), variables.end(),
                     [](std::string_view name) {
                       const std::string ownedName(name);
                       const char *value = std::getenv(ownedName.c_str());
                       return value != nullptr && value[0] != '\0';
                     });
}

std::vector<std::filesystem::path>
nativeCppIncludeDirectories(const ToolchainLayout &toolchain,
                            const NativeInputs &additional) {
  std::vector<std::filesystem::path> directories{toolchain.runtimeInclude};
  append(directories, additional.includeDirectories);
  return directories;
}

NativeInputs
effectiveNativeInputs(const ToolchainLayout &toolchain,
                      const NativeInputs &additional,
                      const std::vector<std::filesystem::path> &nativeObjects) {
  NativeInputs inputs;
  // The resolved cross-compilation policy is driver-owned and must survive
  // link-input assembly: without it the final link would silently run the
  // declared cross compiler in its host configuration.
  inputs.targetPolicy = additional.targetPolicy;
  inputs.instrumentation = additional.instrumentation;
  inputs.includeDirectories =
      nativeCppIncludeDirectories(toolchain, additional);
  append(inputs.compilerArguments, additional.compilerArguments);
  append(inputs.libraryDirectories, additional.libraryDirectories);
  const bool groupLinkContribution = std::any_of(
      additional.dependencyGroups.begin(), additional.dependencyGroups.end(),
      [](const NativeDependencyGroup &group) {
        return !group.linkOperands.empty() || !group.libraryDirectories.empty();
      });
  for (const NativeDependencyGroup &group : additional.dependencyGroups) {
    append(inputs.libraryDirectories, group.libraryDirectories);
  }
  if (additional.orderedLinkOperands.empty() && !groupLinkContribution) {
    append(inputs.libraryFiles, nativeObjects);
    append(inputs.libraryFiles, additional.libraryFiles);
    append(inputs.libraries, additional.libraries);
    append(inputs.frameworks, additional.frameworks);
  } else {
    for (const std::filesystem::path &object : nativeObjects) {
      inputs.orderedLinkOperands.push_back(
          {NativeLinkOperandKind::File, object.string()});
    }
    append(inputs.orderedLinkOperands, additional.orderedLinkOperands);
    // Dependency link contributions follow the application's own, in the
    // composed dependents-before-dependencies order, so a dependent's
    // libraries may resolve symbols from the packages it depends on.
    for (const NativeDependencyGroup &group : additional.dependencyGroups) {
      append(inputs.orderedLinkOperands, group.linkOperands);
    }
  }
  append(inputs.linkerArguments, additional.linkerArguments);
  append(inputs.trailingArguments, additional.trailingArguments);
  // The runtime is the last link input, after forwarded arguments too, so
  // native objects, libraries and forwarded sources may call or replace its
  // definitions under order-sensitive linkers exactly as under ld64.
  inputs.finalLibraryFiles.push_back(toolchain.runtimeLibrary);
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

std::optional<BuildRuntimeProvenance> captureBuildRuntimeProvenance(
    const ToolchainLayout &toolchain,
    const std::vector<std::filesystem::path> &dependencies,
    bool requireRuntimeHeaders, std::string &errorMessage) {
  BuildRuntimeProvenance provenance;
  const std::filesystem::path runtimeRoot =
      normalizedAbsolute(toolchain.runtimeInclude / "gti");
  std::vector<std::string> seen;
  for (const std::filesystem::path &dependency : dependencies) {
    const std::filesystem::path canonical = normalizedAbsolute(dependency);
    BuildRuntimeHeaderKind kind;
    std::filesystem::path relative;
    if (pathIsWithin(runtimeRoot, canonical)) {
      kind = BuildRuntimeHeaderKind::Runtime;
      relative = std::filesystem::path("gti") /
                 canonical.lexically_relative(runtimeRoot);
    } else {
      continue;
    }
    const std::string key = canonical.generic_string();
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
      continue;
    }
    seen.push_back(key);
    std::optional<BuildFileProvenance> material =
        captureFileProvenance(canonical, errorMessage);
    if (!material) {
      return std::nullopt;
    }
    provenance.headers.push_back({.kind = kind,
                                  .logicalIdentity = relative.generic_string(),
                                  .material = std::move(*material)});
  }
  std::sort(provenance.headers.begin(), provenance.headers.end(),
            [](const BuildRuntimeHeaderProvenance &left,
               const BuildRuntimeHeaderProvenance &right) {
              if (left.kind != right.kind) {
                return left.kind < right.kind;
              }
              return left.logicalIdentity < right.logicalIdentity;
            });
  if (requireRuntimeHeaders &&
      std::none_of(provenance.headers.begin(), provenance.headers.end(),
                   [](const BuildRuntimeHeaderProvenance &header) {
                     return header.kind == BuildRuntimeHeaderKind::Runtime;
                   })) {
    errorMessage = "native dependency report contains no used GTI runtime "
                   "headers";
    return std::nullopt;
  }
  std::optional<BuildFileProvenance> archive =
      captureFileProvenance(toolchain.runtimeLibrary, errorMessage);
  if (!archive) {
    return std::nullopt;
  }
  provenance.archive = std::move(*archive);

  IdentityBuilder materialsIdentity;
  materialsIdentity.add("runtime.headers",
                        static_cast<std::uint64_t>(provenance.headers.size()));
  for (const BuildRuntimeHeaderProvenance &header : provenance.headers) {
    materialsIdentity.add("runtime.kind",
                          static_cast<std::uint64_t>(header.kind));
    materialsIdentity.add("runtime.logical", header.logicalIdentity);
    materialsIdentity.add("runtime.bytes",
                          static_cast<std::uint64_t>(header.material.bytes));
    materialsIdentity.add("runtime.sha256", header.material.sha256);
  }
  materialsIdentity.add("runtime.archive.bytes",
                        static_cast<std::uint64_t>(provenance.archive.bytes));
  materialsIdentity.add("runtime.archive.sha256", provenance.archive.sha256);
  provenance.materialsSha256 = materialsIdentity.finish();
  return provenance;
}

} // namespace

std::string_view compilerSourceStateName(CompilerSourceState state) {
  switch (state) {
  case CompilerSourceState::Clean:
    return "clean";
  case CompilerSourceState::Dirty:
    return "dirty";
  case CompilerSourceState::Unavailable:
    return "unavailable";
  }
  return "unavailable";
}

CompilerBuildIdentity currentCompilerBuildIdentity() {
  const std::string_view state = GTI_COMPILER_SOURCE_STATE;
  const std::string_view commit = GTI_COMPILER_SOURCE_COMMIT;
  if (commit.size() != 40) {
    return {};
  }
  if (state == "clean") {
    return {.state = CompilerSourceState::Clean, .commit = std::string(commit)};
  }
  if (state == "dirty") {
    return {.state = CompilerSourceState::Dirty, .commit = std::string(commit)};
  }
  return {};
}

std::string_view buildRuntimeHeaderKindName(BuildRuntimeHeaderKind kind) {
  switch (kind) {
  case BuildRuntimeHeaderKind::Runtime:
    return "runtime";
  }
  return "runtime";
}

std::optional<std::vector<std::filesystem::path>>
parseNativeDependencyFile(std::string_view contents,
                          std::string &errorMessage) {
  std::vector<std::filesystem::path> dependencies;
  std::string token;
  bool sawSeparator = false;
  bool tokenEndedRule = false;

  const auto finishToken = [&]() -> bool {
    if (token.empty()) {
      tokenEndedRule = false;
      return true;
    }
    if (tokenEndedRule) {
      // A token ending in an unescaped colon after the rule separator would
      // be an additional make rule (for example -MP phony targets). The
      // driver never requests those, so refuse rather than guess.
      errorMessage = "dependency report contains an unexpected second rule";
      return false;
    }
    if (sawSeparator) {
      dependencies.emplace_back(token);
    }
    // Tokens before the separator name the rule target and carry no
    // dependency identity.
    token.clear();
    return true;
  };

  for (std::size_t index = 0; index < contents.size(); ++index) {
    const char character = contents[index];
    if (character == '\\') {
      if (index + 1 < contents.size()) {
        const char next = contents[index + 1];
        if (next == '\n') {
          ++index;
          if (!finishToken()) {
            return std::nullopt;
          }
          continue;
        }
        if (next == '\r' && index + 2 < contents.size() &&
            contents[index + 2] == '\n') {
          index += 2;
          if (!finishToken()) {
            return std::nullopt;
          }
          continue;
        }
        if (next == ' ' || next == '\t' || next == '#' || next == ':' ||
            next == '\\') {
          token.push_back(next);
          ++index;
          continue;
        }
      }
      // A lone backslash is an ordinary path character (Windows separators).
      token.push_back(character);
      continue;
    }
    if (character == '$' && index + 1 < contents.size() &&
        contents[index + 1] == '$') {
      token.push_back('$');
      ++index;
      continue;
    }
    if (character == ' ' || character == '\t' || character == '\n' ||
        character == '\r') {
      if (!finishToken()) {
        return std::nullopt;
      }
      continue;
    }
    if (character == ':' && !sawSeparator) {
      // A colon directly after a single leading letter with a path separator
      // following is a Windows drive specifier, not the rule separator.
      const bool driveColon =
          token.size() == 1 &&
          std::isalpha(static_cast<unsigned char>(token.front())) != 0 &&
          index + 1 < contents.size() &&
          (contents[index + 1] == '/' || contents[index + 1] == '\\');
      if (!driveColon) {
        token.clear();
        sawSeparator = true;
        continue;
      }
      token.push_back(character);
      continue;
    }
    if (character == ':' && sawSeparator) {
      const bool endsToken =
          index + 1 >= contents.size() || contents[index + 1] == ' ' ||
          contents[index + 1] == '\t' || contents[index + 1] == '\n' ||
          contents[index + 1] == '\r';
      const bool driveColon =
          token.size() == 1 &&
          std::isalpha(static_cast<unsigned char>(token.front())) != 0 &&
          !endsToken &&
          (contents[index + 1] == '/' || contents[index + 1] == '\\');
      if (driveColon) {
        token.push_back(character);
        continue;
      }
      token.push_back(character);
      if (endsToken) {
        token.pop_back();
        tokenEndedRule = true;
      }
      continue;
    }
    token.push_back(character);
  }
  if (!finishToken()) {
    return std::nullopt;
  }
  if (!sawSeparator) {
    errorMessage = "dependency report has no rule separator";
    return std::nullopt;
  }
  return dependencies;
}

NativeLinkInputClass
classifyNativeLinkInput(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return NativeLinkInputClass::Unreadable;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return NativeLinkInputClass::Unreadable;
  }
  std::array<unsigned char, 20> header{};
  input.read(reinterpret_cast<char *>(header.data()),
             static_cast<std::streamsize>(header.size()));
  const std::size_t available = static_cast<std::size_t>(input.gcount());

  const auto startsWith = [&](std::string_view magic) {
    if (available < magic.size()) {
      return false;
    }
    for (std::size_t index = 0; index < magic.size(); ++index) {
      if (header[index] != static_cast<unsigned char>(magic[index])) {
        return false;
      }
    }
    return true;
  };
  if (startsWith("!<arch>\n")) {
    return NativeLinkInputClass::StaticArchive;
  }
  if (startsWith("!<thin>\n")) {
    return NativeLinkInputClass::RequiresDependencyDiscovery;
  }

  const auto read32 = [&](std::size_t offset, bool littleEndian) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
      const std::size_t position =
          littleEndian ? offset + 3 - index : offset + index;
      value = (value << 8U) | header[position];
    }
    return value;
  };
  if (available >= 18 && header[0] == 0x7fU && header[1] == 'E' &&
      header[2] == 'L' && header[3] == 'F') {
    const bool bigEndian = header[5] == 2;
    const std::uint16_t type =
        bigEndian ? static_cast<std::uint16_t>((header[16] << 8U) | header[17])
                  : static_cast<std::uint16_t>(header[16] | (header[17] << 8U));
    constexpr std::uint16_t elfRelocatable = 1;
    return type == elfRelocatable
               ? NativeLinkInputClass::RelocatableObject
               : NativeLinkInputClass::RequiresDependencyDiscovery;
  }
  if (available >= 16) {
    constexpr std::uint32_t machO32 = 0xfeedfaceU;
    constexpr std::uint32_t machO64 = 0xfeedfacfU;
    constexpr std::uint32_t machObjectFile = 1;
    for (const bool littleEndian : {true, false}) {
      const std::uint32_t magic = read32(0, littleEndian);
      if (magic == machO32 || magic == machO64) {
        return read32(12, littleEndian) == machObjectFile
                   ? NativeLinkInputClass::RelocatableObject
                   : NativeLinkInputClass::RequiresDependencyDiscovery;
      }
    }
  }
  // Text linker scripts, universal/fat binaries, import libraries, and every
  // unrecognized format may reference inputs beyond their own bytes.
  return NativeLinkInputClass::RequiresDependencyDiscovery;
}

namespace {

// The file names a linker considers for `-l<name>`, in the order it prefers
// them inside one directory. Only these spellings are modeled; anything else
// that could plausibly answer the name makes resolution ambiguous.
std::vector<std::string> nativeLibraryFileNames(std::string_view name) {
  const std::string owned(name);
#if defined(_WIN32)
  return {owned + ".lib", "lib" + owned + ".a", "lib" + owned + ".dll.a"};
#elif defined(__APPLE__)
  return {"lib" + owned + ".dylib", "lib" + owned + ".tbd",
          "lib" + owned + ".a"};
#else
  return {"lib" + owned + ".so", "lib" + owned + ".a"};
#endif
}

// Deliberately wider than the modeled candidate set: a spelling this driver
// does not model must make resolution ambiguous, never invisible.
bool namesNativeLibraryCandidate(std::string_view fileName,
                                 std::string_view library) {
  const std::string prefix = "lib" + std::string(library) + ".";
  if (fileName.size() > prefix.size() &&
      fileName.substr(0, prefix.size()) == prefix) {
    return true;
  }
#if defined(_WIN32)
  const std::string bare = std::string(library) + ".";
  if (fileName.size() > bare.size() &&
      fileName.substr(0, bare.size()) == bare) {
    return true;
  }
#endif
  return false;
}

} // namespace

NativeLibraryResolution resolveNativeLibraryName(
    std::string_view name,
    const std::vector<std::filesystem::path> &searchDirectories) {
  NativeLibraryResolution resolution;
  if (name.empty() || name.front() == ':' ||
      name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos) {
    resolution.status = NativeLibraryResolutionStatus::UnsupportedSpelling;
    return resolution;
  }
  const std::vector<std::string> candidates = nativeLibraryFileNames(name);
  for (const std::filesystem::path &directory : searchDirectories) {
    std::error_code error;
    std::filesystem::directory_iterator entry(directory, error);
    const std::filesystem::directory_iterator end;
    if (error) {
      resolution.status = NativeLibraryResolutionStatus::DirectoryUnreadable;
      resolution.directory = directory;
      return resolution;
    }
    std::vector<std::filesystem::path> matches;
    bool unmodelledCandidate = false;
    while (entry != end) {
      const std::filesystem::path candidate = entry->path();
      const std::string fileName = candidate.filename().string();
      if (namesNativeLibraryCandidate(fileName, name)) {
        if (std::find(candidates.begin(), candidates.end(), fileName) ==
            candidates.end()) {
          unmodelledCandidate = true;
        } else {
          matches.push_back(candidate);
        }
      }
      entry.increment(error);
      if (error) {
        resolution.status = NativeLibraryResolutionStatus::DirectoryUnreadable;
        resolution.directory = directory;
        return resolution;
      }
    }
    if (unmodelledCandidate || matches.size() > 1) {
      resolution.status = NativeLibraryResolutionStatus::Ambiguous;
      resolution.directory = directory;
      return resolution;
    }
    if (matches.size() == 1) {
      std::error_code fileError;
      if (!std::filesystem::is_regular_file(matches.front(), fileError) ||
          fileError) {
        resolution.status = NativeLibraryResolutionStatus::Ambiguous;
        resolution.directory = directory;
        return resolution;
      }
      resolution.status = NativeLibraryResolutionStatus::Resolved;
      resolution.file = matches.front();
      resolution.directory = directory;
      return resolution;
    }
  }
  resolution.status = NativeLibraryResolutionStatus::Unresolved;
  return resolution;
}

bool containsTimeSensitivePreprocessorUse(std::string_view text) {
  return text.find("__DATE__") != std::string_view::npos ||
         text.find("__TIME__") != std::string_view::npos ||
         text.find("__TIMESTAMP__") != std::string_view::npos;
}

namespace {
std::optional<NativeInstrumentationPlan>
resolveLlvmNativeRuntimeImpl(const std::string &compiler,
                             const LlvmBackendIdentity &backend,
                             LlvmInstrumentationMode mode, std::string &error) {
  NativeInstrumentationPlan plan;
  try {
    plan.mode = llvmInstrumentationModeName(mode);
  } catch (const std::invalid_argument &failure) {
    error = failure.what();
    return std::nullopt;
  }
  if (mode == LlvmInstrumentationMode::None)
    return plan;
  const auto fail =
      [&](std::string detail) -> std::optional<NativeInstrumentationPlan> {
    error = std::move(detail);
    return std::nullopt;
  };
  const auto configured = [](const char *name) {
    const char *value = std::getenv(name);
    return value && *value;
  };
  if (hasDependencyInjectingNativeEnvironment() ||
      configured("CCC_OVERRIDE_OPTIONS") ||
      configured("MACOSX_DEPLOYMENT_TARGET") ||
      configured("IPHONEOS_DEPLOYMENT_TARGET") || configured("LD_PRELOAD") ||
      configured("LD_AUDIT") || configured("DYLD_INSERT_LIBRARIES") ||
      configured("DYLD_IMAGE_SUFFIX") || configured("DYLD_ROOT_PATH") ||
      configured("DYLD_VERSIONED_LIBRARY_PATH") ||
      configured("DYLD_VERSIONED_FRAMEWORK_PATH"))
    return fail(
        "LLVM instrumentation requires an identifiable implicit host "
        "toolchain without native search, deployment or loader overrides");
  if (backend.llvmVersion.empty() || backend.triple.empty())
    return fail("LLVM instrumentation requires the producer's actual toolchain "
                "identity");
  const auto executable = resolveExecutableForIdentity(compiler);
  if (!executable)
    return fail("cannot resolve the selected instrumentation compiler");
  plan.compilerCommand = compiler;
  plan.compilerExecutable = std::filesystem::weakly_canonical(*executable);
  plan.llvmVersion = backend.llvmVersion;
  const auto probe =
      [&](std::vector<std::string> arguments,
          std::chrono::milliseconds timeout =
              std::chrono::milliseconds(30000)) -> std::optional<std::string> {
    const auto result = invokeProcess(
        arguments, {.captureSuccessfulOutput = true,
                    .description = "LLVM native runtime identity probe",
                    .timeout = timeout,
                    .isolateProcessGroup = true});
    if (!result.succeeded() || result.output.size() > 1024 * 1024) {
      error = "selected Clang could not establish a bounded native runtime "
              "identity";
      return std::nullopt;
    }
    return result.output;
  };
  const auto trim = [](std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
      return std::string{};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
  };
  const auto version = probe({compiler, "--version"});
  if (!version)
    return std::nullopt;
  const auto firstLine = version->substr(0, version->find('\n'));
  const auto marker = firstLine.find("clang version ");
  if (marker == std::string::npos || firstLine.starts_with("Apple clang"))
    return fail(
        "LLVM instrumentation requires matching upstream Clang compiler-rt; "
        "the selected native compiler is not an identified upstream Clang");
  const auto versionBegin = marker + std::string_view("clang version ").size();
  plan.compilerVersion = firstLine.substr(
      versionBegin,
      firstLine.find_first_of(" \t", versionBegin) - versionBegin);
  if (plan.compilerVersion != backend.llvmVersion)
    return fail("selected Clang version " + plan.compilerVersion +
                " does not match backend LLVM " + backend.llvmVersion);
  std::vector<std::string> prefix{compiler, "--no-default-config"};
  std::istringstream versionLines(*version);
  std::string line;
  while (std::getline(versionLines, line)) {
    constexpr std::string_view label = "Configuration file: ";
    if (!line.starts_with(label))
      continue;
    const auto path =
        std::filesystem::weakly_canonical(trim(line.substr(label.size())));
    if (!std::filesystem::is_regular_file(path) ||
        !plan.configurationFiles.empty())
      return fail(
          "cannot identify one flat selected Clang default configuration");
    std::ifstream input(path);
    std::string contents{std::istreambuf_iterator<char>(input), {}};
    if (!input || contents.size() > 65536)
      return fail("cannot read the selected Clang default configuration");
    std::istringstream configuration(contents);
    std::string token;
    while (configuration >> std::quoted(token)) {
      if (token.starts_with('#')) {
        std::getline(configuration, line);
        continue;
      }
      if (token.starts_with('@') || token == "--config" ||
          token.starts_with("--config=") || token == "-resource-dir" ||
          token.starts_with("-resource-dir=") ||
          token.find('\\') != std::string::npos ||
          token.find('\'') != std::string::npos)
        return fail(
            "nested or redirected Clang configuration needs exact input "
            "discovery before instrumentation can be selected");
    }
    plan.configurationFiles.push_back(path);
    prefix.push_back("--config=" + path.string());
  }
  const auto query = [&](std::string option) {
    auto command = prefix;
    command.push_back(std::move(option));
    return probe(std::move(command));
  };
  const auto resource = query("--print-resource-dir");
  const auto runtime = query("--print-runtime-dir");
  const auto triple = query("-dumpmachine");
  if (!resource || !runtime || !triple)
    return std::nullopt;
  const auto nativeTarget = parseTargetTriple(trim(*triple));
  const auto producerTarget = parseTargetTriple(backend.triple);
  if (!nativeTarget || !producerTarget ||
      nativeTarget->os != producerTarget->os ||
      nativeTarget->arch != producerTarget->arch ||
      nativeTarget->dataLayout != producerTarget->dataLayout ||
      !((producerTarget->os == "macos" && producerTarget->arch == "arm64") ||
        (producerTarget->os == "linux" && producerTarget->arch == "x86_64")))
    return fail("selected Clang runtime target does not match the supported "
                "LLVM host ABI");
  plan.resourceDirectory = std::filesystem::weakly_canonical(trim(*resource));
  plan.runtimeDirectory = std::filesystem::weakly_canonical(trim(*runtime));
  const auto below = [](const std::filesystem::path &root,
                        const std::filesystem::path &path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() &&
           *relative.begin() != "..";
  };
  if (!std::filesystem::is_directory(plan.resourceDirectory) ||
      (producerTarget->os == "macos" &&
       !std::filesystem::is_directory(plan.runtimeDirectory)) ||
      !below(plan.resourceDirectory, plan.runtimeDirectory))
    return fail("selected compiler-rt directory is unavailable or outside "
                "Clang's resource root");
  plan.compileArguments.assign(prefix.begin() + 1, prefix.end());
  plan.linkArguments = plan.compileArguments;
  std::string compileMode;
  std::string linkMode;
  switch (mode) {
  case LlvmInstrumentationMode::None:
    break;
  case LlvmInstrumentationMode::Address:
    compileMode = linkMode = "address";
    break;
  case LlvmInstrumentationMode::UndefinedMemory:
    compileMode = "null,alignment";
    linkMode = "undefined";
    break;
  case LlvmInstrumentationMode::AddressAndUndefinedMemory:
    compileMode = "address,null,alignment";
    linkMode = "address,undefined";
    break;
  case LlvmInstrumentationMode::Thread:
    compileMode = linkMode = "thread";
    break;
  }
  for (auto *arguments : {&plan.compileArguments, &plan.linkArguments}) {
    arguments->emplace_back("-fno-sanitize=all");
    arguments->emplace_back("-fno-sanitize-recover=all");
    arguments->emplace_back("-fno-sanitize-trap=all");
  }
  plan.compileArguments.push_back("-fsanitize=" + compileMode);
  plan.compileArguments.emplace_back("-fno-sanitize-ignorelist");
  plan.linkArguments.push_back("-fsanitize=" + linkMode);
  if (producerTarget->os == "macos") {
    append(plan.linkArguments,
           std::vector<std::string>{"-Xlinker", "-rpath", "-Xlinker",
                                    plan.runtimeDirectory.string()});
  }
  if (mode == LlvmInstrumentationMode::Address ||
      mode == LlvmInstrumentationMode::AddressAndUndefinedMemory)
    plan.compileArguments.emplace_back("-fno-sanitize-address-use-after-scope");
  if (mode == LlvmInstrumentationMode::Thread) {
    plan.compileArguments.emplace_back("-fPIE");
    plan.linkArguments.push_back(producerTarget->os == "macos" ? "-Wl,-pie"
                                                               : "-pie");
  }
  struct ProbeDirectory {
    std::filesystem::path path;
    ~ProbeDirectory() {
      std::error_code ignored;
      if (!path.empty())
        std::filesystem::remove_all(path, ignored);
    }
  } temporary;
  const auto directory = stagedArtifactPath(
      std::filesystem::temp_directory_path() / "gti-native-runtime-probe");
  if (!std::filesystem::create_directory(directory))
    return fail("cannot create isolated native runtime probe directory");
  temporary.path = directory;
  const auto source = directory / "probe.c";
  const auto object = directory / "probe.o";
  const auto output = directory / "probe";
  std::string program;
  std::vector<std::string> hooks;
  if (mode == LlvmInstrumentationMode::Address ||
      mode == LlvmInstrumentationMode::AddressAndUndefinedMemory) {
    program += "extern void __asan_init(void);\n";
    hooks.emplace_back("__asan_init");
  }
  if (mode == LlvmInstrumentationMode::UndefinedMemory ||
      mode == LlvmInstrumentationMode::AddressAndUndefinedMemory) {
    program += "extern void __ubsan_handle_type_mismatch_v1_abort(void*, "
               "unsigned long);\n";
    hooks.emplace_back("__ubsan_handle_type_mismatch_v1_abort");
  }
  if (mode == LlvmInstrumentationMode::Thread) {
    program += "extern void __tsan_init(void);\n";
    hooks.emplace_back("__tsan_init");
  }
  for (std::size_t index = 0; index < hooks.size(); ++index) {
    const auto parameters =
        hooks[index] == "__ubsan_handle_type_mismatch_v1_abort"
            ? "void*, unsigned long"
            : "void";
    program += "void (*volatile hook" + std::to_string(index) + ")(" +
               parameters + ") = &" + hooks[index] + ";\n";
  }
  program += "int main(void) { return ";
  for (std::size_t index = 0; index < hooks.size(); ++index)
    program +=
        (index ? " || " : "") + std::string("!hook") + std::to_string(index);
  program += "; }\n";
  {
    std::ofstream file(source);
    file << program;
    if (!file)
      return fail("cannot stage native runtime probe");
  }
  std::vector<std::string> compile{compiler};
  append(compile, plan.compileArguments);
  append(compile,
         std::vector<std::string>{"-x", "c", "-std=c17", "-c", source.string(),
                                  "-o", object.string()});
  auto compileTraceCommand = compile;
  compileTraceCommand.emplace_back("-###");
  const auto compileTrace = probe(compileTraceCommand);
  if (!compileTrace || !probe(compile, std::chrono::milliseconds(60000)))
    return std::nullopt;
  std::vector<std::string> link{compiler};
  append(link, plan.linkArguments);
  append(link,
         std::vector<std::string>{object.string(), "-o", output.string()});
  auto traceCommand = link;
  traceCommand.emplace_back("-###");
  const auto trace = probe(traceCommand);
  if (!trace)
    return std::nullopt;
  const auto quotedCommands = [&](const std::string &text) {
    std::vector<std::vector<std::string>> commands;
    std::istringstream lines(text);
    std::string row;
    while (std::getline(lines, row)) {
      row = trim(std::move(row));
      if (row.empty() || row.front() != '"')
        continue;
      std::istringstream input(row);
      std::vector<std::string> arguments;
      while (input >> std::ws && !input.eof()) {
        std::string argument;
        if (input.peek() != '"' || !(input >> std::quoted(argument)))
          return std::vector<std::vector<std::string>>{};
        arguments.push_back(std::move(argument));
      }
      if (!arguments.empty())
        commands.push_back(std::move(arguments));
    }
    return commands;
  };
  const auto compilers = quotedCommands(*compileTrace);
  const auto linkers = quotedCommands(*trace);
  if (compilers.size() != 1 || linkers.size() != 1 ||
      std::find(compilers.front().begin(), compilers.front().end(), "-cc1") ==
          compilers.front().end())
    return fail(
        "cannot establish exact Clang frontend and linker command identity");
  plan.linkTrace = linkers.front();
  if (producerTarget->os == "linux") {
    // Some distribution Clang drivers report a nonexistent per-target runtime
    // directory while selecting the installed compiler-rt layout at link time.
    // The driver's concrete link plan, not a guessed directory name, owns that
    // choice. Retain one exact resource-contained root for hashing and checks.
    std::set<std::filesystem::path> selectedRuntimeRoots;
    const auto resourceLibraries = plan.resourceDirectory / "lib";
    for (const auto &argument : plan.linkTrace) {
      const std::filesystem::path path(argument);
      if (!path.is_absolute() || !std::filesystem::is_regular_file(path))
        continue;
      const auto canonical = std::filesystem::weakly_canonical(path);
      if (below(resourceLibraries, path)) {
        if (!below(resourceLibraries, canonical))
          return fail(
              "selected compiler-rt link escapes Clang's resource root");
        selectedRuntimeRoots.insert(canonical.parent_path());
      }
    }
    if (selectedRuntimeRoots.size() != 1)
      return fail("Clang's native link plan requires one exact installed "
                  "compiler-rt directory");
    plan.runtimeDirectory = *selectedRuntimeRoots.begin();
  }
  const auto frontend =
      std::filesystem::weakly_canonical(compilers.front().front());
  if (!std::filesystem::equivalent(plan.compilerExecutable, frontend))
    return fail("opaque Clang launchers require explicit transitive toolchain "
                "provenance");
  if (producerTarget->os == "macos") {
    const auto first =
        std::find(plan.linkTrace.begin(), plan.linkTrace.end(), "-rpath");
    if (first == plan.linkTrace.end() ||
        std::next(first) == plan.linkTrace.end() ||
        std::filesystem::weakly_canonical(*std::next(first)) !=
            plan.runtimeDirectory)
      return fail("native runtime loader policy does not prioritize the "
                  "identified compiler-rt root");
  }
  std::set<std::filesystem::path> files{plan.compilerExecutable};
  files.insert(std::filesystem::weakly_canonical(compilers.front().front()));
  // Frontend resource files (for example an explicitly selected forced include)
  // can change native instrumentation without changing preprocessed user code.
  for (const auto &argument : compilers.front()) {
    std::filesystem::path path(argument);
    if (!path.is_absolute()) {
      const auto equals = argument.find('=');
      if (equals == std::string::npos)
        continue;
      path = argument.substr(equals + 1);
    }
    if (!path.is_absolute() || !std::filesystem::is_regular_file(path))
      continue;
    const auto canonical = std::filesystem::weakly_canonical(path);
    if (!below(std::filesystem::weakly_canonical(directory), canonical))
      files.insert(canonical);
  }
  for (auto &argument : plan.linkTrace) {
    if (argument == object.string()) {
      argument = "<native-runtime-probe-object>";
      continue;
    }
    if (argument == output.string()) {
      argument = "<native-runtime-probe-output>";
      continue;
    }
    const std::filesystem::path path(argument);
    if (!path.is_absolute() || !std::filesystem::is_regular_file(path))
      continue;
    const auto canonical = std::filesystem::weakly_canonical(path);
    const std::filesystem::path reportedRuntime(trim(*runtime));
    if ((below(reportedRuntime, path) || below(plan.runtimeDirectory, path)) &&
        !below(plan.runtimeDirectory, canonical))
      return fail(
          "selected compiler-rt link escapes Clang's identified runtime root");
    files.insert(canonical);
    if (below(plan.runtimeDirectory, canonical))
      plan.runtimeFiles.push_back(canonical);
  }
  if (plan.runtimeFiles.empty())
    return fail("Clang's native link plan did not select concrete matching "
                "compiler-rt files");
  for (std::size_t index = 0; index + 1 < plan.linkTrace.size(); ++index) {
    if (plan.linkTrace[index] == "-syslibroot" ||
        plan.linkTrace[index] == "--sysroot") {
      const auto sdk =
          std::filesystem::weakly_canonical(plan.linkTrace[index + 1]);
      if (!std::filesystem::is_directory(sdk / "usr/lib") ||
          !std::filesystem::is_regular_file(sdk / "SDKSettings.json"))
        return fail(
            "selected native SDK library/settings identity is unavailable");
      plan.sdkDirectories.push_back(sdk);
    }
  }
  // Ask the actual linker for its selected file closure, including SDK tbd
  // reexports. Unselected SDK directory aliases cannot affect this link.
  auto tracedLink = link;
  tracedLink.emplace_back("-Wl,-t");
  const auto loaded = probe(tracedLink, std::chrono::milliseconds(60000));
  if (loaded && producerTarget->os == "macos") {
    // Clang also supplies @executable_path. An unrelated library beside the
    // probe must not shadow the identified matching runtime directory.
    for (const auto &file : plan.runtimeFiles) {
      if (file.extension() != ".dylib")
        continue;
      std::ofstream shadow(directory / file.filename());
      shadow << "unrelated compiler-rt shadow must not load\n";
      if (!shadow)
        return fail("cannot establish isolated native runtime loader control");
    }
  }
  if (!loaded || !probe({output.string()}))
    return fail(
        "matching compiler-rt could not link and start on the selected host; "
        "no instrumentation profile was admitted");
  std::istringstream loadedFiles(*loaded);
  const auto probeRoot = std::filesystem::weakly_canonical(directory);
  while (std::getline(loadedFiles, line)) {
    line = trim(std::move(line));
    if (line.empty() || !std::filesystem::path(line).is_absolute())
      continue;
    std::filesystem::path path(line);
    if (!std::filesystem::is_regular_file(path) && line.ends_with(')')) {
      auto member = line.rfind('(');
      while (member != std::string::npos) {
        const std::filesystem::path archive(line.substr(0, member));
        if (std::filesystem::is_regular_file(archive)) {
          path = archive;
          break;
        }
        member = member == 0 ? std::string::npos : line.rfind('(', member - 1);
      }
    }
    if (!std::filesystem::is_regular_file(path))
      return fail(
          "cannot establish an exact native linker input from its file trace");
    const auto canonical = std::filesystem::weakly_canonical(path);
    if (below(probeRoot, canonical))
      continue;
    const std::filesystem::path reportedRuntime(trim(*runtime));
    if ((below(reportedRuntime, path) || below(plan.runtimeDirectory, path)) &&
        !below(plan.runtimeDirectory, canonical))
      return fail(
          "selected compiler-rt link escapes Clang's identified runtime root");
    for (std::size_t index = 0; index + 1 < plan.linkTrace.size(); ++index) {
      if (plan.linkTrace[index] != "-syslibroot" &&
          plan.linkTrace[index] != "--sysroot")
        continue;
      const std::filesystem::path reportedSdk(plan.linkTrace[index + 1]);
      const auto sdk = std::filesystem::weakly_canonical(reportedSdk);
      if ((below(reportedSdk, path) || below(sdk, path)) &&
          !below(sdk, canonical))
        return fail("selected native SDK library alias escapes its identified "
                    "SDK root");
    }
    files.insert(canonical);
    if (below(plan.runtimeDirectory, canonical))
      plan.runtimeFiles.push_back(canonical);
  }
  plan.implementationFiles.assign(files.begin(), files.end());
  std::sort(plan.runtimeFiles.begin(), plan.runtimeFiles.end());
  plan.runtimeFiles.erase(
      std::unique(plan.runtimeFiles.begin(), plan.runtimeFiles.end()),
      plan.runtimeFiles.end());
  IdentityBuilder identity;
  identity.add("schema", "gti-native-instrumentation-v1");
  identity.add("mode", plan.mode);
  identity.add("llvm.version", backend.llvmVersion);
  identity.add("target", backend.triple);
  identity.add("compiler.command", compiler);
  identity.add("compiler.version", *version);
  identity.add("resource", plan.resourceDirectory.generic_string());
  identity.add("runtime", plan.runtimeDirectory.generic_string());
  for (const auto &path : plan.configurationFiles)
    if (!addFileIdentity(identity, "configuration", path, true, error))
      return std::nullopt;
  for (const auto &path : plan.implementationFiles)
    if (!addFileIdentity(identity, "implementation", path, true, error))
      return std::nullopt;
  for (const auto &sdk : plan.sdkDirectories)
    if (!addFileIdentity(identity, "sdk.settings", sdk / "SDKSettings.json",
                         true, error))
      return std::nullopt;
  for (const auto &argument : plan.compileArguments)
    identity.add("compile.argument", argument);
  for (const auto &argument : plan.linkArguments)
    identity.add("link.argument", argument);
  for (const auto &argument : plan.linkTrace)
    identity.add("resolved.link.argument", argument);
  plan.identity = identity.finish();
  return plan;
}

} // namespace

std::optional<NativeInstrumentationPlan>
resolveLlvmNativeRuntime(const std::string &compiler,
                         const LlvmBackendIdentity &backend,
                         LlvmInstrumentationMode mode, std::string &error) {
  try {
    return resolveLlvmNativeRuntimeImpl(compiler, backend, mode, error);
  } catch (const std::exception &failure) {
    error = "cannot inspect selected Clang runtime resources: " +
            std::string(failure.what());
    return std::nullopt;
  }
}

ExecutableBuildRequest::ExecutableBuildRequest(
    CompilationRequest compilation, ToolchainLayout toolchain,
    std::filesystem::path generatedSource, std::filesystem::path output,
    std::string nativeCompiler, NativeInputs nativeInputs,
    bool keepGeneratedSource, bool createParentDirectories,
    bool captureSuccessfulNativeOutput,
    std::optional<ManagedOutputPolicy> managedOutput,
    std::optional<std::string> cCompiler, std::optional<BuildCachePolicy> cache,
    std::optional<BuildReportRequest> buildReport)
    : compilationRequest(compilation.forExecutable()),
      toolchainLayout(std::move(toolchain)),
      generatedSourcePath(std::move(generatedSource)),
      outputPath(std::move(output)),
      compilerExecutable(std::move(nativeCompiler)),
      cCompilerExecutable(std::move(cCompiler)),
      additionalNativeInputs(std::move(nativeInputs)),
      retainGeneratedSource(keepGeneratedSource),
      createParents(createParentDirectories),
      captureSuccessfulOutput(captureSuccessfulNativeOutput),
      managedOutputPolicy(std::move(managedOutput)),
      buildCachePolicy(std::move(cache)),
      requestedBuildReport(std::move(buildReport)) {}

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

const std::optional<BuildCachePolicy> &ExecutableBuildRequest::cache() const {
  return buildCachePolicy;
}

const std::optional<BuildReportRequest> &
ExecutableBuildRequest::buildReport() const {
  return requestedBuildReport;
}

namespace {
ExecutableBuildResult
buildResolvedExecutable(const ExecutableBuildRequest &request,
                        ProgressObserver *progress) {
  ExecutableBuildResult result;
  result.generatedSource = request.generatedSource();
  result.nativeInstrumentation = request.nativeInputs().instrumentation;
  const auto hasObjectiveCpp = [](const auto &sources) {
    return std::any_of(sources.begin(), sources.end(), [](const auto &source) {
      return source.extension() == ".mm";
    });
  };
  if (request.compilation().target().os != "macos" &&
      (hasObjectiveCpp(request.nativeInputs().cppSources) ||
       std::any_of(request.nativeInputs().dependencyGroups.begin(),
                   request.nativeInputs().dependencyGroups.end(),
                   [&](const auto &group) {
                     return hasObjectiveCpp(group.cppSources);
                   }))) {
    result.status = ExecutableBuildStatus::ToolchainConfigurationFailure;
    result.driverDiagnostic =
        "gti: Objective-C++ '.mm' sources require a macOS target";
    return result;
  }
  if (request.buildReport()) {
    const std::filesystem::path report =
        normalizedAbsolute(request.buildReport()->output);
    const auto conflictsWith = [&](const std::filesystem::path &path) {
      return artifactPathsAlias(report, path);
    };
    if (conflictsWith(request.output()) ||
        conflictsWith(request.generatedSource()) ||
        conflictsWith(request.buildReport()->producerExecutable)) {
      result.status = ExecutableBuildStatus::ArtifactPathConflict;
      result.driverDiagnostic =
          "gti: --build-report must not overwrite the executable, generated "
          "backend representation, or compiler executable";
      return result;
    }
  }
  const std::vector<NativeDependencyGroup> &dependencyGroups =
      request.nativeInputs().dependencyGroups;
  std::vector<std::filesystem::path> nativeObjects;
  for (std::size_t index = 0; index < request.nativeInputs().cSources.size();
       ++index) {
    nativeObjects.push_back(
        nativeObjectPath(request.generatedSource(),
                         request.nativeInputs().cSources[index], index));
  }
  const std::size_t cppObjectOffset = nativeObjects.size();
  for (std::size_t index = 0; index < request.nativeInputs().cppSources.size();
       ++index) {
    nativeObjects.push_back(nativeObjectPath(
        request.generatedSource(), request.nativeInputs().cppSources[index],
        cppObjectOffset + index));
  }
  // Dependency-group objects follow the application's own objects with one
  // continuous index, so object names stay unique and deterministic across
  // packages that reuse source stems.
  struct GroupObjectRange {
    std::size_t cBegin = 0;
    std::size_t cppBegin = 0;
  };
  std::vector<GroupObjectRange> groupObjectRanges;
  groupObjectRanges.reserve(dependencyGroups.size());
  bool groupCSources = false;
  for (const NativeDependencyGroup &group : dependencyGroups) {
    GroupObjectRange range{.cBegin = nativeObjects.size(), .cppBegin = 0};
    for (const std::filesystem::path &source : group.cSources) {
      nativeObjects.push_back(nativeObjectPath(request.generatedSource(),
                                               source, nativeObjects.size()));
      groupCSources = true;
    }
    range.cppBegin = nativeObjects.size();
    for (const std::filesystem::path &source : group.cppSources) {
      nativeObjects.push_back(nativeObjectPath(request.generatedSource(),
                                               source, nativeObjects.size()));
    }
    groupObjectRanges.push_back(range);
  }
  if ((!request.nativeInputs().cSources.empty() || groupCSources) &&
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

  CompilationInputs compilationInputs =
      loadCompilationInputs(request.compilation(), progress);
  if (!compilationInputs.succeeded()) {
    result.compilation = compileToSelectedBackend(
        request.compilation(), std::move(compilationInputs), progress);
    result.status = ExecutableBuildStatus::CompilationFailure;
    return result;
  }
  if (const std::optional<std::string> diagnostic =
          loadedSourceCollisionDiagnostic(request.output(), "executable output",
                                          compilationInputs.sources)) {
    result.status = ExecutableBuildStatus::ArtifactPathConflict;
    result.driverDiagnostic = diagnostic;
    return result;
  }
  if (const std::optional<std::string> diagnostic =
          loadedSourceCollisionDiagnostic(request.generatedSource(),
                                          "backend representation output",
                                          compilationInputs.sources)) {
    result.status = ExecutableBuildStatus::ArtifactPathConflict;
    result.driverDiagnostic = diagnostic;
    return result;
  }
  if (request.compilation().debugInfo() &&
      request.compilation().target().os == "macos") {
    const std::filesystem::path symbols(request.output().string() + ".dSYM");
    if (const auto collision = debugBundleSourceCollision(
            symbols, compilationInputs.sources, request.generatedSource())) {
      result.status = ExecutableBuildStatus::ArtifactPathConflict;
      result.driverDiagnostic = "gti: " + *collision;
      return result;
    }
    if (const auto failure =
            DebugBundlePublication::validateDestination(symbols)) {
      result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
      result.driverDiagnostic =
          "gti: failed to validate debug companion: " + *failure;
      return result;
    }
  }
  std::optional<BuildProvenance> buildProvenance;
  if (request.buildReport()) {
    if (const std::optional<std::string> diagnostic =
            loadedSourceCollisionDiagnostic(request.buildReport()->output,
                                            "build report",
                                            compilationInputs.sources)) {
      result.status = ExecutableBuildStatus::ArtifactPathConflict;
      result.driverDiagnostic = diagnostic;
      return result;
    }
    std::string provenanceError;
    std::optional<BuildSourceProvenance> source = captureBuildSourceProvenance(
        request.compilation(), compilationInputs, provenanceError);
    if (!source) {
      result.status = ExecutableBuildStatus::BuildReportCaptureFailure;
      result.driverDiagnostic =
          "gti: failed to capture build source provenance: " + provenanceError;
      return result;
    }
    buildProvenance = BuildProvenance{
        .debugInfo = request.compilation().debugInfo(),
        .rawPointerAudit = request.compilation().rawPointerAudit(),
        .rawPointerAuditLibraryFindings =
            request.compilation().rawPointerAuditLibraryFindings(),
        .compilerBuild = currentCompilerBuildIdentity(),
        .project = request.buildReport()->project,
        .source = std::move(*source),
    };
  }

  const auto prepareOutputDirectories = [&]() {
    if (!request.createParentDirectories()) {
      return true;
    }
    const std::optional<std::string> generatedDiagnostic =
        request.managedOutput()
            ? managedPathDiagnostic(*request.managedOutput(),
                                    request.generatedSource(), true)
            : createParent(request.generatedSource());
    if (generatedDiagnostic) {
      result.status = ExecutableBuildStatus::OutputDirectoryFailure;
      result.driverDiagnostic = generatedDiagnostic;
      return false;
    }
    const std::optional<std::string> outputDiagnostic =
        request.managedOutput()
            ? managedPathDiagnostic(*request.managedOutput(), request.output(),
                                    true)
            : createParent(request.output());
    if (outputDiagnostic) {
      result.status = ExecutableBuildStatus::OutputDirectoryFailure;
      result.driverDiagnostic = outputDiagnostic;
      return false;
    }
    return true;
  };

  std::optional<std::filesystem::path> cacheEntry;
  bool corruptCacheEntry = false;
  NativeLinkIdentityPlan linkIdentityPlan;
  if (request.cache()) {
    const ProgressScope cacheProgress(progress, ProgressPhase::Cache,
                                      "checking build cache");
    if (request.buildReport()) {
      result.cache.status = BuildCacheStatus::Bypassed;
      result.cache.detail =
          "build-report capture requires a real native invocation";
    } else if (const std::optional<std::string> bypassDetail =
                   nativeCacheBypassDetail(request, linkIdentityPlan)) {
      result.cache.status = BuildCacheStatus::Bypassed;
      result.cache.detail = bypassDetail;
    } else if (hasDependencyInjectingNativeEnvironment()) {
      result.cache.status = BuildCacheStatus::Bypassed;
      result.cache.detail =
          "native environment search paths require compiler/linker dependency "
          "discovery";
    } else if (!request.managedOutput()) {
      result.cache.status = BuildCacheStatus::Bypassed;
      result.cache.detail = "cache requires a managed project output policy";
    } else {
      const bool object = nativeObjectCache(request);
      const std::filesystem::path versionRoot =
          request.cache()->root / (object ? "v5" : "v4");
      const std::filesystem::path cacheProbe = versionRoot / "probe/metadata";
      if (const std::optional<std::string> diagnostic = managedPathDiagnostic(
              *request.managedOutput(), cacheProbe, false)) {
        result.status = ExecutableBuildStatus::OutputDirectoryFailure;
        result.driverDiagnostic = diagnostic;
        return result;
      }

      std::string cacheIdentityError;
      std::string cachePolicyBypass;
      const std::optional<std::string> key = buildCacheKey(
          request, compilationInputs, *request.cache(), linkIdentityPlan,
          cacheIdentityError, cachePolicyBypass);
      if (!key) {
        result.cache.status = BuildCacheStatus::Bypassed;
        result.cache.detail =
            cachePolicyBypass.empty()
                ? "identity unavailable: " + cacheIdentityError
                : cachePolicyBypass;
      } else {
        result.cache.key = *key;
        cacheEntry = versionRoot / *key;
        result.cache.entry = *cacheEntry;
        if (const std::optional<std::string> diagnostic = managedPathDiagnostic(
                *request.managedOutput(), *cacheEntry / "metadata", false)) {
          result.status = ExecutableBuildStatus::OutputDirectoryFailure;
          result.driverDiagnostic = diagnostic;
          return result;
        }

        const CacheLookupResult lookup =
            lookupCacheEntry(*cacheEntry, *key, object,
                             request.compilation().debugInfo() &&
                                 request.compilation().target().os == "macos");
        if (lookup.status == CacheLookupStatus::Hit) {
          cacheProgress.update("hit; restoring executable");
          result.cache.status = BuildCacheStatus::Hit;
          result.cache.detail = lookup.detail;
          result.resourceError = validateToolchainLayout(request.toolchain());
          if (result.resourceError) {
            result.status =
                ExecutableBuildStatus::ToolchainConfigurationFailure;
            return result;
          }
          if (!prepareOutputDirectories()) {
            return result;
          }

          result.compilation.status = CompilationStatus::Success;
          result.compilation.testCases = lookup.testCases;
          result.compilation.sources = std::move(compilationInputs.sources);
          result.compilation.diagnostics =
              std::move(compilationInputs.diagnostics);
          result.compilation.artifact =
              BackendArtifact{.kind = object ? BackendArtifactKind::Object
                                             : BackendArtifactKind::Source,
                              .contents = lookup.generated,
                              .extension = object ? ".o" : ".cpp"};
          if (request.keepGeneratedSource()) {
            std::string writeError;
            if (!publishTextAtomically(request.generatedSource(),
                                       lookup.generated, writeError)) {
              result.artifactWriteStatus = ArtifactWriteStatus::WriteFailure;
              result.driverDiagnostic = "gti: failed to restore cached "
                                        "backend representation: " +
                                        writeError;
              result.status = ExecutableBuildStatus::GeneratedArtifactFailure;
              return result;
            }
            result.artifactWriteStatus = ArtifactWriteStatus::Success;
          } else {
            std::error_code removeError;
            std::filesystem::remove(request.generatedSource(), removeError);
            if (removeError) {
              result.artifactWriteStatus = ArtifactWriteStatus::WriteFailure;
              result.driverDiagnostic = "gti: failed to remove backend "
                                        "representation after cache hit: " +
                                        removeError.message();
              result.status = ExecutableBuildStatus::GeneratedArtifactFailure;
              return result;
            }
          }

          std::string restoreError;
          if (lookup.symbols) {
            const std::filesystem::path symbols(request.output().string() +
                                                ".dSYM");
            if (const auto collision = debugBundleSourceCollision(
                    symbols, result.compilation.sources,
                    request.generatedSource())) {
              result.status = ExecutableBuildStatus::ArtifactPathConflict;
              result.driverDiagnostic = "gti: " + *collision;
              return result;
            }
            const auto stagedExecutable = stagedArtifactPath(request.output());
            TemporaryArtifact executableCleanup(stagedExecutable, true);
            const auto stagedSymbols = stagedArtifactPath(symbols);
            StagedDebugBundleCleanup symbolsCleanup{stagedSymbols};
            const auto copied = copyFileAtomically(
                *cacheEntry / "executable", stagedExecutable, restoreError,
                lookup.executableDigest, lookup.executablePermissions);
            if (!copied.succeeded() ||
                !copyTree(*lookup.symbols, stagedSymbols, restoreError)) {
              result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
              result.driverDiagnostic =
                  "gti: failed to stage cached debug pair: " + restoreError;
              return result;
            }
            DebugBundlePublication publication;
            if (const auto failure =
                    publication.publish(stagedSymbols, symbols)) {
              result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
              result.driverDiagnostic =
                  "gti: failed to publish cached debug bundle: " + *failure;
              return result;
            }
            result.artifactPublishResult =
                publishArtifact(stagedExecutable, request.output());
            if (!result.artifactPublishResult->succeeded()) {
              const auto restore = publication.rollback();
              result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
              result.driverDiagnostic =
                  "gti: failed to restore cached executable: " +
                  result.artifactPublishResult->error.message() +
                  (restore ? "; " + *restore : "");
              return result;
            }
            if (const auto warning = publication.commit())
              result.driverDiagnostic = "gti: warning: " + *warning;
          } else {
            result.artifactPublishResult = copyFileAtomically(
                *cacheEntry / "executable", request.output(), restoreError,
                lookup.executableDigest, lookup.executablePermissions);
            if (!result.artifactPublishResult->succeeded()) {
              result.driverDiagnostic =
                  "gti: failed to restore cached executable: " + restoreError;
              result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
              return result;
            }
          }
          result.generatedSourceRetained = request.keepGeneratedSource();
          result.status = ExecutableBuildStatus::Success;
          return result;
        }

        result.cache.status = BuildCacheStatus::Miss;
        result.cache.detail = lookup.detail;
        if (lookup.status == CacheLookupStatus::Corrupt) {
          corruptCacheEntry = true;
          result.cache.warning = "gti: ignored corrupt build cache entry '" +
                                 cacheEntry->string() + "': " + lookup.detail;
        }
      }
    }
  }

  result.compilation = compileToSelectedBackend(
      request.compilation(), std::move(compilationInputs), progress);
  if (!result.compilation.succeeded()) {
    if (result.compilation.backendFailureDetails) {
      result.backendDiagnosticLog =
          writeDiagnosticLog(*result.compilation.backendFailureDetails);
    }
    result.status = ExecutableBuildStatus::CompilationFailure;
    return result;
  }

  if (!prepareOutputDirectories()) {
    return result;
  }

  const BackendArtifactKind backendArtifactKind =
      result.compilation.artifact->kind;

  std::string generatedWriteError;
  if (!publishTextAtomically(request.generatedSource(),
                             result.compilation.artifact->contents,
                             generatedWriteError)) {
    result.artifactWriteStatus = ArtifactWriteStatus::WriteFailure;
    result.driverDiagnostic =
        "gti: failed to publish backend representation '" +
        request.generatedSource().string() + "': " + generatedWriteError;
    result.status = ExecutableBuildStatus::GeneratedArtifactFailure;
    return result;
  }
  result.artifactWriteStatus = ArtifactWriteStatus::Success;

  TemporaryArtifact generatedArtifact(request.generatedSource(),
                                      !request.keepGeneratedSource());
  result.resourceError = validateToolchainLayout(request.toolchain());
  if (result.resourceError) {
    result.status = ExecutableBuildStatus::ToolchainConfigurationFailure;
    return result;
  }

  const NativeToolchain nativeToolchain;
  const auto compileNativeCSource =
      [&](const std::filesystem::path &source,
          const std::filesystem::path &object, CStandard cStandard,
          const std::vector<std::filesystem::path> &includeDirectories,
          const std::vector<std::string> &arguments) {
        const ProgressScope nativeProgress(
            progress, ProgressPhase::NativeSource, source.string());
        const std::filesystem::path stagedObject = stagedArtifactPath(object);
        TemporaryArtifact stagedNativeObject(stagedObject, true);
        const NativeCCompileRequest cRequest(
            *request.cCompiler(), source, stagedObject, cStandard,
            request.compilation().optimization(), includeDirectories,
            nativeSourceArguments(arguments, request.nativeInputs(),
                                  request.compilation().debugInfo()),
            request.nativeInputs().targetPolicy);
        NativeCCompilationResult cCompilation{
            .source = source,
            .object = object,
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
          return false;
        }
        cCompilation.artifactPublishResult =
            publishArtifact(stagedObject, object);
        const bool published = cCompilation.artifactPublishResult->succeeded();
        if (!published) {
          result.driverDiagnostic =
              "gti: failed to publish native object '" + object.string() +
              "': " + cCompilation.artifactPublishResult->error.message();
        }
        result.cCompilations.push_back(std::move(cCompilation));
        if (!published) {
          generatedArtifact.keep();
          result.generatedSourceRetained = true;
          result.status = ExecutableBuildStatus::NativeObjectPublicationFailure;
          return false;
        }
        return true;
      };
  const auto compileNativeCppSource =
      [&](const std::filesystem::path &source,
          const std::filesystem::path &object,
          const std::vector<std::filesystem::path> &includeDirectories,
          const std::vector<std::string> &arguments) {
        const ProgressScope nativeProgress(
            progress, ProgressPhase::NativeSource, source.string());
        const std::filesystem::path stagedObject = stagedArtifactPath(object);
        TemporaryArtifact stagedNativeObject(stagedObject, true);
        const NativeCppCompileRequest cppRequest(
            request.nativeCompiler(), source, stagedObject,
            request.compilation().cppStandard(),
            request.compilation().optimization(), includeDirectories,
            nativeSourceArguments(arguments, request.nativeInputs(),
                                  request.compilation().debugInfo()),
            request.nativeInputs().targetPolicy);
        NativeCppCompilationResult cppCompilation{
            .source = source,
            .object = object,
            .command = nativeToolchain.command(cppRequest),
            .process = nativeToolchain.invoke(
                cppRequest, {.captureSuccessfulOutput =
                                 request.captureSuccessfulNativeOutput()}),
        };
        if (!cppCompilation.process.succeeded()) {
          result.cppCompilations.push_back(std::move(cppCompilation));
          generatedArtifact.keep();
          result.generatedSourceRetained = true;
          result.status = ExecutableBuildStatus::NativeCppCompilerFailure;
          return false;
        }
        cppCompilation.artifactPublishResult =
            publishArtifact(stagedObject, object);
        const bool published =
            cppCompilation.artifactPublishResult->succeeded();
        if (!published) {
          result.driverDiagnostic =
              "gti: failed to publish native object '" + object.string() +
              "': " + cppCompilation.artifactPublishResult->error.message();
        }
        result.cppCompilations.push_back(std::move(cppCompilation));
        if (!published) {
          generatedArtifact.keep();
          result.generatedSourceRetained = true;
          result.status = ExecutableBuildStatus::NativeObjectPublicationFailure;
          return false;
        }
        return true;
      };

  std::vector<std::filesystem::path> cIncludeDirectories{
      request.toolchain().runtimeInclude};
  append(cIncludeDirectories, request.nativeInputs().includeDirectories);
  for (std::size_t index = 0; index < cppObjectOffset; ++index) {
    if (!compileNativeCSource(
            request.nativeInputs().cSources[index], nativeObjects[index],
            request.nativeInputs().cStandard.value_or(CStandard::C17),
            cIncludeDirectories, request.nativeInputs().cCompilerArguments)) {
      return result;
    }
  }

  const std::vector<std::filesystem::path> cppIncludeDirectories =
      nativeCppIncludeDirectories(request.toolchain(), request.nativeInputs());
  for (std::size_t index = 0; index < request.nativeInputs().cppSources.size();
       ++index) {
    if (!compileNativeCppSource(request.nativeInputs().cppSources[index],
                                nativeObjects[cppObjectOffset + index],
                                cppIncludeDirectories,
                                request.nativeInputs().compilerArguments)) {
      return result;
    }
  }

  // Dependency-group sources compile with only their own include
  // directories and validated macro definitions; nothing leaks between
  // packages or into the generated translation unit.
  for (std::size_t groupIndex = 0; groupIndex < dependencyGroups.size();
       ++groupIndex) {
    const NativeDependencyGroup &group = dependencyGroups[groupIndex];
    const GroupObjectRange &range = groupObjectRanges[groupIndex];
    std::vector<std::filesystem::path> groupCIncludes{
        request.toolchain().runtimeInclude};
    append(groupCIncludes, group.includeDirectories);
    for (std::size_t index = 0; index < group.cSources.size(); ++index) {
      if (!compileNativeCSource(
              group.cSources[index], nativeObjects[range.cBegin + index],
              group.cStandard, groupCIncludes, group.cMacroDefinitions)) {
        return result;
      }
    }
    std::vector<std::filesystem::path> groupCppIncludes{
        request.toolchain().runtimeInclude};
    append(groupCppIncludes, group.includeDirectories);
    for (std::size_t index = 0; index < group.cppSources.size(); ++index) {
      if (!compileNativeCppSource(
              group.cppSources[index], nativeObjects[range.cppBegin + index],
              groupCppIncludes, group.cppMacroDefinitions)) {
        return result;
      }
    }
  }

  // The native linker derives output-identity metadata from the output
  // basename (for example the macOS ad-hoc code-signature identifier), so
  // the staged link output keeps the final filename inside a unique staging
  // directory. Staging under a uniquely named file would make otherwise
  // identical builds produce different bytes.
  const std::filesystem::path stagedOutputDirectory =
      stagedArtifactPath(request.output());
  std::error_code stagedDirectoryError;
  std::filesystem::create_directory(stagedOutputDirectory,
                                    stagedDirectoryError);
  if (stagedDirectoryError) {
    generatedArtifact.keep();
    result.generatedSourceRetained = true;
    result.driverDiagnostic =
        "gti: failed to create staged output directory '" +
        stagedOutputDirectory.string() + "': " + stagedDirectoryError.message();
    result.status = ExecutableBuildStatus::OutputDirectoryFailure;
    return result;
  }
  TemporaryArtifact stagedDirectoryCleanup(stagedOutputDirectory, true);
  const std::filesystem::path stagedOutput =
      stagedOutputDirectory / request.output().filename();
  TemporaryArtifact stagedArtifact(stagedOutput, true);
  const StagedDebugBundleCleanup stagedSymbolsCleanup{
      std::filesystem::path(stagedOutput.string() + ".dSYM")};
  const std::filesystem::path nativeDependencyFile =
      request.buildReport() &&
              backendArtifactKind == BackendArtifactKind::Source
          ? temporaryArtifactPath(request.output(), ".build-report.d")
          : std::filesystem::path{};
  TemporaryArtifact nativeDependencyCleanup(
      nativeDependencyFile,
      request.buildReport().has_value() &&
          backendArtifactKind == BackendArtifactKind::Source);
  const NativeInputs effectiveInputs = effectiveNativeInputs(
      request.toolchain(), request.nativeInputs(), nativeObjects);
  ProgressScope nativeProgress(progress, ProgressPhase::NativeLink,
                               request.nativeCompiler());
  if (backendArtifactKind == BackendArtifactKind::Source) {
    const NativeCompileRequest nativeRequest(
        request.nativeCompiler(), request.generatedSource(), stagedOutput,
        request.compilation().cppStandard(),
        request.compilation().optimization(), effectiveInputs,
        request.buildReport()
            ? std::optional<std::filesystem::path>(nativeDependencyFile)
            : std::nullopt,
        request.compilation().debugInfo());
    result.nativeCommand = nativeToolchain.command(nativeRequest);
    result.nativeProcess = nativeToolchain.invoke(
        nativeRequest,
        {.captureSuccessfulOutput = request.captureSuccessfulNativeOutput()});
  } else {
    const NativeLinkRequest nativeRequest(
        request.nativeCompiler(), request.generatedSource(), stagedOutput,
        effectiveInputs, request.compilation().debugInfo());
    result.nativeCommand = nativeToolchain.command(nativeRequest);
    result.nativeProcess = nativeToolchain.invoke(
        nativeRequest,
        {.captureSuccessfulOutput = request.captureSuccessfulNativeOutput()});
  }
  nativeProgress.finish();
  if (result.nativeProcess->driverDiagnostic) {
    result.driverDiagnostic = result.nativeProcess->driverDiagnostic;
  }
  if (!result.nativeProcess->succeeded()) {
    generatedArtifact.keep();
    result.generatedSourceRetained = true;
    result.status = ExecutableBuildStatus::NativeCompilerFailure;
    std::ostringstream evidence;
    evidence << "GTI backend build failure\nBackend: "
             << backendKindName(request.compilation().backend())
             << "\nBackend representation: "
             << (backendArtifactKind == BackendArtifactKind::Source
                     ? "source"
                     : "native object")
             << "\nStage: native compilation and linking\nRepresentation: "
             << request.generatedSource().string()
             << "\nCommand: " << renderCommand(result.nativeCommand)
             << "\nExit code: " << result.nativeProcess->exitCode << '\n';
    if (result.nativeProcess->driverDiagnostic) {
      evidence << "Driver diagnostic: "
               << *result.nativeProcess->driverDiagnostic << '\n';
    }
    evidence << "Tool output:\n" << result.nativeProcess->output;
    result.backendDiagnosticLog = writeDiagnosticLog(evidence.str());
    return result;
  }

  if (buildProvenance) {
    std::string provenanceError;
    std::vector<std::filesystem::path> dependencies;
    if (backendArtifactKind == BackendArtifactKind::Source) {
      const std::optional<std::string> depfile =
          readTextFile(nativeDependencyFile, provenanceError);
      if (!depfile) {
        generatedArtifact.keep();
        result.generatedSourceRetained = true;
        result.status = ExecutableBuildStatus::BuildReportCaptureFailure;
        result.driverDiagnostic =
            "gti: failed to read native build-report dependencies: " +
            provenanceError;
        return result;
      }
      std::optional<std::vector<std::filesystem::path>> parsedDependencies =
          parseNativeDependencyFile(*depfile, provenanceError);
      if (!parsedDependencies) {
        generatedArtifact.keep();
        result.generatedSourceRetained = true;
        result.status = ExecutableBuildStatus::BuildReportCaptureFailure;
        result.driverDiagnostic =
            "gti: failed to parse native build-report dependencies: " +
            provenanceError;
        return result;
      }
      dependencies = std::move(*parsedDependencies);
    }
    std::optional<BuildRuntimeProvenance> runtime =
        captureBuildRuntimeProvenance(request.toolchain(), dependencies,
                                      backendArtifactKind ==
                                          BackendArtifactKind::Source,
                                      provenanceError);
    const std::optional<std::filesystem::path> nativeCompiler =
        resolveExecutableForIdentity(request.nativeCompiler());
    std::optional<BuildFileProvenance> producer = captureFileProvenance(
        request.buildReport()->producerExecutable, provenanceError);
    std::optional<BuildFileProvenance> nativeCompilerIdentity =
        nativeCompiler ? captureFileProvenance(*nativeCompiler, provenanceError)
                       : std::nullopt;
    if (!runtime || !producer || !nativeCompilerIdentity) {
      generatedArtifact.keep();
      result.generatedSourceRetained = true;
      result.status = ExecutableBuildStatus::BuildReportCaptureFailure;
      if (!nativeCompiler) {
        provenanceError = "cannot resolve native compiler executable '" +
                          request.nativeCompiler() + "'";
      }
      result.driverDiagnostic =
          "gti: failed to capture build provenance: " + provenanceError;
      return result;
    }
    buildProvenance->producerExecutable = std::move(*producer);
    buildProvenance->nativeCompilerExecutable =
        std::move(*nativeCompilerIdentity);
    buildProvenance->nativeCommand = result.nativeCommand;
    buildProvenance->runtime = std::move(*runtime);
    result.provenance = std::move(buildProvenance);
  }

  DebugBundlePublication debugPublication;
  if (request.compilation().debugInfo() &&
      request.compilation().target().os == "macos") {
    const ProgressScope symbolProgress(progress, ProgressPhase::Symbols);
    const std::filesystem::path stagedSymbols(stagedOutput.string() + ".dSYM");
    const std::filesystem::path symbols(request.output().string() + ".dSYM");
    if (const auto collision = debugBundleSourceCollision(
            symbols, result.compilation.sources, request.generatedSource())) {
      result.status = ExecutableBuildStatus::ArtifactPathConflict;
      result.driverDiagnostic = "gti: " + *collision;
      return result;
    }
    std::error_code symbolError;
    if (!std::filesystem::is_directory(stagedSymbols, symbolError)) {
      // Clang normally creates this during its compile/link invocation. A
      // native toolchain that does not must provide the standard DWARF tool.
      const ProcessResult symbolProcess = invokeProcess(
          {"dsymutil", stagedOutput.string(), "-o", stagedSymbols.string()},
          {.captureSuccessfulOutput = request.captureSuccessfulNativeOutput()});
      if (!symbolProcess.succeeded()) {
        generatedArtifact.keep();
        result.generatedSourceRetained = true;
        result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
        result.driverDiagnostic =
            "gti: failed to collect native debug symbols: " +
            symbolProcess.output;
        return result;
      }
    }
    if (const auto diagnostic =
            debugPublication.publish(stagedSymbols, symbols)) {
      generatedArtifact.keep();
      result.generatedSourceRetained = true;
      result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
      result.driverDiagnostic = "gti: failed to publish debug bundle '" +
                                symbols.string() + "': " + *diagnostic;
      return result;
    }
  }

  const ProgressScope publicationProgress(
      progress, ProgressPhase::Publishing,
      cacheEntry && !result.cache.key.empty() ? "executable and build cache"
                                              : "executable");
  result.artifactPublishResult =
      publishArtifact(stagedOutput, request.output());
  if (!result.artifactPublishResult->succeeded()) {
    const auto restore = debugPublication.rollback();
    generatedArtifact.keep();
    result.generatedSourceRetained = true;
    result.driverDiagnostic =
        "gti: failed to publish executable '" + request.output().string() +
        "': " + result.artifactPublishResult->error.message() +
        (restore ? "; " + *restore : "");
    result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
    return result;
  }

  if (const auto warning = debugPublication.commit())
    result.driverDiagnostic = "gti: warning: " + *warning;

  // The produced artifact's identity is captured from the published
  // executable, so the record describes the exact bytes a consumer holds
  // rather than the staged copy's path.
  if (result.provenance) {
    std::string artifactError;
    std::optional<BuildFileProvenance> artifact =
        captureFileProvenance(request.output(), artifactError);
    if (!artifact) {
      result.status = ExecutableBuildStatus::BuildReportCaptureFailure;
      result.driverDiagnostic =
          "gti: failed to capture built artifact provenance: " + artifactError;
      return result;
    }
    result.provenance->artifact = std::move(*artifact);
  }

  if (cacheEntry && !result.cache.key.empty()) {
    std::string cacheStoreError;
    if (storeCacheEntry(request, *cacheEntry, result.cache.key,
                        *result.compilation.artifact,
                        result.compilation.testCases, cacheStoreError)) {
      if (corruptCacheEntry) {
        result.cache.status = BuildCacheStatus::RecoveredCorruption;
        result.cache.detail =
            "replaced a corrupt entry after a successful rebuild";
      }
    } else {
      result.cache.warning =
          "gti: build succeeded but cache publication failed for '" +
          cacheEntry->string() + "': " + cacheStoreError;
      if (corruptCacheEntry) {
        result.cache.status = BuildCacheStatus::Bypassed;
      }
    }
  }

  result.generatedSourceRetained = request.keepGeneratedSource();
  result.status = ExecutableBuildStatus::Success;
  return result;
}

} // namespace

ExecutableBuildResult buildExecutable(const ExecutableBuildRequest &request,
                                      ProgressObserver *progress) {
  ExecutableBuildResult result;
  result.generatedSource = request.generatedSource();
  if (nativeObjectCache(request)) {
    const auto configured = [](const char *name) {
      const char *value = std::getenv(name);
      return value != nullptr && *value != '\0';
    };
    const auto &policy = request.nativeInputs().targetPolicy;
    const auto hasDeploymentArgument = [](const auto &arguments) {
      return std::any_of(arguments.begin(), arguments.end(),
                         darwinDeploymentArgument);
    };
    const bool darwinPolicy =
        request.compilation().target().os == "macos" &&
        (configured("MACOSX_DEPLOYMENT_TARGET") || configured("SDKROOT") ||
         configured("DEVELOPER_DIR") ||
         hasDeploymentArgument(request.nativeInputs().linkerArguments) ||
         hasDeploymentArgument(request.nativeInputs().trailingArguments));
    if (!policy.triple.empty() || !policy.sysroot.empty() || darwinPolicy) {
      result.status = ExecutableBuildStatus::ToolchainConfigurationFailure;
      result.driverDiagnostic =
          "gti: explicit LLVM native target, sysroot or deployment policy "
          "requires coordinated backend target options and a matching "
          "target-built runtime; the current profile supports implicit host "
          "policy only";
      if (request.cache()) {
        result.cache.status = BuildCacheStatus::Bypassed;
        result.cache.detail = "LLVM native target/runtime identity unavailable";
      }
      return result;
    }
  }

  const auto reject = [&](std::string reason) {
    result.status = ExecutableBuildStatus::ToolchainConfigurationFailure;
    result.driverDiagnostic = "gti: " + std::move(reason);
    if (request.cache()) {
      result.cache.status = BuildCacheStatus::Bypassed;
      result.cache.detail = "LLVM instrumentation configuration unavailable";
    }
    return result;
  };
  if (request.nativeInputs().instrumentation)
    return reject(
        "native instrumentation plans are resolved by the build driver");
  const auto mode = request.compilation().llvmInstrumentation();
  if (mode == LlvmInstrumentationMode::None)
    return buildResolvedExecutable(request, progress);
  const auto &native = request.nativeInputs();
  if (!native.compilerArguments.empty() || !native.cCompilerArguments.empty() ||
      !native.linkerArguments.empty() || !native.trailingArguments.empty())
    return reject("LLVM instrumentation requires structured native arguments; "
                  "opaque compiler/linker policy is not admitted");
  const auto externalLink = [](const NativeDependencyGroup &group) {
    return !group.linkOperands.empty() || !group.libraryDirectories.empty();
  };
  if (!native.finalLibraryFiles.empty() || !native.libraryFiles.empty() ||
      !native.libraries.empty() || !native.frameworks.empty() ||
      !native.orderedLinkOperands.empty() ||
      !native.libraryDirectories.empty() ||
      std::any_of(native.dependencyGroups.begin(),
                  native.dependencyGroups.end(), externalLink))
    return reject("LLVM diagnostic native link dependencies need matching "
                  "instrumentation provenance before admission");
  if (mode == LlvmInstrumentationMode::Thread)
    return reject("LLVM Thread instrumentation requires verified communicating "
                  "GTI runtime and native-provider instrumentation provenance");
  if (request.buildReport())
    return reject("LLVM instrumented build reports require a matching recorded "
                  "policy and verifier replay contract");
  std::string reason;
  // This is a toolchain-only query of the already implemented default policy,
  // not an identity for the requested instrumented object. Full object policy
  // is independently preflighted below and used by generation/cache lookup.
  const auto toolchain =
      queryLlvmBackendIdentity(request.compilation().target(), {}, reason);
  if (!toolchain)
    return reject(reason);
  auto runtime = resolveLlvmNativeRuntime(request.nativeCompiler(), *toolchain,
                                          mode, reason);
  if (!runtime)
    return reject(reason);
  const bool cSources =
      !native.cSources.empty() ||
      std::any_of(native.dependencyGroups.begin(),
                  native.dependencyGroups.end(),
                  [](const auto &group) { return !group.cSources.empty(); });
  if (cSources) {
    if (!request.cCompiler())
      return reject(
          "instrumented native C sources require an explicit matching Clang");
    const auto cRuntime = resolveLlvmNativeRuntime(*request.cCompiler(),
                                                   *toolchain, mode, reason);
    if (!cRuntime || cRuntime->runtimeDirectory != runtime->runtimeDirectory ||
        cRuntime->compilerVersion != runtime->compilerVersion)
      return reject(
          "native C compiler-rt does not match the selected C++/LLVM runtime");
    IdentityBuilder combined;
    combined.add("native-cxx.runtime", runtime->identity);
    combined.add("native-cc.runtime", cRuntime->identity);
    runtime->identity = combined.finish();
    append(runtime->implementationFiles, cRuntime->implementationFiles);
    std::sort(runtime->implementationFiles.begin(),
              runtime->implementationFiles.end());
    runtime->implementationFiles.erase(
        std::unique(runtime->implementationFiles.begin(),
                    runtime->implementationFiles.end()),
        runtime->implementationFiles.end());
  }
  result.nativeInstrumentation = *runtime;
  const auto object = queryLlvmBackendIdentity(
      request.compilation().target(), llvmBackendOptions(request.compilation()),
      reason);
  if (!object)
    return reject(reason);
  ExecutableBuildRequest resolved = request;
  resolved.additionalNativeInputs.instrumentation = std::move(runtime);
  return buildResolvedExecutable(resolved, progress);
}

} // namespace lang::driver
