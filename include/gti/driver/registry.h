#pragma once

#include "gti/driver/dependencies.h"
#include "gti/driver/registry_credentials.h"
#include "gti/driver/semver.h"

#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

struct RegistryPackageKey {
  std::string registry = "official";
  std::string package;
  [[nodiscard]] std::string identity() const;
  auto operator<=>(const RegistryPackageKey &) const = default;
};

struct RegistryDependency {
  std::string alias;
  RegistryPackageKey key;
  std::string requirement;
  bool operator==(const RegistryDependency &) const = default;
};

struct RegistryPackageMetadata {
  RegistryPackageKey key;
  std::string
      name; // Canonical GTI manifest name; independent of include alias.
  std::string version;
  std::string archive = "source.gtipkg";
  std::string checksum;
  std::vector<RegistryDependency> dependencies;
};

// Stable driver diagnostic codes accompany all registry failures. Host-specific
// HTTP details are optional, never a credential-bearing response body or URL.
class RegistryError final : public std::runtime_error {
public:
  RegistryError(std::string code, std::string message);
  std::string code;
};

class Registry {
public:
  virtual ~Registry() = default;
  // Optional backend authentication capability, independent of source formats.
  [[nodiscard]] virtual std::optional<RegistryAuthenticationInfo>
  authentication() const {
    return std::nullopt;
  }
  virtual void validateCredential(const RegistryCredential &credential);
  [[nodiscard]] virtual std::vector<std::string>
  listVersions(std::string_view package) = 0;
  // nullopt means a version is incomplete/not a published GTI package.
  [[nodiscard]] virtual std::optional<std::string>
  metadata(std::string_view package, std::string_view version) = 0;
  [[nodiscard]] virtual std::string archive(std::string_view package,
                                            std::string_view version,
                                            std::string_view file) = 0;
  virtual void publish(std::string_view package, std::string_view version,
                       std::string_view archive, std::string_view metadata) = 0;
};

class RegistryProvider {
public:
  virtual ~RegistryProvider() = default;
  [[nodiscard]] virtual Registry &get(std::string_view identity) = 0;
};

[[nodiscard]] std::unique_ptr<RegistryProvider>
configuredRegistries(std::shared_ptr<RegistryCredentialStore> credentials = {});
// Validate the explicitly supplied credential before replacing stored state.
// Does not inherit an environment override or an already stored credential.
void loginRegistry(Registry &registry, RegistryCredentialStore &store,
                   const RegistryCredential &credential);
void logoutRegistry(Registry &registry, RegistryCredentialStore &store);
[[nodiscard]] RegistryPackageMetadata
parseRegistryMetadata(std::string_view text, const RegistryPackageKey &key,
                      std::string_view version);
[[nodiscard]] std::string
renderRegistryMetadata(const RegistryPackageMetadata &metadata);
[[nodiscard]] std::string packageChecksum(std::string_view bytes);
[[nodiscard]] bool validPackageChecksum(std::string_view checksum);
[[nodiscard]] bool validRegistryName(std::string_view name);

// All version solving consumes only metadata. Sources are acquired after a
// coherent closure exists. A single version per registry/package is the same
// structural boundary as the current workspace's unique canonical names.
struct RegistrySelection {
  RegistryPackageMetadata metadata;
  std::string metadataText;
};
struct RegistryCachePolicy {
  bool allowAcquisition = false;
  bool allowMaterialization = false;
};
[[nodiscard]] std::map<RegistryPackageKey, RegistrySelection>
resolveRegistryPackages(
    const std::vector<RegistryDependency> &roots, const DependencyLock &lock,
    RegistryProvider &registries, bool lockedOnly, bool offline,
    const std::optional<RegistryPackageKey> &preferLatest = std::nullopt,
    const std::vector<std::string> &reservedCanonicalNames = {});

[[nodiscard]] std::filesystem::path registryCacheRoot();
[[nodiscard]] std::filesystem::path
registryPackageCache(const RegistryPackageKey &key, std::string_view version,
                     std::string_view checksum);
// Verifies metadata, archive and extracted source on every use. Does not repair
// corruption silently, execute code or trust a mutable ready/cache marker.
[[nodiscard]] LockedRegistryPackage materializeRegistryPackage(
    const RegistrySelection &selection, RegistryProvider &registries,
    RegistryCachePolicy policy, const LockedRegistryPackage *locked = nullptr);
[[nodiscard]] std::filesystem::path
registryPackageSource(const LockedRegistryPackage &package);

struct PackageArchiveEntry {
  std::string path;
  std::string contents;
  bool executable = false;
};
[[nodiscard]] std::string
encodePackageArchive(std::vector<PackageArchiveEntry> entries);
[[nodiscard]] std::vector<PackageArchiveEntry>
decodePackageArchive(std::string_view archive);

struct RegistryCommandResult {
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;
  std::string summary;
  [[nodiscard]] bool succeeded() const { return diagnostics.empty(); }
};
[[nodiscard]] RegistryCommandResult addRegistryDependency(
    const std::filesystem::path &start, std::string_view package,
    std::string_view requirement = {}, std::string_view registry = "official");
[[nodiscard]] RegistryCommandResult
publishRegistryPackage(const std::filesystem::path &start,
                       std::string_view registry = "official",
                       std::string_view publicName = {}, bool dryRun = false);

} // namespace lang::driver
