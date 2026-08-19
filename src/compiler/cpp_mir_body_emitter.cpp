#include "cpp_mir_body_emitter.h"
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace {
thread_local int gtiProbeTraceKind;
thread_local unsigned long gtiProbeTraceOwner;
thread_local int gtiProbeTraceForm;
} // namespace

namespace lang {
namespace {

// A fixed-array element place carries one Index projection over its
// unprojected sibling array place (same binding); the access spells as
// subscription on the sibling with a dynamic value or constant index.
struct ArrayElementAccess {
  MirPlaceId array = 0;
  MirValueId index = 0;
  std::optional<std::uint64_t> constantIndex;
};

[[nodiscard]] inline std::optional<ArrayElementAccess>
arrayElementAccess(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding || place.binding == 0 ||
      place.projections.size() != 1 ||
      place.projections[0].kind != MirProjectionKind::Index) {
    return std::nullopt;
  }
  for (const MirPlace &candidate : body.places) {
    if (candidate.id != place.id &&
        candidate.root == MirPlaceRootKind::Binding &&
        candidate.binding == place.binding && candidate.projections.empty() &&
        candidate.type.kind == SemanticType::Array) {
      return ArrayElementAccess{candidate.id, place.projections[0].index,
                                place.projections[0].constantIndex};
    }
  }
  return std::nullopt;
}

// A string-view element place reads through the terminal bounds-checked
// helper, exactly like the compatibility subscript: string_view_at
// contains a violated bound itself, so both text forms spell the plain
// call. The access reuses the array-element shape with the sibling
// string-view local as the base.
[[nodiscard]] std::optional<ArrayElementAccess>
viewElementAccess(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding || place.binding == 0 ||
      place.projections.size() != 1 ||
      place.projections[0].kind != MirProjectionKind::Index) {
    return std::nullopt;
  }
  for (const MirPlace &candidate : body.places) {
    if (candidate.id != place.id &&
        candidate.root == MirPlaceRootKind::Binding &&
        candidate.binding == place.binding && candidate.projections.empty() &&
        candidate.type.kind == SemanticType::StringView) {
      return ArrayElementAccess{candidate.id, place.projections[0].index,
                                place.projections[0].constantIndex};
    }
  }
  return std::nullopt;
}

struct ClassSubscriptAccess {
  MirPlaceId base = 0;
  HirClassInstanceId owner = 0;
  MirValueId index = 0;
  std::optional<std::uint64_t> constantIndex;
  SemanticType indexType = SemanticType::Unknown;
};

// A class-subscription place: an Index projection over a sibling
// class-typed local. The compatibility path spells it as the class's
// subscript member call, so the access carries the member-resolution
// facts: the base place, its class instance, and the index identity.
[[nodiscard]] std::optional<ClassSubscriptAccess>
classSubscriptAccess(const MirProgram &program, const MirBody &body,
                     const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding || place.binding == 0 ||
      place.projections.size() != 1 ||
      place.projections[0].kind != MirProjectionKind::Index) {
    return std::nullopt;
  }
  for (const MirPlace &candidate : body.places) {
    if (candidate.id == place.id ||
        candidate.root != MirPlaceRootKind::Binding ||
        candidate.binding != place.binding || !candidate.projections.empty() ||
        candidate.type.kind != SemanticType::Class) {
      continue;
    }
    for (const MirClassInstance &instance : program.classInstances()) {
      if (instance.type != candidate.type) {
        continue;
      }
      const MirValue *index = body.findValue(place.projections[0].index);
      if (index == nullptr) {
        return std::nullopt;
      }
      return ClassSubscriptAccess{
          candidate.id, instance.id, place.projections[0].index,
          place.projections[0].constantIndex, index->info.type};
    }
    return std::nullopt;
  }
  return std::nullopt;
}

// Resolves the unique subscript member for one access direction and
// proves it: a source-defined GTI member of the base class instance with
// the exact receiver mutability and index parameter type, carrying a
// body-name row for its spelling, whose own emitted body contains
// failure terminally — cycles fail closed exactly like the plain-callee
// convention.
[[nodiscard]] const MirFunctionInstance *containedSubscriptMember(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    HirClassInstanceId owner, ReceiverMutability mutability,
    const SemanticType &indexType) {
  const MirFunctionInstance *found = nullptr;
  for (const MirFunctionInstance &candidate : program.functionInstances()) {
    if (!candidate.overloadedOperator ||
        *candidate.overloadedOperator != OverloadedOperator::Subscript ||
        !candidate.owner || *candidate.owner != owner ||
        candidate.receiverMutability != mutability ||
        candidate.parameterTypes.size() != 1 ||
        candidate.parameterTypes.front() != indexType ||
        candidate.linkage != LanguageLinkage::Gti ||
        candidate.definitionKind !=
            MirFunctionInstance::DefinitionKind::Source) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &candidate;
  }
  if (found == nullptr) {
    return nullptr;
  }
  const MirBodyAddress address{.kind = MirBodyKind::Function,
                               .owner = found->id};
  const auto row = std::find_if(
      representations.bodies().begin(), representations.bodies().end(),
      [&](const CppMirBodyNameRepresentation &candidate) {
        return candidate.address == address;
      });
  if (row == representations.bodies().end() || row->spelling.empty()) {
    return nullptr;
  }
  thread_local std::vector<HirFunctionInstanceId> probing;
  if (std::find(probing.begin(), probing.end(), found->id) != probing.end()) {
    return nullptr;
  }
  probing.push_back(found->id);
  // The success form contains terminally inside its own text; the failure
  // form's boundary wrapper keeps the original member name and signature
  // and terminates on a propagated record, so the plain call is exact
  // against either emitted form.
  const CppMirBodyEmitter emitter(program, representations);
  const bool contained = emitter.supportsBodyText(address) ||
                         emitter.supportsFailureBodyText(address);
  probing.pop_back();
  return contained ? found : nullptr;
}

template <typename Enum>
[[nodiscard]] constexpr std::size_t ordinal(Enum value) {
  return static_cast<std::size_t>(value);
}

[[nodiscard]] CppMirBodyEmissionReadiness
mergeReadiness(CppMirBodyEmissionReadiness left,
               CppMirBodyEmissionReadiness right) {
  return ordinal(left) >= ordinal(right) ? left : right;
}

[[nodiscard]] CppMirBodyEmissionReadiness
readinessForIssue(CppMirBodyEmissionIssueKind kind) {
  switch (kind) {
  case CppMirBodyEmissionIssueKind::MissingTypeRepresentation:
  case CppMirBodyEmissionIssueKind::MissingBodyRepresentation:
  case CppMirBodyEmissionIssueKind::MissingSymbolRepresentation:
  case CppMirBodyEmissionIssueKind::MissingEnumRepresentation:
  case CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation:
    return CppMirBodyEmissionReadiness::MissingRepresentation;
  case CppMirBodyEmissionIssueKind::MissingPackExpansionMir:
  case CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir:
  case CppMirBodyEmissionIssueKind::MissingCheckedFailureControlFlow:
  case CppMirBodyEmissionIssueKind::MissingAggregateRollbackMir:
  case CppMirBodyEmissionIssueKind::MissingCallInputScheduleMir:
  case CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir:
  case CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir:
  case CppMirBodyEmissionIssueKind::MissingFailureCleanupMir:
  case CppMirBodyEmissionIssueKind::MissingProgramInitializationMir:
  case CppMirBodyEmissionIssueKind::MissingHostedStartupMir:
    return CppMirBodyEmissionReadiness::MissingMirAuthority;
  case CppMirBodyEmissionIssueKind::InvalidMirProgram:
  case CppMirBodyEmissionIssueKind::InvalidBodyAddress:
  case CppMirBodyEmissionIssueKind::InvalidRepresentationEnum:
  case CppMirBodyEmissionIssueKind::InvalidRepresentationRow:
  case CppMirBodyEmissionIssueKind::DuplicateTypeRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateBodyRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateSymbolRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateEnumRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateCapabilityRepresentation:
  case CppMirBodyEmissionIssueKind::InvalidBodyKind:
  case CppMirBodyEmissionIssueKind::InvalidInstructionKind:
  case CppMirBodyEmissionIssueKind::InvalidOperation:
  case CppMirBodyEmissionIssueKind::InvalidOperandKind:
  case CppMirBodyEmissionIssueKind::InvalidPlaceRootKind:
  case CppMirBodyEmissionIssueKind::InvalidProjectionKind:
  case CppMirBodyEmissionIssueKind::InvalidTerminatorKind:
  case CppMirBodyEmissionIssueKind::Count:
    return CppMirBodyEmissionReadiness::Incoherent;
  }
  return CppMirBodyEmissionReadiness::Incoherent;
}

[[nodiscard]] std::optional<CppMirTypeRepresentationKind>
expectedTypeRepresentation(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Void:
    return CppMirTypeRepresentationKind::Void;
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
    return CppMirTypeRepresentationKind::Scalar;
  case SemanticType::StringView:
    return CppMirTypeRepresentationKind::StringView;
  case SemanticType::NullPtr:
    return CppMirTypeRepresentationKind::NullPointer;
  case SemanticType::RawPointer:
    return CppMirTypeRepresentationKind::RawPointer;
  case SemanticType::Array:
    return CppMirTypeRepresentationKind::FixedArray;
  case SemanticType::Class:
    return CppMirTypeRepresentationKind::Class;
  case SemanticType::Enum:
    return CppMirTypeRepresentationKind::Enum;
  case SemanticType::Reference:
    return CppMirTypeRepresentationKind::Reference;
  case SemanticType::UniqueOwner:
    return CppMirTypeRepresentationKind::UniqueOwner;
  case SemanticType::SharedPointer:
    return CppMirTypeRepresentationKind::SharedPointer;
  case SemanticType::Storage:
  case SemanticType::PrefixStorage:
    return CppMirTypeRepresentationKind::Storage;
  case SemanticType::TypeParameter:
  case SemanticType::TypePack:
  case SemanticType::TypeName:
    return CppMirTypeRepresentationKind::Meta;
  case SemanticType::Function:
    return CppMirTypeRepresentationKind::Function;
  case SemanticType::Lambda:
    return CppMirTypeRepresentationKind::Lambda;
  case SemanticType::Expected:
    return CppMirTypeRepresentationKind::Expected;
  case SemanticType::Unexpected:
    return CppMirTypeRepresentationKind::Unexpected;
  case SemanticType::Unknown:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] bool isInitializerBody(MirBodyKind kind) {
  return kind == MirBodyKind::Module ||
         kind == MirBodyKind::FieldInitializers ||
         kind == MirBodyKind::StaticFieldInitializers;
}

[[nodiscard]] bool isCanonicalNoExecutionInitializer(const MirBody &body) {
  if (!isInitializerBody(body.kind) || body.returnType != SemanticType::Void ||
      body.entry != 1 || body.blocks.size() != 1 || !body.places.empty() ||
      !body.loans.empty() || !body.fullExpressions.empty() ||
      !body.cleanupBoundaries.empty() || !body.dropObligations.empty() ||
      !body.failureRecords.empty() || !body.values.empty() ||
      !body.valueUses.empty()) {
    return false;
  }
  MirBlock expected;
  expected.id = 1;
  expected.terminator.kind = MirTerminatorKind::Exit;
  expected.reachable = true;
  return body.blocks.front() == expected;
}

[[nodiscard]] bool
hasExecutableProgramInitialization(const MirProgram &program) {
  return std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });
}

[[nodiscard]] const MirInstruction *findInstruction(const MirBody &body,
                                                    MirInstructionId id) {
  if (id == 0) {
    return nullptr;
  }
  for (const MirBlock &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [id](const MirInstruction &instruction) {
                       return instruction.id == id;
                     });
    if (found != block.instructions.end()) {
      return &*found;
    }
  }
  return nullptr;
}

// Inline closure chains: a C++ closure type is unnameable, so no
// lambda-typed place or value ever declares a local. A Closure compute
// either feeds an invocation receiver directly or initializes a dedicated
// lambda-typed local whose loads feed further initializations or
// invocation receivers, and every consumer spells the full literal inline
// at its own use. Resolution walks the chain backwards; validation walks
// it forwards and freezes the captured places so a literal spelled at a
// later invocation still captures exactly the values the Closure saw.
[[nodiscard]] bool callableValueInvocation(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call &&
         instruction.intrinsic == IntrinsicKind::None &&
         !instruction.functionTarget && instruction.receiver &&
         instruction.receiver->kind == MirOperandKind::Value &&
         instruction.receiver->type.kind == SemanticType::Lambda;
}

// A callable-parameter invocation stages its receiver place through one
// Load (or Move) whose result feeds exactly the invocation: the call
// spells the place expression (or its std::move) directly, matching the
// compatibility `operation(args)` form with no intermediate copy. Only a
// deduced-callable template emission carries a type row for the place's
// concrete callable type, so this shape stays dormant under production
// rows.
[[nodiscard]] const MirInstruction *callableReceiverStage(const MirBody &body,
                                                          MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr ||
      (definition->kind != MirInstructionKind::Load &&
       definition->kind != MirInstructionKind::Move) ||
      definition->operands.size() != 1 ||
      definition->operands.front().place == 0 || body.usesOf(id).size() != 1) {
    return nullptr;
  }
  const MirPlace *place = body.findPlace(definition->operands.front().place);
  // Only an initially-available place (a parameter) stages: a local
  // carrier written by an Initialize belongs to the fused closure chain
  // and never declares, so spelling it here would name a nonexistent
  // local.
  if (place == nullptr || place->type.kind != SemanticType::Lambda ||
      place->root != MirPlaceRootKind::Binding || !place->projections.empty() ||
      !place->initiallyAvailable) {
    return nullptr;
  }
  return definition;
}

[[nodiscard]] bool deducedCallableCallee(const MirProgram &program,
                                         const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      !instruction.functionTarget ||
      instruction.intrinsic != IntrinsicKind::None) {
    return false;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  return target != nullptr && !target->callableParameters.empty() &&
         target->linkage == LanguageLinkage::Gti &&
         target->definitionKind == MirFunctionInstance::DefinitionKind::Source;
}

// A may-raise free callee whose own body proves the plain success shape:
// its failure is terminally contained inside its own emitted text, so a
// caller in either form calls the plain name and the paired invoke edge
// is a plain goto — the deduced-callable convention generalized to
// concrete free functions. Cycles fail closed: a body currently being
// probed higher in this chain cannot vouch for itself.
[[nodiscard]] bool
terminallyContainedPlainCallee(const MirProgram &program,
                               const CppMirBodyEmissionMap &representations,
                               const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      !instruction.functionTarget ||
      instruction.intrinsic != IntrinsicKind::None) {
    return false;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  if (target == nullptr || target->owner || !target->mayRaiseDefinedFailure ||
      target->linkage != LanguageLinkage::Gti ||
      target->definitionKind != MirFunctionInstance::DefinitionKind::Source ||
      !target->callableParameters.empty() ||
      target->entryKind != ProgramEntryKind::None) {
    return false;
  }
  thread_local std::vector<HirFunctionInstanceId> probing;
  if (std::find(probing.begin(), probing.end(), target->id) != probing.end()) {
    return false;
  }
  probing.push_back(target->id);
  // Only the success shape contains terminally inside its own text; a
  // failure-admitted callee keeps the transformed propagation convention
  // and never claims the plain call.
  const bool contained = CppMirBodyEmitter(program, representations)
                             .supportsBodyText({.kind = MirBodyKind::Function,
                                                .owner = target->id});
  probing.pop_back();
  return contained;
}

// The member analogue of the terminally contained plain callee: a
// may-raise member whose own emitted body proves either form contains
// failure away from the caller — the success shape terminally inside its
// text, or the transformed sibling behind the boundary wrapper that
// keeps the original member name and terminates on a propagated record.
// Either way the caller's plain member call never observes failure, so
// its paired invoke edge is a plain goto. Cycles fail closed.
[[nodiscard]] bool
terminallyContainedMemberCallee(const MirProgram &program,
                                const CppMirBodyEmissionMap &representations,
                                const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      !instruction.functionTarget ||
      instruction.intrinsic != IntrinsicKind::None) {
    return false;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  if (target == nullptr || !target->owner || !target->mayRaiseDefinedFailure ||
      target->linkage != LanguageLinkage::Gti ||
      target->definitionKind != MirFunctionInstance::DefinitionKind::Source ||
      !target->callableParameters.empty() ||
      target->entryKind != ProgramEntryKind::None) {
    return false;
  }
  thread_local std::vector<HirFunctionInstanceId> probing;
  if (std::find(probing.begin(), probing.end(), target->id) != probing.end()) {
    return false;
  }
  probing.push_back(target->id);
  // Success-shape containment only: a failure-admitted member keeps the
  // transformed propagation convention.
  const bool contained = CppMirBodyEmitter(program, representations)
                             .supportsBodyText({.kind = MirBodyKind::Function,
                                                .owner = target->id});
  probing.pop_back();
  return contained;
}

[[nodiscard]] const MirInstruction *callableArgumentStage(const MirBody &body,
                                                          MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr || definition->kind != MirInstructionKind::Load ||
      definition->operands.size() != 1 ||
      definition->operands.front().place == 0 || body.usesOf(id).size() != 1 ||
      body.usesOf(id).front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  const MirInstruction *user =
      findInstruction(body, body.usesOf(id).front().instruction);
  if (user == nullptr || user->kind != MirInstructionKind::Call ||
      !user->functionTarget || user->intrinsic != IntrinsicKind::None) {
    return nullptr;
  }
  const MirPlace *place = body.findPlace(definition->operands.front().place);
  if (place == nullptr || place->type.kind != SemanticType::Lambda ||
      place->root != MirPlaceRootKind::Binding || !place->projections.empty() ||
      !place->initiallyAvailable) {
    return nullptr;
  }
  return definition;
}

// A loan-staged call input carries a borrowed entry parameter into a staged
// invocation: the call spells the dereferenced pointer carrier (ADR 018 §4)
// and the staged reference value never materializes as a local.
[[nodiscard]] const MirInstruction *loanStagedCallInput(const MirBody &body,
                                                        MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr ||
      definition->kind != MirInstructionKind::CallInput ||
      definition->receiver || definition->operands.size() != 1 ||
      definition->operands.front().kind != MirOperandKind::Loan ||
      definition->operands.front().loan == 0 ||
      body.findLoan(definition->operands.front().loan) == nullptr ||
      body.usesOf(id).size() != 1 ||
      body.usesOf(id).front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  return definition;
}

// The source feeding a value-staged temporary: a Copy operand names its
// place directly, and a Value operand fed by a Move of a place names the
// moved source (spelled std::move(source) at the consuming call).
struct StagedTemporarySource {
  const MirPlace *place = nullptr;
  bool moved = false;
};
[[nodiscard]] StagedTemporarySource
stagedTemporarySourceFor(const MirBody &body, const MirInstruction &stage) {
  const MirOperand &operand = stage.operands.front();
  if (operand.kind == MirOperandKind::Copy && operand.place != 0) {
    return {body.findPlace(operand.place), false};
  }
  if (operand.kind == MirOperandKind::Value && operand.value != 0 &&
      body.usesOf(operand.value).size() == 1) {
    const MirValue *value = body.findValue(operand.value);
    const MirInstruction *definition =
        value == nullptr ? nullptr : findInstruction(body, value->definition);
    if (definition != nullptr && definition->kind == MirInstructionKind::Move &&
        definition->operands.size() == 1 &&
        definition->operands.front().kind == MirOperandKind::Move &&
        definition->operands.front().place != 0) {
      return {body.findPlace(definition->operands.front().place), true};
    }
  }
  return {};
}

// A bare value-rooted place that no instruction, loan, or terminator
// references is a pure root record: the rooted value flows through its
// own uses and the place spells nothing, so it needs no declaration and
// no representation row.
[[nodiscard]] bool unreferencedValueRootedPlace(const MirBody &body,
                                                const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Value || !place.projections.empty() ||
      place.type.kind != SemanticType::Class) {
    return false;
  }
  for (const MirLoan &loan : body.loans) {
    if (loan.source == place.id) {
      return false;
    }
  }
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.value && block.terminator.value->place == place.id) {
      return false;
    }
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.destination == place.id ||
          (instruction.receiver && instruction.receiver->place == place.id) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return operand.place == place.id;
                      })) {
        return false;
      }
    }
  }
  return true;
}

// The bounded pack-forwarding shape: an operand-less PackExpansion whose
// TypePack value feeds exactly one allocation-intrinsic call. The pack's
// flattened parameters spell at that call and nothing else references
// the pack, so neither the expansion nor the pack value materializes.
[[nodiscard]] const MirInstruction *
packExpansionForwardedOnce(const MirBody &body, MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr ||
      definition->kind != MirInstructionKind::Compute ||
      definition->operation != MirOperation::PackExpansion ||
      !definition->operands.empty() ||
      value->info.type.kind != SemanticType::TypePack ||
      body.usesOf(id).size() != 1 ||
      body.usesOf(id).front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  const MirInstruction *user =
      findInstruction(body, body.usesOf(id).front().instruction);
  if (user == nullptr || user->kind != MirInstructionKind::Call ||
      user->intrinsic != IntrinsicKind::AllocateUniqueOwner ||
      user->operands.size() != 1 || user->receiver || !user->result) {
    return nullptr;
  }
  return definition;
}

// A by-value argument staging temporary: one CallInput carries a source
// place — copied, or moved through its staged value — into a bare
// class-typed temporary that nothing else references, and the staged
// value feeds exactly one call. The consuming call spells the source
// place (moved sources under std::move) and C++ materializes the
// temporary at the call boundary, exactly like the compatibility call.
[[nodiscard]] const MirInstruction *
copyStageForTemporary(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Temporary || !place.projections.empty() ||
      place.type.kind != SemanticType::Class) {
    return nullptr;
  }
  const MirInstruction *stage = nullptr;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const bool references =
          instruction.destination == place.id ||
          (instruction.receiver && instruction.receiver->place == place.id) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return operand.place == place.id;
                      });
      if (!references) {
        continue;
      }
      if (stage != nullptr ||
          instruction.kind != MirInstructionKind::CallInput ||
          instruction.destination != place.id || !instruction.result ||
          instruction.receiver || instruction.operands.size() != 1 ||
          stagedTemporarySourceFor(body, instruction).place == nullptr) {
        return nullptr;
      }
      stage = &instruction;
    }
  }
  if (stage == nullptr || body.usesOf(*stage->result).size() != 1 ||
      (body.usesOf(*stage->result).front().kind !=
           MirValueUseKind::InstructionOperand &&
       body.usesOf(*stage->result).front().kind !=
           MirValueUseKind::InstructionReceiver)) {
    return nullptr;
  }
  const MirInstruction *user =
      findInstruction(body, body.usesOf(*stage->result).front().instruction);
  if (user == nullptr || user->kind != MirInstructionKind::Call) {
    return nullptr;
  }
  return stage;
}

// The value-side view of the same shape, keyed by the staged value.
[[nodiscard]] const MirInstruction *copyStagedCallInput(const MirBody &body,
                                                        MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr ||
      definition->kind != MirInstructionKind::CallInput ||
      !definition->destination) {
    return nullptr;
  }
  const MirPlace *destination = body.findPlace(*definition->destination);
  if (destination == nullptr ||
      copyStageForTemporary(body, *destination) != definition) {
    return nullptr;
  }
  return definition;
}

[[nodiscard]] const MirInstruction *closureChainDefinition(const MirBody &body,
                                                           MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr) {
    return nullptr;
  }
  if (definition->kind == MirInstructionKind::Compute &&
      definition->operation == MirOperation::Closure) {
    return definition;
  }
  if (definition->kind != MirInstructionKind::Load ||
      definition->operands.size() != 1) {
    return nullptr;
  }
  const MirPlaceId carrier = definition->operands.front().place;
  const MirPlace *place = body.findPlace(carrier);
  if (place == nullptr || place->type.kind != SemanticType::Lambda) {
    return nullptr;
  }
  const MirInstruction *initialize = nullptr;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &candidate : block.instructions) {
      if (candidate.kind == MirInstructionKind::Initialize &&
          candidate.destination && *candidate.destination == carrier) {
        if (initialize != nullptr) {
          return nullptr;
        }
        initialize = &candidate;
      }
    }
  }
  if (initialize == nullptr || initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value) {
    return nullptr;
  }
  return closureChainDefinition(body, initialize->operands.front().value);
}

// A Drop of a unique-owner place whose value an earlier Move in the
// same block unconditionally consumed: the C++ local is moved-from, its
// representation's scope-end destruction is a no-op by construction
// (a null owner deletes nothing), so the Drop spells as a comment.
[[nodiscard]] bool movedOutOwnerDrop(const MirBody &body,
                                     const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Drop ||
      !instruction.destination) {
    return false;
  }
  const MirPlace *destination = body.findPlace(*instruction.destination);
  if (destination == nullptr ||
      destination->type.kind != SemanticType::UniqueOwner ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty()) {
    return false;
  }
  for (const MirLifecycleEvent &event : instruction.lifecycle) {
    if (event.conditional) {
      return false;
    }
  }
  const MirBlock *block = nullptr;
  for (const MirBlock &candidate : body.blocks) {
    for (const MirInstruction &member : candidate.instructions) {
      if (member.id == instruction.id) {
        block = &candidate;
      }
    }
  }
  if (block == nullptr) {
    return false;
  }
  bool moved = false;
  for (const MirInstruction &candidate : block->instructions) {
    if (candidate.id == instruction.id) {
      break;
    }
    if (candidate.kind == MirInstructionKind::Move &&
        candidate.operands.size() == 1 &&
        candidate.operands.front().kind == MirOperandKind::Move &&
        candidate.operands.front().place == destination->id) {
      moved = true;
    }
  }
  return moved;
}

// A storage growth step moves the replacement value out of its staging
// local and stores it into the receiver field; the store is the value's
// single consuming use and spells as a C++ move-assignment, so the Drop
// of the moved-from value afterwards is a no-op by representation and
// spells as a comment. The proof demands the exact shape: the dropped
// place is a projection-free Value-rooted storage place, every lifecycle
// event on the Drop is unconditional non-failure cleanup, the value is
// read exactly once in the whole body — by an Initialize or Assign that
// precedes the Drop in its own block — and no other place roots at it.
[[nodiscard]] bool
storeConsumedStorageValueDrop(const MirBody &body,
                              const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Drop ||
      !instruction.destination) {
    return false;
  }
  const MirPlace *destination = body.findPlace(*instruction.destination);
  if (destination == nullptr ||
      (destination->type.kind != SemanticType::Storage &&
       destination->type.kind != SemanticType::PrefixStorage) ||
      destination->root != MirPlaceRootKind::Value ||
      !destination->projections.empty()) {
    return false;
  }
  for (const MirLifecycleEvent &event : instruction.lifecycle) {
    if (event.conditional || event.failureCleanup) {
      return false;
    }
  }
  for (const MirPlace &place : body.places) {
    if (place.id != destination->id && place.root == MirPlaceRootKind::Value &&
        place.value == destination->value) {
      return false;
    }
  }
  const MirInstruction *consumer = nullptr;
  std::size_t reads = 0;
  for (const MirBlock &candidate : body.blocks) {
    for (const MirInstruction &member : candidate.instructions) {
      for (const MirOperand &operand : member.operands) {
        if (operand.kind == MirOperandKind::Value &&
            operand.value == destination->value) {
          ++reads;
          consumer = &member;
        }
      }
    }
    if (candidate.terminator.value &&
        candidate.terminator.value->kind == MirOperandKind::Value &&
        candidate.terminator.value->value == destination->value) {
      return false;
    }
  }
  if (reads != 1 || consumer == nullptr ||
      (consumer->kind != MirInstructionKind::Initialize &&
       consumer->kind != MirInstructionKind::Assign) ||
      !consumer->destination) {
    return false;
  }
  // The consuming store precedes the Drop inside the Drop's own block.
  bool sawConsumer = false;
  for (const MirBlock &candidate : body.blocks) {
    for (const MirInstruction &member : candidate.instructions) {
      if (member.id == consumer->id) {
        sawConsumer = true;
      }
      if (member.id == instruction.id) {
        return sawConsumer;
      }
    }
    sawConsumer = false;
  }
  return false;
}

// A Drop is trivial when every drop obligation governing it — the ones
// its lifecycle events name and the ones anchored on its destination
// place — carries neither a destructor nor active cleanup: C++ scope-end
// destruction of the declared local (or nothing, for a fused closure) is
// exactly the verified semantics, so the Drop spells as a comment. An
// unresolvable obligation fails closed.
[[nodiscard]] bool trivialMirDrop(const MirBody &body,
                                  const MirInstruction &instruction) {
  const MirPlace *destination = instruction.destination
                                    ? body.findPlace(*instruction.destination)
                                    : nullptr;
  if (destination == nullptr) {
    return false;
  }
  // A slot-shaped place stays on the lifetime-slot protocol regardless of
  // obligation triviality: the slot's engage/destroy pairing is the
  // verified escape check, and skipping its destroy would trip it.
  if (destination->root == MirPlaceRootKind::Binding &&
      destination->projections.empty() &&
      (destination->type.kind == SemanticType::Class ||
       destination->type.kind == SemanticType::Storage ||
       destination->type.kind == SemanticType::PrefixStorage)) {
    return false;
  }
  const auto trivialObligation = [&](MirDropObligationId id) {
    if (id == 0) {
      return true;
    }
    for (const MirDropObligation &obligation : body.dropObligations) {
      if (obligation.id == id) {
        return !obligation.dropType.destructor &&
               !obligation.dropType.requiresActiveCleanup;
      }
    }
    return false;
  };
  bool governed = false;
  for (const MirLifecycleEvent &event : instruction.lifecycle) {
    governed = governed || event.source != 0 || event.target != 0;
    if (!trivialObligation(event.source) || !trivialObligation(event.target)) {
      return false;
    }
  }
  for (const MirDropObligation &obligation : body.dropObligations) {
    if (obligation.place != *instruction.destination) {
      continue;
    }
    governed = true;
    if (obligation.dropType.destructor ||
        obligation.dropType.requiresActiveCleanup) {
      return false;
    }
  }
  return governed;
}

// Plain-shape checked arithmetic: the compatibility path's terminal helper
// family checks and contains the defined failure itself and never returns
// on failure. Inline lambda literals keep exactly that spelling, so their
// MIR failure edges are unreachable in emitted text.
[[nodiscard]] std::string_view
cppMirTerminalCheckedHelperSpelling(MirOperation operation) {
  switch (operation) {
  case MirOperation::Add:
    return "::gti_internal::backend::add";
  case MirOperation::Subtract:
    return "::gti_internal::backend::subtract";
  case MirOperation::Multiply:
    return "::gti_internal::backend::multiply";
  case MirOperation::Divide:
    return "::gti_internal::backend::divide";
  case MirOperation::Remainder:
    return "::gti_internal::backend::modulo";
  case MirOperation::Negate:
    return "::gti_internal::backend::negate";
  default:
    return {};
  }
}

// Forward half of one Closure's fused chain. Captured places must stay
// frozen after the Closure: their only writes are entry-block Initializes
// that precede it, nothing loans or drops them, and the entry block is
// never re-entered, so a literal spelled at any later invocation captures
// the same values the Closure saw. Move captures collapse to exactly one
// direct same-block invocation because a duplicated or delayed literal
// would move a captured place twice or after an interleaved failure edge.
[[nodiscard]] bool closureChainAdmits(const MirProgram &program,
                                      const MirBody &body,
                                      const MirInstruction &closure) {
  const MirLambdaInstance *lambda =
      closure.lambdaTarget ? program.findLambda(*closure.lambdaTarget)
                           : nullptr;
  const MirValue *result =
      closure.result ? body.findValue(*closure.result) : nullptr;
  if (lambda == nullptr || result == nullptr ||
      closure.operands.size() != lambda->captureSymbols.size() ||
      closure.operands.size() != lambda->captureModes.size() ||
      closure.operands.size() != lambda->captureTypes.size()) {
    return false;
  }
  bool movesCapture = false;
  for (std::size_t index = 0; index < closure.operands.size(); ++index) {
    const MirOperand &operand = closure.operands[index];
    const LambdaCaptureMode mode = lambda->captureModes[index];
    const bool modeMatches = (mode == LambdaCaptureMode::Copy &&
                              operand.kind == MirOperandKind::Copy) ||
                             (mode == LambdaCaptureMode::Move &&
                              operand.kind == MirOperandKind::Move);
    movesCapture = movesCapture || mode == LambdaCaptureMode::Move;
    const MirPlace *captured =
        operand.place == 0 ? nullptr : body.findPlace(operand.place);
    // A captured lambda or owned object would itself need the unnameable
    // or slot-managed local this chain exists to avoid; both decline.
    if (!modeMatches || captured == nullptr ||
        captured->root != MirPlaceRootKind::Binding ||
        !captured->projections.empty() ||
        captured->type.kind == SemanticType::Lambda ||
        captured->type.kind == SemanticType::Class ||
        captured->type.kind == SemanticType::Storage ||
        captured->type.kind == SemanticType::PrefixStorage) {
      return false;
    }
    for (const MirLoan &loan : body.loans) {
      if (loan.source == captured->id) {
        return false;
      }
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &writer : block.instructions) {
        if (!writer.destination || *writer.destination != captured->id) {
          continue;
        }
        if (writer.kind != MirInstructionKind::Initialize ||
            block.id != body.entry || result->definitionBlock != body.entry) {
          return false;
        }
        bool writerPrecedes = false;
        for (const MirInstruction &ordered : block.instructions) {
          if (ordered.id == writer.id) {
            writerPrecedes = true;
            break;
          }
          if (ordered.id == closure.id) {
            break;
          }
        }
        if (!writerPrecedes) {
          return false;
        }
      }
    }
  }
  if (!closure.operands.empty()) {
    for (const MirBlock &block : body.blocks) {
      const MirTerminator &terminator = block.terminator;
      if (terminator.target == body.entry ||
          terminator.elseTarget == body.entry) {
        return false;
      }
      for (const MirSwitchTarget &target : terminator.switchTargets) {
        if (target.target == body.entry) {
          return false;
        }
      }
    }
  }
  // Forward walk: every transitive consumer is an Initialize into a fresh
  // single-write lambda local or the receiver of an invocation.
  std::size_t invocations = 0;
  bool directOnly = true;
  std::vector<MirPlaceId> visitedCarriers;
  std::vector<MirValueId> pending{*closure.result};
  while (!pending.empty()) {
    const MirValueId current = pending.back();
    pending.pop_back();
    for (const MirValueUse &use : body.usesOf(current)) {
      const MirInstruction *user = findInstruction(body, use.instruction);
      if (use.kind == MirValueUseKind::InstructionReceiver) {
        if (user == nullptr || !callableValueInvocation(*user)) {
          return false;
        }
        ++invocations;
        directOnly = directOnly && current == *closure.result &&
                     use.block == result->definitionBlock;
        continue;
      }
      if (use.kind == MirValueUseKind::InstructionOperand && user != nullptr &&
          deducedCallableCallee(program, *user)) {
        // The literal spells inline as the template call's deduced
        // callable argument, exactly like the compatibility call.
        ++invocations;
        directOnly = directOnly && current == *closure.result &&
                     use.block == result->definitionBlock;
        continue;
      }
      if (use.kind != MirValueUseKind::InstructionOperand || user == nullptr ||
          user->kind != MirInstructionKind::Initialize || !user->destination) {
        return false;
      }
      directOnly = false;
      const MirPlaceId carrier = *user->destination;
      const MirPlace *place = body.findPlace(carrier);
      if (place == nullptr || place->root != MirPlaceRootKind::Binding ||
          !place->projections.empty() ||
          place->type.kind != SemanticType::Lambda ||
          std::find(visitedCarriers.begin(), visitedCarriers.end(), carrier) !=
              visitedCarriers.end()) {
        return false;
      }
      visitedCarriers.push_back(carrier);
      for (const MirLoan &loan : body.loans) {
        if (loan.source == carrier) {
          return false;
        }
      }
      // The carrier's whole life is this one Initialize plus loads whose
      // results rejoin the chain.
      for (const MirBlock &block : body.blocks) {
        for (const MirInstruction &reference : block.instructions) {
          if (reference.destination && *reference.destination == carrier &&
              reference.id != user->id) {
            return false;
          }
          bool readsCarrier =
              reference.receiver && reference.receiver->place == carrier;
          for (const MirOperand &operand : reference.operands) {
            readsCarrier = readsCarrier || operand.place == carrier;
          }
          if (!readsCarrier || reference.id == user->id) {
            continue;
          }
          if (reference.kind != MirInstructionKind::Load || !reference.result) {
            return false;
          }
          pending.push_back(*reference.result);
        }
      }
    }
  }
  if (movesCapture && (invocations != 1 || !directOnly)) {
    return false;
  }
  return true;
}

[[nodiscard]] const MirInstruction *definitionFor(const MirBody &body,
                                                  const MirOperand &operand) {
  if (operand.kind != MirOperandKind::Value || operand.value == 0) {
    return nullptr;
  }
  const MirValue *value = body.findValue(operand.value);
  return value == nullptr ? nullptr : findInstruction(body, value->definition);
}

[[nodiscard]] bool hasExactCallInput(const MirBody &body,
                                     const MirOperand &operand,
                                     HirValueId callSite, MirCallInputRole role,
                                     std::size_t index) {
  const MirInstruction *input = definitionFor(body, operand);
  return input != nullptr && input->kind == MirInstructionKind::CallInput &&
         input->callSite == callSite && input->callInputRole == role &&
         input->callInputIndex == index;
}

// A borrow-staged call input carries a read borrow of a place instead of a
// scalar value: the call spells the place expression directly, so the
// staged value never materializes as a local.
[[nodiscard]] const MirInstruction *
borrowStagedCallInput(const MirBody &body, const MirOperand &operand) {
  const MirInstruction *input = definitionFor(body, operand);
  return input != nullptr && input->kind == MirInstructionKind::CallInput &&
                 input->operands.size() == 1 &&
                 (input->operands.front().kind == MirOperandKind::BorrowRead ||
                  input->operands.front().kind ==
                      MirOperandKind::BorrowWrite) &&
                 input->operands.front().place != 0
             ? input
             : nullptr;
}

