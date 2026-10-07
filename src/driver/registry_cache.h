#pragma once
#include "gti/driver/registry.h"

namespace lang::driver::detail {
[[nodiscard]] std::filesystem::path
metadataCachePath(const RegistryPackageKey &key, std::string_view version);
[[nodiscard]] std::optional<std::string>
readCachedRegistryMetadata(const RegistryPackageKey &key,
                           std::string_view version);
void storeCachedRegistryMetadata(const RegistryPackageKey &key,
                                 std::string_view version,
                                 std::string_view text);
[[nodiscard]] std::optional<std::string>
readRegistryCacheFile(const std::filesystem::path &path, std::uintmax_t limit);
void storeRegistryCacheFile(const std::filesystem::path &path,
                            std::string_view bytes);
void verifyRegistrySource(const std::filesystem::path &root,
                          const std::vector<PackageArchiveEntry> &entries);
void validateRegistryManifest(const std::filesystem::path &root,
                              const RegistryPackageMetadata &metadata);
void validateRegistryArchive(std::string_view archive,
                             const RegistryPackageMetadata &metadata);
} // namespace lang::driver::detail
