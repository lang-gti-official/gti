#include "placement.h"
#include "gti/backend.h"

#include <algorithm>
#include <set>

namespace lang::native_llvm {

bool containsHierarchyStorage(const MirProgram &program,
                              const SemanticType &type) {
  // Scoped to this exact program query. A completed sibling is not a cycle;
  // only an identity still on the current inline-storage path is recursive.
  std::set<HirClassInstanceId> visiting;
  std::map<HirClassInstanceId, bool> completed;
  const auto visit = [&](const auto &self,
                         const SemanticType &selected) -> bool {
    if (selected.kind == SemanticType::Reference ||
        selected.kind == SemanticType::RawPointer) {
      if (selected.arguments.size() != 1)
        throw BackendUnsupportedError(
            "LLVM hierarchy transport lost its indirection type");
      return false;
    }
    if (selected.kind == SemanticType::Array) {
      if (selected.arguments.size() != 1 || selected.arrayLengthParameterId)
        throw BackendUnsupportedError(
            "LLVM hierarchy transport requires a concrete array element");
      return self(self, selected.arguments.front());
    }
    if (selected.kind != SemanticType::Class) {
      switch (selected.kind) {
      case SemanticType::Void:
      case SemanticType::Int8:
      case SemanticType::Int16:
      case SemanticType::Int32:
      case SemanticType::Int64:
      case SemanticType::UInt8:
      case SemanticType::UInt16:
      case SemanticType::UInt32:
      case SemanticType::UInt64:
      case SemanticType::Float:
      case SemanticType::Double:
      case SemanticType::Bool:
      case SemanticType::Char:
      case SemanticType::StringView:
      case SemanticType::CString:
      case SemanticType::NullPtr:
      case SemanticType::Function:
      case SemanticType::FunctionItem:
      case SemanticType::NativeFunction:
      case SemanticType::FunctionSignature:
        return false;
      case SemanticType::Enum:
        if (std::any_of(program.integralEnums().begin(),
                        program.integralEnums().end(),
                        [&](const auto &row) { return row.type == selected; }))
          return false;
        throw BackendUnsupportedError(
            "LLVM hierarchy transport requires a sealed integral enum");
      default:
        throw BackendUnsupportedError(
            "LLVM hierarchy transport has no concrete inline-storage proof");
      }
    }
    const MirClassInstance *owner = nullptr;
    for (const auto &row : program.classInstances())
      if (row.type == selected) {
        if (owner)
          throw BackendUnsupportedError(
              "LLVM hierarchy transport has an ambiguous class identity");
        owner = &row;
      }
    if (!owner || program.findClassInstance(owner->id) != owner ||
        owner->compilerCapability != CompilerCapabilityTypeKind::None ||
        owner->unionLayout || owner->cOpaqueHandle)
      throw BackendUnsupportedError(
          "LLVM hierarchy transport requires an exact ordinary class");
    if (visiting.contains(owner->id))
      throw BackendUnsupportedError(
          "LLVM hierarchy transport has recursive inline class storage");
    if (const auto found = completed.find(owner->id); found != completed.end())
      return found->second;
    visiting.insert(owner->id);
    bool result = owner->polymorphic || owner->abstract ||
                  owner->kind == ClassKind::Interface ||
                  !owner->bases.empty() || !owner->structuralBases.empty();
    for (const auto &field : owner->declaredFields)
      result = self(self, field.type) || result;
    const auto base = [&](const auto &row) {
      const auto *target = program.findClassInstance(row.instance);
      if (!target || target->type != row.type ||
          row.interface != (target->kind == ClassKind::Interface))
        throw BackendUnsupportedError(
            "LLVM hierarchy transport lost an exact base identity");
      result = self(self, row.type) || result;
    };
    for (const auto &row : owner->bases)
      base(row);
    for (const auto &row : owner->structuralBases)
      base(row);
    visiting.erase(owner->id);
    completed.emplace(owner->id, result);
    return result;
  };
  return visit(visit, type);
}

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
