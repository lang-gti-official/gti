#pragma once

#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"
#include "gti/llvm_embedding.h"

namespace lang::driver {

struct EmbeddingLibraryRequest {
  std::string compiler;
  std::filesystem::path runtimeArchive;
  // A new immutable generation directory. Existing generations are never
  // replaced: hosts may keep one loaded while building another.
  std::filesystem::path generationDirectory;
  std::string name;
  NativeInputs nativeInputs;
  bool debugInfo = false;
  std::optional<ForwardedSourcePolicy> forwardedSources;
};

struct EmbeddingLibraryResult {
  std::filesystem::path library;
  std::filesystem::path header;
  std::vector<std::string> command;
  ProcessResult process;
  std::string error;
  [[nodiscard]] bool succeeded() const {
    return error.empty() && !library.empty() && process.succeeded();
  }
};

// Reuses the native command and atomic artifact helpers. Publishes the
// matching library/header directory only after successful native linking.
[[nodiscard]] EmbeddingLibraryResult
publishEmbeddingLibrary(const LlvmEmbeddingArtifacts &artifacts,
                        const EmbeddingLibraryRequest &request);

// Commits a privately staged complete generation without replacing any path.
[[nodiscard]] ArtifactPublishResult
publishEmbeddingGeneration(const std::filesystem::path &staged,
                           const std::filesystem::path &destination);
} // namespace lang::driver
