#include "mir_verification_queries.h"
#include "gti/mir_dominance.h"
#include "gti/borrow_conditions.h"
#include "gti/lifetime_types.h"
#include "gti/mir_effects.h"
#include "mir_verification_support.h"
#include "mir_lifetimes.h"

#include <algorithm>
#include <climits>
#include <unordered_map>
#include <unordered_set>

namespace lang::mir_detail {

struct MirBodyReadScope::Index {
  const MirBody *body;
  std::vector<const MirValue *> sources;
  std::vector<std::pair<const MirBlock *, const MirInstruction *>> instructions;
  bool uniqueInstructions = true;
  explicit Index(const MirBody &value) : body(&value) {
    sources.reserve(value.values.size());
    for (const auto &item : value.values)
      sources.push_back(&item);
    std::stable_sort(sources.begin(), sources.end(), [](auto left, auto right) {
      return left->sourceValue < right->sourceValue;
    });
    instructions.reserve(value.instructionCount());
    for (const auto &block : value.blocks)
      for (const auto &item : block.instructions)
        instructions.emplace_back(&block, &item);
    std::sort(instructions.begin(), instructions.end(),
              [](auto left, auto right) {
                return left.second->id < right.second->id;
              });
    uniqueInstructions =
        std::adjacent_find(instructions.begin(), instructions.end(),
                           [](auto left, auto right) {
                             return left.second->id == right.second->id;
                           }) == instructions.end();
  }
};
thread_local const MirBodyReadScope::Index *MirBodyReadScope::current = nullptr;
MirBodyReadScope::MirBodyReadScope(const MirBody &body) : previous(current) {
  if (!current || current->body != &body) {
    owned = std::make_unique<Index>(body);
    current = owned.get();
  }
}
MirBodyReadScope::~MirBodyReadScope() { current = previous; }
std::optional<std::span<const MirValue *const>>
indexedSourceValues(const MirBody &body, HirValueId source) {
  const auto *index = MirBodyReadScope::current;
  if (!index || index->body != &body)
    return std::nullopt;
  const auto first =
      std::lower_bound(index->sources.begin(), index->sources.end(), source,
                       [](const MirValue *value, HirValueId id) {
                         return value->sourceValue < id;
                       });
  const auto last = std::upper_bound(first, index->sources.end(), source,
                                     [](HirValueId id, const MirValue *value) {
                                       return id < value->sourceValue;
                                     });
  return std::span<const MirValue *const>(first, last);
}
std::optional<const MirInstruction *> indexedInstruction(const MirBody &body,
                                                         const MirBlock &block,
                                                         MirInstructionId id) {
  const auto *index = MirBodyReadScope::current;
  if (!index || index->body != &body || !index->uniqueInstructions)
    return std::nullopt;
  const auto found =
      std::lower_bound(index->instructions.begin(), index->instructions.end(),
                       id, [](const auto &entry, MirInstructionId value) {
                         return entry.second->id < value;
                       });
  return found == index->instructions.end() || found->second->id != id ||
                 found->first != &block
             ? nullptr
             : found->second;
}

bool nullGuardedDestructor(const MirProgram &program,
                           const MirClassInstance &owner) {
  if (!owner.inactiveDropPointerField || !owner.requiresActiveDropState ||
      !owner.destructor || owner.kind == ClassKind::Union ||
      owner.kind == ClassKind::Interface || owner.polymorphic ||
      !owner.bases.empty() || !owner.structuralBases.empty() ||
      owner.cAbiRecord || owner.cOpaqueHandle || owner.unionLayout ||
      owner.compilerCapability != CompilerCapabilityTypeKind::None ||
      owner.declaredFields.empty())
    return false;
  bool found = false;
  for (const auto &field : owner.declaredFields) {
    if (field.type.kind != SemanticType::RawPointer ||
        field.requiresActiveCleanup)
      return false;
    if (field.symbol == *owner.inactiveDropPointerField) {
      if (found)
        return false;
      found = true;
    }
  }
  const auto *destructor = program.findDestructorInstance(*owner.destructor);
  if (!found || !destructor || destructor->owner != owner.id)
    return false;
  const auto &body = destructor->body;
  enum class Fact { Unknown, Null, False, True };
  std::unordered_map<MirValueId, Fact> values;
  const auto literal = [](const Literal &value) {
    if (std::holds_alternative<std::nullptr_t>(value))
      return Fact::Null;
    if (const auto *boolean = std::get_if<bool>(&value))
      return *boolean ? Fact::True : Fact::False;
    return Fact::Unknown;
  };
  const auto operand = [&](const MirOperand &input) {
    if (input.literal)
      return literal(*input.literal);
    const auto it = values.find(input.value);
    return it == values.end() ? Fact::Unknown : it->second;
  };
  // The null path must terminate without touching any other storage, running
  // user code, changing ownership or raising failure. Cycles and unknown
  // control fail closed; this is a proof of a path, not a second interpreter.
  std::unordered_set<MirBlockId> visited;
  auto next = body.entry;
  while (visited.insert(next).second) {
    const auto *block = body.findBlock(next);
    if (!block || !block->parameters.empty())
      return false;
    for (const auto &row : block->instructions) {
      if (row.tombstone)
        continue;
      if (!row.lifecycle.empty())
        return false;
      Fact result = Fact::Unknown;
      if (row.kind == MirInstructionKind::Lifecycle && !row.result &&
          row.operands.empty())
        continue;
      if (row.kind == MirInstructionKind::Load && row.operands.size() == 1) {
        const auto &input = row.operands.front();
        const auto *place = body.findPlace(input.place);
        if (input.kind != MirOperandKind::Copy || !place ||
            place->root != MirPlaceRootKind::This ||
            place->projections.size() != 1 ||
            place->projections.front().kind != MirProjectionKind::Field ||
            place->projections.front().field !=
                *owner.inactiveDropPointerField ||
            place->type.kind != SemanticType::RawPointer)
          return false;
        result = Fact::Null;
      } else if (row.kind == MirInstructionKind::Compute &&
                 row.operation == MirOperation::Literal && row.literal &&
                 row.operands.empty()) {
        result = literal(*row.literal);
      } else if (row.kind == MirInstructionKind::Compute &&
                 (row.operation == MirOperation::Equal ||
                  row.operation == MirOperation::NotEqual) &&
                 row.operands.size() == 2 &&
                 operand(row.operands[0]) == Fact::Null &&
                 operand(row.operands[1]) == Fact::Null) {
        result =
            row.operation == MirOperation::Equal ? Fact::True : Fact::False;
      } else if (row.kind == MirInstructionKind::Compute &&
                 row.operation == MirOperation::Identity &&
                 row.operands.size() == 1) {
        result = operand(row.operands.front());
      } else {
        return false;
      }
      if (!row.result)
        return false;
      values[*row.result] = result;
    }
    const auto &term = block->terminator;
    if (!term.targetArguments.empty() || !term.elseArguments.empty() ||
        !term.successLifecycle.empty())
      return false;
    if (term.kind == MirTerminatorKind::Return)
      return !term.value && !term.returnLoan;
    if (term.kind == MirTerminatorKind::Goto) {
      next = term.target;
    } else if (term.kind == MirTerminatorKind::Branch && term.value) {
      const auto fact = operand(*term.value);
      if (fact != Fact::True && fact != Fact::False)
        return false;
      next = fact == Fact::True ? term.target : term.elseTarget;
    } else {
      return false;
    }
  }
  return false;
}

const MirInstruction *baseConstructorPublicationStage(const MirBody &body,
                                                      const MirLoan &loan) {
  if (body.kind != MirBodyKind::Constructor || loan.kind != MirLoanKind::Stored ||
      !loan.escapes || loan.entry || loan.producedBy || loan.storedDependency ||
      loan.storedPath.size() != 1 || !isWellFormedBorrowPath(loan.storedPath) ||
      loan.storedPath.front().kind != PlaceProjectionKind::Base)
    return nullptr;
  const MirInstruction *result = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.tombstone || instruction.loan != loan.id)
        continue;
      // Retirement is an endpoint, not a second producer. The independent
      // CFG/lifecycle proof still checks its reachability and exact ordering.
      if (instruction.kind == MirInstructionKind::EndBorrow)
        continue;
      const auto *destination = instruction.destination
          ? body.findPlace(*instruction.destination) : nullptr;
      if (result || instruction.kind != MirInstructionKind::InitializeBase ||
          !instruction.constructorInitializer || !instruction.constructorTarget ||
          !destination || destination->root != MirPlaceRootKind::This ||
          destination->projections.size() != 1 ||
          destination->projections.front().kind != MirProjectionKind::Base ||
          destination->projections.front().constantIndex != loan.storedPath.front().index ||
          !destination->projections.front().selection)
        return nullptr;
      result = &instruction;
    }
  return result;
}

std::optional<MirBorrowPathView> walkBorrowPath(
    const MirProgram &program, const SemanticType &root, AccessMode rootAccess,
    std::span<const PlaceProjection> path,
    std::span<const BorrowDependencyCondition> guards, MirBorrowPathRoot rootRole) {
  if (path.size() > 256 || guards.size() > 256 ||
      !isWellFormedBorrowPath(path) || root == SemanticType::Unknown ||
      root == SemanticType::Void ||
      (rootRole != MirBorrowPathRoot::Value &&
       rootRole != MirBorrowPathRoot::PhysicalReference) ||
      (rootRole == MirBorrowPathRoot::PhysicalReference) !=
          (root.kind == SemanticType::Reference))
    return std::nullopt;
  for (std::size_t index = 0; index < guards.size(); ++index) {
    const auto &guard = guards[index];
    if (!isWellFormedBorrowPath(guard.subobject) ||
        guard.subobject.size() > path.size() ||
        !std::equal(guard.subobject.begin(), guard.subobject.end(), path.begin()) ||
        std::any_of(guards.begin(), guards.begin() + index, [&](const auto &other) {
          return other.subobject == guard.subobject;
        }))
      return std::nullopt;
  }
  MirBorrowPathView result{.type = root, .referenceAccessLimit = rootAccess};
  std::size_t work = 0;
  const auto dereference = [&](std::size_t pathOffset, bool physicalRoot) {
    while (result.type.kind == SemanticType::Reference) {
      if (++work > 512 || result.type.arguments.size() != 1)
        return false;
      result.referenceCrossings.push_back(
          {.pathOffset = pathOffset,
           .projectionOffset = result.projections.size(),
           .reference = result.type,
           .physicalRoot = physicalRoot});
      if (result.type.referenceAccess == AccessMode::ReadOnly)
        result.referenceAccessLimit = AccessMode::ReadOnly;
      const SemanticType referent = result.type.arguments.front();
      result.type = referent;
      result.projections.push_back({.kind = MirProjectionKind::Dereference});
      physicalRoot = false;
    }
    return result.type != SemanticType::Unknown && result.type != SemanticType::Void;
  };
  if (!dereference(0, rootRole == MirBorrowPathRoot::PhysicalReference))
    return std::nullopt;
  for (std::size_t offset = 0; offset <= path.size(); ++offset) {
    if (++work > 512)
      return std::nullopt;
    const auto guard = std::find_if(guards.begin(), guards.end(), [&](const auto &item) {
      return item.subobject.size() == offset;
    });
    std::optional<MirEnumLifetimeView> enumView;
    if (result.type.kind == SemanticType::Enum) {
      enumView = mirEnumLifetimeView(program, result.type);
      if (!enumView)
        return std::nullopt;
    }
    if (guard != guards.end() &&
        (guard->enumeration != result.type || !enumView || !enumView->payload ||
         guard->variant >= enumView->payload->variants.size() ||
         enumView->payload->variants[guard->variant].variantIndex != guard->variant))
      return std::nullopt;
    if (offset == path.size()) {
      if ((result.type.kind == SemanticType::Class && !mirClassLifetimeView(program, result.type)) ||
          (result.type.kind == SemanticType::Lambda && !mirLambdaCaptureTypes(program, result.type)) ||
          result.type.kind == SemanticType::TypeParameter ||
          (result.type.kind == SemanticType::TypePack && !result.type.concretePack))
        return std::nullopt;
      return result;
    }
    const auto &step = path[offset];
    SemanticType child = SemanticType::Unknown;
    MirPlaceProjection projection;
    switch (step.kind) {
    case PlaceProjectionKind::Field: {
      projection = {.kind = MirProjectionKind::Field, .field = step.field};
      if (result.type.kind == SemanticType::Class) {
        const auto owner = mirClassLifetimeView(program, result.type);
        if (!owner)
          return std::nullopt;
        const MirClassFieldInfo *selected = nullptr;
        for (const auto &field : owner->instance->declaredFields)
          if (field.symbol == step.field) {
            if (selected)
              return std::nullopt;
            selected = &field;
          }
        const auto type = selected ? owner->instantiate(selected->type) : std::nullopt;
        if (!type)
          return std::nullopt;
        child = *type;
      } else if (result.type.kind == SemanticType::Enum) {
        if (guard == guards.end() || !enumView->payload)
          return std::nullopt;
        const auto &variant = enumView->payload->variants[guard->variant];
        if (variant.payloadTypes.size() != variant.payloadSymbols.size())
          return std::nullopt;
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < variant.payloadSymbols.size(); ++index)
          if (variant.payloadSymbols[index] == step.field) {
            if (selected)
              return std::nullopt;
            selected = index;
          }
        const auto type = selected ? enumView->instantiate(variant.payloadTypes[*selected]) : std::nullopt;
        if (!type)
          return std::nullopt;
        result.payloadSelections.push_back(
            {.pathOffset = offset, .projectionOffset = result.projections.size(),
             .enumeration = result.type, .variant = guard->variant,
             .payloadIndex = *selected});
        child = *type;
      } else if (result.type.kind == SemanticType::Lambda) {
        std::optional<std::vector<SymbolId>> symbols;
        const auto captureTypes = mirLambdaCaptureTypes(program, result.type);
        if (!captureTypes)
          return std::nullopt;
        for (const auto &instance : program.lambdaInstances()) {
          if (!mirLambdaLifetimeView(instance, result.type))
            continue;
          if (instance.captureSymbols.size() != captureTypes->size() ||
              (symbols && *symbols != instance.captureSymbols))
            return std::nullopt;
          symbols = instance.captureSymbols;
        }
        if (!symbols)
          return std::nullopt;
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < symbols->size(); ++index)
          if ((*symbols)[index] == step.field) {
            if (selected)
              return std::nullopt;
            selected = index;
          }
        if (!selected)
          return std::nullopt;
        child = (*captureTypes)[*selected];
      } else {
        return std::nullopt;
      }
      break;
    }
    case PlaceProjectionKind::Base: {
      if (result.type.kind != SemanticType::Class || result.type.classId != step.baseOwner)
        return std::nullopt;
      const auto owner = mirClassLifetimeView(program, result.type);
      if (!owner || step.index >= owner->instance->structuralBases.size())
        return std::nullopt;
      const auto &base = owner->instance->structuralBases[step.index];
      if (base.interface || !base.instance)
        return std::nullopt;
      const auto type = owner->instantiate(base.type);
      const auto selected = type ? mirClassLifetimeView(program, *type) : std::nullopt;
      if (!selected || selected->instance->id != base.instance ||
          selected->instance->kind == ClassKind::Interface)
        return std::nullopt;
      child = *type;
      projection = {.kind = MirProjectionKind::Base,
                    .constantIndex = step.index, .selection = base.instance};
      break;
    }
    case PlaceProjectionKind::ConstantIndex:
      if (result.type.kind == SemanticType::Array &&
          result.type.arguments.size() == 1 && !result.type.arrayLengthParameterId &&
          step.index < result.type.arrayLength) {
        child = result.type.arguments.front();
        projection = {.kind = MirProjectionKind::Index, .constantIndex = step.index};
      } else if (result.type.kind == SemanticType::TypePack && result.type.concretePack &&
                 step.index < result.type.arguments.size()) {
        child = result.type.arguments[step.index];
        projection = {.kind = MirProjectionKind::PackElement, .constantIndex = step.index};
      } else {
        return std::nullopt;
      }
      break;
    case PlaceProjectionKind::DynamicIndex:
    case PlaceProjectionKind::Dereference:
    default:
      return std::nullopt;
    }
    result.type = std::move(child);
    result.projections.push_back(projection);
    if (!dereference(offset + 1, false))
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::size_t> borrowPathProjectionPrefix(
    std::span<const MirPlaceProjection> projections,
    std::span<const PlaceProjection> path, bool trailingReference) {
  if (!isWellFormedBorrowPath(path))
    return std::nullopt;
  std::size_t offset = 0;
  const auto dereference = [&] {
    if (offset >= projections.size() || projections[offset].kind != MirProjectionKind::Dereference)
      return true;
    const auto &value = projections[offset++];
    return value.field == 0 && value.index == 0 && value.selection == 0 && !value.constantIndex;
  };
  for (const auto &step : path) {
    if (!dereference() || offset >= projections.size())
      return std::nullopt;
    const auto &value = projections[offset++];
    switch (step.kind) {
    case PlaceProjectionKind::Field:
      if (value.kind != MirProjectionKind::Field || value.field != step.field ||
          value.index || value.selection || value.constantIndex)
        return std::nullopt;
      break;
    case PlaceProjectionKind::Base:
      if (value.kind != MirProjectionKind::Base || value.field || value.index ||
          !value.selection || value.constantIndex != step.index)
        return std::nullopt;
      break;
    case PlaceProjectionKind::ConstantIndex:
      if ((value.kind != MirProjectionKind::Index && value.kind != MirProjectionKind::PackElement) ||
          value.field || value.index || value.selection || value.constantIndex != step.index)
        return std::nullopt;
      break;
    default:
      return std::nullopt;
    }
  }
  if (trailingReference && (offset >= projections.size() ||
      projections[offset].kind != MirProjectionKind::Dereference || !dereference()))
    return std::nullopt;
  return offset;
}

