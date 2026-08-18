#include "gti/driver/build.h"

#include "sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lang::driver {
namespace {

constexpr std::string_view cacheSchema = "gti-build-cache-v2";

class IdentityBuilder final {
public:
  void add(std::string_view label, std::string_view value) {
    appendFramed(label);
    appendFramed(value);
  }

  void add(std::string_view label, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[bytes.size() - index - 1] =
          static_cast<char>((value >> (index * 8U)) & 0xffU);
    }
    appendFramed(label);
    appendFramed(std::string_view(bytes.data(), bytes.size()));
  }

  [[nodiscard]] std::string finish() const { return hash.finishHex(); }

private:
  void appendFramed(std::string_view value) {
    std::array<char, 8> size{};
    const std::uint64_t length = value.size();
    for (std::size_t index = 0; index < size.size(); ++index) {
      size[size.size() - index - 1] =
          static_cast<char>((length >> (index * 8U)) & 0xffU);
    }
    hash.update(std::string_view(size.data(), size.size()));
    hash.update(value);
  }

  Sha256 hash;
};

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

bool addCompilerIdentity(IdentityBuilder &identity, std::string_view label,
                         const std::string &compiler,
                         std::string &errorMessage) {
  identity.add(std::string(label) + ".command", compiler);
  const std::optional<std::filesystem::path> executable =
      resolveExecutableForIdentity(compiler);
  if (!executable) {
    errorMessage = "cannot resolve compiler executable '" + compiler + "'";
    return false;
  }
  if (!addFileIdentity(identity, std::string(label) + ".executable",
                       *executable, true, errorMessage)) {
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
  identity.add(std::string(label) + ".version", version.output);
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

bool addSourceIdentity(IdentityBuilder &identity,
                       const CompilationInputs &inputs,
                       const std::filesystem::path &sourceRoot,
                       std::string &errorMessage) {
  struct UnitRecord {
    const SourceUnit *unit = nullptr;
    std::string logicalName;
  };
  std::vector<UnitRecord> units;
  units.reserve(inputs.sourceGraph.sourceUnits().size());
  std::vector<std::string> namesById(inputs.sourceGraph.sourceUnits().size() +
                                     1);
  for (const SourceUnit &unit : inputs.sourceGraph.sourceUnits()) {
    const std::optional<std::string> name = logicalSourceName(unit, sourceRoot);
    if (!name) {
      errorMessage = "cannot derive a logical source name for '" +
                     unit.path.string() + "'";
      return false;
    }
    if (unit.id >= namesById.size()) {
      errorMessage = "source graph contains an invalid unit identifier";
      return false;
    }
    namesById[unit.id] = *name;
    units.push_back({.unit = &unit, .logicalName = *name});
  }
  std::sort(units.begin(), units.end(),
            [](const UnitRecord &left, const UnitRecord &right) {
              return left.logicalName < right.logicalName;
            });
  for (std::size_t index = 1; index < units.size(); ++index) {
    if (units[index - 1].logicalName == units[index].logicalName) {
      errorMessage = "source graph contains duplicate logical unit '" +
                     units[index].logicalName + "'";
      return false;
    }
  }

  identity.add("source.units", static_cast<std::uint64_t>(units.size()));
  for (const UnitRecord &record : units) {
    const std::string *source = inputs.sources.find(record.unit->path.string());
    if (source == nullptr) {
      errorMessage = "loaded source text is missing for '" +
                     record.unit->path.string() + "'";
      return false;
    }
    identity.add("source.name", record.logicalName);
    identity.add("source.role", static_cast<std::uint64_t>(record.unit->role));
    identity.add("source.entry",
                 static_cast<std::uint64_t>(record.unit->entry));
    identity.add("source.prelude",
                 static_cast<std::uint64_t>(record.unit->prelude));
    identity.add("source.size", static_cast<std::uint64_t>(source->size()));
    identity.add("source.sha256", digestText(*source));
  }

  const std::vector<SourceUnitId> &preludeRoots =
      inputs.sourceGraph.preludeRoots();
  identity.add("source.prelude-roots",
               static_cast<std::uint64_t>(preludeRoots.size()));
  for (const SourceUnitId root : preludeRoots) {
    if (root == 0 || root >= namesById.size() || namesById[root].empty()) {
      errorMessage = "source graph contains an invalid prelude root";
      return false;
    }
    const SourceUnit *unit = inputs.sourceGraph.findUnit(root);
    if (unit == nullptr ||
        (unit->role != SourceUnitRole::Prelude && !unit->prelude)) {
      errorMessage = "source graph prelude root does not name a prelude unit";
      return false;
    }
    identity.add("source.prelude-root", namesById[root]);
  }

  std::vector<std::string> edges;
  edges.reserve(inputs.sourceGraph.dependencyEdges().size());
  for (const SourceDependency &dependency :
       inputs.sourceGraph.dependencyEdges()) {
    if (dependency.source >= namesById.size() ||
        dependency.target >= namesById.size() ||
        namesById[dependency.source].empty() ||
        namesById[dependency.target].empty()) {
      errorMessage = "source graph contains an invalid dependency edge";
      return false;
    }
    edges.push_back(namesById[dependency.source] + "\n" +
                    namesById[dependency.target] + "\n" +
                    std::to_string(static_cast<unsigned>(dependency.kind)));
  }
  std::sort(edges.begin(), edges.end());
  identity.add("source.edges", static_cast<std::uint64_t>(edges.size()));
  for (const std::string &edge : edges) {
    identity.add("source.edge", edge);
  }
  return true;
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

std::vector<std::filesystem::path>
nativeCppIncludeDirectories(const ToolchainLayout &toolchain,
                            CppStandard standard,
                            const NativeInputs &additional);

// Runs one native preprocessor probe and folds the compiler's own dependency
// report into the cache identity: every file the preprocessor opened joins
// with content identity, and the preprocessed translation unit itself is
// hashed so include resolution changes (for example a newly added shadowing
// header) always change the key. Policy violations set `policyBypassDetail`;
// mechanical failures set `errorMessage`.
bool addNativeSourceDiscovery(IdentityBuilder &identity, std::string_view label,
                              const std::vector<std::string> &probeCommand,
                              const std::filesystem::path &source,
                              const std::filesystem::path &preprocessed,
                              const std::filesystem::path &depfile,
                              std::string &errorMessage,
                              std::string &policyBypassDetail) {
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

  identity.add(std::string(label) + ".source",
               normalizedAbsolute(source).generic_string());
  identity.add(std::string(label) + ".deps",
               static_cast<std::uint64_t>(discovered.size()));
  for (const std::string &dependency : discovered) {
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
    identity.add(std::string(label) + ".dep.path", dependency);
    identity.add(std::string(label) + ".dep.size",
                 static_cast<std::uint64_t>(contents->size()));
    identity.add(std::string(label) + ".dep.sha256", digestText(*contents));
  }

  const std::optional<FileDigest> preprocessedDigest =
      digestFile(preprocessed, errorMessage);
  if (!preprocessedDigest) {
    return false;
  }
  identity.add(std::string(label) + ".preprocessed.size",
               static_cast<std::uint64_t>(preprocessedDigest->size));
  identity.add(std::string(label) + ".preprocessed.sha256",
               preprocessedDigest->hash);
  return true;
}

std::optional<std::string> buildCacheKey(const ExecutableBuildRequest &request,
                                         const CompilationInputs &inputs,
                                         const BuildCachePolicy &policy,
                                         std::string &errorMessage,
                                         std::string &policyBypassDetail) {
  IdentityBuilder identity;
  identity.add("cache.schema", cacheSchema);
  identity.add("compiler.identity", policy.compilerIdentity);
  identity.add("project-model.identity", policy.projectModelIdentity);
  constexpr std::array<std::string_view, 17> nativeEnvironment{
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
  identity.add("backend.standard",
               static_cast<std::uint64_t>(request.compilation().cppStandard()));
  identity.add("optimization", static_cast<std::uint64_t>(
                                   request.compilation().optimization()));

  const TargetInfo &target = request.compilation().target();
  identity.add("target.os", target.os);
  identity.add("target.vendor", target.vendor);
  identity.add("target.arch", target.arch);
  identity.add("target.execution-profile",
               static_cast<std::uint64_t>(target.executionProfile));
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

  if (!addCompilerIdentity(identity, "native-cxx", request.nativeCompiler(),
                           errorMessage)) {
    return std::nullopt;
  }
  if (!request.nativeInputs().cSources.empty()) {
    if (!request.cCompiler() || request.cCompiler()->empty()) {
      errorMessage = "native C inputs have no selected C compiler";
      return std::nullopt;
    }
    if (!addCompilerIdentity(identity, "native-cc", *request.cCompiler(),
                             errorMessage)) {
      return std::nullopt;
    }
  }

  if (!addDirectoryIdentity(identity, "toolchain.runtime-include",
                            request.toolchain().runtimeInclude, false,
                            errorMessage) ||
      !addFileIdentity(identity, "toolchain.runtime-library",
                       request.toolchain().runtimeLibrary, false,
                       errorMessage)) {
    return std::nullopt;
  }
  if (request.compilation().cppStandard() == CppStandard::Cpp20 &&
      !addDirectoryIdentity(identity, "toolchain.vendor-include",
                            request.toolchain().vendorInclude, false,
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

  if (!native.cSources.empty() || !native.cppSources.empty()) {
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
    std::vector<std::filesystem::path> cIncludeDirectories{
        request.toolchain().runtimeInclude};
    append(cIncludeDirectories, native.includeDirectories);
    const std::vector<std::filesystem::path> cppIncludeDirectories =
        nativeCppIncludeDirectories(
            request.toolchain(), request.compilation().cppStandard(), native);
    const auto discover = [&](const std::filesystem::path &source,
                              std::string_view label, bool cSource) {
      const std::filesystem::path preprocessed = stagedArtifactPath(
          temporaryRoot / (source.stem().string() + ".gti-discovery.i"));
      const std::filesystem::path depfile = stagedArtifactPath(
          temporaryRoot / (source.stem().string() + ".gti-discovery.d"));
      TemporaryArtifact preprocessedCleanup(preprocessed, true);
      TemporaryArtifact depfileCleanup(depfile, true);
      const std::vector<std::string> probeCommand =
          cSource ? nativeToolchain.preprocessCommand(
                        NativeCCompileRequest(
                            *request.cCompiler(), source, preprocessed,
                            native.cStandard.value_or(CStandard::C17),
                            request.compilation().optimization(),
                            cIncludeDirectories, native.cCompilerArguments),
                        depfile)
                  : nativeToolchain.preprocessCommand(
                        NativeCppCompileRequest(
                            request.nativeCompiler(), source, preprocessed,
                            request.compilation().cppStandard(),
                            request.compilation().optimization(),
                            cppIncludeDirectories, native.compilerArguments),
                        depfile);
      return addNativeSourceDiscovery(identity, label, probeCommand, source,
                                      preprocessed, depfile, errorMessage,
                                      policyBypassDetail);
    };
    for (const std::filesystem::path &source : native.cSources) {
      if (!discover(source, "native.c-source.discovery", true)) {
        return std::nullopt;
      }
    }
    for (const std::filesystem::path &source : native.cppSources) {
      if (!discover(source, "native.cpp-source.discovery", false)) {
        return std::nullopt;
      }
    }
  }
  return identity.finish();
}

struct CacheMetadata {
  std::string key;
  FileDigest generated;
  FileDigest executable;
  std::uint32_t executablePermissions = 0;
};

bool validDigest(std::string_view digest) {
  return digest.size() == 64 &&
         std::all_of(digest.begin(), digest.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::optional<CacheMetadata>
readCacheMetadata(const std::filesystem::path &path,
                  std::string &errorMessage) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    errorMessage = "metadata is missing or unreadable";
    return std::nullopt;
  }

  std::string schema;
  CacheMetadata metadata;
  std::string generatedLabel;
  std::string executableLabel;
  if (!(input >> schema >> metadata.key >> generatedLabel >>
        metadata.generated.hash >> metadata.generated.size >> executableLabel >>
        metadata.executable.hash >> metadata.executable.size >>
        metadata.executablePermissions) ||
      schema != cacheSchema || generatedLabel != "generated" ||
      executableLabel != "executable" || !validDigest(metadata.key) ||
      !validDigest(metadata.generated.hash) ||
      !validDigest(metadata.executable.hash)) {
    errorMessage = "metadata has an invalid schema or digest";
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
  return std::string(cacheSchema) + "\n" + metadata.key + "\n" + "generated " +
         metadata.generated.hash + " " +
         std::to_string(metadata.generated.size) + "\n" + "executable " +
         metadata.executable.hash + " " +
         std::to_string(metadata.executable.size) + " " +
         std::to_string(metadata.executablePermissions) + "\n";
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
  std::string detail;
};

std::optional<std::string> readTextFile(const std::filesystem::path &path,
                                        std::string &errorMessage) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    errorMessage = "cannot read '" + path.string() + "'";
    return std::nullopt;
  }
  std::string contents{std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>()};
  if (input.bad()) {
    errorMessage = "cannot finish reading '" + path.string() + "'";
    return std::nullopt;
  }
  return contents;
}

CacheLookupResult lookupCacheEntry(const std::filesystem::path &entry,
                                   std::string_view expectedKey) {
  const std::filesystem::path metadataPath = entry / "metadata";
  const std::filesystem::path generatedPath = entry / "generated.cpp";
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
  const std::optional<CacheMetadata> metadata =
      readCacheMetadata(metadataPath, detail);
  if (!metadata) {
    return {.status = CacheLookupStatus::Corrupt, .detail = std::move(detail)};
  }
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
            .detail = "generated C++ digest does not match metadata"};
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
  return {.status = CacheLookupStatus::Hit,
          .generated = std::move(*generated),
          .executableDigest = *executableDigest,
          .executablePermissions = executablePermissions,
          .detail = "verified generated C++ and executable digests"};
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
                     std::string_view generated, std::string &errorMessage) {
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

  const std::filesystem::path generatedPath = entry / "generated.cpp";
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

  const CacheMetadata metadata{
      .key = std::string(key),
      .generated = generatedDigest,
      .executable = *executableDigest,
      .executablePermissions =
          static_cast<std::uint32_t>(executablePermissions)};
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

// Returns the reason a native configuration must bypass the cache, or
// nullopt when every native input can join the identity exactly. Declared C
// and C++ sources and include directories are cacheable through dependency
// discovery and full-tree identity; the remaining bypasses are inputs the
// driver cannot resolve to exact content identities. Narrowing a bypass here
// must never widen what the key trusts: an unidentifiable input bypasses.
std::optional<std::string> nativeCacheBypassDetail(const NativeInputs &inputs) {
  if (!inputs.compilerArguments.empty() || !inputs.cCompilerArguments.empty() ||
      !inputs.linkerArguments.empty() || !inputs.trailingArguments.empty()) {
    return "opaque native argument vectors can introduce undeclared "
           "compiler and linker inputs";
  }
  const bool namedOperands =
      !inputs.libraries.empty() || !inputs.frameworks.empty() ||
      std::any_of(inputs.orderedLinkOperands.begin(),
                  inputs.orderedLinkOperands.end(),
                  [](const NativeLinkOperand &operand) {
                    return operand.kind != NativeLinkOperandKind::File;
                  });
  if (namedOperands) {
    return "name-resolved libraries and frameworks require linker search "
           "resolution";
  }
  if (!inputs.libraryDirectories.empty()) {
    return "native library search directories require linker search "
           "resolution";
  }

  std::vector<std::filesystem::path> linkFiles = inputs.libraryFiles;
  for (const NativeLinkOperand &operand : inputs.orderedLinkOperands) {
    linkFiles.emplace_back(operand.value);
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
  return std::nullopt;
}

bool hasDependencyInjectingNativeEnvironment() {
  // These variables name mutable search roots or toolchain resources. Hashing
  // only the variable spelling cannot identify headers, linker scripts,
  // archives, plugins, or sub-tools reached through the named location.
  constexpr std::array<std::string_view, 13> variables{
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
                            CppStandard standard,
                            const NativeInputs &additional) {
  std::vector<std::filesystem::path> directories{toolchain.runtimeInclude};
  if (standard == CppStandard::Cpp20 &&
      toolchain.vendorInclude != toolchain.runtimeInclude) {
    directories.push_back(toolchain.vendorInclude);
  }
  append(directories, additional.includeDirectories);
  return directories;
}

NativeInputs
effectiveNativeInputs(const ToolchainLayout &toolchain, CppStandard standard,
                      const NativeInputs &additional,
                      const std::vector<std::filesystem::path> &nativeObjects) {
  NativeInputs inputs;
  inputs.includeDirectories =
      nativeCppIncludeDirectories(toolchain, standard, additional);
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

bool containsTimeSensitivePreprocessorUse(std::string_view text) {
  return text.find("__DATE__") != std::string_view::npos ||
         text.find("__TIME__") != std::string_view::npos ||
         text.find("__TIMESTAMP__") != std::string_view::npos;
}

ExecutableBuildRequest::ExecutableBuildRequest(
    CompilationRequest compilation, ToolchainLayout toolchain,
    std::filesystem::path generatedSource, std::filesystem::path output,
    std::string nativeCompiler, NativeInputs nativeInputs,
    bool keepGeneratedSource, bool createParentDirectories,
    bool captureSuccessfulNativeOutput,
    std::optional<ManagedOutputPolicy> managedOutput,
    std::optional<std::string> cCompiler, std::optional<BuildCachePolicy> cache)
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
      managedOutputPolicy(std::move(managedOutput)),
      buildCachePolicy(std::move(cache)) {}

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

ExecutableBuildResult buildExecutable(const ExecutableBuildRequest &request) {
  ExecutableBuildResult result;
  result.generatedSource = request.generatedSource();
  std::vector<std::filesystem::path> nativeObjects;
  nativeObjects.reserve(request.nativeInputs().cSources.size() +
                        request.nativeInputs().cppSources.size());
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
  if (!request.nativeInputs().cSources.empty() &&
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
      loadCompilationInputs(request.compilation());
  if (!compilationInputs.succeeded()) {
    result.compilation =
        compileToCpp(request.compilation(), std::move(compilationInputs));
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
                                          "generated C++ output",
                                          compilationInputs.sources)) {
    result.status = ExecutableBuildStatus::ArtifactPathConflict;
    result.driverDiagnostic = diagnostic;
    return result;
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
  if (request.cache()) {
    if (const std::optional<std::string> bypassDetail =
            nativeCacheBypassDetail(request.nativeInputs())) {
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
      const std::filesystem::path versionRoot = request.cache()->root / "v2";
      const std::filesystem::path cacheProbe = versionRoot / "probe/metadata";
      if (const std::optional<std::string> diagnostic = managedPathDiagnostic(
              *request.managedOutput(), cacheProbe, false)) {
        result.status = ExecutableBuildStatus::OutputDirectoryFailure;
        result.driverDiagnostic = diagnostic;
        return result;
      }

      std::string cacheIdentityError;
      std::string cachePolicyBypass;
      const std::optional<std::string> key =
          buildCacheKey(request, compilationInputs, *request.cache(),
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

        const CacheLookupResult lookup = lookupCacheEntry(*cacheEntry, *key);
        if (lookup.status == CacheLookupStatus::Hit) {
          result.cache.status = BuildCacheStatus::Hit;
          result.cache.detail = lookup.detail;
          result.resourceError = validateToolchainLayout(
              request.toolchain(), request.compilation().cppStandard());
          if (result.resourceError) {
            result.status =
                ExecutableBuildStatus::ToolchainConfigurationFailure;
            return result;
          }
          if (!prepareOutputDirectories()) {
            return result;
          }

          result.compilation.status = CompilationStatus::Success;
          result.compilation.sources = std::move(compilationInputs.sources);
          result.compilation.artifact =
              BackendArtifact{.kind = BackendArtifactKind::Source,
                              .contents = lookup.generated,
                              .extension = ".cpp"};
          if (request.keepGeneratedSource()) {
            std::string writeError;
            if (!publishTextAtomically(request.generatedSource(),
                                       lookup.generated, writeError)) {
              result.artifactWriteStatus = ArtifactWriteStatus::WriteFailure;
              result.driverDiagnostic = "gti: failed to restore cached "
                                        "generated C++: " +
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
              result.driverDiagnostic =
                  "gti: failed to remove generated C++ after cache hit: " +
                  removeError.message();
              result.status = ExecutableBuildStatus::GeneratedArtifactFailure;
              return result;
            }
          }

          std::string restoreError;
          result.artifactPublishResult = copyFileAtomically(
              *cacheEntry / "executable", request.output(), restoreError,
              lookup.executableDigest, lookup.executablePermissions);
          if (!result.artifactPublishResult->succeeded()) {
            result.driverDiagnostic =
                "gti: failed to restore cached executable: " + restoreError;
            result.status = ExecutableBuildStatus::ArtifactPublicationFailure;
            return result;
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

  result.compilation =
      compileToCpp(request.compilation(), std::move(compilationInputs));
  if (!result.compilation.succeeded()) {
    result.status = ExecutableBuildStatus::CompilationFailure;
    return result;
  }

  if (!prepareOutputDirectories()) {
    return result;
  }

  std::string generatedWriteError;
  if (!publishTextAtomically(request.generatedSource(),
                             result.compilation.artifact->contents,
                             generatedWriteError)) {
    result.artifactWriteStatus = ArtifactWriteStatus::WriteFailure;
    result.driverDiagnostic = "gti: failed to publish generated C++ '" +
                              request.generatedSource().string() +
                              "': " + generatedWriteError;
    result.status = ExecutableBuildStatus::GeneratedArtifactFailure;
    return result;
  }
  result.artifactWriteStatus = ArtifactWriteStatus::Success;

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
  for (std::size_t index = 0; index < cppObjectOffset; ++index) {
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

  const std::vector<std::filesystem::path> cppIncludeDirectories =
      nativeCppIncludeDirectories(request.toolchain(),
                                  request.compilation().cppStandard(),
                                  request.nativeInputs());
  for (std::size_t index = 0; index < request.nativeInputs().cppSources.size();
       ++index) {
    const std::filesystem::path &nativeObject =
        nativeObjects[cppObjectOffset + index];
    const std::filesystem::path stagedObject = stagedArtifactPath(nativeObject);
    TemporaryArtifact stagedNativeObject(stagedObject, true);
    const NativeCppCompileRequest cppRequest(
        request.nativeCompiler(), request.nativeInputs().cppSources[index],
        stagedObject, request.compilation().cppStandard(),
        request.compilation().optimization(), cppIncludeDirectories,
        request.nativeInputs().compilerArguments);
    NativeCppCompilationResult cppCompilation{
        .source = request.nativeInputs().cppSources[index],
        .object = nativeObject,
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
      return result;
    }
    cppCompilation.artifactPublishResult =
        publishArtifact(stagedObject, nativeObject);
    const bool published = cppCompilation.artifactPublishResult->succeeded();
    if (!published) {
      result.driverDiagnostic =
          "gti: failed to publish native object '" + nativeObject.string() +
          "': " + cppCompilation.artifactPublishResult->error.message();
    }
    result.cppCompilations.push_back(std::move(cppCompilation));
    if (!published) {
      generatedArtifact.keep();
      result.generatedSourceRetained = true;
      result.status = ExecutableBuildStatus::NativeObjectPublicationFailure;
      return result;
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

  if (cacheEntry && !result.cache.key.empty()) {
    std::string cacheStoreError;
    if (storeCacheEntry(request, *cacheEntry, result.cache.key,
                        result.compilation.artifact->contents,
                        cacheStoreError)) {
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

} // namespace lang::driver
