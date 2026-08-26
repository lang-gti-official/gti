#pragma once

#include "gti/driver/compilation.h"
#include "sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver::detail {

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

struct BuildSourceMaterialRecord {
  SourceUnitId id = 0;
  SourceUnitRole role = SourceUnitRole::Application;
  std::string logicalIdentity;
  bool entry = false;
  bool prelude = false;
  std::uintmax_t bytes = 0;
  std::string sha256;
};

struct BuildSourceMaterialProjection {
  std::vector<BuildSourceMaterialRecord> units;
  std::vector<std::string> logicalNamesById;
  std::vector<std::string> preludeRoots;
  std::vector<std::string> dependencyEdges;
};

[[nodiscard]] std::optional<BuildSourceMaterialProjection>
projectBuildSourceMaterials(const CompilationInputs &inputs,
                            const std::filesystem::path &sourceRoot,
                            std::string &errorMessage);

void appendBuildSourceMaterialsIdentity(
    IdentityBuilder &identity, const BuildSourceMaterialProjection &projection);

[[nodiscard]] std::string
buildSourceMaterialsSha256(const BuildSourceMaterialProjection &projection);

} // namespace lang::driver::detail
