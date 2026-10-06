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

struct DiagnosticLogResult {
  std::filesystem::path path;
  std::error_code error;

  [[nodiscard]] bool succeeded() const { return !path.empty() && !error; }
};

// Retains failure evidence in a fresh private temporary directory, outside
// build outputs and caches. Never overwrites an existing path. A failed write
// does not claim a saved log; callers retain the original failure status/text.
[[nodiscard]] DiagnosticLogResult writeDiagnosticLog(std::string_view contents);

// True when two path spellings select the same normalized destination or when
// both existing paths identify the same filesystem object. A missing path is
// permitted and conflicts only through its normalized spelling.
[[nodiscard]] bool artifactPathsAlias(const std::filesystem::path &left,
                                      const std::filesystem::path &right);

[[nodiscard]] std::optional<std::filesystem::path>
findLoadedSourceCollision(const std::filesystem::path &artifact,
                          const SourceManager &sources);

[[nodiscard]] std::filesystem::path
temporaryCppPath(const std::filesystem::path &input);

[[nodiscard]] std::filesystem::path
temporaryArtifactPath(const std::filesystem::path &input,
                      std::string_view extension);

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
