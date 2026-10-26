#include "gti/llvm_embedding.h"
#include "gti/lowered_program.h"
#include <stdexcept>
#include <utility>

namespace lang {
LlvmEmbeddingArtifacts generateLlvmEmbedding(LoweredProgram program,
                                             LlvmBackendOptions options) {
  if (!program.embeddingPlan())
    throw BackendUnsupportedError(
        "embedding generation requires a library plan");
  LlvmEmbeddingArtifacts result;
  result.target = program.target();
  result.object = LlvmBackend(options).generate(std::move(program));
  if (!result.object.embedding)
    throw std::logic_error("LLVM embedding lost its matched companion");
  result.header = {BackendArtifactKind::Source, result.object.embedding->header,
                   ".h"};
  result.publicSymbols = result.object.embedding->publicSymbols;
  return result;
}
} // namespace lang
