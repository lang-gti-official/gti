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
struct CompiledMirField {
  SemanticType type;
  bool writable;
};
struct CompiledMirStep {
  MirOperation operation = MirOperation::None;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  SemanticType::Kind type = SemanticType::Unknown;
  std::vector<unsigned> inputs;
  std::uint64_t literalBits = 0;
  // Cell zero is a scalar referent; cell n+1 is field n of a record.
  std::optional<unsigned> loadedCell;
};
struct CompiledMirBody {
  std::vector<CompiledMirStep> steps;
  unsigned returned = 0;
  // Final value for each modified cell of the single reference parameter.
  std::map<unsigned, unsigned> writes;
};
using CompiledMirBodies =
    std::map<std::string, std::shared_ptr<const CompiledMirBody>>;

[[nodiscard]] std::shared_ptr<const CompiledMirBody>
projectCompiledMir(const MirProgram &, const MirFunctionInstance &function);
[[nodiscard]] std::vector<CompiledMirField>
compiledMirFields(const MirProgram &, std::span<const SemanticType> parameters);
// Decode field types and access independently from checked interface metadata.
[[nodiscard]] std::vector<CompiledMirField>
compiledMirInterfaceFields(std::string_view metadata,
                           std::span<const SemanticType> syntheticParameters);
[[nodiscard]] bool verifyCompiledMir(const CompiledMirBody &body,
                                     std::span<const SemanticType> parameters,
                                     const SemanticType &result,
                                     std::span<const CompiledMirField> fields);
[[nodiscard]] std::string encodeCompiledMir(const CompiledMirBodies &bodies);
[[nodiscard]] CompiledMirBodies decodeCompiledMir(std::string_view bytes);

} // namespace lang
