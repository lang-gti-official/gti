#include "gti/driver/artifact.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <process.h>
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

std::filesystem::path temporaryCppPath(const std::filesystem::path &input) {
  const auto nonce =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::uint64_t sequence =
      temporaryArtifactSequence.fetch_add(1, std::memory_order_relaxed);
  return std::filesystem::temp_directory_path() /
         ("gti-" + std::to_string(processId()) + "-" + std::to_string(nonce) +
          "-" + std::to_string(sequence) + "-" + input.stem().string() +
          ".cpp");
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