std::optional<std::vector<BorrowDependencyCondition>> deriveBorrowPathConditions(
    const MirProgram &program, const SemanticType &root, AccessMode rootAccess,
    std::span<const PlaceProjection> path, MirBorrowPathRoot role) {
  if (!isWellFormedBorrowPath(path) || path.size() > borrowInventoryDepthLimit)
    return std::nullopt;
  std::vector<BorrowDependencyCondition> conditions;
  auto current = walkBorrowPath(program, root, rootAccess, {}, {}, role);
  if (!current)
    return std::nullopt;
  for (std::size_t offset = 0; offset < path.size(); ++offset) {
    if (current->type.kind == SemanticType::Enum) {
      const auto view = mirEnumLifetimeView(program, current->type);
      if (!view || !view->payload || path[offset].kind != PlaceProjectionKind::Field)
        return std::nullopt;
      std::optional<std::size_t> selected;
      for (std::size_t variant = 0; variant < view->payload->variants.size(); ++variant) {
        const auto &candidate = view->payload->variants[variant];
        if (candidate.variantIndex != variant ||
            candidate.payloadTypes.size() != candidate.payloadSymbols.size())
          return std::nullopt;
        for (const auto field : candidate.payloadSymbols)
          if (field == path[offset].field) {
            if (selected)
              return std::nullopt;
            selected = variant;
          }
      }
      if (!selected)
        return std::nullopt;
      conditions.push_back({.subobject = BorrowPath(path.begin(), path.begin() + offset),
                            .enumeration = current->type, .variant = *selected});
    }
    current = walkBorrowPath(program, root, rootAccess, path.first(offset + 1), conditions, role);
    if (!current)
      return std::nullopt;
  }
  return conditions;
}

std::optional<BorrowPath> borrowPathForProjections(
    const MirProgram &program, const SemanticType &root, AccessMode rootAccess,
    std::span<const MirPlaceProjection> projections,
    std::span<const BorrowDependencyCondition> guards,
    std::span<const MirBorrowPathPayloadSelection> selections,
    MirBorrowPathRoot rootRole) {
  if (projections.size() > 512 || guards.size() > 256 || selections.size() > 256)
    return std::nullopt;
  BorrowPath path;
  const auto replay = [&]() -> std::optional<MirBorrowPathView> {
    std::vector<BorrowDependencyCondition> prefixGuards;
    for (const auto &guard : guards)
      if (guard.subobject.size() <= path.size())
        prefixGuards.push_back(guard);
    const auto view = walkBorrowPath(program, root, rootAccess, path, prefixGuards, rootRole);
    if (!view || view->projections.size() > projections.size() ||
        !std::equal(view->projections.begin(), view->projections.end(), projections.begin()))
      return std::nullopt;
    return view;
  };
  auto view = replay();
  if (!view)
    return std::nullopt;
  while (view->projections.size() < projections.size()) {
    if (path.size() == 256)
      return std::nullopt;
    const auto &step = projections[view->projections.size()];
    switch (step.kind) {
    case MirProjectionKind::Field:
      path.push_back({.kind = PlaceProjectionKind::Field, .field = step.field});
      break;
    case MirProjectionKind::Base:
      if (!step.constantIndex || view->type.kind != SemanticType::Class)
        return std::nullopt;
      path.push_back({.kind = PlaceProjectionKind::Base, .index = *step.constantIndex,
                      .baseOwner = view->type.classId});
      break;
    case MirProjectionKind::Index:
    case MirProjectionKind::PackElement:
      if (!step.constantIndex)
        return std::nullopt;
      path.push_back({.kind = PlaceProjectionKind::ConstantIndex, .index = *step.constantIndex});
      break;
    default:
      return std::nullopt;
    }
    view = replay();
    if (!view)
      return std::nullopt;
  }
  const auto complete = walkBorrowPath(program, root, rootAccess, path, guards, rootRole);
  return complete && std::equal(complete->payloadSelections.begin(), complete->payloadSelections.end(),
                                 selections.begin(), selections.end())
             ? std::optional{std::move(path)} : std::nullopt;
}

bool inlineBorrowProjectionsDisjoint(
    const MirProgram &program, SemanticType root,
    std::span<const MirPlaceProjection> left,
    std::span<const MirPlaceProjection> right) {
  if (root.kind == SemanticType::Reference) {
    if (root.arguments.size() != 1 ||
        root.arguments.front().kind == SemanticType::Reference)
      return false;
    root = SemanticType(root.arguments.front());
    const auto referentPath = [](std::span<const MirPlaceProjection> &path) {
      if (!path.empty() && path.front().kind == MirProjectionKind::Dereference) {
        if (path.front() != MirPlaceProjection{.kind = MirProjectionKind::Dereference})
          return false;
        path = path.subspan(1);
      }
      return true;
    };
    if (!referentPath(left) || !referentPath(right))
      return false;
  }
  const auto leftPath = borrowPathForProjections(
      program, root, AccessMode::ReadOnly, left, {}, {}, MirBorrowPathRoot::Value);
  const auto rightPath = borrowPathForProjections(
      program, root, AccessMode::ReadOnly, right, {}, {}, MirBorrowPathRoot::Value);
  if (!leftPath || !rightPath)
    return false;
  return inlineBorrowPathsDisjoint(root, *leftPath, *rightPath,
      [&](const SemanticType &type, const PlaceProjection &step)
          -> std::optional<SemanticType> {
        if (type.kind == SemanticType::Class) {
          const auto owner = mirClassLifetimeView(program, type);
          if (!owner || (owner->instance->kind != ClassKind::Class &&
                         owner->instance->kind != ClassKind::Struct))
            return std::nullopt;
        }
        const auto view = walkBorrowPath(
            program, type, AccessMode::ReadOnly, std::span{&step, std::size_t{1}},
            {}, MirBorrowPathRoot::Value);
        if (!view || !view->referenceCrossings.empty() ||
            !view->payloadSelections.empty())
          return std::nullopt;
        return view->type;
      });
}

const MirPlace *physicalPayloadSelectionSource(
    const MirProgram &program, const MirBody &body,
    const MirReferenceSelection &selection) {
  if (selection.kind != ReferenceSelectionKind::PhysicalPayload ||
      selection.transferredFrom || selection.reference.kind != SemanticType::Reference ||
      selection.reference.arguments.size() != 1)
    return nullptr;
  const auto *value = body.findValue(selection.value);
  const auto *extract = definitionFor(body, selection.value);
  if (!value || !extract || extract->tombstone ||
      extract->kind != MirInstructionKind::Compute ||
      extract->operation != MirOperation::PayloadExtract ||
      extract->payloadExtractionKind != PayloadExtractionKind::BorrowValue ||
      extract->result != selection.value || extract->info != value->info ||
      extract->operands.size() != 1 || !extract->enumOwner ||
      !extract->enumVariant || !extract->payloadIndex)
    return nullptr;
  const auto &operand = extract->operands.front();
  const auto *carrier = body.findPlace(operand.place);
  const auto view = mirEnumLifetimeView(program, operand.type);
  if (!carrier || operand.place != selection.carrier || carrier->foreignStorage ||
      carrier->type != operand.type || !view || !view->payload ||
      extract->enumOwner != view->payload->declaration ||
      *extract->enumVariant >= view->payload->variants.size() ||
      (operand.kind != MirOperandKind::BorrowRead &&
       operand.kind != MirOperandKind::BorrowWrite))
    return nullptr;
  const auto &variant = view->payload->variants[*extract->enumVariant];
  if (*extract->payloadIndex >= variant.payloadTypes.size() ||
      *extract->payloadIndex >= variant.payloadSymbols.size())
    return nullptr;
  const auto field = view->instantiate(variant.payloadTypes[*extract->payloadIndex]);
  if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
    std::fprintf(stderr, "[payload-src] field=%d fieldEq=%d refEq=%d cat=%d valueAccess=%d operandKind=%d refAccess=%d fieldsEq=%d condsEq=%d\n", field.has_value(), field && *field == value->info.type, field && *field == selection.reference.arguments.front(), static_cast<int>(value->info.category), static_cast<int>(value->info.access), static_cast<int>(operand.kind), static_cast<int>(selection.reference.referenceAccess), selection.fields == BorrowPath{fieldProjection(variant.payloadSymbols[*extract->payloadIndex])}, selection.conditions == std::vector<BorrowDependencyCondition>{{.enumeration = operand.type, .variant = *extract->enumVariant}}); // TEMP-TRACE
  if (!field || field->kind == SemanticType::Reference ||
      *field != value->info.type || *field != selection.reference.arguments.front() ||
      value->info.category != ValueCategory::Place ||
      (selection.reference.referenceAccess == AccessMode::Mutable &&
       (value->info.access != AccessMode::Mutable ||
        operand.kind != MirOperandKind::BorrowWrite)) ||
      selection.fields != BorrowPath{fieldProjection(variant.payloadSymbols[*extract->payloadIndex])} ||
      selection.conditions != std::vector<BorrowDependencyCondition>{
          {.enumeration = operand.type, .variant = *extract->enumVariant}})
    return nullptr;
  const MirPlace *result = nullptr;
  for (const auto &place : body.places)
    if (place.root == MirPlaceRootKind::Value && place.value == selection.value &&
        place.projections.empty() && place.type == *field && !place.foreignStorage &&
        (selection.reference.referenceAccess == AccessMode::ReadOnly ||
         place.access == AccessMode::Mutable)) {
      if (result)
        return nullptr;
      result = &place;
    }
  return result;
}

std::optional<SemanticType> storedPayloadSelectionReference(
    const MirProgram &program, const MirBody &body,
    const MirReferenceSelection &selection) {
  if (selection.kind != ReferenceSelectionKind::StoredExternalReference)
    return std::nullopt;
  const auto *value = body.findValue(selection.value);
  const auto *extract = definitionFor(body, selection.value);
  if (!value || !extract || extract->tombstone ||
      extract->kind != MirInstructionKind::Compute ||
      extract->operation != MirOperation::PayloadExtract ||
      !extract->payloadExtractionKind ||
      !isStoredReferenceExtraction(*extract->payloadExtractionKind) ||
      extract->result != selection.value || extract->info != value->info ||
      extract->operands.size() != 1 || !extract->enumOwner ||
      !extract->enumVariant || !extract->payloadIndex)
    return std::nullopt;
  const auto &operand = extract->operands.front();
  const auto *carrier = body.findPlace(operand.place);
  const auto view = mirEnumLifetimeView(program, operand.type);
  if (!carrier || operand.place != selection.carrier || carrier->foreignStorage ||
      carrier->type != operand.type || !view || !view->payload ||
      extract->enumOwner != view->payload->declaration ||
      *extract->enumVariant >= view->payload->variants.size())
    return std::nullopt;
  const auto &variant = view->payload->variants[*extract->enumVariant];
  if (*extract->payloadIndex >= variant.payloadTypes.size() ||
      *extract->payloadIndex >= variant.payloadSymbols.size())
    return std::nullopt;
  const auto field = view->instantiate(variant.payloadTypes[*extract->payloadIndex]);
  if (!field || field->kind != SemanticType::Reference || field->arguments.size() != 1 ||
      *field != selection.reference || field->arguments.front() != value->info.type ||
      value->info.category != ValueCategory::Place ||
      (value->info.access == AccessMode::Mutable && field->referenceAccess != AccessMode::Mutable) ||
      selection.fields != BorrowPath{fieldProjection(variant.payloadSymbols[*extract->payloadIndex])} ||
      selection.conditions != std::vector<BorrowDependencyCondition>{
          {.enumeration = operand.type, .variant = *extract->enumVariant}})
    return std::nullopt;
  return field;
}

std::optional<MirSelectedReferenceHandoff>
selectedReferenceHandoff(const MirBody &body, const MirLoan &selected) {
  if (!selected.referenceSelection || selected.referenceSelection->kind !=
                                         ReferenceSelectionKind::StoredExternalReference ||
      selected.entry || selected.escapes ||
      selected.kind != MirLoanKind::Local)
    return std::nullopt;
  const auto &selection = *selected.referenceSelection;
  const auto *value = body.findValue(selection.value);
  const auto *block = value ? body.findBlock(value->definitionBlock) : nullptr;
  const auto *group = body.findLoan(selection.transferredFrom);
  const auto *carrier = body.findPlace(selection.carrier);
  if (!block || !group || group->kind != MirLoanKind::Stored ||
      group->escapes || !carrier ||
      carrier->root != MirPlaceRootKind::Binding ||
      !carrier->projections.empty() ||
      carrier->type.kind != SemanticType::Enum ||
      std::find(group->carriers.begin(), group->carriers.end(),
                carrier->binding) == group->carriers.end())
    return std::nullopt;
  MirSelectedReferenceHandoff result{
      .block = block->id, .carrier = carrier->id, .group = group->id};
  std::vector<const MirInstruction *> extracts;
  std::vector<const MirLoan *> successors;
  std::size_t position = 0;
  const auto next = [&]() -> const MirInstruction * {
    while (position < block->instructions.size() &&
           block->instructions[position].tombstone)
      ++position;
    return position == block->instructions.size()
               ? nullptr
               : &block->instructions[position];
  };
  while (const auto *instruction = next()) {
    if (instruction->kind != MirInstructionKind::Compute ||
        instruction->operation != MirOperation::PayloadExtract)
      break;
    if (!instruction->result || !instruction->enumOwner ||
        !instruction->enumVariant || !instruction->payloadIndex ||
        *instruction->payloadIndex != extracts.size() ||
        !instruction->payloadExtractionKind ||
        !isConsumingPayloadExtraction(*instruction->payloadExtractionKind) ||
        instruction->operands.size() != 1 ||
        instruction->operands.front().kind != MirOperandKind::Move ||
        instruction->operands.front().place != carrier->id ||
        instruction->operands.front().type != carrier->type ||
        (!extracts.empty() &&
         (instruction->enumOwner != extracts.front()->enumOwner ||
          instruction->enumVariant != extracts.front()->enumVariant)))
      return std::nullopt;
    const MirLoan *successor = nullptr;
    if (*instruction->payloadExtractionKind ==
        PayloadExtractionKind::TransferReference) {
      for (const auto &candidate : body.loans) {
        if (!candidate.referenceSelection || candidate.referenceSelection->kind !=
                                                 ReferenceSelectionKind::StoredExternalReference ||
            candidate.referenceSelection->value != *instruction->result ||
            candidate.producedBy != instruction->hirValue)
          continue;
        if (successor || candidate.entry || candidate.escapes ||
            candidate.kind != MirLoanKind::Local ||
            candidate.referenceSelection->carrier != carrier->id ||
            candidate.referenceSelection->transferredFrom != group->id ||
            candidate.carriers.size() != 1)
          return std::nullopt;
        successor = &candidate;
      }
      if (!successor)
        return std::nullopt;
      result.successors.push_back(successor->id);
    }
    extracts.push_back(instruction);
    successors.push_back(successor);
    ++position;
  }
  if (extracts.empty() ||
      std::find(result.successors.begin(), result.successors.end(),
                selected.id) == result.successors.end())
    return std::nullopt;
  const auto *retire = next();
  if (!retire || retire->kind != MirInstructionKind::EndBorrow ||
      retire->loan != group->id)
    return std::nullopt;
  result.retirement = retire->id;
  ++position;
  for (std::size_t field = 0; field < extracts.size(); ++field) {
    const auto *successor = successors[field];
    const auto *publication = next();
    if (successor) {
      const auto *address = publication && publication->operands.size() == 1
                                ? body.findPlace(publication->operands[0].place)
                                : nullptr;
      if (!publication || publication->kind != MirInstructionKind::Borrow ||
          publication->loan != successor->id || !address ||
          address->root != MirPlaceRootKind::Value ||
          address->value != *extracts[field]->result ||
          !address->projections.empty())
        return std::nullopt;
      ++position;
      publication = next();
    }
    const auto *destination = publication && publication->destination
                                  ? body.findPlace(*publication->destination)
                                  : nullptr;
    if (!publication || publication->kind != MirInstructionKind::Initialize ||
        !destination || destination->root != MirPlaceRootKind::Binding ||
        !destination->projections.empty() || destination->id == carrier->id ||
        publication->operands.size() != 1)
      return std::nullopt;
    const auto &input = publication->operands.front();
    if (successor ? (input.kind != MirOperandKind::Loan ||
                     input.loan != successor->id ||
                     destination->binding != successor->carriers.front())
                  : (input.kind != MirOperandKind::Value ||
                     input.value != *extracts[field]->result))
      return std::nullopt;
    ++position;
  }
  result.endPosition = position;
  // An arm entry is not established by a tag assertion attached to an extract:
  // require its unique executable switch edge over this physical carrier.
  std::size_t incoming = 0;
  for (const auto &predecessor : body.blocks) {
    const auto targets = terminatorSuccessors(predecessor.terminator);
    if (std::find(targets.begin(), targets.end(), block->id) == targets.end())
      continue;
    const auto &terminator = predecessor.terminator;
    if (++incoming != 1 || terminator.kind != MirTerminatorKind::Switch ||
        !terminator.value ||
        terminator.value->kind != MirOperandKind::BorrowRead ||
        terminator.value->place != carrier->id ||
        terminator.target == block->id)
      return std::nullopt;
    std::size_t matching = 0;
    for (const auto &target : terminator.switchTargets)
      if (target.target == block->id) {
        if (++matching != 1 || !target.value ||
            target.value->enumOwner != extracts.front()->enumOwner ||
            target.value->value.negative ||
            target.value->value.magnitude != *extracts.front()->enumVariant)
          return std::nullopt;
      }
    if (matching != 1)
      return std::nullopt;
  }
  return incoming == 1 && block->id != body.entry
             ? std::optional{std::move(result)}
             : std::nullopt;
}

