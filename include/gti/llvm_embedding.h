#pragma once

#include "gti/llvm_backend.h"
#include "gti/target.h"
#include <vector>

namespace lang {

// Produced together from one verified lowered program. The driver may link
// and publish them but never derives C types or public roots from source text.
struct LlvmEmbeddingArtifacts {
  BackendArtifact object;
  BackendArtifact header;
  TargetInfo target;
  std::vector<std::string> publicSymbols;
};

[[nodiscard]] LlvmEmbeddingArtifacts
generateLlvmEmbedding(LoweredProgram program, LlvmBackendOptions options = {});
} // namespace lang