[[nodiscard]] bool isBorrowStagedResult(const MirBody &body,
                                        const MirValue &value) {
  const MirInstruction *definition = findInstruction(body, value.definition);
  return definition != nullptr &&
         definition->kind == MirInstructionKind::CallInput &&
         definition->operands.size() == 1 &&
         (definition->operands.front().kind == MirOperandKind::BorrowRead ||
          definition->operands.front().kind == MirOperandKind::BorrowWrite) &&
         definition->operands.front().place != 0;
}

[[nodiscard]] bool hasCompleteCallInputSchedule(const MirBody &body,
                                                const MirInstruction &call) {
  if (call.callSite == 0) {
    // HostedStartup is compiler-generated and has no source HIR call site.
    // Its nonzero operation tag is closed over the exact call/input schedule
    // by verifyMirProgram before this private classifier runs.
    if (body.kind == MirBodyKind::HostedStartup &&
        call.hostedStartupOperation != 0) {
      return true;
    }
    // A compiler-generated call carries its receiver as a place-carrying
    // staged borrow and its arguments as direct value operands: no
    // CallInput stages exist by construction, so the operand list itself
    // is the complete schedule.
    const bool stagedReceiver =
        call.receiver &&
        (call.receiver->kind == MirOperandKind::BorrowRead ||
         call.receiver->kind == MirOperandKind::BorrowWrite) &&
        call.receiver->place != 0;
    const bool directValues = std::all_of(
        call.operands.begin(), call.operands.end(),
        [](const MirOperand &operand) {
          return operand.kind == MirOperandKind::Value && operand.value != 0;
        });
    return (!call.receiver || stagedReceiver) && directValues;
  }
  if (call.receiver && !hasExactCallInput(body, *call.receiver, call.callSite,
                                          MirCallInputRole::Receiver, 0)) {
    return false;
  }
  for (std::size_t index = 0; index < call.operands.size(); ++index) {
    if (!hasExactCallInput(body, call.operands[index], call.callSite,
                           MirCallInputRole::Argument, index)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
isHostedStartupArgumentIndexAdvance(const MirBody &body,
                                    const MirInstruction &instruction) {
  // The hosted-startup verifier binds every nonzero operation tag to one
  // exact plan row. It is therefore the sole authority for the generated
  // Modify/PreIncrement schedule; source Modify remains unsupported here.
  return body.kind == MirBodyKind::HostedStartup &&
         instruction.hostedStartupOperation != 0 &&
         instruction.kind == MirInstructionKind::Modify &&
         instruction.operation == MirOperation::PreIncrement;
}

[[nodiscard]] bool instructionHasInvoke(const MirBlock &block,
                                        const MirInstruction &instruction) {
  return block.terminator.kind == MirTerminatorKind::Invoke &&
         block.terminator.invokeInstruction == instruction.id;
}

// True when the constructor's verified MIR carries the complete rollback
// authority for its owner: no state-bearing bases, no unarmed subobject
// transfer (the body still routes failure edges), and every declared field
// with a non-trivial drop armed exactly one ConstructionRollback obligation.
[[nodiscard]] bool
constructorRollbackCovered(const MirConstructorInstance &constructor,
                           const MirClassInstance *owner);

[[nodiscard]] bool classHasStateBearingBase(const MirClassInstance &instance) {
  return std::any_of(
      instance.bases.begin(), instance.bases.end(),
      [](const HirBaseInstance &base) { return !base.interface; });
}

// A constructor body with no failure records and no Invoke terminators
// has no within-body failure path at all: every failure source inside it
// terminates at its own site, so partial-construction rollback is
// vacuously complete. The shared lowering predicate stays untouched —
// this exemption is a backend emission fact, not a lowering decision.
[[nodiscard]] bool constructorBodyFailureEdgeFree(const MirBody &body) {
  return body.failureRecords.empty() &&
         std::none_of(
             body.blocks.begin(), body.blocks.end(), [](const MirBlock &block) {
               return block.terminator.kind == MirTerminatorKind::Invoke;
             });
}

[[nodiscard]] std::string_view
constructorRollbackGap(const MirConstructorInstance &constructor,
                       const MirClassInstance *owner) {
  if (owner == nullptr) {
    return "constructor lost its owner instance";
  }
  if (classHasStateBearingBase(*owner)) {
    return "owner carries a state-bearing base subobject";
  }
  if (constructorBodyFailureEdgeFree(constructor.body)) {
    return {};
  }
  if (!mirBodyRoutesFailureEdges(constructor.body)) {
    return "constructor body does not route its failure edges";
  }
  for (const MirClassFieldInfo &field : owner->declaredFields) {
    if (field.dropKind == DropKind::Trivial) {
      continue;
    }
    const bool armed = std::any_of(
        constructor.body.dropObligations.begin(),
        constructor.body.dropObligations.end(),
        [&](const MirDropObligation &obligation) {
          if (obligation.kind != MirDropObligationKind::ConstructionRollback) {
            return false;
          }
          const MirPlace *place = constructor.body.findPlace(obligation.place);
          return place != nullptr && place->projections.size() == 1 &&
                 place->projections.front().field == field.symbol;
        });
    if (!armed) {
      return "a non-trivial field carries no construction-rollback "
             "obligation";
    }
  }
  return {};
}

bool constructorRollbackCovered(const MirConstructorInstance &constructor,
                                const MirClassInstance *owner) {
  if (owner == nullptr || classHasStateBearingBase(*owner)) {
    return false;
  }
  if (constructorBodyFailureEdgeFree(constructor.body)) {
    return true;
  }
  if (!mirBodyRoutesFailureEdges(constructor.body)) {
    return false;
  }
  for (const MirClassFieldInfo &field : owner->declaredFields) {
    if (field.dropKind == DropKind::Trivial) {
      continue;
    }
    const bool armed = std::any_of(
        constructor.body.dropObligations.begin(),
        constructor.body.dropObligations.end(),
        [&](const MirDropObligation &obligation) {
          if (obligation.kind != MirDropObligationKind::ConstructionRollback) {
            return false;
          }
          const MirPlace *place = constructor.body.findPlace(obligation.place);
          return place != nullptr && place->projections.size() == 1 &&
                 place->projections.front().field == field.symbol;
        });
    if (!armed) {
      return false;
    }
  }
  return true;
}

class BodyAnalysisBuilder {
public:
  BodyAnalysisBuilder(const MirProgram &program,
                      const CppMirBodyEmissionMap &representations,
                      MirBodyAddress address)
      : program(program), representations(representations) {
    result.body = address;
    result.readiness = CppMirBodyEmissionReadiness::Ready;
  }

  [[nodiscard]] CppMirBodyEmissionAnalysis run(bool validateProgramAndMap) {
    if (validateProgramAndMap) {
      validateProgram();
      validateRepresentations();
    }

    const MirBody *body = findMirBody(program, result.body);
    if (body == nullptr || body->kind != result.body.kind) {
      add(CppMirBodyEmissionIssueKind::InvalidBodyAddress, 0, 0,
          "MIR body address does not resolve to its exact core owner");
      return std::move(result);
    }
    if (classifyCppMirBodyKind(body->kind) == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidBodyKind, 0, 0,
          "MIR body kind is outside the exhaustive emitter vocabulary");
      return std::move(result);
    }

    scanOwnerMetadata(*body);
    scanBody(*body);
    return std::move(result);
  }

  void validateProgram() {
    const MirVerificationResult verification = verifyMirProgram(program);
    if (!program.valid() || !verification.valid()) {
      if (verification.errors.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidMirProgram, 0, 0,
            "MIR program is not marked valid");
        return;
      }
      for (const MirVerificationError &error : verification.errors) {
        add(CppMirBodyEmissionIssueKind::InvalidMirProgram, error.block,
            error.instruction, error.message);
      }
    }
  }

  void validateRepresentations() {
    for (std::size_t index = 0; index < representations.types().size();
         ++index) {
      const CppMirTypeRepresentation &row = representations.types()[index];
      const std::optional<CppMirTypeRepresentationKind> expected =
          expectedTypeRepresentation(row.type);
      if (!expected ||
          ordinal(row.kind) >= ordinal(CppMirTypeRepresentationKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, 0, 0,
            "type row has an invalid semantic or representation kind");
      } else if (*expected != row.kind || row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "type row disagrees with the exact semantic type");
      }
      if (std::find_if(representations.types().begin(),
                       representations.types().begin() + index,
                       [&](const CppMirTypeRepresentation &prior) {
                         return prior.type == row.type;
                       }) != representations.types().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateTypeRepresentation, 0, 0,
            "copied map contains duplicate exact type rows");
      }
    }

    for (std::size_t index = 0; index < representations.bodies().size();
         ++index) {
      const CppMirBodyNameRepresentation &row = representations.bodies()[index];
      if (findMirBody(program, row.address) == nullptr ||
          row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "body-name row is stale or empty");
      }
      if (std::find_if(representations.bodies().begin(),
                       representations.bodies().begin() + index,
                       [&](const CppMirBodyNameRepresentation &prior) {
                         return prior.address == row.address;
                       }) != representations.bodies().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateBodyRepresentation, 0, 0,
            "copied map contains duplicate body-name rows");
      }
    }

    for (std::size_t index = 0; index < representations.symbols().size();
         ++index) {
      const CppMirSymbolRepresentation &row = representations.symbols()[index];
      const bool enumValid =
          ordinal(row.kind) < ordinal(CppMirSymbolRepresentationKind::Count);
      const bool ownerValid =
          row.kind == CppMirSymbolRepresentationKind::Storage || row.owner != 0;
      const bool ordinalValid =
          row.kind == CppMirSymbolRepresentationKind::Capture
              ? row.ordinal != 0
              : row.ordinal == 0;
      if (!enumValid) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, 0, 0,
            "symbol row has an invalid representation kind");
      } else if (!ownerValid || !ordinalValid || row.symbol == 0 ||
                 row.type == SemanticType::Unknown || row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "symbol row has an invalid owner, identity, type, or spelling");
      }
      if (std::find_if(representations.symbols().begin(),
                       representations.symbols().begin() + index,
                       [&](const CppMirSymbolRepresentation &prior) {
                         return prior.kind == row.kind &&
                                prior.owner == row.owner &&
                                prior.symbol == row.symbol &&
                                prior.ordinal == row.ordinal;
                       }) != representations.symbols().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateSymbolRepresentation, 0, 0,
            "copied map contains duplicate symbol rows");
      }
    }

    for (std::size_t index = 0; index < representations.enums().size();
         ++index) {
      const CppMirEnumRepresentation &row = representations.enums()[index];
      if (row.owner == 0 || row.spelling.empty() ||
          row.underlyingType == SemanticType::Unknown) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "enum row has an invalid owner, underlying type, or spelling");
      }
      if (std::find_if(representations.enums().begin(),
                       representations.enums().begin() + index,
                       [&](const CppMirEnumRepresentation &prior) {
                         return prior.owner == row.owner;
                       }) != representations.enums().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateEnumRepresentation, 0, 0,
            "copied map contains duplicate enum rows");
      }
      for (std::size_t variant = 0; variant < row.payloadVariants.size();
           ++variant) {
        const CppMirPayloadVariantRepresentation &current =
            row.payloadVariants[variant];
        if (current.spelling.empty() ||
            std::any_of(current.fieldTypes.begin(), current.fieldTypes.end(),
                        [](const SemanticType &type) {
                          return type == SemanticType::Unknown;
                        })) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
              "payload-variant row has an empty spelling or unknown field");
        }
        if (std::find_if(row.payloadVariants.begin(),
                         row.payloadVariants.begin() + variant,
                         [&](const CppMirPayloadVariantRepresentation &prior) {
                           return prior.index == current.index;
                         }) != row.payloadVariants.begin() + variant) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
              "enum row contains a duplicate payload variant index");
        }
      }
    }

    for (std::size_t index = 0; index < representations.capabilities().size();
         ++index) {
      const CppMirEmissionCapabilityRepresentation &row =
          representations.capabilities()[index];
      if (ordinal(row.kind) >= ordinal(CppMirEmissionCapabilityKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, 0, 0,
            "capability row has an invalid representation kind");
      } else if (row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "capability row has an empty helper spelling");
      }
      if (std::find_if(
              representations.capabilities().begin(),
              representations.capabilities().begin() + index,
              [&](const CppMirEmissionCapabilityRepresentation &prior) {
                return prior.kind == row.kind;
              }) != representations.capabilities().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateCapabilityRepresentation, 0,
            0, "copied map contains duplicate capability rows");
      }
    }
  }

  [[nodiscard]] CppMirBodyEmissionAnalysis finishValidation() {
    return std::move(result);
  }

private:
  void add(CppMirBodyEmissionIssueKind kind, MirBlockId block,
           MirInstructionId instruction, std::string detail) {
    const auto duplicate =
        std::find_if(result.issues.begin(), result.issues.end(),
                     [&](const CppMirBodyEmissionIssue &issue) {
                       return issue.kind == kind && issue.block == block &&
                              issue.instruction == instruction &&
                              issue.detail == detail;
                     });
    if (duplicate != result.issues.end()) {
      return;
    }
    result.readiness =
        mergeReadiness(result.readiness, readinessForIssue(kind));
    result.issues.push_back({.kind = kind,
                             .body = result.body,
                             .block = block,
                             .instruction = instruction,
                             .detail = std::move(detail)});
  }

  [[nodiscard]] const CppMirTypeRepresentation *
  findType(const SemanticType &type) const {
    const auto found = std::find_if(
        representations.types().begin(), representations.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    return found == representations.types().end() ? nullptr : &*found;
  }

  void requireType(const SemanticType &type, MirBlockId block = 0,
                   MirInstructionId instruction = 0) {
    if (type == SemanticType::Unknown) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "executable MIR references an unknown semantic type");
      return;
    }
    // A C++ closure type is unnameable, so Lambda-kind types are row-free
    // by design: the fused closure chain and the deduced-callable
    // template vocabularies own every spelling that touches one.
    if (type.kind == SemanticType::Lambda) {
      return;
    }
    const CppMirTypeRepresentation *row = findType(type);
    if (row == nullptr) {
      add(CppMirBodyEmissionIssueKind::MissingTypeRepresentation, block,
          instruction, "copied map has no row for an exact MIR type");
    } else if (expectedTypeRepresentation(type) != row->kind ||
               row->spelling.empty()) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "copied type row is stale or structurally mismatched");
    }
    for (const SemanticType &argument : type.arguments) {
      requireType(argument, block, instruction);
    }
    for (const SemanticType &argument : type.lambdaEnclosingClassTypes) {
      requireType(argument, block, instruction);
    }
    for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
      requireType(argument, block, instruction);
    }
  }

  void requireBody(MirBodyAddress address, MirBlockId block = 0,
                   MirInstructionId instruction = 0) {
    const auto found = std::find_if(
        representations.bodies().begin(), representations.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == address;
        });
    if (found == representations.bodies().end()) {
      add(CppMirBodyEmissionIssueKind::MissingBodyRepresentation, block,
          instruction,
          "copied map has no emitted name for an exact MIR body target");
    } else if (found->spelling.empty()) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "body-name row has an empty spelling");
    }
  }

  [[nodiscard]] const CppMirSymbolRepresentation *
  findSymbol(CppMirSymbolRepresentationKind kind, std::size_t owner,
             SymbolId symbol, std::size_t ordinalValue = 0,
             bool *ambiguousStorage = nullptr) const {
    if (ambiguousStorage != nullptr) {
      *ambiguousStorage = false;
    }
    if (kind == CppMirSymbolRepresentationKind::Storage && owner == 0) {
      const CppMirSymbolRepresentation *only = nullptr;
      std::size_t matches = 0;
      for (const CppMirSymbolRepresentation &row : representations.symbols()) {
        if (row.kind != kind || row.symbol != symbol ||
            row.ordinal != ordinalValue) {
          continue;
        }
        only = &row;
        ++matches;
      }
      if (ambiguousStorage != nullptr) {
        *ambiguousStorage = matches > 1;
      }
      return matches == 1 ? only : nullptr;
    }

    const auto exact = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == kind && row.owner == owner &&
                 row.symbol == symbol && row.ordinal == ordinalValue;
        });
    if (exact != representations.symbols().end()) {
      return &*exact;
    }
    if (kind == CppMirSymbolRepresentationKind::Storage) {
      const auto namespaceStorage = std::find_if(
          representations.symbols().begin(), representations.symbols().end(),
          [&](const CppMirSymbolRepresentation &row) {
            return row.kind == kind && row.owner == 0 && row.symbol == symbol &&
                   row.ordinal == ordinalValue;
          });
      return namespaceStorage == representations.symbols().end()
                 ? nullptr
                 : &*namespaceStorage;
    }
    return nullptr;
  }

  void requireSymbol(CppMirSymbolRepresentationKind kind, std::size_t owner,
                     SymbolId symbol, const SemanticType *type,
                     std::size_t ordinalValue, MirBlockId block = 0,
                     MirInstructionId instruction = 0) {
    bool ambiguousStorage = false;
    const CppMirSymbolRepresentation *row =
        findSymbol(kind, owner, symbol, ordinalValue, &ambiguousStorage);
    if (row == nullptr) {
      if (ambiguousStorage) {
        add(CppMirBodyEmissionIssueKind::MissingProgramInitializationMir, block,
            instruction,
            "MIR does not identify which concrete static-storage owner a "
            "same-symbol representation row denotes");
        return;
      }
      add(CppMirBodyEmissionIssueKind::MissingSymbolRepresentation, block,
          instruction,
          "copied map has no exact storage, field, or capture name row");
      return;
    }
    if ((type != nullptr && row->type != *type) || row->spelling.empty()) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "symbol row type or spelling disagrees with MIR");
    }
  }

  [[nodiscard]] const CppMirEnumRepresentation *findEnum(EnumId owner) const {
    const auto found = std::find_if(
        representations.enums().begin(), representations.enums().end(),
        [owner](const CppMirEnumRepresentation &row) {
          return row.owner == owner;
        });
    return found == representations.enums().end() ? nullptr : &*found;
  }

  const CppMirEnumRepresentation *
  requireEnum(EnumId owner, MirBlockId block = 0,
              MirInstructionId instruction = 0) {
    const CppMirEnumRepresentation *row = findEnum(owner);
    if (row == nullptr) {
      add(CppMirBodyEmissionIssueKind::MissingEnumRepresentation, block,
          instruction,
          "copied map has no declaration row for an exact MIR enum");
    } else {
      requireType(row->underlyingType, block, instruction);
    }
    return row;
  }

  void requireCapability(CppMirEmissionCapabilityKind kind,
                         MirBlockId block = 0,
                         MirInstructionId instruction = 0) {
    const auto found =
        std::find_if(representations.capabilities().begin(),
                     representations.capabilities().end(),
                     [kind](const CppMirEmissionCapabilityRepresentation &row) {
                       return row.kind == kind;
                     });
    if (found == representations.capabilities().end()) {
      add(CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation, block,
          instruction,
          "copied map lacks a required sealed representation helper (kind " +
              std::to_string(static_cast<unsigned>(kind)) + ")");
    }
  }

  [[nodiscard]] std::optional<HirClassInstanceId>
  classInstanceForType(const SemanticType &type) const {
    std::optional<HirClassInstanceId> resultId;
    for (const MirClassInstance &instance : program.classInstances()) {
      if (instance.type != type) {
        continue;
      }
      if (resultId) {
        return std::nullopt;
      }
      resultId = instance.id;
    }
    return resultId;
  }

  [[nodiscard]] std::optional<SemanticType> thisType() const {
    switch (result.body.kind) {
    case MirBodyKind::FieldInitializers:
    case MirBodyKind::StaticFieldInitializers:
      if (const MirClassInstance *instance =
              program.findClassInstance(result.body.owner)) {
        return instance->type;
      }
      return std::nullopt;
    case MirBodyKind::Function:
      if (const MirFunctionInstance *function =
              program.findFunctionInstance(result.body.owner);
          function != nullptr && function->owner) {
        if (const MirClassInstance *instance =
                program.findClassInstance(*function->owner)) {
          return instance->type;
        }
      }
      return std::nullopt;
    case MirBodyKind::Constructor:
      if (const MirConstructorInstance *constructor =
              program.findConstructorInstance(result.body.owner)) {
        if (const MirClassInstance *instance =
                program.findClassInstance(constructor->owner)) {
          return instance->type;
        }
      }
      return std::nullopt;
    case MirBodyKind::Destructor:
      if (const MirDestructorInstance *destructor =
              program.findDestructorInstance(result.body.owner)) {
        if (const MirClassInstance *instance =
                program.findClassInstance(destructor->owner)) {
          return instance->type;
        }
      }
      return std::nullopt;
    case MirBodyKind::Module:
    case MirBodyKind::Lambda:
    case MirBodyKind::HostedStartup:
      return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<HirClassInstanceId> concreteClassOwner() const {
    switch (result.body.kind) {
    case MirBodyKind::FieldInitializers:
    case MirBodyKind::StaticFieldInitializers:
      return result.body.owner;
    case MirBodyKind::Function:
      if (const MirFunctionInstance *function =
              program.findFunctionInstance(result.body.owner)) {
        return function->owner;
      }
      return std::nullopt;
    case MirBodyKind::Constructor:
      if (const MirConstructorInstance *constructor =
              program.findConstructorInstance(result.body.owner)) {
        return constructor->owner;
      }
      return std::nullopt;
    case MirBodyKind::Destructor:
      if (const MirDestructorInstance *destructor =
              program.findDestructorInstance(result.body.owner)) {
        return destructor->owner;
      }
      return std::nullopt;
    case MirBodyKind::Module:
    case MirBodyKind::Lambda:
    case MirBodyKind::HostedStartup:
      return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::size_t storageOwner(SymbolId symbol) const {
    if (const MirProgramInitializationStep *step =
            program.programInitializationPlan().findStepForSymbol(symbol)) {
      return step->ownerClass;
    }
    return concreteClassOwner().value_or(0);
  }

  [[nodiscard]] static bool sameRoot(const MirPlace &left,
                                     const MirPlace &right) {
    if (left.root != right.root) {
      return false;
    }
    switch (left.root) {
    case MirPlaceRootKind::Binding:
      return left.binding == right.binding;
    case MirPlaceRootKind::Symbol:
      return left.symbol == right.symbol && left.capture == right.capture;
    case MirPlaceRootKind::This:
      return true;
    case MirPlaceRootKind::Temporary:
      return left.temporary == right.temporary;
    case MirPlaceRootKind::Value:
      return left.value == right.value;
    case MirPlaceRootKind::Loan:
      return left.loan == right.loan;
    }
    return false;
  }

  [[nodiscard]] std::optional<SemanticType>
  rootType(const MirBody &body, const MirPlace &place) const {
    if (place.projections.empty()) {
      return place.type;
    }
    if (place.root == MirPlaceRootKind::This) {
      return thisType();
    }
    if (place.root == MirPlaceRootKind::Value) {
      const MirValue *value = body.findValue(place.value);
      return value == nullptr ? std::nullopt
                              : std::optional<SemanticType>{value->info.type};
    }
    if (place.root == MirPlaceRootKind::Loan) {
      const MirLoan *loan = body.findLoan(place.loan);
      const MirPlace *source =
          loan == nullptr ? nullptr : body.findPlace(loan->source);
      return source == nullptr ? std::nullopt
                               : std::optional<SemanticType>{source->type};
    }
    const auto root = std::find_if(
        body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
          return candidate.projections.empty() && sameRoot(candidate, place);
        });
    if (root != body.places.end()) {
      return root->type;
    }
    if (place.root == MirPlaceRootKind::Symbol) {
      const CppMirSymbolRepresentationKind kind =
          place.capture == 0 ? CppMirSymbolRepresentationKind::Storage
                             : CppMirSymbolRepresentationKind::Capture;
      const std::size_t owner =
          place.capture == 0 ? storageOwner(place.symbol) : result.body.owner;
      if (const CppMirSymbolRepresentation *row =
              findSymbol(kind, owner, place.symbol, place.capture)) {
        return row->type;
      }
    }
    return std::nullopt;
  }

  void scanOwnerMetadata(const MirBody &body) {
    const bool executableBody =
        result.body.kind == MirBodyKind::Module
            ? hasExecutableProgramInitialization(program)
            : !isCanonicalNoExecutionInitializer(body);
    if (executableBody) {
      requireBody(result.body);
    }
    requireType(body.returnType);

    switch (result.body.kind) {
    case MirBodyKind::Module:
      if (hasExecutableProgramInitialization(program)) {
        requireCapability(CppMirEmissionCapabilityKind::ProgramInitialization);
      }
      return;
    case MirBodyKind::FieldInitializers:
      // A field-initializer body whose every owning transfer armed rollback
      // routes failure edges and carries the complete construction
      // schedule; only a body with an unarmed transfer keeps the issue.
      if (!isCanonicalNoExecutionInitializer(body) &&
          !mirBodyRoutesFailureEdges(body)) {
        add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir, 0, 0,
            "declaration field initializers lack a complete constructor "
            "destination and partial-construction schedule");
      }
      if (const MirClassInstance *owner =
              program.findClassInstance(result.body.owner)) {
        requireType(owner->type);
      }
      return;
    case MirBodyKind::StaticFieldInitializers:
      if (!isCanonicalNoExecutionInitializer(body)) {
        requireCapability(CppMirEmissionCapabilityKind::ProgramInitialization);
        add(CppMirBodyEmissionIssueKind::MissingProgramInitializationMir, 0, 0,
            "static-field initialization is not yet merged into the verified "
            "program initialization walk");
      }
      if (const MirClassInstance *owner =
              program.findClassInstance(result.body.owner)) {
        requireType(owner->type);
      }
      return;
    case MirBodyKind::Function: {
      const MirFunctionInstance *function =
          program.findFunctionInstance(result.body.owner);
      if (function == nullptr) {
        return;
      }
      requireType(function->returnType);
      for (const SemanticType &type : function->parameterTypes) {
        requireType(type);
      }
      if (function->owner) {
        const MirClassInstance *owner =
            program.findClassInstance(*function->owner);
        if (owner != nullptr) {
          requireType(owner->type);
        }
      }
      if (function->entryKind != ProgramEntryKind::None) {
        requireCapability(CppMirEmissionCapabilityKind::HostedEntry);
      }
      if (function->linkage == LanguageLinkage::C ||
          function->definitionKind == MirDefinitionKind::RuntimeBinding) {
        requireCapability(CppMirEmissionCapabilityKind::NativeInterop);
      }
      if (function->virtualMethod || function->pureVirtual ||
          function->overrideMethod) {
        requireCapability(CppMirEmissionCapabilityKind::VirtualDispatch);
      }
      if (!function->callableParameters.empty()) {
        requireCapability(CppMirEmissionCapabilityKind::CallableDispatch);
      }
      for (const MirCallableParameter &parameter :
           function->callableParameters) {
        requireType(parameter.callableType);
        for (const MirCallableSignature &signature : parameter.signatures) {
          requireType(signature.returnType);
          for (const SemanticType &type : signature.parameterTypes) {
            requireType(type);
          }
          if (signature.functionTarget) {
            requireBody({.kind = MirBodyKind::Function,
                         .owner = *signature.functionTarget});
          }
          if (signature.lambdaTarget) {
            requireBody({.kind = MirBodyKind::Lambda,
                         .owner = *signature.lambdaTarget});
          }
        }
      }
      return;
    }
    case MirBodyKind::Constructor: {
      const MirConstructorInstance *constructor =
          program.findConstructorInstance(result.body.owner);
      if (constructor == nullptr) {
        return;
      }
      const MirClassInstance *owner =
          program.findClassInstance(constructor->owner);
      if (owner != nullptr) {
        requireType(owner->type);
      }
      for (const SemanticType &type : constructor->parameterTypes) {
        requireType(type);
      }
      for (const MirConstructorInitializer &initializer :
           constructor->initializers) {
        requireType(initializer.targetType);
        if (initializer.constructorTarget) {
          requireBody({.kind = MirBodyKind::Constructor,
                       .owner = *initializer.constructorTarget});
        }
      }
      constructorRollbackAuthority =
          constructorRollbackCovered(*constructor, owner);
      if (constructor->definitionKind == MirDefinitionKind::Source &&
          !constructorRollbackAuthority &&
          (constructor->mayRaiseDefinedFailure ||
           (owner != nullptr && (owner->requiresActiveCleanup ||
                                 classHasStateBearingBase(*owner))))) {
        add(CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir,
            0, 0,
            "constructor lacks rollback authority: " +
                std::string(constructorRollbackGap(*constructor, owner)));
      }
      return;
    }
    case MirBodyKind::Destructor: {
      const MirDestructorInstance *destructor =
          program.findDestructorInstance(result.body.owner);
      if (destructor == nullptr) {
        return;
      }
      const MirClassInstance *owner =
          program.findClassInstance(destructor->owner);
      if (owner != nullptr) {
        requireType(owner->type);
      }
      if (destructor->definitionKind == MirDefinitionKind::Source &&
          destructor->mayRaiseDefinedFailure) {
        add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, 0, 0,
            "failure-capable cleanup requires the emergency double-failure "
            "control path");
      }
      if (owner != nullptr &&
          (classHasStateBearingBase(*owner) ||
           std::any_of(owner->fields.begin(), owner->fields.end(),
                       [](const MirClassFieldLifecycle &field) {
                         return field.requiresActiveCleanup;
                       }))) {
        add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir, 0, 0,
            "general field/base destruction composition is not yet a complete "
            "MIR body schedule");
      }
      return;
    }
    case MirBodyKind::Lambda: {
      const MirLambdaInstance *lambda = program.findLambda(result.body.owner);
      if (lambda == nullptr) {
        return;
      }
      requireCapability(CppMirEmissionCapabilityKind::Closure);
      requireType(lambda->returnType);
      for (const SemanticType &type : lambda->parameterTypes) {
        requireType(type);
      }
      for (std::size_t index = 0; index < lambda->captureTypes.size();
           ++index) {
        requireType(lambda->captureTypes[index]);
        if (index < lambda->captureSymbols.size() &&
            lambda->captureSymbols[index] != 0) {
          requireSymbol(CppMirSymbolRepresentationKind::Capture, lambda->id,
                        lambda->captureSymbols[index],
                        &lambda->captureTypes[index], index + 1);
        }
      }
      return;
    }
    case MirBodyKind::HostedStartup: {
      requireCapability(CppMirEmissionCapabilityKind::HostedEntry);
      const bool ownedArgumentsSchedule =
          cppMirHostedStartupOwnedArgumentsSchedule(program);
      if (!cppMirHostedStartupNoArgumentsSchedule(program) &&
          !ownedArgumentsSchedule) {
        add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, 0, 0,
            "compiler-generated hosted startup lacks the Stage-E terminal "
            "failure-containment path");
      }
      // The verified owned-arguments schedule carries its own
      // drop/end failure-cleanup envelope, so its owned marshaling
      // obligations are covered by the plan itself.
      if (!ownedArgumentsSchedule &&
          std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                      [](const MirDropObligation &obligation) {
                        return obligation.dropType.requiresActiveCleanup;
                      })) {
        add(CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir,
            0, 0,
            "owned hosted arguments lack the Stage-E partial-construction "
            "rollback and transfer envelope");
      }
      return;
    }
    }
  }

  void scanBody(const MirBody &body) {
    for (const MirPlace &place : body.places) {
      scanPlace(body, place);
    }
    bool ownsRaisingFailureCleanup = false;
    for (const MirDropObligation &obligation : body.dropObligations) {
      requireType(obligation.dropType.type);
      if (obligation.dropType.destructor) {
        requireBody({.kind = MirBodyKind::Destructor,
                     .owner = *obligation.dropType.destructor});
        // A cleanup destructor that may itself raise puts the body inside
        // the unrepresented double-failure envelope.
        const MirDestructorInstance *destructor =
            program.findDestructorInstance(*obligation.dropType.destructor);
        if (obligation.dropType.requiresActiveCleanup &&
            (destructor == nullptr || destructor->mayRaiseDefinedFailure)) {
          ownsRaisingFailureCleanup = true;
        }
      }
      if (obligation.dropType.requiresActiveCleanup) {
        requireCapability(CppMirEmissionCapabilityKind::LifetimeStorage);
      }
    }
    // Failure-capable bodies whose cleanup cannot itself raise are
    // admitted: the MIR verifier owns drop-schedule correctness, the slot
    // vocabulary spells the drains, and unspellable shapes decline at the
    // probe. A cleanup destructor that may raise keeps the body behind the
    // double-failure envelope until that representation lands.
    if (!body.failureRecords.empty() && ownsRaisingFailureCleanup) {
      add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, 0, 0,
          "a general failure-capable body owns cleanup whose destructor may "
          "itself raise; the double-failure envelope is unrepresented");
    }
    for (const MirValue &value : body.values) {
      requireType(value.info.type);
    }

    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        scanInstruction(body, block, instruction);
      }
      scanTerminator(block);
    }
  }

  void scanPlace(const MirBody &body, const MirPlace &place) {
    requireType(place.type);
    const CppMirEmissionEncoding root = classifyCppMirPlaceRootKind(place.root);
    if (root == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidPlaceRootKind, 0, 0,
          "place root is outside the exhaustive emitter vocabulary");
    }
    std::optional<SemanticType> currentType = rootType(body, place);
    if (place.root == MirPlaceRootKind::Binding &&
        result.body.kind == MirBodyKind::Module) {
      const MirProgramInitializationStep *step =
          program.programInitializationPlan().findStepForSymbol(place.symbol);
      if (step == nullptr || step->binding != place.binding ||
          step->storagePlace != place.id) {
        add(CppMirBodyEmissionIssueKind::InvalidMirProgram, 0, 0,
            "Module binding place has no exact program-initialization row");
      } else {
        requireSymbol(CppMirSymbolRepresentationKind::Storage, step->ownerClass,
                      place.symbol, currentType ? &*currentType : nullptr, 0);
      }
    } else if (place.root == MirPlaceRootKind::Symbol) {
      if (place.capture != 0 && result.body.kind == MirBodyKind::Lambda) {
        requireSymbol(CppMirSymbolRepresentationKind::Capture,
                      result.body.owner, place.symbol,
                      currentType ? &*currentType : nullptr, place.capture);
      } else {
        requireSymbol(CppMirSymbolRepresentationKind::Storage,
                      storageOwner(place.symbol), place.symbol,
                      currentType ? &*currentType : nullptr, 0);
      }
    } else if (place.root == MirPlaceRootKind::This) {
      if (result.body.kind == MirBodyKind::Function) {
        const MirFunctionInstance *function =
            program.findFunctionInstance(result.body.owner);
        if (function != nullptr && function->owner) {
          const MirClassInstance *owner =
              program.findClassInstance(*function->owner);
          if (owner != nullptr) {
            requireType(owner->type);
          }
        }
      }
    } else if (place.root == MirPlaceRootKind::Loan) {
      requireCapability(CppMirEmissionCapabilityKind::Borrow);
    }

    if (!currentType) {
      add(CppMirBodyEmissionIssueKind::MissingTypeRepresentation, 0, 0,
          "projected place needs an explicit copied root-type row; MIR does "
          "not identify a unique concrete root type");
    }

    for (const MirPlaceProjection &projection : place.projections) {
      const CppMirEmissionEncoding encoding =
          classifyCppMirProjectionKind(projection.kind);
      if (encoding == CppMirEmissionEncoding::Invalid) {
        add(CppMirBodyEmissionIssueKind::InvalidProjectionKind, 0, 0,
            "place projection is outside the exhaustive emitter vocabulary");
        continue;
      }
      switch (projection.kind) {
      case MirProjectionKind::Field: {
        const std::optional<HirClassInstanceId> owner =
            currentType ? classInstanceForType(*currentType) : std::nullopt;
        const MirClassInstance *instance =
            owner ? program.findClassInstance(*owner) : nullptr;
        const auto field =
            instance == nullptr
                ? std::vector<MirClassFieldInfo>::const_iterator{}
                : std::find_if(instance->declaredFields.begin(),
                               instance->declaredFields.end(),
                               [&](const MirClassFieldInfo &candidate) {
                                 return candidate.symbol == projection.field;
                               });
        if (instance == nullptr || field == instance->declaredFields.end()) {
          add(CppMirBodyEmissionIssueKind::MissingSymbolRepresentation, 0, 0,
              "field projection cannot be keyed to one exact concrete class "
              "instance from its evolving place type");
          currentType.reset();
        } else {
          requireSymbol(CppMirSymbolRepresentationKind::Field, instance->id,
                        projection.field, &field->type, 0);
          currentType = field->type;
        }
        break;
      }
      case MirProjectionKind::Index:
        // A dynamic index is representable since the checked fixed-array
        // access family shipped: a site-carrying access spells the checked
        // helper and its record, and a proven-safe access spells a plain
        // subscription. The Bounds capability row names that family; the
        // text vocabulary decides each instruction.
        requireCapability(CppMirEmissionCapabilityKind::Bounds);
        if (currentType && currentType->kind == SemanticType::Array &&
            currentType->arguments.size() == 1) {
          // Copy before assignment: the element lives inside the vector the
          // assignment replaces, so a direct self-assign reads freed storage.
          SemanticType element = currentType->arguments.front();
          currentType = std::move(element);
        } else {
          currentType.reset();
        }
        break;
      case MirProjectionKind::Dereference:
        requireCapability(CppMirEmissionCapabilityKind::Borrow);
        if (currentType && currentType->arguments.size() == 1 &&
            (currentType->kind == SemanticType::Reference ||
             currentType->kind == SemanticType::UniqueOwner ||
             currentType->kind == SemanticType::SharedPointer)) {
          SemanticType pointee = currentType->arguments.front();
          currentType = std::move(pointee);
        } else {
          currentType.reset();
        }
        break;
      case MirProjectionKind::RawIndex:
      case MirProjectionKind::RawDereference:
        requireCapability(CppMirEmissionCapabilityKind::RawMemory);
        if (currentType && currentType->kind == SemanticType::RawPointer &&
            currentType->arguments.size() == 1) {
          SemanticType pointee = currentType->arguments.front();
          currentType = std::move(pointee);
        } else {
          currentType.reset();
        }
        break;
      }
    }
    if (currentType && *currentType != place.type) {
      add(CppMirBodyEmissionIssueKind::InvalidMirProgram, 0, 0,
          "place projection result type disagrees with its concrete root "
          "and projection chain");
    }
  }

  void scanOperand(const MirOperand &operand, MirBlockId block,
                   MirInstructionId instruction) {
    const CppMirEmissionEncoding encoding =
        classifyCppMirOperandKind(operand.kind);
    if (encoding == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidOperandKind, block, instruction,
          "operand kind is outside the exhaustive emitter vocabulary");
      return;
    }
    requireType(operand.type, block, instruction);
    switch (operand.kind) {
    case MirOperandKind::Address:
      requireCapability(CppMirEmissionCapabilityKind::RawMemory, block,
                        instruction);
      break;
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
    case MirOperandKind::Loan:
      requireCapability(CppMirEmissionCapabilityKind::Borrow, block,
                        instruction);
      break;
    case MirOperandKind::Value:
    case MirOperandKind::Constant:
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
      break;
    }
  }

  void scanOperation(const MirBody &body, const MirBlock &block,
                     const MirInstruction &instruction) {
    const CppMirEmissionEncoding encoding =
        classifyCppMirOperation(instruction.operation);
    if (encoding == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidOperation, block.id,
          instruction.id,
          "MIR operation is outside the exhaustive emitter vocabulary");
      return;
    }
    if (encoding == CppMirEmissionEncoding::MissingMirAuthority &&
        !isHostedStartupArgumentIndexAdvance(body, instruction)) {
      if (instruction.operation == MirOperation::PackExpansion) {
        // The bounded forward-once shape needs no per-element MIR: the
        // pack's flattened parameters spell at the one consuming
        // allocation call.
        if (instruction.result &&
            packExpansionForwardedOnce(body, *instruction.result) ==
                &instruction) {
          return;
        }
        add(CppMirBodyEmissionIssueKind::MissingPackExpansionMir, block.id,
            instruction.id,
            "PackExpansion has no concrete ordered element operands or "
            "targets in MIR");
      } else {
        add(CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir, block.id,
            instruction.id,
            "compound operation lacks the complete verified target/operand/"
            "commit schedule required for generic emission");
      }
    }

    switch (instruction.operation) {
    case MirOperation::EnumConstant:
      if (instruction.enumOwner) {
        requireEnum(*instruction.enumOwner, block.id, instruction.id);
      }
      break;
    case MirOperation::Aggregate:
      requireCapability(CppMirEmissionCapabilityKind::Aggregate, block.id,
                        instruction.id);
      if (instruction.info.traits.drop != DropKind::Trivial) {
        add(CppMirBodyEmissionIssueKind::MissingAggregateRollbackMir, block.id,
            instruction.id,
            "cleanup-owning aggregate construction lacks per-element partial "
            "initialization and rollback state");
      }
      break;
    case MirOperation::Index:
      requireCapability(CppMirEmissionCapabilityKind::Bounds, block.id,
                        instruction.id);
      break;
    case MirOperation::AddressOf:
    case MirOperation::PointerAdd:
    case MirOperation::PointerSubtract:
    case MirOperation::PointerDifference:
      requireCapability(CppMirEmissionCapabilityKind::RawMemory, block.id,
                        instruction.id);
      break;
    case MirOperation::ExpectedHasValue:
    case MirOperation::Unexpected:
      requireCapability(CppMirEmissionCapabilityKind::Expected, block.id,
                        instruction.id);
      break;
    case MirOperation::Closure:
      requireCapability(CppMirEmissionCapabilityKind::Closure, block.id,
                        instruction.id);
      if (instruction.lambdaTarget) {
        requireBody(
            {.kind = MirBodyKind::Lambda, .owner = *instruction.lambdaTarget},
            block.id, instruction.id);
      }
      break;
    case MirOperation::PackFold:
      requireCapability(CppMirEmissionCapabilityKind::PackFold, block.id,
                        instruction.id);
      for (const MirPackFoldElement &element : instruction.packFoldElements) {
        requireType(element.elementType, block.id, instruction.id);
        for (const SemanticType &type : element.parameterTypes) {
          requireType(type, block.id, instruction.id);
        }
        requireBody(
            {.kind = MirBodyKind::Function, .owner = element.functionTarget},
            block.id, instruction.id);
      }
      break;
    case MirOperation::PayloadConstruct:
    case MirOperation::PayloadExtract: {
      requireCapability(CppMirEmissionCapabilityKind::Payload, block.id,
                        instruction.id);
      const CppMirEnumRepresentation *enumeration =
          instruction.enumOwner
              ? requireEnum(*instruction.enumOwner, block.id, instruction.id)
              : nullptr;
      if (enumeration != nullptr && instruction.enumVariant) {
        const auto variant =
            std::find_if(enumeration->payloadVariants.begin(),
                         enumeration->payloadVariants.end(),
                         [&](const CppMirPayloadVariantRepresentation &row) {
                           return row.index == *instruction.enumVariant;
                         });
        if (variant == enumeration->payloadVariants.end()) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block.id,
              instruction.id,
              "payload operation names a variant absent from the copied map");
        } else if (instruction.operation == MirOperation::PayloadConstruct) {
          std::vector<SemanticType> operands;
          operands.reserve(instruction.operands.size());
          for (const MirOperand &operand : instruction.operands) {
            operands.push_back(operand.type);
          }
          if (variant->fieldTypes != operands) {
            add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block.id,
                instruction.id,
                "payload constructor fields disagree with the copied enum "
                "variant");
          }
        } else if (!instruction.payloadIndex ||
                   *instruction.payloadIndex >= variant->fieldTypes.size() ||
                   variant->fieldTypes[*instruction.payloadIndex] !=
                       instruction.info.type) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block.id,
              instruction.id,
              "payload extraction index or type disagrees with the copied "
              "enum variant");
        }
      }
      break;
    }
    case MirOperation::PackExpansion:
    case MirOperation::Comma:
    case MirOperation::AddAssign:
    case MirOperation::SubtractAssign:
    case MirOperation::MultiplyAssign:
    case MirOperation::DivideAssign:
    case MirOperation::RemainderAssign:
    case MirOperation::BitwiseAndAssign:
    case MirOperation::BitwiseOrAssign:
    case MirOperation::BitwiseXorAssign:
    case MirOperation::ShiftLeftAssign:
    case MirOperation::ShiftRightAssign:
    case MirOperation::PreIncrement:
    case MirOperation::PreDecrement:
    case MirOperation::PostIncrement:
    case MirOperation::PostDecrement:
      break;
    case MirOperation::None:
    case MirOperation::Literal:
    case MirOperation::Identity:
    case MirOperation::Convert:
    case MirOperation::Add:
    case MirOperation::Subtract:
    case MirOperation::Multiply:
    case MirOperation::Divide:
    case MirOperation::Remainder:
    case MirOperation::BitwiseAnd:
    case MirOperation::BitwiseOr:
    case MirOperation::BitwiseXor:
    case MirOperation::ShiftLeft:
    case MirOperation::ShiftRight:
    case MirOperation::Equal:
    case MirOperation::NotEqual:
    case MirOperation::Less:
    case MirOperation::LessEqual:
    case MirOperation::Greater:
    case MirOperation::GreaterEqual:
    case MirOperation::Positive:
    case MirOperation::Negate:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
    case MirOperation::Assign:
      break;
    case MirOperation::Count:
      add(CppMirBodyEmissionIssueKind::InvalidOperation, block.id,
          instruction.id, "MirOperation::Count is not executable");
      break;
    }

    (void)body;
  }

  void scanInstruction(const MirBody &body, const MirBlock &block,
                       const MirInstruction &instruction) {
    const CppMirEmissionEncoding kind =
        classifyCppMirInstructionKind(instruction.kind);
    if (kind == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidInstructionKind, block.id,
          instruction.id,
          "instruction kind is outside the exhaustive emitter vocabulary");
      return;
    }
    if (kind == CppMirEmissionEncoding::MissingMirAuthority &&
        !isHostedStartupArgumentIndexAdvance(body, instruction)) {
      add(CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir, block.id,
          instruction.id,
          "Modify lacks the verified read/check/convert/commit schedule");
    }

    switch (instruction.kind) {
    case MirInstructionKind::Drop:
    case MirInstructionKind::EndBorrow:
    case MirInstructionKind::Lifecycle:
      if (instruction.info.type != SemanticType::Unknown) {
        requireType(instruction.info.type, block.id, instruction.id);
      }
      break;
    case MirInstructionKind::Compute:
    case MirInstructionKind::Load:
    case MirInstructionKind::Initialize:
    case MirInstructionKind::Assign:
    case MirInstructionKind::Modify:
    case MirInstructionKind::Move:
    case MirInstructionKind::Borrow:
    case MirInstructionKind::CallInput:
    case MirInstructionKind::Call:
    case MirInstructionKind::Construct:
      requireType(instruction.info.type, block.id, instruction.id);
      break;
    case MirInstructionKind::CallBody:
      requireType(instruction.info.type, block.id, instruction.id);
      break;
    case MirInstructionKind::Count:
      break;
    }
    for (const SemanticType &type : instruction.parameterTypes) {
      requireType(type, block.id, instruction.id);
    }
    for (const SemanticType &type : instruction.closureCaptureTypes) {
      requireType(type, block.id, instruction.id);
    }
    if (instruction.receiver) {
      scanOperand(*instruction.receiver, block.id, instruction.id);
    }
    for (const MirOperand &operand : instruction.operands) {
      scanOperand(operand, block.id, instruction.id);
    }
    scanOperation(body, block, instruction);

    if (instruction.functionTarget) {
      requireBody(
          {.kind = MirBodyKind::Function, .owner = *instruction.functionTarget},
          block.id, instruction.id);
      const MirFunctionInstance *target =
          program.findFunctionInstance(*instruction.functionTarget);
      if (target != nullptr && target->linkage == LanguageLinkage::C) {
        requireCapability(CppMirEmissionCapabilityKind::NativeInterop, block.id,
                          instruction.id);
      }
    }
    if (instruction.constructorTarget) {
      requireBody({.kind = MirBodyKind::Constructor,
                   .owner = *instruction.constructorTarget},
                  block.id, instruction.id);
    }
    if (instruction.lambdaTarget) {
      requireBody(
          {.kind = MirBodyKind::Lambda, .owner = *instruction.lambdaTarget},
          block.id, instruction.id);
    }
    if (instruction.bodyTarget) {
      requireBody(*instruction.bodyTarget, block.id, instruction.id);
    }
    if (instruction.dispatch == CallDispatch::Virtual) {
      requireCapability(CppMirEmissionCapabilityKind::VirtualDispatch, block.id,
                        instruction.id);
    }
    if (instruction.callableInvocation || instruction.callableBoundary ||
        !instruction.callableArguments.empty()) {
      requireCapability(CppMirEmissionCapabilityKind::CallableDispatch,
                        block.id, instruction.id);
    }
    if (instruction.intrinsic != IntrinsicKind::None) {
      if (ordinal(instruction.intrinsic) >= ordinal(IntrinsicKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, block.id,
            instruction.id, "call has an invalid intrinsic identity");
      }
      requireCapability(CppMirEmissionCapabilityKind::Intrinsic, block.id,
                        instruction.id);
    }
    if (instruction.synchronization.kind !=
        SynchronizationOperationKind::None) {
      if (ordinal(instruction.synchronization.kind) >=
          ordinal(SynchronizationOperationKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, block.id,
            instruction.id, "call has an invalid synchronization identity");
      }
      requireCapability(CppMirEmissionCapabilityKind::Synchronization, block.id,
                        instruction.id);
    }
    if (instruction.unsafeOperation != UnsafeOperationKind::None ||
        instruction.rawMemoryAccess) {
      requireCapability(CppMirEmissionCapabilityKind::RawMemory, block.id,
                        instruction.id);
    }
    if (!instruction.definedFailure.empty()) {
      requireCapability(CppMirEmissionCapabilityKind::DefinedFailure, block.id,
                        instruction.id);
      if (result.body.kind == MirBodyKind::HostedStartup) {
        if (!cppMirHostedStartupNoArgumentsSchedule(program) &&
            !cppMirHostedStartupOwnedArgumentsSchedule(program)) {
          add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, block.id,
              instruction.id,
              instruction.definedFailure.propagation ==
                      FailurePropagationKind::BodyCall
                  ? "compiler-generated body-call propagation lacks the "
                    "Stage-E hosted cleanup and terminal containment path"
                  : "compiler-generated hosted failure propagation lacks the "
                    "Stage-E cleanup and terminal containment path");
        }
      } else if (!instructionHasInvoke(block, instruction)) {
        // A proven-safe element access records its site without a failure
        // edge: flow analysis discharged the bounds check, so no Invoke,
        // record, or cleanup successor exists to demand.
        const MirPlace *elementPlace = nullptr;
        if (instruction.kind == MirInstructionKind::Load &&
            instruction.operands.size() == 1) {
          elementPlace = body.findPlace(instruction.operands.front().place);
        } else if (instruction.kind == MirInstructionKind::Assign &&
                   instruction.destination) {
          elementPlace = body.findPlace(*instruction.destination);
        }
        // A prefix-storage read whose site carries no failure edge is the
        // same discharged shape: the enclosing trusted container proved
        // the index against its logical size (the preceding
        // index_bounds_check Invoke), so the read records its site while
        // the sealed runtime guard remains defense in depth.
        const bool dischargedStorageRead =
            instruction.kind == MirInstructionKind::Call &&
            (instruction.intrinsic == IntrinsicKind::PrefixStorageRead ||
             instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut) &&
            !instruction.definedFailure.localOrigins.empty() &&
            instruction.definedFailure.propagation ==
                FailurePropagationKind::None;
        // A direct call whose callee may raise carries the propagation
        // dimension with no local origin and no failure edge: the callee
        // writes the caller's forwarded record and the caller returns
        // false transparently. MIR itself asserts the caller owns no
        // cleanup here, so there is no Invoke/record/cleanup successor
        // to demand.
        const bool transparentCallPropagation =
            (instruction.kind == MirInstructionKind::Call &&
             instruction.intrinsic == IntrinsicKind::None &&
             instruction.definedFailure.propagation ==
                 FailurePropagationKind::DirectCall &&
             instruction.definedFailure.localOrigins.empty()) ||
            // A propagating construction has no failure edge because the
            // constructor's failure terminates at its own site on every
            // shipped path — the untransformed constructor and the
            // compatibility one behave identically — so the caller owns
            // nothing here until the constructor failure ABI exists.
            (instruction.kind == MirInstructionKind::Construct &&
             instruction.definedFailure.propagation ==
                 FailurePropagationKind::Constructor &&
             instruction.definedFailure.localOrigins.empty());
        // A unique-owner allocation contains its failure terminally
        // inside the backend helper — the compatibility call site carries
        // no handling either — so no Invoke, record, or cleanup successor
        // exists to demand.
        const bool terminalAllocation =
            instruction.kind == MirInstructionKind::Call &&
            instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner;
        if (!dischargedStorageRead && !transparentCallPropagation &&
            !terminalAllocation &&
            (elementPlace == nullptr ||
             !arrayElementAccess(body, *elementPlace))) {
          add(CppMirBodyEmissionIssueKind::MissingCheckedFailureControlFlow,
              block.id, instruction.id,
              "checked operation has no exact Invoke/record/cleanup "
              "successor");
        }
      }
      if (result.body.kind == MirBodyKind::Constructor &&
          !constructorRollbackAuthority) {
        add(CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir,
            block.id, instruction.id,
            "failure-capable construction has no general subobject rollback");
      }
      if (result.body.kind == MirBodyKind::Destructor) {
        add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, block.id,
            instruction.id,
            "failure-capable cleanup has no double-failure envelope path");
      }
    }

    if ((instruction.kind == MirInstructionKind::Call ||
         instruction.kind == MirInstructionKind::Construct) &&
        instruction.intrinsic == IntrinsicKind::None &&
        // A callable-value invocation has no CallInput schedule: its
        // receiver is the fused closure literal and its arguments pass as
        // plain staged values, exactly like the compatibility call.
        !callableValueInvocation(instruction) &&
        !hasCompleteCallInputSchedule(body, instruction)) {
      add(CppMirBodyEmissionIssueKind::MissingCallInputScheduleMir, block.id,
          instruction.id,
          "call or construction operands do not all come from exact ordered "
          "CallInput stages");
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.constructorKind != ConstructorKind::Ordinary) {
      add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir, block.id,
          instruction.id,
          "generated copy/move construction lacks the complete generic "
          "destination and cleanup schedule");
    }
    if (instruction.kind == MirInstructionKind::Borrow ||
        instruction.kind == MirInstructionKind::EndBorrow) {
      requireCapability(CppMirEmissionCapabilityKind::Borrow, block.id,
                        instruction.id);
    }
    if (instruction.kind == MirInstructionKind::Drop ||
        !instruction.lifecycle.empty()) {
      requireCapability(CppMirEmissionCapabilityKind::LifetimeStorage, block.id,
                        instruction.id);
    }
  }

  void scanTerminator(const MirBlock &block) {
    const MirTerminator &terminator = block.terminator;
    const CppMirEmissionEncoding encoding =
        classifyCppMirTerminatorKind(terminator.kind);
    if (encoding == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidTerminatorKind, block.id, 0,
          "terminator kind is not executable");
      return;
    }
    if (terminator.value) {
      scanOperand(*terminator.value, block.id, 0);
    }
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      if (!target.value) {
        continue;
      }
      requireType(target.value->type, block.id, 0);
      if (target.value->enumOwner != 0) {
        requireEnum(target.value->enumOwner, block.id, 0);
      }
    }
    if (terminator.kind == MirTerminatorKind::Invoke ||
        terminator.kind == MirTerminatorKind::PropagateFailure ||
        terminator.kind == MirTerminatorKind::ContainFailure ||
        terminator.kind == MirTerminatorKind::TerminateCleanupFailure) {
      requireCapability(CppMirEmissionCapabilityKind::DefinedFailure, block.id,
                        0);
    }
    if (terminator.kind == MirTerminatorKind::Exit &&
        !isInitializerBody(result.body.kind)) {
      add(CppMirBodyEmissionIssueKind::InvalidTerminatorKind, block.id, 0,
          "Exit is reserved for module/field initializer bodies");
    }
  }

  const MirProgram &program;
  const CppMirBodyEmissionMap &representations;
  CppMirBodyEmissionAnalysis result;
  // Set by the Constructor owner-metadata scan: the body's verified MIR
  // carries complete rollback authority, so the categorical rollback issues
  // do not apply.
  bool constructorRollbackAuthority = false;
};

} // namespace