const MirInstruction *
referenceInitializerCall(const MirProgram &program,
                         const MirConstructorInstance &constructor,
                         const MirConstructorInitializer &initializer) {
  if (!initializer.storesReference || initializer.field == 0 ||
      initializer.arguments.size() != 1 ||
      initializer.targetType.kind != SemanticType::Reference ||
      initializer.targetType.arguments.size() != 1)
    return nullptr;
  const auto &body = constructor.body;
  const MirInstruction *call = nullptr;
  const MirInstruction *publication = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.hirValue != initializer.arguments.front())
        continue;
      if (instruction.kind == MirInstructionKind::Call) {
        if (call)
          return nullptr;
        call = &instruction;
      }
      const auto *loan =
          instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
      if (instruction.kind == MirInstructionKind::Borrow && loan &&
          loan->kind == MirLoanKind::Stored &&
          loan->storedPath == BorrowPath{fieldProjection(initializer.field)}) {
        if (publication)
          return nullptr;
        publication = &instruction;
      }
    }
  if (!call || !publication || !call->functionTarget || !call->result ||
      !call->loan || call->receiver || call->operands.size() != 1 ||
      call->parameterTypes.size() != 1 ||
      call->parameterTypes.front().kind != SemanticType::Reference ||
      call->borrowOrigin != BorrowOriginKind::Argument ||
      call->borrowArgument != 0 || !call->borrowArguments.empty() ||
      !call->borrowFields.empty() ||
      call->info.type != initializer.targetType.arguments.front() ||
      call->info.category != ValueCategory::Place ||
      call->borrowAccess != initializer.targetType.referenceAccess ||
      publication->operands.size() != 1 ||
      publication->operands.front().kind != MirOperandKind::Loan ||
      publication->operands.front().loan != *call->loan ||
      publication->operands.front().type != call->info.type ||
      publication->info.type != call->info.type)
    return nullptr;
  const auto *target = program.findFunctionInstance(*call->functionTarget);
  const auto *result = body.findLoan(*call->loan);
  const auto *stored = body.findLoan(*publication->loan);
  const auto origin = borrowSourceForOperand(body, call->operands.front());
  if (!target || target->returnType != initializer.targetType ||
      target->parameterTypes != call->parameterTypes || target->owner ||
      target->returnBorrowOrigin != BorrowOriginKind::Argument ||
      target->returnBorrowParameter != 0 ||
      target->returnBorrowAccess != call->borrowAccess ||
      !target->returnBorrowParameters.empty() ||
      !target->returnBorrowFields.empty() || !result || !stored || !origin ||
      result->kind != MirLoanKind::CallResult || result->entry ||
      result->escapes || result->producedBy != call->hirValue ||
      result->source != *origin || result->access != call->borrowAccess ||
      !stored->escapes || stored->entry || stored->parent != 0 ||
      stored->source != result->source || stored->access != result->access ||
      stored->storedDependency != result->storedDependency ||
      stored->producedBy != call->hirValue ||
      stored->additionalDependencies != result->additionalDependencies)
    return nullptr;
  return call;
}

bool hasReferenceContents(const MirBorrowDependencySet &dependencies) {
  return std::any_of(dependencies.dependencies.begin(), dependencies.dependencies.end(),
      [](const auto &dependency) { return dependency.referenceContents.has_value(); });
}

bool hasReferenceContents(const MirBody &body) {
  return std::any_of(body.loans.begin(), body.loans.end(), [](const auto &loan) {
      return loan.referenceContents || loan.contentsOpening ||
          std::any_of(loan.additionalDependencies.begin(), loan.additionalDependencies.end(),
              [](const auto &dependency) { return dependency.referenceContents.has_value(); });
    }) || std::any_of(body.values.begin(), body.values.end(), [](const auto &value) {
      return value.borrowDependencies && hasReferenceContents(*value.borrowDependencies);
    });
}

bool storedCarrierConstructorPublication(const MirBody &body, const MirLoan &loan) {
  if (body.kind != MirBodyKind::Constructor || loan.kind != MirLoanKind::Stored ||
      !loan.escapes || loan.entry || loan.parent || loan.storedDependency ||
      !loan.producedBy || loan.designatedFields.empty() || loan.storedPath.size() != 1 ||
      loan.storedPath.front().kind != PlaceProjectionKind::Field)
    return false;
  const MirInstruction *publication = nullptr;
  const MirBlock *publicationBlock = nullptr;
  std::size_t publicationPosition = 0;
  for (const auto &block : body.blocks)
    for (std::size_t position = 0; position < block.instructions.size(); ++position) {
      const auto &instruction = block.instructions[position];
      if (instruction.tombstone || instruction.loan != loan.id ||
          instruction.kind == MirInstructionKind::EndBorrow ||
          instruction.kind == MirInstructionKind::ActivateBorrow)
        continue;
      if (publication) return false;
      publication = &instruction;
      publicationBlock = &block;
      publicationPosition = position;
    }
  if (!publication || publication->kind != MirInstructionKind::Borrow ||
      publication->hirValue != loan.producedBy || publication->operands.size() != 1)
    return false;
  const auto &operand = publication->operands.front();
  const auto *input = body.findLoan(operand.loan);
  if (operand.kind != MirOperandKind::Loan || !operand.value || operand.place ||
      !input || input->id == loan.id || input->kind != MirLoanKind::Stored ||
      input->escapes || input->storedDependency || input->source != loan.source ||
      input->access != loan.access || input->designatedFields != loan.designatedFields ||
      input->conditions != loan.conditions ||
      input->additionalDependencies != loan.additionalDependencies)
    return false;
  const MirInstruction *initialization = nullptr;
  for (std::size_t position = 0; position < publicationPosition; ++position) {
    const auto &instruction = publicationBlock->instructions[position];
    if (instruction.tombstone || instruction.kind != MirInstructionKind::Initialize ||
        !instruction.constructorInitializer || instruction.hirValue != loan.producedBy)
      continue;
    const auto *destination = instruction.destination ? body.findPlace(*instruction.destination) : nullptr;
    if (!destination || destination->root != MirPlaceRootKind::This ||
        destination->projections.size() != 1 ||
        destination->projections.front().kind != MirProjectionKind::Field ||
        destination->projections.front().field != loan.storedPath.front().field)
      continue;
    if (initialization || instruction.operands.size() != 1 ||
        instruction.operands.front().kind != MirOperandKind::Value ||
        instruction.operands.front().value != operand.value ||
        instruction.operands.front().type != operand.type)
      return false;
    initialization = &instruction;
  }
  return initialization != nullptr;
}

std::optional<MirReferenceReturnProjection>
candidateSharedErasedReferenceReturn(const MirBody &body, const MirLoan &restored) {
  if (body.returnType.kind != SemanticType::Reference ||
      body.returnType.arguments.size() != 1 ||
      body.returnType.referenceAccess != AccessMode::ReadOnly ||
      restored.kind != MirLoanKind::CallResult || restored.entry || restored.escapes ||
      restored.access != AccessMode::ReadOnly || restored.lifecycleBound ||
      restored.referenceSelection || restored.referenceContents || restored.contentsOpening ||
      !restored.designatedFields.empty() || !restored.conditions.empty())
    return std::nullopt;
  const auto *restore = restoredBorrowAuthority(body, restored);
  const auto *opaque = body.findLoan(restored.storedDependency);
  if (!restore || restore->loan != restored.id || !restore->result ||
      restore->info.type != body.returnType.arguments.front() ||
      !opaque || opaque->kind != MirLoanKind::CallResult || opaque->entry || opaque->escapes ||
      opaque->access != AccessMode::ReadOnly || opaque->lifecycleBound ||
      opaque->referenceSelection || opaque->referenceContents || opaque->contentsOpening ||
      !opaque->designatedFields.empty() || !opaque->conditions.empty())
    return std::nullopt;
  const MirInstruction *erase = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (!instruction.tombstone && instruction.loan == opaque->id &&
          (instruction.kind == MirInstructionKind::Call ||
           instruction.kind == MirInstructionKind::Borrow)) {
        if (erase) return std::nullopt;
        erase = &instruction;
      }
  if (!erase || erase->kind != MirInstructionKind::Call || !erase->result ||
      erase->hirValue != opaque->producedBy ||
      erase->borrowOrigin != BorrowOriginKind::Argument ||
      erase->borrowArgument >= erase->operands.size() ||
      erase->borrowArgument >= erase->parameterTypes.size())
    return std::nullopt;
  const auto &input = erase->operands[erase->borrowArgument];
  const auto &reference = erase->parameterTypes[erase->borrowArgument];
  if (reference.kind != SemanticType::Reference || reference.arguments.size() != 1 ||
      reference.referenceAccess != AccessMode::ReadOnly || input.type != reference ||
      !sameLifetimeConversionShape(reference, body.returnType))
    return std::nullopt;
  const auto terminal = [&](const auto &self, const MirOperand &operand,
                            std::size_t depth) -> const MirOperand * {
    if (depth > body.values.size()) return nullptr;
    if (operand.kind != MirOperandKind::Value) return &operand;
    const auto *stage = definitionFor(body, operand.value);
    if (!stage || stage->tombstone || stage->kind != MirInstructionKind::CallInput ||
        stage->callInputKind != HirCallInputKind::ReadBorrow || stage->operands.size() != 1)
      return nullptr;
    return self(self, stage->operands.front(), depth + 1);
  };
  const auto *actual = terminal(terminal, input, 0);
  if (!actual || actual->value || actual->literal ||
      (actual->kind == MirOperandKind::BorrowRead && actual->loan))
    return std::nullopt;
  const MirLoan *parameter = actual->kind == MirOperandKind::Loan
      ? body.findLoan(actual->loan) : nullptr;
  const auto *address = body.findPlace(actual->place);
  if (actual->kind == MirOperandKind::BorrowRead && address)
    for (const auto &loan : body.loans) {
      const auto *root = body.findPlace(loan.source);
      if (loan.kind == MirLoanKind::Parameter && loan.entry && root &&
          wholeEntryPlaceMatches(body, *root, *address)) {
        if (parameter) return std::nullopt;
        parameter = &loan;
      }
    }
  const auto *root = parameter ? body.findPlace(parameter->source) : nullptr;
  if (!parameter || parameter->kind != MirLoanKind::Parameter || !parameter->entry ||
      parameter->parent || parameter->escapes || parameter->access != AccessMode::ReadOnly ||
      parameter->lifecycleBound || parameter->referenceSelection ||
      parameter->referenceContents || parameter->contentsOpening ||
      !parameter->additionalDependencies.empty() || !parameter->designatedFields.empty() ||
      !parameter->conditions.empty() || !root || root->root != MirPlaceRootKind::Binding ||
      !root->initiallyAvailable || root->foreignStorage ||
      root->type.kind != SemanticType::Reference ||
      !sameLifetimeConversionShape(root->type, reference) ||
      parameter->carriers.size() != 1 || parameter->carriers.front() != root->binding ||
      (actual->place && (!address || !wholeEntryPlaceMatches(body, *root, *address))) ||
      (actual->kind != MirOperandKind::Loan && actual->kind != MirOperandKind::BorrowRead))
    return std::nullopt;
  const auto *stored = body.findLoan(parameter->storedDependency);
  if (!stored || stored->kind != MirLoanKind::Stored || !stored->entry ||
      stored->source != parameter->source || stored->parent || stored->storedDependency ||
      stored->access != AccessMode::ReadOnly || stored->lifecycleBound || stored->escapes ||
      stored->referenceSelection || stored->referenceContents || stored->contentsOpening ||
      stored->designatedFields.empty() || !stored->conditions.empty())
    return std::nullopt;
  MirBorrowDependencySet original{.dependencies = {{
      .source = opaque->source, .parent = opaque->parent,
      .designatedFields = opaque->designatedFields, .access = opaque->access,
      .conditions = opaque->conditions}}};
  original.dependencies.insert(original.dependencies.end(),
      opaque->additionalDependencies.begin(), opaque->additionalDependencies.end());
  MirBorrowDependencySet publication;
  std::optional<MirLoanDependency> shell;
  for (const auto &dependency : original.dependencies) {
    if (!body.findPlace(dependency.source) || dependency.referenceContents ||
        dependency.access != AccessMode::ReadOnly || !dependency.designatedFields.empty() ||
        !dependency.conditions.empty())
      return std::nullopt;
    if (dependency.parent == parameter->id) {
      const auto *supplier = body.findPlace(dependency.source);
      if (shell || !supplier || !wholeEntryPlaceMatches(body, *root, *supplier))
        return std::nullopt;
      shell = dependency;
      shell->source = parameter->source;
      shell->parent = 0;
    } else if (dependency.parent == stored->id) {
      publication.dependencies.push_back(dependency);
    } else {
      return std::nullopt;
    }
  }
  if (!shell || publication.dependencies.empty()) return std::nullopt;
  publication.dependencies.insert(publication.dependencies.begin(), *shell);
  std::vector<MirLoanDependency> expected = original.dependencies;
  for (auto &dependency : expected) dependency.parent = opaque->id;
  std::vector<MirLoanDependency> restoredSuppliers{{
      .source = restored.source, .parent = restored.parent,
      .designatedFields = restored.designatedFields, .access = restored.access,
      .conditions = restored.conditions}};
  restoredSuppliers.insert(restoredSuppliers.end(), restored.additionalDependencies.begin(),
                           restored.additionalDependencies.end());
  if (!std::is_permutation(expected.begin(), expected.end(),
                           restoredSuppliers.begin(), restoredSuppliers.end()))
    return std::nullopt;
  return MirReferenceReturnProjection{.restoration = restore, .erasure = erase,
      .erasureInput = input, .referenceType = reference, .parameter = parameter->id,
      .stored = stored->id, .originalSuppliers = std::move(original),
      .publication = std::move(publication)};
}

std::optional<MirReferenceReturnProjection>
verifiedSharedErasedReferenceReturn(const MirProgram &program, const MirBody &body,
                                   const MirLoan &restored, std::size_t owner) {
  auto candidate = candidateSharedErasedReferenceReturn(body, restored);
  if (!candidate || !verifiedReferenceRestoreCall(program, body, *candidate->restoration))
    return std::nullopt;
  const auto &restore = *candidate->restoration;
  const auto actual = verifiedReferenceErasureSource(program, body,
      restore.operands[restore.borrowArgument]);
  const auto *parameter = body.findLoan(candidate->parameter);
  const auto *root = parameter ? body.findPlace(parameter->source) : nullptr;
  if (!actual || actual->operand != candidate->erasureInput ||
      actual->referenceType != candidate->referenceType || !root ||
      !mirLifetimeReferenceConversion(program, body, owner, root->type, actual->referenceType) ||
      !mirLifetimeReferenceConversion(program, body, owner, root->type, body.returnType))
    return std::nullopt;
  // This publication exports the independently identified entry permission.
  // An erasing helper can shorten its argument region for its own call; that
  // transient type is not the lifetime of the still-live original reference.
  // Exact pointer identity and full original-parent replay are both mandatory.
  return candidate;
}

bool matchesSharedErasedReferenceReturn(const MirBody &body, const MirBlock &block,
    const MirInstruction &instruction, const MirReferenceReturnProjection &projection) {
  const auto *published = instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
  if (instruction.tombstone || instruction.kind != MirInstructionKind::Borrow ||
      instruction.operands.size() != 1 || !published ||
      published->kind != MirLoanKind::Return || published->entry || !published->escapes ||
      published->parent || published->lifecycleBound || published->referenceSelection ||
      published->referenceContents || published->contentsOpening ||
      published->storedDependency != projection.stored ||
      instruction.hirValue != projection.restoration->hirValue ||
      published->producedBy != instruction.hirValue ||
      instruction.info.type != projection.restoration->info.type)
    return false;
  const auto &operand = instruction.operands.front();
  if (operand.kind != MirOperandKind::Loan || operand.place || operand.value || operand.literal ||
      operand.loan != projection.restoration->loan || operand.type != instruction.info.type)
    return false;
  std::vector<MirLoanDependency> claimed{{.source = published->source,
      .parent = published->parent, .designatedFields = published->designatedFields,
      .access = published->access, .conditions = published->conditions}};
  claimed.insert(claimed.end(), published->additionalDependencies.begin(),
                 published->additionalDependencies.end());
  if (claimed != projection.publication.dependencies) return false;
  const auto current = std::find_if(block.instructions.begin(), block.instructions.end(),
      [&](const auto &item) { return &item == &instruction; });
  if (current == block.instructions.end() || std::next(current) == block.instructions.end())
    return false;
  const auto *restored = body.findLoan(operand.loan);
  if (!restored || (!restored->carriers.empty() &&
      !callResultReferenceAliases(body, *restored, block, current - block.instructions.begin())))
    return false;
  const auto &retire = *std::next(current);
  return !retire.tombstone && retire.kind == MirInstructionKind::EndBorrow &&
      retire.loan == restored->id && retire.hirValue == instruction.hirValue;
}

