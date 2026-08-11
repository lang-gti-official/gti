#pragma once

#include "gti/diagnostic.h"

#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace lang::driver {

enum class ArtifactWriteStatus {
  Success,
  OpenFailure,
  WriteFailure,
};

[[nodiscard]] ArtifactWriteStatus
writeArtifact(const std::filesystem::path &path, std::string_view contents);

[[nodiscard]] std::optional<std::filesystem::path>
findLoadedSourceCollision(const std::filesystem::path &artifact,
                          const SourceManager &sources);

[[nodiscard]] std::filesystem::path
temporaryCppPath(const std::filesystem::path &input);

[[nodiscard]] std::filesystem::path
stagedArtifactPath(const std::filesystem::path &output);

struct ArtifactPublishResult {
  std::error_code error;

  [[nodiscard]] bool succeeded() const { return !error; }
};

[[nodiscard]] ArtifactPublishResult
publishArtifact(const std::filesystem::path &staged,
                const std::filesystem::path &destination);

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