CppMirBodyEmissionMap::CppMirBodyEmissionMap(CppMirBodyEmissionMapRows rows)
    : types_(std::move(rows.types)), bodies_(std::move(rows.bodies)),
      symbols_(std::move(rows.symbols)), enums_(std::move(rows.enums)),
      capabilities_(std::move(rows.capabilities)) {}

CppMirEmissionEncoding classifyCppMirInstructionKind(MirInstructionKind kind) {
  switch (kind) {
  case MirInstructionKind::Compute:
  case MirInstructionKind::Load:
  case MirInstructionKind::Initialize:
  case MirInstructionKind::Assign:
  case MirInstructionKind::Move:
  case MirInstructionKind::Lifecycle:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirInstructionKind::Borrow:
  case MirInstructionKind::CallInput:
  case MirInstructionKind::Call:
  case MirInstructionKind::Construct:
  case MirInstructionKind::Drop:
  case MirInstructionKind::EndBorrow:
  case MirInstructionKind::CallBody:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  case MirInstructionKind::Modify:
    return CppMirEmissionEncoding::MissingMirAuthority;
  case MirInstructionKind::Count:
    return CppMirEmissionEncoding::Invalid;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirOperation(MirOperation operation) {
  switch (operation) {
  case MirOperation::None:
  case MirOperation::Literal:
  case MirOperation::Identity:
  case MirOperation::Convert:
  case MirOperation::Add:
  case MirOperation::Subtract:
  case MirOperation::Multiply:
  case MirOperation::Divide:
  case MirOperation::Remainder:
  case MirOperation::BitwiseAnd:
  case MirOperation::BitwiseOr:
  case MirOperation::BitwiseXor:
  case MirOperation::ShiftLeft:
  case MirOperation::ShiftRight:
  case MirOperation::Equal:
  case MirOperation::NotEqual:
  case MirOperation::Less:
  case MirOperation::LessEqual:
  case MirOperation::Greater:
  case MirOperation::GreaterEqual:
  case MirOperation::Positive:
  case MirOperation::Negate:
  case MirOperation::LogicalNot:
  case MirOperation::BitwiseNot:
  case MirOperation::Assign:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirOperation::EnumConstant:
  case MirOperation::Aggregate:
  case MirOperation::Index:
  case MirOperation::ExpectedHasValue:
  case MirOperation::Closure:
  case MirOperation::PackFold:
  case MirOperation::PayloadConstruct:
  case MirOperation::PayloadExtract:
  case MirOperation::Unexpected:
  case MirOperation::AddressOf:
  case MirOperation::PointerAdd:
  case MirOperation::PointerSubtract:
  case MirOperation::PointerDifference:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  case MirOperation::PackExpansion:
  case MirOperation::Comma:
  case MirOperation::AddAssign:
  case MirOperation::SubtractAssign:
  case MirOperation::MultiplyAssign:
  case MirOperation::DivideAssign:
  case MirOperation::RemainderAssign:
  case MirOperation::BitwiseAndAssign:
  case MirOperation::BitwiseOrAssign:
  case MirOperation::BitwiseXorAssign:
  case MirOperation::ShiftLeftAssign:
  case MirOperation::ShiftRightAssign:
  case MirOperation::PreIncrement:
  case MirOperation::PreDecrement:
  case MirOperation::PostIncrement:
  case MirOperation::PostDecrement:
    return CppMirEmissionEncoding::MissingMirAuthority;
  case MirOperation::Count:
    return CppMirEmissionEncoding::Invalid;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirOperandKind(MirOperandKind kind) {
  switch (kind) {
  case MirOperandKind::Value:
  case MirOperandKind::Constant:
  case MirOperandKind::Copy:
  case MirOperandKind::Move:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirOperandKind::Address:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
  case MirOperandKind::Loan:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirPlaceRootKind(MirPlaceRootKind kind) {
  switch (kind) {
  case MirPlaceRootKind::Binding:
  case MirPlaceRootKind::Temporary:
  case MirPlaceRootKind::Value:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirPlaceRootKind::Symbol:
  case MirPlaceRootKind::This:
  case MirPlaceRootKind::Loan:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirProjectionKind(MirProjectionKind kind) {
  switch (kind) {
  case MirProjectionKind::Field:
  case MirProjectionKind::Index:
  case MirProjectionKind::Dereference:
  case MirProjectionKind::RawIndex:
  case MirProjectionKind::RawDereference:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirTerminatorKind(MirTerminatorKind kind) {
  switch (kind) {
  case MirTerminatorKind::Goto:
  case MirTerminatorKind::Branch:
  case MirTerminatorKind::Switch:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirTerminatorKind::Invoke:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  case MirTerminatorKind::None:
    return CppMirEmissionEncoding::Invalid;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirBodyKind(MirBodyKind kind) {
  switch (kind) {
  case MirBodyKind::Module:
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
  case MirBodyKind::Function:
  case MirBodyKind::Constructor:
  case MirBodyKind::Destructor:
  case MirBodyKind::Lambda:
  case MirBodyKind::HostedStartup:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

namespace {

// General per-instance text step (ADR 016 phase 5). Ported verbatim from the
// transitional emitter's scalar-cfg/scalar-direct-call body emission so
// production text is byte-identical across the delegation; every naming or
// type consultation is replaced by a copied representation row. Constructs
// outside this vocabulary after a Ready analysis are emission drift and
// throw, exactly as the transitional emitter throws.
// The vocabulary spells only the wrapping/saturating kinds: their helpers
// take and return the operand scalar directly, while a checked variant
// produces an `Expected` payload the scalar vocabulary cannot represent.
// The prefix-storage intrinsic family the failure form spells through the
// shipped mir_prefix_*_v1 checked helpers.
[[nodiscard]] bool prefixStorageIntrinsic(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::AllocatePrefixStorage:
  case IntrinsicKind::PrefixStorageAppend:
  case IntrinsicKind::PrefixStoragePop:
  case IntrinsicKind::PrefixStorageInsert:
  case IntrinsicKind::PrefixStorageErase:
  case IntrinsicKind::PrefixStorageRelocate:
    return true;
  default:
    return false;
  }
}

// A prefix-storage read whose bounds proof the enclosing trusted
// container discharged (the preceding logical-size check): the site is
// recorded with no failure edge, the sealed runtime guard stays as
// defense in depth, and the element address feeds the loan directly.
[[nodiscard]] bool
dischargedStorageReadCall(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call &&
         (instruction.intrinsic == IntrinsicKind::PrefixStorageRead ||
          instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut) &&
         !instruction.definedFailure.localOrigins.empty() &&
         instruction.definedFailure.propagation == FailurePropagationKind::None;
}

// The identity-bound public logical-size check (P-STORAGE-01 slice 1)
// contains its failure terminally inside the compatibility helper, so its
// invoke edge never branches on either form.
[[nodiscard]] bool storageBoundsCheckCall(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call &&
         instruction.intrinsic == IntrinsicKind::StorageBoundsCheck &&
         !instruction.functionTarget;
}

// A site-carrying numeric conversion arriving as an intrinsic call is a
// checked detector exactly like the Convert compute: the status helper
// writes the converted value and the paired invoke branches on it.
[[nodiscard]] bool
checkedConversionIntrinsicCall(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call &&
         (instruction.intrinsic == IntrinsicKind::NumericAliasConversion ||
          instruction.intrinsic ==
              IntrinsicKind::NumericTypeParameterConversion) &&
         !instruction.functionTarget &&
         instruction.localFailureSites.size() == 1;
}

// A class-valued failure Return publishes its constructor call inline
// through the `T *` out-parameter: the construct's value result has
// exactly the Return as its consumer and never declares a local.
[[nodiscard]] const MirInstruction *
returnConstructDefinition(const MirBody &body, MirValueId value) {
  const MirValue *record = body.findValue(value);
  const MirInstruction *definition =
      record == nullptr ? nullptr : findInstruction(body, record->definition);
  if (definition == nullptr ||
      definition->kind != MirInstructionKind::Construct ||
      !definition->result || definition->receiver ||
      definition->constructorKind != ConstructorKind::Ordinary ||
      body.usesOf(value).size() != 1 ||
      body.usesOf(value).front().kind != MirValueUseKind::Terminator) {
    return nullptr;
  }
  return definition;
}

// A class-valued plain Return may publish its defining call directly:
// the call sits last in the Return's own block and the value's only use
// is the Return, so the call spells `return <call>` and the class
// result never materializes as a local.
[[nodiscard]] const MirInstruction *returnCallDefinition(const MirBody &body,
                                                         MirValueId value) {
  const MirValue *record = body.findValue(value);
  const MirInstruction *definition =
      record == nullptr ? nullptr : findInstruction(body, record->definition);
  if (record == nullptr || record->info.type.kind != SemanticType::Class ||
      definition == nullptr || definition->kind != MirInstructionKind::Call ||
      !definition->result || body.usesOf(value).size() != 1 ||
      body.usesOf(value).front().kind != MirValueUseKind::Terminator) {
    return nullptr;
  }
  for (const MirBlock &block : body.blocks) {
    if (!block.instructions.empty() &&
        block.instructions.back().id == definition->id) {
      return block.terminator.kind == MirTerminatorKind::Return &&
                     block.terminator.value &&
                     block.terminator.value->kind == MirOperandKind::Value &&
                     block.terminator.value->value == value
                 ? definition
                 : nullptr;
    }
  }
  return nullptr;
}

// A class-valued failure Return may publish a moved local instead: the
// Move sits in the Return's own block with nothing between them touching
// the source place, the value's uses are exactly the Return plus at most
// one Value-rooted place that only a Drop touches, and no instruction or
// terminator anywhere else references the source place after the Move.
// The out-parameter's move-assignment then consumes the source directly
// at the point the Move proved it live.
[[nodiscard]] const MirInstruction *returnMoveDefinition(const MirBody &body,
                                                         MirValueId value) {
  const MirValue *record = body.findValue(value);
  const MirInstruction *definition =
      record == nullptr ? nullptr : findInstruction(body, record->definition);
  if (record == nullptr || record->info.type.kind != SemanticType::Class ||
      definition == nullptr || definition->kind != MirInstructionKind::Move ||
      !definition->result || definition->operands.size() != 1 ||
      definition->operands.front().kind != MirOperandKind::Move ||
      definition->operands.front().place == 0) {
    return nullptr;
  }
  const MirPlaceId source = definition->operands.front().place;
  // Same-block adjacency only: the Return's block contains the Move and
  // nothing after it references the source place. The cross-block form
  // (Move before an Invoke whose success target returns the value)
  // proved unsound as a simple allowance — the slot-state flow needs a
  // real proof before that widening returns.
  const MirBlock *home = nullptr;
  for (const MirBlock &candidate : body.blocks) {
    for (const MirInstruction &member : candidate.instructions) {
      if (member.id == definition->id) {
        home = &candidate;
      }
    }
  }
  if (home == nullptr) {
    return nullptr;
  }
  const auto returnsValue = [&](const MirBlock &block) {
    return block.terminator.kind == MirTerminatorKind::Return &&
           block.terminator.value &&
           block.terminator.value->kind == MirOperandKind::Value &&
           block.terminator.value->value == value;
  };
  const auto touchesSource = [&](const MirInstruction &member) {
    if (member.kind == MirInstructionKind::Drop) {
      // A drop of the moved-from source is a no-op by representation.
      return false;
    }
    if ((member.destination && *member.destination == source) ||
        (member.receiver && member.receiver->place == source)) {
      return true;
    }
    for (const MirOperand &operand : member.operands) {
      if (operand.place == source) {
        return true;
      }
    }
    return false;
  };
  const MirBlock *returnBlock = nullptr;
  // Every path leaving the Move's block may touch the source only via
  // drops — this single scan vets same-block secondary Returns and the
  // cross-block form alike, because def-dominance routes every
  // value-consuming Return through blocks reachable from here.
  {
    const auto successorsOf = [&](MirBlockId id, std::vector<MirBlockId> &out) {
      for (const MirBlock &candidate : body.blocks) {
        if (candidate.id != id) {
          continue;
        }
        const MirTerminator &edge = candidate.terminator;
        if (edge.kind == MirTerminatorKind::Goto ||
            edge.kind == MirTerminatorKind::Branch ||
            edge.kind == MirTerminatorKind::Invoke) {
          out.push_back(edge.target);
        }
        if (edge.kind == MirTerminatorKind::Branch ||
            edge.kind == MirTerminatorKind::Invoke) {
          out.push_back(edge.elseTarget);
        }
      }
    };
    std::vector<MirBlockId> stack;
    std::vector<MirBlockId> seen;
    successorsOf(home->id, stack);
    while (!stack.empty()) {
      const MirBlockId id = stack.back();
      stack.pop_back();
      if (id == 0 || id == home->id ||
          std::find(seen.begin(), seen.end(), id) != seen.end()) {
        continue;
      }
      seen.push_back(id);
      for (const MirBlock &candidate : body.blocks) {
        if (candidate.id != id) {
          continue;
        }
        for (const MirInstruction &member : candidate.instructions) {
          if (touchesSource(member)) {
            return nullptr;
          }
        }
      }
      successorsOf(id, stack);
    }
  }
  if (returnsValue(*home)) {
    returnBlock = home;
  } else if (home->terminator.kind == MirTerminatorKind::Invoke &&
             home->terminator.target != 0) {
    // Cross-block form: the invoke's success target returns the value,
    // the Move's block dominates it — so no merge path reaches the
    // Return without executing the Move — and every block reachable
    // after the Move touches the source only through drops (a drop of
    // the moved-from local is a no-op by representation).
    const auto successors = [&](MirBlockId id, std::vector<MirBlockId> &out) {
      for (const MirBlock &candidate : body.blocks) {
        if (candidate.id != id) {
          continue;
        }
        const MirTerminator &edge = candidate.terminator;
        if (edge.kind == MirTerminatorKind::Goto ||
            edge.kind == MirTerminatorKind::Branch ||
            edge.kind == MirTerminatorKind::Invoke) {
          out.push_back(edge.target);
        }
        if (edge.kind == MirTerminatorKind::Branch ||
            edge.kind == MirTerminatorKind::Invoke) {
          out.push_back(edge.elseTarget);
        }
      }
    };
    // Dominance by removal: the target is dominated by `home` when the
    // entry cannot reach it without passing through `home`.
    bool dominated = true;
    {
      std::vector<MirBlockId> stack{body.entry};
      std::vector<MirBlockId> seen;
      while (!stack.empty()) {
        const MirBlockId id = stack.back();
        stack.pop_back();
        if (id == 0 || id == home->id ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
          continue;
        }
        seen.push_back(id);
        if (id == home->terminator.target) {
          dominated = false;
          break;
        }
        successors(id, stack);
      }
    }
    const MirBlock *target = nullptr;
    for (const MirBlock &candidate : body.blocks) {
      if (candidate.id == home->terminator.target) {
        target = &candidate;
      }
    }
    bool clean = dominated && target != nullptr && returnsValue(*target);
    if (clean) {
      // Every block reachable from the Move's block may only drop the
      // source.
      std::vector<MirBlockId> stack;
      std::vector<MirBlockId> seen;
      successors(home->id, stack);
      while (!stack.empty() && clean) {
        const MirBlockId id = stack.back();
        stack.pop_back();
        if (id == 0 || id == home->id ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
          continue;
        }
        seen.push_back(id);
        for (const MirBlock &candidate : body.blocks) {
          if (candidate.id != id) {
            continue;
          }
          for (const MirInstruction &member : candidate.instructions) {
            if (touchesSource(member)) {
              clean = false;
            }
          }
        }
        successors(id, stack);
      }
    }
    if (clean) {
      returnBlock = target;
    }
  }
  if (returnBlock == nullptr) {
    return nullptr;
  }
  bool afterMove = false;
  for (const MirInstruction &member : home->instructions) {
    if (member.id == definition->id) {
      afterMove = true;
      continue;
    }
    if (afterMove && touchesSource(member)) {
      // Publication happens at the Move itself, so a later drop of the
      // moved-from source is the only admissible touch.
      return nullptr;
    }
  }
  const MirPlace *rooted = nullptr;
  std::size_t terminatorUses = 0;
  for (const MirValueUse &use : body.usesOf(value)) {
    if (use.kind == MirValueUseKind::Terminator) {
      // Every consuming terminator must be a Return of this value; the
      // universal reachable-only-drops scan above proved the source
      // untouched on every path to each publication.
      const MirBlock *user = nullptr;
      for (const MirBlock &candidate : body.blocks) {
        if (candidate.id == use.block) {
          user = &candidate;
        }
      }
      if (user == nullptr || !returnsValue(*user)) {
        return nullptr;
      }
      ++terminatorUses;
      continue;
    }
    if (use.kind == MirValueUseKind::PlaceRoot && rooted == nullptr) {
      rooted = body.findPlace(use.place);
      if (rooted == nullptr) {
        return nullptr;
      }
      continue;
    }
    return nullptr;
  }
  if (terminatorUses == 0) {
    return nullptr;
  }
  if (rooted != nullptr) {
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        const bool dropsIt = instruction.kind == MirInstructionKind::Drop &&
                             instruction.destination &&
                             *instruction.destination == rooted->id;
        if (dropsIt) {
          continue;
        }
        if (instruction.destination && *instruction.destination == rooted->id) {
          return nullptr;
        }
        if (instruction.receiver && instruction.receiver->place == rooted->id) {
          return nullptr;
        }
        for (const MirOperand &operand : instruction.operands) {
          if (operand.place == rooted->id) {
            return nullptr;
          }
        }
      }
    }
  }
  return definition;
}

// An Unexpected value never materializes: std::unexpected has no default
// construction, so the SSA declare-then-assign pattern cannot hold it. Its
// single consumer (the Return that converts it into the expected-typed
// result) spells the construction inline instead.
[[nodiscard]] const MirInstruction *unexpectedDefinition(const MirBody &body,
                                                         MirValueId value) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Compute &&
          instruction.operation == MirOperation::Unexpected &&
          instruction.result && *instruction.result == value) {
        return &instruction;
      }
    }
  }
  return nullptr;
}

// A call-result loan pairs to its producing call through the shared HIR
// value; ambiguity declines exactly like the Borrow pairing below.
[[nodiscard]] const MirLoan *
producedCallResultLoan(const MirBody &body, const MirInstruction &producer) {
  if (producer.hirValue == 0) {
    return nullptr;
  }
  const MirLoan *found = nullptr;
  for (const MirLoan &loan : body.loans) {
    if (loan.kind == MirLoanKind::CallResult &&
        loan.producedBy == producer.hirValue) {
      if (found != nullptr) {
        return nullptr;
      }
      found = &loan;
    }
  }
  return found;
}

// The transformed reference-returning call that produces a call-result
// loan (ADR 018 §5): the callee publishes its loan pointer through the
// caller's `T **` out-argument, which is exactly the caller's loan local.
[[nodiscard]] const MirInstruction *
loanProducingReferenceCall(const MirProgram &program, const MirBody &body,
                           const MirLoan &loan) {
  if (loan.producedBy == 0) {
    return nullptr;
  }
  const MirInstruction *found = nullptr;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Call ||
          instruction.hirValue != loan.producedBy ||
          !instruction.functionTarget ||
          instruction.intrinsic != IntrinsicKind::None) {
        continue;
      }
      if (found != nullptr) {
        return nullptr;
      }
      found = &instruction;
    }
  }
  if (found == nullptr) {
    return nullptr;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*found->functionTarget);
  if (target == nullptr || !target->mayRaiseDefinedFailure ||
      target->returnType.kind != SemanticType::Reference ||
      target->linkage != LanguageLinkage::Gti ||
      target->definitionKind != MirFunctionInstance::DefinitionKind::Source) {
    return nullptr;
  }
  return found;
}

// The Borrow that publishes a discharged read's element pairs with its
// producing call through the shared HIR value; ambiguity declines.
[[nodiscard]] const MirInstruction *pairedDischargedRead(const MirBody &body,
                                                         HirValueId produced) {
  const MirInstruction *found = nullptr;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (dischargedStorageReadCall(instruction) &&
          instruction.hirValue == produced) {
        if (found != nullptr) {
          return nullptr;
        }
        found = &instruction;
      }
    }
  }
  return found;
}

[[nodiscard]] std::string_view
prefixStorageHelperSpelling(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::AllocatePrefixStorage:
    return "::gti_internal::backend::mir_prefix_allocate_v1";
  case IntrinsicKind::PrefixStorageAppend:
    return "::gti_internal::backend::mir_prefix_append_v1";
  case IntrinsicKind::PrefixStoragePop:
    return "::gti_internal::backend::mir_prefix_pop_v1";
  case IntrinsicKind::PrefixStorageInsert:
    return "::gti_internal::backend::mir_prefix_insert_v1";
  case IntrinsicKind::PrefixStorageErase:
    return "::gti_internal::backend::mir_prefix_erase_v1";
  case IntrinsicKind::PrefixStorageRelocate:
    return "::gti_internal::backend::mir_prefix_relocate_v1";
  default:
    return "";
  }
}

// A value produced by loading a storage-typed place stages the storage for
// exactly one storage-intrinsic call: it never materializes as a local, and
// the call spells the place lvalue directly.
[[nodiscard]] const MirPlace *storageStagedPlace(const MirBody &body,
                                                 const MirOperand &operand) {
  const MirInstruction *definition = definitionFor(body, operand);
  if (definition == nullptr || definition->kind != MirInstructionKind::Load ||
      definition->operands.size() != 1 ||
      definition->operands.front().place == 0) {
    return nullptr;
  }
  const MirPlace *place = body.findPlace(definition->operands.front().place);
  return place != nullptr && (place->type.kind == SemanticType::Storage ||
                              place->type.kind == SemanticType::PrefixStorage)
             ? place
             : nullptr;
}

[[nodiscard]] bool isStorageStagedResult(const MirBody &body,
                                         const MirValue &value) {
  const MirInstruction *definition = findInstruction(body, value.definition);
  if (definition == nullptr || definition->kind != MirInstructionKind::Load ||
      definition->operands.size() != 1 ||
      definition->operands.front().place == 0) {
    return false;
  }
  const MirPlace *place = body.findPlace(definition->operands.front().place);
  return place != nullptr && (place->type.kind == SemanticType::Storage ||
                              place->type.kind == SemanticType::PrefixStorage);
}

[[nodiscard]] bool scalarSpellableArithmeticIntrinsic(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::IntegerWrappingAdd:
  case IntrinsicKind::IntegerWrappingSubtract:
  case IntrinsicKind::IntegerWrappingMultiply:
  case IntrinsicKind::IntegerSaturatingAdd:
  case IntrinsicKind::IntegerSaturatingSubtract:
  case IntrinsicKind::IntegerSaturatingMultiply:
    return true;
  default:
    return false;
  }
}

// The per-body facts the scalar text step spells from. Function and
// destructor instances project onto the same shape: a destructor has no
// parameters, its receiver is inherently mutable, and its banner names a
// destructor-instance.
struct ScalarBodyFacts {
  const MirBody &body;
  std::uint64_t id;
  std::string_view instanceLabel;
  std::optional<HirClassInstanceId> owner;
  const std::vector<HirBindingId> &parameterBindings;
  ReceiverMutability receiverMutability;
};

class ScalarBodyTextEmitter {
public:
  ScalarBodyTextEmitter(const MirProgram &program,
                        const CppMirBodyEmissionMap &representations,
                        std::size_t indentation, bool failureForm = false)
      : program(program), representations(representations),
        indentation(indentation), failureForm(failureForm) {}

  [[nodiscard]] std::string emit(const MirFunctionInstance &function,
                                 std::string_view familyLabel) {
    return emit(
        ScalarBodyFacts{.body = function.body,
                        .id = function.id,
                        .instanceLabel = "function-instance",
                        .owner = function.owner,
                        .parameterBindings = function.parameterBindings,
                        .receiverMutability = function.receiverMutability},
        familyLabel);
  }

  [[nodiscard]] std::string emit(const MirDestructorInstance &destructor,
                                 std::string_view familyLabel) {
    return emit(
        ScalarBodyFacts{.body = destructor.body,
                        .id = destructor.id,
                        .instanceLabel = "destructor-instance",
                        .owner = destructor.owner == 0
                                     ? std::optional<HirClassInstanceId>()
                                     : std::optional(destructor.owner),
                        .parameterBindings = emptyParameterBindings(),
                        .receiverMutability = ReceiverMutability::Mutable},
        familyLabel);
  }

  // Spells one literal value through the shared literal writer so the
  // initializer-schedule surface reuses the exact range assertions and type
  // spellings of the body text step.
  [[nodiscard]] std::string literalSpelling(const Literal &literal,
                                            const SemanticType &type) {
    output.str("");
    emitLiteral(literal, type);
    return output.str();
  }

  // Mirrors emitLiteral's dispatch without throwing, so a probing caller
  // can decline unsupported literal representations fail-closed.
  [[nodiscard]] static bool spellableLiteral(const Literal &literal,
                                             const SemanticType &type) {
    if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
      return integerFitsType(*integer, type);
    }
    if (std::holds_alternative<CharacterLiteral>(literal) ||
        std::holds_alternative<bool>(literal)) {
      return true;
    }
    if (std::holds_alternative<std::string>(literal)) {
      return type.kind == SemanticType::StringView;
    }
    if (const auto *value = std::get_if<BinaryFloat>(&literal)) {
      return validBinaryFloat(*value) &&
             (value->format == BinaryFloatFormat::Binary64
                  ? type == SemanticType::Double
                  : type == SemanticType::Float);
    }
    return false;
  }

  [[nodiscard]] std::string emit(const MirConstructorInstance &constructor,
                                 std::string_view familyLabel) {
    // A constructor projects like a mutable-receiver member: its receiver
    // is inherently mutable while the object is under construction, and
    // its banner names a constructor-instance.
    return emit(
        ScalarBodyFacts{.body = constructor.body,
                        .id = constructor.id,
                        .instanceLabel = "constructor-instance",
                        .owner = constructor.owner == 0
                                     ? std::optional<HirClassInstanceId>()
                                     : std::optional(constructor.owner),
                        .parameterBindings = constructor.parameterBindings,
                        .receiverMutability = ReceiverMutability::Mutable},
        familyLabel);
  }

  [[nodiscard]] std::string emit(const MirLambdaInstance &lambda,
                                 std::string_view familyLabel) {
    // A lambda body spells only nested inside its closure literal: the
    // receiver is the immutable closure object and capture places spell
    // through their Capture rows rather than storage rows.
    return emit(
        ScalarBodyFacts{.body = lambda.body,
                        .id = lambda.id,
                        .instanceLabel = "lambda-instance",
                        .owner = std::optional<HirClassInstanceId>(),
                        .parameterBindings = lambda.parameterBindings,
                        .receiverMutability = ReceiverMutability::ReadOnly},
        familyLabel);
  }

  [[nodiscard]] std::string emit(const ScalarBodyFacts &facts,
                                 std::string_view familyLabel) {
    currentFamilyLabel = familyLabel;
    output.str("");
    output << "{\n";
    ++indentation;
    writeIndent();
    output << "// GTI verified-MIR body: " << familyLabel << " "
           << facts.instanceLabel << " " << facts.id << "\n";
    for (const MirPlace &place : facts.body.places) {
      // An owning class local lives in a sealed lifetime slot so failure
      // and scope cleanup can destroy it exactly once from verified MIR.
      if (slotPlace(place)) {
        if (canonicalSlotPlaceId(facts.body, place) != place.id) {
          // The duplicate view shares its binding's canonical slot.
          continue;
        }
        writeIndent();
        output << lifetimeSlotSpelling() << '<' << typeSpelling(place.type)
               << "> __gti_mir_p_" << place.id << ";\n";
        // A slot-allocated parameter engages its slot from the argument;
        // MIR models the parameter as initialized at entry.
        if (const std::optional<std::size_t> parameter =
                parameterIndex(place, facts)) {
          writeIndent();
          output << "__gti_mir_p_" << place.id
                 << ".construct(std::move(__gti_mir_arg_" << *parameter
                 << "));\n";
        }
        continue;
      }
      // A storage-rooted place reads or writes its named global directly.
      if (place.root == MirPlaceRootKind::Symbol) {
        continue;
      }
      // An element place spells as subscription on its sibling array.
      if (arrayElementAccess(facts.body, place)) {
        continue;
      }
      // A view element spells through the terminal checked helper.
      if (viewElementAccess(facts.body, place)) {
        continue;
      }
      // A loan carrier place spells through its loan pointer (ADR 018).
      if (place.root == MirPlaceRootKind::Loan) {
        continue;
      }
      // A by-value argument staging temporary never materializes; the
      // consuming call spells the source place.
      if (copyStageForTemporary(facts.body, place) != nullptr) {
        continue;
      }
      // A pure root record spells nothing.
      if (unreferencedValueRootedPlace(facts.body, place)) {
        continue;
      }
      // A This-rooted field element spells through the live member.
      if (place.root == MirPlaceRootKind::This &&
          place.projections.size() == 2 &&
          place.projections[0].kind == MirProjectionKind::Field &&
          place.projections[1].kind == MirProjectionKind::Index) {
        continue;
      }
      // A dereference-projected place spells through its base carrier.
      if (place.root == MirPlaceRootKind::Binding &&
          !place.projections.empty() &&
          place.projections[0].kind == MirProjectionKind::Dereference) {
        continue;
      }
      // A reference parameter keeps its C++ reference at the signature and
      // binds a pointer carrier in the body (ADR 018).
      if (place.root == MirPlaceRootKind::Binding &&
          place.projections.empty() &&
          place.type.kind == SemanticType::Reference) {
        const std::optional<std::size_t> parameter =
            parameterIndex(place, facts);
        if (!parameter) {
          throw std::logic_error(
              "reference local outside parameter binding is not in the loan "
              "vocabulary");
        }
        writeIndent();
        if (place.type.referenceAccess == AccessMode::ReadOnly) {
          output << "const ";
        }
        output << "auto *__gti_mir_p_" << place.id << " = &__gti_mir_arg_"
               << *parameter << ";\n";
        continue;
      }
      // Receiver-place handling is derived from MIR, not selected by the
      // caller: a This-rooted place is the projection carrier (skipped) or
      // one projected field bound by reference to the live member. No
      // admitted body declares a receiver as an ordinary local.
      if (place.root == MirPlaceRootKind::This) {
        // The bare receiver place is only the projection carrier and is never
        // referenced. A field place binds by reference so every load reads
        // the live member and every store lands in it. Constness follows the
        // receiver, not the per-place access mode: a store destination and a
        // read of the same field share one binding, and the probe rejects
        // stores to This-rooted places under a read-only receiver.
        if (place.projections.empty()) {
          continue;
        }
        writeIndent();
        output << (facts.receiverMutability == ReceiverMutability::Mutable
                       ? "auto &__gti_mir_p_"
                       : "const auto &__gti_mir_p_")
               << place.id << " = (*this)."
               << fieldSpelling(facts, place.projections.front().field)
               << ";\n";
        continue;
      }
      // A lambda-typed local declares only under a template emission's
      // overlay row (spelling its template parameter name); otherwise its
      // C++ closure type is unnameable and every consumer spells the
      // fused literal inline.
      if (place.type.kind == SemanticType::Lambda &&
          !typeRowExists(place.type)) {
        continue;
      }
      writeIndent();
      output << typeSpelling(place.type) << " __gti_mir_p_" << place.id;
      if (const std::optional<std::size_t> parameter =
              parameterIndex(place, facts)) {
        // A move-only owner parameter cannot copy-initialize its local.
        if (place.type.kind == SemanticType::UniqueOwner) {
          output << " = std::move(__gti_mir_arg_" << *parameter << ')';
        } else {
          output << " = __gti_mir_arg_" << *parameter;
        }
      } else {
        output << "{}";
      }
      output << ";\n";
    }
    for (const MirValue &value : facts.body.values) {
      if (value.info.type.kind == SemanticType::Class) {
        // A class value declares only when its row carries the 0.215
        // boundary proof AND its definition is the value-producing
        // construction that assigns into it — a blanket declaration would
        // run default constructors for values other vocabularies spell
        // without a local.
        const MirInstruction *definition =
            findInstruction(facts.body, value.definition);
        const bool valueProducingConstruct =
            definition != nullptr &&
            definition->kind == MirInstructionKind::Construct &&
            definition->result && *definition->result == value.id &&
            !definition->destination && !definition->receiver &&
            returnConstructDefinition(facts.body, value.id) != definition &&
            !slotConsumedConstruct(facts, *definition);
        // A transformed callee's class result lands in the declared
        // receiving local through the `T *` out-parameter.
        const bool transformedClassResult =
            !valueProducingConstruct && definition != nullptr &&
            definition->kind == MirInstructionKind::Call &&
            definition->result && *definition->result == value.id &&
            transformedCallee(*definition) != nullptr;
        const auto row = std::find_if(
            representations.types().begin(), representations.types().end(),
            [&](const CppMirTypeRepresentation &candidate) {
              return candidate.type == value.info.type;
            });
        if ((!valueProducingConstruct && !transformedClassResult) ||
            row == representations.types().end() || row->spelling.empty() ||
            !row->boundaryConstructible) {
          continue;
        }
        writeIndent();
        output << typeSpelling(value.info.type) << " __gti_mir_v_" << value.id
               << "{};\n";
        continue;
      }
      // A borrow-staged call input never materializes: the call spells its
      // place expression directly, so the staged value has no local.
      if (isBorrowStagedResult(facts.body, value)) {
        continue;
      }
      // A storage-staged load never materializes either: the storage
      // intrinsic call spells the storage place lvalue directly.
      if (isStorageStagedResult(facts.body, value)) {
        continue;
      }
      // An Unexpected result spells inline at its consuming Return and
      // never declares: std::unexpected has no default construction.
      if (unexpectedDefinition(facts.body, value.id) != nullptr) {
        continue;
      }
      // A lambda-typed value never declares either; the fused closure
      // chain spells the literal at each consuming invocation.
      if (value.info.type.kind == SemanticType::Lambda) {
        continue;
      }
      // A reference-typed value never declares; its paired call-result
      // loan pointer carries the referent.
      if (value.info.type.kind == SemanticType::Reference) {
        continue;
      }
      // A discharged read's element is published through its loan
      // pointer, never copied into a local.
      {
        bool dischargedReadResult = false;
        for (const MirBlock &block : facts.body.blocks) {
          for (const MirInstruction &instruction : block.instructions) {
            if (dischargedStorageReadCall(instruction) && instruction.result &&
                *instruction.result == value.id) {
              dischargedReadResult = true;
            }
          }
        }
        if (dischargedReadResult) {
          continue;
        }
      }
      writeIndent();
      output << typeSpelling(value.info.type) << " __gti_mir_v_" << value.id
             << "{};\n";
    }
    for (const MirLoan &loan : facts.body.loans) {
      // A Stored loan binds its reference field in the member initializer
      // list; no pointer local exists in the body.
      if (loan.kind == MirLoanKind::Stored) {
        continue;
      }
      const MirPlace *source = facts.body.findPlace(loan.source);
      if (source == nullptr) {
        throw std::logic_error("verified MIR loan lost its source place");
      }
      writeIndent();
      if (loan.access == AccessMode::ReadOnly) {
        output << "const ";
      }
      // A loan published by a discharged storage read points at the
      // element, not the storage that sources it.
      if ((source->type.kind == SemanticType::Storage ||
           source->type.kind == SemanticType::PrefixStorage) &&
          pairedDischargedRead(facts.body, loan.producedBy) != nullptr) {
        if (source->type.arguments.empty()) {
          throw std::logic_error(
              "verified MIR storage loan lost its element type");
        }
        output << typeSpelling(source->type.arguments.front())
               << " *__gti_mir_loan_" << loan.id << "{};\n";
        continue;
      }
      if (loan.kind == MirLoanKind::Parameter) {
        // The entry loan aliases the reference parameter's pointer
        // carrier (ADR 018 §4): the place prelude above already bound
        // the carrier from the argument, so the loan pointer copies it.
        writeIndent();
        output << "auto *__gti_mir_loan_" << loan.id << " = __gti_mir_p_"
               << loan.source << ";\n";
        continue;
      }
      // A loan published by a transformed reference-returning callee
      // points at the callee's return element, not the receiver that
      // sources it (ADR 018 §5).
      if (loan.kind == MirLoanKind::CallResult) {
        const MirInstruction *call =
            loanProducingReferenceCall(program, facts.body, loan);
        const MirFunctionInstance *target =
            call != nullptr && call->functionTarget
                ? program.findFunctionInstance(*call->functionTarget)
                : nullptr;
        if (target == nullptr || target->returnType.arguments.empty()) {
          throw std::logic_error(
              "verified MIR call-result loan lost its element type");
        }
        output << typeSpelling(target->returnType.arguments.front())
               << " *__gti_mir_loan_" << loan.id << "{};\n";
        continue;
      }
      output << typeSpelling(source->type) << " *__gti_mir_loan_" << loan.id
             << "{};\n";
    }
    if (failureForm) {
      for (const MirBlock &block : facts.body.blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          if (instruction.kind == MirInstructionKind::Compute &&
              !cppMirCheckedOperationHelperSpelling(instruction.operation)
                   .empty() &&
              !instruction.localFailureSites.empty()) {
            writeIndent();
            output << "::gti_internal::backend::mir_failure_status_v1 "
                      "__gti_mir_failure_status_"
                   << instruction.id
                   << " = ::gti_internal::backend::mir_failure_success_v1;\n";
          }
          if ((instruction.kind == MirInstructionKind::Load ||
               instruction.kind == MirInstructionKind::Assign) &&
              instruction.localFailureSites.size() == 1) {
            writeIndent();
            output << "::gti_internal::backend::mir_failure_status_v1 "
                      "__gti_mir_failure_status_"
                   << instruction.id
                   << " = ::gti_internal::backend::mir_failure_success_v1;\n";
          }
          if (instruction.kind == MirInstructionKind::Call &&
              (checkedConversionIntrinsicCall(instruction) ||
               (prefixStorageIntrinsic(instruction.intrinsic) &&
                !instruction.localFailureSites.empty()))) {
            writeIndent();
            output << "::gti_internal::backend::mir_failure_status_v1 "
                      "__gti_mir_failure_status_"
                   << instruction.id
                   << " = ::gti_internal::backend::mir_failure_success_v1;\n";
          }
          if (instruction.kind == MirInstructionKind::Call &&
              transformedCallee(instruction) != nullptr) {
            writeIndent();
            output << "bool __gti_mir_call_success_" << instruction.id
                   << " = false;\n";
            const SemanticType &calleeReturn =
                transformedCallee(instruction)->returnType;
            if (calleeReturn.kind == SemanticType::Reference &&
                producedCallResultLoan(facts.body, instruction) == nullptr) {
              writeIndent();
              if (calleeReturn.referenceAccess == AccessMode::ReadOnly) {
                output << "const ";
              }
              output << typeSpelling(calleeReturn.arguments.front())
                     << " *__gti_mir_discard_" << instruction.id << "{};\n";
            } else if (!instruction.result &&
                       calleeReturn != SemanticType::Void &&
                       calleeReturn.kind != SemanticType::Reference) {
              writeIndent();
              output << typeSpelling(calleeReturn) << " __gti_mir_discard_"
                     << instruction.id << "{};\n";
            }
          }
        }
      }
    }
    writeIndent();
    output << "std::size_t __gti_mir_bb = " << facts.body.entry << ";\n";
    writeIndent();
    output << "for (;;) {\n";
    ++indentation;
    writeIndent();
    output << "switch (__gti_mir_bb) {\n";
    ++indentation;
    for (const MirBlock &block : facts.body.blocks) {
      writeIndent();
      output << "case " << block.id << ": {\n";
      ++indentation;
      if (failureForm && block.failureParameter != 0) {
        writeIndent();
        output << "// GTI MIR failure-record " << block.failureParameter
               << " cleanup\n";
      }
      for (const MirInstruction &instruction : block.instructions) {
        emitInstruction(instruction, facts);
      }
      emitTerminator(block.terminator, facts);
      --indentation;
      writeIndent();
      output << "}\n";
    }
    writeIndent();
    output << "default:\n";
    ++indentation;
    writeIndent();
    output << "std::abort();\n";
    --indentation;
    --indentation;
    writeIndent();
    output << "}\n";
    --indentation;
    writeIndent();
    output << "}\n";
    --indentation;
    writeIndent();
    output << "}\n";
    return output.str();
  }

