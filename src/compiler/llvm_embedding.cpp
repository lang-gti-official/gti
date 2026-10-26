#include "gti/llvm_embedding.h"
#include "gti/lowered_program.h"
#include "gti/native_header.h"

#include <algorithm>

namespace lang {
LlvmEmbeddingArtifacts generateLlvmEmbedding(LoweredProgram program,
                                             LlvmBackendOptions options) {
  if (!program.embeddingPlan())
    throw BackendUnsupportedError(
        "embedding generation requires a library plan");
  LlvmEmbeddingArtifacts result;
  result.target = program.target();
  result.publicSymbols = {"gti_context_create_v1", "gti_context_destroy_v1",
                          "gti_get_module_descriptor_v1"};
  for (const auto &selected : program.embeddingPlan()->exports)
    result.publicSymbols.push_back(selected.symbol);
  std::sort(result.publicSymbols.begin(), result.publicSymbols.end());
  result.header = NativeHeaderBackend().generate(program);
  result.object = LlvmBackend(options).generate(std::move(program));
  return result;
}
} // namespace lang
