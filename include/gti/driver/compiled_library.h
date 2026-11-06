#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lang::driver {
using CompiledLibrarySnapshot = std::shared_ptr<const std::string>;
// Resolved project policy. The compiler owns export selection and callable
// validation; these values never grant source visibility or native ABI trust.
struct CompiledLibraryTarget {
  std::string package;
  std::string target;
  std::vector<std::string> functions;
};

struct CompiledLibraryImport {
  std::string package;
  std::string target;
  std::filesystem::path artifact;
  std::filesystem::path interfacePath;
  bool interfaceRequired = true;
  bool directDependency = true;
  // Owned input bytes, not an admission receipt. Every consumer still checks
  // the complete bundle, compatibility, object and dependency contracts.
  CompiledLibrarySnapshot snapshot = {};
};

// One project build's bounded artifact selection. Paths identify selections;
// once selected, replacement/deletion on disk cannot change their bytes.
class CompiledLibrarySnapshots {
public:
  void retain(const std::filesystem::path &, CompiledLibrarySnapshot);
  [[nodiscard]] std::vector<CompiledLibraryImport>
      select(std::span<const CompiledLibraryImport>);

private:
  std::map<std::filesystem::path, CompiledLibrarySnapshot> artifacts;
  std::size_t bytes = 0;
};
// Preserve the original single-import API while keeping one canonical list.
inline std::vector<CompiledLibraryImport>
normalizeCompiledLibraryImports(std::optional<CompiledLibraryImport> legacy,
                                std::vector<CompiledLibraryImport> imports) {
  if (legacy) {
    if (!imports.empty())
      throw std::invalid_argument(
          "cannot combine singular and plural compiled library imports");
    imports.push_back(std::move(*legacy));
  }
  if (imports.size() > 64)
    throw std::invalid_argument(
        "at most 64 compiled libraries may be imported");
  return imports;
}
} // namespace lang::driver