bool storedReferenceReturnPublication(const MirBody &body,
                                       const MirBlock &block,
                                       const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Borrow || !instruction.loan ||
      instruction.operands.size() != 1 || !instruction.hirValue)
    return false;
  const auto *published = body.findLoan(*instruction.loan);
  const auto &operand = instruction.operands.front();
  const auto *entry = body.findLoan(operand.loan);
  const auto *runtime = body.findPlace(operand.place);
  const auto *root = entry ? body.findPlace(entry->source) : nullptr;
  // A shared field of a local carrier publishes only the selected external
  // dependency. The carrier's complete group remains live for its own drop;
  // it is not retired or copied wholesale into the escaping scalar reference.
  if (published && published->kind == MirLoanKind::Return && !published->entry &&
      published->escapes && !published->parent &&
      published->producedBy == instruction.hirValue &&
      published->access == AccessMode::ReadOnly && !published->storedDependency &&
      published->designatedFields.empty() && published->conditions.empty() &&
      operand.kind == MirOperandKind::Loan && !operand.value && entry &&
      entry->kind == MirLoanKind::Stored && !entry->entry && !entry->escapes &&
      entry->storedPath.empty() && runtime && !runtime->foreignStorage &&
      runtime->root == MirPlaceRootKind::Binding && runtime->projections.size() >= 2 &&
      runtime->projections.back().kind == MirProjectionKind::Dereference &&
      runtime->type == operand.type && operand.type == instruction.info.type &&
      std::find(entry->carriers.begin(), entry->carriers.end(), runtime->binding) !=
          entry->carriers.end()) {
    std::vector<MirLoanDependency> selected;
    const auto select = [&](const MirLoanDependency &dependency) {
      if (dependency.designatedFields.empty() || !dependency.conditions.empty())
        return;
      const auto prefix = borrowPathProjectionPrefix(
          runtime->projections, dependency.designatedFields, true);
      if (!prefix || *prefix != runtime->projections.size())
        return;
      selected.push_back({.source = dependency.source, .access = AccessMode::ReadOnly});
    };
    select({.source = entry->source, .designatedFields = entry->designatedFields,
            .access = entry->access, .conditions = entry->conditions});
    for (const auto &dependency : entry->additionalDependencies)
      select(dependency);
    std::vector<MirLoanDependency> claimed{
        {.source = published->source, .access = published->access}};
    claimed.insert(claimed.end(), published->additionalDependencies.begin(),
                   published->additionalDependencies.end());
    if (selected.empty() || selected.size() != claimed.size())
      return false;
    std::vector<bool> matched(selected.size());
    for (const auto &dependency : claimed) {
      if (dependency.parent || !dependency.designatedFields.empty() ||
          !dependency.conditions.empty() || dependency.access != AccessMode::ReadOnly)
        return false;
      bool found = false;
      for (std::size_t index = 0; index != selected.size(); ++index)
        if (!matched[index] && selected[index].source == dependency.source) {
          matched[index] = true;
          found = true;
          break;
        }
      if (!found)
        return false;
    }
    // This publication captures its pointer before local carrier cleanup.
    // Consumers must read that pointer; an operand-level place override would
    // evaluate the old field again, potentially after its object was dropped.
    const auto rereadsAddress = [&](const MirOperand &use) {
      return use.kind == MirOperandKind::Loan && use.loan == published->id && use.place;
    };
    for (const auto &consumerBlock : body.blocks) {
      const auto &end = consumerBlock.terminator;
      if ((end.value && rereadsAddress(*end.value)) ||
          std::any_of(end.targetArguments.begin(), end.targetArguments.end(), rereadsAddress) ||
          std::any_of(end.elseArguments.begin(), end.elseArguments.end(), rereadsAddress) ||
          std::any_of(end.switchTargets.begin(), end.switchTargets.end(), [&](const auto &edge) {
            return std::any_of(edge.arguments.begin(), edge.arguments.end(), rereadsAddress);
          }))
        return false;
      for (const auto &consumer : consumerBlock.instructions)
        if (!consumer.tombstone &&
            ((consumer.receiver && rereadsAddress(*consumer.receiver)) ||
             std::any_of(consumer.operands.begin(), consumer.operands.end(), rereadsAddress)))
          return false;
    }
    // This is a shape proof only. Program verification independently derives
    // the selected sources from current storage flow and the exact typed path.
    return true;
  }
  if (!published || published->kind != MirLoanKind::Return || published->entry ||
      !published->escapes || published->parent || published->producedBy != instruction.hirValue ||
      operand.kind != MirOperandKind::Loan || operand.value || !entry || !entry->entry ||
      entry->kind != MirLoanKind::Stored || entry->escapes || entry->parent ||
      !runtime || !root || published->source != runtime->id ||
      published->storedDependency != entry->storedDependency ||
      !published->additionalDependencies.empty() || !published->designatedFields.empty() ||
      !published->conditions.empty() || runtime->type != operand.type ||
      operand.type != instruction.info.type || runtime->foreignStorage ||
      (published->access == AccessMode::Mutable && runtime->access != AccessMode::Mutable) ||
      runtime->root != root->root || runtime->symbol != root->symbol ||
      runtime->binding != root->binding || runtime->capture != root->capture ||
      (root->root != MirPlaceRootKind::This && root->root != MirPlaceRootKind::Binding) ||
      !root->projections.empty() || runtime->projections.size() < 2 ||
      runtime->projections.back().kind != MirProjectionKind::Dereference)
    return false;
  // Body-local shape only. The program verifier independently walks the
  // same typed path through the sealed nominal inventory, proving each base
  // owner and concrete selection; an executable instance ID is not a source
  // declaration identity that this body-only helper may invent.
  const auto matches = [&](const BorrowPath &path, const auto &conditions, AccessMode access) {
    if (!isWellFormedBorrowPath(path) || path.empty() || !conditions.empty() ||
        (published->access == AccessMode::Mutable && access != AccessMode::Mutable))
      return false;
    const auto prefix = borrowPathProjectionPrefix(runtime->projections, path, true);
    return prefix && *prefix == runtime->projections.size();
  };
  if (!matches(entry->designatedFields, entry->conditions, entry->access) &&
      std::none_of(entry->additionalDependencies.begin(), entry->additionalDependencies.end(),
                   [&](const auto &dependency) {
                     // Entry groups describe later stored fields by an
                     // abstract path with no concrete source. The exact
                     // runtime field address and callable inventory prove
                     // that path; no local or parented dependency may use it.
                     return (dependency.source == entry->source ||
                             (dependency.source == 0 && dependency.parent == 0)) &&
                            matches(dependency.designatedFields, dependency.conditions, dependency.access);
                   }))
    return false;
  const auto current = std::find_if(block.instructions.begin(), block.instructions.end(),
      [&](const auto &candidate) { return candidate.id == instruction.id; });
  if (current == block.instructions.end())
    return false;
  auto retire = std::next(current);
  bool referenceShell = false;
  if (retire != block.instructions.end() && retire->kind == MirInstructionKind::EndBorrow &&
      retire->loan && retire->loan != entry->id) {
    const auto *shell = body.findLoan(*retire->loan);
    referenceShell = shell && shell->kind == MirLoanKind::Parameter && shell->entry &&
        !shell->parent && !shell->escapes && !shell->producedBy &&
        shell->source == entry->source && shell->storedDependency == entry->id &&
        root->root == MirPlaceRootKind::Binding &&
        root->type.kind == SemanticType::Reference && root->type.arguments.size() == 1 &&
        std::find(shell->carriers.begin(), shell->carriers.end(), root->binding) != shell->carriers.end() &&
        retire->hirValue == instruction.hirValue;
    if (!referenceShell)
      return false;
    ++retire;
  }
  if (root->root == MirPlaceRootKind::Binding && !referenceShell &&
      std::find(entry->carriers.begin(), entry->carriers.end(), root->binding) == entry->carriers.end())
    return false;
  return retire != block.instructions.end() && retire->kind == MirInstructionKind::EndBorrow &&
         retire->loan == entry->id && retire->hirValue == instruction.hirValue;
}

bool referenceParameterReturnPublication(const MirBody &body,
                                         const MirBlock &block,
                                         const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Borrow || !instruction.loan ||
      instruction.operands.size() != 1)
    return false;
  const auto *published = body.findLoan(*instruction.loan);
  if (published == nullptr)
    return false;
  const auto &produced = *published;
  const auto &source = instruction.operands.front();
  if (produced.kind != MirLoanKind::Return || produced.entry ||
      !produced.escapes || produced.parent != 0 ||
      source.kind != MirOperandKind::Loan || source.value != 0 ||
      instruction.hirValue == 0 ||
      instruction.hirValue != produced.producedBy ||
      source.type != instruction.info.type) {
    return false;
  }
  const MirLoan *retained = body.findLoan(source.loan);
  if (retained == nullptr || retained->kind != MirLoanKind::Parameter ||
      !retained->entry || retained->escapes || retained->parent != 0 ||
      retained->producedBy != 0 || retained->carriers.empty() ||
      retained->source != produced.source ||
      (retained->access != produced.access &&
       retained->access != AccessMode::Mutable)) {
    return false;
  }
  const MirPlace *carrier = body.findPlace(retained->source);
  if (carrier == nullptr || carrier->root != MirPlaceRootKind::Binding ||
      !carrier->projections.empty() ||
      carrier->type.kind != SemanticType::Reference ||
      carrier->type.arguments.size() != 1 ||
      std::find(retained->carriers.begin(), retained->carriers.end(),
                carrier->binding) == retained->carriers.end()) {
    return false;
  }
  if (source.place == 0) {
    // With no explicit address, only the parameter pointer itself
    // can be published, with its exact referent type.
    const bool selectedAlias = std::any_of(
        body.places.begin(), body.places.end(), [&](const MirPlace &place) {
          return place.root == MirPlaceRootKind::Binding &&
                 place.binding != carrier->binding &&
                 place.sourceValue == instruction.hirValue &&
                 place.type == source.type &&
                 std::find(retained->carriers.begin(), retained->carriers.end(),
                           place.binding) != retained->carriers.end();
        });
    if (selectedAlias ||
        carrier->type.arguments.front() != instruction.info.type)
      return false;
  } else {
    const auto exactRuntimeAlias =
        [&](const auto &self, const MirPlace *runtime,
            const MirBlock &consumerBlock, const MirInstruction &consumer,
            std::size_t depth) -> bool {
      if (depth > body.places.size() || runtime == nullptr ||
          runtime->root != MirPlaceRootKind::Binding ||
          runtime->binding == carrier->binding ||
          (depth == 0 && (runtime->type != source.type ||
                          runtime->sourceValue != instruction.hirValue)) ||
          runtime->projections.empty() ||
          runtime->projections.front().kind != MirProjectionKind::Dereference ||
          !std::all_of(std::next(runtime->projections.begin()),
                       runtime->projections.end(),
                       [](const auto &projection) {
                         return projection.kind == MirProjectionKind::Field &&
                                projection.field != 0 &&
                                projection.index == 0 &&
                                !projection.constantIndex &&
                                projection.selection == 0;
                       }) ||
          (produced.access == AccessMode::Mutable &&
           runtime->access != AccessMode::Mutable) ||
          std::find(retained->carriers.begin(), retained->carriers.end(),
                    runtime->binding) == retained->carriers.end())
        return false;
      const MirInstruction *initialize = nullptr;
      const MirBlock *initializeBlock = nullptr;
      for (const auto &candidateBlock : body.blocks)
        for (const auto &candidate : candidateBlock.instructions) {
          const auto *destination = candidate.destination
                                        ? body.findPlace(*candidate.destination)
                                        : nullptr;
          if (destination == nullptr ||
              destination->root != MirPlaceRootKind::Binding ||
              destination->binding != runtime->binding ||
              !destination->projections.empty())
            continue;
          if (candidate.kind != MirInstructionKind::Initialize ||
              initialize != nullptr ||
              destination->type.kind != SemanticType::Reference ||
              destination->type.arguments.size() != 1 ||
              destination->symbol != runtime->symbol ||
              (runtime->projections.size() == 1 &&
               destination->type.arguments.front() != runtime->type) ||
              (runtime->access == AccessMode::Mutable &&
               destination->type.referenceAccess != AccessMode::Mutable) ||
              candidate.operands.size() != 1)
            return false;
          initialize = &candidate;
          initializeBlock = &candidateBlock;
        }
      if (initialize == nullptr)
        return false;
      const auto &input = initialize->operands.front();
      const auto *address = body.findPlace(input.place);
      if (input.kind != MirOperandKind::Loan || input.loan != retained->id ||
          input.type != body.findPlace(*initialize->destination)
                            ->type.arguments.front() ||
          input.value != 0 || address == nullptr || address->type != input.type)
        return false;
      // Ordinary verification proves the call result's producer and
      // Initialize address. Its sole publication must dominate this
      // return; carrier metadata or matching types alone grant nothing.
      if (initializeBlock == &consumerBlock) {
        const auto before = std::find_if(
            consumerBlock.instructions.begin(),
            consumerBlock.instructions.end(),
            [&](const auto &candidate) { return &candidate == initialize; });
        const auto after = std::find_if(
            consumerBlock.instructions.begin(),
            consumerBlock.instructions.end(),
            [&](const auto &candidate) { return &candidate == &consumer; });
        if (before >= after)
          return false;
      } else {
        const auto dominance = computeMirDominance(body);
        if (!dominance ||
            !dominance->dominates(initializeBlock->id, consumerBlock.id))
          return false;
      }
      if (address->root == MirPlaceRootKind::Binding) {
        if (address->binding == carrier->binding)
          return mir_detail::referenceParameterContainsPlace(body, *retained,
                                                             address->id) &&
                 (runtime->access == AccessMode::ReadOnly ||
                  address->access == AccessMode::Mutable);
        return self(self, address, *initializeBlock, *initialize, depth + 1);
      }
      const auto *resultLoan = body.findLoan(address->loan);
      return address->root == MirPlaceRootKind::Loan &&
             address->projections.empty() && resultLoan != nullptr &&
             resultLoan->kind == MirLoanKind::CallResult &&
             resultLoan->parent == retained->id &&
             (resultLoan->source == retained->source ||
              mir_detail::referenceParameterContainsPlace(
                  body, *retained, resultLoan->source)) &&
             resultLoan->storedDependency == retained->storedDependency &&
             resultLoan->producedBy != 0 &&
             (runtime->access == AccessMode::ReadOnly ||
              resultLoan->access == AccessMode::Mutable);
    };
    if (!exactRuntimeAlias(exactRuntimeAlias, body.findPlace(source.place),
                           block, instruction, 0))
      return false;
  }
  const auto current =
      std::find_if(block.instructions.begin(), block.instructions.end(),
                   [&](const MirInstruction &candidate) {
                     return candidate.id == instruction.id;
                   });
  if (current == block.instructions.end() ||
      std::next(current) == block.instructions.end()) {
    return false;
  }
  const MirInstruction &retire = *std::next(current);
  return retire.kind == MirInstructionKind::EndBorrow && retire.loan &&
         *retire.loan == source.loan && retire.hirValue == instruction.hirValue;
}

bool referenceParameterContainsPlace(const MirBody &body,
                                     const MirLoan &parameter,
                                     MirPlaceId place) {
  const MirPlace *formal = body.findPlace(parameter.source);
  if (parameter.kind != MirLoanKind::Parameter || !parameter.entry ||
      parameter.parent != 0 || parameter.escapes || parameter.producedBy != 0 ||
      formal == nullptr || formal->root != MirPlaceRootKind::Binding ||
      !formal->projections.empty() ||
      formal->type.kind != SemanticType::Reference ||
      formal->type.arguments.size() != 1 ||
      std::find(parameter.carriers.begin(), parameter.carriers.end(),
                formal->binding) == parameter.carriers.end())
    return false;
  const auto contains = [&](const auto &self, MirPlaceId id,
                            std::size_t depth) -> bool {
    const MirPlace *projected = body.findPlace(id);
    if (projected == nullptr || depth > body.places.size() + body.loans.size())
      return false;
    if (id == parameter.source)
      return true;
    if (projected->root != MirPlaceRootKind::Binding ||
        projected->projections.empty() ||
        projected->projections.front() !=
            MirPlaceProjection{.kind = MirProjectionKind::Dereference} ||
        !std::all_of(std::next(projected->projections.begin()),
                     projected->projections.end(), [](const auto &projection) {
                       return projection.kind == MirProjectionKind::Field &&
                              projection.field != 0 && projection.index == 0 &&
                              !projection.constantIndex &&
                              projection.selection == 0;
                     }))
      return false;
    if (projected->binding == formal->binding)
      return projected->symbol == formal->symbol &&
             (projected->projections.size() > 1 ||
              projected->type == formal->type.arguments.front());
    if (std::find(parameter.carriers.begin(), parameter.carriers.end(),
                  projected->binding) == parameter.carriers.end())
      return false;
    const MirInstruction *initialize = nullptr;
    const MirPlace *destination = nullptr;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *target = instruction.destination
                                 ? body.findPlace(*instruction.destination)
                                 : nullptr;
        if (target == nullptr || target->root != MirPlaceRootKind::Binding ||
            target->binding != projected->binding ||
            !target->projections.empty())
          continue;
        if (initialize != nullptr ||
            instruction.kind != MirInstructionKind::Initialize)
          return false;
        initialize = &instruction;
        destination = target;
      }
    if (initialize == nullptr ||
        destination->type.kind != SemanticType::Reference ||
        destination->type.arguments.size() != 1 ||
        initialize->operands.size() != 1)
      return false;
    const auto &input = initialize->operands.front();
    const auto *address = body.findPlace(input.place);
    if (input.kind != MirOperandKind::Loan || input.loan != parameter.id ||
        input.value != 0 || address == nullptr ||
        input.type != destination->type.arguments.front() ||
        address->type != input.type)
      return false;
    if (address->root == MirPlaceRootKind::Loan) {
      const auto *result = body.findLoan(address->loan);
      return result != nullptr && result->kind == MirLoanKind::CallResult &&
             referenceAddressRetainsLoan(body, *address, parameter) &&
             self(self, result->source, depth + 1);
    }
    return self(self, address->id, depth + 1);
  };
  return contains(contains, place, 0);
}