private:
  [[nodiscard]] static const std::vector<HirBindingId> &
  emptyParameterBindings() {
    static const std::vector<HirBindingId> empty;
    return empty;
  }

  void writeIndent() {
    for (std::size_t index = 0; index < indentation; ++index) {
      output << "  ";
    }
  }

  [[nodiscard]] static MirPlaceId
  constructDestination(const ScalarBodyFacts &facts,
                       const MirInstruction &construct) {
    if (!construct.result) {
      return 0;
    }
    MirPlaceId selected = 0;
    for (const MirBlock &block : facts.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Initialize ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Value ||
            instruction.operands.front().value != *construct.result ||
            !instruction.destination) {
          continue;
        }
        if (selected != 0) {
          return 0;
        }
        selected = *instruction.destination;
      }
    }
    return selected;
  }

  // The slot protocol owns any construct whose value a slot-place
  // Initialize consumes; the value route must not bypass slot engagement.
  [[nodiscard]] bool
  slotConsumedConstruct(const ScalarBodyFacts &facts,
                        const MirInstruction &instruction) const {
    if (!instruction.result) {
      return false;
    }
    for (const MirValueUse &use : facts.body.usesOf(*instruction.result)) {
      const MirInstruction *consumer =
          findInstruction(facts.body, use.instruction);
      if (consumer != nullptr &&
          consumer->kind == MirInstructionKind::Initialize &&
          consumer->destination) {
        const MirPlace *destinationPlace =
            facts.body.findPlace(*consumer->destination);
        if (destinationPlace != nullptr && slotPlace(*destinationPlace)) {
          return true;
        }
      }
    }
    return false;
  }

  [[nodiscard]] bool slotPlace(const MirPlace &place) const {
    return place.root == MirPlaceRootKind::Binding &&
           (place.type.kind == SemanticType::Class ||
            place.type.kind == SemanticType::Storage ||
            place.type.kind == SemanticType::PrefixStorage) &&
           place.projections.empty();
  }

  // Duplicate bare binding places (a mutable store view and a read-only
  // view of one local) share the local's single lifetime slot: every
  // sibling spells the lowest-id place so the construct, each read, and
  // the destroy all touch the same slot.
  [[nodiscard]] MirPlaceId canonicalSlotPlaceId(const MirBody &body,
                                                const MirPlace &place) const {
    MirPlaceId canonical = place.id;
    for (const MirPlace &candidate : body.places) {
      if (candidate.root == MirPlaceRootKind::Binding &&
          candidate.binding == place.binding && candidate.projections.empty() &&
          candidate.type.kind == place.type.kind && candidate.id < canonical) {
        canonical = candidate.id;
      }
    }
    return canonical;
  }

  [[nodiscard]] const std::string &lifetimeSlotSpelling() {
    const auto found = std::find_if(
        representations.capabilities().begin(),
        representations.capabilities().end(),
        [](const CppMirEmissionCapabilityRepresentation &row) {
          return row.kind == CppMirEmissionCapabilityKind::LifetimeStorage;
        });
    if (found == representations.capabilities().end() ||
        found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost the sealed lifetime-slot helper");
    }
    return found->spelling;
  }

  [[nodiscard]] const std::string &storageSpelling(SymbolId symbol) {
    const auto found = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Storage &&
                 row.owner == 0 && row.symbol == symbol && row.ordinal == 0;
        });
    if (found == representations.symbols().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact storage symbol row");
    }
    return found->spelling;
  }

  [[nodiscard]] const std::string &captureSpelling(std::size_t lambdaOwner,
                                                   SymbolId symbol,
                                                   std::size_t ordinal) {
    const auto found = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Capture &&
                 row.owner == lambdaOwner && row.symbol == symbol &&
                 row.ordinal == ordinal;
        });
    if (found == representations.symbols().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact capture name row");
    }
    return found->spelling;
  }

  [[nodiscard]] bool typeRowExists(const SemanticType &type) const {
    return std::any_of(
        representations.types().begin(), representations.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
  }

  [[nodiscard]] const std::string &typeSpelling(const SemanticType &type) {
    const auto found = std::find_if(
        representations.types().begin(), representations.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    if (found == representations.types().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost a copied type row");
    }
    return found->spelling;
  }

  [[nodiscard]] const std::string &fieldSpelling(const ScalarBodyFacts &facts,
                                                 SymbolId field) {
    if (!facts.owner) {
      throw std::logic_error(
          "general MIR body emission lost the receiver class instance");
    }
    const auto found = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Field &&
                 row.owner == *facts.owner && row.symbol == field &&
                 row.ordinal == 0;
        });
    if (found == representations.symbols().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact field symbol row");
    }
    return found->spelling;
  }

  [[nodiscard]] const std::string &bodySpelling(HirFunctionInstanceId target) {
    const MirBodyAddress address{.kind = MirBodyKind::Function,
                                 .owner = target};
    const auto found = std::find_if(
        representations.bodies().begin(), representations.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == address;
        });
    if (found == representations.bodies().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact call-target name row");
    }
    return found->spelling;
  }

  [[nodiscard]] static std::optional<std::size_t>
  parameterIndex(const MirPlace &place, const ScalarBodyFacts &facts) {
    const auto parameter =
        std::find(facts.parameterBindings.begin(),
                  facts.parameterBindings.end(), place.binding);
    if (parameter == facts.parameterBindings.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(
        std::distance(facts.parameterBindings.begin(), parameter));
  }

  [[nodiscard]] static bool
  isSyntheticLogicalConstant(const MirOperand &operand) {
    return operand.kind == MirOperandKind::Constant && operand.value == 0 &&
           operand.place == 0 && operand.loan == 0 && operand.literal &&
           operand.type == SemanticType::Bool &&
           std::holds_alternative<bool>(*operand.literal);
  }

  void emitIntegerLiteral(std::uint64_t value) {
    output << value;
    if (value >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      output << "ULL";
    }
  }

  // The emitted spelling must be value-faithful, not merely well-typed: a
  // magnitude outside the target type would round-trip through C++
  // conversion semantics instead of the verified GTI value, so it is
  // emission drift even when the structural probe admitted the body.
  [[nodiscard]] static bool integerFitsType(std::uint64_t value,
                                            const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int8_t>::max());
    case SemanticType::Int16:
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int16_t>::max());
    case SemanticType::Int32:
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int32_t>::max());
    case SemanticType::Int64:
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max());
    case SemanticType::UInt8:
      return value <= std::numeric_limits<std::uint8_t>::max();
    case SemanticType::UInt16:
      return value <= std::numeric_limits<std::uint16_t>::max();
    case SemanticType::UInt32:
      return value <= std::numeric_limits<std::uint32_t>::max();
    case SemanticType::UInt64:
      return true;
    default:
      return false;
    }
  }

  void emitLiteral(const Literal &literal, const SemanticType &type) {
    if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
      if (!integerFitsType(*integer, type)) {
        throw std::logic_error(
            "verified MIR scalar literal exceeds its exact result type");
      }
      output << "static_cast<" << typeSpelling(type) << ">(";
      emitIntegerLiteral(*integer);
      output << ')';
      return;
    }
    if (const auto *character = std::get_if<CharacterLiteral>(&literal)) {
      output << "std::uint8_t{" << static_cast<unsigned int>(character->value)
             << '}';
      return;
    }
    if (const auto *boolean = std::get_if<bool>(&literal)) {
      output << (*boolean ? "true" : "false");
      return;
    }
    if (const auto *text = std::get_if<std::string>(&literal)) {
      if (type.kind != SemanticType::StringView) {
        throw std::logic_error(
            "verified MIR string literal is not a string view");
      }
      output << cppMirStringViewLiteralSpelling(*text);
      return;
    }
    if (const auto *value = std::get_if<BinaryFloat>(&literal)) {
      const bool binary64 = value->format == BinaryFloatFormat::Binary64;
      if ((binary64 && type != SemanticType::Double) ||
          (!binary64 && type != SemanticType::Float)) {
        throw std::logic_error(
            "verified MIR floating literal disagrees with its typed value");
      }
      output << cppMirBinaryFloatLiteralSpelling(*value);
      return;
    }
    throw std::logic_error(
        "verified MIR scalar-CFG literal has an unsupported representation");
  }

  void emitOperand(const MirOperand &operand,
                   bool allowSyntheticLogicalConstant = false) {
    if (operand.kind == MirOperandKind::Value) {
      output << "__gti_mir_v_" << operand.value;
      return;
    }
    if (allowSyntheticLogicalConstant && isSyntheticLogicalConstant(operand)) {
      emitLiteral(*operand.literal, operand.type);
      return;
    }
    throw std::logic_error(
        "verified MIR scalar-CFG operand is not a proven value");
  }

  [[nodiscard]] std::string_view expectedConstructionSpelling() const {
    const auto found = std::find_if(
        representations.capabilities().begin(),
        representations.capabilities().end(),
        [](const CppMirEmissionCapabilityRepresentation &row) {
          return row.kind == CppMirEmissionCapabilityKind::Expected;
        });
    if (found == representations.capabilities().end() ||
        found->spelling.empty()) {
      throw std::logic_error(
          "verified MIR Unexpected lost its Expected capability row");
    }
    return found->spelling;
  }

  // Spells the full inline literal for the fused chain that produced
  // `receiver`: captures from the lambda's Capture rows over the enclosing
  // body's place expressions, positional parameters, and the recursively
  // emitted verified lambda body carrying its own banner marker.
  void emitClosureLiteral(const ScalarBodyFacts &facts, MirValueId receiver) {
    const MirInstruction *closure =
        closureChainDefinition(facts.body, receiver);
    const MirLambdaInstance *lambda =
        closure != nullptr && closure->lambdaTarget
            ? program.findLambda(*closure->lambdaTarget)
            : nullptr;
    if (lambda == nullptr) {
      throw std::logic_error(
          "verified MIR invocation lost its fused closure chain");
    }
    output << '[';
    for (std::size_t index = 0; index < closure->operands.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      const MirPlace *captured =
          facts.body.findPlace(closure->operands[index].place);
      if (captured == nullptr) {
        throw std::logic_error("verified MIR closure lost a captured place");
      }
      output << captureSpelling(lambda->id, lambda->captureSymbols[index],
                                index + 1)
             << " = ";
      const bool moves = lambda->captureModes[index] == LambdaCaptureMode::Move;
      if (moves) {
        output << "std::move(";
      }
      emitStoragePlaceValue(facts, *captured);
      if (moves) {
        output << ')';
      }
    }
    output << "](";
    for (std::size_t index = 0; index < lambda->parameterTypes.size();
         ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << typeSpelling(lambda->parameterTypes[index]) << " __gti_mir_arg_"
             << index;
    }
    output << ") -> " << typeSpelling(lambda->returnType) << ' '
           << ScalarBodyTextEmitter(program, representations, indentation)
                  .emit(*lambda, currentFamilyLabel);
  }

  void emitCompute(const MirInstruction &instruction) {
    output << "__gti_mir_v_" << *instruction.result << " = ";
    if (instruction.operation == MirOperation::Literal) {
      emitLiteral(*instruction.literal, instruction.info.type);
      output << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::Identity) {
      emitOperand(instruction.operands.front());
      output << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::Aggregate) {
      output << typeSpelling(instruction.info.type) << "{};\n";
      return;
    }
    if (instruction.operation == MirOperation::Convert) {
      output << "static_cast<" << typeSpelling(instruction.info.type) << ">(";
      emitOperand(instruction.operands.front());
      output << ");\n";
      return;
    }
    if (instruction.operation == MirOperation::LogicalNot) {
      output << '!';
      emitOperand(instruction.operands.front());
      output << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::ExpectedHasValue) {
      emitOperand(instruction.operands.front());
      output << ".has_value();\n";
      return;
    }
    if (instruction.operation == MirOperation::Unexpected) {
      // Spelled inline by the consuming Return; nothing stages here. The
      // leading result assignment this writer already emitted is repaired
      // by the caller, which skips Unexpected before writing it.
      throw std::logic_error(
          "verified MIR Unexpected must spell at its consuming Return");
    }
    if (instruction.operation == MirOperation::Positive ||
        instruction.operation == MirOperation::BitwiseNot) {
      output << "static_cast<" << typeSpelling(instruction.info.type) << ">("
             << (instruction.operation == MirOperation::Positive ? '+' : '~');
      emitOperand(instruction.operands.front());
      output << ");\n";
      return;
    }
    if (instruction.operation == MirOperation::Index) {
      // A value-level view element read: the terminal helper reports the
      // defined bound contract and never returns on failure, exactly like
      // the place-projected form.
      output << "::gti_internal::backend::string_view_at(";
      emitOperand(instruction.operands[0]);
      output << ", ";
      emitOperand(instruction.operands[1]);
      output << ");\n";
      return;
    }
    const auto spelling = [&]() -> std::string_view {
      switch (instruction.operation) {
      case MirOperation::BitwiseAnd:
        return "&";
      case MirOperation::BitwiseOr:
        return "|";
      case MirOperation::BitwiseXor:
        return "^";
      case MirOperation::Equal:
        return "==";
      case MirOperation::NotEqual:
        return "!=";
      case MirOperation::Less:
        return "<";
      case MirOperation::LessEqual:
        return "<=";
      case MirOperation::Greater:
        return ">";
      case MirOperation::GreaterEqual:
        return ">=";
      default:
        throw std::logic_error(
            "verified MIR scalar-CFG compute operation is unsupported");
      }
    }();
    const bool castResult = instruction.operation == MirOperation::BitwiseAnd ||
                            instruction.operation == MirOperation::BitwiseOr ||
                            instruction.operation == MirOperation::BitwiseXor;
    if (castResult) {
      output << "static_cast<" << typeSpelling(instruction.info.type) << ">(";
    }
    const auto emitBinaryOperand = [&](const MirOperand &operand) {
      if (operand.kind == MirOperandKind::Constant && operand.literal) {
        emitLiteral(*operand.literal, operand.type);
        return;
      }
      emitOperand(operand);
    };
    emitBinaryOperand(instruction.operands[0]);
    output << ' ' << spelling << ' ';
    emitBinaryOperand(instruction.operands[1]);
    if (castResult) {
      output << ')';
    }
    output << ";\n";
  }

  // A store destination is the declared binding unless the place is
  // Symbol-rooted: a storage place has no local binding and reads and
  // writes its named global through the storage row, exactly like Load.
  [[nodiscard]] std::string destinationSpelling(const ScalarBodyFacts &facts,
                                                MirPlaceId destination) {
    const MirPlace *place = facts.body.findPlace(destination);
    if (place != nullptr && place->root == MirPlaceRootKind::Symbol) {
      return storageSpelling(place->symbol);
    }
    // A dereference-projected reference place stores through its sibling
    // pointer carrier (ADR 018 §4), exactly as the read path spells it.
    if (place != nullptr && place->root == MirPlaceRootKind::Binding &&
        place->projections.size() == 1 &&
        place->projections[0].kind == MirProjectionKind::Dereference) {
      for (const MirPlace &candidate : facts.body.places) {
        if (candidate.id != place->id &&
            candidate.root == MirPlaceRootKind::Binding &&
            candidate.binding == place->binding &&
            candidate.projections.empty()) {
          return "(*__gti_mir_p_" + std::to_string(candidate.id) + ")";
        }
      }
      throw std::logic_error(
          "dereference store destination lost its base carrier");
    }
    return "__gti_mir_p_" + std::to_string(destination);
  }

  void emitPlainInstruction(const MirInstruction &instruction,
                            const ScalarBodyFacts &facts) {
    writeIndent();
    if (instruction.kind == MirInstructionKind::Lifecycle) {
      if (instruction.fullExpressionEnd != 0) {
        output << "// GTI MIR full-expression boundary "
               << instruction.fullExpressionEnd << "\n";
      } else {
        output << "// GTI MIR cleanup boundary "
               << instruction.cleanupBoundaryEnd << "\n";
      }
      return;
    }
    if (instruction.kind == MirInstructionKind::Compute) {
      if (failureForm &&
          !cppMirCheckedOperationHelperSpelling(instruction.operation)
               .empty() &&
          !instruction.localFailureSites.empty()) {
        output << "__gti_mir_failure_status_" << instruction.id << " = "
               << cppMirCheckedOperationHelperSpelling(instruction.operation)
               << '<' << typeSpelling(instruction.info.type) << ">(";
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          if (index != 0) {
            output << ", ";
          }
          const MirOperand &operand = instruction.operands[index];
          if (operand.kind == MirOperandKind::Constant && operand.literal) {
            emitLiteral(*operand.literal, operand.type);
          } else {
            emitOperand(operand);
          }
        }
        if (!instruction.operands.empty()) {
          output << ", ";
        }
        output << "&__gti_mir_v_" << *instruction.result << ");\n";
        return;
      }
      if (instruction.operation == MirOperation::Unexpected) {
        // The consuming Return spells the construction inline; nothing
        // stages here and the result value is never declared.
        writeIndent();
        output << "// unexpected value " << *instruction.result
               << " spells at its consuming return\n";
        return;
      }
      if (!failureForm && !instruction.localFailureSites.empty() &&
          !cppMirTerminalCheckedHelperSpelling(instruction.operation).empty()) {
        // Plain literal shape: the compatibility terminal helper both
        // checks and contains, so the result assignment is unconditional
        // and the paired invoke edge is a plain goto.
        output << "__gti_mir_v_" << *instruction.result << " = "
               << cppMirTerminalCheckedHelperSpelling(instruction.operation)
               << '(';
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          if (index != 0) {
            output << ", ";
          }
          emitOperand(instruction.operands[index]);
        }
        output << ");\n";
        return;
      }
      emitCompute(instruction);
      return;
    }
    if (instruction.kind == MirInstructionKind::Borrow) {
      const MirLoan *loan = nullptr;
      for (const MirLoan &candidate : facts.body.loans) {
        if (instruction.loan && candidate.id == *instruction.loan) {
          loan = &candidate;
        }
      }
      if (loan != nullptr && loan->kind == MirLoanKind::Stored) {
        writeIndent();
        output << "// GTI MIR stored loan " << loan->id
               << " binds its reference field in the initializer list\n";
        return;
      }
      const MirPlace *source =
          loan == nullptr ? nullptr : facts.body.findPlace(loan->source);
      if (source == nullptr) {
        throw std::logic_error("verified MIR borrow lost its loan source");
      }
      // A loan produced by a discharged storage read publishes the element
      // address through the trusted reference helper: the bounds proof was
      // discharged by the enclosing container's logical-size check, so the
      // sealed runtime guard is defense in depth, exactly as on the
      // compatibility path.
      if (source->type.kind == SemanticType::Storage ||
          source->type.kind == SemanticType::PrefixStorage) {
        const MirInstruction *read =
            pairedDischargedRead(facts.body, loan->producedBy);
        if (read == nullptr || read->operands.size() != 2) {
          throw std::logic_error(
              "verified MIR storage loan lost its discharged read");
        }
        const MirPlace *storage =
            storageStagedPlace(facts.body, read->operands.front());
        if (storage == nullptr) {
          throw std::logic_error(
              "verified MIR discharged read lost its staged storage place");
        }
        output << "__gti_mir_loan_" << loan->id
               << " = &::gti_internal::backend::"
               << (read->intrinsic == IntrinsicKind::PrefixStorageReadMut
                       ? "prefix_storage_read_mut"
                       : "prefix_storage_read")
               << '(';
        emitStoragePlaceValue(facts, *storage);
        output << ", ";
        emitOperand(read->operands.back());
        output << ");\n";
        return;
      }
      output << "__gti_mir_loan_" << loan->id << " = &";
      emitPlaceExpression(facts, *source);
      output << ";\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::EndBorrow) {
      output << "// GTI MIR end-borrow loan "
             << (instruction.loan ? *instruction.loan : 0) << "\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Drop &&
        movedOutOwnerDrop(facts.body, instruction)) {
      // The owner's value moved out; the moved-from local's scope-end
      // destruction is a no-op by representation.
      writeIndent();
      output << "// GTI MIR moved-out owner drop of place "
             << *instruction.destination << "\n";
      return;
    }
    if (failureForm && instruction.kind == MirInstructionKind::Drop &&
        instruction.destination) {
      if (const MirPlace *place =
              facts.body.findPlace(*instruction.destination);
          place != nullptr && place->root == MirPlaceRootKind::Value &&
          returnMoveDefinition(facts.body, place->value) != nullptr) {
        // The out-parameter's move-assignment consumed this value; the
        // moved-from residual needs no destruction step.
        writeIndent();
        output << "// GTI MIR publication-consumed drop of place "
               << *instruction.destination << "\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Drop &&
        storeConsumedStorageValueDrop(facts.body, instruction)) {
      // The consuming store moved this value into its destination; the
      // moved-from local's scope-end destruction is a no-op.
      writeIndent();
      output << "// GTI MIR store-consumed storage value drop of place "
             << *instruction.destination << "\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Drop &&
        trivialMirDrop(facts.body, instruction)) {
      // Scope-end destruction of the declared local is exactly the
      // verified semantics for a trivial obligation.
      writeIndent();
      output << "// GTI MIR trivial drop of place " << *instruction.destination
             << "\n";
      return;
    }
    if (failureForm && instruction.kind == MirInstructionKind::Drop &&
        instruction.lifecycle.size() == 1 &&
        instruction.lifecycle.front().failureCleanup) {
      // Failure cleanup destroys the engaged slot exactly like the
      // success path: the propagate edge must never leak an engaged
      // lifetime slot past the early false return.
      const MirPlace *slot =
          instruction.destination
              ? facts.body.findPlace(*instruction.destination)
              : nullptr;
      if (slot == nullptr || !slotPlace(*slot)) {
        throw std::logic_error(
            "verified MIR failure cleanup lost its lifetime slot");
      }
      output << "__gti_mir_p_" << *instruction.destination << ".destroy();"
             << " // failure cleanup drop-obligation "
             << instruction.lifecycle.front().source << '\n';
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct) {
      const MirPlaceId destination = constructDestination(facts, instruction);
      const MirPlace *slot =
          destination == 0 ? nullptr : facts.body.findPlace(destination);
      if (slot == nullptr || !slotPlace(*slot)) {
        throw std::logic_error(
            "verified MIR class construction lost its reparent slot");
      }
      output << "__gti_mir_p_" << destination << ".construct(";
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (index != 0) {
          output << ", ";
        }
        const bool consumed =
            instruction.operands[index].type.kind == SemanticType::Class ||
            instruction.operands[index].type.kind ==
                SemanticType::UniqueOwner ||
            instruction.operands[index].type.kind == SemanticType::Storage ||
            instruction.operands[index].type.kind ==
                SemanticType::PrefixStorage;
        if (consumed) {
          output << "std::move(";
        }
        emitOperand(instruction.operands[index]);
        if (consumed) {
          output << ')';
        }
      }
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Drop) {
      const MirPlace *slot =
          instruction.destination
              ? facts.body.findPlace(*instruction.destination)
              : nullptr;
      if (slot == nullptr || !slotPlace(*slot)) {
        throw std::logic_error(
            "verified MIR class drop lost its lifetime slot");
      }
      output << "__gti_mir_p_" << *instruction.destination << ".destroy();\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Load) {
      const MirPlace *source =
          facts.body.findPlace(instruction.operands.front().place);
      if (source != nullptr && source->root == MirPlaceRootKind::Symbol) {
        output << "__gti_mir_v_" << *instruction.result << " = ";
        emitPlaceExpression(facts, *source);
        output << ";\n";
        return;
      }
      if (source != nullptr &&
          (source->root == MirPlaceRootKind::Loan ||
           (source->root == MirPlaceRootKind::This &&
            source->projections.size() == 2) ||
           (source->root == MirPlaceRootKind::Binding &&
            !source->projections.empty() &&
            source->projections[0].kind == MirProjectionKind::Dereference))) {
        output << "__gti_mir_v_" << *instruction.result << " = ";
        emitPlaceExpression(facts, *source);
        output << ";\n";
        return;
      }
      if (source != nullptr) {
        if (const std::optional<ClassSubscriptAccess> access =
                classSubscriptAccess(program, facts.body, *source)) {
          const MirFunctionInstance *member = containedSubscriptMember(
              program, representations, access->owner,
              ReceiverMutability::ReadOnly, access->indexType);
          if (member == nullptr) {
            throw std::logic_error(
                "verified MIR subscript read lost its contained member");
          }
          // The compatibility subscription: a read-only receiver wrapper
          // around the base, the member's emitted name, and the index.
          output << "__gti_mir_v_" << *instruction.result
                 << " = (::gti_internal::backend::read_only_receiver(";
          emitStoragePlaceValue(facts, *facts.body.findPlace(access->base));
          output << "))." << bodySpelling(member->id) << '(';
          emitSubscriptIndex(*access);
          output << ");\n";
          return;
        }
        if (const std::optional<ArrayElementAccess> access =
                viewElementAccess(facts.body, *source)) {
          // The terminal helper reports the defined bound contract and
          // never returns on failure, on both text forms.
          output << "__gti_mir_v_" << *instruction.result
                 << " = ::gti_internal::backend::string_view_at(__gti_mir_p_"
                 << access->array << ", ";
          emitElementIndexValue(*access);
          output << ");\n";
          return;
        }
        if (const std::optional<ArrayElementAccess> access =
                arrayElementAccess(facts.body, *source)) {
          if (failureForm && !instruction.localFailureSites.empty()) {
            output << "__gti_mir_failure_status_" << instruction.id
                   << " = ::gti_internal::backend::mir_checked_array_read_v1("
                      "__gti_mir_p_"
                   << access->array << ", ";
            emitElementIndexValue(*access);
            output << ", &__gti_mir_v_" << *instruction.result << ");\n";
            return;
          }
          output << "__gti_mir_v_" << *instruction.result << " = __gti_mir_p_"
                 << access->array << '[';
          emitElementIndex(*access);
          output << "];\n";
          return;
        }
      }
      output << "__gti_mir_v_" << *instruction.result << " = __gti_mir_p_"
             << instruction.operands.front().place << ";\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Initialize &&
        instruction.destination) {
      const MirPlace *slot = facts.body.findPlace(*instruction.destination);
      if (slot != nullptr && slotPlace(*slot)) {
        if (instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Value &&
            (instruction.operands.front().type.kind == SemanticType::Storage ||
             instruction.operands.front().type.kind ==
                 SemanticType::PrefixStorage)) {
          // A storage value constructs its lifetime slot by move.
          output << "__gti_mir_p_" << *instruction.destination
                 << ".construct(std::move(__gti_mir_v_"
                 << instruction.operands.front().value << "));\n";
          return;
        }
        if (instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Value &&
            instruction.operands.front().type.kind == SemanticType::Class) {
          // A class value that lives in a declared local (a transformed
          // callee's published result) engages the slot here by move; the
          // reparent comment is exact only when the paired construct
          // built the value inside this slot already — a destination on
          // the construct itself, not merely a consuming initialize.
          const MirValue *record =
              facts.body.findValue(instruction.operands.front().value);
          const MirInstruction *definition =
              record == nullptr
                  ? nullptr
                  : findInstruction(facts.body, record->definition);
          const bool builtInSlot =
              definition != nullptr &&
              definition->kind == MirInstructionKind::Construct &&
              definition->destination &&
              slotConsumedConstruct(facts, *definition);
          if (definition != nullptr &&
              definition->kind == MirInstructionKind::Construct &&
              !definition->destination && !definition->receiver &&
              slotConsumedConstruct(facts, *definition)) {
            // The undeclarable class value publishes here instead: the
            // slot constructs in place from the construction's own
            // arguments, exactly like the compatibility direct
            // initialization.
            output << "__gti_mir_p_" << *instruction.destination
                   << ".construct(";
            for (std::size_t index = 0; index < definition->operands.size();
                 ++index) {
              if (index != 0) {
                output << ", ";
              }
              // The construction consumes its arguments: class-typed
              // operands move so deleted copy constructors cannot reject
              // the call.
              const bool consumed = definition->operands[index].type.kind ==
                                        SemanticType::Class ||
                                    definition->operands[index].type.kind ==
                                        SemanticType::UniqueOwner ||
                                    definition->operands[index].type.kind ==
                                        SemanticType::Storage ||
                                    definition->operands[index].type.kind ==
                                        SemanticType::PrefixStorage;
              if (consumed) {
                output << "std::move(";
              }
              emitOperand(definition->operands[index]);
              if (consumed) {
                output << ')';
              }
            }
            output << ");\n";
            return;
          }
          if (!builtInSlot) {
            output << "__gti_mir_p_" << *instruction.destination
                   << ".construct(std::move(__gti_mir_v_"
                   << instruction.operands.front().value << "));\n";
            return;
          }
        }
        output << "// GTI MIR reparent into p" << *instruction.destination
               << "\n";
        return;
      }
    }
    if (const MirPlace *destinationPlace =
            facts.body.findPlace(*instruction.destination)) {
      if (const std::optional<ClassSubscriptAccess> access =
              classSubscriptAccess(program, facts.body, *destinationPlace)) {
        const MirFunctionInstance *member = containedSubscriptMember(
            program, representations, access->owner,
            ReceiverMutability::Mutable, access->indexType);
        if (member == nullptr) {
          throw std::logic_error(
              "verified MIR subscript store lost its contained member");
        }
        output << '(';
        emitStoragePlaceValue(facts, *facts.body.findPlace(access->base));
        output << ")." << bodySpelling(member->id) << '(';
        emitSubscriptIndex(*access);
        output << ") = ";
        emitOperand(instruction.operands.front());
        output << ";\n";
        if (instruction.kind == MirInstructionKind::Assign) {
          writeIndent();
          output << "__gti_mir_v_" << *instruction.result << " = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        return;
      }
      if (const std::optional<ArrayElementAccess> access =
              arrayElementAccess(facts.body, *destinationPlace)) {
        if (failureForm && !instruction.localFailureSites.empty()) {
          output << "__gti_mir_failure_status_" << instruction.id
                 << " = ::gti_internal::backend::mir_checked_array_write_v1("
                    "__gti_mir_p_"
                 << access->array << ", ";
          emitElementIndexValue(*access);
          output << ", ";
          emitOperand(instruction.operands.front());
          output << ");\n";
        } else {
          output << "__gti_mir_p_" << access->array << '[';
          emitElementIndex(*access);
          output << "] = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        if (instruction.kind == MirInstructionKind::Assign) {
          writeIndent();
          output << "__gti_mir_v_" << *instruction.result << " = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        return;
      }
    }
    // Storage values and unique-owner values are move-only in the C++
    // representation; the store is their consuming use, so it spells as a
    // move.
    const bool movedOperand =
        instruction.operands.front().type.kind == SemanticType::Storage ||
        instruction.operands.front().type.kind == SemanticType::PrefixStorage ||
        instruction.operands.front().type.kind == SemanticType::UniqueOwner;
    output << destinationSpelling(facts, *instruction.destination) << " = ";
    if (movedOperand) {
      output << "std::move(";
    }
    emitOperand(instruction.operands.front(),
                instruction.kind == MirInstructionKind::Initialize);
    if (movedOperand) {
      output << ')';
    }
    output << ";\n";
    if (instruction.kind == MirInstructionKind::Assign && !movedOperand) {
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = "
             << destinationSpelling(facts, *instruction.destination) << ";\n";
    }
  }

  void emitInstruction(const MirInstruction &instruction,
                       const ScalarBodyFacts &facts) {
    // The fused closure chain never materializes: the Closure compute,
    // the Initialize into a lambda-typed local, and the loads that rejoin
    // the chain all spell as comments, and each consuming invocation
    // spells the full literal inline.
    if (instruction.kind == MirInstructionKind::Compute &&
        instruction.operation == MirOperation::Closure) {
      writeIndent();
      output << "// closure value "
             << (instruction.result ? *instruction.result : 0)
             << " spells at its consuming invocation\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Initialize &&
        instruction.destination) {
      if (const MirPlace *destination =
              facts.body.findPlace(*instruction.destination);
          destination != nullptr &&
          destination->type.kind == SemanticType::Lambda) {
        writeIndent();
        output << "// closure local " << destination->id
               << " joins the fused chain\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Load && instruction.result &&
        instruction.operands.size() == 1) {
      if (const MirPlace *source =
              facts.body.findPlace(instruction.operands.front().place);
          source != nullptr && source->type.kind == SemanticType::Lambda) {
        writeIndent();
        output << "// load " << *instruction.result
               << " rejoins the fused closure chain\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Load &&
        instruction.operands.size() == 1 &&
        instruction.operands.front().place != 0) {
      if (const MirPlace *place =
              facts.body.findPlace(instruction.operands.front().place);
          place != nullptr &&
          (place->type.kind == SemanticType::Storage ||
           place->type.kind == SemanticType::PrefixStorage)) {
        // The staged storage never materializes; the storage-intrinsic
        // call spells the place lvalue directly.
        writeIndent();
        output << "// load " << (instruction.result ? *instruction.result : 0)
               << " stages a storage place\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Move && instruction.result &&
        instruction.operands.size() == 1) {
      if (const MirPlace *source =
              facts.body.findPlace(instruction.operands.front().place);
          source != nullptr && source->type.kind == SemanticType::Lambda) {
        // The moved callable stages its place; the invocation spells
        // std::move over the place expression directly.
        writeIndent();
        output << "// move " << *instruction.result
               << " stages a callable place\n";
        return;
      }
    }
    if (failureForm && instruction.kind == MirInstructionKind::Move &&
        instruction.result &&
        returnMoveDefinition(facts.body, *instruction.result) == &instruction) {
      // The publication happens here, where MIR consumes the source —
      // before any later drop of the moved-from local — and the paired
      // Return then only reports success.
      writeIndent();
      output << "*__gti_mir_out_result = std::move(";
      emitStoragePlaceValue(
          facts, *facts.body.findPlace(instruction.operands.front().place));
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Move && instruction.result &&
        !facts.body.usesOf(*instruction.result).empty()) {
      const MirInstruction *user = findInstruction(
          facts.body,
          facts.body.usesOf(*instruction.result).front().instruction);
      if (user != nullptr && user->kind == MirInstructionKind::CallInput &&
          user->result &&
          copyStagedCallInput(facts.body, *user->result) == user) {
        // The moved source feeds a value-staged call input; the consuming
        // call spells std::move over the place expression directly.
        writeIndent();
        output << "// move " << *instruction.result
               << " stages into its call input\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Move) {
      // By-value element staging: the moved place feeds exactly the
      // staged element value.
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = std::move(";
      emitStoragePlaceValue(
          facts, *facts.body.findPlace(instruction.operands.front().place));
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.result &&
        returnConstructDefinition(facts.body, *instruction.result) ==
            &instruction) {
      writeIndent();
      output << "// construct " << *instruction.result
             << " publishes at its consuming return\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.result && !instruction.destination &&
        !instruction.receiver &&
        instruction.info.type.kind == SemanticType::Class &&
        slotConsumedConstruct(facts, instruction)) {
      // The undeclarable class value publishes at its consuming
      // initialize, which constructs the destination slot in place from
      // these arguments.
      writeIndent();
      output << "// construct " << *instruction.result
             << " publishes at its consuming initialize\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.result && !instruction.destination &&
        !instruction.receiver &&
        instruction.info.type.kind == SemanticType::Class &&
        !slotConsumedConstruct(facts, instruction)) {
      // A value-producing construction assigns the constructor call into
      // its declared class local; the row's boundary proof guaranteed the
      // declaration above. Slot-consumed constructs keep the slot
      // protocol's own spelling.
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = "
             << typeSpelling(instruction.info.type) << '(';
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (index != 0) {
          output << ", ";
        }
        // The construction consumes its arguments: class-typed operands
        // move so deleted copy constructors cannot reject the call.
        const bool consumed =
            instruction.operands[index].type.kind == SemanticType::Class ||
            instruction.operands[index].type.kind ==
                SemanticType::UniqueOwner ||
            instruction.operands[index].type.kind == SemanticType::Storage ||
            instruction.operands[index].type.kind ==
                SemanticType::PrefixStorage;
        if (consumed) {
          output << "std::move(";
        }
        emitOperand(instruction.operands[index]);
        if (consumed) {
          output << ')';
        }
      }
      output << ");\n";
      return;
    }
    if (instruction.kind != MirInstructionKind::CallInput &&
        instruction.kind != MirInstructionKind::Call) {
      emitPlainInstruction(instruction, facts);
      return;
    }
    writeIndent();
    if (instruction.kind == MirInstructionKind::CallInput) {
      if (instruction.operands.front().kind == MirOperandKind::BorrowRead ||
          instruction.operands.front().kind == MirOperandKind::BorrowWrite) {
        // The staged borrow never materializes; the call spells the place.
        output << "// call input " << *instruction.result
               << " stages a borrowed place\n";
        return;
      }
      if (instruction.result &&
          loanStagedCallInput(facts.body, *instruction.result) ==
              &instruction) {
        // The staged loan never materializes; the call spells the
        // dereferenced pointer carrier (ADR 018 §4).
        output << "// call input " << *instruction.result
               << " stages a loaned argument\n";
        return;
      }
      if (instruction.result &&
          copyStagedCallInput(facts.body, *instruction.result) ==
              &instruction) {
        // The staged copy never materializes; the call spells the source
        // place and C++ copies at the call boundary.
        output << "// call input " << *instruction.result
               << " stages a by-value argument copy\n";
        return;
      }
      output << "__gti_mir_v_" << *instruction.result << " = ";
      emitOperand(instruction.operands.front());
      output << ";\n";
      return;
    }
    if (dischargedStorageReadCall(instruction)) {
      // A call-result loan binds the element address here; otherwise the
      // element is published by the loan-producing Borrow and the call
      // site itself stages nothing.
      if (const MirLoan *loan =
              producedCallResultLoan(facts.body, instruction)) {
        const MirPlace *storage =
            storageStagedPlace(facts.body, instruction.operands.front());
        if (storage == nullptr) {
          throw std::logic_error(
              "verified MIR discharged read lost its staged storage place");
        }
        output << "__gti_mir_loan_" << loan->id
               << " = &::gti_internal::backend::"
               << (instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut
                       ? "prefix_storage_read_mut"
                       : "prefix_storage_read")
               << '(';
        emitStoragePlaceValue(facts, *storage);
        output << ", ";
        emitOperand(instruction.operands.back());
        output << ");\n";
        return;
      }
      output << "// discharged storage read " << instruction.id
             << " publishes through its loan\n";
      return;
    }
    if (callableValueInvocation(instruction)) {
      // The invocation spells the fused closure literal followed by its
      // plain argument list; the literal contains failure terminally, so
      // no status local or record write stages here.
      if (instruction.result) {
        output << "__gti_mir_v_" << *instruction.result << " = ";
      }
      if (const MirInstruction *stage =
              callableReceiverStage(facts.body, instruction.receiver->value)) {
        const MirPlace *place =
            facts.body.findPlace(stage->operands.front().place);
        if (place == nullptr) {
          throw std::logic_error(
              "verified MIR invocation lost its staged callable place");
        }
        const bool moves = stage->kind == MirInstructionKind::Move;
        if (moves) {
          output << "std::move(";
        }
        emitStoragePlaceValue(facts, *place);
        if (moves) {
          output << ')';
        }
      } else {
        emitClosureLiteral(facts, instruction.receiver->value);
      }
      output << '(';
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (index != 0) {
          output << ", ";
        }
        // A loaned argument dereferences its pointer carrier (ADR 018
        // §4); every other argument is a staged value.
        if (instruction.operands[index].kind == MirOperandKind::Loan) {
          output << "(*__gti_mir_loan_" << instruction.operands[index].loan
                 << ')';
        } else {
          emitOperand(instruction.operands[index]);
        }
      }
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic == IntrinsicKind::PrefixStorageLength) {
      const MirPlace *storage =
          storageStagedPlace(facts.body, instruction.operands.front());
      if (storage == nullptr) {
        throw std::logic_error(
            "verified MIR length read lost its staged storage place");
      }
      output << "__gti_mir_v_" << *instruction.result
             << " = ::gti_internal::backend::prefix_storage_length(";
      emitStoragePlaceValue(facts, *storage);
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        prefixStorageIntrinsic(instruction.intrinsic)) {
      if (instruction.intrinsic == IntrinsicKind::AllocatePrefixStorage) {
        output << "__gti_mir_failure_status_" << instruction.id << " = "
               << prefixStorageHelperSpelling(instruction.intrinsic) << '<'
               << typeSpelling(instruction.info.type.arguments.front()) << ">(";
        emitOperand(instruction.operands.front());
        output << ", &__gti_mir_v_" << *instruction.result << ");\n";
        return;
      }
      const MirPlace *storage =
          storageStagedPlace(facts.body, instruction.operands.front());
      if (storage == nullptr || instruction.localFailureSites.empty()) {
        throw std::logic_error(
            "verified MIR storage intrinsic lost its staged storage place");
      }
      output << "__gti_mir_failure_status_" << instruction.id << " = "
             << prefixStorageHelperSpelling(instruction.intrinsic) << '(';
      emitStoragePlaceValue(facts, *storage);
      for (std::size_t index = 1; index < instruction.operands.size();
           ++index) {
        output << ", ";
        if (const MirPlace *second =
                storageStagedPlace(facts.body, instruction.operands[index])) {
          emitStoragePlaceValue(facts, *second);
          continue;
        }
        emitOperand(instruction.operands[index]);
      }
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        (instruction.intrinsic == IntrinsicKind::IntegerCheckedAdd ||
         instruction.intrinsic == IntrinsicKind::IntegerCheckedSubtract ||
         instruction.intrinsic == IntrinsicKind::IntegerCheckedMultiply)) {
      if (!instruction.result || instruction.operands.size() != 2 ||
          instruction.info.type.arguments.size() != 2) {
        throw std::logic_error(
            "verified MIR checked-result intrinsic lost its exact shape");
      }
      output << "__gti_mir_v_" << *instruction.result << " = "
             << cppIntegerArithmeticIntrinsicSpelling(instruction.intrinsic)
             << '<' << typeSpelling(instruction.info.type.arguments[1]) << ">(";
      emitOperand(instruction.operands[0]);
      output << ", ";
      emitOperand(instruction.operands[1]);
      output << ");\n";
      return;
    }
    if (storageBoundsCheckCall(instruction)) {
      // The terminal logical-size check keeps the exact compatibility
      // helper: it reports the container's defined GTI-R0007 contract and
      // never returns on failure.
      output << "::gti_internal::backend::index_bounds_check(";
      emitOperand(instruction.operands[0]);
      output << ", ";
      emitOperand(instruction.operands[1]);
      output << ");\n";
      return;
    }
    if (failureForm && checkedConversionIntrinsicCall(instruction)) {
      output << "__gti_mir_failure_status_" << instruction.id
             << " = ::gti_internal::backend::mir_checked_convert_v1<"
             << typeSpelling(instruction.info.type) << ">(";
      emitOperand(instruction.operands.front());
      output << ", &__gti_mir_v_" << *instruction.result << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        (instruction.intrinsic ==
             IntrinsicKind::NumericTypeParameterConversion ||
         instruction.intrinsic == IntrinsicKind::NumericAliasConversion)) {
      if (!instruction.result || instruction.operands.size() != 1) {
        throw std::logic_error(
            "verified MIR numeric-conversion intrinsic lost its operand");
      }
      output << "__gti_mir_v_" << *instruction.result
             << " = ::gti_internal::backend::numeric_cast<"
             << typeSpelling(instruction.info.type) << ">(";
      emitOperand(instruction.operands.front());
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call && instruction.receiver &&
        (instruction.intrinsic == IntrinsicKind::StringViewSize ||
         instruction.intrinsic == IntrinsicKind::StringViewEmpty ||
         instruction.intrinsic == IntrinsicKind::ArraySize ||
         instruction.intrinsic == IntrinsicKind::ExpectedValue ||
         instruction.intrinsic == IntrinsicKind::ExpectedError)) {
      // A builtin member read spells the staged place's member directly,
      // exactly like the compatibility route's generic member spelling;
      // the expected extractions contain their wrong-state failure inside
      // the spelled member itself.
      const MirInstruction *staged =
          borrowStagedCallInput(facts.body, *instruction.receiver);
      const MirOperand &receiverBorrow =
          staged != nullptr ? staged->operands.front() : *instruction.receiver;
      const MirPlace *viewPlace =
          receiverBorrow.kind == MirOperandKind::BorrowRead &&
                  receiverBorrow.place != 0
              ? facts.body.findPlace(receiverBorrow.place)
              : nullptr;
      if (viewPlace == nullptr || !instruction.result) {
        throw std::logic_error(
            "verified MIR builtin member read lost its staged place");
      }
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = ";
      emitPlaceExpression(facts, *viewPlace);
      switch (instruction.intrinsic) {
      case IntrinsicKind::StringViewSize:
      case IntrinsicKind::ArraySize:
        output << ".size();\n";
        break;
      case IntrinsicKind::StringViewEmpty:
        output << ".empty();\n";
        break;
      case IntrinsicKind::ExpectedValue:
        output << ".value();\n";
        break;
      default:
        output << ".error();\n";
        break;
      }
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic != IntrinsicKind::None) {
      const std::string_view helper =
          cppIntegerArithmeticIntrinsicSpelling(instruction.intrinsic);
      if (!scalarSpellableArithmeticIntrinsic(instruction.intrinsic) ||
          helper.empty() || instruction.receiver || !instruction.result ||
          instruction.operands.size() != 2) {
        throw std::logic_error(
            "verified MIR intrinsic call is outside the spellable "
            "arithmetic helper family");
      }
      output << "__gti_mir_v_" << *instruction.result << " = " << helper << '(';
      emitOperand(instruction.operands[0]);
      output << ", ";
      emitOperand(instruction.operands[1]);
      output << ");\n";
      return;
    }
    if (!instruction.functionTarget) {
      throw std::logic_error(
          "verified MIR direct call lost its exact target declaration");
    }
    // A receiver-carrying call spells its staged borrowed place followed by
    // the qualified member name: the explicit qualification states the
    // static dispatch MIR proved.
    const MirPlace *receiverPlace = nullptr;
    bool receiverMoved = false;
    if (instruction.receiver) {
      // Staged through a CallInput, or borrowed directly on the receiver
      // operand (a self-member call); both name the spellable place.
      const MirInstruction *staged =
          borrowStagedCallInput(facts.body, *instruction.receiver);
      const MirOperand &receiverBorrow =
          staged != nullptr ? staged->operands.front() : *instruction.receiver;
      const MirInstruction *movedStage =
          staged == nullptr &&
                  instruction.receiver->kind == MirOperandKind::Value
              ? copyStagedCallInput(facts.body, instruction.receiver->value)
              : nullptr;
      if (movedStage != nullptr) {
        const StagedTemporarySource source =
            stagedTemporarySourceFor(facts.body, *movedStage);
        receiverPlace = source.moved ? source.place : nullptr;
        receiverMoved = receiverPlace != nullptr;
      }
      if (receiverPlace == nullptr) {
        receiverPlace = (receiverBorrow.kind == MirOperandKind::BorrowRead ||
                         receiverBorrow.kind == MirOperandKind::BorrowWrite) &&
                                receiverBorrow.place != 0
                            ? facts.body.findPlace(receiverBorrow.place)
                            : nullptr;
      }
      if (receiverPlace == nullptr) {
        throw std::logic_error(
            "verified MIR receiver call lost its staged borrowed place");
      }
    }
    const auto emitCallArgument = [&](const MirOperand &operand,
                                      bool marshalled) {
      if (operand.kind == MirOperandKind::Value &&
          operand.type.kind == SemanticType::Lambda &&
          closureChainDefinition(facts.body, operand.value) != nullptr) {
        emitClosureLiteral(facts, operand.value);
        return;
      }
      if (operand.kind == MirOperandKind::Value) {
        if (const MirInstruction *stage =
                callableArgumentStage(facts.body, operand.value)) {
          // The staged callable argument passes the parameter place by
          // value, exactly like the compatibility call.
          const MirPlace *place =
              facts.body.findPlace(stage->operands.front().place);
          if (place == nullptr) {
            throw std::logic_error(
                "verified MIR call lost its staged callable argument place");
          }
          emitStoragePlaceValue(facts, *place);
          return;
        }
        if (const MirInstruction *stage =
                loanStagedCallInput(facts.body, operand.value)) {
          // The loan-staged argument dereferences its pointer carrier
          // (ADR 018 §4), exactly like a directly loaned operand.
          output << "(*__gti_mir_loan_" << stage->operands.front().loan << ')';
          return;
        }
        if (const MirInstruction *stage =
                copyStagedCallInput(facts.body, operand.value)) {
          // The value-staged argument spells its source place — moved
          // sources under std::move — and C++ materializes the argument
          // at the call boundary, exactly like the compatibility call.
          const StagedTemporarySource source =
              stagedTemporarySourceFor(facts.body, *stage);
          if (source.place == nullptr) {
            throw std::logic_error(
                "verified MIR call lost its value-staged source place");
          }
          if (source.moved) {
            output << "std::move(";
          }
          emitStoragePlaceValue(facts, *source.place);
          if (source.moved) {
            output << ')';
          }
          return;
        }
      }
      if (const MirInstruction *staged =
              borrowStagedCallInput(facts.body, operand)) {
        const MirPlace *place =
            facts.body.findPlace(staged->operands.front().place);
        if (place == nullptr) {
          throw std::logic_error(
              "verified MIR call lost a staged borrowed argument place");
        }
        emitPlaceExpression(facts, *place);
        return;
      }
      if (marshalled) {
        output << "::gti_internal::backend::to_c_string_view(";
      }
      emitOperand(operand);
      if (marshalled) {
        output << ')';
      }
    };
    if (failureForm && transformedCallee(instruction) != nullptr) {
      // The callee's transformed body carries the derived name and writes
      // the caller's record on failure; the paired Invoke branches on the
      // success bool.
      output << "__gti_mir_call_success_" << instruction.id << " = ";
      if (receiverPlace != nullptr) {
        if (receiverMoved) {
          output << "std::move(";
        }
        emitStoragePlaceValue(facts, *receiverPlace);
        if (receiverMoved) {
          output << ')';
        }
        output << '.';
      }
      const std::string sibling = cppMirFailureSiblingSpelling(
          bodySpelling(*instruction.functionTarget));
      if (sibling.empty()) {
        throw std::logic_error(
            "verified MIR failure call lost its transformed sibling name");
      }
      output << sibling << '(';
      for (const MirOperand &operand : instruction.operands) {
        emitCallArgument(operand, false);
        output << ", ";
      }
      if (transformedCallee(instruction)->returnType == SemanticType::Void) {
        output << "__gti_mir_failure_record);\n";
      } else if (transformedCallee(instruction)->returnType.kind ==
                 SemanticType::Reference) {
        const MirLoan *paired = producedCallResultLoan(facts.body, instruction);
        if (paired != nullptr) {
          output << "&__gti_mir_loan_" << paired->id;
        } else {
          output << "&__gti_mir_discard_" << instruction.id;
        }
        output << ", __gti_mir_failure_record);\n";
      } else {
        if (instruction.result) {
          output << "&__gti_mir_v_" << *instruction.result;
        } else {
          output << "&__gti_mir_discard_" << instruction.id;
        }
        output << ", __gti_mir_failure_record);\n";
      }
      // Without a paired Invoke the propagation is transparent: the
      // callee already wrote the forwarded record, so the caller simply
      // publishes failure.
      bool invokePaired = false;
      for (const MirBlock &block : facts.body.blocks) {
        for (const MirInstruction &candidate : block.instructions) {
          if (candidate.id == instruction.id) {
            invokePaired = instructionHasInvoke(block, instruction);
          }
        }
      }
      if (!invokePaired) {
        writeIndent();
        output << "if (!__gti_mir_call_success_" << instruction.id << ") {"
               << '\n';
        ++indentation;
        writeIndent();
        output << "return false;\n";
        --indentation;
        writeIndent();
        output << '}' << '\n';
      }
      return;
    }
    if (instruction.result) {
      if (!failureForm &&
          returnCallDefinition(facts.body, *instruction.result) ==
              &instruction) {
        // The class result publishes at its consuming return: the call
        // spells the return expression and the value never declares.
        output << "return ";
      } else {
        output << "__gti_mir_v_" << *instruction.result << " = ";
      }
    }
    if (receiverPlace != nullptr) {
      if (receiverMoved) {
        output << "std::move(";
      }
      emitStoragePlaceValue(facts, *receiverPlace);
      if (receiverMoved) {
        output << ')';
      }
      output << '.';
    }
    output << bodySpelling(*instruction.functionTarget);
    output << '(';
    const MirFunctionInstance *target =
        program.findFunctionInstance(*instruction.functionTarget);
    const bool cBoundary =
        target != nullptr && target->linkage == LanguageLinkage::C;
    for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      // The C prototype takes ::gti_c_string_view; the shipped converter
      // marshals the value view exactly as compatibility call sites do.
      emitCallArgument(instruction.operands[index],
                       cBoundary && instruction.operands[index].type.kind ==
                                        SemanticType::StringView);
    }
    output << ");\n";
  }

  void emitSwitchInteger(const EnumConstant &value, const SemanticType &type) {
    if (!value.negative) {
      output << value.magnitude;
      if (type == SemanticType::UInt64 &&
          value.magnitude > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max())) {
        output << "ULL";
      }
      return;
    }

    std::uint64_t signedLimit = 0;
    switch (type.kind) {
    case SemanticType::Int8:
      signedLimit = std::uint64_t{1} << 7U;
      break;
    case SemanticType::Int16:
      signedLimit = std::uint64_t{1} << 15U;
      break;
    case SemanticType::Int32:
      signedLimit = std::uint64_t{1} << 31U;
      break;
    case SemanticType::Int64:
      signedLimit = std::uint64_t{1} << 63U;
      break;
    default:
      break;
    }
    if (signedLimit != 0 && value.magnitude == signedLimit) {
      output << "(-" << signedLimit - 1 << "LL - 1LL)";
      return;
    }
    output << '-' << value.magnitude;
  }

  void emitTerminator(const MirTerminator &terminator,
                      const ScalarBodyFacts &facts) {
    switch (terminator.kind) {
    case MirTerminatorKind::Invoke: {
      const MirInstruction *producer =
          findInstruction(facts.body, terminator.invokeInstruction);
      if (producer != nullptr && (callableValueInvocation(*producer) ||
                                  deducedCallableCallee(program, *producer) ||
                                  terminallyContainedPlainCallee(
                                      program, representations, *producer) ||
                                  storageBoundsCheckCall(*producer))) {
        // The fused literal or template callee contains its failure
        // terminally; the edge is a plain goto and the else block never
        // runs.
        writeIndent();
        output << "__gti_mir_bb = " << terminator.target << ";\n";
        writeIndent();
        output << "continue;\n";
        return;
      }
      if (!failureForm) {
        // Plain shape: checked arithmetic spells its terminal helper, a
        // template body's may-raise call reaches a terminally-contained
        // convention, and a propagating construction terminates at its own
        // site; none ever returns on failure, so the edge is a plain goto.
        // The probe admits exactly these producers.
        if ((producer != nullptr &&
             producer->kind == MirInstructionKind::Compute &&
             !cppMirTerminalCheckedHelperSpelling(producer->operation)
                  .empty() &&
             !producer->localFailureSites.empty()) ||
            (producer != nullptr &&
             producer->kind == MirInstructionKind::Call) ||
            (producer != nullptr &&
             producer->kind == MirInstructionKind::Construct &&
             producer->localFailureSites.empty() &&
             producer->definedFailure.propagation ==
                 FailurePropagationKind::Constructor)) {
          writeIndent();
          output << "__gti_mir_bb = " << terminator.target << ";\n";
          writeIndent();
          output << "continue;\n";
          return;
        }
        throw std::logic_error(
            "verified MIR invoke is outside the failure vocabulary");
      }
      // The vacuous-else producers the probe admits: a discharged
      // storage read (flow proved the bound) and a propagating
      // construction (constructor failure terminates at its own site);
      // their edges are plain gotos.
      if (producer != nullptr &&
          ((producer->kind == MirInstructionKind::Call &&
            prefixStorageIntrinsic(producer->intrinsic) &&
            producer->localFailureSites.empty() &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::None) ||
           (producer->kind == MirInstructionKind::Construct &&
            producer->localFailureSites.empty() &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::Constructor) ||
           // The value-level view element read spells the terminal
           // string_view_at helper; the else edge is dead.
           (producer->kind == MirInstructionKind::Compute &&
            producer->operation == MirOperation::Index) ||
           // The expected extraction's spelled member contains the
           // wrong-state failure terminally; the else edge is dead.
           (producer->kind == MirInstructionKind::Call &&
            (producer->intrinsic == IntrinsicKind::ExpectedValue ||
             producer->intrinsic == IntrinsicKind::ExpectedError)))) {
        writeIndent();
        output << "__gti_mir_bb = " << terminator.target << ";\n";
        writeIndent();
        output << "continue;\n";
        return;
      }
      if (producer != nullptr && producer->kind == MirInstructionKind::Call &&
          transformedCallee(*producer) != nullptr) {
        writeIndent();
        output << "__gti_mir_bb = __gti_mir_call_success_" << producer->id
               << " ? " << terminator.target << " : " << terminator.elseTarget
               << ";\n";
        writeIndent();
        output << "continue;\n";
        return;
      }
      const bool elementDetector =
          producer != nullptr &&
          (producer->kind == MirInstructionKind::Load ||
           producer->kind == MirInstructionKind::Assign) &&
          producer->localFailureSites.size() == 1;
      const bool storageDetector =
          producer != nullptr && producer->kind == MirInstructionKind::Call &&
          ((prefixStorageIntrinsic(producer->intrinsic) &&
            !producer->localFailureSites.empty()) ||
           checkedConversionIntrinsicCall(*producer));
      if (producer == nullptr ||
          (!elementDetector && !storageDetector &&
           (producer->kind != MirInstructionKind::Compute ||
            cppMirCheckedOperationHelperSpelling(producer->operation)
                .empty()))) {
        throw std::logic_error(
            "verified MIR invoke is outside the failure vocabulary");
      }
      writeIndent();
      output << "if (__gti_mir_failure_status_" << producer->id
             << ".code == GTI_FAILURE_CODE_NONE_V1) {\n";
      ++indentation;
      writeIndent();
      output << "__gti_mir_bb = " << terminator.target << ";\n";
      --indentation;
      writeIndent();
      output << "} else {\n";
      ++indentation;
      emitFailureRecordWrite(*producer);
      writeIndent();
      output << "__gti_mir_bb = " << terminator.elseTarget << ";\n";
      --indentation;
      writeIndent();
      output << "}\n";
      writeIndent();
      output << "continue;\n";
      return;
    }
    case MirTerminatorKind::PropagateFailure:
      if (!failureForm) {
        // Only an inline literal's plain shape admits this terminator:
        // every failure source is contained terminally inside its helper,
        // so the block is unreachable.
        writeIndent();
        output << "std::abort();\n";
        return;
      }
      writeIndent();
      output << "// GTI MIR propagate failure-record "
             << terminator.failureRecord << " after cleanup\n";
      writeIndent();
      output << "return false;\n";
      return;
    default:
      break;
    }
    switch (terminator.kind) {
    case MirTerminatorKind::Goto:
      writeIndent();
      output << "__gti_mir_bb = " << terminator.target << ";\n";
      writeIndent();
      output << "continue;\n";
      return;
    case MirTerminatorKind::Branch:
      writeIndent();
      output << "__gti_mir_bb = ";
      emitOperand(*terminator.value);
      output << " ? " << terminator.target << " : " << terminator.elseTarget
             << ";\n";
      writeIndent();
      output << "continue;\n";
      return;
    case MirTerminatorKind::Switch:
      writeIndent();
      output << "switch (";
      emitOperand(*terminator.value);
      output << ") {\n";
      ++indentation;
      for (const MirSwitchTarget &target : terminator.switchTargets) {
        writeIndent();
        output << "case static_cast<" << typeSpelling(target.value->type)
               << ">(";
        emitSwitchInteger(target.value->value, target.value->type);
        output << "):\n";
        ++indentation;
        writeIndent();
        output << "__gti_mir_bb = " << target.target << ";\n";
        writeIndent();
        output << "break;\n";
        --indentation;
      }
      writeIndent();
      output << "default:\n";
      ++indentation;
      writeIndent();
      output << "__gti_mir_bb = " << terminator.target << ";\n";
      writeIndent();
      output << "break;\n";
      --indentation;
      --indentation;
      writeIndent();
      output << "}\n";
      writeIndent();
      output << "continue;\n";
      return;
    case MirTerminatorKind::Return:
      if (terminator.returnLoan && *terminator.returnLoan != 0) {
        if (failureForm) {
          // The loan pointer publishes through the `T **` out-parameter
          // (ADR 018 §5); the wrapper dereferences on the boundary.
          writeIndent();
          output << "*__gti_mir_out_result = __gti_mir_loan_"
                 << *terminator.returnLoan << ";\n";
          writeIndent();
          output << "return true;\n";
          return;
        }
        writeIndent();
        output << "return *__gti_mir_loan_" << *terminator.returnLoan << ";\n";
        return;
      }
      if (failureForm) {
        writeIndent();
        output << "// GTI MIR return publication\n";
        if (terminator.value) {
          const MirInstruction *construct =
              terminator.value->kind == MirOperandKind::Value
                  ? returnConstructDefinition(facts.body,
                                              terminator.value->value)
                  : nullptr;
          const MirInstruction *moved =
              construct == nullptr &&
                      terminator.value->kind == MirOperandKind::Value
                  ? returnMoveDefinition(facts.body, terminator.value->value)
                  : nullptr;
          if (moved != nullptr) {
            // Already published at its Move; nothing to assign here.
            writeIndent();
            output << "// GTI MIR moved value published at its move\n";
          } else {
            writeIndent();
            output << "*__gti_mir_out_result = ";
            if (construct != nullptr) {
              // The class value publishes its constructor call inline;
              // the move-assignment into the out-parameter matches the
              // compatibility return's observable behavior exactly.
              output << typeSpelling(construct->info.type) << '(';
              for (std::size_t index = 0; index < construct->operands.size();
                   ++index) {
                if (index != 0) {
                  output << ", ";
                }
                // A borrow-staged argument never materialized a local;
                // the constructor call spells its place directly, exactly
                // like a call argument.
                if (const MirInstruction *staged = borrowStagedCallInput(
                        facts.body, construct->operands[index])) {
                  const MirPlace *place =
                      facts.body.findPlace(staged->operands.front().place);
                  if (place == nullptr) {
                    throw std::logic_error(
                        "verified MIR publication construct lost a staged "
                        "borrowed argument place");
                  }
                  emitPlaceExpression(facts, *place);
                } else {
                  emitOperand(construct->operands[index]);
                }
              }
              output << ')';
            } else {
              emitOperand(*terminator.value);
            }
            output << ";\n";
          }
        }
        writeIndent();
        output << "return true;\n";
        return;
      }
      if (terminator.value && terminator.value->kind == MirOperandKind::Value &&
          returnCallDefinition(facts.body, terminator.value->value) !=
              nullptr) {
        // The class result already published at its defining call.
        writeIndent();
        output << "// GTI MIR class result returned at its call\n";
        return;
      }
      writeIndent();
      output << "return";
      if (terminator.value) {
        output << ' ';
        const MirInstruction *unexpectedValue =
            terminator.value->kind == MirOperandKind::Value
                ? unexpectedDefinition(facts.body, terminator.value->value)
                : nullptr;
        if (unexpectedValue != nullptr) {
          // The unexpected wrapper converts into the expected-typed
          // result here; the construction call is copied from the
          // Expected capability row for the emitted standard.
          output << expectedConstructionSpelling() << '(';
          emitOperand(unexpectedValue->operands.front());
          output << ')';
        } else {
          emitOperand(*terminator.value);
        }
      }
      output << ";\n";
      return;
    case MirTerminatorKind::Unreachable:
      writeIndent();
      output << "std::abort();\n";
      return;
    default:
      break;
    }
    throw std::logic_error("verified MIR scalar-CFG terminator is unsupported");
  }

  // Spells an lvalue expression for a place the loan machinery touches:
  // ordinary bindings, storage globals, receiver fields, receiver field
  // elements, sibling-array elements, and loan carriers (ADR 018).
  // Spells the VALUE held at a place: a lifetime slot exposes it through
  // its checked accessor, every other place is its own lvalue.
  void emitStoragePlaceValue(const ScalarBodyFacts &facts,
                             const MirPlace &place) {
    emitPlaceExpression(facts, place);
    if (slotPlace(place)) {
      output << ".get()";
    }
  }

  void emitPlaceExpression(const ScalarBodyFacts &facts,
                           const MirPlace &place) {
    if (place.root == MirPlaceRootKind::Loan) {
      output << "(*__gti_mir_loan_" << place.loan << ')';
      for (const MirPlaceProjection &projection : place.projections) {
        if (projection.kind == MirProjectionKind::Field) {
          output << '.' << fieldSpelling(facts, projection.field);
        } else {
          throw std::logic_error(
              "loan carrier projection is outside the vocabulary");
        }
      }
      return;
    }
    if (place.root == MirPlaceRootKind::Symbol) {
      if (place.capture != 0 &&
          facts.instanceLabel == std::string_view("lambda-instance")) {
        output << captureSpelling(facts.id, place.symbol, place.capture);
      } else {
        output << storageSpelling(place.symbol);
      }
      return;
    }
    if (place.root == MirPlaceRootKind::This) {
      if (place.projections.empty()) {
        output << "(*this)";
        return;
      }
      if (place.projections.size() == 1 &&
          place.projections[0].kind == MirProjectionKind::Field) {
        output << "__gti_mir_p_" << place.id;
        return;
      }
      if (place.projections.size() == 2 &&
          place.projections[0].kind == MirProjectionKind::Field &&
          place.projections[1].kind == MirProjectionKind::Dereference) {
        // A reference member dereferences implicitly; the bound reference
        // local already names the referent.
        output << "__gti_mir_p_" << place.id;
        return;
      }
      if (place.projections.size() == 2 &&
          place.projections[0].kind == MirProjectionKind::Field &&
          place.projections[1].kind == MirProjectionKind::Index) {
        output << "(*this)." << fieldSpelling(facts, place.projections[0].field)
               << '[';
        if (place.projections[1].constantIndex) {
          output << "static_cast<std::size_t>("
                 << *place.projections[1].constantIndex << ")";
        } else {
          output << "static_cast<std::size_t>(__gti_mir_v_"
                 << place.projections[1].index << ')';
        }
        output << ']';
        return;
      }
    }
    if (const std::optional<ArrayElementAccess> access =
            arrayElementAccess(facts.body, place)) {
      output << "__gti_mir_p_" << access->array << '[';
      emitElementIndex(*access);
      output << ']';
      return;
    }
    if (place.root == MirPlaceRootKind::Binding && !place.projections.empty() &&
        place.projections[0].kind == MirProjectionKind::Dereference) {
      const MirPlace *base = nullptr;
      for (const MirPlace &candidate : facts.body.places) {
        if (candidate.id != place.id &&
            candidate.root == MirPlaceRootKind::Binding &&
            candidate.binding == place.binding &&
            candidate.projections.empty()) {
          base = &candidate;
        }
      }
      if (base == nullptr) {
        throw std::logic_error("dereference projection lost its base carrier");
      }
      output << "(*__gti_mir_p_" << base->id << ')';
      for (std::size_t index = 1; index < place.projections.size(); ++index) {
        if (place.projections[index].kind != MirProjectionKind::Field) {
          throw std::logic_error(
              "dereference chain projection is outside the vocabulary");
        }
        output << '.' << fieldSpelling(facts, place.projections[index].field);
      }
      return;
    }
    if (place.projections.empty()) {
      output << "__gti_mir_p_"
             << (slotPlace(place) ? canonicalSlotPlaceId(facts.body, place)
                                  : place.id);
      return;
    }
    throw std::logic_error("place expression is outside the loan vocabulary");
  }

  void emitElementIndex(const ArrayElementAccess &access) {
    if (access.constantIndex) {
      output << "static_cast<std::size_t>(" << *access.constantIndex << ")";
      return;
    }
    output << "static_cast<std::size_t>(__gti_mir_v_" << access.index << ")";
  }

  void emitSubscriptIndex(const ClassSubscriptAccess &access) {
    if (access.index != 0) {
      output << "__gti_mir_v_" << access.index;
      return;
    }
    output << *access.constantIndex;
  }

  void emitElementIndexValue(const ArrayElementAccess &access) {
    if (access.constantIndex) {
      output << "UINT64_C(" << *access.constantIndex << ")";
      return;
    }
    output << "__gti_mir_v_" << access.index;
  }

  [[nodiscard]] const MirFunctionInstance *
  transformedCallee(const MirInstruction &instruction) const {
    if (instruction.kind != MirInstructionKind::Call ||
        !instruction.functionTarget ||
        instruction.intrinsic != IntrinsicKind::None) {
      return nullptr;
    }
    const MirFunctionInstance *target =
        program.findFunctionInstance(*instruction.functionTarget);
    // A deduced-callable template callee keeps the compatibility plain
    // convention — its failure contains terminally inside the innermost
    // literal or helper — so it is never reached through the transformed
    // record ABI.
    return target != nullptr && target->mayRaiseDefinedFailure &&
                   target->callableParameters.empty() &&
                   target->linkage == LanguageLinkage::Gti &&
                   target->definitionKind ==
                       MirFunctionInstance::DefinitionKind::Source &&
                   !terminallyContainedPlainCallee(program, representations,
                                                   instruction)
               ? target
               : nullptr;
  }

  [[nodiscard]] static const MirInstruction *
  findInstruction(const MirBody &body, MirInstructionId id) {
    const MirInstruction *found = nullptr;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.id != id) {
          continue;
        }
        if (found != nullptr) {
          throw std::logic_error("verified MIR duplicated an instruction id");
        }
        found = &instruction;
      }
    }
    return found;
  }

  // The record's contents are wholly MIR-owned: the detector's exact site
  // plus the program's artifact identity (ADR 017).
  void emitFailureRecordWrite(const MirInstruction &instruction) {
    if (instruction.localFailureSites.size() != 1 ||
        instruction.definedFailure.localOrigins.size() != 1 ||
        program.failureMetadata().findSite(
            instruction.localFailureSites.front()) == nullptr) {
      throw std::logic_error(
          "failure-form MIR detector lost its canonical site");
    }
    writeIndent();
    output << "*__gti_mir_failure_record = ::gti_failure_record_v1{\n";
    ++indentation;
    writeIndent();
    output << "GTI_FAILURE_ABI_VERSION_V1, __gti_mir_failure_status_"
           << instruction.id << ".code, __gti_mir_failure_status_"
           << instruction.id << ".detail, UINT32_C("
           << instruction.localFailureSites.front() << "), UINT32_C(0), {";
    const auto &bytes = program.failureMetadata().artifactIdentity().bytes;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << static_cast<unsigned int>(bytes[index]);
    }
    output << "}};\n";
    --indentation;
  }

  const MirProgram &program;
  const CppMirBodyEmissionMap &representations;
  std::size_t indentation;
  bool failureForm = false;
  std::string_view currentFamilyLabel;
  std::ostringstream output;
};

} // namespace

