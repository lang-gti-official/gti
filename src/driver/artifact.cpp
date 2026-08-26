#include "gti/driver/artifact.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lang::driver {
namespace {

std::atomic<std::uint64_t> temporaryArtifactSequence = 0;

std::uint64_t processId() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

std::string temporaryIdentity() {
  const auto nonce =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::uint64_t sequence =
      temporaryArtifactSequence.fetch_add(1, std::memory_order_relaxed);
  return std::to_string(processId()) + "-" + std::to_string(nonce) + "-" +
         std::to_string(sequence);
}

} // namespace

ArtifactWriteStatus writeArtifact(const std::filesystem::path &path,
                                  std::string_view contents) {
  std::ofstream output(path);
  if (!output) {
    return ArtifactWriteStatus::OpenFailure;
  }
  output << contents;
  return output ? ArtifactWriteStatus::Success
                : ArtifactWriteStatus::WriteFailure;
}

bool artifactPathsAlias(const std::filesystem::path &left,
                        const std::filesystem::path &right) {
  std::error_code leftError;
  const std::filesystem::path absoluteLeft =
      std::filesystem::absolute(left, leftError);
  std::error_code rightError;
  const std::filesystem::path absoluteRight =
      std::filesystem::absolute(right, rightError);
  if (leftError || rightError) {
    return false;
  }

  std::error_code equivalentError;
  if (std::filesystem::equivalent(absoluteLeft, absoluteRight,
                                  equivalentError)) {
    return true;
  }

  leftError.clear();
  const std::filesystem::path resolvedLeft =
      std::filesystem::weakly_canonical(absoluteLeft, leftError);
  rightError.clear();
  const std::filesystem::path resolvedRight =
      std::filesystem::weakly_canonical(absoluteRight, rightError);
  return !leftError && !rightError && resolvedLeft == resolvedRight;
}

std::optional<std::filesystem::path>
findLoadedSourceCollision(const std::filesystem::path &artifact,
                          const SourceManager &sources) {
  std::vector<std::string> names = sources.names();
  std::sort(names.begin(), names.end());
  for (const std::string &name : names) {
    const std::filesystem::path source(name);
    if (artifactPathsAlias(artifact, source)) {
      return source;
    }
  }
  return std::nullopt;
}

std::filesystem::path temporaryCppPath(const std::filesystem::path &input) {
  return temporaryArtifactPath(input, ".cpp");
}

std::filesystem::path temporaryArtifactPath(const std::filesystem::path &input,
                                            std::string_view extension) {
  return std::filesystem::temp_directory_path() /
         ("gti-" + temporaryIdentity() + "-" + input.stem().string() +
          std::string(extension));
}

std::filesystem::path stagedArtifactPath(const std::filesystem::path &output) {
  const std::string stem =
      output.stem().empty() ? "artifact" : output.stem().string();
  return output.parent_path() /
         ("." + stem + ".gti-stage-" + temporaryIdentity() +
          output.extension().string());
}

ArtifactPublishResult
publishArtifact(const std::filesystem::path &staged,
                const std::filesystem::path &destination) {
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(destination.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if (ReplaceFileW(destination.c_str(), staged.c_str(), nullptr, 0, nullptr,
                     nullptr) != 0) {
      return {};
    }
    const DWORD replaceError = GetLastError();
    return {std::error_code(static_cast<int>(replaceError),
                            std::system_category())};
  }

  const DWORD attributeError = GetLastError();
  if (attributeError != ERROR_FILE_NOT_FOUND &&
      attributeError != ERROR_PATH_NOT_FOUND) {
    return {std::error_code(static_cast<int>(attributeError),
                            std::system_category())};
  }
  if (MoveFileExW(staged.c_str(), destination.c_str(),
                  MOVEFILE_WRITE_THROUGH) != 0) {
    return {};
  }
  const DWORD moveError = GetLastError();
  return {std::error_code(static_cast<int>(moveError), std::system_category())};
#else
  std::error_code error;
  std::filesystem::rename(staged, destination, error);
  return {error};
#endif
}

TemporaryArtifact::TemporaryArtifact(std::filesystem::path path,
                                     bool removeOnDestruction)
    : artifactPath(std::move(path)), removeOnDestruction(removeOnDestruction) {}

TemporaryArtifact::~TemporaryArtifact() {
  if (removeOnDestruction) {
    std::error_code error;
    std::filesystem::remove(artifactPath, error);
  }
}

void TemporaryArtifact::keep() { removeOnDestruction = false; }

} // namespace lang::driver