bool referenceAddressRetainsLoan(const MirBody &body, const MirPlace &address,
                                 const MirLoan &retained) {
  if (address.root != MirPlaceRootKind::Loan || address.loan == retained.id)
    return true;
  std::unordered_set<MirLoanId> visited;
  MirLoanId link = address.loan;
  while (link != retained.id) {
    const MirLoan *child = body.findLoan(link);
    if (child == nullptr || !visited.insert(link).second)
      return false;
    if (child->kind == MirLoanKind::CallResult) {
      const MirInstruction *producer = nullptr;
      for (const MirBlock &block : body.blocks)
        for (const MirInstruction &instruction : block.instructions)
          if (instruction.kind == MirInstructionKind::Call &&
              instruction.loan == child->id &&
              instruction.hirValue == child->producedBy) {
            if (producer != nullptr)
              return false;
            producer = &instruction;
          }
      if (producer == nullptr || child->producedBy == 0 ||
          producer->info.access != child->access)
        return false;
      const auto &result = producer->info.type;
      const SemanticType &referent =
          result.kind == SemanticType::Reference && result.arguments.size() == 1
              ? result.arguments.front()
              : result;
      if (link == address.loan && referent != address.type)
        return false;
      // The call that produced this address may also produce the binding's
      // durable loan as its sibling: both reborrow one parent with the same
      // access, as a structural range element does beside its dereference.
      if (link == address.loan && child->parent != 0 &&
          child->parent == retained.parent &&
          child->producedBy == retained.producedBy &&
          child->access == retained.access &&
          retained.kind != MirLoanKind::CallResult)
        return true;
      // A call on a checked field (or on a captured reference carrier) names
      // that precise receiver storage as its dependency. Do not let the loan
      // table replace it with a coarse formal or an unrelated same-typed place.
      if (retained.kind == MirLoanKind::Parameter &&
          child->parent == retained.id &&
          producer->borrowOrigin == BorrowOriginKind::Receiver &&
          producer->receiver) {
        const auto *formal = body.findPlace(retained.source);
        const auto *receiver = body.findPlace(producer->receiver->place);
        const auto *source = body.findPlace(child->source);
        if (formal != nullptr && receiver != nullptr &&
            receiver->root == MirPlaceRootKind::Binding &&
            ((receiver->binding == formal->binding &&
              receiver->projections.size() > 1) ||
             (receiver->binding != formal->binding &&
              child->source != retained.source)) &&
            (source == nullptr || source->root != receiver->root ||
             source->binding != receiver->binding ||
             source->symbol != receiver->symbol ||
             source->projections != receiver->projections ||
             source->type != receiver->type))
          return false;
      }
      // Shared carrier extraction can publish a fresh address loan with no
      // reborrow parent. Its exact external source and dependency identities
      // still agree with the retained read-only owner; a mutable parent cannot
      // use this parallel-loan family to hide a detached exclusive reborrow.
      if (child->parent == 0 && child->access == AccessMode::ReadOnly &&
          retained.access == AccessMode::ReadOnly &&
          child->source == retained.source &&
          child->storedDependency == retained.storedDependency &&
          child->additionalDependencies == retained.additionalDependencies)
        return true;
      // A result through a whole named receiver is a child of that
      // receiver's storage. Its stored edge is the receiver's Stored group,
      // which also parents the one external path it forwards; the complete
      // dependency-set replay proves that path independently.
      if (link == address.loan && child->parent == 0 &&
          retained.kind == MirLoanKind::Stored && !retained.entry &&
          child->storedDependency == retained.id &&
          producer->borrowOrigin == BorrowOriginKind::Receiver &&
          (retained.access == AccessMode::Mutable ||
           child->access == AccessMode::ReadOnly) &&
          std::any_of(child->additionalDependencies.begin(),
                      child->additionalDependencies.end(),
                      [&](const MirLoanDependency &dependency) {
                        return dependency.parent == retained.id &&
                               dependency.source == retained.source;
                      }))
        return true;
    }
    link = child->parent;
  }
  return !visited.empty();
}

namespace {

bool neutralTargetlessConstruction(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Construct &&
         instruction.info.type.kind == SemanticType::Class &&
         instruction.info.category == ValueCategory::Value &&
         instruction.result && !instruction.functionTarget &&
         !instruction.constructorTarget && !instruction.lambdaTarget &&
         !instruction.bodyTarget && !instruction.nativeCalleeType &&
         !instruction.receiver && !instruction.loan && !instruction.ownership &&
         instruction.constructorKind == ConstructorKind::Ordinary &&
         instruction.intrinsic == IntrinsicKind::None &&
         instruction.operation == MirOperation::None &&
         instruction.unsafeOperation == UnsafeOperationKind::None &&
         !instruction.rawMemoryAccess &&
         instruction.synchronization.kind ==
             SynchronizationOperationKind::None &&
         instruction.dispatch == CallDispatch::Static &&
         instruction.callSite == 0 && !instruction.callableBoundary &&
         !instruction.callableInvocation &&
         instruction.callableArguments.empty() &&
         !instruction.nativeCallbackAdapter && !instruction.threadTaskAdapter &&
         instruction.scopedThreadTasks.empty() &&
         instruction.borrowOrigin == BorrowOriginKind::None &&
         instruction.borrowArguments.empty() &&
         instruction.borrowFields.empty() && !instruction.borrowPlace &&
         !instruction.dependentTextPublication &&
         !instruction.restoresReferenceResult &&
         instruction.restoredBorrowFields.empty() &&
         instruction.objectAllocation == 0 &&
         instruction.definedFailure.empty() &&
         instruction.localFailureSites.empty();
}

} // namespace

bool generatedDefaultConstruction(const MirInstruction &instruction) {
  return neutralTargetlessConstruction(instruction) &&
         instruction.operands.empty() && instruction.parameterTypes.empty();
}

bool generatedCopyConstruction(const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Construct ||
      instruction.constructorKind != ConstructorKind::Copy ||
      !instruction.result || instruction.info.type.kind != SemanticType::Class ||
      instruction.info.category != ValueCategory::Value ||
      !instruction.info.traits.copyable || instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1)
    return false;
  if (instruction.ownership &&
      (instruction.ownership->kind != OwnershipEventKind::Read ||
       instruction.ownership->before != OwnershipStateSet::Available ||
       instruction.ownership->after != OwnershipStateSet::Available ||
       !instruction.ownership->reachable))
    return false;
  const auto &parameter = instruction.parameterTypes.front();
  if (parameter.kind != SemanticType::Reference ||
      parameter.referenceAccess != AccessMode::ReadOnly ||
      parameter.arguments != std::vector<SemanticType>{instruction.info.type} ||
      instruction.operands.front().type != parameter ||
      (instruction.borrowOrigin != BorrowOriginKind::None &&
       instruction.borrowOrigin != BorrowOriginKind::Argument) ||
      instruction.borrowArgument != 0 ||
      instruction.borrowAccess != AccessMode::ReadOnly ||
      (instruction.loan && instruction.borrowOrigin != BorrowOriginKind::Argument) ||
      (!instruction.borrowFields.empty() && !instruction.loan))
    return false;
  for (std::size_t index = 0; index < instruction.borrowFields.size(); ++index) {
    const auto &field = instruction.borrowFields[index];
    if (field.origin != BorrowOriginKind::Argument || field.parameter != 0 ||
        field.globalRoot != 0 ||
        field.resultFields != field.sourceFields ||
        field.resultConditions != field.sourceConditions ||
        field.access != AccessMode::ReadOnly ||
        std::find(instruction.borrowFields.begin(),
                  instruction.borrowFields.begin() + index, field) !=
            instruction.borrowFields.begin() + index)
      return false;
  }
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Construct,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .result = instruction.result,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .loan = instruction.loan,
                             .borrowOrigin = instruction.borrowOrigin,
                             .borrowArgument = 0,
                             .borrowAccess = AccessMode::ReadOnly,
                             .borrowFields = instruction.borrowFields,
                             .sourceLocation = instruction.sourceLocation,
                             .constructorKind = ConstructorKind::Copy,
                             .info = instruction.info,
                             .ownership = instruction.ownership,
                             .lifecycle = instruction.lifecycle};
  return instruction == exact;
}

bool passiveUnionFieldConstruction(const MirProgram &program,
                                   const MirInstruction &instruction) {
  if (!neutralTargetlessConstruction(instruction) ||
      instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1 || instruction.enumOwner ||
      instruction.enumValue || instruction.enumVariant ||
      instruction.payloadIndex || instruction.payloadExtractionKind ||
      instruction.literal || instruction.objectLayoutType ||
      !instruction.closureCaptureTypes.empty() ||
      !instruction.closureCaptureModes.empty() ||
      instruction.functionPointerFunctionTarget ||
      instruction.functionPointerLambdaTarget || instruction.neverReturns ||
      instruction.noNormalBorrowResult || instruction.tracksCaller ||
      instruction.callerLocation || instruction.callerLocationSite != 0 ||
      instruction.forwardsCallerLocation || instruction.sourceLocation ||
      instruction.constructorInitializer != 0 || instruction.callInputRole ||
      instruction.retainedOwnerPlace || instruction.preparedParameterDrop ||
      instruction.successResultDrop || instruction.successResultDestination ||
      instruction.elementCleanup || instruction.replacementCleanup ||
      instruction.replacementPair != 0 || !instruction.lifecycle.empty())
    return false;
  const MirClassInstance *owner = nullptr;
  for (const auto &candidate : program.classInstances()) {
    if (candidate.type != instruction.info.type)
      continue;
    if (owner != nullptr)
      return false;
    owner = &candidate;
  }
  if (owner == nullptr || owner->kind != ClassKind::Union ||
      !owner->unionLayout || owner->abstract || owner->polymorphic ||
      owner->compilerCapability != CompilerCapabilityTypeKind::None ||
      !owner->bases.empty() || !owner->structuralBases.empty() ||
      owner->destructor || owner->requiresActiveCleanup ||
      owner->requiresActiveDropState || owner->traits.containsBorrowedState ||
      owner->traits.drop != DropKind::Trivial ||
      instruction.info.traits != owner->traits || !owner->fields.empty() ||
      !owner->fieldDropOrder.empty() || owner->generatedDefaultRequiresSchedule)
    return false;
  const auto &layout = *owner->unionLayout;
  if (layout.sizeBytes == 0 || layout.abiAlignmentBytes == 0 ||
      layout.sizeBytes % layout.abiAlignmentBytes != 0 ||
      layout.fields.empty() ||
      layout.fields.size() != owner->declaredFields.size())
    return false;
  for (std::size_t index = 0; index < layout.fields.size(); ++index) {
    const auto &field = owner->declaredFields[index];
    const auto &native = layout.fields[index];
    if (field.field == 0 || field.symbol == 0 ||
        field.type == SemanticType::Unknown ||
        field.dropKind != DropKind::Trivial || field.requiresActiveCleanup ||
        native.field != field.symbol || native.type != field.type ||
        native.sizeBytes == 0 || native.abiAlignmentBytes == 0 ||
        native.sizeBytes > layout.sizeBytes ||
        layout.abiAlignmentBytes % native.abiAlignmentBytes != 0)
      return false;
  }
  const auto &type = owner->declaredFields.front().type;
  const auto &input = instruction.operands.front();
  return instruction.parameterTypes.front() == type && input.type == type &&
         (input.kind == MirOperandKind::Value ||
          input.kind == MirOperandKind::Constant);
}

MirScalarReturnConversion scalarReturnConversion(const MirBody &body,
                                                 const MirOperand &operand) {
  if (!isPassiveScalarValueType(body.returnType))
    return MirScalarReturnConversion::NotApplicable;
  switch (operand.kind) {
  case MirOperandKind::Value:
  case MirOperandKind::Constant:
    break;
  case MirOperandKind::Copy:
  case MirOperandKind::Move: {
    const auto *place = body.findPlace(operand.place);
    if (place == nullptr || place->type != operand.type)
      return MirScalarReturnConversion::Invalid;
    break;
  }
  default:
    return MirScalarReturnConversion::Invalid;
  }
  if (operand.type == body.returnType)
    return MirScalarReturnConversion::Exact;
  return isImplicitScalarInitializationConversion(body.returnType, operand.type)
             ? MirScalarReturnConversion::Initialization
             : MirScalarReturnConversion::Invalid;
}

bool generatedUniqueConstruction(const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::AllocateUniqueOwner ||
      instruction.constructorTarget || instruction.functionTarget ||
      instruction.receiver || !instruction.result ||
      instruction.info.type.kind != SemanticType::UniqueOwner ||
      instruction.info.type.arguments.size() != 1 ||
      instruction.info.type.arguments.front().kind != SemanticType::Class)
    return false;
  if (instruction.constructorKind == ConstructorKind::Ordinary)
    return instruction.operands.empty() && instruction.parameterTypes.empty();
  if (instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1)
    return false;
  const auto &pointee = instruction.info.type.arguments.front();
  const auto &parameter = instruction.parameterTypes.front();
  const auto &operand = instruction.operands.front();
  if (operand.kind != MirOperandKind::Value || operand.value == 0 ||
      operand.place != 0 || operand.loan != 0 || operand.literal ||
      operand.type != parameter)
    return false;
  return (instruction.constructorKind == ConstructorKind::Copy &&
          parameter.kind == SemanticType::Reference &&
          parameter.referenceAccess == AccessMode::ReadOnly &&
          parameter.arguments == std::vector<SemanticType>{pointee}) ||
         (instruction.constructorKind == ConstructorKind::Move &&
          parameter == pointee);
}

bool bindingReceivedStoredLoan(const MirBody &body, HirBindingId binding,
                               MirLoanId dependency, std::size_t depth) {
  const auto *loan = body.findLoan(dependency);
  if (depth > body.places.size() || loan == nullptr ||
      loan->kind != MirLoanKind::Stored || !loan->lifecycleBound)
    return false;
  const MirInstruction *publication = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      const auto *place = instruction.destination
                              ? body.findPlace(*instruction.destination)
                              : nullptr;
      if (instruction.kind != MirInstructionKind::Initialize ||
          place == nullptr || place->root != MirPlaceRootKind::Binding ||
          place->binding != binding || !place->projections.empty())
        continue;
      if (publication != nullptr)
        return false;
      publication = &instruction;
    }
  if (publication == nullptr || publication->operands.size() != 1 ||
      publication->operands.front().kind != MirOperandKind::Value)
    return false;
  const auto *producer =
      definitionFor(body, publication->operands.front().value);
  const auto *destination = body.findPlace(*publication->destination);
  if (producer == nullptr || destination == nullptr ||
      producer->info.type != destination->type ||
      !destination->traits.containsBorrowedState)
    return false;
  if (producer->loan == dependency)
    return true;
  if (producer->kind != MirInstructionKind::Move ||
      producer->operands.size() != 1 ||
      producer->operands.front().kind != MirOperandKind::Move)
    return false;
  const auto *previous = body.findPlace(producer->operands.front().place);
  return previous != nullptr && previous->root == MirPlaceRootKind::Binding &&
         previous->projections.empty() && previous->type == destination->type &&
         bindingReceivedStoredLoan(body, previous->binding, dependency,
                                   depth + 1);
}

bool completeObjectType(const MirProgram &program,
                        const SemanticType &selected) {
  const auto eligible = [&](const auto &self, const SemanticType &type,
                            std::size_t depth) -> bool {
    if (depth > 128 || type.arrayLengthParameterId != 0)
      return false;
    switch (type.kind) {
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
      return type == SemanticType(type.kind) ||
             (type.nativeCScalar != NativeCScalarKind::None &&
              type == SemanticType::nativeCScalarType(type.nativeCScalar,
                                                      program.target()));
    case SemanticType::RawPointer:
      return type.arguments.size() == 1 &&
             type.arguments.front().kind != SemanticType::TypeParameter &&
             type.arguments.front().kind != SemanticType::Unknown;
    case SemanticType::StringView:
    case SemanticType::CString:
      // A text region changes validity, not the object's layout.
      return eraseTypeLifetimes(type) == SemanticType(type.kind);
    case SemanticType::Enum:
      return program.findIntegralEnum(type) != nullptr ||
             program.findPayloadEnum(type) != nullptr;
    case SemanticType::Array:
      return type.arguments.size() == 1 && type.arrayLength > 0 &&
             self(self, type.arguments.front(), depth + 1);
    case SemanticType::Class: {
      // A region application changes reference validity, not the identity of
      // the complete object definition. Resolve its exact checked class view
      // rather than requiring declaration-region IDs to equal call-site IDs.
      const auto view = mirClassLifetimeView(program, type);
      const auto *owner = view ? view->instance : nullptr;
      return owner != nullptr && owner->kind != ClassKind::Interface &&
             !owner->abstract && !owner->cOpaqueHandle &&
             owner->compilerCapability == CompilerCapabilityTypeKind::None;
    }
    default:
      return false;
    }
  };
  return eligible(eligible, selected, 0);
}

namespace {
// Caller attribution is independent of the operation's object/unsafe shape.
// Admit only the immutable selected declaration's contract and the canonical
// caller anchor, then retain it in the exact comparison. All other metadata
// remains neutral; this helper cannot erase evidence before validation.
bool exactWithIntrinsicCallerContract(const MirProgram &program,
                                      const MirInstruction &instruction,
                                      MirInstruction exact) {
  if (const auto &contract = instruction.intrinsicCallContract; contract) {
    if (instruction.kind != MirInstructionKind::Call ||
        contract->declaration() == 0 ||
        contract->snapshot() != program.semanticSnapshot() ||
        contract->intrinsic() != instruction.intrinsic ||
        contract->neverReturns() != exact.neverReturns ||
        contract->tracksCaller() != instruction.tracksCaller ||
        instruction.tracksCaller != instruction.callerLocation.has_value() ||
        instruction.tracksCaller != (instruction.callerLocationSite != 0) ||
        (instruction.callerLocation &&
         program.failureMetadata().callerSiteFor(*instruction.callerLocation) !=
             instruction.callerLocationSite) ||
        (!instruction.tracksCaller && instruction.forwardsCallerLocation))
      return false;
    exact.intrinsicCallContract = contract;
    exact.tracksCaller = instruction.tracksCaller;
    exact.callerLocation = instruction.callerLocation;
    exact.callerLocationSite = instruction.callerLocationSite;
    exact.forwardsCallerLocation = instruction.forwardsCallerLocation;
  }
  return instruction == exact;
}
} // namespace