std::optional<CppMirTypeRepresentationKind>
cppMirExpectedTypeRepresentation(const SemanticType &type) {
  return expectedTypeRepresentation(type);
}

std::string cppMirBinaryFloatLiteralSpelling(BinaryFloat value) {
  if (!validBinaryFloat(value)) {
    throw std::logic_error(
        "floating literal contains bits outside its declared format");
  }
  std::ostringstream text;
  const bool binary64 = value.format == BinaryFloatFormat::Binary64;
  text << (binary64 ? "std::bit_cast<double>(std::uint64_t{0x"
                    : "std::bit_cast<float>(std::uint32_t{0x")
       << std::hex << std::setw(binary64 ? 16 : 8) << std::setfill('0')
       << value.bits << (binary64 ? "ULL})" : "U})");
  return text.str();
}

std::string cppMirStringViewLiteralSpelling(std::string_view value) {
  std::string result = "std::string_view{\"";
  for (const char character : value) {
    switch (character) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    case '\0':
      result += "\\000";
      break;
    default: {
      const auto byte = static_cast<unsigned char>(character);
      if (byte < 32 || byte >= 127) {
        result += '\\';
        result += static_cast<char>('0' + ((byte >> 6U) & 0x07U));
        result += static_cast<char>('0' + ((byte >> 3U) & 0x07U));
        result += static_cast<char>('0' + (byte & 0x07U));
      } else {
        result += character;
      }
      break;
    }
    }
  }
  result += "\", ";
  result += std::to_string(value.size());
  result += '}';
  return result;
}

