#include "gti/standard_library.h"

#include "gti/executable_path.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

namespace lang {

StandardLibraryLayout standardLibraryLayout(std::filesystem::path configured) {
  if (configured.extension() == ".gti") {
    return {.root = configured.parent_path(), .prelude = std::move(configured)};
  }
  return {.root = configured, .prelude = configured / "prelude.gti"};
}

StandardLibraryLayout
discoverStandardLibrary(const char *driver,
                        const std::filesystem::path &buildRoot) {
  if (const char *configured = std::getenv("GTI_STDLIB_PATH");
      configured != nullptr && *configured != '\0') {
    return standardLibraryLayout(configured);
  }

  const std::filesystem::path executable = executablePath(driver);
  const std::filesystem::path installedRoot =
      executable.parent_path().parent_path() / "share/gti/stdlib";
  std::error_code error;
  if (std::filesystem::is_regular_file(installedRoot / "prelude.gti", error)) {
    return standardLibraryLayout(installedRoot);
  }
  return standardLibraryLayout(buildRoot.empty() ? installedRoot : buildRoot);
}

} // namespace lang
