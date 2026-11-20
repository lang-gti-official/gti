#pragma once

#include <filesystem>

namespace lang {

struct StandardLibraryLayout {
  std::filesystem::path root;
  std::filesystem::path prelude;
  // Installed bundles are stamped for their producer executable. An embedding
  // host uses the accompanying sources unless it produced compatible bundles.
  bool useInstalledBundles = false;
};

StandardLibraryLayout standardLibraryLayout(std::filesystem::path configured);
StandardLibraryLayout
discoverStandardLibrary(const char *driver,
                        const std::filesystem::path &buildRoot);

} // namespace lang