std::string_view cppMirCheckedOperationHelperSpelling(MirOperation operation) {
  switch (operation) {
  case MirOperation::Add:
    return "::gti_internal::backend::mir_checked_add_v1";
  case MirOperation::Subtract:
    return "::gti_internal::backend::mir_checked_subtract_v1";
  case MirOperation::Multiply:
    return "::gti_internal::backend::mir_checked_multiply_v1";
  case MirOperation::Divide:
    return "::gti_internal::backend::mir_checked_divide_v1";
  case MirOperation::Remainder:
    return "::gti_internal::backend::mir_checked_remainder_v1";
  case MirOperation::ShiftLeft:
    return "::gti_internal::backend::mir_checked_shift_left_v1";
  case MirOperation::ShiftRight:
    return "::gti_internal::backend::mir_checked_shift_right_v1";
  case MirOperation::Negate:
    return "::gti_internal::backend::mir_checked_negate_v1";
  case MirOperation::Convert:
    return "::gti_internal::backend::mir_checked_convert_v1";
  default:
    return {};
  }
}

std::string_view
cppIntegerArithmeticIntrinsicSpelling(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::IntegerWrappingAdd:
    return "::gti_internal::backend::wrapping_add";
  case IntrinsicKind::IntegerWrappingSubtract:
    return "::gti_internal::backend::wrapping_sub";
  case IntrinsicKind::IntegerWrappingMultiply:
    return "::gti_internal::backend::wrapping_mul";
  case IntrinsicKind::IntegerSaturatingAdd:
    return "::gti_internal::backend::saturating_add";
  case IntrinsicKind::IntegerSaturatingSubtract:
    return "::gti_internal::backend::saturating_sub";
  case IntrinsicKind::IntegerSaturatingMultiply:
    return "::gti_internal::backend::saturating_mul";
  case IntrinsicKind::IntegerCheckedAdd:
    return "::gti_internal::backend::checked_add";
  case IntrinsicKind::IntegerCheckedSubtract:
    return "::gti_internal::backend::checked_sub";
  case IntrinsicKind::IntegerCheckedMultiply:
    return "::gti_internal::backend::checked_mul";
  default:
    return {};
  }
}

bool CppMirBodyEmitter::supportsBodyText(MirBodyAddress address) const {
  return supportsBodyTextImpl(address, false);
}

bool CppMirBodyEmitter::supportsFailureBodyText(MirBodyAddress address) const {
  return supportsBodyTextImpl(address, true);
}

std::optional<std::vector<CppMirStoredReferenceBinding>>
cppMirStoredReferenceBindings(const MirConstructorInstance &constructor) {
  std::vector<CppMirStoredReferenceBinding> bindings;
  std::vector<bool> loanUsed(constructor.body.loans.size(), false);
  for (std::size_t index = 0; index < constructor.initializers.size();
       ++index) {
    const MirConstructorInitializer &initializer =
        constructor.initializers[index];
    if (!initializer.storesReference) {
      continue;
    }
    if (initializer.kind != ConstructorInitializerTargetKind::Field ||
        initializer.field == 0) {
      return std::nullopt;
    }
    const MirLoan *stored = nullptr;
    std::size_t storedIndex = 0;
    for (std::size_t loanIndex = 0; loanIndex < constructor.body.loans.size();
         ++loanIndex) {
      const MirLoan &loan = constructor.body.loans[loanIndex];
      if (loan.kind != MirLoanKind::Stored ||
          loan.storedField != initializer.field) {
        continue;
      }
      if (stored != nullptr) {
        return std::nullopt;
      }
      stored = &loan;
      storedIndex = loanIndex;
    }
    if (stored == nullptr || loanUsed[storedIndex]) {
      return std::nullopt;
    }
    loanUsed[storedIndex] = true;
    // The loan source must be the dereference carrier of one reference
    // parameter: the initializer list then binds the field straight to the
    // C++ reference parameter.
    const MirPlace *source = constructor.body.findPlace(stored->source);
    if (source == nullptr || source->root != MirPlaceRootKind::Binding ||
        source->projections.size() != 1 ||
        source->projections.front().kind != MirProjectionKind::Dereference) {
      return std::nullopt;
    }
    const auto parameter =
        std::find(constructor.parameterBindings.begin(),
                  constructor.parameterBindings.end(), source->binding);
    if (parameter == constructor.parameterBindings.end()) {
      return std::nullopt;
    }
    bindings.push_back(
        {.initializer = index,
         .field = initializer.field,
         .parameter = static_cast<std::size_t>(
             parameter - constructor.parameterBindings.begin())});
  }
  // Every Stored loan must be claimed by exactly one initializer; a stray
  // stored loan would silently drop its binding.
  for (std::size_t loanIndex = 0; loanIndex < constructor.body.loans.size();
       ++loanIndex) {
    if (constructor.body.loans[loanIndex].kind == MirLoanKind::Stored &&
        !loanUsed[loanIndex]) {
      return std::nullopt;
    }
  }
  return bindings;
}

bool cppMirHostedStartupNoArgumentsSchedule(const MirProgram &program) {
  const std::optional<MirHostedStartupPlan> &plan = program.hostedStartupPlan();
  if (!plan || plan->kind != ProgramEntryKind::NoArguments ||
      plan->entry == 0 ||
      plan->exitPolicy != MirHostedStartupExitPolicy::ImmediateExit70 ||
      plan->operations.size() != 4) {
    return false;
  }
  return plan->operations[0].kind == MirHostedStartupOperationKind::CallEntry &&
         plan->operations[1].kind ==
             MirHostedStartupOperationKind::RouteOperationFailure &&
         plan->operations[2].kind ==
             MirHostedStartupOperationKind::ContainFailure &&
         plan->operations[3].kind == MirHostedStartupOperationKind::ReturnEntry;
}

bool cppMirHostedStartupOwnedArgumentsSchedule(const MirProgram &program) {
  const std::optional<MirHostedStartupPlan> &plan = program.hostedStartupPlan();
  if (!plan || plan->kind != ProgramEntryKind::OwnedArguments ||
      plan->entry == 0 || plan->appendFunction == 0 ||
      plan->vectorConstructor == 0 || plan->stringConstructor == 0 ||
      plan->exitPolicy != MirHostedStartupExitPolicy::ImmediateExit70) {
    return false;
  }
  using Kind = MirHostedStartupOperationKind;
  using Behavior = MirHostedStartupFailureBehavior;
  struct ExpectedOperation {
    Kind kind;
    Behavior behavior;
    bool terminator;
    bool record;
    bool drop;
  };
  // The exact marshaling schedule the emitted argc/argv main performs:
  // detect-validated count and conversion, propagating vector
  // construction, then the per-argument loop — view read, string
  // construction and append under the drop/end failure-cleanup envelope —
  // and the entry call, each failure routed and terminally contained.
  static constexpr ExpectedOperation expected[] = {
      {Kind::ValidateArgumentCount, Behavior::Detect, false, false, false},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::ConvertArgumentCount, Behavior::Detect, false, false, false},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::ConstructArgumentVector, Behavior::Propagate, false, false, true},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::InitializeArgumentIndex, Behavior::None, false, false, false},
      {Kind::EnterArgumentLoop, Behavior::None, true, false, false},
      {Kind::LoadArgumentIndex, Behavior::None, false, false, false},
      {Kind::TestArgumentIndex, Behavior::None, false, false, false},
      {Kind::BranchArgumentLoop, Behavior::None, true, false, false},
      {Kind::ReadArgumentView, Behavior::None, false, false, false},
      {Kind::PrepareStringConstructorArgument, Behavior::None, false, false,
       false},
      {Kind::ConstructArgumentString, Behavior::Propagate, false, false, true},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::DropFailureCleanup, Behavior::None, false, false, false},
      {Kind::EndFailureCleanup, Behavior::None, false, false, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::PrepareAppendReceiver, Behavior::None, false, false, false},
      {Kind::PrepareAppendArgumentMove, Behavior::None, false, false, true},
      {Kind::CallAppend, Behavior::Propagate, false, false, false},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::DropFailureCleanup, Behavior::None, false, false, false},
      {Kind::EndFailureCleanup, Behavior::None, false, false, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::AdvanceArgumentIndex, Behavior::None, false, false, false},
      {Kind::ContinueArgumentLoop, Behavior::None, true, false, false},
      {Kind::PrepareEntryCount, Behavior::None, false, false, false},
      {Kind::PrepareEntryArgumentsMove, Behavior::None, false, false, true},
      {Kind::CallEntry, Behavior::Propagate, false, false, false},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::ReturnEntry, Behavior::None, true, false, false},
  };
  if (plan->operations.size() != std::size(expected)) {
    return false;
  }
  std::vector<MirFailureRecordId> routedRecords;
  for (std::size_t index = 0; index < plan->operations.size(); ++index) {
    const MirHostedStartupOperation &operation = plan->operations[index];
    const ExpectedOperation &shape = expected[index];
    if (operation.kind != shape.kind ||
        operation.failureBehavior != shape.behavior ||
        operation.terminator != shape.terminator ||
        (operation.failureRecord != 0) != shape.record ||
        (operation.dropObligation != 0) != shape.drop) {
      return false;
    }
    if (shape.record) {
      if (std::find(routedRecords.begin(), routedRecords.end(),
                    operation.failureRecord) != routedRecords.end()) {
        return false;
      }
      routedRecords.push_back(operation.failureRecord);
    }
  }
  return true;
}

std::string cppMirFailureSiblingSpelling(std::string_view memberSpelling) {
  constexpr std::string_view keyword = "operator";
  const std::size_t at = memberSpelling.find(keyword);
  const std::size_t after =
      at == std::string_view::npos ? 0 : at + keyword.size();
  const bool bridge =
      at != std::string_view::npos &&
      (after >= memberSpelling.size() ||
       (!std::isalnum(static_cast<unsigned char>(memberSpelling[after])) &&
        memberSpelling[after] != '_'));
  if (!bridge) {
    return std::string(memberSpelling) + "__gti_mir_failure";
  }
  const std::string_view symbol = memberSpelling.substr(after);
  static constexpr std::pair<std::string_view, std::string_view> tokens[] = {
      {"==", "eq"},     {"!=", "ne"},    {"<=", "le"},   {">=", "ge"},
      {"<<", "shl"},    {">>", "shr"},   {"<", "lt"},    {">", "gt"},
      {"+", "plus"},    {"-", "minus"},  {"*", "star"},  {"/", "slash"},
      {"%", "percent"}, {"[]", "index"}, {"()", "call"}, {"!", "not"},
      {"~", "tilde"},   {"&", "amp"},    {"|", "pipe"},  {"^", "caret"},
  };
  for (const auto &[spelling, token] : tokens) {
    if (symbol == spelling) {
      return std::string(memberSpelling.substr(0, at)) + "__gti_mir_op_" +
             std::string(token) + "__gti_mir_failure";
    }
  }
  return {};
}

bool CppMirBodyEmitter::boundaryDeclarationBody(MirBodyAddress address) const {
  if (address.kind != MirBodyKind::Function) {
    return false;
  }
  const MirFunctionInstance *function =
      program_.findFunctionInstance(address.owner);
  if (function == nullptr ||
      function->definitionKind == MirFunctionInstance::DefinitionKind::Source) {
    return false;
  }
  const MirBody &body = function->body;
  if (body.blocks.size() != 1 || !body.loans.empty() ||
      !body.dropObligations.empty() || !body.cleanupBoundaries.empty() ||
      !body.failureRecords.empty()) {
    return false;
  }
  const MirBlock &block = body.blocks.front();
  return block.reachable && block.instructions.empty() &&
         (block.terminator.kind == MirTerminatorKind::Return ||
          block.terminator.kind == MirTerminatorKind::Unreachable);
}

