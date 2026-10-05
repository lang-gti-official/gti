#include "registry_cache.h"
#include "gti/driver/artifact.h"
#include "gti/driver/manifest.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>

namespace lang::driver {
namespace {
[[noreturn]] void integrity() {
  throw RegistryError("GTI-B1717",
                      "Package integrity check failed; refusing changed "
                      "metadata, archive or cached source. Inspect or remove "
                      "the affected cache entry explicitly before retrying.");
}
[[noreturn]] void storeFailure() {
  throw RegistryError("GTI-B1722",
                      "Cannot safely read or publish the package cache.");
}
std::string env(const char *name) {
  auto *value = std::getenv(name);
  return value ? value : "";
}
std::filesystem::path versionCache(const RegistryPackageKey &key,
                                   std::string_view version) {
  // One full digest frames logical source/version, keeping portable cache
  // paths short without truncating cryptographic identity.
  return registryCacheRoot() /
         packageChecksum(key.identity() + "@" + std::string(version)).substr(7);
}
// The configured root is trusted user policy. All managed descendants must
// remain ordinary directories/files, never redirects into unrelated paths.
void safePath(const std::filesystem::path &path, bool createParents) {
  const auto root = registryCacheRoot();
  auto relative = path.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..") {
    storeFailure();
  }
  std::error_code error;
  if (createParents) {
    std::filesystem::create_directories(root, error);
    if (error) {
      storeFailure();
    }
  }
  auto current = root;
  for (auto it = relative.begin(); it != relative.end(); ++it) {
    current /= *it;
    auto status = std::filesystem::symlink_status(current, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      storeFailure();
    }
    error.clear();
    if (std::filesystem::is_symlink(status)) {
      integrity();
    }
    const bool final = std::next(it) == relative.end();
    if (!final && std::filesystem::exists(status) &&
        !std::filesystem::is_directory(status)) {
      integrity();
    }
    if (!final && !std::filesystem::exists(status) && createParents) {
      std::filesystem::create_directory(current, error);
      if (error) {
        storeFailure();
      }
    }
  }
}
struct StagedDirectory {
  std::filesystem::path path;
  ~StagedDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
} // namespace

std::filesystem::path registryCacheRoot() {
  auto path = env("GTI_PACKAGE_CACHE");
  if (path.empty()) {
    path = env("XDG_CACHE_HOME");
    if (path.empty()) {
      path = env("HOME");
      if (!path.empty()) {
        path += "/.cache";
      }
    }
    if (path.empty()) {
      path = env("LOCALAPPDATA");
    }
    if (path.empty()) {
      throw RegistryError(
          "GTI-B1722",
          "Set GTI_PACKAGE_CACHE to an absolute user-cache directory.");
    }
    path += "/gti/packages";
  }
  std::filesystem::path root(path);
  if (!root.is_absolute() || root == root.root_path()) {
    storeFailure();
  }
  std::error_code error;
  root = std::filesystem::weakly_canonical(root, error);
  if (error) {
    storeFailure();
  }
  return root;
}
std::filesystem::path registryPackageCache(const RegistryPackageKey &key,
                                           std::string_view version,
                                           std::string_view checksum) {
  if (!validRegistryName(key.registry) || !validRegistryName(key.package) ||
      !parsePackageVersion(version) || !validPackageChecksum(checksum)) {
    storeFailure();
  }
  return versionCache(key, version) / std::string(checksum.substr(7));
}
std::filesystem::path
registryPackageSource(const LockedRegistryPackage &package) {
  return registryPackageCache({package.registry, package.package},
                              package.version, package.checksum) /
         "source";
}
namespace detail {
void validateRegistryManifest(const std::filesystem::path &root,
                              const RegistryPackageMetadata &metadata) {
  const auto loaded = loadProjectManifest(root / "gti.toml");
  if (!loaded.succeeded() || loaded.manifest->workspace() ||
      loaded.manifest->package().name != metadata.name ||
      loaded.manifest->package().version != metadata.version) {
    throw RegistryError("GTI-B1714",
                        "Archive manifest is invalid after relocation or "
                        "differs from registry metadata.");
  }
  std::vector<RegistryDependency> declared;
  std::error_code error;
  if (!std::filesystem::is_directory(loaded.manifest->package().sourceRoot,
                                     error) ||
      error) {
    throw RegistryError("GTI-B1714", "Archive has no existing declared package "
                                     "source root after relocation.");
  }
  for (const auto &dep : loaded.manifest->dependencies()) {
    if (!dep.registry) {
      throw RegistryError("GTI-B1714",
                          "Registry packages must have registry-only "
                          "dependencies in this slice.");
    }
    declared.push_back({dep.alias,
                        {dep.registry->registry, dep.registry->package},
                        dep.registry->requirement});
  }
  if (declared != metadata.dependencies) {
    throw RegistryError("GTI-B1714",
                        "Archive dependencies differ from registry metadata.");
  }
}
void validateRegistryArchive(std::string_view archive,
                             const RegistryPackageMetadata &metadata) {
  const auto path = temporaryArtifactPath("registry-archive", ".dir");
  std::error_code error;
  if (!std::filesystem::create_directory(path, error) || error) {
    storeFailure();
  }
  StagedDirectory cleanup{path};
  for (const auto &entry : decodePackageArchive(archive)) {
    const auto target = path / entry.path;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
      storeFailure();
    }
    std::ofstream output(target, std::ios::binary);
    output.write(entry.contents.data(),
                 static_cast<std::streamsize>(entry.contents.size()));
    output.close();
    if (!output) {
      storeFailure();
    }
  }
  validateRegistryManifest(path, metadata);
}
std::filesystem::path metadataCachePath(const RegistryPackageKey &key,
                                        std::string_view version) {
  return versionCache(key, version) / "metadata.record";
}
std::optional<std::string>
readCachedRegistryMetadata(const RegistryPackageKey &key,
                           std::string_view version) {
  constexpr std::string_view magic = "GTI-METADATA-CACHE1\n";
  auto bytes = readRegistryCacheFile(metadataCachePath(key, version),
                                     1024U * 1024U + 128U);
  if (!bytes) {
    return std::nullopt;
  }
  if (!bytes->starts_with(magic) || bytes->size() < magic.size() + 72 ||
      (*bytes)[magic.size() + 71] != '\n') {
    integrity();
  }
  auto text = bytes->substr(magic.size() + 72);
  if (bytes->substr(magic.size(), 71) != packageChecksum(text)) {
    integrity();
  }
  return text;
}
void storeCachedRegistryMetadata(const RegistryPackageKey &key,
                                 std::string_view version,
                                 std::string_view text) {
  const auto record = "GTI-METADATA-CACHE1\n" + packageChecksum(text) + "\n" +
                      std::string(text);
  storeRegistryCacheFile(metadataCachePath(key, version), record);
}
std::optional<std::string>
readRegistryCacheFile(const std::filesystem::path &path, std::uintmax_t limit) {
  safePath(path, false);
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (!std::filesystem::exists(status) &&
      (!error || error == std::errc::no_such_file_or_directory)) {
    return std::nullopt;
  }
  if (error || !std::filesystem::is_regular_file(status)) {
    integrity();
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > limit) {
    integrity();
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    storeFailure();
  }
  std::string text{std::istreambuf_iterator<char>(input), {}};
  if (input.bad() || text.size() != size) {
    storeFailure();
  }
  return text;
}
void storeRegistryCacheFile(const std::filesystem::path &path,
                            std::string_view bytes) {
  safePath(path, true);
  if (auto existing = readRegistryCacheFile(path, 256U * 1024U * 1024U)) {
    if (*existing != bytes) {
      integrity();
    }
    return;
  }
  const auto staged = stagedArtifactPath(path);
  TemporaryArtifact cleanup(staged, true);
  {
    std::ofstream output(staged, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
      storeFailure();
    }
  }
  // A hard link publishes a completely written immutable file atomically
  // without replacing another publisher's entry. Identical concurrent writers
  // may share it; different bytes are always an integrity failure.
  std::error_code error;
  std::filesystem::create_hard_link(staged, path, error);
  if (error) {
    if (auto existing = readRegistryCacheFile(path, 256U * 1024U * 1024U)) {
      if (*existing != bytes) {
        integrity();
      }
      return;
    }
    storeFailure();
  }
}
void verifyRegistrySource(const std::filesystem::path &root,
                          const std::vector<PackageArchiveEntry> &entries) {
  safePath(root, false);
  std::map<std::string, const PackageArchiveEntry *> expected;
  for (const auto &entry : entries) {
    expected.emplace(entry.path, &entry);
  }
  std::error_code error;
  std::filesystem::recursive_directory_iterator it(root, error), end;
  if (error) {
    storeFailure();
  }
  std::size_t observed = 0;
  while (it != end) {
    const auto status = it->symlink_status(error);
    if (error || std::filesystem::is_symlink(status)) {
      integrity();
    }
    if (std::filesystem::is_regular_file(status)) {
      const auto path = it->path().lexically_relative(root).generic_string();
      const auto wanted = expected.find(path);
      if (wanted == expected.end()) {
        integrity();
      }
      auto contents = readRegistryCacheFile(it->path(), 64U * 1024U * 1024U);
      if (!contents || *contents != wanted->second->contents) {
        integrity();
      }
#if !defined(_WIN32)
      const bool executable =
          (status.permissions() & std::filesystem::perms::owner_exec) !=
          std::filesystem::perms::none;
      if (executable != wanted->second->executable) {
        integrity();
      }
#endif
      ++observed;
    } else if (!std::filesystem::is_directory(status)) {
      integrity();
    }
    it.increment(error);
    if (error) {
      storeFailure();
    }
  }
  if (observed != entries.size()) {
    integrity();
  }
}
} // namespace detail