bool objectRelocate(const MirProgram &program,
                    const MirInstruction &instruction) {
  const bool range =
      instruction.intrinsic == IntrinsicKind::ObjectRelocateRange;
  if (instruction.kind != MirInstructionKind::Call ||
      (!range && instruction.intrinsic != IntrinsicKind::ObjectRelocate) ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::Void) ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.operands.size() != (range ? 3 : 2) ||
      instruction.parameterTypes.size() != (range ? 3 : 2))
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.pointerAccess != AccessMode::Mutable ||
      pointer.arguments.size() != 1 ||
      !completeObjectType(program, pointer.arguments.front()) ||
      mirTypeRelocationKind(program, pointer.arguments.front()) ==
          RelocationKind::Unavailable ||
      !mirTypeRelocationIsDefinedFailureFree(program,
                                             pointer.arguments.front()) ||
      (range && mirTypeRelocationKind(program, pointer.arguments.front()) !=
                    RelocationKind::Bitwise) ||
      instruction.parameterTypes[1] != pointer ||
      !std::all_of(instruction.operands.begin(),
                   instruction.operands.begin() + 2,
                   [&](const MirOperand &operand) {
                     return operand.kind == MirOperandKind::Value &&
                            operand.type == pointer;
                   }))
    return false;
  if (range && (instruction.parameterTypes[2] != SemanticType::UInt64 ||
                instruction.operands[2].kind != MirOperandKind::Value ||
                instruction.operands[2].type != SemanticType::UInt64))
    return false;
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .intrinsic = instruction.intrinsic,
                             .info = instruction.info};
  return exactWithIntrinsicCallerContract(program, instruction, exact);
}

bool objectTake(const MirProgram &program, const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectTake ||
      !instruction.result ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits.containsBorrowedState ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1)
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  const auto &element = instruction.info.type;
  if (pointer != SemanticType::rawPointerTo(element, AccessMode::Mutable) ||
      !completeObjectType(program, element) ||
      mirTypeRelocationKind(program, element) == RelocationKind::Unavailable ||
      !mirTypeRelocationIsDefinedFailureFree(program, element) ||
      instruction.operands[0].kind != MirOperandKind::Value ||
      instruction.operands[0].type != pointer)
    return false;
  // The result creates one ordinary owned temporary. The general lifecycle
  // verifier binds its obligation to this exact result and checks every exit;
  // taking raw storage cannot consume a checked source obligation here.
  if (instruction.lifecycle.size() > 1 ||
      std::any_of(instruction.lifecycle.begin(), instruction.lifecycle.end(),
                  [](const MirLifecycleEvent &event) {
                    return event.kind != MirLifecycleEventKind::Initialize ||
                           event.source != 0 || event.target == 0 ||
                           event.conditional || event.failureCleanup;
                  }))
    return false;
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .result = instruction.result,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .intrinsic = instruction.intrinsic,
                             .info = instruction.info,
                             .lifecycle = instruction.lifecycle};
  return instruction == exact;
}

bool objectDestroy(const MirProgram &program,
                   const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectDestroy ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.result || instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::Void) ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1)
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.pointerAccess != AccessMode::Mutable ||
      pointer.arguments.size() != 1 ||
      !completeObjectType(program, pointer.arguments.front()) ||
      instruction.operands.front().kind != MirOperandKind::Value ||
      instruction.operands.front().type != pointer ||
      !instruction.definedFailure.localOrigins.empty() ||
      (instruction.definedFailure.propagation != FailurePropagationKind::None &&
       instruction.definedFailure.propagation !=
           FailurePropagationKind::Destructor))
    return false;
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .intrinsic = IntrinsicKind::ObjectDestroy,
                             .definedFailure = instruction.definedFailure,
                             .elementCleanup = instruction.elementCleanup,
                             .info = instruction.info};
  return exactWithIntrinsicCallerContract(program, instruction, exact);
}

bool objectLayoutQuery(const MirProgram &program,
                       const MirInstruction &instruction) {
  if ((instruction.intrinsic != IntrinsicKind::ObjectSize &&
       instruction.intrinsic != IntrinsicKind::ObjectAlignment) ||
      !instruction.objectLayoutType ||
      instruction.kind != MirInstructionKind::Call || !instruction.result ||
      instruction.info.type != SemanticType::UInt64 ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::UInt64))
    return false;
  // All unlisted fields must remain neutral, including future ownership,
  // failure and invocation metadata. Provenance IDs do not grant authority.
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .result = instruction.result,
                             .intrinsic = instruction.intrinsic,
                             .objectLayoutType = instruction.objectLayoutType,
                             .info = instruction.info};
  if (!exactWithIntrinsicCallerContract(program, instruction, exact))
    return false;
  return completeObjectType(program, *instruction.objectLayoutType);
}

bool objectConstruct(const MirProgram &program,
                     const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectConstruct ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.result || instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::Void) ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.parameterTypes.empty() ||
      instruction.parameterTypes.size() != instruction.operands.size())
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.pointerAccess != AccessMode::Mutable ||
      pointer.arguments.size() != 1 ||
      !completeObjectType(program, pointer.arguments.front()) ||
      !instruction.definedFailure.localOrigins.empty() ||
      (instruction.definedFailure.propagation != FailurePropagationKind::None &&
       instruction.definedFailure.propagation !=
           FailurePropagationKind::Constructor))
    return false;
  // Call-input/lifecycle verification proves every prepared parameter and its
  // handoff; the program verifier binds constructor identity/signature/failure.
  for (std::size_t index = 0; index < instruction.operands.size(); ++index)
    if (instruction.operands[index].kind != MirOperandKind::Value ||
        instruction.operands[index].type != instruction.parameterTypes[index])
      return false;
  const auto &element = pointer.arguments.front();
  if (element.kind == SemanticType::Class) {
    const auto view = mirClassLifetimeView(program, element);
    if (!view)
      return false;
    const auto &owner = *view->instance;
    if (owner.traits.containsBorrowedState)
      return false;
    if (!instruction.constructorTarget) {
      const bool transfer =
          instruction.parameterTypes.size() == 2 &&
          instruction.parameterTypes[1] == element &&
          ((instruction.constructorKind == ConstructorKind::Copy &&
            owner.traits.copyable) ||
           (instruction.constructorKind == ConstructorKind::Move &&
            owner.traits.movable));
      const bool defaultConstruction =
          instruction.parameterTypes.size() == 1 &&
          instruction.constructorKind == ConstructorKind::Ordinary &&
          owner.defaultConstructor == SpecialMemberStatus::Generated &&
          !owner.generatedDefaultRequiresSchedule;
      if (!transfer && !defaultConstruction)
        return false;
    }
  } else {
    const bool exactValue = instruction.parameterTypes.size() == 2 &&
                            instruction.parameterTypes[1] == element;
    const auto *payload = program.findPayloadEnum(element);
    const bool payloadTransfer =
        exactValue && payload != nullptr &&
        !payload->traits.containsBorrowedState &&
        ((instruction.constructorKind == ConstructorKind::Copy &&
          payload->traits.copyable) ||
         (instruction.constructorKind == ConstructorKind::Move &&
          payload->traits.movable));
    // Exact owned carrier parameters participate in the ordinary prepared
    // argument handoff. The program's call-input/lifecycle proof verifies each
    // transfer; omitting it would leak a live owning enum/result parameter.
    const bool carrierHandoff =
        exactValue && mirUniqueCleanupType(program, element).has_value();
    if (instruction.constructorTarget ||
        (!instruction.lifecycle.empty() && !carrierHandoff) ||
        (instruction.constructorKind != ConstructorKind::Ordinary &&
         !payloadTransfer) ||
        !instruction.definedFailure.empty() ||
        instruction.parameterTypes.size() > 2 ||
        (instruction.parameterTypes.size() == 2 && !exactValue))
      return false;
  }
  // The destination pointer is an intrinsic input, not a constructor
  // parameter. Preserve only an application checked against the selected
  // constructor's own binders and its exact constructed object type. Body
  // verification separately proves the actual argument regions and bounds.
  if (instruction.constructorTarget) {
    const auto *target =
        program.findConstructorInstance(*instruction.constructorTarget);
    const auto signature = instantiateMirLifetimeTarget(program, instruction);
    if (!target || !signature || instruction.lifetimeApplication.callableSignature ||
        !equivalentLifetimeTypes(signature->returnType, element) ||
        !mirLifetimeParameterTypesMatch(
            std::span(instruction.parameterTypes).subspan(1),
            target->parameterTypes, target->lifetimeContract,
            instruction.lifetimeApplication))
      return false;
  } else if (instruction.lifetimeApplication != MirLifetimeApplication{}) {
    return false;
  }
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .lifetimeApplication = instruction.lifetimeApplication,
                             .intrinsic = IntrinsicKind::ObjectConstruct,
                             .definedFailure = instruction.definedFailure,
                             .constructorTarget = instruction.constructorTarget,
                             .constructorKind = instruction.constructorKind,
                             .info = instruction.info,
                             .lifecycle = instruction.lifecycle};
  return exactWithIntrinsicCallerContract(program, instruction, exact);
}

bool rawPointerConversion(const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Compute ||
      instruction.operation != MirOperation::Convert ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.operands.size() != 1 || !instruction.result ||
      instruction.operands.front().kind != MirOperandKind::Value ||
      instruction.intrinsic != IntrinsicKind::None || instruction.loan ||
      instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      !instruction.lifecycle.empty() || instruction.ownership ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty())
    return false;
  const auto &source = instruction.operands.front().type;
  const auto &target = instruction.info.type;
  return source.kind == SemanticType::RawPointer &&
         target.kind == SemanticType::RawPointer &&
         source.arguments.size() == 1 && target.arguments.size() == 1 &&
         (source.pointerAccess == AccessMode::Mutable ||
          target.pointerAccess == AccessMode::ReadOnly) &&
         (source.arguments.front() == target.arguments.front() ||
          source.arguments.front() == SemanticType::Void ||
          target.arguments.front() == SemanticType::Void);
}

namespace {

[[nodiscard]] std::optional<std::uint64_t>
signedMinimumMagnitude(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Int8:
    return std::uint64_t{1} << 7U;
  case SemanticType::Int16:
    return std::uint64_t{1} << 15U;
  case SemanticType::Int32:
    return std::uint64_t{1} << 31U;
  case SemanticType::Int64:
    return std::uint64_t{1} << 63U;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] bool integerLiteralCarrierCanRepresent(const SemanticType &type,
                                                     std::uint64_t magnitude) {
  switch (type.kind) {
  case SemanticType::Int8:
    return magnitude <= static_cast<std::uint64_t>(INT8_MAX);
  case SemanticType::Int16:
    return magnitude <= static_cast<std::uint64_t>(INT16_MAX);
  case SemanticType::Int32:
    return magnitude <= static_cast<std::uint64_t>(INT32_MAX);
  case SemanticType::Int64:
    return magnitude <= static_cast<std::uint64_t>(INT64_MAX);
  case SemanticType::UInt8:
    return magnitude <= UINT8_MAX;
  case SemanticType::UInt16:
    return magnitude <= UINT16_MAX;
  case SemanticType::UInt32:
    return magnitude <= UINT32_MAX;
  case SemanticType::UInt64:
    return true;
  default:
    return false;
  }
}

} // namespace

// GTI parses the signed minimum as a positive magnitude followed by unary
// negation. The magnitude is intentionally one past the positive range, so it
// has no independently spellable value of the result type. Fuse the exact
// one-use source chain. Parentheses can retain an explicit MIR Identity
// between the magnitude and negation; those identities must be exact,
// failure-free, same-type, and single-use. Program-constant substitution may
// retain the result type on the magnitude, while a contextually typed direct
// literal may use a wider integer carrier that can represent the positive
// magnitude.
[[nodiscard]] const MirInstruction *
fusedSignedMinimumLiteral(const MirBody &body, const MirInstruction &negation) {
  if (negation.kind != MirInstructionKind::Compute ||
      negation.operation != MirOperation::Negate || !negation.result ||
      negation.operands.size() != 1 ||
      negation.operands.front().kind != MirOperandKind::Value ||
      !negation.localFailureSites.empty() || !negation.definedFailure.empty()) {
    return nullptr;
  }
  const MirInstruction *consumer = &negation;
  const MirValue *operand = body.findValue(negation.operands.front().value);
  const MirInstruction *literal = nullptr;
  while (operand != nullptr) {
    const MirInstruction *producer = definitionFor(body, operand->id);
    if (producer == nullptr || !producer->result ||
        *producer->result != operand->id) {
      return nullptr;
    }
    const std::vector<MirValueUse> &uses = body.usesOf(operand->id);
    if (uses.size() != 1 ||
        uses.front().kind != MirValueUseKind::InstructionOperand ||
        uses.front().instruction != consumer->id ||
        uses.front().operandIndex != 0) {
      return nullptr;
    }
    if (producer->operation != MirOperation::Identity) {
      literal = producer;
      break;
    }
    if (producer->kind != MirInstructionKind::Compute ||
        producer->operands.size() != 1 ||
        producer->operands.front().kind != MirOperandKind::Value ||
        producer->info.type != producer->operands.front().type ||
        !producer->localFailureSites.empty() ||
        !producer->definedFailure.empty()) {
      return nullptr;
    }
    consumer = producer;
    operand = body.findValue(producer->operands.front().value);
  }
  const std::optional<std::uint64_t> minimum =
      signedMinimumMagnitude(negation.info.type);
  const auto *magnitude = literal != nullptr && literal->literal
                              ? std::get_if<std::uint64_t>(&*literal->literal)
                              : nullptr;
  if (literal == nullptr || literal->kind != MirInstructionKind::Compute ||
      literal->operation != MirOperation::Literal || !literal->result ||
      operand == nullptr || *literal->result != operand->id ||
      literal->info.type != negation.operands.front().type ||
      !literal->localFailureSites.empty() || !literal->definedFailure.empty() ||
      !minimum || magnitude == nullptr || *magnitude != *minimum ||
      (literal->info.type != negation.info.type &&
       !integerLiteralCarrierCanRepresent(literal->info.type, *magnitude))) {
    return nullptr;
  }
  return literal;
}

[[nodiscard]] const MirInstruction *
fusedSignedMinimumNegation(const MirBody &body, const MirInstruction &carrier) {
  const auto ownsCarrier = [&](const MirInstruction &candidate) {
    const MirInstruction *literal = fusedSignedMinimumLiteral(body, candidate);
    if (literal == nullptr) {
      return false;
    }
    const MirValue *operand = body.findValue(candidate.operands.front().value);
    while (operand != nullptr) {
      const MirInstruction *producer = definitionFor(body, operand->id);
      if (producer == nullptr) {
        return false;
      }
      if (producer == &carrier) {
        return true;
      }
      if (producer == literal ||
          producer->operation != MirOperation::Identity ||
          producer->operands.size() != 1 ||
          producer->operands.front().kind != MirOperandKind::Value) {
        return false;
      }
      operand = body.findValue(producer->operands.front().value);
    }
    return false;
  };
  // Every link of a fused chain has exactly one use, at operand zero of the
  // next link, so at most one negation can own a carrier: the first
  // non-Identity reached by following those uses forward. Confirm it with the
  // negation's own chain. Searching every negation instead would make each
  // query cost the whole body.
  const MirInstruction *current = &carrier;
  for (std::size_t depth = 0; depth <= body.values.size(); ++depth) {
    if (!current->result) {
      return nullptr;
    }
    const std::vector<MirValueUse> &uses = body.usesOf(*current->result);
    if (uses.size() != 1 ||
        uses.front().kind != MirValueUseKind::InstructionOperand ||
        uses.front().operandIndex != 0) {
      return nullptr;
    }
    const MirBlock *block = body.findBlock(uses.front().block);
    const auto consumer =
        block == nullptr
            ? std::vector<MirInstruction>::const_iterator{}
            : std::find_if(block->instructions.begin(),
                           block->instructions.end(),
                           [&](const MirInstruction &instruction) {
                             return instruction.id == uses.front().instruction;
                           });
    if (block == nullptr || consumer == block->instructions.end()) {
      break;
    }
    if (consumer->operation != MirOperation::Identity) {
      return ownsCarrier(*consumer) ? &*consumer : nullptr;
    }
    current = &*consumer;
  }
  // The use index did not locate a consumer; search every negation.
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &candidate : block.instructions) {
      if (ownsCarrier(candidate)) {
        return &candidate;
      }
    }
  }
  return nullptr;
}

