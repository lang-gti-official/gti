#include "gti/cpp_backend.h"

#include "gti/cpp_emitter.h"

namespace lang {

CppBackend::CppBackend(CppStandard standard) : standard(standard) {}

std::string_view CppBackend::name() const { return "cpp"; }

BackendArtifact CppBackend::generate(const BackendInput &input) {
  return {.kind = BackendArtifactKind::Source,
          .contents = CppEmitter(input.semantics, input.hir, standard,
                                 input.target, &input.optimizations)
                          .emit(input.program),
          .extension = ".cpp"};
}

} // namespace lang
