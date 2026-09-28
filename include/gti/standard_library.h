#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace lang {

struct StandardLibraryLayout {
  std::filesystem::path root;
  std::filesystem::path prelude;
};

StandardLibraryLayout standardLibraryLayout(std::filesystem::path configured);
StandardLibraryLayout
discoverStandardLibrary(const char *driver,
                        const std::filesystem::path &buildRoot);

// Returns the canonical standard-library include that owns a public nominal
// type. This deliberately covers declarations, not arbitrary spelling
// guesses, so semantic diagnostics can offer a mechanically safe include edit.
[[nodiscard]] std::optional<std::string_view>
standardLibraryIncludeForType(std::string_view qualifiedName);

} // namespace lang