const MirInstruction *definitionFor(const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirBlock *block =
      value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
  if (block == nullptr) {
    return nullptr;
  }
  if (const auto indexed = indexedInstruction(body, *block, value->definition))
    return *indexed;
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [&](const MirInstruction &instruction) {
                     return instruction.id == value->definition;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

std::optional<std::size_t> entryProjectionPrefix(
    const MirBody &body, const MirPlace &entry,
    std::span<const MirPlaceProjection> selected) {
  if (entry.projections.empty())
    return 0;
  if (entry.root != MirPlaceRootKind::Binding ||
      !std::all_of(entry.projections.begin(), entry.projections.end(), [](const auto &step) {
        return step.kind == MirProjectionKind::PackElement;
      }) || mirConcretePackElementType(body, entry) == nullptr ||
      selected.size() < entry.projections.size() ||
      !std::equal(entry.projections.begin(), entry.projections.end(), selected.begin()))
    return std::nullopt;
  return entry.projections.size();
}

bool wholeEntryPlaceMatches(const MirBody &body, const MirPlace &entry,
                            const MirPlace &selected) {
  if ((entry.root != MirPlaceRootKind::Binding &&
       entry.root != MirPlaceRootKind::This) ||
      entry.root != selected.root || entry.binding != selected.binding ||
      entry.symbol != selected.symbol)
    return false;
  const auto prefix = entryProjectionPrefix(body, entry, selected.projections);
  if (!prefix)
    return false;
  const auto suffix = std::span(selected.projections).subspan(*prefix);
  if (suffix.empty())
    return equivalentLifetimeTypes(entry.type, selected.type);
  return suffix.size() == 1 &&
         suffix.front() == MirPlaceProjection{.kind = MirProjectionKind::Dereference} &&
         entry.type.kind == SemanticType::Reference && entry.type.arguments.size() == 1 &&
         equivalentLifetimeTypes(entry.type.arguments.front(), selected.type);
}

std::optional<MirPlaceId> borrowSourceForPlace(const MirBody &body,
                                               MirPlaceId placeId,
                                               std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size()) {
    return std::nullopt;
  }
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return std::nullopt;
  }
  if (place->root == MirPlaceRootKind::Loan) {
    const MirLoan *loan = body.findLoan(place->loan);
    return loan == nullptr ? std::nullopt
                           : std::optional<MirPlaceId>{loan->source};
  }
  if (place->root == MirPlaceRootKind::Value) {
    const std::optional<MirPlaceId> source =
        borrowSourceForValue(body, place->value, depth + 1);
    return source ? source : std::optional<MirPlaceId>{placeId};
  }
  if (place->root == MirPlaceRootKind::Binding) {
    if (!place->projections.empty() &&
        place->projections.front().kind == MirProjectionKind::PackElement &&
        !mirConcretePackElementType(body, *place))
      return std::nullopt;
    const MirLoan *carrier = nullptr;
    bool hasReferenceEntry = false;
    bool projectedEntry = false;
    for (const MirLoan &loan : body.loans) {
      if (std::find(loan.carriers.begin(), loan.carriers.end(),
                    place->binding) == loan.carriers.end()) {
        continue;
      }
      if (loan.entry && loan.kind == MirLoanKind::Parameter) {
        hasReferenceEntry = true;
        const auto *entry = body.findPlace(loan.source);
        const auto prefix = entry && entry->root == place->root &&
            entry->binding == place->binding && entry->symbol == place->symbol
            ? entryProjectionPrefix(body, *entry, place->projections) : std::nullopt;
        if (!prefix) continue;
        const auto suffix = std::span(place->projections).subspan(*prefix);
        const bool whole = suffix.empty() && entry->type == place->type;
        const bool referent = suffix.size() == 1 &&
            suffix.front() == MirPlaceProjection{.kind = MirProjectionKind::Dereference} &&
            entry->type.kind == SemanticType::Reference && entry->type.arguments.size() == 1 &&
            entry->type.arguments.front() == place->type;
        // Field/payload paths below this reference keep their own identity.
        // The physical formal is a lifetime footprint, not their address.
        projectedEntry = projectedEntry || (!whole && !referent);
      }
      if (carrier != nullptr && carrier->source != loan.source) {
        return std::nullopt;
      }
      carrier = &loan;
    }
    if (carrier != nullptr) {
      return projectedEntry ? placeId : carrier->source;
    }
    if (hasReferenceEntry)
      return std::nullopt;
  }
  // A by-reference capture has an environment slot rather than a binding
  // carrier. Only its exact whole referent resolves to the slot's entry loan;
  // fields and elements must retain their selected-place identity.
  if (place->root == MirPlaceRootKind::Symbol && place->capture != 0 &&
      place->projections.size() == 1 &&
      place->projections.front().kind == MirProjectionKind::Dereference) {
    for (const auto &loan : body.loans) {
      const auto *slot = body.findPlace(loan.source);
      if (loan.entry && loan.kind == MirLoanKind::Parameter && !loan.parent &&
          slot && slot->root == MirPlaceRootKind::Symbol &&
          slot->symbol == place->symbol && slot->capture == place->capture &&
          slot->projections.empty() &&
          slot->type.kind == SemanticType::Reference &&
          slot->type.arguments.size() == 1 &&
          slot->type.arguments.front() == place->type)
        return slot->id;
    }
  }
  return placeId;
}

std::optional<MirPlaceId> borrowSourceForOperand(const MirBody &body,
                                                 const MirOperand &operand,
                                                 std::size_t depth) {
  switch (operand.kind) {
  case MirOperandKind::Loan: {
    const MirLoan *loan = body.findLoan(operand.loan);
    return loan == nullptr ? std::nullopt
                           : std::optional<MirPlaceId>{loan->source};
  }
  case MirOperandKind::Address:
  case MirOperandKind::Copy:
  case MirOperandKind::Move:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
    return borrowSourceForPlace(body, operand.place, depth + 1);
  case MirOperandKind::Value:
    return borrowSourceForValue(body, operand.value, depth + 1);
  case MirOperandKind::Constant:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<MirPlaceId> borrowSourceForValue(const MirBody &body,
                                               MirValueId valueId,
                                               std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size()) {
    return std::nullopt;
  }
  const MirInstruction *definition = definitionFor(body, valueId);
  if (definition == nullptr) {
    return std::nullopt;
  }
  if (definition->loan) {
    const MirLoan *loan = body.findLoan(*definition->loan);
    if (loan != nullptr) {
      return loan->source;
    }
  }
  if (definition->operands.size() == 1) {
    return borrowSourceForOperand(body, definition->operands.front(),
                                  depth + 1);
  }
  return std::nullopt;
}

const MirLoan *rawBorrowResultLoan(const MirBody &body,
                                   const MirInstruction &instruction) {
  const bool mutableBorrow =
      instruction.intrinsic == IntrinsicKind::RawBorrowMut;
  if ((!mutableBorrow && instruction.intrinsic != IntrinsicKind::RawBorrow) ||
      instruction.kind != MirInstructionKind::Call || !instruction.result ||
      instruction.functionTarget || instruction.receiver ||
      instruction.destination || instruction.operands.size() != 2 ||
      instruction.parameterTypes.size() != 2 ||
      instruction.unsafeOperation != UnsafeOperationKind::RawDereference ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !instruction.lifecycle.empty() || !instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::Argument ||
      instruction.borrowArgument != 1 || !instruction.borrowArguments.empty() ||
      !instruction.borrowFields.empty() || instruction.borrowPlace)
    return nullptr;
  const auto access =
      mutableBorrow ? AccessMode::Mutable : AccessMode::ReadOnly;
  const auto &pointer = instruction.operands[0];
  const auto &witness = instruction.operands[1];
  const auto *address = pointer.kind == MirOperandKind::Value
                            ? body.findValue(pointer.value)
                            : nullptr;
  const auto source = borrowSourceForOperand(body, witness);
  const auto *loan = body.findLoan(*instruction.loan);
  const auto *parent = witness.kind == MirOperandKind::Loan
                           ? body.findLoan(witness.loan)
                           : nullptr;
  if (!address || address->info.type != pointer.type ||
      pointer.type.kind != SemanticType::RawPointer ||
      pointer.type.arguments.size() != 1 ||
      pointer.type.arguments.front() == SemanticType::Void ||
      (mutableBorrow && pointer.type.pointerAccess != AccessMode::Mutable) ||
      instruction.parameterTypes[0] != pointer.type ||
      instruction.parameterTypes[1].kind != SemanticType::Reference ||
      instruction.parameterTypes[1].arguments.size() != 1 ||
      instruction.parameterTypes[1].referenceAccess != access ||
      witness.type != instruction.parameterTypes[1] ||
      instruction.info.type != pointer.type.arguments.front() ||
      instruction.info.category != ValueCategory::Place ||
      instruction.info.access != access || instruction.borrowAccess != access ||
      !source || !loan || loan->kind != MirLoanKind::CallResult ||
      loan->source != *source || loan->access != access ||
      loan->producedBy != instruction.hirValue || loan->entry ||
      loan->escapes || loan->lifecycleBound ||
      (witness.kind == MirOperandKind::Loan &&
       (!parent || loan->parent != witness.loan ||
        (mutableBorrow && parent->access != AccessMode::Mutable))))
    return nullptr;
  return loan;
}

const MirInstruction *callResultReferenceAliases(const MirBody &body,
                                                 const MirLoan &loan,
                                                 const MirBlock &consumer,
                                                 std::size_t consumerPosition) {
  if (loan.kind != MirLoanKind::CallResult || loan.entry || loan.escapes ||
      loan.carriers.empty() || loan.producedBy == 0 ||
      consumerPosition > consumer.instructions.size())
    return nullptr;
  const MirInstruction *producer = nullptr;
  const MirBlock *producerBlock = nullptr;
  std::size_t producerPosition = 0;
  for (const auto &block : body.blocks)
    for (std::size_t position = 0; position < block.instructions.size();
         ++position) {
      const auto &instruction = block.instructions[position];
      if (instruction.loan != loan.id ||
          instruction.kind == MirInstructionKind::EndBorrow ||
          instruction.kind == MirInstructionKind::ActivateBorrow)
        continue;
      if (producer || instruction.kind != MirInstructionKind::Call ||
          !instruction.result || instruction.hirValue != loan.producedBy ||
          instruction.info.category != ValueCategory::Place ||
          instruction.info.access != loan.access ||
          instruction.borrowAccess != loan.access)
        return nullptr;
      producer = &instruction;
      producerBlock = &block;
      producerPosition = position;
    }
  if (!producer)
    return nullptr;
  const auto precedes = [&](const MirBlock &first, std::size_t firstPosition,
                            const MirBlock &second,
                            std::size_t secondPosition) {
    if (first.id == second.id)
      return firstPosition < secondPosition;
    const auto dominance = computeMirDominance(body);
    return dominance && dominance->dominates(first.id, second.id);
  };
  std::unordered_set<HirBindingId> seen;
  for (const auto binding : loan.carriers) {
    if (!binding || !seen.insert(binding).second)
      return nullptr;
    const MirPlace *root = nullptr;
    for (const auto &place : body.places)
      if (place.root == MirPlaceRootKind::Binding && place.binding == binding &&
          place.projections.empty()) {
        if (root)
          return nullptr;
        root = &place;
      }
    if (!root || root->initiallyAvailable ||
        root->type.kind != SemanticType::Reference ||
        root->type.arguments.size() != 1 ||
        root->type.arguments.front() != producer->info.type ||
        (root->type.referenceAccess == AccessMode::Mutable &&
         loan.access != AccessMode::Mutable))
      return nullptr;
    const auto exposesStorage = [&](const MirOperand &operand) {
      const auto *place = body.findPlace(operand.place);
      return place && place->root == MirPlaceRootKind::Binding &&
             place->binding == binding && place->projections.empty();
    };
    const MirInstruction *initializer = nullptr;
    const MirBlock *initialBlock = nullptr;
    std::size_t initialPosition = 0;
    for (const auto &block : body.blocks) {
      if ((block.terminator.value && exposesStorage(*block.terminator.value)) ||
          std::any_of(block.terminator.targetArguments.begin(),
                      block.terminator.targetArguments.end(), exposesStorage) ||
          std::any_of(block.terminator.elseArguments.begin(),
                      block.terminator.elseArguments.end(), exposesStorage) ||
          std::any_of(
              block.terminator.switchTargets.begin(),
              block.terminator.switchTargets.end(), [&](const auto &target) {
                return std::any_of(target.arguments.begin(),
                                   target.arguments.end(), exposesStorage);
              }))
        return nullptr;
      for (std::size_t position = 0; position < block.instructions.size();
           ++position) {
        const auto &instruction = block.instructions[position];
        if ((instruction.receiver && exposesStorage(*instruction.receiver)) ||
            std::any_of(instruction.operands.begin(),
                        instruction.operands.end(), exposesStorage))
          return nullptr;
        const auto *destination = instruction.destination
                                      ? body.findPlace(*instruction.destination)
                                      : nullptr;
        if (!destination || destination->root != MirPlaceRootKind::Binding ||
            destination->binding != binding ||
            !destination->projections.empty())
          continue;
        if (initializer || instruction.kind != MirInstructionKind::Initialize ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Loan ||
            instruction.operands.front().loan != loan.id ||
            instruction.operands.front().value != 0 ||
            instruction.operands.front().type != root->type.arguments.front())
          return nullptr;
        // A lowered reference initializer may publish its runtime address
        // explicitly. It must be the very same unprojected CallResult, never
        // another carrier or a selected subobject with the same owner loan.
        if (instruction.operands.front().place != 0) {
          const auto *referent =
              body.findPlace(instruction.operands.front().place);
          if (!referent || referent->root != MirPlaceRootKind::Loan ||
              referent->loan != loan.id || !referent->projections.empty() ||
              referent->type != producer->info.type ||
              referent->access != loan.access)
            return nullptr;
        }
        initializer = &instruction;
        initialBlock = &block;
        initialPosition = position;
      }
    }
    if (!initializer ||
        !precedes(*producerBlock, producerPosition, *initialBlock,
                  initialPosition) ||
        !precedes(*initialBlock, initialPosition, consumer, consumerPosition))
      return nullptr;
  }
  return producer;
}

bool restoredReferenceReturnWitness(const MirBody &body, const MirLoan &loan) {
  const auto *witness = body.findLoan(loan.storedDependency);
  const auto *source = witness ? body.findPlace(witness->source) : nullptr;
  return witness && source && !loan.lifecycleBound &&
         loan.additionalDependencies.empty() && loan.parent == witness->id &&
         witness->kind == MirLoanKind::Parameter && witness->entry &&
         !witness->escapes && !witness->lifecycleBound &&
         witness->additionalDependencies.empty() && witness->parent == 0 &&
         witness->storedDependency == 0 && loan.source == witness->source &&
         loan.access == witness->access && loan.designatedFields.empty() &&
         witness->designatedFields.empty() &&
         source->root == MirPlaceRootKind::Binding &&
         source->projections.empty() && source->initiallyAvailable &&
         source->type.kind == SemanticType::Reference &&
         source->type.arguments.size() == 1 &&
         source->type.referenceAccess == witness->access &&
         witness->carriers == std::vector<HirBindingId>{source->binding} &&
         restoredBorrowAuthority(body, loan);
}

bool restoredReferenceReturnAliases(const MirBody &body, const MirLoan &loan,
                                    const MirBlock &consumer,
                                    std::size_t consumerPosition) {
  return restoredReferenceReturnWitness(body, loan) &&
         callResultReferenceAliases(body, loan, consumer, consumerPosition);
}

const MirInstruction *restoredBorrowAuthority(const MirBody &body,
                                              const MirLoan &loan) {
  const auto *witness = body.findLoan(loan.storedDependency);
  if (!witness || witness->id == loan.id ||
      (loan.access == AccessMode::Mutable &&
       witness->access != AccessMode::Mutable))
    return nullptr;
  // Resolve only the actual reference capability passed to restoration.
  // Staging preserves that identity; a different loan with the same lifetime
  // supplier is not the opaque input witness.
  const auto inputLoan = [&](const auto &self, const MirOperand &operand,
                             std::size_t depth) -> MirLoanId {
    if (depth > body.values.size())
      return 0;
    if (operand.kind == MirOperandKind::Loan)
      return body.findLoan(operand.loan) ? operand.loan : 0;
    if (operand.kind != MirOperandKind::Value)
      return 0;
    const auto *definition = definitionFor(body, operand.value);
    if (!definition || definition->tombstone)
      return 0;
    if (definition->kind == MirInstructionKind::CallInput &&
        definition->operands.size() == 1 &&
        (definition->callInputKind == HirCallInputKind::ReadBorrow ||
         definition->callInputKind == HirCallInputKind::MutableBorrow))
      return self(self, definition->operands.front(), depth + 1);
    if (definition->kind == MirInstructionKind::Call && definition->loan &&
        definition->info.category == ValueCategory::Place)
      return *definition->loan;
    return 0;
  };
  std::unordered_set<MirLoanId> visiting;
  const auto trace = [&](const auto &self,
                         const MirLoan &current) -> const MirInstruction * {
    if (!visiting.insert(current.id).second ||
        current.storedDependency != witness->id)
      return nullptr;
    const MirInstruction *producer = nullptr;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.loan == current.id &&
            (instruction.kind == MirInstructionKind::Call ||
             instruction.kind == MirInstructionKind::Borrow)) {
          if (producer)
            return nullptr;
          producer = &instruction;
        }
    if (producer && producer->kind == MirInstructionKind::Call &&
        producer->restoresReferenceResult &&
        producer->borrowOrigin == BorrowOriginKind::Argument &&
        producer->borrowArgument < producer->operands.size() &&
        producer->borrowArgument < producer->parameterTypes.size() &&
        producer->parameterTypes[producer->borrowArgument].kind ==
            SemanticType::Reference &&
        current.kind == MirLoanKind::CallResult && !current.entry &&
        !current.escapes && current.parent == witness->id &&
        current.producedBy == producer->hirValue &&
        current.source == witness->source &&
        inputLoan(inputLoan, producer->operands[producer->borrowArgument], 0) ==
            witness->id)
      return producer;
    if (producer && producer->kind == MirInstructionKind::Borrow &&
        producer->operands.size() == 1 &&
        producer->operands.front().kind == MirOperandKind::Loan) {
      const auto *input = body.findLoan(producer->operands.front().loan);
      if (input && input->source == current.source &&
          (current.access == AccessMode::ReadOnly ||
           input->access == AccessMode::Mutable))
        if (const auto *authority = self(self, *input))
          return authority;
    }
    const auto *parent = body.findLoan(current.parent);
    return parent && parent->id != witness->id ? self(self, *parent) : nullptr;
  };
  return trace(trace, loan);
}