bool CppMirBodyEmitter::supportsBodyTextImpl(MirBodyAddress address,
                                             bool failureForm) const {
  // Probe-decline tracing (diagnostic only): with GTI_PROBE_TRACE set in
  // the environment, every decline point reports its stable ordinal and
  // the probed body, so decline censuses do not need rebuilt
  // instrumentation. The ordinals shift only when this function gains or
  // loses a decline point.
  // Nested probes (containment checks) run inside this one; the scope
  // guard restores the outer body's identity so every decline print
  // attributes to the body actually declining.
  const struct ProbeTraceScope {
    int kind;
    unsigned long owner;
    int form;
    ProbeTraceScope(int nextKind, unsigned long nextOwner, int nextForm)
        : kind(gtiProbeTraceKind), owner(gtiProbeTraceOwner),
          form(gtiProbeTraceForm) {
      gtiProbeTraceKind = nextKind;
      gtiProbeTraceOwner = nextOwner;
      gtiProbeTraceForm = nextForm;
    }
    ~ProbeTraceScope() {
      gtiProbeTraceKind = kind;
      gtiProbeTraceOwner = owner;
      gtiProbeTraceForm = form;
    }
  } gtiProbeTraceScope{static_cast<int>(address.kind),
                       static_cast<unsigned long>(address.owner),
                       failureForm ? 1 : 0};
  // The vocabulary is shared between function and destructor bodies; the
  // probe needs only the body, the owning class instance for field rows,
  // and the receiver mutability for the store direction. The failure form
  // (ADR 017) restricts to leaf function bodies under the transformed
  // private ABI and additionally admits checked detectors and failure
  // control flow.
  const MirBody *bodyPointer = nullptr;
  std::optional<HirClassInstanceId> owner;
  ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
  const std::vector<HirBindingId> *parameterBindings = nullptr;
  // A deduced-callable template body (plain shape, Function kind, callable
  // parameters): its concrete callable types are spellable only under a
  // template emission's overlay rows, and every failure convention it can
  // reach is terminally contained, exactly like the compatibility path.
  bool callableTemplateBody = false;
  // Set when the Constructor case proved the stored-reference pairing, so
  // the generic loan rule can admit the paired Stored loans.
  bool storedReferenceBindings = false;
  switch (address.kind) {
  case MirBodyKind::Function: {
    const MirFunctionInstance *function =
        program_.findFunctionInstance(address.owner);
    if (function == nullptr) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 1,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    if (failureForm) {
      // The transformed sibling's name comes from the shared naming
      // authority: plain and mangled member names carry the suffix
      // directly, a structural operator bridge derives its mangled token
      // sibling, and an operator outside the token map keeps the
      // compatibility route.
      if (function->overloadedOperator) {
        const MirBodyAddress self{.kind = MirBodyKind::Function,
                                  .owner = address.owner};
        const auto row = std::find_if(
            representations_.bodies().begin(), representations_.bodies().end(),
            [&](const CppMirBodyNameRepresentation &candidate) {
              return candidate.address == self;
            });
        if (row == representations_.bodies().end() ||
            cppMirFailureSiblingSpelling(row->spelling).empty()) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 2,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
      }
      // The transformed ABI publishes through a scalar out-parameter; a
      // body that cannot raise keeps its plain form instead.
      const std::optional<CppMirTypeRepresentationKind> returnKind =
          cppMirExpectedTypeRepresentation(function->returnType);
      if (!function->mayRaiseDefinedFailure || !returnKind ||
          (*returnKind != CppMirTypeRepresentationKind::Scalar &&
           *returnKind != CppMirTypeRepresentationKind::Void &&
           // A loan-returning body publishes through a `T **`
           // out-parameter (ADR 018 §5); its Return-with-loan rule and
           // the caller's paired loan own the rest of the proof.
           *returnKind != CppMirTypeRepresentationKind::Reference &&
           // An expected-typed result publishes by value through the
           // ordinary out-parameter; the scalar-payload demand keeps the
           // boundary default-constructible on every shipped standard.
           // A class-valued result publishes its constructor inline at
           // the Return; the row demand is inline because the shared row
           // helpers are defined below.
           !(*returnKind == CppMirTypeRepresentationKind::Class &&
             std::any_of(representations_.types().begin(),
                         representations_.types().end(),
                         [&](const CppMirTypeRepresentation &row) {
                           return row.type == function->returnType &&
                                  !row.spelling.empty();
                         })) &&
           !(*returnKind == CppMirTypeRepresentationKind::Expected &&
             function->returnType.arguments.size() == 2 &&
             cppMirExpectedTypeRepresentation(
                 function->returnType.arguments.front()) &&
             (*cppMirExpectedTypeRepresentation(
                  function->returnType.arguments.front()) ==
                  CppMirTypeRepresentationKind::Scalar ||
              *cppMirExpectedTypeRepresentation(
                  function->returnType.arguments.front()) ==
                  CppMirTypeRepresentationKind::Void)))) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 3,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
    }
    bodyPointer = &function->body;
    owner = function->owner;
    receiverMutability = function->receiverMutability;
    parameterBindings = &function->parameterBindings;
    // Both forms ride the overlay route: the plain template since 0.209,
    // and the transformed sibling template whose invocations keep the
    // same terminally-contained callable conventions.
    callableTemplateBody = !function->callableParameters.empty();
    break;
  }
  case MirBodyKind::Destructor: {
    if (failureForm) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 4,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    const MirDestructorInstance *destructor =
        program_.findDestructorInstance(address.owner);
    if (destructor == nullptr || destructor->owner == 0) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 5,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    bodyPointer = &destructor->body;
    owner = destructor->owner;
    receiverMutability = ReceiverMutability::Mutable;
    break;
  }
  case MirBodyKind::Constructor: {
    // The success form spells the verified initializer schedule inside the
    // constructor body; failure-capable construction stays with the
    // rollback machinery until its own slice.
    if (failureForm) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 6,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    const MirConstructorInstance *constructor =
        program_.findConstructorInstance(address.owner);
    if (constructor == nullptr || constructor->owner == 0 ||
        !constructor->body.failureRecords.empty() ||
        !std::all_of(constructor->initializers.begin(),
                     constructor->initializers.end(),
                     [](const MirConstructorInitializer &initializer) {
                       return initializer.kind ==
                              ConstructorInitializerTargetKind::Field;
                     })) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 7,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    // Stores-reference initializers bind their fields in the C++ member
    // initializer list from the paired Stored loans; the single pairing
    // authority declines anything outside that exact shape. A nonzero
    // constructor borrow origin is the caller-side lifetime fact of
    // exactly that schedule, so it is admissible only alongside it.
    {
      const std::optional<std::vector<CppMirStoredReferenceBinding>> bindings =
          cppMirStoredReferenceBindings(*constructor);
      if (!bindings || (constructor->borrowOrigin != BorrowOriginKind::None &&
                        bindings->empty())) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 8,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      storedReferenceBindings = !bindings->empty();
    }
    bodyPointer = &constructor->body;
    owner = constructor->owner;
    receiverMutability = ReceiverMutability::Mutable;
    parameterBindings = &constructor->parameterBindings;
    break;
  }
  case MirBodyKind::Lambda: {
    // The plain literal shape is fixed by its invocation sites and by
    // compatibility parity, so a lambda body admits only in the success
    // form; its checked arithmetic contains terminally instead.
    if (failureForm) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 9,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    const MirLambdaInstance *lambda = program_.findLambda(address.owner);
    if (lambda == nullptr) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 10,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    bodyPointer = &lambda->body;
    receiverMutability = ReceiverMutability::ReadOnly;
    parameterBindings = &lambda->parameterBindings;
    break;
  }
  default: {
    if (::getenv("GTI_PROBE_TRACE") != nullptr) {
      std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 11,
                   gtiProbeTraceKind, gtiProbeTraceOwner, gtiProbeTraceForm);
    }
    return false;
  }
  }
  const MirBody &body = *bodyPointer;
  if (body.blocks.empty() || body.entry == 0 ||
      body.entry > body.blocks.size()) {
    {
      if (::getenv("GTI_PROBE_TRACE") != nullptr) {
        std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 12,
                     gtiProbeTraceKind, gtiProbeTraceOwner, gtiProbeTraceForm);
      }
      return false;
    }
  }
  const auto loanById = [&](MirLoanId id) -> const MirLoan * {
    for (const MirLoan &loan : body.loans) {
      if (loan.id == id) {
        return &loan;
      }
    }
    return nullptr;
  };
  // Loan erasure (ADR 018): every loan needs a resolvable source place
  // whose type row spells the pointer local. A call-result loan binds the
  // exact element address its producing discharged storage read returns;
  // stored and parameter loans wait for their own slices.
  for (const MirLoan &loan : body.loans) {
    const MirPlace *loanSource = body.findPlace(loan.source);
    if (loanSource == nullptr) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 13,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    if (loan.kind == MirLoanKind::Local || loan.kind == MirLoanKind::Return) {
      continue;
    }
    if (loan.kind == MirLoanKind::Stored) {
      // A field-carrying stored loan is admitted only when the
      // Constructor case proved the bijective stores-reference pairing;
      // the binding spells in the member initializer list and no pointer
      // local exists. A field-less stored loan rides a borrow-carrying
      // object value: the object local holds the borrow internally, no
      // pointer ever binds, and its call-result children bind their own
      // pointers — admissible exactly when nothing roots a place at it.
      if (loan.storedField == 0 && !storedReferenceBindings) {
        bool rootedPlace = false;
        for (const MirPlace &place : body.places) {
          if (place.root == MirPlaceRootKind::Loan && place.loan == loan.id) {
            rootedPlace = true;
          }
        }
        if (rootedPlace) {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 133,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
        continue;
      }
      if (!storedReferenceBindings) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 14,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (loan.kind == MirLoanKind::Parameter) {
      // The entry loan aliases the reference parameter's pointer carrier
      // (ADR 018 §4): bound once in the prelude, dereferenced at use.
      // Admission requires exactly the carrier shape the reference-local
      // place vocabulary already spells.
      if (!loan.entry || loan.carriers.size() != 1 ||
          loanSource->root != MirPlaceRootKind::Binding ||
          !loanSource->projections.empty() ||
          loanSource->type.kind != SemanticType::Reference ||
          parameterBindings == nullptr ||
          std::find(parameterBindings->begin(), parameterBindings->end(),
                    loanSource->binding) == parameterBindings->end()) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 130,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (loan.kind != MirLoanKind::CallResult) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 15,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    const MirInstruction *discharged =
        pairedDischargedRead(body, loan.producedBy);
    const MirInstruction *referenceCall =
        discharged == nullptr ? loanProducingReferenceCall(program_, body, loan)
                              : nullptr;
    if (discharged != nullptr) {
      if (producedCallResultLoan(body, *discharged) == nullptr ||
          (loanSource->type.kind != SemanticType::Storage &&
           loanSource->type.kind != SemanticType::PrefixStorage) ||
          loanSource->type.arguments.empty()) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 16,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
    } else if (referenceCall != nullptr) {
      // The pointer local declares from the callee's return element row;
      // the receiver staging still spells the source place separately.
      const MirFunctionInstance *referenceTarget =
          referenceCall->functionTarget
              ? program_.findFunctionInstance(*referenceCall->functionTarget)
              : nullptr;
      const bool elementRow =
          referenceTarget != nullptr &&
          !referenceTarget->returnType.arguments.empty() &&
          std::any_of(
              representations_.types().begin(), representations_.types().end(),
              [&](const CppMirTypeRepresentation &row) {
                return row.type ==
                           referenceTarget->returnType.arguments.front() &&
                       !row.spelling.empty();
              });
      if (producedCallResultLoan(body, *referenceCall) == nullptr ||
          !elementRow) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 17,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
    } else {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 18,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    // The pointer local declares the element type; the producing call's
    // own branch validates its staged storage place and index.
    bool borrowed = false;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        borrowed =
            borrowed || (instruction.kind == MirInstructionKind::Borrow &&
                         instruction.loan && *instruction.loan == loan.id);
      }
    }
    if (borrowed) {
      // A Borrow would assign the pointer a second time.
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 19,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
  }

  const auto typeRow = [&](const SemanticType &type) {
    const auto found = std::find_if(
        representations_.types().begin(), representations_.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    return found != representations_.types().end() && !found->spelling.empty();
  };
  // A class value declares in the prelude only when its row carries the
  // 0.215 boundary proof (usable default constructor and move assignment).
  const auto constructibleClassRow = [&](const SemanticType &type) {
    const auto found = std::find_if(
        representations_.types().begin(), representations_.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    return found != representations_.types().end() &&
           !found->spelling.empty() && found->boundaryConstructible;
  };
  const auto fieldRow = [&](SymbolId field) {
    if (!owner) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 20,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    const auto found = std::find_if(
        representations_.symbols().begin(), representations_.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Field &&
                 row.owner == *owner && row.symbol == field && row.ordinal == 0;
        });
    return found != representations_.symbols().end() &&
           !found->spelling.empty();
  };
  const auto bodyRow = [&](HirFunctionInstanceId target) {
    const MirBodyAddress callee{.kind = MirBodyKind::Function, .owner = target};
    const auto found = std::find_if(
        representations_.bodies().begin(), representations_.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == callee;
        });
    return found != representations_.bodies().end() && !found->spelling.empty();
  };
  const auto valueOperand = [](const MirOperand &operand) {
    return operand.kind == MirOperandKind::Value;
  };
  const auto slotPlace = [](const MirPlace &place) {
    return place.root == MirPlaceRootKind::Binding &&
           (place.type.kind == SemanticType::Class ||
            place.type.kind == SemanticType::Storage ||
            place.type.kind == SemanticType::PrefixStorage) &&
           place.projections.empty();
  };
  const auto lifetimeSlotRow = [&]() {
    return std::any_of(
        representations_.capabilities().begin(),
        representations_.capabilities().end(),
        [](const CppMirEmissionCapabilityRepresentation &row) {
          return row.kind == CppMirEmissionCapabilityKind::LifetimeStorage &&
                 !row.spelling.empty();
        });
  };
  const auto storageRow = [&](SymbolId symbol) {
    return std::any_of(
        representations_.symbols().begin(), representations_.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Storage &&
                 row.owner == 0 && row.symbol == symbol && row.ordinal == 0 &&
                 !row.spelling.empty();
        });
  };
  const auto captureRow = [&](std::size_t lambdaOwner, SymbolId symbol,
                              std::size_t ordinal) {
    return std::any_of(
        representations_.symbols().begin(), representations_.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Capture &&
                 row.owner == lambdaOwner && row.symbol == symbol &&
                 row.ordinal == ordinal && !row.spelling.empty();
        });
  };
  const auto capabilityRow = [&](CppMirEmissionCapabilityKind kind) {
    return std::any_of(representations_.capabilities().begin(),
                       representations_.capabilities().end(),
                       [&](const CppMirEmissionCapabilityRepresentation &row) {
                         return row.kind == kind && !row.spelling.empty();
                       });
  };
  const auto lambdaBodyRow = [&](HirLambdaId target) {
    const MirBodyAddress nested{.kind = MirBodyKind::Lambda, .owner = target};
    const auto found = std::find_if(
        representations_.bodies().begin(), representations_.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == nested;
        });
    return found != representations_.bodies().end() && !found->spelling.empty();
  };
  const auto constructSlot = [&](const MirInstruction &construct) {
    if (!construct.result) {
      return MirPlaceId{0};
    }
    MirPlaceId selected = 0;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Initialize ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Value ||
            instruction.operands.front().value != *construct.result ||
            !instruction.destination) {
          continue;
        }
        if (selected != 0) {
          return MirPlaceId{0};
        }
        selected = *instruction.destination;
      }
    }
    return selected;
  };
  const auto syntheticBool = [](const MirOperand &operand) {
    return operand.kind == MirOperandKind::Constant && operand.value == 0 &&
           operand.place == 0 && operand.loan == 0 && operand.literal &&
           operand.type == SemanticType::Bool &&
           std::holds_alternative<bool>(*operand.literal);
  };
  const auto literalSupported = [&](const std::optional<Literal> &literal,
                                    const SemanticType &type) {
    if (!literal) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 21,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    if (std::holds_alternative<std::uint64_t>(*literal)) {
      return typeRow(type);
    }
    if (std::holds_alternative<std::string>(*literal)) {
      return type.kind == SemanticType::StringView && typeRow(type);
    }
    if (const auto *value = std::get_if<BinaryFloat>(&*literal)) {
      return validBinaryFloat(*value) &&
             (value->format == BinaryFloatFormat::Binary64
                  ? type == SemanticType::Double
                  : type == SemanticType::Float) &&
             typeRow(type);
    }
    return std::holds_alternative<CharacterLiteral>(*literal) ||
           std::holds_alternative<bool>(*literal);
  };

  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::This) {
      if (place.projections.empty()) {
        continue;
      }
      // One projected field, or a reference field followed by its
      // dereference: C++ reference members dereference implicitly, so
      // both bind through the same member spelling.
      const bool referenceFieldChain =
          place.projections.size() == 2 &&
          place.projections[0].kind == MirProjectionKind::Field &&
          place.projections[1].kind == MirProjectionKind::Dereference;
      if ((place.projections.size() != 1 && !referenceFieldChain) ||
          place.projections.front().kind != MirProjectionKind::Field ||
          !fieldRow(place.projections.front().field)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 22,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      // A storage-typed receiver field is only the staging carrier for
      // storage-intrinsic calls; it needs a field row but no value row.
      continue;
    }
    if (slotPlace(place)) {
      if (!lifetimeSlotRow() || !typeRow(place.type)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 23,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      // A function body's slot-allocated parameter constructs its slot
      // from the argument in the prelude. Constructor and lambda bodies
      // keep the compatibility route: their moved-in class values still
      // flow through undeclarable class-typed value locals.
      if (parameterBindings != nullptr &&
          std::find(parameterBindings->begin(), parameterBindings->end(),
                    place.binding) != parameterBindings->end() &&
          address.kind != MirBodyKind::Function) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 24,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Symbol) {
      if (place.capture != 0) {
        // A capture place spells its Capture row name inside the literal.
        if (address.kind != MirBodyKind::Lambda || !place.projections.empty() ||
            !captureRow(address.owner, place.symbol, place.capture)) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 25,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      }
      if (!place.projections.empty() || !storageRow(place.symbol)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 26,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (const std::optional<ArrayElementAccess> access =
            arrayElementAccess(body, place)) {
      const MirPlace *array = body.findPlace(access->array);
      if (array == nullptr || !typeRow(array->type) || !typeRow(place.type)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 27,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (const std::optional<ArrayElementAccess> access =
            viewElementAccess(body, place)) {
      const MirPlace *view = body.findPlace(access->array);
      if (view == nullptr || !typeRow(view->type) || !typeRow(place.type)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 28,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (classSubscriptAccess(program_, body, place)) {
      // Reads and stores spell the class's subscript member; each
      // direction proves its contained member at its instruction rule.
      if (!typeRow(place.type)) {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 121,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Loan) {
      const MirLoan *loan = loanById(place.loan);
      if (loan == nullptr || !typeRow(place.type) ||
          !std::all_of(place.projections.begin(), place.projections.end(),
                       [&](const MirPlaceProjection &projection) {
                         return projection.kind == MirProjectionKind::Field &&
                                fieldRow(projection.field);
                       })) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 29,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::This && place.projections.size() == 2 &&
        place.projections[0].kind == MirProjectionKind::Field &&
        place.projections[1].kind == MirProjectionKind::Index) {
      if (!fieldRow(place.projections[0].field) || !typeRow(place.type)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 30,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
        place.type.kind == SemanticType::Reference) {
      // The reference must be a parameter: the signature keeps the C++
      // reference and the body binds its pointer carrier (ADR 018).
      if (parameterBindings == nullptr || !typeRow(place.type) ||
          std::find(parameterBindings->begin(), parameterBindings->end(),
                    place.binding) == parameterBindings->end()) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 31,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Binding && !place.projections.empty() &&
        place.projections[0].kind == MirProjectionKind::Dereference) {
      const MirPlace *base = nullptr;
      for (const MirPlace &candidate : body.places) {
        if (candidate.id != place.id &&
            candidate.root == MirPlaceRootKind::Binding &&
            candidate.binding == place.binding &&
            candidate.projections.empty()) {
          base = &candidate;
        }
      }
      if (base == nullptr || base->type.kind != SemanticType::Reference ||
          !typeRow(place.type) ||
          !std::all_of(place.projections.begin() + 1, place.projections.end(),
                       [&](const MirPlaceProjection &projection) {
                         return projection.kind == MirProjectionKind::Field &&
                                fieldRow(projection.field);
                       })) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 32,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
        place.type.kind == SemanticType::Lambda) {
      // Under a template emission's overlay row the callable local spells
      // its template parameter name and declares ordinarily; without a
      // row the C++ closure type is unnameable, so the local never
      // declares and the fused chain owns every reference or the body
      // declines at the Closure rule or the value rows below.
      if (callableTemplateBody && !typeRow(place.type)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 33,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (copyStageForTemporary(body, place) != nullptr) {
      // The by-value argument staging temporary never materializes: the
      // consuming call spells the source place and C++ performs the copy
      // at the call boundary.
      continue;
    }
    if (unreferencedValueRootedPlace(body, place)) {
      // A pure root record: the rooted value flows through its own uses
      // and the place spells nothing.
      continue;
    }
    if (place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
        place.type.kind == SemanticType::TypePack &&
        parameterBindings != nullptr &&
        std::find(parameterBindings->begin(), parameterBindings->end(),
                  place.binding) != parameterBindings->end()) {
      // The trailing parameter pack's flattened parameters spell at the
      // one forwarding call; the pack place itself never declares.
      continue;
    }
    if (!place.projections.empty() || !typeRow(place.type) ||
        // A class-typed local declares value-initialized, so its row must
        // carry the boundary proof (a deleted default constructor cannot
        // spell the declaration).
        (place.type.kind == SemanticType::Class &&
         !constructibleClassRow(place.type))) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr,
                       "PD id=%d kind=%d owner=%lu ff=%d place=%lu ptype=%d "
                       "proj=%zu row=%d boundary=%d\n",
                       34, gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm, static_cast<unsigned long>(place.id),
                       static_cast<int>(place.type.kind),
                       place.projections.size(), typeRow(place.type) ? 1 : 0,
                       place.type.kind == SemanticType::Class
                           ? (constructibleClassRow(place.type) ? 1 : 0)
                           : -1);
        }
        return false;
      }
    }
  }
  for (const MirValue &value : body.values) {
    if (value.info.type.kind == SemanticType::TypePack) {
      // The pack value spells nothing: its flattened parameters spell at
      // the one forwarding call the bounded shape proved.
      if (packExpansionForwardedOnce(body, value.id) == nullptr) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 135,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (value.info.type.kind == SemanticType::Class) {
      continue;
    }
    // A borrow-staged call input never materializes as a local, so its
    // staged value needs no representation row.
    if (isBorrowStagedResult(body, value)) {
      continue;
    }
    if (isStorageStagedResult(body, value)) {
      continue;
    }
    if (value.info.type.kind == SemanticType::Lambda) {
      // A lambda-typed value never declares; it descends from a fused
      // Closure whose own rule validates every consumer, or it stages a
      // callable parameter place for exactly one invocation receiver.
      if (closureChainDefinition(body, value.id) == nullptr &&
          !(callableTemplateBody &&
            (callableReceiverStage(body, value.id) != nullptr ||
             callableArgumentStage(body, value.id) != nullptr))) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 35,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (value.info.type.kind == SemanticType::Reference) {
      // A reference-typed value never declares a local: a paired
      // call-result loan carries it, and every consumer reads through
      // the loan's place instead.
      const MirValue *referenceValue = body.findValue(value.id);
      const MirInstruction *definition =
          referenceValue == nullptr
              ? nullptr
              : findInstruction(body, referenceValue->definition);
      if (loanStagedCallInput(body, value.id) != nullptr) {
        // The loan-staged reference argument spells its dereferenced
        // pointer carrier at the consuming call and never declares.
        continue;
      }
      if (definition == nullptr ||
          producedCallResultLoan(body, *definition) == nullptr ||
          !body.usesOf(value.id).empty()) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 36,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    if (!typeRow(value.info.type)) {
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 37,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
  }

  for (const MirBlock &block : body.blocks) {
    // Borrow-staged call inputs whose consuming call has not yet appeared.
    // Between staging and the call, the spelled place expression must stay
    // valid: only value-producing reads may intervene, none may observe a
    // staged value, and the block must not end with staging pending.
    std::vector<MirValueId> pendingStaged;
    // A borrow staged in the unique invoke-predecessor whose success edge
    // targets this block hands its place spelling across the edge; the
    // consuming instruction here spells the place exactly as a same-block
    // consumer would.
    const auto stagedInInvokePredecessor = [&](MirValueId id) {
      const MirValue *record = body.findValue(id);
      const MirInstruction *stage =
          record == nullptr ? nullptr
                            : findInstruction(body, record->definition);
      if (stage == nullptr || stage->kind != MirInstructionKind::CallInput) {
        return false;
      }
      const MirBlock *stageBlock = nullptr;
      for (const MirBlock &candidate : body.blocks) {
        for (const MirInstruction &member : candidate.instructions) {
          if (member.id == stage->id) {
            stageBlock = &candidate;
          }
        }
      }
      if (stageBlock == nullptr ||
          stageBlock->terminator.kind != MirTerminatorKind::Invoke ||
          stageBlock->terminator.target != block.id) {
        return false;
      }
      std::size_t predecessors = 0;
      for (const MirBlock &candidate : body.blocks) {
        const MirTerminator &edge = candidate.terminator;
        if ((edge.kind == MirTerminatorKind::Goto ||
             edge.kind == MirTerminatorKind::Branch ||
             edge.kind == MirTerminatorKind::Invoke) &&
            edge.target == block.id) {
          ++predecessors;
        }
        if ((edge.kind == MirTerminatorKind::Branch ||
             edge.kind == MirTerminatorKind::Invoke) &&
            edge.elseTarget == block.id) {
          ++predecessors;
        }
      }
      return predecessors == 1;
    };
    const auto referencesPendingStaged = [&](const MirInstruction &between) {
      for (const MirOperand &operand : between.operands) {
        if (operand.kind == MirOperandKind::Value &&
            std::find(pendingStaged.begin(), pendingStaged.end(),
                      operand.value) != pendingStaged.end()) {
          return true;
        }
      }
      return between.receiver &&
             between.receiver->kind == MirOperandKind::Value &&
             std::find(pendingStaged.begin(), pendingStaged.end(),
                       between.receiver->value) != pendingStaged.end();
    };
    for (const MirInstruction &instruction : block.instructions) {
      if (!pendingStaged.empty()) {
        const bool consumingCall =
            (instruction.kind == MirInstructionKind::Call &&
             instruction.intrinsic == IntrinsicKind::None) ||
            // The class-valued publication construct consumes its staged
            // arguments exactly like a call: the constructor call spells
            // the staged places directly.
            (instruction.kind == MirInstructionKind::Construct &&
             instruction.result &&
             returnConstructDefinition(body, *instruction.result) ==
                 &instruction);
        // A Move between the staging and its consuming call is inert
        // argument staging: it neither raises nor observes, so the
        // staged callable's deferred spelling stays exact. Lifecycle and
        // EndBorrow spell as comments and touch no state, so they are
        // equally inert inside the window.
        if (!consumingCall &&
            (instruction.kind != MirInstructionKind::CallInput &&
             instruction.kind != MirInstructionKind::Load &&
             instruction.kind != MirInstructionKind::Compute &&
             instruction.kind != MirInstructionKind::Move &&
             instruction.kind != MirInstructionKind::Lifecycle &&
             instruction.kind != MirInstructionKind::EndBorrow &&
             instruction.kind != MirInstructionKind::Call)) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 38,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        if (!consumingCall && referencesPendingStaged(instruction)) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 39,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
      }
      switch (instruction.kind) {
      case MirInstructionKind::Lifecycle:
        continue;
      case MirInstructionKind::Construct: {
        // The slot vocabulary spells argument-less generated-default
        // construction only; a declared constructor's arguments would be
        // silently dropped by `.construct()`.
        if (failureForm && instruction.result &&
            returnConstructDefinition(body, *instruction.result) ==
                &instruction) {
          // The class-valued publication construct: every operand is a
          // staged value and the constructor's own row spells the call.
          if (!typeRow(instruction.info.type) ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), valueOperand) ||
              !instruction.localFailureSites.empty()) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 40,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          // A borrow-staged argument consumes its stage here: pending in
          // this block, or handed off from the unique invoke-predecessor.
          bool stagedConsumable = true;
          for (const MirOperand &operand : instruction.operands) {
            const MirValue *record = body.findValue(operand.value);
            const MirInstruction *stage =
                record == nullptr ? nullptr
                                  : findInstruction(body, record->definition);
            if (stage == nullptr ||
                stage->kind != MirInstructionKind::CallInput ||
                stage->operands.empty() ||
                (stage->operands.front().kind != MirOperandKind::BorrowRead &&
                 stage->operands.front().kind != MirOperandKind::BorrowWrite)) {
              continue;
            }
            const auto pending = std::find(pendingStaged.begin(),
                                           pendingStaged.end(), operand.value);
            if (pending != pendingStaged.end()) {
              pendingStaged.erase(pending);
            } else if (!stagedInInvokePredecessor(operand.value)) {
              stagedConsumable = false;
            }
          }
          if (!stagedConsumable) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 126,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        if (instruction.result && !instruction.destination &&
            !instruction.receiver &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            instruction.localFailureSites.empty() &&
            std::all_of(instruction.operands.begin(),
                        instruction.operands.end(), valueOperand) &&
            constructibleClassRow(instruction.info.type)) {
          // A value-producing construction assigns the constructor call
          // into its declared class local; the row's boundary proof
          // guarantees the declaration and the assignment both compile,
          // and a propagating construction's invoke edge stays vacuous
          // under the producer rules. The slot protocol keeps ownership
          // of any construct whose value a slot-place Initialize
          // consumes: the value route would bypass slot engagement and
          // later slot reads would find the slot empty.
          bool slotConsumer = false;
          for (const MirValueUse &use : body.usesOf(*instruction.result)) {
            const MirInstruction *consumer =
                findInstruction(body, use.instruction);
            if (consumer != nullptr &&
                consumer->kind == MirInstructionKind::Initialize &&
                consumer->destination) {
              const MirPlace *destinationPlace =
                  body.findPlace(*consumer->destination);
              if (destinationPlace != nullptr && slotPlace(*destinationPlace)) {
                slotConsumer = true;
              }
            }
          }
          if (!slotConsumer) {
            continue;
          }
        }
        const MirPlaceId destination = constructSlot(instruction);
        const MirPlace *slot =
            destination == 0 ? nullptr : body.findPlace(destination);
        if (slot == nullptr || !slotPlace(*slot) || instruction.receiver ||
            !std::all_of(instruction.operands.begin(),
                         instruction.operands.end(), valueOperand) ||
            instruction.constructorKind != ConstructorKind::Ordinary) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 41,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      }
      case MirInstructionKind::Borrow: {
        const MirLoan *loan =
            instruction.loan ? loanById(*instruction.loan) : nullptr;
        if (loan == nullptr || !instruction.localFailureSites.empty()) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 42,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        if (loan->kind == MirLoanKind::Stored) {
          // The reference field binds in the member initializer list;
          // the Borrow spells as a comment only.
          continue;
        }
        const MirPlace *source = body.findPlace(loan->source);
        if (source == nullptr || !typeRow(source->type)) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 43,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        if (source->type.kind == SemanticType::Storage ||
            source->type.kind == SemanticType::PrefixStorage) {
          // A storage-sourced loan publishes a discharged read's element;
          // the pairing, the staged storage place, and the element type
          // row must all resolve or the body declines.
          const MirInstruction *read =
              pairedDischargedRead(body, loan->producedBy);
          if (read == nullptr || read->operands.size() != 2 ||
              storageStagedPlace(body, read->operands.front()) == nullptr ||
              source->type.arguments.empty() ||
              !typeRow(source->type.arguments.front())) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 44,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
        }
        continue;
      }
      case MirInstructionKind::EndBorrow:
        if (!instruction.loan || loanById(*instruction.loan) == nullptr) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 45,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      case MirInstructionKind::Drop: {
        if (trivialMirDrop(body, instruction) ||
            movedOutOwnerDrop(body, instruction) ||
            storeConsumedStorageValueDrop(body, instruction)) {
          continue;
        }
        if (failureForm && instruction.destination) {
          if (const MirPlace *dropped =
                  body.findPlace(*instruction.destination);
              dropped != nullptr && dropped->root == MirPlaceRootKind::Value &&
              returnMoveDefinition(body, dropped->value) != nullptr) {
            continue;
          }
        }
        if (failureForm && instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().failureCleanup &&
            instruction.destination) {
          // The failure-cleanup text destroys the slot, so the probe
          // demands the same slot place the success path demands.
          const MirPlace *cleanupSlot =
              body.findPlace(*instruction.destination);
          if (cleanupSlot == nullptr || !slotPlace(*cleanupSlot)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 46,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        const MirPlace *slot = instruction.destination
                                   ? body.findPlace(*instruction.destination)
                                   : nullptr;
        if (slot == nullptr || !slotPlace(*slot)) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 47,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      }
      case MirInstructionKind::Compute: {
        if (!instruction.result) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 48,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        if (instruction.operation == MirOperation::PackExpansion) {
          // Only the bounded forward-once shape is spellable: the pack's
          // flattened parameters spell at the consuming allocation call.
          // HELD CLOSED until the class-value flow guards land: admitting
          // the allocation body flips its callers through the admission
          // fixpoint, and their class-typed call results have no declared
          // spelling yet (measured: 13-ownership and 16-move-generics
          // mains emitted assignments into undeclared locals).
          if (true || packExpansionForwardedOnce(body, *instruction.result) !=
                          &instruction) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 136,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        if (!failureForm && address.kind == MirBodyKind::Lambda &&
            !cppMirTerminalCheckedHelperSpelling(instruction.operation)
                 .empty() &&
            !instruction.localFailureSites.empty()) {
          // Plain-shape checked arithmetic spells the compatibility
          // terminal helper: it contains the failure itself and never
          // returns on it, so the paired invoke edge cannot branch. The
          // operand types must equal the result type so the helper's
          // deduced result assigns without conversion.
          if (instruction.localFailureSites.size() != 1 ||
              instruction.definedFailure.localOrigins.size() != 1 ||
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) == nullptr ||
              instruction.operands.empty() || instruction.operands.size() > 2 ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), valueOperand) ||
              !typeRow(instruction.info.type) ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(),
                           [&](const MirOperand &operand) {
                             return operand.type == instruction.info.type;
                           })) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 49,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        if (failureForm &&
            !cppMirCheckedOperationHelperSpelling(instruction.operation)
                 .empty() &&
            !instruction.localFailureSites.empty()) {
          // A checked detector spells as its status helper and its failure
          // edge writes one exact record: one site, one origin, both known
          // to the program's failure metadata.
          // The mir_checked_* helper family is integral-only (its range
          // checks static_assert on integral operands); a floating source
          // or result waits for a floating checked family.
          const auto integralKind = [](const SemanticType &type) {
            switch (type.kind) {
            case SemanticType::Int8:
            case SemanticType::Int16:
            case SemanticType::Int32:
            case SemanticType::Int64:
            case SemanticType::UInt8:
            case SemanticType::UInt16:
            case SemanticType::UInt32:
            case SemanticType::UInt64:
            case SemanticType::Char:
              return true;
            default: {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 50,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
            }
          };
          const auto detectorOperand = [&](const MirOperand &operand) {
            return integralKind(operand.type) &&
                   (valueOperand(operand) ||
                    (operand.kind == MirOperandKind::Constant &&
                     operand.literal.has_value() && typeRow(operand.type)));
          };
          if (!integralKind(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 51,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          if (instruction.localFailureSites.size() != 1 ||
              instruction.definedFailure.localOrigins.size() != 1 ||
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) == nullptr ||
              instruction.operands.empty() || instruction.operands.size() > 2 ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), detectorOperand) ||
              !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 52,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        switch (instruction.operation) {
        case MirOperation::Literal:
          if (!literalSupported(instruction.literal, instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 53,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        case MirOperation::Identity:
        case MirOperation::LogicalNot:
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front())) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 54,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        case MirOperation::ExpectedHasValue:
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !instruction.localFailureSites.empty()) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 55,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        case MirOperation::Unexpected: {
          // Spells through the Expected capability row's construction
          // call; the result is the expected-typed value itself.
          const auto expectedRow = std::find_if(
              representations_.capabilities().begin(),
              representations_.capabilities().end(),
              [](const CppMirEmissionCapabilityRepresentation &row) {
                return row.kind == CppMirEmissionCapabilityKind::Expected;
              });
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !instruction.localFailureSites.empty() ||
              expectedRow == representations_.capabilities().end() ||
              expectedRow->spelling.empty()) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 56,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          // The wrapper value never materializes (std::unexpected has no
          // default construction): its only consumer must be the Return
          // that converts it into the expected-typed result, where the
          // construction spells inline.
          {
            const std::vector<MirValueUse> &uses =
                body.usesOf(*instruction.result);
            bool returnConsumed = false;
            if (uses.size() == 1 &&
                uses.front().kind == MirValueUseKind::Terminator) {
              const MirBlock *userBlock = nullptr;
              for (const MirBlock &candidate : body.blocks) {
                if (candidate.id == uses.front().block) {
                  userBlock = &candidate;
                }
              }
              returnConsumed =
                  userBlock != nullptr &&
                  userBlock->terminator.kind == MirTerminatorKind::Return &&
                  userBlock->terminator.value &&
                  userBlock->terminator.value->kind == MirOperandKind::Value &&
                  userBlock->terminator.value->value == *instruction.result;
            }
            if (!returnConsumed) {
              {
                if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                  std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 57,
                               gtiProbeTraceKind, gtiProbeTraceOwner,
                               gtiProbeTraceForm);
                }
                return false;
              }
            }
          }
          continue;
        }
        case MirOperation::Closure: {
          const MirLambdaInstance *lambda =
              instruction.lambdaTarget
                  ? program_.findLambda(*instruction.lambdaTarget)
                  : nullptr;
          if (lambda == nullptr ||
              !capabilityRow(CppMirEmissionCapabilityKind::Closure) ||
              !lambdaBodyRow(lambda->id) ||
              !closureChainAdmits(program_, body, instruction) ||
              !typeRow(lambda->returnType)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 58,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          bool rows = true;
          for (const SemanticType &type : lambda->parameterTypes) {
            rows = rows && typeRow(type);
          }
          for (std::size_t index = 0; index < lambda->captureSymbols.size();
               ++index) {
            rows = rows && captureRow(lambda->id, lambda->captureSymbols[index],
                                      index + 1);
          }
          // The literal embeds the lambda body itself, so the nested body
          // must prove its own plain-shape text.
          if (!rows ||
              !supportsBodyTextImpl(
                  {.kind = MirBodyKind::Lambda, .owner = lambda->id}, false)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 59,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        case MirOperation::Positive:
        case MirOperation::BitwiseNot:
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 60,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        case MirOperation::Aggregate:
          // The empty aggregate spells as the row type's value
          // initialization; element-carrying aggregates stay outside.
          if (!instruction.operands.empty() ||
              !instruction.localFailureSites.empty() ||
              !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 61,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        case MirOperation::Convert:
          // An unchecked conversion is proven in-range by MIR; a checked
          // one carries sites and belongs to the failure vocabulary's
          // detector rules above.
          if (!instruction.localFailureSites.empty() ||
              instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 62,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        case MirOperation::BitwiseAnd:
        case MirOperation::BitwiseOr:
        case MirOperation::BitwiseXor:
          if (instruction.operands.size() != 2 ||
              !valueOperand(instruction.operands[0]) ||
              !valueOperand(instruction.operands[1]) ||
              !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 63,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        case MirOperation::Equal:
        case MirOperation::NotEqual:
        case MirOperation::Less:
        case MirOperation::LessEqual:
        case MirOperation::Greater:
        case MirOperation::GreaterEqual: {
          // A literal comparison operand spells inline, exactly like the
          // checked detectors' constant operands.
          const auto comparisonOperand = [&](const MirOperand &operand) {
            return valueOperand(operand) ||
                   (operand.kind == MirOperandKind::Constant &&
                    operand.literal.has_value() && typeRow(operand.type));
          };
          if (instruction.operands.size() != 2 ||
              !comparisonOperand(instruction.operands[0]) ||
              !comparisonOperand(instruction.operands[1])) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 64,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        case MirOperation::Index: {
          // A value-level view element read: string_view_at contains the
          // defined bound contract terminally on both forms, exactly like
          // the place-projected form the Load path spells.
          if (instruction.operands.size() != 2 ||
              !valueOperand(instruction.operands[0]) ||
              !valueOperand(instruction.operands[1]) ||
              instruction.operands[0].type.kind != SemanticType::StringView ||
              !instruction.result || !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 129,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        default: {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 65,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
        }
      }
      case MirInstructionKind::Load: {
        if (!instruction.result || instruction.operands.size() != 1 ||
            instruction.operands.front().place == 0) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 66,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        if (const MirPlace *source =
                body.findPlace(instruction.operands.front().place);
            source != nullptr) {
          if (const std::optional<ClassSubscriptAccess> subscript =
                  classSubscriptAccess(program_, body, *source)) {
            // The subscript member call contains failure terminally in
            // its own emitted body, so the plain call is exact on both
            // text forms and any paired invoke edge is a plain goto.
            if (containedSubscriptMember(program_, representations_,
                                         subscript->owner,
                                         ReceiverMutability::ReadOnly,
                                         subscript->indexType) == nullptr ||
                !typeRow(instruction.info.type)) {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 122,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
            continue;
          }
        }
        if (!instruction.localFailureSites.empty()) {
          // A bounds-checked element load is a failure detector: exactly
          // one site and origin, spelled through the checked-read helper.
          const MirPlace *source =
              body.findPlace(instruction.operands.front().place);
          if (!failureForm || source == nullptr ||
              !arrayElementAccess(body, *source) ||
              instruction.localFailureSites.size() != 1 ||
              instruction.definedFailure.localOrigins.size() != 1 ||
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) == nullptr ||
              !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 67,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
        }
        continue;
      }
      case MirInstructionKind::Initialize: {
        if (!instruction.destination || instruction.operands.size() != 1) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 68,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        const MirPlace *destination = body.findPlace(*instruction.destination);
        // A store into a receiver field is only expressible through the
        // mutable-receiver binding; under a read-only receiver the text step
        // would bind the field const and the emitted C++ would not compile.
        if (destination != nullptr &&
            destination->root == MirPlaceRootKind::This &&
            receiverMutability != ReceiverMutability::Mutable) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 69,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        if (destination != nullptr &&
            classSubscriptAccess(program_, body, *destination)) {
          // An initializing subscript store uses the same mutable member
          // call as an assignment.
          if (const std::optional<ClassSubscriptAccess> subscript =
                  classSubscriptAccess(program_, body, *destination);
              containedSubscriptMember(program_, representations_,
                                       subscript->owner,
                                       ReceiverMutability::Mutable,
                                       subscript->indexType) == nullptr ||
              !valueOperand(instruction.operands.front())) {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 123,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          } else {
            continue;
          }
        }
        if (destination != nullptr && slotPlace(*destination)) {
          // The reparenting Initialize is the slot construct's paired
          // destination and emits as a comment only.
          if (!valueOperand(instruction.operands.front())) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 70,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        if (!valueOperand(instruction.operands.front()) &&
            !syntheticBool(instruction.operands.front())) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 71,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      }
      case MirInstructionKind::Assign: {
        if (!instruction.destination || !instruction.result ||
            instruction.operands.size() != 1 ||
            !valueOperand(instruction.operands.front())) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 72,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        const MirPlace *destination = body.findPlace(*instruction.destination);
        if (destination != nullptr &&
            destination->root == MirPlaceRootKind::This &&
            receiverMutability != ReceiverMutability::Mutable) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 73,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        if (destination != nullptr) {
          if (const std::optional<ClassSubscriptAccess> subscript =
                  classSubscriptAccess(program_, body, *destination)) {
            // The mutable subscript member call contains failure
            // terminally in its own emitted body; the plain call is
            // exact on both text forms.
            if (containedSubscriptMember(program_, representations_,
                                         subscript->owner,
                                         ReceiverMutability::Mutable,
                                         subscript->indexType) == nullptr) {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 124,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
            continue;
          }
        }
        if (!instruction.localFailureSites.empty()) {
          // A bounds-checked element store is a failure detector spelled
          // through the checked-write helper.
          if (!failureForm || destination == nullptr ||
              !arrayElementAccess(body, *destination) ||
              instruction.localFailureSites.size() != 1 ||
              instruction.definedFailure.localOrigins.size() != 1 ||
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) == nullptr) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 74,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
        }
        continue;
      }
      case MirInstructionKind::Move:
        // By-value element staging: exactly one moved place, no sites.
        if (!instruction.result || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Move ||
            instruction.operands.front().place == 0 ||
            body.findPlace(instruction.operands.front().place) == nullptr ||
            !instruction.localFailureSites.empty()) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 75,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      case MirInstructionKind::CallInput: {
        if (!instruction.result || instruction.receiver ||
            instruction.operands.size() != 1) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 76,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        const MirOperand &staged = instruction.operands.front();
        if (staged.kind == MirOperandKind::BorrowRead ||
            staged.kind == MirOperandKind::BorrowWrite) {
          // A borrowed call input stages a place the call spells directly;
          // the write form carries the mutable receiver.
          if (staged.place == 0 || body.findPlace(staged.place) == nullptr) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 77,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          pendingStaged.push_back(*instruction.result);
          continue;
        }
        if (loanStagedCallInput(body, *instruction.result) == &instruction) {
          // The loan-staged input spells its dereferenced pointer carrier
          // at the consuming call; nothing stages here.
          continue;
        }
        if (copyStagedCallInput(body, *instruction.result) == &instruction) {
          // The by-value copy stage spells its source place at the
          // consuming call; the temporary never materializes.
          continue;
        }
        if (!valueOperand(staged)) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 78,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      }
      case MirInstructionKind::Call: {
        if (dischargedStorageReadCall(instruction)) {
          // The element publishes through the loan-producing Borrow: the
          // call needs its staged storage place, a value index, and a
          // result no other instruction consumes.
          if (instruction.functionTarget || instruction.operands.size() != 2 ||
              storageStagedPlace(body, instruction.operands.front()) ==
                  nullptr ||
              !valueOperand(instruction.operands.back()) ||
              !instruction.result || !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 79,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          if (instruction.receiver &&
              ((instruction.receiver->kind != MirOperandKind::BorrowRead &&
                instruction.receiver->kind != MirOperandKind::BorrowWrite) ||
               instruction.receiver->place == 0 ||
               body.findPlace(instruction.receiver->place) == nullptr)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 80,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        if (instruction.intrinsic == IntrinsicKind::PrefixStorageLength) {
          // The logical-length read carries no failure and spells the
          // shipped helper over the staged storage place on both forms.
          if (instruction.functionTarget || !instruction.result ||
              !instruction.localFailureSites.empty() ||
              instruction.operands.size() != 1 ||
              storageStagedPlace(body, instruction.operands.front()) ==
                  nullptr ||
              !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 81,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        if (prefixStorageIntrinsic(instruction.intrinsic)) {
          // The storage failure form spells the shipped mir_prefix_*_v1
          // checked helper over the staged storage place lvalue; every
          // operation carries checkable sites, so the success form
          // declines. The modeling receiver is a raw borrow of a
          // spellable place and never spells.
          if (!failureForm || instruction.functionTarget ||
              instruction.localFailureSites.empty() ||
              instruction.definedFailure.localOrigins.empty() ||
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) == nullptr) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 82,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          if (instruction.intrinsic == IntrinsicKind::AllocatePrefixStorage) {
            // Allocation publishes into its storage-typed result value;
            // interior exhaustion keeps the sealed legacy path.
            if (!instruction.result || instruction.operands.size() != 1 ||
                !valueOperand(instruction.operands.front()) ||
                !typeRow(instruction.info.type) ||
                instruction.info.type.arguments.size() != 1 ||
                !typeRow(instruction.info.type.arguments.front())) {
              {
                if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                  std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 83,
                               gtiProbeTraceKind, gtiProbeTraceOwner,
                               gtiProbeTraceForm);
                }
                return false;
              }
            }
            // A growth-path allocation borrows the container it will
            // replace as its modeling receiver; the receiver is a raw
            // borrow of a spellable place and never spells, exactly like
            // the other storage intrinsics below.
            if (instruction.receiver &&
                ((instruction.receiver->kind != MirOperandKind::BorrowRead &&
                  instruction.receiver->kind != MirOperandKind::BorrowWrite) ||
                 instruction.receiver->place == 0 ||
                 body.findPlace(instruction.receiver->place) == nullptr)) {
              {
                if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                  std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 84,
                               gtiProbeTraceKind, gtiProbeTraceOwner,
                               gtiProbeTraceForm);
                }
                return false;
              }
            }
            continue;
          }
          if (instruction.result || instruction.operands.empty()) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 85,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          if (instruction.receiver &&
              ((instruction.receiver->kind != MirOperandKind::BorrowRead &&
                instruction.receiver->kind != MirOperandKind::BorrowWrite) ||
               instruction.receiver->place == 0 ||
               body.findPlace(instruction.receiver->place) == nullptr)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 86,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          if (storageStagedPlace(body, instruction.operands.front()) ==
              nullptr) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 87,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          for (std::size_t index = 1; index < instruction.operands.size();
               ++index) {
            if (storageStagedPlace(body, instruction.operands[index]) !=
                nullptr) {
              // Relocation's destination stages a second storage place.
              continue;
            }
            if (!valueOperand(instruction.operands[index])) {
              {
                if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                  std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 88,
                               gtiProbeTraceKind, gtiProbeTraceOwner,
                               gtiProbeTraceForm);
                }
                return false;
              }
            }
          }
          continue;
        }
        if (callableValueInvocation(instruction)) {
          // Invoking a callable value spells the fused closure literal —
          // or, in a template body, the staged callable parameter place —
          // followed by its plain argument list; both interiors contain
          // failure terminally, so the paired invoke edge never branches.
          // No CallInput staging exists here.
          const bool fusedLiteral =
              closureChainDefinition(body, instruction.receiver->value) !=
              nullptr;
          const bool stagedPlace =
              callableTemplateBody &&
              callableReceiverStage(body, instruction.receiver->value) !=
                  nullptr;
          // An invocation argument is a staged value or a loan of an
          // admitted entry parameter (spelled as the dereferenced
          // pointer carrier; the loan loop above vetted every loan).
          const auto invocationOperand = [&](const MirOperand &operand) {
            return valueOperand(operand) ||
                   (operand.kind == MirOperandKind::Loan && operand.loan != 0 &&
                    body.findLoan(operand.loan) != nullptr);
          };
          if (!pendingStaged.empty() ||
              !capabilityRow(CppMirEmissionCapabilityKind::Closure) ||
              !capabilityRow(CppMirEmissionCapabilityKind::CallableDispatch) ||
              (!fusedLiteral && !stagedPlace) ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), invocationOperand) ||
              (instruction.result && !typeRow(instruction.info.type))) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 89,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        // A receiver-carrying call spells its staged borrowed place and the
        // qualified member name — the explicit qualification states the
        // static dispatch MIR proved. Admission requires a read-only,
        // source-defined GTI member target; write-staged receivers wait
        // for measured demand.
        std::vector<MirValueId> consumedStaged;
        if (instruction.receiver &&
            (instruction.intrinsic == IntrinsicKind::StringViewSize ||
             instruction.intrinsic == IntrinsicKind::StringViewEmpty ||
             instruction.intrinsic == IntrinsicKind::ArraySize ||
             instruction.intrinsic == IntrinsicKind::ExpectedValue ||
             instruction.intrinsic == IntrinsicKind::ExpectedError)) {
          // A builtin member read spells the staged place's member
          // directly: the view size/empty pair is failure-free, and the
          // expected extractions contain their wrong-state failure
          // terminally inside the spelled member itself.
          const bool expectedExtraction =
              instruction.intrinsic == IntrinsicKind::ExpectedValue ||
              instruction.intrinsic == IntrinsicKind::ExpectedError;
          const MirInstruction *staged =
              borrowStagedCallInput(body, *instruction.receiver);
          const MirOperand &receiverBorrow = staged != nullptr
                                                 ? staged->operands.front()
                                                 : *instruction.receiver;
          const MirPlace *viewPlace =
              receiverBorrow.kind == MirOperandKind::BorrowRead &&
                      receiverBorrow.place != 0
                  ? body.findPlace(receiverBorrow.place)
                  : nullptr;
          if (viewPlace == nullptr || !instruction.result ||
              !instruction.operands.empty() ||
              instruction.localFailureSites.size() >
                  (expectedExtraction ? 1u : 0u) ||
              instruction.dispatch != CallDispatch::Static ||
              !typeRow(instruction.info.type)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 127,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          if (staged != nullptr) {
            consumedStaged.push_back(*staged->result);
          }
          if (!std::all_of(consumedStaged.begin(), consumedStaged.end(),
                           [&](MirValueId id) {
                             return std::find(pendingStaged.begin(),
                                              pendingStaged.end(),
                                              id) != pendingStaged.end() ||
                                    stagedInInvokePredecessor(id);
                           })) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 128,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          for (const MirValueId id : consumedStaged) {
            pendingStaged.erase(
                std::remove(pendingStaged.begin(), pendingStaged.end(), id),
                pendingStaged.end());
          }
          continue;
        }
        if (instruction.receiver) {
          if (instruction.intrinsic != IntrinsicKind::None ||
              instruction.dispatch != CallDispatch::Static) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 90,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          // The receiver borrow arrives staged through a CallInput or
          // directly on the receiver operand (a self-member call borrows
          // its own receiver with no staging step); both name a spellable
          // place whose access mode must match the member's receiver
          // mutability exactly.
          const MirInstruction *staged =
              borrowStagedCallInput(body, *instruction.receiver);
          const MirOperand &receiverBorrow = staged != nullptr
                                                 ? staged->operands.front()
                                                 : *instruction.receiver;
          const MirInstruction *movedStage =
              staged == nullptr &&
                      instruction.receiver->kind == MirOperandKind::Value
                  ? copyStagedCallInput(body, instruction.receiver->value)
                  : nullptr;
          const StagedTemporarySource movedSource =
              movedStage != nullptr
                  ? stagedTemporarySourceFor(body, *movedStage)
                  : StagedTemporarySource{};
          const MirPlace *receiverPlace =
              movedSource.place != nullptr && movedSource.moved
                  ? movedSource.place
                  : ((receiverBorrow.kind == MirOperandKind::BorrowRead ||
                      receiverBorrow.kind == MirOperandKind::BorrowWrite) &&
                             receiverBorrow.place != 0
                         ? body.findPlace(receiverBorrow.place)
                         : nullptr);
          if (receiverPlace == nullptr) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 91,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          const MirFunctionInstance *target =
              instruction.functionTarget
                  ? program_.findFunctionInstance(*instruction.functionTarget)
                  : nullptr;
          const ReceiverMutability stagedMutability =
              movedSource.place != nullptr && movedSource.moved
                  ? ReceiverMutability::Consuming
                  : (receiverBorrow.kind == MirOperandKind::BorrowWrite
                         ? ReceiverMutability::Mutable
                         : ReceiverMutability::ReadOnly);
          if (target == nullptr || !target->owner || target->staticMember ||
              target->virtualMethod || target->pureVirtual ||
              target->overrideMethod ||
              target->receiverMutability != stagedMutability ||
              target->linkage != LanguageLinkage::Gti ||
              target->definitionKind !=
                  MirFunctionInstance::DefinitionKind::Source) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 92,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          // The staged place must BE the member's owner object: a place
          // that merely reaches it through an implicit owner dereference
          // would misdirect the spelled member access.
          const MirClassInstance *ownerInstance =
              program_.findClassInstance(*target->owner);
          if (ownerInstance == nullptr ||
              receiverPlace->type != ownerInstance->type) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 93,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          if (staged != nullptr) {
            consumedStaged.push_back(*staged->result);
          }
        }
        if (!failureForm && instruction.functionTarget) {
          // The success form spells plain calls; a failure-capable GTI
          // callee needs the transformed convention and stays with the
          // failure form fail-closed. Inside a deduced-callable template
          // body every reachable callee convention is terminally
          // contained (the compatibility wrapper or another template), so
          // the plain call is exact there.
          const MirFunctionInstance *target =
              program_.findFunctionInstance(*instruction.functionTarget);
          if (target == nullptr ||
              (!callableTemplateBody &&
               !deducedCallableCallee(program_, instruction) &&
               !terminallyContainedPlainCallee(program_, representations_,
                                               instruction) &&
               !terminallyContainedMemberCallee(program_, representations_,
                                                instruction) &&
               target->mayRaiseDefinedFailure &&
               target->linkage == LanguageLinkage::Gti &&
               target->definitionKind ==
                   MirFunctionInstance::DefinitionKind::Source)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 94,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
        }
        if (!failureForm && instruction.result &&
            instruction.info.type.kind == SemanticType::Class &&
            returnCallDefinition(body, *instruction.result) == nullptr) {
          // A plain class result publishes only at its consuming return;
          // any other consumer would need an undeclarable class local.
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 132,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        if (failureForm) {
          const MirFunctionInstance *target =
              instruction.functionTarget
                  ? program_.findFunctionInstance(*instruction.functionTarget)
                  : nullptr;
          if (instruction.functionTarget && target == nullptr) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 95,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          if (target != nullptr && target->mayRaiseDefinedFailure &&
              deducedCallableCallee(program_, instruction)) {
            // The template callee contains its failure terminally and is
            // called plainly; the paired invoke edge never branches.
          } else if (target != nullptr && target->mayRaiseDefinedFailure &&
                     terminallyContainedPlainCallee(program_, representations_,
                                                    instruction)) {
            // The callee's own plain body contains its failure terminally,
            // so this call spells the plain name and its invoke edge is a
            // plain goto; only the result row is demanded.
            if (instruction.result && !typeRow(instruction.info.type)) {
              {
                if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                  std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 96,
                               gtiProbeTraceKind, gtiProbeTraceOwner,
                               gtiProbeTraceForm);
                }
                return false;
              }
            }
          } else if (target != nullptr && target->mayRaiseDefinedFailure) {
            // A failure-capable callee is reached through the transformed
            // convention: the call publishes into a scalar result (or a
            // typed discard) and forwards the caller's record pointer.
            // Whether the callee's transformed body actually exists is the
            // admission fixpoint's question, answered before any body from
            // this program is selected.
            const std::optional<CppMirTypeRepresentationKind> returnKind =
                cppMirExpectedTypeRepresentation(target->returnType);
            if (target->linkage != LanguageLinkage::Gti ||
                target->definitionKind !=
                    MirFunctionInstance::DefinitionKind::Source ||
                !returnKind ||
                (*returnKind != CppMirTypeRepresentationKind::Scalar &&
                 *returnKind != CppMirTypeRepresentationKind::Void &&
                 *returnKind != CppMirTypeRepresentationKind::Expected &&
                 // A class result lands in the declared receiving local
                 // (or a declared discard), which the boundary-proof row
                 // guarantees compiles.
                 !(*returnKind == CppMirTypeRepresentationKind::Class &&
                   constructibleClassRow(target->returnType)) &&
                 // A reference result arrives through the callee's `T **`
                 // out-parameter, landing directly in this call's paired
                 // loan pointer (ADR 018 §5).
                 !(*returnKind == CppMirTypeRepresentationKind::Reference &&
                   producedCallResultLoan(body, instruction) != nullptr)) ||
                ((*returnKind == CppMirTypeRepresentationKind::Scalar ||
                  *returnKind == CppMirTypeRepresentationKind::Expected) &&
                 !typeRow(target->returnType))) {
              {
                if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                  std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 97,
                               gtiProbeTraceKind, gtiProbeTraceOwner,
                               gtiProbeTraceForm);
                }
                return false;
              }
            }
          }
        }
        if (instruction.intrinsic != IntrinsicKind::None) {
          // A numeric-conversion intrinsic spells as the shipped
          // numeric_cast helper over one staged operand.
          if ((instruction.intrinsic ==
                   IntrinsicKind::NumericTypeParameterConversion ||
               instruction.intrinsic ==
                   IntrinsicKind::NumericAliasConversion) &&
              !instruction.functionTarget && instruction.result &&
              instruction.localFailureSites.empty() &&
              instruction.operands.size() == 1 &&
              valueOperand(instruction.operands.front()) &&
              typeRow(instruction.info.type)) {
            continue;
          }
          // A checked-result intrinsic produces its failure inside the
          // Expected value — no edges — and spells as the shipped helper
          // with the error type as its template argument.
          if ((instruction.intrinsic == IntrinsicKind::IntegerCheckedAdd ||
               instruction.intrinsic == IntrinsicKind::IntegerCheckedSubtract ||
               instruction.intrinsic ==
                   IntrinsicKind::IntegerCheckedMultiply) &&
              !instruction.functionTarget && instruction.result &&
              instruction.localFailureSites.empty() &&
              instruction.operands.size() == 2 &&
              std::all_of(instruction.operands.begin(),
                          instruction.operands.end(), valueOperand) &&
              instruction.info.type.kind == SemanticType::Expected &&
              instruction.info.type.arguments.size() == 2 &&
              typeRow(instruction.info.type) &&
              typeRow(instruction.info.type.arguments[1])) {
            continue;
          }
          if (storageBoundsCheckCall(instruction)) {
            // The terminal logical-size check spells the compatibility
            // helper over its two staged operands; the record and edge
            // machinery never runs because the helper contains.
            if (instruction.result || instruction.operands.size() != 2 ||
                !std::all_of(instruction.operands.begin(),
                             instruction.operands.end(), valueOperand)) {
              {
                if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                  std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 98,
                               gtiProbeTraceKind, gtiProbeTraceOwner,
                               gtiProbeTraceForm);
                }
                return false;
              }
            }
            continue;
          }
          if (checkedConversionIntrinsicCall(instruction)) {
            // The checked-conversion detector: one site, one origin, one
            // staged operand. The failure form spells the status helper
            // writing the converted result; the plain shape spells the
            // terminal numeric_cast, which contains its failure by
            // terminating at the site.
            if (!instruction.result ||
                instruction.definedFailure.localOrigins.size() != 1 ||
                program_.failureMetadata().findSite(
                    instruction.localFailureSites.front()) == nullptr ||
                instruction.operands.size() != 1 ||
                !valueOperand(instruction.operands.front()) ||
                !typeRow(instruction.info.type)) {
              {
                if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                  std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 99,
                               gtiProbeTraceKind, gtiProbeTraceOwner,
                               gtiProbeTraceForm);
                }
                return false;
              }
            }
            continue;
          }
          if (instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner) {
            // The unique-owner allocation spells the backend helper over
            // the pack's flattened parameters and contains its failure
            // terminally inside the helper, exactly like the
            // compatibility call site.
            const MirValue *packValue =
                instruction.operands.size() == 1 &&
                        valueOperand(instruction.operands.front())
                    ? body.findValue(instruction.operands.front().value)
                    : nullptr;
            if (packValue == nullptr || !instruction.result ||
                instruction.receiver || instruction.functionTarget ||
                packExpansionForwardedOnce(body, packValue->id) == nullptr ||
                instruction.info.type.kind != SemanticType::UniqueOwner ||
                instruction.info.type.arguments.size() != 1 ||
                !typeRow(instruction.info.type) ||
                !typeRow(instruction.info.type.arguments.front())) {
              {
                if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                  std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n",
                               137, gtiProbeTraceKind, gtiProbeTraceOwner,
                               gtiProbeTraceForm);
                }
                return false;
              }
            }
            continue;
          }
          // An arithmetic intrinsic call names no body: it spells directly
          // as the shipped helper over its two staged scalar operands.
          if (instruction.functionTarget ||
              !scalarSpellableArithmeticIntrinsic(instruction.intrinsic) ||
              !instruction.result || instruction.operands.size() != 2 ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), valueOperand)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 100,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
          continue;
        }
        if (!instruction.functionTarget ||
            !bodyRow(*instruction.functionTarget)) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 101,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        for (const MirOperand &operand : instruction.operands) {
          if (const MirInstruction *staged =
                  borrowStagedCallInput(body, operand)) {
            // A borrow-staged argument passes the place as a C++ lvalue;
            // the callee's reference parameter binds it directly.
            consumedStaged.push_back(*staged->result);
            continue;
          }
          if (!valueOperand(operand)) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 102,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
        }
        if (const MirFunctionInstance *target =
                program_.findFunctionInstance(*instruction.functionTarget);
            target != nullptr && target->linkage == LanguageLinkage::C) {
          // The C boundary takes ::gti_c_string_view: a view argument is
          // marshalled through the shipped converter, but no reverse
          // converter is modelled, so a view result stays outside the
          // vocabulary.
          if (instruction.info.type.kind == SemanticType::StringView) {
            {
              if (::getenv("GTI_PROBE_TRACE") != nullptr) {
                std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 103,
                             gtiProbeTraceKind, gtiProbeTraceOwner,
                             gtiProbeTraceForm);
              }
              return false;
            }
          }
        }
        // Every staged borrow in flight must feed exactly this call: a
        // borrowed place must not outlive its block or bypass a call that
        // could observe or invalidate it.
        // Each call consumes exactly its own staged inputs; borrows
        // staged for a later call in the same block stay pending — the
        // window rule polices what may sit between, and the block-end
        // check still demands the set drains to empty. A consumed borrow
        // staged in the unique invoke-predecessor block is equally
        // exact: the predecessor's block-end rule proved the hand-off.
        if (!std::all_of(consumedStaged.begin(), consumedStaged.end(),
                         [&](MirValueId id) {
                           return std::find(pendingStaged.begin(),
                                            pendingStaged.end(),
                                            id) != pendingStaged.end() ||
                                  stagedInInvokePredecessor(id);
                         })) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 104,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        for (const MirValueId id : consumedStaged) {
          pendingStaged.erase(
              std::remove(pendingStaged.begin(), pendingStaged.end(), id),
              pendingStaged.end());
        }
        continue;
      }
      default: {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 105,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
      }
    }
    if (!pendingStaged.empty() &&
        block.terminator.kind == MirTerminatorKind::Invoke) {
      // A staged borrow may hand off across the invoke edge when every
      // leftover's consuming call sits in the success target and the
      // else path never references the staged places.
      bool handsOff = true;
      for (const MirValueId id : pendingStaged) {
        bool consumedInTarget = false;
        for (const MirValueUse &use : body.usesOf(id)) {
          // A staged borrow hands off whether the success target consumes
          // it as a call receiver or as a staged argument value (the
          // publication construct's operands arrive this way).
          if ((use.kind == MirValueUseKind::InstructionReceiver ||
               use.kind == MirValueUseKind::InstructionOperand) &&
              use.block == block.terminator.target) {
            consumedInTarget = true;
          }
        }
        if (!consumedInTarget) {
          handsOff = false;
        }
        const MirValue *record = body.findValue(id);
        const MirInstruction *stage =
            record == nullptr ? nullptr
                              : findInstruction(body, record->definition);
        const MirPlace *stagedPlace =
            stage != nullptr && !stage->operands.empty()
                ? body.findPlace(stage->operands.front().place)
                : nullptr;
        if (stagedPlace == nullptr) {
          handsOff = false;
          continue;
        }
        for (const MirBlock &candidate : body.blocks) {
          if (candidate.id != block.terminator.elseTarget) {
            continue;
          }
          for (const MirInstruction &member : candidate.instructions) {
            if (member.kind == MirInstructionKind::Drop) {
              continue;
            }
            if ((member.destination &&
                 *member.destination == stagedPlace->id) ||
                (member.receiver &&
                 member.receiver->place == stagedPlace->id)) {
              handsOff = false;
            }
            for (const MirOperand &operand : member.operands) {
              if (operand.place == stagedPlace->id) {
                handsOff = false;
              }
            }
          }
        }
      }
      if (handsOff) {
        pendingStaged.clear();
      }
    }
    if (!pendingStaged.empty()) {
      // A staged borrow must be consumed by a call in its own block.
      {
        if (::getenv("GTI_PROBE_TRACE") != nullptr) {
          std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 106,
                       gtiProbeTraceKind, gtiProbeTraceOwner,
                       gtiProbeTraceForm);
        }
        return false;
      }
    }
    switch (block.terminator.kind) {
    case MirTerminatorKind::Invoke: {
      if (block.terminator.target == 0 || block.terminator.elseTarget == 0) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 107,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      // The invoke's producer must be this block's checked detector (the
      // status local and record write are spellable) or its transformed
      // call (the callee wrote the record; the edge branches on the call's
      // success bool).
      const MirInstruction *producer = nullptr;
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.id == block.terminator.invokeInstruction) {
          producer = &instruction;
        }
      }
      if (producer == nullptr) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 108,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      if (callableValueInvocation(*producer) ||
          terminallyContainedPlainCallee(program_, representations_,
                                         *producer) ||
          terminallyContainedMemberCallee(program_, representations_,
                                          *producer) ||
          storageBoundsCheckCall(*producer)) {
        // The fused literal, terminally-contained plain callee, or
        // terminal logical-size check contains its failure; the edge is a
        // plain goto and the else block never runs.
        continue;
      }
      if (!failureForm) {
        // Plain shape: a Lambda's terminal checked compute, or a template
        // body's terminally-contained plain call, produce invokes whose
        // helpers never return on failure.
        if (callableTemplateBody &&
            producer->kind == MirInstructionKind::Call) {
          continue;
        }
        if (producer->kind == MirInstructionKind::Construct &&
            producer->localFailureSites.empty() &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::Constructor) {
          // A propagating construction terminates at its own site on every
          // shipped path — the constructor failure ABI does not exist yet —
          // so the else edge is dead in the plain shape exactly as it is in
          // the failure form below.
          continue;
        }
        if (checkedConversionIntrinsicCall(*producer)) {
          // The plain shape spells the terminal numeric_cast, which never
          // returns on failure, so the else edge is dead.
          continue;
        }
        if (producer->intrinsic == IntrinsicKind::ExpectedValue ||
            producer->intrinsic == IntrinsicKind::ExpectedError) {
          // The expected extraction's spelled member contains the
          // wrong-state failure terminally; the else edge is dead.
          continue;
        }
        if (address.kind != MirBodyKind::Lambda ||
            producer->kind != MirInstructionKind::Compute ||
            cppMirTerminalCheckedHelperSpelling(producer->operation).empty() ||
            producer->localFailureSites.size() != 1) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 109,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      }
      if (producer->kind == MirInstructionKind::Construct &&
          producer->localFailureSites.empty() &&
          producer->definedFailure.propagation ==
              FailurePropagationKind::Constructor) {
        // A propagating construction terminates at its own site on every
        // shipped path — the constructor failure ABI does not exist yet —
        // so the invoke's else edge is unreachable and the edge is a
        // plain goto, exactly like the analysis's transparent exemption.
        continue;
      }
      if (producer->kind == MirInstructionKind::Compute &&
          producer->operation == MirOperation::Index) {
        // The value-level view element read spells the terminal
        // string_view_at helper on both forms; it never returns on
        // failure, so the else edge is dead in the failure form too.
        continue;
      }
      if (producer->kind == MirInstructionKind::Call &&
          (producer->intrinsic == IntrinsicKind::ExpectedValue ||
           producer->intrinsic == IntrinsicKind::ExpectedError)) {
        // The expected extraction's spelled member contains the
        // wrong-state failure terminally on both forms.
        continue;
      }
      if (producer->kind == MirInstructionKind::Call) {
        if (prefixStorageIntrinsic(producer->intrinsic) &&
            !producer->localFailureSites.empty()) {
          // The storage detector's own status local carries the fired
          // outcome; the edge branches on it like any checked detector.
          continue;
        }
        if (prefixStorageIntrinsic(producer->intrinsic) &&
            producer->localFailureSites.empty() &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::None) {
          // A discharged storage read carries no site: flow analysis
          // proved the bound, so the invoke's else edge is unreachable
          // and the edge is a plain goto, exactly like the analysis's
          // discharged-read exemption.
          continue;
        }
        if (checkedConversionIntrinsicCall(*producer)) {
          continue;
        }
        const MirFunctionInstance *target =
            producer->functionTarget
                ? program_.findFunctionInstance(*producer->functionTarget)
                : nullptr;
        if (target == nullptr || !target->mayRaiseDefinedFailure) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 110,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      }
      if ((producer->kind == MirInstructionKind::Load ||
           producer->kind == MirInstructionKind::Assign) &&
          producer->localFailureSites.size() == 1) {
        continue;
      }
      if (producer->kind != MirInstructionKind::Compute ||
          cppMirCheckedOperationHelperSpelling(producer->operation).empty() ||
          producer->localFailureSites.size() != 1) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 111,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    case MirTerminatorKind::PropagateFailure:
      if (block.terminator.failureRecord == 0) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 112,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      if (!failureForm && address.kind != MirBodyKind::Lambda &&
          !callableTemplateBody) {
        // A plain shape admits this terminator only with an unreachability
        // proof: walk from the entry following every edge except an
        // invoke's else edge (every invoke producer this probe admits in
        // the plain shape terminates at its own site on failure, so that
        // edge is dead), and require the walk never lands here. The block
        // then spells abort.
        bool reachable = false;
        std::vector<MirBlockId> stack{body.entry};
        std::vector<MirBlockId> seen;
        while (!stack.empty()) {
          const MirBlockId id = stack.back();
          stack.pop_back();
          if (std::find(seen.begin(), seen.end(), id) != seen.end()) {
            continue;
          }
          seen.push_back(id);
          if (id == block.id) {
            reachable = true;
            break;
          }
          for (const MirBlock &candidate : body.blocks) {
            if (candidate.id != id) {
              continue;
            }
            const MirTerminator &edge = candidate.terminator;
            if (edge.kind == MirTerminatorKind::Invoke) {
              stack.push_back(edge.target);
              continue;
            }
            if (edge.kind == MirTerminatorKind::Return ||
                edge.kind == MirTerminatorKind::Exit ||
                edge.kind == MirTerminatorKind::Unreachable ||
                edge.kind == MirTerminatorKind::PropagateFailure) {
              continue;
            }
            if (edge.target != 0) {
              stack.push_back(edge.target);
            }
            if (edge.elseTarget != 0) {
              stack.push_back(edge.elseTarget);
            }
            for (const MirSwitchTarget &target : edge.switchTargets) {
              stack.push_back(target.target);
            }
          }
        }
        if (reachable) {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 113,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    case MirTerminatorKind::Goto:
    case MirTerminatorKind::Unreachable:
      continue;
    case MirTerminatorKind::Branch:
      if (!block.terminator.value || !valueOperand(*block.terminator.value)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 114,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    case MirTerminatorKind::Switch: {
      if (!block.terminator.value || !valueOperand(*block.terminator.value)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 115,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      bool targets = true;
      for (const MirSwitchTarget &target : block.terminator.switchTargets) {
        targets = targets && target.value && typeRow(target.value->type);
      }
      if (!targets) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 116,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    }
    case MirTerminatorKind::Return:
      if (block.terminator.returnLoan && *block.terminator.returnLoan != 0) {
        if (loanById(*block.terminator.returnLoan) == nullptr) {
          {
            if (::getenv("GTI_PROBE_TRACE") != nullptr) {
              std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 117,
                           gtiProbeTraceKind, gtiProbeTraceOwner,
                           gtiProbeTraceForm);
            }
            return false;
          }
        }
        continue;
      }
      if (block.terminator.value && !valueOperand(*block.terminator.value)) {
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 118,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      if (failureForm && block.terminator.value &&
          block.terminator.value->type.kind == SemanticType::Class &&
          returnConstructDefinition(body, block.terminator.value->value) ==
              nullptr &&
          returnMoveDefinition(body, block.terminator.value->value) ==
              nullptr) {
        // A class value publishes only through its paired inline
        // construct; anything else has no local to spell.
        {
          if (::getenv("GTI_PROBE_TRACE") != nullptr) {
            std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 119,
                         gtiProbeTraceKind, gtiProbeTraceOwner,
                         gtiProbeTraceForm);
          }
          return false;
        }
      }
      continue;
    default: {
      if (::getenv("GTI_PROBE_TRACE") != nullptr) {
        std::fprintf(stderr, "PD id=%d kind=%d owner=%lu ff=%d\n", 120,
                     gtiProbeTraceKind, gtiProbeTraceOwner, gtiProbeTraceForm);
      }
      return false;
    }
    }
  }
  return true;
}

CppMirBodyEmissionText
CppMirBodyEmitter::emitBodyText(MirBodyAddress address,
                                std::string_view familyLabel,
                                std::size_t indentation) const {
  CppMirBodyEmissionText result;
  result.analysis = analyze(address);
  if (!result.analysis.ready()) {
    return result;
  }
  switch (address.kind) {
  case MirBodyKind::Function: {
    const MirFunctionInstance *function =
        program_.findFunctionInstance(address.owner);
    if (function == nullptr) {
      throw std::logic_error(
          "general MIR body text emission lost its exact function instance");
    }
    result.text = ScalarBodyTextEmitter(program_, representations_, indentation)
                      .emit(*function, familyLabel);
    return result;
  }
  case MirBodyKind::Destructor: {
    const MirDestructorInstance *destructor =
        program_.findDestructorInstance(address.owner);
    if (destructor == nullptr) {
      throw std::logic_error(
          "general MIR body text emission lost its exact destructor instance");
    }
    result.text = ScalarBodyTextEmitter(program_, representations_, indentation)
                      .emit(*destructor, familyLabel);
    return result;
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *constructor =
        program_.findConstructorInstance(address.owner);
    if (constructor == nullptr) {
      throw std::logic_error(
          "general MIR body text emission lost its exact constructor "
          "instance");
    }
    result.text = ScalarBodyTextEmitter(program_, representations_, indentation)
                      .emit(*constructor, familyLabel);
    return result;
  }
  default:
    throw std::logic_error("general MIR body text emission supports function, "
                           "constructor, and destructor bodies only");
  }
}

CppMirBodyEmissionText
CppMirBodyEmitter::emitFailureBodyText(MirBodyAddress address,
                                       std::string_view familyLabel,
                                       std::size_t indentation) const {
  CppMirBodyEmissionText result;
  result.analysis = analyze(address);
  if (!result.analysis.ready()) {
    return result;
  }
  if (address.kind != MirBodyKind::Function) {
    throw std::logic_error(
        "failure-form MIR body text emission supports function bodies only");
  }
  const MirFunctionInstance *function =
      program_.findFunctionInstance(address.owner);
  if (function == nullptr) {
    throw std::logic_error(
        "failure-form MIR body text emission lost its function instance");
  }
  result.text =
      ScalarBodyTextEmitter(program_, representations_, indentation, true)
          .emit(*function, familyLabel);
  return result;
}

CppMirInitializerScheduleText
CppMirBodyEmitter::initializerSchedule(MirBodyAddress address) const {
  CppMirInitializerScheduleText result;
  result.analysis = analyze(address);
  if (!result.analysis.ready()) {
    return result;
  }
  const MirBody *body = nullptr;
  switch (address.kind) {
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers: {
    const MirClassInstance *owner = program_.findClassInstance(address.owner);
    if (owner == nullptr) {
      return result;
    }
    body = address.kind == MirBodyKind::FieldInitializers
               ? &owner->fieldInitializers
               : &owner->staticFieldInitializers;
    break;
  }
  case MirBodyKind::Module:
    body = &program_.module();
    break;
  default:
    return result;
  }
  // The schedule is the straight-line chain of blocks from the entry:
  // literal materialization, terminally-contained checked computes (the
  // compatibility helper family that reports and never returns on
  // failure, so each Invoke edge is sequential and its else block is
  // unreachable), per-field Initialize stages, and lifecycle boundaries.
  // Storage reads and every other shape stay with the compatibility
  // route.
  if (body->blocks.empty() || body->entry != 1 || !body->loans.empty() ||
      !body->dropObligations.empty() || !body->cleanupBoundaries.empty()) {
    return result;
  }
  std::vector<const MirInstruction *> schedule;
  {
    const MirBlock *block = &body->blocks.front();
    std::size_t visited = 0;
    for (;;) {
      if (++visited > body->blocks.size()) {
        return result;
      }
      for (const MirInstruction &instruction : block->instructions) {
        schedule.push_back(&instruction);
      }
      if (block->terminator.kind == MirTerminatorKind::Exit) {
        break;
      }
      // Program-initialization units chain by plain Goto; checked steps
      // chain by Invoke over a terminally-contained compute.
      if (block->terminator.kind == MirTerminatorKind::Goto) {
        const MirBlock *next = nullptr;
        for (const MirBlock &candidate : body->blocks) {
          if (candidate.id == block->terminator.target) {
            next = &candidate;
          }
        }
        if (next == nullptr) {
          return result;
        }
        block = next;
        continue;
      }
      if (block->terminator.kind != MirTerminatorKind::Invoke) {
        return result;
      }
      // The invoke's producer must be this block's terminally-contained
      // checked compute; its else target must be an empty propagate
      // block, which the helper's own containment makes unreachable.
      const MirInstruction *producer = nullptr;
      for (const MirInstruction &instruction : block->instructions) {
        if (instruction.id == block->terminator.invokeInstruction) {
          producer = &instruction;
        }
      }
      const MirBlock *elseBlock = nullptr;
      const MirBlock *next = nullptr;
      for (const MirBlock &candidate : body->blocks) {
        if (candidate.id == block->terminator.elseTarget) {
          elseBlock = &candidate;
        }
        if (candidate.id == block->terminator.target) {
          next = &candidate;
        }
      }
      if (producer == nullptr ||
          producer->kind != MirInstructionKind::Compute ||
          cppMirTerminalCheckedHelperSpelling(producer->operation).empty() ||
          producer->localFailureSites.size() != 1 || next == nullptr ||
          elseBlock == nullptr || !elseBlock->instructions.empty() ||
          elseBlock->terminator.kind != MirTerminatorKind::PropagateFailure) {
        return result;
      }
      block = next;
    }
  }
  ScalarBodyTextEmitter writer(program_, representations_, 0);
  std::unordered_map<MirValueId, std::string> spellings;
  const auto integralKind = [](const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Char:
      return true;
    default:
      return false;
    }
  };
  for (const MirInstruction *instruction : schedule) {
    switch (instruction->kind) {
    case MirInstructionKind::Lifecycle:
      continue;
    case MirInstructionKind::Compute: {
      if (!instruction->result) {
        return result;
      }
      if (instruction->operation == MirOperation::Literal) {
        if (!instruction->literal || !instruction->operands.empty() ||
            !instruction->localFailureSites.empty() ||
            !ScalarBodyTextEmitter::spellableLiteral(*instruction->literal,
                                                     instruction->info.type)) {
          return result;
        }
        spellings.emplace(*instruction->result,
                          writer.literalSpelling(*instruction->literal,
                                                 instruction->info.type));
        continue;
      }
      // A terminally-contained checked compute spells the compatibility
      // helper over its already-spelled operands, exactly as the
      // compatibility in-class initializer does.
      const std::string_view helper =
          cppMirTerminalCheckedHelperSpelling(instruction->operation);
      if (helper.empty() || instruction->localFailureSites.size() != 1 ||
          instruction->operands.empty() || instruction->operands.size() > 2 ||
          !integralKind(instruction->info.type)) {
        return result;
      }
      std::string spelled(helper);
      spelled += '(';
      for (std::size_t index = 0; index < instruction->operands.size();
           ++index) {
        const MirOperand &operand = instruction->operands[index];
        const auto found = operand.kind == MirOperandKind::Value
                               ? spellings.find(operand.value)
                               : spellings.end();
        if (found == spellings.end() || !integralKind(operand.type)) {
          return result;
        }
        if (index != 0) {
          spelled += ", ";
        }
        spelled += found->second;
      }
      spelled += ')';
      spellings.emplace(*instruction->result, std::move(spelled));
      continue;
    }
    case MirInstructionKind::Initialize: {
      const MirPlace *destination =
          instruction->destination ? body->findPlace(*instruction->destination)
                                   : nullptr;
      if (destination == nullptr ||
          destination->root != MirPlaceRootKind::Binding ||
          !destination->projections.empty() || destination->symbol == 0 ||
          !instruction->localFailureSites.empty() ||
          instruction->operands.size() > 1) {
        return result;
      }
      if (instruction->operands.empty()) {
        // The bare default: the field carries no in-class initializer text.
        result.fields.push_back({.field = destination->symbol});
        continue;
      }
      const MirOperand &operand = instruction->operands.front();
      if (operand.kind == MirOperandKind::Constant && operand.literal &&
          ScalarBodyTextEmitter::spellableLiteral(*operand.literal,
                                                  operand.type)) {
        // A frontend-evaluated constant carries its literal on the
        // operand itself.
        result.fields.push_back({.field = destination->symbol,
                                 .spelling = writer.literalSpelling(
                                     *operand.literal, operand.type)});
        continue;
      }
      const auto spelled = operand.kind == MirOperandKind::Value
                               ? spellings.find(operand.value)
                               : spellings.end();
      if (spelled == spellings.end()) {
        return result;
      }
      result.fields.push_back(
          {.field = destination->symbol, .spelling = spelled->second});
      continue;
    }
    default:
      return result;
    }
  }
  result.supported = true;
  return result;
}

CppMirBodyEmissionAnalysis
CppMirBodyEmitter::analyze(MirBodyAddress address) const {
  return BodyAnalysisBuilder(program_, representations_, address).run(true);
}

CppMirProgramEmissionAnalysis CppMirBodyEmitter::analyzeProgram() const {
  CppMirProgramEmissionAnalysis analysis;
  analysis.readiness = CppMirBodyEmissionReadiness::Ready;

  const std::vector<MirBodyAddress> addresses =
      enumerateMirBodyAddresses(program_);
  if (addresses.empty()) {
    analysis.readiness = CppMirBodyEmissionReadiness::Incoherent;
    analysis.issues.push_back(
        {.kind = CppMirBodyEmissionIssueKind::InvalidMirProgram,
         .detail = "core MIR body inventory is empty"});
    return analysis;
  }

  BodyAnalysisBuilder validation(program_, representations_, addresses.front());
  validation.validateProgram();
  validation.validateRepresentations();
  CppMirBodyEmissionAnalysis validationResult = validation.finishValidation();
  analysis.readiness =
      mergeReadiness(analysis.readiness, validationResult.readiness);
  analysis.issues = validationResult.issues;

  analysis.bodies.reserve(addresses.size());
  for (const MirBodyAddress address : addresses) {
    CppMirBodyEmissionAnalysis body =
        BodyAnalysisBuilder(program_, representations_, address).run(false);
    analysis.readiness = mergeReadiness(analysis.readiness, body.readiness);
    analysis.bodies.push_back(std::move(body));
  }
  return analysis;
}

} // namespace lang
