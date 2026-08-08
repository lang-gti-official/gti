#pragma once

#include <filesystem>
#include <string_view>

namespace lang::driver {

enum class ArtifactWriteStatus {
  Success,
  OpenFailure,
  WriteFailure,
};

[[nodiscard]] ArtifactWriteStatus
writeArtifact(const std::filesystem::path &path, std::string_view contents);

[[nodiscard]] std::filesystem::path
temporaryCppPath(const std::filesystem::path &input);

class TemporaryArtifact final {
public:
  TemporaryArtifact(std::filesystem::path path, bool removeOnDestruction);
  TemporaryArtifact(const TemporaryArtifact &) = delete;
  TemporaryArtifact &operator=(const TemporaryArtifact &) = delete;
  TemporaryArtifact(TemporaryArtifact &&) = delete;
  TemporaryArtifact &operator=(TemporaryArtifact &&) = delete;
  ~TemporaryArtifact();

  void keep();

private:
  std::filesystem::path artifactPath;
  bool removeOnDestruction;
};

} // namespace lang::driver
