#include "placement.h"
#include "gti/backend.h"

namespace lang::native_llvm {

Placement::Placement(const MirBody &body) {
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions)
      if (!stage.tombstone && stage.result && addressValue(stage.info.type))
        producers.emplace(*stage.result, &stage);
  for (const auto &block : body.blocks) {
    for (const auto &stage : block.instructions) {
      if (stage.tombstone)
        continue;
      if ((stage.kind == MirInstructionKind::Initialize ||
           stage.kind == MirInstructionKind::CallInput) &&
          stage.destination && stage.operands.size() == 1 &&
          stage.operands[0].kind == MirOperandKind::Value) {
        const auto *place = body.findPlace(*stage.destination);
        if (place && addressValue(place->type) &&
            place->type == stage.operands[0].type)
          bind(stage.operands[0].value, {.place = *stage.destination});
      }
      if (stage.operation == MirOperation::Aggregate && stage.result &&
          addressValue(stage.info.type))
        for (std::size_t index = 0; index < stage.operands.size(); ++index)
          if (stage.operands[index].kind == MirOperandKind::Value)
            bind(stage.operands[index].value,
                 {.aggregate = *stage.result, .element = index});
    }
    const auto &term = block.terminator;
    if (term.kind == MirTerminatorKind::Return && term.value &&
        term.value->kind == MirOperandKind::Value &&
        body.returnType == term.value->type)
      bind(term.value->value, {.returned = true});
  }
}

bool Placement::fresh(MirValueId value) const {
  const auto found = producers.find(value);
  if (found == producers.end())
    return false;
  const auto &stage = *found->second;
  if (stage.kind == MirInstructionKind::CallInput &&
      stage.operands.size() == 1 &&
      stage.operands[0].kind == MirOperandKind::Value)
    return fresh(stage.operands[0].value);
  return stage.kind == MirInstructionKind::Construct ||
         (stage.kind == MirInstructionKind::Call && !stage.loan &&
          stage.intrinsic == IntrinsicKind::None) ||
         (stage.kind == MirInstructionKind::Compute &&
          stage.operation == MirOperation::Aggregate);
}

void Placement::bind(MirValueId value, Target target) {
  if (!fresh(value))
    return;
  const auto &stage = *producers.at(value);
  if (stage.kind == MirInstructionKind::CallInput) {
    bind(stage.operands[0].value, target);
    return;
  }
  const auto [found, inserted] = targets.emplace(value, target);
  if (!inserted && found->second != target)
    throw BackendUnsupportedError(
        "LLVM fresh object has conflicting physical destinations");
}

const Placement::Target *Placement::target(MirValueId value) const {
  if (const auto found = producers.find(value);
      found != producers.end() &&
      found->second->kind == MirInstructionKind::CallInput && fresh(value))
    return target(found->second->operands[0].value);
  const auto found = targets.find(value);
  return found == targets.end() ? nullptr : &found->second;
}

} // namespace lang::native_llvm
