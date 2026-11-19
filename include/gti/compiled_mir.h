#pragma once

#include "gti/mir.h"

#include <map>
#include <memory>
#include <span>
#include <string>

namespace lang {

// An executable projection of verified, concrete MIR. Formals occupy the first
// value slots. Each subsequent row defines one value using earlier slots.
// Keys, slots and scalar types survive independently of producer session IDs.
struct CompiledMirStep {
  MirOperation operation = MirOperation::None;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  SemanticType::Kind type = SemanticType::Unknown;
  std::vector<unsigned> inputs;
  std::uint64_t literalBits = 0;
};
struct CompiledMirBody {
  std::vector<CompiledMirStep> steps;
  unsigned returned = 0;
};
using CompiledMirBodies =
    std::map<std::string, std::shared_ptr<const CompiledMirBody>>;

[[nodiscard]] std::shared_ptr<const CompiledMirBody>
projectCompiledMir(const MirFunctionInstance &function);
[[nodiscard]] bool verifyCompiledMir(const CompiledMirBody &body,
                                     std::span<const SemanticType> parameters,
                                     const SemanticType &result);
[[nodiscard]] std::string encodeCompiledMir(const CompiledMirBodies &bodies);
[[nodiscard]] CompiledMirBodies decodeCompiledMir(std::string_view bytes);

} // namespace lang
