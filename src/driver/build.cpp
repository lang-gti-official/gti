#include "gti/driver/build.h"

#include "build_source_identity.h"
#include "gti/build_identity_config.h"
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

} // namespace

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
    const std::optional<std::string> name = logicalSourceName(unit, sourceRoot);
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

std::vector<std::filesystem::path>
nativeCppIncludeDirectories(const ToolchainLayout &toolchain,
                            CppStandard standard,
                            const NativeInputs &additional);

struct ResolvedNativeLibrary {
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
};

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
                                         const NativeLinkIdentityPlan &linkPlan,
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
  for (const std::string &flag : request.compilation().configurationFlags()) {
    identity.add("configuration.flag", flag);
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

  if (!addCompilerIdentity(identity, "native-cxx", request.nativeCompiler(),
                           errorMessage)) {
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
          const std::vector<std::string> probeCommand =
              cSource ? nativeToolchain.preprocessCommand(
                            NativeCCompileRequest(
                                *request.cCompiler(), source, preprocessed,
                                cStandard, request.compilation().optimization(),
                                includeDirectories, arguments,
                                native.targetPolicy),
                            depfile)
                      : nativeToolchain.preprocessCommand(
                            NativeCppCompileRequest(
                                request.nativeCompiler(), source, preprocessed,
                                request.compilation().cppStandard(),
                                request.compilation().optimization(),
                                includeDirectories, arguments,
                                native.targetPolicy),
                            depfile);
          return addNativeSourceDiscovery(identity, label, probeCommand, source,
                                          preprocessed, depfile, errorMessage,
                                          policyBypassDetail);
        };

    std::vector<std::filesystem::path> cIncludeDirectories{
        request.toolchain().runtimeInclude};
    append(cIncludeDirectories, native.includeDirectories);
    const std::vector<std::filesystem::path> cppIncludeDirectories =
        nativeCppIncludeDirectories(
            request.toolchain(), request.compilation().cppStandard(), native);
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
      if (request.compilation().cppStandard() == CppStandard::Cpp20 &&
          request.toolchain().vendorInclude !=
              request.toolchain().runtimeInclude) {
        groupCppIncludes.push_back(request.toolchain().vendorInclude);
      }
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
nativeCacheBypassDetail(const NativeInputs &inputs,
                        NativeLinkIdentityPlan &plan) {
  plan = {};
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
  bool namedFramework = false;
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
            namedFramework = true;
            break;
          }
        }
      };
  if (inputs.orderedLinkOperands.empty() && !groupLinkContribution) {
    linkFiles = inputs.libraryFiles;
    namedLibraries = inputs.libraries;
    namedFramework = !inputs.frameworks.empty();
  } else {
    collectOperands(inputs.orderedLinkOperands);
    for (const NativeDependencyGroup &group : inputs.dependencyGroups) {
      collectOperands(group.linkOperands);
    }
  }

  if (namedFramework) {
    // A framework resolves through undeclared `-F` and system framework
    // directories to a bundle whose binary, headers, and re-exported
    // dependencies are not determined by any declared path.
    return "name-resolved frameworks require framework search resolution";
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
  // The resolved cross-compilation policy is driver-owned and must survive
  // link-input assembly: without it the final link would silently run the
  // declared cross compiler in its host configuration.
  inputs.targetPolicy = additional.targetPolicy;
  inputs.includeDirectories =
      nativeCppIncludeDirectories(toolchain, standard, additional);
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
    // Dependency link contributions follow the application's own, in the
    // composed dependents-before-dependencies order, so a dependent's
    // libraries may resolve symbols from the packages it depends on.
    for (const NativeDependencyGroup &group : additional.dependencyGroups) {
      append(inputs.orderedLinkOperands, group.linkOperands);
    }
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

std::optional<BuildRuntimeProvenance> captureBuildRuntimeProvenance(
    const ToolchainLayout &toolchain,
    const std::vector<std::filesystem::path> &dependencies,
    std::string &errorMessage) {
  BuildRuntimeProvenance provenance;
  const std::filesystem::path runtimeRoot =
      normalizedAbsolute(toolchain.runtimeInclude / "gti");
  const std::filesystem::path compatibilityRoot =
      normalizedAbsolute(toolchain.vendorInclude / "nonstd");
  std::vector<std::string> seen;
  for (const std::filesystem::path &dependency : dependencies) {
    const std::filesystem::path canonical = normalizedAbsolute(dependency);
    BuildRuntimeHeaderKind kind;
    std::filesystem::path relative;
    if (pathIsWithin(runtimeRoot, canonical)) {
      kind = BuildRuntimeHeaderKind::Runtime;
      relative = std::filesystem::path("gti") /
                 canonical.lexically_relative(runtimeRoot);
    } else if (pathIsWithin(compatibilityRoot, canonical)) {
      kind = BuildRuntimeHeaderKind::Compatibility;
      relative = std::filesystem::path("nonstd") /
                 canonical.lexically_relative(compatibilityRoot);
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
  if (std::none_of(provenance.headers.begin(), provenance.headers.end(),
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
  case BuildRuntimeHeaderKind::Compatibility:
    return "compatibility";
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

ExecutableBuildRequest::ExecutableBuildRequest(
    CompilationRequest compilation, ToolchainLayout toolchain,
    std::filesystem::path generatedSource, std::filesystem::path output,
    std::string nativeCompiler, NativeInputs nativeInputs,
    bool keepGeneratedSource, bool createParentDirectories,
    bool captureSuccessfulNativeOutput,
    std::optional<ManagedOutputPolicy> managedOutput,
    std::optional<std::string> cCompiler, std::optional<BuildCachePolicy> cache,
    std::optional<BuildReportRequest> buildReport)
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

ExecutableBuildResult buildExecutable(const ExecutableBuildRequest &request) {
  ExecutableBuildResult result;
  result.generatedSource = request.generatedSource();
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
          "C++, or compiler executable";
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
        .compilerBuild = currentCompilerBuildIdentity(),
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
    if (request.buildReport()) {
      result.cache.status = BuildCacheStatus::Bypassed;
      result.cache.detail =
          "build-report capture requires a real native invocation";
    } else if (const std::optional<std::string> bypassDetail =
                   nativeCacheBypassDetail(request.nativeInputs(),
                                           linkIdentityPlan)) {
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
          result.compilation.diagnostics =
              std::move(compilationInputs.diagnostics);
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
  const auto compileNativeCSource =
      [&](const std::filesystem::path &source,
          const std::filesystem::path &object, CStandard cStandard,
          const std::vector<std::filesystem::path> &includeDirectories,
          const std::vector<std::string> &arguments) {
        const std::filesystem::path stagedObject = stagedArtifactPath(object);
        TemporaryArtifact stagedNativeObject(stagedObject, true);
        const NativeCCompileRequest cRequest(
            *request.cCompiler(), source, stagedObject, cStandard,
            request.compilation().optimization(), includeDirectories, arguments,
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
        const std::filesystem::path stagedObject = stagedArtifactPath(object);
        TemporaryArtifact stagedNativeObject(stagedObject, true);
        const NativeCppCompileRequest cppRequest(
            request.nativeCompiler(), source, stagedObject,
            request.compilation().cppStandard(),
            request.compilation().optimization(), includeDirectories, arguments,
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
      nativeCppIncludeDirectories(request.toolchain(),
                                  request.compilation().cppStandard(),
                                  request.nativeInputs());
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
    if (request.compilation().cppStandard() == CppStandard::Cpp20 &&
        request.toolchain().vendorInclude !=
            request.toolchain().runtimeInclude) {
      groupCppIncludes.push_back(request.toolchain().vendorInclude);
    }
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
  const std::filesystem::path nativeDependencyFile =
      request.buildReport()
          ? temporaryArtifactPath(request.output(), ".build-report.d")
          : std::filesystem::path{};
  TemporaryArtifact nativeDependencyCleanup(nativeDependencyFile,
                                            request.buildReport().has_value());
  const NativeCompileRequest nativeRequest(
      request.nativeCompiler(), request.generatedSource(), stagedOutput,
      request.compilation().cppStandard(), request.compilation().optimization(),
      effectiveNativeInputs(request.toolchain(),
                            request.compilation().cppStandard(),
                            request.nativeInputs(), nativeObjects),
      request.buildReport()
          ? std::optional<std::filesystem::path>(nativeDependencyFile)
          : std::nullopt);
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

  if (buildProvenance) {
    std::string provenanceError;
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
    std::optional<std::vector<std::filesystem::path>> dependencies =
        parseNativeDependencyFile(*depfile, provenanceError);
    if (!dependencies) {
      generatedArtifact.keep();
      result.generatedSourceRetained = true;
      result.status = ExecutableBuildStatus::BuildReportCaptureFailure;
      result.driverDiagnostic =
          "gti: failed to parse native build-report dependencies: " +
          provenanceError;
      return result;
    }
    std::optional<BuildRuntimeProvenance> runtime =
        captureBuildRuntimeProvenance(request.toolchain(), *dependencies,
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
