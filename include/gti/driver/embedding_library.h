#pragma once

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
} // namespace lang::driver