const MirLoan *erasedReferenceResultLoan(const MirProgram &program,
                                         const MirBody &body,
                                         const MirInstruction &instruction) {
  const bool restoring =
      instruction.intrinsic == IntrinsicKind::RestoreReference ||
      instruction.intrinsic == IntrinsicKind::RestoreReferenceMut;
  const bool erasing =
      instruction.intrinsic == IntrinsicKind::EraseReference ||
      instruction.intrinsic == IntrinsicKind::EraseReferenceMut;
  const auto access =
      instruction.intrinsic == IntrinsicKind::EraseReferenceMut ||
              instruction.intrinsic == IntrinsicKind::RestoreReferenceMut
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  if ((!restoring && !erasing) ||
      instruction.kind != MirInstructionKind::Call || !instruction.result ||
      !instruction.loan || instruction.functionTarget || instruction.receiver ||
      instruction.destination || instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1 ||
      instruction.parameterTypes.front().kind != SemanticType::Reference ||
      instruction.parameterTypes.front().arguments.size() != 1 ||
      instruction.parameterTypes.front().referenceAccess != access ||
      instruction.operands.front().type != instruction.parameterTypes.front() ||
      instruction.info.category != ValueCategory::Place ||
      instruction.info.access != access ||
      instruction.borrowOrigin != BorrowOriginKind::Argument ||
      instruction.borrowArgument != 0 || instruction.borrowAccess != access ||
      !instruction.borrowArguments.empty() ||
      !instruction.borrowFields.empty() || instruction.borrowPlace ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      instruction.unsafeOperation !=
          (restoring ? UnsafeOperationKind::ErasedReferenceRestore
                     : UnsafeOperationKind::None))
    return nullptr;
  const auto signature = instantiateMirLifetimeTarget(program, instruction);
  if (!signature || signature->parameterTypes != instruction.parameterTypes ||
      signature->returnType.kind != SemanticType::Reference ||
      signature->returnType.arguments.size() != 1 ||
      signature->returnType.referenceAccess != access ||
      signature->returnType.arguments.front() != instruction.info.type ||
      signature->returnType.referenceLifetime !=
          instruction.parameterTypes.front().referenceLifetime)
    return nullptr;
  const auto &opaque =
      restoring ? instruction.parameterTypes.front().arguments.front()
                : instruction.info.type;
  const auto owner = mirClassLifetimeView(program, opaque);
  const bool exactOpaque = owner && opaque.arguments.empty() &&
      owner->instance->compilerCapability == CompilerCapabilityTypeKind::ErasedObject;
  const auto source =
      borrowSourceForOperand(body, instruction.operands.front());
  const auto *loan = body.findLoan(*instruction.loan);
  if (!exactOpaque || !source || !loan ||
      loan->kind != MirLoanKind::CallResult || loan->source != *source ||
      loan->access != access || loan->producedBy != instruction.hirValue ||
      loan->entry || loan->escapes || loan->lifecycleBound)
    return nullptr;
  return loan;
}

const SemanticType *functionPointerSignature(const MirProgram &program,
                                             const SemanticType &type) {
  if (type.kind != SemanticType::Class || type.arguments.size() != 1 ||
      !type.valueArguments.empty())
    return nullptr;
  const auto owner = mirClassLifetimeView(program, type);
  if (!owner ||
      owner->instance->compilerCapability != CompilerCapabilityTypeKind::FunctionPointer)
    return nullptr;
  const auto &signature = type.arguments.front();
  return signature.kind == SemanticType::FunctionSignature &&
                 !signature.arguments.empty() &&
                 signature.signatureReceiver == ReceiverMutability::ReadOnly
             ? &signature
             : nullptr;
}

bool textFieldResult(const SemanticType &returnType, BorrowOriginKind origin,
                     std::size_t parameter,
                     std::span<const BorrowedFieldOrigin> fields) {
  if ((returnType.kind != SemanticType::StringView &&
       returnType.kind != SemanticType::CString) ||
      fields.size() != 1)
    return false;
  const BorrowedFieldOrigin &mapping = fields.front();
  return mapping.resultFields.empty() && mapping.sourceFields.size() == 1 &&
         mapping.sourceFields.front().kind == PlaceProjectionKind::Field &&
         mapping.resultConditions.empty() && mapping.sourceConditions.empty() &&
         mapping.access == AccessMode::ReadOnly && mapping.origin == origin &&
         mapping.parameter == parameter &&
         (origin == BorrowOriginKind::Receiver ||
          origin == BorrowOriginKind::Argument);
}

std::optional<std::vector<ConcreteFormalInput>>
concreteFormalInputs(const HirBody &body, std::span<const SemanticType> types,
                     std::span<const HirBindingId> bindings) {
  std::vector<ConcreteFormalRoot> roots;
  for (const auto binding : bindings) {
    const HirBinding *found = nullptr;
    for (const auto &candidate : body.bindings) {
      if (candidate.id != binding)
        continue;
      if (found)
        return std::nullopt;
      found = &candidate;
    }
    if (!found)
      return std::nullopt;
    roots.push_back({.binding = binding, .type = found->info.type});
  }
  return deriveConcreteFormalInputs(roots, types);
}

std::optional<std::vector<ConcreteFormalInput>>
concreteFormalInputs(const MirBody &body, std::span<const SemanticType> types,
                     std::span<const HirBindingId> bindings) {
  std::vector<ConcreteFormalRoot> roots;
  for (const auto binding : bindings) {
    const MirPlace *found = nullptr;
    for (const auto &candidate : body.places) {
      if (candidate.root != MirPlaceRootKind::Binding ||
          candidate.binding != binding || !candidate.projections.empty())
        continue;
      if (!candidate.initiallyAvailable || candidate.foreignStorage ||
          (found && (candidate.type != found->type || candidate.symbol != found->symbol ||
                     candidate.key != found->key)))
        return std::nullopt;
      found = &candidate;
    }
    if (!found)
      return std::nullopt;
    roots.push_back({.binding = binding, .type = found->type});
  }
  return deriveConcreteFormalInputs(roots, types);
}

std::optional<MirReturnOriginFormal>
returnOriginFormal(const HirBody &body, std::span<const SemanticType> types,
                   std::span<const HirBindingId> bindings, std::size_t index) {
  const auto inputs = concreteFormalInputs(body, types, bindings);
  return inputs && index < inputs->size() ? std::optional((*inputs)[index]) : std::nullopt;
}
std::optional<MirReturnOriginFormal>
returnOriginFormal(const MirBody &body, std::span<const SemanticType> types,
                   std::span<const HirBindingId> bindings, std::size_t index) {
  const auto inputs = concreteFormalInputs(body, types, bindings);
  return inputs && index < inputs->size() ? std::optional((*inputs)[index]) : std::nullopt;
}

bool formalInputPlace(const MirPlace &place, const ConcreteFormalInput &input) {
  if (place.root != MirPlaceRootKind::Binding || place.binding != input.binding ||
      place.type != input.type || place.projections.size() != input.bindingPrefix.size())
    return false;
  for (std::size_t index = 0; index < input.bindingPrefix.size(); ++index) {
    const auto &expected = input.bindingPrefix[index];
    const auto &actual = place.projections[index];
    if (expected.kind != PlaceProjectionKind::ConstantIndex ||
        actual.kind != MirProjectionKind::PackElement ||
        actual.constantIndex != expected.index || actual.field || actual.index || actual.selection)
      return false;
  }
  return true;
}

bool functionPointerOperation(const MirProgram &program,
                              const MirInstruction &instruction) {
  const bool forming = instruction.intrinsic == IntrinsicKind::FunctionPointerFromItem;
  const auto *callable = checkedMirCallableLifetimeSignature(program, instruction);
  if (!callable || instruction.kind != MirInstructionKind::Call ||
      instruction.functionTarget || instruction.lambdaTarget ||
      instruction.constructorTarget || instruction.bodyTarget ||
      instruction.receiver || instruction.destination ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      instruction.dispatch != CallDispatch::Static || instruction.borrowPlace ||
      instruction.parameterTypes.size() != instruction.operands.size())
    return false;
  if (forming) {
    const auto &application = instruction.lifetimeApplication;
    if (!instruction.result || instruction.operands.size() != 1 || instruction.loan ||
        instruction.borrowOrigin != BorrowOriginKind::None ||
        !instruction.borrowFields.empty() || !instruction.borrowArguments.empty() ||
        instruction.functionPointerFunctionTarget.has_value() ==
            instruction.functionPointerLambdaTarget.has_value() ||
        !instruction.definedFailure.empty() || !instruction.localFailureSites.empty() ||
        !application.bindings.empty() || !application.variables.empty() ||
        !application.obligations.empty() || !application.typeObligations.empty() ||
        !application.origins.empty())
      return false;
    const auto &targetType = instruction.parameterTypes.front();
    if (instruction.functionPointerFunctionTarget) {
      const auto *target = program.findFunctionInstance(*instruction.functionPointerFunctionTarget);
      return target && targetType.hasFunctionItemShape() &&
             targetType.functionId == target->declaration && !target->owner &&
             target->linkage == LanguageLinkage::Gti && !target->requiresUnsafe &&
             target->definitionKind == MirDefinitionKind::Source && !target->tracksCaller &&
             target->entryKind == ProgramEntryKind::None &&
             mirCallableLifetimeTargetMatches(program, *callable, target->returnType,
                                               target->parameterTypes, target->lifetimeContract);
    }
    const auto *target = program.findLambda(*instruction.functionPointerLambdaTarget);
    return target && target->captureTypes.empty() &&
           target->capability == CallableInvocationCapability::Read &&
           mirLambdaCallableLifetimeTargetMatches(program, *callable, *target, targetType);
  }
  if (instruction.functionPointerFunctionTarget || instruction.functionPointerLambdaTarget ||
      instruction.definedFailure.propagation != FailurePropagationKind::Callable)
    return false;
  const auto applied = instantiateMirLifetimeTarget(program, instruction);
  if (!applied || applied->parameterTypes != instruction.parameterTypes)
    return false;
  const auto &result = applied->returnType;
  if (result.kind != SemanticType::Reference)
    return instruction.info.type == result &&
           !instruction.info.traits.containsBorrowedState &&
           instruction.borrowOrigin == BorrowOriginKind::None && !instruction.loan &&
           instruction.borrowArguments.empty() && instruction.borrowFields.empty();
  if (result.arguments.size() != 1 || instruction.info.type != result.arguments.front() ||
      instruction.info.category != ValueCategory::Place ||
      instruction.info.access != result.referenceAccess ||
      instruction.borrowAccess != result.referenceAccess)
    return false;
  // Derive alternatives before invocation substitution: incidental equality
  // of caller regions must never change the signature's supplier contract.
  const auto summary = mirCallableLifetimeBorrowSummary(program, instruction);
  if (!summary)
    return false;
  if (summary->dependencies.empty())
    return result.referenceLifetime == Lifetime::staticLifetime() &&
           instruction.borrowOrigin == BorrowOriginKind::None && !instruction.loan &&
           instruction.borrowArguments.empty() && instruction.borrowFields.empty();
  if (instruction.borrowOrigin != BorrowOriginKind::Argument ||
      instruction.borrowArgument != summary->dependencies.front().parameter)
    return false;
  if (summary->dependencies.size() == 1)
    return instruction.borrowArguments.empty() && instruction.borrowFields.empty();
  std::vector<std::size_t> suppliers;
  for (const auto &dependency : summary->dependencies)
    suppliers.push_back(dependency.parameter);
  return instruction.borrowArguments == suppliers &&
         instruction.borrowFields == summary->dependencies;
}

const MirLoan *textAddressResultLoan(const MirBody &body,
                                     const MirInstruction &instruction,
                                     const MirProgram *program) {
  const bool counted =
      instruction.intrinsic == IntrinsicKind::TextViewFromAddress;
  const std::size_t witness = counted ? 2 : 1;
  if ((!counted &&
       instruction.intrinsic != IntrinsicKind::CStringFromAddress) ||
      instruction.kind != MirInstructionKind::Call || !instruction.result ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.receiver || instruction.destination ||
      instruction.callableInvocation || instruction.callableBoundary ||
      !instruction.callableArguments.empty() ||
      instruction.dispatch != CallDispatch::Static ||
      instruction.operation != MirOperation::None ||
      instruction.operands.size() != witness + 1 ||
      instruction.parameterTypes.size() != witness + 1 ||
      instruction.unsafeOperation != UnsafeOperationKind::RawDereference ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !instruction.safetyDischarges.empty() || !instruction.lifecycle.empty() ||
      !instruction.loan || instruction.callSite == 0 ||
      instruction.borrowOrigin != BorrowOriginKind::Argument ||
      instruction.borrowArgument != witness ||
      instruction.borrowAccess != AccessMode::ReadOnly ||
      !instruction.borrowArguments.empty() ||
      !instruction.borrowFields.empty() || instruction.borrowPlace ||
      instruction.info.type !=
          (counted ? SemanticType::StringView : SemanticType::CString) ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly)
    return nullptr;
  // No representation, failure, synchronization, lifecycle or future metadata
  // can acquire meaning merely by accompanying a valid carrier assertion.
  MirInstruction exact{.id = instruction.id,
                       .kind = MirInstructionKind::Call,
                       .hirValue = instruction.hirValue,
                       .hirStatement = instruction.hirStatement,
                       .callSite = instruction.callSite,
                       .unsafeOperation = UnsafeOperationKind::RawDereference,
                       .result = instruction.result,
                       .operands = instruction.operands,
                       .parameterTypes = instruction.parameterTypes,
                       .loan = instruction.loan,
                       .borrowOrigin = BorrowOriginKind::Argument,
                       .borrowArgument = witness,
                       .borrowAccess = AccessMode::ReadOnly,
                       .intrinsic = instruction.intrinsic,
                       .info = instruction.info};
  // The call's lifetime application is region proof metadata. Lifetime
  // verification checks it independently against the intrinsic's contract; it
  // grants no representation, so the carrier shape carries it through as is.
  exact.lifetimeApplication = instruction.lifetimeApplication;
  auto traits = semanticTraits(instruction.info.type);
  traits.ownership = OwnershipKind::Borrowed;
  traits.copyAssignable = false;
  traits.moveAssignable = false;
  traits.containsBorrowedState = true;
  traits.transferCapable = false;
  traits.shareCapable = false;
  // Body verification has no artifact table. It checks the source snapshot
  // and forwarding relation here; program verification and every backend
  // caller additionally require the canonical artifact-local caller anchor.
  bool exactShape = false;
  if (program) {
    exactShape = exactWithIntrinsicCallerContract(*program, instruction, exact);
  } else {
    if (const auto &contract = instruction.intrinsicCallContract; contract) {
      if (!contract->declaration() ||
          contract->snapshot() != body.placeDomain.snapshot ||
          contract->intrinsic() != instruction.intrinsic ||
          contract->neverReturns() || !contract->tracksCaller() ||
          !instruction.tracksCaller || !instruction.callerLocation ||
          !instruction.callerLocationSite ||
          instruction.forwardsCallerLocation != body.hasCallerLocation)
        return nullptr;
      exact.intrinsicCallContract = contract;
      exact.tracksCaller = instruction.tracksCaller;
      exact.callerLocation = instruction.callerLocation;
      exact.callerLocationSite = instruction.callerLocationSite;
      exact.forwardsCallerLocation = instruction.forwardsCallerLocation;
    }
    exactShape = instruction == exact;
  }
  if (!exactShape || instruction.info.traits != traits ||
      instruction.info.updateArithmeticType != SemanticType::Unknown)
    return nullptr;
  for (std::size_t index = 0; index <= witness; ++index) {
    const auto &operand = instruction.operands[index];
    const auto *stage = operand.kind == MirOperandKind::Value
                            ? definitionFor(body, operand.value)
                            : nullptr;
    if (!stage || stage->kind != MirInstructionKind::CallInput ||
        stage->result != operand.value ||
        stage->callSite != instruction.callSite ||
        stage->callInputRole != MirCallInputRole::Argument ||
        stage->callInputIndex != index || stage->operands.size() != 1 ||
        stage->callInputKind != (index == witness ? HirCallInputKind::ReadBorrow
                                                  : HirCallInputKind::Value) ||
        stage->info.type != instruction.parameterTypes[index] ||
        operand.type != instruction.parameterTypes[index] || operand.place ||
        operand.loan || operand.literal)
      return nullptr;
  }
  if (instruction.parameterTypes.front() !=
          SemanticType::rawPointerTo(SemanticType::Char,
                                     AccessMode::ReadOnly) ||
      (counted && instruction.parameterTypes[1] != SemanticType::UInt64))
    return nullptr;
  const auto &owner = instruction.parameterTypes[witness];
  if (owner.kind != SemanticType::Reference || owner.arguments.size() != 1 ||
      owner.referenceAccess != AccessMode::ReadOnly)
    return nullptr;
  const auto source =
      borrowSourceForOperand(body, instruction.operands[witness]);
  const auto *loan = body.findLoan(*instruction.loan);
  if (!source || !loan || loan->source != *source ||
      (loan->kind != MirLoanKind::CallResult &&
       loan->kind != MirLoanKind::Stored) ||
      loan->access != AccessMode::ReadOnly ||
      loan->producedBy != instruction.hirValue || loan->entry ||
      loan->escapes || loan->lifecycleBound)
    return nullptr;
  return loan;
}

} // namespace lang::mir_detail
