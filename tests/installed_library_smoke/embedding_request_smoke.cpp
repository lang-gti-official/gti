#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "gti/program_request.h"

#include <utility>

int main() {
  lang::ProgramRequest request{.kind = lang::ProgramKind::Library};
  request.exports.push_back({.function = "update", .symbol = "update_v1"});
  const auto result =
      lang::Frontend({.program = std::move(request)})
          .analyze("sdk-embedding.gti",
                   "int32_t update(int32_t frame) { return frame + 1; }");
  return result.canGenerateCode() &&
                 result.mir.programKind() == lang::ProgramKind::Library &&
                 result.mir.embeddingPlan() &&
                 result.mir.embeddingPlan()->exports.size() == 1 &&
                 result.mir.embeddingPlan()->exports.front().symbol ==
                     "update_v1" &&
                 !result.mir.hostedStartupPlan() &&
                 lang::verifyMirProgram(result.mir).valid()
             ? 0
             : 1;
}