LockedRegistryPackage materializeRegistryPackage(
    const RegistrySelection &selection, RegistryProvider &registries,
    RegistryCachePolicy policy, const LockedRegistryPackage *locked) {
  const auto &meta = selection.metadata;
  const auto metadataHash = packageChecksum(selection.metadataText);
  if (locked &&
      (locked->registry != meta.key.registry ||
       locked->package != meta.key.package || locked->version != meta.version ||
       locked->checksum != meta.checksum ||
       locked->metadataChecksum != metadataHash || locked->name != meta.name)) {
    integrity();
  }
  const auto cache =
      registryPackageCache(meta.key, meta.version, meta.checksum);
  auto bytes = detail::readRegistryCacheFile(cache / "source.gtipkg",
                                             256U * 1024U * 1024U);
  if (!bytes) {
    if (!policy.allowAcquisition) {
      throw RegistryError("GTI-B1703",
                          "Registry archive is not cached; run `gti fetch` "
                          "before offline builds or editor analysis.");
    }
    bytes = registries.get(meta.key.registry)
                .archive(meta.key.package, meta.version, meta.archive);
    if (packageChecksum(*bytes) != meta.checksum) {
      integrity();
    }
    detail::storeRegistryCacheFile(cache / "source.gtipkg", *bytes);
  }
  if (packageChecksum(*bytes) != meta.checksum) {
    integrity();
  }
  auto entries = decodePackageArchive(*bytes);
  const auto source = cache / "source";
  safePath(source, policy.allowMaterialization);
  std::error_code error;
  if (!std::filesystem::exists(source, error)) {
    if (!policy.allowMaterialization) {
      throw RegistryError("GTI-B1703",
                          "Registry source is not materialized; run `gti "
                          "fetch` before builds or editor analysis.");
    }
    if (error) {
      storeFailure();
    }
    const auto staged = stagedArtifactPath(source);
    if (!std::filesystem::create_directory(staged, error) || error) {
      storeFailure();
    }
    StagedDirectory cleanup{staged};
    for (const auto &entry : entries) {
      const auto path = staged / entry.path;
      std::filesystem::create_directories(path.parent_path(), error);
      if (error) {
        storeFailure();
      }
      {
        std::ofstream output(path, std::ios::binary);
        output.write(entry.contents.data(),
                     static_cast<std::streamsize>(entry.contents.size()));
        output.close();
        if (!output) {
          storeFailure();
        }
      }
      const auto permissions = std::filesystem::perms::owner_read |
                               std::filesystem::perms::owner_write |
                               std::filesystem::perms::group_read |
                               std::filesystem::perms::others_read;
      std::filesystem::permissions(
          path,
          permissions |
              (entry.executable ? std::filesystem::perms::owner_exec |
                                      std::filesystem::perms::group_exec |
                                      std::filesystem::perms::others_exec
                                : std::filesystem::perms::none),
          error);
      if (error) {
        storeFailure();
      }
    }
    detail::validateRegistryManifest(staged, meta);
    std::filesystem::rename(staged, source, error);
    if (error) {
      // Another fetch may have won the complete-directory publication race.
      // Verify its source before using it; our private stage is still cleaned.
      std::error_code inspected;
      if (!std::filesystem::is_directory(source, inspected) || inspected) {
        storeFailure();
      }
      detail::verifyRegistrySource(source, entries);
    }
  }
  detail::verifyRegistrySource(source, entries);
  detail::validateRegistryManifest(source, meta);
  return {.name = meta.name,
          .version = meta.version,
          .registry = meta.key.registry,
          .package = meta.key.package,
          .checksum = meta.checksum,
          .metadataChecksum = metadataHash};
}
} // namespace lang::driver
