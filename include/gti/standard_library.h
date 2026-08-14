#pragma once

#include <filesystem>

namespace lang {

struct StandardLibraryLayout {
  std::filesystem::path root;
  std::filesystem::path prelude;
};

StandardLibraryLayout standardLibraryLayout(std::filesystem::path configured);
StandardLibraryLayout
discoverStandardLibrary(const char *driver,
                        const std::filesystem::path &buildRoot);

} // namespace lang
