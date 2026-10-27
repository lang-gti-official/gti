#include "semantic_analyzer_impl.h"

#include <stdexcept>

namespace lang {
namespace {
void preserveStorageConstructorParameters(
    const SemanticModel &model, const Call &call, std::size_t storageArguments,
    std::vector<SemanticType> &parameters) {
  if (const auto *construction = model.findConstruction(call);
      construction != nullptr &&
      construction->kind == ConstructorKind::Ordinary &&
      construction->constructor != 0 && !construction->generatedDefault &&
      parameters.size() >= storageArguments) {
    // The selected signature owns reference/value roles, including when
    // concrete argument expressions still carry a grouped pack type.
    parameters.resize(storageArguments);
    parameters.insert(parameters.end(), construction->parameterTypes.begin(),
                      construction->parameterTypes.end());
  }
}
} // namespace

auto SemanticVisitor::Impl::reportMissingLambdaCapture(const Token &use)
    -> bool {
  if (lambdaUncapturedLocals.empty()) {
    return false;
  }
  const auto found = lambdaUncapturedLocals.back().find(use.lexeme);
  if (found == lambdaUncapturedLocals.back().end()) {
    return false;
  }
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2027", DiagnosticPhase::Semantics, use,
      "Local binding '" + use.lexeme + "' is not captured by this lambda.");
  if (!found->second.lexeme.empty()) {
    diagnostic.related.push_back(
        {tokenSpan(found->second), "Local binding declared here."});
  }
  diagnostic.hints.emplace_back("Add '" + use.lexeme +
                                "' to the lambda capture list.");
  diagnostics.push_back(std::move(diagnostic));
  return true;
}

auto SemanticVisitor::Impl::typeTraits(const SemanticType &type) const
    -> SemanticTypeTraits {
  std::vector<SemanticType> visiting;
  SemanticTypeTraits traits = typeTraits(type, visiting);
  std::vector<SemanticType> capabilityVisiting;
  const ConcurrencyCapabilities capabilities =
      typeCapabilities(type, capabilityVisiting);
  traits.transferCapable = capabilities.transferCapable;
  traits.shareCapable = capabilities.shareCapable;
  return traits;
}

auto SemanticVisitor::Impl::semanticTypeComplexity(const SemanticType &type)
    -> std::size_t {
  std::size_t result = 1;
  for (const SemanticType &argument : type.arguments) {
    result += semanticTypeComplexity(argument);
  }
  return result;
}

auto SemanticVisitor::Impl::nonDecreasingNominalRecurrence(
    const SemanticType &type, std::span<const SemanticType> active) const
    -> bool {
  // Exact cycles remain cycles even across a closed declaration. The growth
  // guard is different: a closed non-generic declaration breaks dependence
  // on the incoming substitution. Wrapper<Envelope> -> Envelope ->
  // Wrapper<Leaf> is finite composition, not a growing Wrapper recurrence.
  if (std::find(active.begin(), active.end(), type) != active.end())
    return true;
  for (auto cursor = active.rbegin(); cursor != active.rend(); ++cursor) {
    if (cursor->kind != SemanticType::Class &&
        cursor->kind != SemanticType::Enum)
      continue;
    const bool samePrimary =
        cursor->kind == type.kind &&
        (type.kind == SemanticType::Class ? cursor->classId == type.classId
                                          : cursor->enumId == type.enumId);
    if (samePrimary) {
      const bool concreteValues =
          !type.valueArguments.empty() &&
          std::all_of(type.valueArguments.begin(), type.valueArguments.end(),
                      [](const auto &value) {
                        return value.kind == CompileTimeValue::UInt64;
                      }) &&
          std::all_of(cursor->valueArguments.begin(),
                      cursor->valueArguments.end(), [](const auto &value) {
                        return value.kind == CompileTimeValue::UInt64;
                      });
      if (concreteValues && active.size() < 64 &&
          semanticTypeComplexity(type) <= semanticTypeComplexity(*cursor))
        return false;
      return semanticTypeComplexity(type) >= semanticTypeComplexity(*cursor);
    }
    if (const ClassInfo *owner = classInfo(*cursor);
        owner != nullptr && owner->genericParameters.empty())
      return false;
    if (cursor->kind == SemanticType::Enum && cursor->enumId != 0 &&
        cursor->enumId <= enums.size() &&
        enums[cursor->enumId - 1].genericParameters.empty())
      return false;
  }
  return false;
}

auto SemanticVisitor::Impl::lambdaTraitsForCaptures(
    std::span<const SemanticType> captureTypes) const -> SemanticTypeTraits {
  SemanticTypeTraits result;
  result.drop = DropKind::Trivial;
  // An environment is an aggregate of its captures with no self-reference,
  // base, or source destructor, so it relocates exactly as its weakest
  // capture does: bitwise only when every capture is bitwise.
  result.relocation = RelocationKind::Bitwise;
  for (const SemanticType &captureType : captureTypes) {
    const SemanticTypeTraits capture = typeTraits(captureType);
    if (capture.ownership == OwnershipKind::Shared) {
      result.ownership = OwnershipKind::Shared;
    }
    if (capture.drop == DropKind::Lexical) {
      result.drop = DropKind::Lexical;
    }
    result.copyable = result.copyable && capture.copyable;
    result.movable = result.movable && capture.movable;
    result.containsBorrowedState =
        result.containsBorrowedState || capture.containsBorrowedState;
    if (captureType.kind == SemanticType::Reference &&
        captureType.referenceAccess == AccessMode::Mutable)
      result.copyable = false;
    if (capture.relocation == RelocationKind::Unavailable ||
        capture.containsBorrowedState)
      result.relocation = RelocationKind::Unavailable;
    else if (capture.relocation == RelocationKind::Structural &&
             result.relocation == RelocationKind::Bitwise)
      result.relocation = RelocationKind::Structural;
    result.transferCapable = result.transferCapable && capture.transferCapable;
    result.shareCapable = result.shareCapable && capture.shareCapable;
  }
  if (result.containsBorrowedState) {
    result.copyAssignable = false;
    result.moveAssignable = false;
  }
  return result;
}

auto SemanticVisitor::Impl::requiresActiveCleanup(
    const SemanticType &type, ActiveCleanupQuery &query) const -> bool {
  if (type.kind == SemanticType::Class) {
    const ClassInfo *owner = classInfo(type);
    if (owner != nullptr &&
        owner->compilerCapability == CompilerCapabilityTypeKind::SharedOwner) {
      return true;
    }
    if (owner != nullptr &&
        owner->compilerCapability == CompilerCapabilityTypeKind::MutexStorage) {
      return !query.excludeNativeRetirement ||
             (type.arguments.size() == 1 &&
              requiresActiveCleanup(type.arguments.front(), query));
    }
    if (owner != nullptr && owner->compilerCapability ==
                                CompilerCapabilityTypeKind::OptionalStorage) {
      return type.arguments.size() == 1 &&
             requiresActiveCleanup(type.arguments.front(), query);
    }
    if (owner != nullptr &&
        (owner->compilerCapability ==
             CompilerCapabilityTypeKind::MutexLockToken ||
         owner->compilerCapability ==
             CompilerCapabilityTypeKind::PolymorphicStorage)) {
      return false;
    }
  }
  switch (type.kind) {
  case SemanticType::UniqueOwner:
  case SemanticType::SharedPointer:
    return true;
  case SemanticType::Array:
  case SemanticType::Lambda:
  case SemanticType::Enum:
  case SemanticType::Class:
    break;
  default:
    return false;
  }

  const auto cached = std::find_if(
      query.entries.begin(), query.entries.end(),
      [&](const ActiveCleanupEntry &entry) { return entry.type == type; });
  if (cached != query.entries.end()) {
    // Active cleanup is an existential structural property. An exact
    // back-edge contributes no new cleanup, while a completed entry is the
    // memoized result for every later sibling that reaches the same type.
    return cached->state == ActiveCleanupState::Present;
  }

  if (type.kind == SemanticType::Class || type.kind == SemanticType::Enum) {
    if ((type.kind == SemanticType::Class &&
         (type.classId == 0 || type.classId > classes.size())) ||
        (type.kind == SemanticType::Enum &&
         (type.enumId == 0 || type.enumId > enums.size()))) {
      return false;
    }
    if (nonDecreasingNominalRecurrence(type, query.activeTypes)) {
      // A by-value generic recurrence that does not structurally decrease
      // has no finite lifecycle shape. Treat it conservatively as requiring
      // active cleanup so forbidden global/static storage fails before HIR
      // instance expansion. Decreasing concrete nests continue to their
      // leaf type.
      query.entries.push_back(
          {.type = type, .state = ActiveCleanupState::Present});
      return true;
    }
  }

  const std::size_t entryIndex = query.entries.size();
  query.entries.push_back(
      {.type = type, .state = ActiveCleanupState::InProgress});
  query.activeTypes.push_back(type);
  bool result = false;
  switch (type.kind) {
  case SemanticType::Array:
    result = type.arguments.size() == 1 &&
             requiresActiveCleanup(type.arguments.front(), query);
    break;
  case SemanticType::Enum: {
    if (type.enumId == 0 || type.enumId > enums.size() ||
        !enums[type.enumId - 1].payload) {
      break;
    }
    const auto concrete = enumTypeFor(type);
    if (!concrete)
      break;
    for (const auto &enumerator : concrete->enumerators) {
      if (std::any_of(enumerator.payloadTypes.begin(),
                      enumerator.payloadTypes.end(),
                      [&](const SemanticType &payload) {
                        return requiresActiveCleanup(payload, query);
                      })) {
        result = true;
        break;
      }
    }
    break;
  }
  case SemanticType::Lambda: {
    if (type.hasLambdaShape()) {
      result = std::any_of(type.lambdaCaptureTypes().begin(),
                           type.lambdaCaptureTypes().end(),
                           [&](const SemanticType &capture) {
                             return requiresActiveCleanup(capture, query);
                           });
    } else {
      const LambdaInfo *lambda = semanticModel.findLambda(type.lambdaId);
      result = lambda != nullptr &&
               std::any_of(lambda->captures.begin(), lambda->captures.end(),
                           [&](const LambdaCaptureInfo &capture) {
                             return requiresActiveCleanup(capture.type, query);
                           });
    }
    break;
  }
  case SemanticType::Class: {
    const ClassInfo &owner = classInfo(type.classId);
    result = hasSourceDestructorBody(owner);
    const GenericSubstitution substitution = classSubstitution(type);
    if (!result) {
      if (const ClassBaseTypeInfo *base = concreteBase(owner);
          base != nullptr) {
        result = requiresActiveCleanup(substituteType(base->type, substitution),
                                       query);
      }
    }
    for (const FieldInfo &field : owner.fields) {
      if (result) {
        break;
      }
      if (field.declaration == nullptr) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
      if (member != owner.members.end()) {
        result = requiresActiveCleanup(
            substituteType(member->second.symbol.type, substitution), query);
      }
    }
    break;
  }
  default:
    break;
  }
  query.entries[entryIndex].state =
      result ? ActiveCleanupState::Present : ActiveCleanupState::Absent;
  query.activeTypes.pop_back();
  return result;
}

bool SemanticVisitor::Impl::requiresSourceCleanupFor(
    const SemanticType &type) const {
  ActiveCleanupQuery query;
  query.excludeNativeRetirement = true;
  return requiresActiveCleanup(type, query);
}

auto SemanticVisitor::Impl::activeCleanupCause(const SemanticType &type) const
    -> std::optional<ActiveCleanupCause> {
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return std::nullopt;
  }
  const ClassInfo &owner = classInfo(type.classId);
  if (hasSourceDestructorBody(owner)) {
    return ActiveCleanupCause{
        .source = &owner.destructor->declaration->tilde(),
        .message = "Declared cleanup makes this type require active "
                   "lifecycle state."};
  }
  const GenericSubstitution substitution = classSubstitution(type);
  ActiveCleanupQuery query;
  query.excludeNativeRetirement = true;
  if (const ClassBaseTypeInfo *base = concreteBase(owner); base != nullptr) {
    if (requiresActiveCleanup(substituteType(base->type, substitution),
                              query) &&
        base->syntax != nullptr) {
      return ActiveCleanupCause{.source = &base->syntax->type.name.last(),
                                .message =
                                    "This base type requires active cleanup."};
    }
  }
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      continue;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      continue;
    }
    const SemanticType fieldType =
        substituteType(member->second.symbol.type, substitution);
    if (requiresActiveCleanup(fieldType, query)) {
      return ActiveCleanupCause{.source = &field.declaration->name(),
                                .message = "Field '" +
                                           field.declaration->name().lexeme +
                                           "' has cleanup-owning type '" +
                                           typeSpelling(fieldType) + "'."};
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::typeCapabilities(
    const SemanticType &type, std::vector<SemanticType> &visiting,
    ThreadTransferBoundary boundary) const -> ConcurrencyCapabilities {
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
  case SemanticType::NullPtr:
    return {.transferCapable = true, .shareCapable = true};
  case SemanticType::Enum: {
    if (type.enumId == 0 || type.enumId > enums.size() ||
        !enums[type.enumId - 1].payload) {
      return {.transferCapable = true, .shareCapable = true};
    }
    ConcurrencyCapabilities result{.transferCapable = true,
                                   .shareCapable = true};
    if (nonDecreasingNominalRecurrence(type, visiting))
      return {};
    const auto concrete = enumTypeFor(type);
    if (!concrete)
      return {};
    visiting.push_back(type);
    for (const auto &enumerator : concrete->enumerators) {
      for (const SemanticType &payload : enumerator.payloadTypes) {
        const ConcurrencyCapabilities component =
            typeCapabilities(payload, visiting, boundary);
        result.transferCapable =
            result.transferCapable && component.transferCapable;
        result.shareCapable = result.shareCapable && component.shareCapable;
      }
    }
    visiting.pop_back();
    return result;
  }
  case SemanticType::Array:
  case SemanticType::UniqueOwner:
    if (type.arguments.size() == 1) {
      return typeCapabilities(type.arguments.front(), visiting, boundary);
    }
    return {};
  case SemanticType::TypeParameter: {
    const GenericConstraintSet constraints = constraintOf(type);
    return {.transferCapable =
                hasConstraint(constraints, GenericConstraintKind::Transferable),
            .shareCapable =
                hasConstraint(constraints, GenericConstraintKind::Shareable)};
  }
  case SemanticType::Lambda: {
    if (type.hasLambdaShape()) {
      const SemanticTypeTraits traits =
          lambdaTraitsForCaptures(type.lambdaCaptureTypes());
      return {.transferCapable = traits.transferCapable,
              .shareCapable = traits.shareCapable};
    }
    const LambdaInfo *lambda = semanticModel.findLambda(type.lambdaId);
    return lambda == nullptr
               ? ConcurrencyCapabilities{}
               : ConcurrencyCapabilities{
                     .transferCapable = lambda->traits.transferCapable,
                     .shareCapable = lambda->traits.shareCapable};
  }
  case SemanticType::FunctionItem:
    return {.transferCapable = true, .shareCapable = true};
  case SemanticType::Class:
    if (const ClassInfo *owner = classInfo(type);
        owner != nullptr &&
        (owner->compilerCapability ==
             CompilerCapabilityTypeKind::OptionalStorage ||
         owner->compilerCapability ==
             CompilerCapabilityTypeKind::UninitializedStorage) &&
        type.arguments.size() == 1) {
      return typeCapabilities(type.arguments.front(), visiting, boundary);
    }
    break;
  case SemanticType::Unknown:
  case SemanticType::Void:
  case SemanticType::StringView:
  case SemanticType::CString:
  case SemanticType::RawPointer:
  case SemanticType::SharedPointer:
  case SemanticType::TypePack:
  case SemanticType::TypeName:
  case SemanticType::Function:
  case SemanticType::FunctionSignature:
  case SemanticType::NativeFunction:
    return {};
  case SemanticType::Reference: {
    if (boundary != ThreadTransferBoundary::Scoped ||
        type.arguments.size() != 1)
      return {};
    // A scoped exclusive loan moves access, not ownership of its referent.
    // Shared access requires sharing. Do not recursively grant scoped
    // permission to the referent: its own concurrency contract still applies.
    std::vector<SemanticType> referentVisiting;
    const auto referent =
        typeCapabilities(type.arguments.front(), referentVisiting);
    return {.transferCapable = type.referenceAccess == AccessMode::Mutable
                                   ? referent.transferCapable
                                   : referent.shareCapable,
            .shareCapable = false};
  }
  }

  if (type.classId == 0 || type.classId > classes.size()) {
    return {};
  }
  if (nonDecreasingNominalRecurrence(type, visiting)) {
    // Recursive nominal state is evaluated as a greatest fixed point. A
    // back-edge contributes no new denial; fields and lifecycle on the
    // enclosing path still decide the final result. Concrete generic nests
    // that become structurally smaller are not back-edges and must inspect
    // the nested instance's fields.
    return {.transferCapable = true, .shareCapable = true};
  }
  visiting.push_back(type);

  const ClassInfo &owner = classInfo(type.classId);
  if (owner.compilerCapability == CompilerCapabilityTypeKind::ErasedObject ||
      owner.compilerCapability == CompilerCapabilityTypeKind::FunctionPointer) {
    visiting.pop_back();
    // Signature erasure does not establish a target's concurrency effects.
    return {};
  }
  if (owner.compilerCapability ==
      CompilerCapabilityTypeKind::PolymorphicStorage) {
    const auto erased = type.arguments.size() == 1
                            ? typeCapabilities(type.arguments.front(), visiting)
                            : ConcurrencyCapabilities{};
    visiting.pop_back();
    // The hidden dynamic payload is constrained by its interface, not by the
    // empty source shell of the compiler-owned slot.
    return erased;
  }
  if (owner.compilerCapability == CompilerCapabilityTypeKind::SharedOwner) {
    const auto element =
        type.arguments.size() == 1
            ? typeCapabilities(type.arguments.front(), visiting)
            : ConcurrencyCapabilities{};
    visiting.pop_back();
    // Another thread can become the final owner. Both concurrent read access
    // and destruction after transfer must therefore be permitted by T.
    const bool allowed = element.transferCapable && element.shareCapable;
    return {.transferCapable = allowed, .shareCapable = allowed};
  }
  if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage) {
    const auto payload =
        type.arguments.size() == 1
            ? typeCapabilities(type.arguments.front(), visiting)
            : ConcurrencyCapabilities{};
    visiting.pop_back();
    // Locking grants exclusive access on whichever thread acquires the lock.
    // Consequently sharing the synchronization owner requires transfer, not
    // concurrent read access, of its payload. A live guard still forbids
    // moving the owner; moving an unlocked owner transfers its payload too.
    return {.transferCapable = payload.transferCapable,
            .shareCapable = payload.transferCapable};
  }
  if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexLockToken) {
    visiting.pop_back();
    return {};
  }
  if (owner.kind == ClassKind::Interface) {
    ConcurrencyCapabilities result{
        .transferCapable =
            owner.transferPolicy == ConcurrencyCapabilityPolicy::Required,
        .shareCapable =
            owner.sharePolicy == ConcurrencyCapabilityPolicy::Required};
    for (const ClassBaseTypeInfo &base : owner.bases) {
      if (!base.interface) {
        continue;
      }
      const ConcurrencyCapabilities inherited =
          typeCapabilities(base.type, visiting);
      result.transferCapable =
          result.transferCapable || inherited.transferCapable;
      result.shareCapable = result.shareCapable || inherited.shareCapable;
    }
    visiting.pop_back();
    return result;
  }

  ConcurrencyCapabilities result{.transferCapable = true, .shareCapable = true};
  const GenericSubstitution substitution = classSubstitution(type);
  const auto merge = [&](const SemanticType &componentType) {
    const ConcurrencyCapabilities component =
        typeCapabilities(componentType, visiting, boundary);
    result.transferCapable =
        result.transferCapable && component.transferCapable;
    result.shareCapable = result.shareCapable && component.shareCapable;
  };
  if (const ClassBaseTypeInfo *base = concreteBase(owner)) {
    merge(substituteType(base->type, substitution));
  }
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      continue;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member != owner.members.end()) {
      merge(substituteType(member->second.symbol.type, substitution));
    }
  }
  if (hasSourceDestructorBody(owner) ||
      (owner.storedReference && boundary == ThreadTransferBoundary::Owned)) {
    result = {};
  }
  if (owner.transferPolicy == ConcurrencyCapabilityPolicy::Denied) {
    result.transferCapable = false;
  } else if (owner.transferPolicy ==
             ConcurrencyCapabilityPolicy::UnsafeAsserted) {
    result.transferCapable = true;
  } else if (owner.transferPolicy ==
             ConcurrencyCapabilityPolicy::UnsafeConditional) {
    result.transferCapable =
        owner.transferCondition &&
        typeCapabilities(substituteType(*owner.transferCondition, substitution),
                         visiting)
            .transferCapable;
  }
  if (owner.sharePolicy == ConcurrencyCapabilityPolicy::Denied) {
    result.shareCapable = false;
  } else if (owner.sharePolicy == ConcurrencyCapabilityPolicy::UnsafeAsserted) {
    result.shareCapable = true;
  } else if (owner.sharePolicy ==
             ConcurrencyCapabilityPolicy::UnsafeConditional) {
    result.shareCapable =
        owner.shareCondition &&
        typeCapabilities(substituteType(*owner.shareCondition, substitution),
                         visiting)
            .shareCapable;
  }
  visiting.pop_back();
  return result;
}

auto SemanticVisitor::Impl::typeTraits(
    const SemanticType &type, std::vector<SemanticType> &visiting) const
    -> SemanticTypeTraits {
  if (type.kind == SemanticType::TypeParameter) {
    SemanticTypeTraits traits = semanticTraits(type);
    // A dependent field keeps the guarantees of its declaration and active
    // requires clause. Without this, an aggregate containing a relocatable T
    // loses that guarantee even though a direct use of T satisfies the concept.
    // Concrete instances are still reanalysed from their actual field types.
    const GenericConstraintSet constraints = constraintOf(type);
    if (!invalidConstraintSet(constraints)) {
      if (constraintImplies(constraints,
                            GenericConstraintKind::BitwiseRelocatable))
        traits.relocation = RelocationKind::Bitwise;
      else if (constraintImplies(constraints,
                                 GenericConstraintKind::Relocatable))
        traits.relocation = RelocationKind::Structural;
    }
    return traits;
  }
  if (type.kind == SemanticType::Array && type.arguments.size() == 1) {
    SemanticTypeTraits traits;
    const SemanticTypeTraits element = typeTraits(type.arguments[0], visiting);
    traits.ownership = element.ownership;
    traits.drop = element.drop;
    traits.copyable = element.copyable;
    traits.movable = element.movable;
    traits.copyAssignable = element.copyAssignable;
    traits.moveAssignable = element.moveAssignable;
    traits.containsBorrowedState = element.containsBorrowedState;
    traits.relocation = element.relocation;
    return traits;
  }

  if (type.kind == SemanticType::Lambda) {
    if (type.hasLambdaShape()) {
      return lambdaTraitsForCaptures(type.lambdaCaptureTypes());
    }
    const LambdaInfo *lambda = semanticModel.findLambda(type.lambdaId);
    return lambda == nullptr ? semanticTraits(type) : lambda->traits;
  }

  if (type.kind == SemanticType::Enum && type.enumId != 0 &&
      type.enumId <= enums.size() && enums[type.enumId - 1].payload) {
    SemanticTypeTraits traits = semanticTraits(type);
    traits.relocation = RelocationKind::Structural;
    if (nonDecreasingNominalRecurrence(type, visiting)) {
      traits.copyable = traits.movable = false;
      traits.copyAssignable = traits.moveAssignable = false;
      traits.relocation = RelocationKind::Unavailable;
      return traits;
    }
    ActiveCleanupQuery cleanupQuery;
    const auto concrete = enumTypeFor(type);
    if (!concrete)
      return traits;
    visiting.push_back(type);
    for (const auto &enumerator : concrete->enumerators) {
      for (const SemanticType &payload : enumerator.payloadTypes) {
        const SemanticTypeTraits component = typeTraits(payload, visiting);
        if (component.ownership == OwnershipKind::Unique ||
            (traits.ownership == OwnershipKind::Value &&
             component.ownership == OwnershipKind::Shared)) {
          traits.ownership = component.ownership;
        }
        if (requiresActiveCleanup(payload, cleanupQuery)) {
          traits.drop = DropKind::Lexical;
        }
        traits.copyable = traits.copyable && component.copyable;
        // An aggregate owns its stored exclusive capability. Copying a
        // reference expression may reborrow, but copying the whole payload
        // must not duplicate that capability (the same rule as class fields).
        if (payload.kind == SemanticType::Reference &&
            payload.referenceAccess == AccessMode::Mutable)
          traits.copyable = false;
        traits.movable = traits.movable && component.movable;
        traits.copyAssignable =
            traits.copyAssignable && component.copyAssignable;
        traits.moveAssignable =
            traits.moveAssignable && component.moveAssignable;
        traits.containsBorrowedState =
            traits.containsBorrowedState || component.containsBorrowedState;
        if (component.relocation == RelocationKind::Unavailable)
          traits.relocation = RelocationKind::Unavailable;
      }
    }
    visiting.pop_back();
    // An owning sum replaces its active alternative as a whole. The checked
    // replacement sequence destroys the destination before constructing the
    // new alternative; it does not invoke assignment on an existing payload.
    // Borrow-carrying sums retain their existing dependency/assignment rules.
    if (!traits.containsBorrowedState)
      traits.moveAssignable = traits.movable;
    return traits;
  }

  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return semanticTraits(type);
  }

  SemanticTypeTraits traits;
  traits.drop = DropKind::Lexical;
  if (nonDecreasingNominalRecurrence(type, visiting)) {
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    return traits;
  }
  visiting.push_back(type);

  const ClassInfo &owner = classInfo(type.classId);
  if (owner.compilerCapability == CompilerCapabilityTypeKind::ErasedObject ||
      owner.compilerCapability == CompilerCapabilityTypeKind::FunctionPointer) {
    visiting.pop_back();
    const bool pointer =
        owner.compilerCapability == CompilerCapabilityTypeKind::FunctionPointer;
    return SemanticTypeTraits{.ownership = OwnershipKind::Value,
                              .drop = DropKind::Trivial,
                              .copyable = pointer,
                              .movable = pointer,
                              .copyAssignable = pointer,
                              .moveAssignable = pointer,
                              .containsBorrowedState = false,
                              .transferCapable = false,
                              .shareCapable = false,
                              .relocation = pointer
                                                ? RelocationKind::Bitwise
                                                : RelocationKind::Unavailable};
  }
  if (owner.compilerCapability ==
      CompilerCapabilityTypeKind::UninitializedStorage) {
    const auto payload = type.arguments.size() == 1
                             ? typeTraits(type.arguments.front(), visiting)
                             : SemanticTypeTraits{};
    // Symbolic declarations retain generated operations; concrete reanalysis
    // deletes each operation whose payload fails the structural contract.
    const bool open = type.arguments.size() == 1 &&
                      containsOpenTypeParameter(type.arguments.front());
    const bool movable =
        type.arguments.size() == 1 &&
        (open || payload.relocation == RelocationKind::Bitwise);
    const bool copyable = movable && payload.copyable;
    visiting.pop_back();
    return SemanticTypeTraits{.ownership = OwnershipKind::Value,
                              .drop = DropKind::Trivial,
                              .copyable = copyable,
                              .movable = movable,
                              .copyAssignable = copyable,
                              .moveAssignable = movable,
                              .containsBorrowedState = false,
                              .relocation = movable
                                                ? RelocationKind::Bitwise
                                                : RelocationKind::Unavailable};
  }
  if (owner.compilerCapability == CompilerCapabilityTypeKind::ThreadHandle) {
    visiting.pop_back();
    return SemanticTypeTraits{.ownership = OwnershipKind::Unique,
                              .drop = DropKind::Lexical,
                              .copyable = false,
                              .movable = true,
                              .copyAssignable = false,
                              .moveAssignable = false,
                              .containsBorrowedState = false,
                              .transferCapable = false,
                              .shareCapable = false};
  }
  if (owner.compilerCapability == CompilerCapabilityTypeKind::AtomicStorage) {
    visiting.pop_back();
    return SemanticTypeTraits{.ownership = OwnershipKind::Value,
                              .drop = DropKind::Trivial,
                              .copyable = false,
                              .movable = true,
                              .copyAssignable = false,
                              .moveAssignable = false,
                              .containsBorrowedState = false,
                              .transferCapable = true,
                              .shareCapable = true};
  }
  if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage) {
    const SemanticTypeTraits element =
        type.arguments.size() == 1
            ? typeTraits(type.arguments.front(), visiting)
            : SemanticTypeTraits{};
    visiting.pop_back();
    return SemanticTypeTraits{.ownership = OwnershipKind::Value,
                              .drop = DropKind::Lexical,
                              .copyable = false,
                              .movable = true,
                              .copyAssignable = false,
                              .moveAssignable = false,
                              .containsBorrowedState = false,
                              .transferCapable = element.transferCapable,
                              .shareCapable = element.transferCapable,
                              .relocation = element.relocation !=
                                                    RelocationKind::Unavailable
                                                ? RelocationKind::Structural
                                                : RelocationKind::Unavailable};
  }
  if (owner.compilerCapability == CompilerCapabilityTypeKind::OptionalStorage) {
    const bool relocatable =
        type.arguments.size() == 1 &&
        typeTraits(type.arguments.front(), visiting).relocation !=
            RelocationKind::Unavailable;
    visiting.pop_back();
    return SemanticTypeTraits{.ownership = OwnershipKind::Value,
                              .drop = DropKind::Lexical,
                              .copyable = false,
                              .movable = true,
                              .copyAssignable = false,
                              .moveAssignable = false,
                              .containsBorrowedState = false,
                              .relocation = relocatable
                                                ? RelocationKind::Structural
                                                : RelocationKind::Unavailable};
  }
  if (owner.compilerCapability ==
      CompilerCapabilityTypeKind::PolymorphicStorage) {
    // Every admitted payload has failure-free relocation and inert cleanup,
    // so replacing or moving the slot runs no source code. Erasure keeps the
    // interface's concurrency policy, exactly like a unique owner of it.
    const SemanticTypeTraits erased =
        type.arguments.size() == 1
            ? typeTraits(type.arguments.front(), visiting)
            : SemanticTypeTraits{.transferCapable = false,
                                 .shareCapable = false};
    visiting.pop_back();
    return SemanticTypeTraits{.ownership = OwnershipKind::Value,
                              .drop = DropKind::Lexical,
                              .copyable = false,
                              .movable = true,
                              .copyAssignable = false,
                              .moveAssignable = true,
                              .containsBorrowedState = false,
                              .transferCapable = erased.transferCapable,
                              .shareCapable = erased.shareCapable,
                              .relocation = RelocationKind::Structural};
  }
  if (owner.compilerCapability == CompilerCapabilityTypeKind::SharedOwner) {
    visiting.pop_back();
    return SemanticTypeTraits{.ownership = OwnershipKind::Shared,
                              .drop = DropKind::Lexical,
                              .copyable = false,
                              .movable = true,
                              .copyAssignable = false,
                              .moveAssignable = true,
                              .containsBorrowedState = false,
                              .relocation = RelocationKind::Structural};
  }
  if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexLockToken) {
    visiting.pop_back();
    return SemanticTypeTraits{.ownership = OwnershipKind::Unique,
                              .drop = DropKind::Lexical,
                              .copyable = false,
                              .movable = true,
                              .copyAssignable = false,
                              .moveAssignable = false,
                              .containsBorrowedState = false,
                              .transferCapable = false,
                              .shareCapable = false};
  }
  if (owner.cAbiRecord || owner.kind == ClassKind::Union) {
    traits.drop = DropKind::Trivial;
  }
  traits.relocation = RelocationKind::Bitwise;
  const GenericSubstitution substitution = classSubstitution(type);
  const auto mergeTraits = [&](const SemanticTypeTraits &component) {
    if (component.ownership == OwnershipKind::Unique ||
        (traits.ownership == OwnershipKind::Value &&
         component.ownership == OwnershipKind::Shared)) {
      traits.ownership = component.ownership;
    } else if (traits.ownership == OwnershipKind::Value &&
               component.ownership == OwnershipKind::Borrowed) {
      traits.ownership = OwnershipKind::Borrowed;
    }
    if (component.drop == DropKind::Lexical) {
      traits.drop = DropKind::Lexical;
    }
    traits.copyable = traits.copyable && component.copyable;
    traits.movable = traits.movable && component.movable;
    traits.copyAssignable = traits.copyAssignable && component.copyAssignable;
    traits.moveAssignable = traits.moveAssignable && component.moveAssignable;
    traits.containsBorrowedState =
        traits.containsBorrowedState || component.containsBorrowedState;
    if (component.relocation == RelocationKind::Unavailable)
      traits.relocation = RelocationKind::Unavailable;
    else if (component.relocation == RelocationKind::Structural &&
             traits.relocation == RelocationKind::Bitwise)
      traits.relocation = RelocationKind::Structural;
  };
  if (const ClassBaseTypeInfo *base = concreteBase(owner)) {
    mergeTraits(typeTraits(substituteType(base->type, substitution), visiting));
  }
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      continue;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      continue;
    }
    const SemanticType fieldType =
        substituteType(member->second.symbol.type, substitution);
    mergeTraits(typeTraits(fieldType, visiting));
    // Copying a reference as an expression is distinct from duplicating a
    // stored exclusive capability. Every direct reference field participates,
    // not only the first field used by the legacy primary-dependency record.
    if (fieldType.kind == SemanticType::Reference &&
        fieldType.referenceAccess == AccessMode::Mutable)
      traits.copyable = false;
  }
  if (hasSourceDestructorBody(owner)) {
    traits.copyable = false;
    traits.copyAssignable = false;
  }
  if ((hasSourceDestructorBody(owner) || concreteBase(owner) != nullptr ||
       owner.polymorphic || owner.kind == ClassKind::Union) &&
      traits.relocation == RelocationKind::Bitwise)
    traits.relocation = RelocationKind::Structural;
  if (owner.storedReference) {
    traits.ownership = traits.ownership == OwnershipKind::Value
                           ? OwnershipKind::Borrowed
                           : traits.ownership;
    // A direct read-only stored reference may be copied safely: the copy
    // retains the same semantic owner loan while its ordinary value fields
    // (for example an iterator cursor) remain independent. Mutable stored
    // references still carry an exclusive capability and remain move-only.
    traits.copyable = traits.copyable &&
                      owner.storedReference->access == AccessMode::ReadOnly;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.containsBorrowedState = true;
  }
  if (owner.copyConstructor && owner.copyConstructor->declaration != nullptr) {
    const auto &specifier = owner.copyConstructor->declaration->specifier();
    if (specifier && specifier->kind == SpecialMemberSpecifierKind::Deleted) {
      traits.copyable = false;
    }
  }
  if (owner.moveConstructor && owner.moveConstructor->declaration != nullptr) {
    const auto &specifier = owner.moveConstructor->declaration->specifier();
    if (specifier && specifier->kind == SpecialMemberSpecifierKind::Deleted) {
      traits.movable = false;
    }
  }
  if (!traits.movable || traits.containsBorrowedState || owner.abstract ||
      owner.cOpaqueHandle)
    traits.relocation = RelocationKind::Unavailable;
  visiting.pop_back();
  return traits;
}

auto SemanticVisitor::Impl::shareCapabilityDenial(
    const SemanticType &type) const -> std::optional<ShareCapabilityDenial> {
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return std::nullopt;
  }

  const ClassInfo &owner = classInfo(type.classId);
  if (owner.sharePolicy == ConcurrencyCapabilityPolicy::Denied &&
      owner.sharePolicySource) {
    return ShareCapabilityDenial{
        .source = &*owner.sharePolicySource,
        .message = "This type explicitly opts out of sharing here."};
  }
  if (owner.sharePolicy == ConcurrencyCapabilityPolicy::UnsafeConditional &&
      owner.shareCondition && owner.sharePolicySource) {
    return ShareCapabilityDenial{
        .source = &*owner.sharePolicySource,
        .message =
            "This unsafe sharing assertion requires share-capable payload '" +
            typeSpelling(substituteType(*owner.shareCondition,
                                        classSubstitution(type))) +
            "'."};
  }
  if (hasSourceDestructorBody(owner)) {
    return ShareCapabilityDenial{
        .source = &owner.destructor->declaration->tilde(),
        .message = "Declared cleanup prevents automatic sharing here."};
  }
  if (owner.storedReference && owner.storedReference->field != nullptr) {
    return ShareCapabilityDenial{
        .source = &owner.storedReference->field->name(),
        .message = "Stored borrowed state prevents sharing here."};
  }

  const GenericSubstitution substitution = classSubstitution(type);
  if (const ClassBaseTypeInfo *base = concreteBase(owner)) {
    const SemanticType baseType = substituteType(base->type, substitution);
    if (!typeTraits(baseType).shareCapable && base->syntax != nullptr) {
      return ShareCapabilityDenial{.source = &base->syntax->type.name.last(),
                                   .message =
                                       "This base type is not share-capable."};
    }
  }
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      continue;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      continue;
    }
    const SemanticType fieldType =
        substituteType(member->second.symbol.type, substitution);
    if (!typeTraits(fieldType).shareCapable) {
      return ShareCapabilityDenial{
          .source = &field.declaration->name(),
          .message = "This field has non-share-capable type '" +
                     typeSpelling(fieldType) + "'."};
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::transferCapabilityDenial(
    const SemanticType &type) const -> std::optional<TransferCapabilityDenial> {
  if (type.kind == SemanticType::Lambda) {
    const LambdaInfo *lambda = semanticModel.findLambda(type.lambdaId);
    if (lambda == nullptr) {
      return std::nullopt;
    }
    for (const LambdaCaptureInfo &capture : lambda->captures) {
      if (!capture.traits.transferCapable ||
          capture.traits.containsBorrowedState ||
          capture.type.kind == SemanticType::RawPointer) {
        return TransferCapabilityDenial{
            .source = &capture.capture,
            .message = "This capture has non-transfer-capable type '" +
                       typeSpelling(capture.type) + "'."};
      }
    }
    return std::nullopt;
  }
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return std::nullopt;
  }

  const ClassInfo &owner = classInfo(type.classId);
  if (owner.transferPolicy == ConcurrencyCapabilityPolicy::Denied &&
      owner.transferPolicySource) {
    return TransferCapabilityDenial{
        .source = &*owner.transferPolicySource,
        .message = "This type explicitly opts out of transfer here."};
  }
  if (owner.transferPolicy == ConcurrencyCapabilityPolicy::UnsafeConditional &&
      owner.transferCondition && owner.transferPolicySource) {
    return TransferCapabilityDenial{
        .source = &*owner.transferPolicySource,
        .message = "This unsafe transfer assertion requires transfer-capable "
                   "payload '" +
                   typeSpelling(substituteType(*owner.transferCondition,
                                               classSubstitution(type))) +
                   "'."};
  }
  if (hasSourceDestructorBody(owner)) {
    return TransferCapabilityDenial{
        .source = &owner.destructor->declaration->tilde(),
        .message = "Declared cleanup prevents automatic transfer here."};
  }
  if (owner.storedReference && owner.storedReference->field != nullptr) {
    return TransferCapabilityDenial{
        .source = &owner.storedReference->field->name(),
        .message = "Stored borrowed state prevents transfer here."};
  }

  const GenericSubstitution substitution = classSubstitution(type);
  if (const ClassBaseTypeInfo *base = concreteBase(owner)) {
    const SemanticType baseType = substituteType(base->type, substitution);
    if (!typeTraits(baseType).transferCapable && base->syntax != nullptr) {
      return TransferCapabilityDenial{
          .source = &base->syntax->type.name.last(),
          .message = "This base type is not transfer-capable."};
    }
  }
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      continue;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      continue;
    }
    const SemanticType fieldType =
        substituteType(member->second.symbol.type, substitution);
    if (!typeTraits(fieldType).transferCapable) {
      return TransferCapabilityDenial{
          .source = &field.declaration->name(),
          .message = "This field has non-transfer-capable type '" +
                     typeSpelling(fieldType) + "'."};
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::reportOwnershipErasureCapabilityAmplification(
    const Token &site, std::string_view ownership,
    const SemanticType &sourcePointee, const SemanticType &targetPointee,
    const SemanticType &sourceOwner, const SemanticType &targetOwner) -> bool {
  const SemanticTypeTraits source = typeTraits(sourceOwner);
  const SemanticTypeTraits target = typeTraits(targetOwner);
  const bool grantsTransfer = target.transferCapable && !source.transferCapable;
  const bool grantsShare = target.shareCapable && !source.shareCapable;
  if (!grantsTransfer && !grantsShare) {
    return false;
  }

  std::string capabilities;
  if (grantsTransfer) {
    capabilities = "transfer";
  }
  if (grantsShare) {
    if (!capabilities.empty()) {
      capabilities += " and ";
    }
    capabilities += "share";
  }
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2018", DiagnosticPhase::Semantics, site,
      "Cannot upcast " + std::string(ownership) + " ownership from '" +
          typeSpelling(sourcePointee) + "' to '" + typeSpelling(targetPointee) +
          "'; erasing the concrete type would grant " + capabilities +
          " capability that the source owner does not have.");

  const SemanticTypeTraits sourcePointeeTraits = typeTraits(sourcePointee);
  if (grantsTransfer && !sourcePointeeTraits.transferCapable) {
    if (const auto denial = transferCapabilityDenial(sourcePointee);
        denial && denial->source != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(*denial->source), denial->message});
    }
  } else if ((grantsShare || grantsTransfer) &&
             !sourcePointeeTraits.shareCapable) {
    // A shared owner requires both properties even for transfer because its
    // final destruction may occur on another thread.  Surface the pointee's
    // share denial when that is the fact suppressing both handle properties.
    if (const auto denial = shareCapabilityDenial(sourcePointee);
        denial && denial->source != nullptr) {
      diagnostic.related.push_back(
          {tokenSpan(*denial->source), denial->message});
    }
  }
  diagnostic.hints.emplace_back(
      "Keep the concrete owner type, or ensure the source object satisfies "
      "every concurrency capability exposed by the polymorphic base.");
  diagnostics.push_back(std::move(diagnostic));
  return true;
}

auto SemanticVisitor::Impl::isThreadLocalStorage(SymbolId symbol) const
    -> bool {
  const SymbolRecord *record = semanticModel.database().findSymbol(symbol);
  if (record == nullptr && instanceBaseModel != nullptr)
    record = instanceBaseModel->database().findSymbol(symbol);
  return record != nullptr && record->threadLocal;
}

auto SemanticVisitor::Impl::isPersistentStorageSymbol(SymbolId symbol) const
    -> bool {
  const SymbolRecord *record = semanticModel.database().findSymbol(symbol);
  if (record == nullptr && instanceBaseModel != nullptr)
    record = instanceBaseModel->database().findSymbol(symbol);
  return record != nullptr &&
         (record->threadLocal || record->kind == SymbolKind::GlobalVariable ||
          (record->kind == SymbolKind::Field && record->staticMember));
}

auto SemanticVisitor::Impl::isThreadLocalDataOnly(
    const VariableDecl &declaration, const SemanticType &type) const -> bool {
  if (!(isNumeric(type) || type == SemanticType::Bool ||
        type.kind == SemanticType::RawPointer ||
        type.kind == SemanticType::NativeFunction ||
        type.kind == SemanticType::StringView ||
        type.kind == SemanticType::CString) ||
      requiresActiveCleanupFor(type))
    return false;
  const Expr *initializer = declaration.initializer().get();
  while (const auto *group = exprAs<Grouping>(initializer))
    initializer = group->expression().get();
  return initializer == nullptr || exprAs<LiteralExpr>(initializer) != nullptr;
}

auto SemanticVisitor::Impl::threadLocalEnsureFailure(SymbolId symbol,
                                                     const Token &site) const
    -> DefinedFailureOperation {
  const VariableDecl *declaration = nullptr;
  const auto found = requestSession.threadLocalDeclarations.find(symbol);
  if (found != requestSession.threadLocalDeclarations.end())
    declaration = found->second;
  if (declaration == nullptr) {
    const auto *step =
        semanticModel.threadLocalInitializationPlan().findStepForSymbol(symbol);
    declaration = step == nullptr ? nullptr : step->declaration;
  }
  const auto *record = semanticModel.database().findSymbol(symbol);
  if (declaration != nullptr && record != nullptr &&
      isThreadLocalDataOnly(*declaration, record->type))
    return {};
  const SourceSpan span = tokenSpan(site);
  return {
      .localOrigins =
          {{.outcomes =
                {{DefinedFailureCode::HostedRuntimeContractFailure,
                  DefinedFailureDetail::RecursiveThreadLocalInitialization},
                 {DefinedFailureCode::HostedRuntimeContractFailure,
                  DefinedFailureDetail::
                      ThreadLocalInitializationDuringShutdown},
                 {DefinedFailureCode::HostedRuntimeContractFailure,
                  DefinedFailureDetail::ThreadLocalOutsideAttachedLifetime}},
            .sourceUnit = sourceUnitFor(site),
            .start = span.start,
            .end = span.end,
            .line = span.line}},
      .propagation = FailurePropagationKind::BodyCall};
}

auto SemanticVisitor::Impl::validateThreadLocalDeclaration(
    const VariableDecl &declaration) -> bool {
  const Token &keyword = *declaration.threadLocalKeyword();
  std::string reason;
  if (declaration.linkage() != LanguageLinkage::Gti)
    reason = "A thread-local declaration must define GTI-owned storage; native "
             "thread-local bindings are not supported.";
  else if (currentClass && functionDepth == 0 && !declaration.isStatic())
    reason = "A thread-local class field must also be declared 'static'.";
  else if (declaration.isConstexpr())
    reason = "Thread-local storage is initialized at runtime and cannot be "
             "declared 'constexpr'.";
  else if (lambdaDepth != 0)
    reason = "Thread-local declarations inside lambda bodies require a "
             "concrete per-instance storage identity.";
  else if (currentClass && !classInfo(*currentClass).genericParameters.empty())
    reason = "Thread-local declarations inside generic classes require a "
             "concrete per-instance storage identity.";
  else if (functionDepth > 0) {
    const FunctionInfo *function = currentFunctionInfo();
    if ((function != nullptr && !function->genericParameters.empty()) ||
        (currentClass && !classInfo(*currentClass).genericParameters.empty()))
      reason = "Thread-local declarations inside generic bodies require a "
               "concrete per-instance storage identity.";
  }
  if (reason.empty())
    return true;
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2124", DiagnosticPhase::Semantics, keyword, std::move(reason));
  diagnostic.related.push_back({tokenSpan(declaration.name()),
                                "The thread-local storage is declared here."});
  diagnostics.emplace_back(std::move(diagnostic));
  return false;
}

auto SemanticVisitor::Impl::recordThreadLocalDeclaration(
    const VariableDecl &declaration) -> void {
  const BindingInfo *binding = semanticModel.findBinding(declaration);
  if (binding == nullptr || binding->symbol == 0)
    return;
  BindingInfo updated = *binding;
  updated.threadLocal = true;
  // A thread-local name always denotes storage in the current thread, never
  // a program constant whose use may bypass initialization or its lifetime.
  updated.constant.reset();
  semanticModel.record(declaration, updated);
  if (const SymbolRecord *symbol =
          semanticModel.database().findSymbol(updated.symbol)) {
    auto record = *symbol;
    record.threadLocal = true;
    semanticModel.recordSymbol(std::move(record));
  }
  if (updated.type.kind == SemanticType::Reference ||
      updated.traits.containsBorrowedState) {
    if (std::none_of(diagnostics.begin(), diagnostics.end(),
                     [&](const auto &item) {
                       return item.code == "GTI-S2045" &&
                              item.primary == tokenSpan(declaration.name());
                     }))
      report(declaration.name(),
             "Thread-local storage cannot retain a checked reference or "
             "another value's borrowed state through thread shutdown.",
             "GTI-S2045");
  }
  if (functionDepth == 0)
    return;
  if (std::any_of(requestSession.threadLocalSteps.begin(),
                  requestSession.threadLocalSteps.end(), [&](const auto &step) {
                    return step.symbol == updated.symbol;
                  }))
    return;
  const FunctionInfo *function = currentFunctionInfo();
  requestSession.threadLocalSteps.push_back(
      {.sourceUnit = sourceUnitFor(declaration.name()),
       .kind = ProgramStorageKind::FunctionLocal,
       .role = isThreadLocalDataOnly(declaration, updated.type)
                   ? ProgramInitializationStepRole::DataOnly
                   : ProgramInitializationStepRole::Initializer,
       .declaration = &declaration,
       .symbol = updated.symbol,
       .requiresActiveCleanup = requiresActiveCleanupFor(updated.type),
       .threadLocal = true,
       .ownerFunction = function == nullptr ? 0 : function->id,
       .ensureFailure =
           threadLocalEnsureFailure(updated.symbol, declaration.name()),
       .declarationSpan = tokenSpan(declaration.name())});
  if (currentProgramEffectOwner.valid() &&
      !isThreadLocalDataOnly(declaration, updated.type)) {
    auto check = captureProgramStorageBorrows(tokenSpan(declaration.name()));
    const auto owner = currentProgramEffectOwner;
    const auto edges = programEffects[owner].edges.size();
    const auto unknown = programEffects[owner].unknown.size();
    addProgramEffectEdge(
        {.kind = ProgramEffectOwnerKind::Initializer, .id = updated.symbol},
        tokenSpan(declaration.name()));
    publishProgramStorageBorrowCheck(std::move(check), owner, edges, unknown);
  }
}

auto SemanticVisitor::Impl::reportConcurrentGlobalPolicyFailure(
    const VariableDecl &declaration, const SemanticType &type,
    bool globalStorage) -> bool {
  if (!globalStorage || declaration.isThreadLocal() ||
      target.executionProfile != ExecutionProfile::Concurrent ||
      type == SemanticType::Unknown) {
    return false;
  }
  if (currentClass && declaration.isStatic() &&
      !classInfo(*currentClass).genericParameters.empty()) {
    // Member registration unconditionally rejects every static member of a
    // generic class with GTI-S2039 because qualified generic member paths
    // are not represented yet. This branch suppresses only a redundant
    // profile diagnostic. Removing that restriction must also remove this
    // branch so generic static storage cannot bypass GTI-S2060.
    return false;
  }

  const std::string storageKind =
      currentClass ? "static field" : "namespace global";
  if (declaration.isMutable()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2060", DiagnosticPhase::Semantics, declaration.name(),
        "Concurrent execution profile requires " + storageKind + " '" +
            declaration.name().lexeme + "' to be immutable.");
    diagnostic.hints.emplace_back(
        "Remove 'mut' from the binding. Synchronized mutation requires an "
        "immutable value with an approved atomic or mutex contract.");
    diagnostics.emplace_back(std::move(diagnostic));
    return true;
  }

  if (typeTraits(type).shareCapable) {
    return false;
  }
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2060", DiagnosticPhase::Semantics, declaration.name(),
      "Concurrent execution profile requires " + storageKind + " '" +
          declaration.name().lexeme + "' to have a share-capable type; '" +
          typeSpelling(type) + "' is not share-capable.");
  if (const std::optional<ShareCapabilityDenial> denial =
          shareCapabilityDenial(type);
      denial && denial->source != nullptr) {
    diagnostic.related.push_back({tokenSpan(*denial->source), denial->message});
  }
  diagnostic.hints.emplace_back(
      "Use immutable share-capable state. A reviewed nominal wrapper may "
      "declare [[unsafe_share]] only after every shared access and cleanup "
      "path is proven safe.");
  diagnostics.emplace_back(std::move(diagnostic));
  return true;
}

auto SemanticVisitor::Impl::expressionInfo(SemanticType type,
                                           ValueCategory category,
                                           AccessMode access) const
    -> ExpressionInfo {
  const SemanticTypeTraits traits = typeTraits(type);
  return ExpressionInfo{.type = std::move(type),
                        .category = category,
                        .access = access,
                        .traits = traits};
}

auto SemanticVisitor::Impl::ownerDependentTextInfo(SemanticType type,
                                                   ValueCategory category,
                                                   AccessMode access) const
    -> ExpressionInfo {
  ExpressionInfo info = expressionInfo(std::move(type), category, access);
  info.traits.ownership = OwnershipKind::Borrowed;
  info.traits.copyAssignable = false;
  info.traits.moveAssignable = false;
  info.traits.containsBorrowedState = true;
  info.traits.transferCapable = false;
  info.traits.shareCapable = false;
  return info;
}

auto SemanticVisitor::Impl::textDependentValueInfo(SemanticType type,
                                                   ValueCategory category,
                                                   AccessMode access) const
    -> ExpressionInfo {
  // The value keeps its own ownership, copy and move capabilities: a copy
  // aliases the read-only text dependency and a move transfers it. Replacing
  // it would reseat the dependency, and no other thread may observe it.
  ExpressionInfo info = expressionInfo(std::move(type), category, access);
  info.traits.copyAssignable = false;
  info.traits.moveAssignable = false;
  info.traits.containsBorrowedState = true;
  info.traits.transferCapable = false;
  info.traits.shareCapable = false;
  return info;
}

auto SemanticVisitor::Impl::isTextDependentValue(
    const ExpressionInfo &info) const -> bool {
  return (info.type.kind == SemanticType::Class ||
          info.type.kind == SemanticType::Enum) &&
         info.traits.containsBorrowedState &&
         !typeTraits(info.type).containsBorrowedState;
}

auto SemanticVisitor::Impl::contextualTextValue(const Expr &expression,
                                                const SemanticType &type) const
    -> bool {
  const auto *contextual = semanticModel.findContextualConstruction(expression);
  return contextual != nullptr && contextual->dependentTextPublication &&
         contextual->constructedType == type;
}

auto SemanticVisitor::Impl::textDependentPaths(const Expr &expression) const
    -> std::vector<std::vector<SymbolId>> {
  if (const auto *group = exprAs<Grouping>(&expression))
    return group->expression() ? textDependentPaths(*group->expression())
                               : std::vector<std::vector<SymbolId>>{};
  if (const auto *call = exprAs<Call>(&expression);
      call != nullptr && call->arguments().size() == 1 &&
      resolvedIntrinsicKind(*call) == IntrinsicKind::Move)
    return textDependentPaths(*call->arguments().front());
  std::vector<std::vector<SymbolId>> paths;
  const auto mapped = [&](const std::vector<BorrowedFieldOrigin> &fields) {
    for (const auto &mapping : fields)
      paths.push_back(mapping.resultFields);
    return paths;
  };
  if (const auto *construction = semanticModel.findConstruction(expression);
      construction != nullptr && construction->dependentTextPublication)
    return mapped(construction->borrowFields);
  if (const auto *call = exprAs<Call>(&expression)) {
    if (const auto *payload = semanticModel.findPayloadConstruction(*call);
        payload != nullptr && payload->dependentTextPublication)
      return mapped(payload->borrowFields);
    if (const auto *resolved = semanticModel.findCall(*call);
        resolved != nullptr && resolved->dependentTextPublication)
      return mapped(resolved->borrowFields);
  }
  // A reference relay to a nested text class designates the paths under
  // its prefix (L-OWN-02 phase 3).
  if (const auto *call = exprAs<Call>(&expression))
    if (const auto *resolved = semanticModel.findCall(*call))
      if (const auto relay = textReferenceRelay(*call, *resolved))
        if (auto nested = nestedTextRelay(*relay))
          return std::move(*nested);
  if (const auto *unary = exprAs<Unary>(&expression))
    if (const auto relay = operatorTextReferenceRelay(*unary))
      if (auto nested = nestedTextRelay(*relay))
        return std::move(*nested);
  // A reference a call returns into a dependent receiver or argument holds
  // the origin's paths under the field it designates.
  if (const auto *call = exprAs<Call>(&expression))
    if (const auto *resolved = semanticModel.findCall(*call))
      if (const auto reference = dependentReferenceResult(*call, *resolved))
        return dependentReferencePaths(*reference);
  // A field that paths go through holds their remainder.
  if (const auto *get = exprAs<Get>(&expression)) {
    const auto *place = semanticModel.findPlace(expression);
    if (get->object() == nullptr || get->access().kind == TokenKind::ARROW ||
        place == nullptr || place->projections.empty() ||
        place->projections.back().kind != PlaceProjectionKind::Field)
      return paths;
    for (auto path : textDependentPaths(*get->object()))
      if (path.size() > 1 && path.front() == place->projections.back().field) {
        path.erase(path.begin());
        paths.push_back(std::move(path));
      }
    return paths;
  }
  const auto *variable = exprAs<Variable>(&expression);
  const Symbol *symbol =
      variable == nullptr ? nullptr : resolvedBorrowVariable(*variable);
  // A reference local bound to a dependent place holds that place's paths.
  if (symbol != nullptr && symbol->type.kind == SemanticType::Reference &&
      symbol->variableDeclaration != nullptr)
    if (const auto alias =
            dependentReferenceAliases.find(symbol->variableDeclaration);
        alias != dependentReferenceAliases.end())
      return textDependentPaths(*alias->second);
  const SemanticLoanInfo *loan =
      symbol == nullptr || symbol->retainedLoan == 0 ||
              symbol->type.kind == SemanticType::Reference
          ? nullptr
          : semanticModel.findLoan(symbol->retainedLoan);
  if (loan == nullptr || loan->kind != SemanticLoanKind::StoredValue)
    return paths;
  paths.push_back(loan->designatedFields);
  for (const auto &dependency : loan->additionalDependencies)
    paths.push_back(dependency.designatedFields);
  return paths;
}

auto SemanticVisitor::Impl::textDependentFields(const Expr &expression) const
    -> std::vector<SymbolId> {
  std::vector<SymbolId> fields;
  for (const auto &path : textDependentPaths(expression))
    if (path.size() == 1)
      fields.push_back(path.front());
  return fields;
}

auto SemanticVisitor::Impl::noteTextDependentValue(const Expr &expression)
    -> void {
  const auto [entry, inserted] = requestSession.textDependentValues.try_emplace(
      &expression, RequestSessionState::TextDependentValueUse{
                       .location = expressionToken(expression)});
  if (inserted)
    requestSession.textDependentValueOrder.push_back(&expression);
}

auto SemanticVisitor::Impl::dependentTextPayloadLoan(
    SemanticLoanId subject, const ResolvedPayloadPatternInfo &pattern,
    const PayloadBindingInfo &payload, std::span<const SymbolId> prefix) const
    -> const SemanticLoanInfo * {
  const SemanticLoanInfo *loan =
      subject == 0 ? nullptr : semanticModel.findLoan(subject);
  if (loan == nullptr || loan->kind != SemanticLoanKind::StoredValue ||
      loan->entry || !loan->conditions.empty() ||
      !loan->additionalDependencies.empty() ||
      loan->designatedFields.size() != prefix.size() + 1 ||
      !std::equal(prefix.begin(), prefix.end(), loan->designatedFields.begin()))
    return nullptr;
  const EnumTypeInfo *enumeration =
      pattern.type.kind == SemanticType::Enum
          ? semanticModel.findEnumType(pattern.type.enumId)
          : nullptr;
  if (enumeration == nullptr)
    return nullptr;
  const auto variant = std::find_if(
      enumeration->enumerators.begin(), enumeration->enumerators.end(),
      [&](const EnumeratorInfo &candidate) {
        return candidate.variantIndex == pattern.variantIndex;
      });
  if (variant == enumeration->enumerators.end() ||
      payload.payloadIndex >= variant->payloadSymbols.size() ||
      payload.payloadIndex >= variant->payloadTypes.size())
    return nullptr;
  // A generic enum's payload type is the instance's.
  const auto type = substituteType(variant->payloadTypes[payload.payloadIndex],
                                   enumSubstitution(pattern.type));
  if (type.kind != SemanticType::StringView &&
      type.kind != SemanticType::CString)
    return nullptr;
  return loan->designatedFields.back() ==
                 variant->payloadSymbols[payload.payloadIndex]
             ? loan
             : nullptr;
}

auto SemanticVisitor::Impl::consumeTextDependentValue(const ExprPtr &expression)
    -> void {
  // A child is analyzed, and so noted, before the context that consumes it.
  if (!expression)
    return;
  if (const auto found =
          requestSession.textDependentValues.find(expression.get());
      found != requestSession.textDependentValues.end())
    found->second.consumed = true;
}

auto SemanticVisitor::Impl::finalizeTextDependentValues() -> void {
  // A relay into a callee that lets none of the value's dynamic fields
  // outlive the call keeps the value within the caller's loan. Relays are
  // decided by the request's own finalization, once the field contracts they
  // read are final; an instance analysis's finalization leaves them pending.
  for (auto &relay : requestSession.textDependentRelays) {
    const auto found = requestSession.textDependentValues.find(relay.argument);
    if (instanceAnalysisActive || relay.decided ||
        found == requestSession.textDependentValues.end())
      continue;
    relay.decided = true;
    found->second.relayed = false;
    const FunctionInfo *callee = semanticModel.findFunction(relay.callee);
    if (found->second.consumed || found->second.diagnosed ||
        callee == nullptr || relay.paths.empty() ||
        !semanticModel.hasAnalyzedFunctionBody(*callee->bodyDeclaration()))
      continue;
    // A contract names a text field or a payload-enum field's text payload;
    // a path through a nested class field has none, so it may escape.
    SemanticType argumentType = semanticModel.typeOf(*relay.argument);
    if (argumentType.kind == SemanticType::Reference &&
        argumentType.arguments.size() == 1)
      argumentType = SemanticType(argumentType.arguments.front());
    const auto required = std::find_if(
        relay.paths.begin(), relay.paths.end(), [&](const auto &path) {
          const auto entry =
              contractPrefix(argumentType, path, relay.calleeFormal);
          return !entry ||
                 std::binary_search(callee->ownerlessTextFields.begin(),
                                    callee->ownerlessTextFields.end(), *entry);
        });
    if (required == relay.paths.end()) {
      found->second.consumed = true;
      continue;
    }
    found->second.diagnosed = true;
    const std::size_t before = diagnostics.size();
    report(found->second.location,
           "This value holds dynamic text from a local owner, and '" +
               callee->declaration->name().lexeme +
               "' stores or returns that text as ownerless text.",
           "GTI-S2045");
    if (diagnostics.size() > before) {
      diagnostics.back().related.push_back(
          {tokenSpan(callee->declaration->name()),
           "This function lets the field's text outlive the call."});
      diagnostics.back().hints.emplace_back(
          "Pass a value over ownerless text, or keep this use local.");
    }
  }
  for (const Expr *expression : requestSession.textDependentValueOrder) {
    auto &use = requestSession.textDependentValues.at(expression);
    if (use.consumed || use.diagnosed || use.relayed)
      continue;
    use.diagnosed = true;
    const std::size_t before = diagnostics.size();
    report(use.location,
           "This value holds dynamic text from a local owner; it can "
           "initialize a local binding, be moved or have its fields read, "
           "but it cannot yet be passed, returned, captured or stored.",
           "GTI-S2045");
    if (diagnostics.size() > before)
      diagnostics.back().hints.emplace_back(
          "Store owned text such as a std::string instead to let the value "
          "leave this function, or keep it local.");
  }
}

auto SemanticVisitor::Impl::textPublicationOwner(const SemanticType &type) const
    -> bool {
  // Copy and move constructors are defaulted or deleted (custom bodies are
  // GTI-S2020), so copying a dependent value runs no code that could read its
  // fields as ownerless. Supporting custom bodies must revisit this.
  const ClassInfo *owner = classInfo(type);
  return owner != nullptr && !typeTraits(type).containsBorrowedState &&
         storedReferencePaths(type).empty() && !owner->destructor &&
         owner->bases.empty() && !owner->polymorphic;
}

auto SemanticVisitor::Impl::holdsText(const SemanticType &type) const -> bool {
  return type.kind == SemanticType::StringView ||
         type.kind == SemanticType::CString || nestedTextClass(type);
}

auto SemanticVisitor::Impl::nestedTextClass(const SemanticType &type,
                                            std::size_t depth) const -> bool {
  const ClassInfo *owner =
      type.kind == SemanticType::Class ? classInfo(type) : nullptr;
  if (owner == nullptr || depth > 8 || !textPublicationOwner(type))
    return false;
  const auto text = [&](const SemanticType &candidate) {
    return candidate.kind == SemanticType::StringView ||
           candidate.kind == SemanticType::CString ||
           (candidate.kind == SemanticType::Class &&
            nestedTextClass(candidate, depth + 1));
  };
  for (const FieldInfo &field : owner->fields) {
    const auto member =
        field.declaration == nullptr
            ? owner->members.end()
            : owner->members.find(field.declaration->name().lexeme);
    if (member == owner->members.end() || member->second.symbol.staticMember)
      continue;
    const auto fieldType =
        substituteType(member->second.symbol.type, classSubstitution(type));
    if (text(fieldType))
      return true;
    const auto enumeration = enumTypeFor(fieldType);
    if (!enumeration || typeTraits(fieldType).containsBorrowedState)
      continue;
    const auto substitution = enumSubstitution(fieldType);
    for (const auto &variant : enumeration->enumerators)
      for (const auto &payload : variant.payloadTypes)
        if (text(substituteType(payload, substitution)))
          return true;
  }
  return false;
}

auto SemanticVisitor::Impl::nestedTextRelay(const FieldSourcedTextResult &relay)
    const -> std::optional<std::vector<std::vector<SymbolId>>> {
  const ExpressionInfo *info =
      relay.origin == nullptr || !*relay.origin
          ? nullptr
          : semanticModel.findExpression(**relay.origin);
  if (info == nullptr || !isTextDependentValue(*info))
    return std::nullopt;
  std::vector<SymbolId> prefix{relay.field};
  if (relay.payload != 0)
    prefix.push_back(relay.payload);
  std::vector<std::vector<SymbolId>> paths;
  for (auto path : textDependentPaths(**relay.origin))
    if (path.size() > prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), path.begin())) {
      path.erase(path.begin(),
                 path.begin() + static_cast<std::ptrdiff_t>(prefix.size()));
      paths.push_back(std::move(path));
    }
  return paths.empty() ? std::nullopt : std::optional{std::move(paths)};
}

auto SemanticVisitor::Impl::referenceReturnPrefixes(const FunctionInfo &callee,
                                                    BorrowOriginKind origin,
                                                    std::size_t parameter,
                                                    std::size_t depth) const
    -> std::optional<std::vector<std::vector<SymbolId>>> {
  const FunctionDecl *body = callee.bodyDeclaration();
  if (depth > 8 || callee.virtualMethod || callee.overrideMethod ||
      body == nullptr || body->body() == nullptr ||
      callee.returnType.kind != SemanticType::Reference ||
      (origin == BorrowOriginKind::Receiver &&
       (callee.ownerClass == 0 || callee.staticMember)) ||
      (origin == BorrowOriginKind::Argument &&
       (parameter >= callee.parameterTypes.size() ||
        parameter >= body->parameters().size())) ||
      (origin != BorrowOriginKind::Receiver &&
       origin != BorrowOriginKind::Argument))
    return std::nullopt;
  std::vector<const ExprPtr *> returns;
  collectBorrowReturnExpressions(body->body().get(), returns);
  if (returns.empty())
    return std::nullopt;
  // A name designates a formal or member only where nothing can shadow it.
  const auto declarationFree = [](const auto &self, const Stmt *statement) {
    if (statement == nullptr || stmtAs<ReturnStmt>(statement) != nullptr ||
        stmtAs<ExpressionStmt>(statement) != nullptr)
      return true;
    if (const auto *block = stmtAs<BlockStmt>(statement))
      return std::all_of(
          block->statements().begin(), block->statements().end(),
          [&](const StmtPtr &nested) { return self(self, nested.get()); });
    if (const auto *branch = stmtAs<IfStmt>(statement))
      return self(self, branch->thenBranch().get()) &&
             self(self, branch->elseBranch().get());
    return false;
  };
  const bool namesResolve =
      declarationFree(declarationFree, body->body().get());
  SemanticType rootType = origin == BorrowOriginKind::Receiver
                              ? openClassType(callee.ownerClass)
                              : callee.parameterTypes[parameter];
  if (rootType.kind == SemanticType::Reference &&
      rootType.arguments.size() == 1)
    rootType = SemanticType(rootType.arguments.front());
  const auto strip = [](const Expr *current) {
    while (const auto *group = exprAs<Grouping>(current))
      current = group->expression() ? group->expression().get() : nullptr;
    return current;
  };
  // Extends `prefix`, at `type`, by one member's field symbol.
  const auto field = [&](std::vector<SymbolId> &prefix, SemanticType &type,
                         const Token &name) {
    const MemberInfo *member = findMember(type, name);
    if (member == nullptr || member->symbol.staticMember ||
        member->symbol.bindingKind != SemanticBindingKind::Field)
      return false;
    prefix.push_back(symbolForDeclaration(member->symbol.declaration));
    type = substituteType(member->symbol.type, classSubstitution(type));
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      type = SemanticType(type.arguments.front());
    return true;
  };
  std::vector<std::vector<SymbolId>> prefixes;
  for (const ExprPtr *returned : returns) {
    const Expr *current =
        returned == nullptr ? nullptr : strip(returned->get());
    std::vector<const Get *> chain;
    while (const auto *get = exprAs<Get>(current)) {
      if (get->access().kind == TokenKind::ARROW || !get->object())
        return std::nullopt;
      chain.push_back(get);
      current = strip(get->object().get());
    }
    if (current == nullptr)
      return std::nullopt;
    // The roots a return may start from, each with its prefix and type.
    std::vector<std::pair<std::vector<SymbolId>, SemanticType>> roots;
    const auto *variable = exprAs<Variable>(current);
    const auto *call = exprAs<Call>(current);
    if (exprAs<This>(current) != nullptr &&
        origin == BorrowOriginKind::Receiver) {
      roots.emplace_back(std::vector<SymbolId>{}, rootType);
    } else if (variable != nullptr && namesResolve &&
               origin == BorrowOriginKind::Argument &&
               variable->name().lexeme ==
                   body->parameters()[parameter].name.lexeme) {
      roots.emplace_back(std::vector<SymbolId>{}, rootType);
    } else if (variable != nullptr && namesResolve &&
               origin == BorrowOriginKind::Receiver &&
               callee.parameterTypes.empty()) {
      // An implicit member, like `this.field`.
      std::vector<SymbolId> prefix;
      SemanticType type = rootType;
      if (!field(prefix, type, variable->name()))
        return std::nullopt;
      roots.emplace_back(std::move(prefix), std::move(type));
    } else if (call != nullptr && origin == BorrowOriginKind::Receiver &&
               call->arguments().empty() && call->typeArguments().empty()) {
      // Another accessor of the same receiver.
      const auto *member = exprAs<Get>(call->callee().get());
      const auto *implicit = exprAs<Variable>(call->callee().get());
      const bool receiverCall =
          member != nullptr && member->access().kind != TokenKind::ARROW &&
          member->object() &&
          exprAs<This>(strip(member->object().get())) != nullptr;
      if (!receiverCall && (implicit == nullptr || !namesResolve ||
                            !callee.parameterTypes.empty()))
        return std::nullopt;
      const FunctionInfo *accessor = readOnlyReceiverAccessor(
          callee.ownerClass,
          receiverCall ? member->name().lexeme : implicit->name().lexeme);
      const auto inner =
          accessor == nullptr
              ? std::nullopt
              : referenceReturnPrefixes(*accessor, BorrowOriginKind::Receiver,
                                        0, depth + 1);
      if (!inner)
        return std::nullopt;
      for (const auto &prefix : *inner) {
        SemanticType type = rootType;
        std::vector<SymbolId> walked;
        bool valid = true;
        for (const SymbolId symbol : prefix) {
          const ClassInfo *owner = classInfo(type);
          const MemberInfo *member = nullptr;
          if (owner != nullptr)
            for (const FieldInfo &candidate : owner->fields)
              if (const auto found =
                      candidate.declaration == nullptr
                          ? owner->members.end()
                          : owner->members.find(
                                candidate.declaration->name().lexeme);
                  found != owner->members.end() &&
                  symbolForDeclaration(found->second.symbol.declaration) ==
                      symbol)
                member = &found->second;
          if (member == nullptr) {
            valid = false;
            break;
          }
          walked.push_back(symbol);
          type = substituteType(member->symbol.type, classSubstitution(type));
          if (type.kind == SemanticType::Reference &&
              type.arguments.size() == 1)
            type = SemanticType(type.arguments.front());
        }
        if (!valid)
          return std::nullopt;
        roots.emplace_back(std::move(walked), std::move(type));
      }
    } else {
      return std::nullopt;
    }
    for (auto &[prefix, type] : roots) {
      for (auto get = chain.rbegin(); get != chain.rend(); ++get)
        if (!field(prefix, type, (*get)->name()))
          return std::nullopt;
      if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end())
        prefixes.push_back(std::move(prefix));
    }
  }
  return prefixes;
}

auto SemanticVisitor::Impl::dependentReferenceResult(
    const Call &call, const ResolvedCallInfo &resolved) const
    -> std::optional<DependentReferenceResult> {
  // A callee's reference relay summary already designates its result
  // (`textReferenceRelay`, L-OWN-02 phase 3).
  if (resolved.returnedTextReference ||
      resolved.returnType.kind != SemanticType::Reference ||
      resolved.returnType.arguments.size() != 1 ||
      (resolved.returnType.arguments.front().kind != SemanticType::Class &&
       resolved.returnType.arguments.front().kind != SemanticType::Enum))
    return std::nullopt;
  DependentReferenceResult result;
  if (resolved.borrowOrigin == BorrowOriginKind::Receiver) {
    const auto *member = exprAs<Get>(call.callee().get());
    if (member == nullptr || member->access().kind == TokenKind::ARROW ||
        !member->object())
      return std::nullopt;
    result.origin = &member->object();
  } else if (resolved.borrowOrigin == BorrowOriginKind::Argument) {
    result.origin =
        resolvedCallArgument(call, &resolved, resolved.borrowArgument);
  }
  const ExpressionInfo *info =
      result.origin == nullptr || !*result.origin
          ? nullptr
          : semanticModel.findExpression(**result.origin);
  if (info == nullptr || !isTextDependentValue(*info))
    return std::nullopt;
  // Finding the callee is bookkeeping, not a use of its body.
  const FunctionDemandScope pauseDemand(semanticModel, nullptr);
  const FunctionInfo *callee = semanticModel.findFunction(resolved.function);
  if (callee == nullptr && instanceBaseModel != nullptr)
    callee = instanceBaseModel->findFunction(resolved.function);
  if (callee != nullptr)
    result.prefixes = referenceReturnPrefixes(*callee, resolved.borrowOrigin,
                                              resolved.borrowArgument);
  return result;
}

auto SemanticVisitor::Impl::dependentReferencePaths(
    const DependentReferenceResult &result) const
    -> std::vector<std::vector<SymbolId>> {
  std::vector<std::vector<SymbolId>> paths;
  if (!result.prefixes || result.origin == nullptr || !*result.origin)
    return paths;
  for (const auto &path : textDependentPaths(**result.origin))
    for (const auto &prefix : *result.prefixes)
      if (path.size() > prefix.size() &&
          std::equal(prefix.begin(), prefix.end(), path.begin())) {
        std::vector<SymbolId> remainder(
            path.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
            path.end());
        if (std::find(paths.begin(), paths.end(), remainder) == paths.end())
          paths.push_back(std::move(remainder));
      }
  return paths;
}

auto SemanticVisitor::Impl::contractPrefix(const SemanticType &type,
                                           std::span<const SymbolId> path,
                                           OwnerlessTextField formal) const
    -> std::optional<OwnerlessTextField> {
  const ClassInfo *owner = classInfo(type);
  if (path.empty() || owner == nullptr ||
      typeTraits(type).containsBorrowedState)
    return std::nullopt;
  formal.field = path.front();
  formal.payload = 0;
  if (directTextField(type, path.front()))
    return path.size() == 1 ? std::optional{formal} : std::nullopt;
  if (path.size() < 2)
    return std::nullopt;
  for (const FieldInfo &field : owner->fields) {
    const auto member =
        field.declaration == nullptr
            ? owner->members.end()
            : owner->members.find(field.declaration->name().lexeme);
    if (member == owner->members.end() ||
        symbolForDeclaration(member->second.symbol.declaration) != path[0])
      continue;
    const auto fieldType =
        substituteType(member->second.symbol.type, classSubstitution(type));
    const auto enumeration = enumTypeFor(fieldType);
    if (!enumeration || typeTraits(fieldType).containsBorrowedState)
      return std::nullopt;
    for (const auto &variant : enumeration->enumerators)
      for (std::size_t index = 0; index < variant.payloadSymbols.size() &&
                                  index < variant.payloadTypes.size();
           ++index) {
        if (variant.payloadSymbols[index] != path[1])
          continue;
        const auto payload = substituteType(variant.payloadTypes[index],
                                            enumSubstitution(fieldType));
        formal.payload = path[1];
        if (payload.kind == SemanticType::StringView ||
            payload.kind == SemanticType::CString)
          return path.size() == 2 ? std::optional{formal} : std::nullopt;
        return path.size() > 2 && nestedTextClass(payload)
                   ? std::optional{formal}
                   : std::nullopt;
      }
    return std::nullopt;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::contractRelayPrefix(const Expr *object) const
    -> std::optional<OwnerlessTextField> {
  while (const auto *group = exprAs<Grouping>(object))
    object = group->expression() ? group->expression().get() : nullptr;
  const auto found = object == nullptr
                         ? requestSession.contractRelayUses.end()
                         : requestSession.contractRelayUses.find(object);
  return found == requestSession.contractRelayUses.end()
             ? std::nullopt
             : std::optional{found->second.prefix};
}

auto SemanticVisitor::Impl::noteContractRelayUse(const Expr &expression,
                                                 const SemanticType &type)
    -> void {
  const FunctionInfo *scope = textFieldContractScope();
  if (scope == nullptr || !nestedTextClass(type))
    return;
  std::optional<FieldSourcedTextResult> relay;
  if (const auto *call = exprAs<Call>(&expression)) {
    if (const auto *resolved = semanticModel.findCall(*call))
      relay = textReferenceRelay(*call, *resolved);
  } else if (const auto *unary = exprAs<Unary>(&expression)) {
    relay = operatorTextReferenceRelay(*unary);
  }
  std::optional<OwnerlessTextField> prefix;
  if (relay)
    prefix = fieldSourcedTextFormal(*relay);
  else if (const auto *get = exprAs<Get>(&expression);
           get != nullptr && get->access().kind != TokenKind::ARROW)
    prefix = contractRelayPrefix(get->object().get());
  if (!prefix)
    return;
  const auto [entry, inserted] = requestSession.contractRelayUses.try_emplace(
      &expression, RequestSessionState::ContractRelayUse{
                       .function = scope->declaration, .prefix = *prefix});
  if (inserted)
    requestSession.contractRelayOrder.push_back(&expression);
}

auto SemanticVisitor::Impl::textFieldContractFunction(
    const FunctionDecl *declaration, const FunctionDecl *body,
    bool dynamicDispatch, ClassId ownerClass) const -> bool {
  if (declaration == nullptr || body == nullptr || body->body() == nullptr ||
      dynamicDispatch || declaration->linkage() != LanguageLinkage::Gti ||
      !declaration->genericParameters().empty())
    return false;
  // A method of a generic class carries its open contract to every instance;
  // a type-parameter field's requirements bind only its text instances.
  (void)ownerClass;
  return true;
}

auto SemanticVisitor::Impl::textFieldContractScope() const
    -> const FunctionInfo * {
  // An instance analysis records no contract, and looking up its function
  // there would consult the generic declaration and demand its body.
  if (instanceAnalysisActive || lambdaDepth != 0 || constructorDepth != 0 ||
      destructorDepth != 0)
    return nullptr;
  // Finding the scope is bookkeeping, not a use of the function's body.
  const FunctionDemandScope pauseDemand(semanticModel, nullptr);
  const FunctionInfo *function = currentFunctionInfo();
  if (function == nullptr ||
      !textFieldContractFunction(
          function->declaration, function->bodyDeclaration(),
          function->virtualMethod || function->overrideMethod,
          function->ownerClass))
    return nullptr;
  return function;
}

auto SemanticVisitor::Impl::instanceFieldContractScope() const
    -> const FunctionInfo * {
  if (!instanceAnalysisActive || lambdaDepth != 0 || constructorDepth != 0 ||
      destructorDepth != 0)
    return nullptr;
  const FunctionDemandScope pauseDemand(semanticModel, nullptr);
  const FunctionInfo *function = currentFunctionInfo();
  if (function == nullptr || function->ownerClass == 0 ||
      classInfo(function->ownerClass).genericParameters.empty() ||
      !textFieldContractFunction(
          function->declaration, function->bodyDeclaration(),
          function->virtualMethod || function->overrideMethod,
          function->ownerClass))
    return nullptr;
  return function;
}

auto SemanticVisitor::Impl::readOnlyTextFieldFormal(
    const FunctionDecl &declaration, std::span<const SemanticType> types,
    std::size_t index) const -> bool {
  if (index >= declaration.parameters().size() || index >= types.size())
    return false;
  if (types[index].kind == SemanticType::Reference)
    return types[index].referenceAccess == AccessMode::ReadOnly;
  return declaration.parameters()[index].mutability != Mutability::Mutable;
}

auto SemanticVisitor::Impl::textFieldContractFormal(const Expr &object) const
    -> std::optional<OwnerlessTextField> {
  const FunctionInfo *function = textFieldContractScope();
  return function == nullptr ? std::nullopt
                             : textFieldContractFormal(object, *function);
}

auto SemanticVisitor::Impl::textFieldContractFormal(
    const Expr &object, const FunctionInfo &scope) const
    -> std::optional<OwnerlessTextField> {
  const FunctionInfo *function = &scope;
  const Expr *current = &object;
  while (const auto *group = exprAs<Grouping>(current)) {
    if (!group->expression())
      return std::nullopt;
    current = group->expression().get();
  }
  // Only a read-only formal or a read-only or consuming receiver: a mutable
  // one may be rewritten, so its reads keep the ownerless rule and it takes
  // no relay.
  if (exprAs<This>(current) != nullptr) {
    if (function->ownerClass == 0 || function->staticMember ||
        !contractReceiver(function->bodyDeclaration()->receiverMutability()) ||
        !textPublicationOwner(openClassType(function->ownerClass)))
      return std::nullopt;
    return OwnerlessTextField{.receiver = true};
  }
  const auto *variable = exprAs<Variable>(current);
  const Symbol *symbol =
      variable == nullptr ? nullptr : resolve(variable->name());
  if (symbol == nullptr || symbol->parameterDeclaration == nullptr)
    return std::nullopt;
  const auto &parameters = function->bodyDeclaration()->parameters();
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (&parameters[index] != symbol->parameterDeclaration)
      continue;
    if (parameters[index].pack || index >= function->parameterTypes.size() ||
        !readOnlyTextFieldFormal(*function->bodyDeclaration(),
                                 function->parameterTypes, index))
      return std::nullopt;
    SemanticType type = function->parameterTypes[index];
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      type = SemanticType(type.arguments.front());
    if (!textPublicationOwner(type))
      return std::nullopt;
    return OwnerlessTextField{.parameter = index};
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::ownerlessTextForwardingField(
    const ExprPtr &expression) const -> std::optional<OwnerlessTextField> {
  if (!expression)
    return std::nullopt;
  // A move of a consuming receiver's field forwards the text the field holds.
  const Expr *current = expression.get();
  while (current != nullptr) {
    if (const auto *group = exprAs<Grouping>(current))
      current = group->expression() ? group->expression().get() : nullptr;
    else if (const auto *call = exprAs<Call>(current);
             call != nullptr && call->arguments().size() == 1 && [&] {
               const auto intrinsic = intrinsicCandidate(call->callee());
               return intrinsic &&
                      intrinsic->record().intrinsic == IntrinsicKind::Move;
             }())
      current = call->arguments().front().get();
    else
      break;
  }
  if (current == nullptr)
    return std::nullopt;
  if (const auto *get = exprAs<Get>(current);
      get != nullptr && get->access().kind != TokenKind::ARROW &&
      get->object()) {
    // Text read under a contract formal's relayed nested value is the
    // formal's text under the relay's prefix.
    if (const auto prefix = contractRelayPrefix(get->object().get()))
      return prefix;
    auto formal = textFieldContractFormal(*get->object());
    const SemanticType *objectType = semanticModel.findType(*get->object());
    if (!formal || objectType == nullptr)
      return std::nullopt;
    SemanticType type = *objectType;
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      type = SemanticType(type.arguments.front());
    const MemberInfo *member = findMember(type, get->name());
    if (member == nullptr)
      return std::nullopt;
    formal->field = symbolForDeclaration(member->symbol.declaration);
    if (!directTextField(type, formal->field))
      return std::nullopt;
    return formal;
  }
  // A dereference operator's reference relay reads the same field.
  if (const auto *unary = exprAs<Unary>(current))
    if (const auto relayed = operatorTextReferenceRelay(*unary))
      return fieldSourcedTextFormal(*relayed);
  // A call that returns a formal's or the receiver's text field, applied to
  // a contract formal here, reads that formal's field. So does a reference
  // relay applied to a contract receiver.
  if (const auto *call = exprAs<Call>(current)) {
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    if (const auto relayed = resolved == nullptr
                                 ? std::nullopt
                                 : textReferenceRelay(*call, *resolved))
      return fieldSourcedTextFormal(*relayed);
    const auto result = resolved == nullptr
                            ? std::nullopt
                            : fieldSourcedTextResult(*call, *resolved);
    return result ? fieldSourcedTextFormal(*result) : std::nullopt;
  }
  const auto *variable = exprAs<Variable>(current);
  return variable == nullptr ? std::nullopt
                             : implicitReceiverTextField(*variable);
}

auto SemanticVisitor::Impl::fieldSourcedTextResult(
    const Call &call, const ResolvedCallInfo &resolved) const
    -> std::optional<FieldSourcedTextResult> {
  if ((resolved.returnType.kind != SemanticType::StringView &&
       resolved.returnType.kind != SemanticType::CString) ||
      resolved.borrowFields.size() != 1)
    return std::nullopt;
  const BorrowedFieldOrigin &mapping = resolved.borrowFields.front();
  if (mapping.sourceFields.size() != 1 || !mapping.resultFields.empty() ||
      !mapping.resultConditions.empty() || !mapping.sourceConditions.empty())
    return std::nullopt;
  FieldSourcedTextResult result{.field = mapping.sourceFields.front()};
  if (mapping.origin == BorrowOriginKind::Argument) {
    if (mapping.parameter >= resolvedCallArgumentCount(call, &resolved))
      return std::nullopt;
    result.origin = resolvedCallArgument(call, &resolved, mapping.parameter);
    return result.origin != nullptr && *result.origin ? std::optional{result}
                                                      : std::nullopt;
  }
  if (mapping.origin != BorrowOriginKind::Receiver)
    return std::nullopt;
  result.receiver = true;
  if (const auto *member = exprAs<Get>(call.callee().get())) {
    if (member->access().kind == TokenKind::ARROW || !member->object())
      return std::nullopt;
    result.origin = &member->object();
  }
  return result;
}

auto SemanticVisitor::Impl::textReferenceRelay(
    const Call &call, const ResolvedCallInfo &resolved) const
    -> std::optional<FieldSourcedTextResult> {
  if (!resolved.returnedTextReference ||
      resolved.returnType.kind != SemanticType::Reference)
    return std::nullopt;
  FieldSourcedTextResult result{.receiver = true,
                                .field = resolved.returnedTextReference->field,
                                .payload =
                                    resolved.returnedTextReference->payload};
  if (const auto *member = exprAs<Get>(call.callee().get())) {
    if (member->access().kind == TokenKind::ARROW || !member->object())
      return std::nullopt;
    result.origin = &member->object();
  }
  return result;
}

auto SemanticVisitor::Impl::fieldSourcedTextFormal(
    const FieldSourcedTextResult &result) const
    -> std::optional<OwnerlessTextField> {
  if (result.origin != nullptr) {
    auto formal = textFieldContractFormal(**result.origin);
    if (formal) {
      formal->field = result.field;
      formal->payload = result.payload;
    }
    return formal;
  }
  // An implicit receiver call relays this method's own receiver.
  const FunctionInfo *scope = textFieldContractScope();
  if (scope == nullptr || scope->ownerClass == 0 || scope->staticMember ||
      !contractReceiver(scope->bodyDeclaration()->receiverMutability()) ||
      !textPublicationOwner(openClassType(scope->ownerClass)))
    return std::nullopt;
  return OwnerlessTextField{
      .receiver = true, .field = result.field, .payload = result.payload};
}

auto SemanticVisitor::Impl::implicitReceiverTextField(
    const Variable &variable) const -> std::optional<OwnerlessTextField> {
  const FunctionInfo *function = textFieldContractScope();
  if (function == nullptr)
    return std::nullopt;
  const Symbol *symbol = resolve(variable.name());
  // Like `this.field`, only a read-only receiver's field can hold a relayed
  // owner's text.
  if (symbol == nullptr || symbol->ownerClass == 0 || symbol->staticMember ||
      symbol->parameterDeclaration != nullptr ||
      function->ownerClass != symbol->ownerClass || function->staticMember ||
      !contractReceiver(function->bodyDeclaration()->receiverMutability()))
    return std::nullopt;
  const SemanticType owner = openClassType(function->ownerClass);
  const SymbolId field = symbolForDeclaration(symbol->declaration);
  if (!textPublicationOwner(owner) || !directTextField(owner, field))
    return std::nullopt;
  return OwnerlessTextField{.receiver = true, .field = field};
}

auto SemanticVisitor::Impl::directTextFieldSymbols(const SemanticType &type,
                                                   bool conditional) const
    -> std::vector<SymbolId> {
  std::vector<SymbolId> fields;
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr)
    return fields;
  for (const FieldInfo &entry : owner->fields) {
    if (entry.declaration == nullptr)
      continue;
    const auto member = owner->members.find(entry.declaration->name().lexeme);
    if (member == owner->members.end())
      continue;
    const SymbolId field =
        symbolForDeclaration(member->second.symbol.declaration);
    if (directTextField(type, field, conditional))
      fields.push_back(field);
  }
  std::sort(fields.begin(), fields.end());
  return fields;
}

auto SemanticVisitor::Impl::noteTextFieldFormalUse(const Expr &expression,
                                                   OwnerlessTextField formal)
    -> void {
  formal.field = 0;
  const auto [entry, inserted] = requestSession.textFieldFormalUses.try_emplace(
      &expression,
      RequestSessionState::TextFieldFormalUse{
          .function = currentFunctionDeclaration, .formal = formal});
  if (inserted)
    requestSession.textFieldFormalOrder.push_back(&expression);
}

auto SemanticVisitor::Impl::conditionalTextField(const Expr &expression) const
    -> std::optional<OwnerlessTextField> {
  const FunctionInfo *scope = textFieldContractScope();
  if (scope == nullptr || scope->ownerClass == 0 ||
      classInfo(scope->ownerClass).genericParameters.empty())
    return std::nullopt;
  const bool readOnlyReceiver =
      !scope->staticMember &&
      contractReceiver(scope->bodyDeclaration()->receiverMutability());
  std::optional<OwnerlessTextField> formal;
  SemanticType owner = SemanticType::Unknown;
  const Token *name = nullptr;
  if (const auto *get = exprAs<Get>(&expression)) {
    const Expr *object = get->object() ? get->object().get() : nullptr;
    while (const auto *group = exprAs<Grouping>(object))
      object = group->expression() ? group->expression().get() : nullptr;
    if (get->access().kind == TokenKind::ARROW || object == nullptr)
      return std::nullopt;
    if (exprAs<This>(object) != nullptr) {
      if (!readOnlyReceiver)
        return std::nullopt;
      formal = OwnerlessTextField{.receiver = true};
      owner = openClassType(scope->ownerClass);
    } else if (exprAs<Variable>(object) != nullptr) {
      formal = textFieldContractFormal(*object);
      if (!formal || formal->receiver ||
          formal->parameter >= scope->parameterTypes.size())
        return std::nullopt;
      owner = scope->parameterTypes[formal->parameter];
      if (owner.kind == SemanticType::Reference && owner.arguments.size() == 1)
        owner = SemanticType(owner.arguments.front());
    } else {
      return std::nullopt;
    }
    name = &get->name();
  } else if (const auto *variable = exprAs<Variable>(&expression)) {
    const Symbol *symbol = resolve(variable->name());
    if (!readOnlyReceiver || symbol == nullptr ||
        symbol->ownerClass != scope->ownerClass || symbol->staticMember ||
        symbol->parameterDeclaration != nullptr)
      return std::nullopt;
    formal = OwnerlessTextField{.receiver = true};
    owner = openClassType(scope->ownerClass);
    name = &variable->name();
  } else {
    return std::nullopt;
  }
  const ClassInfo *info = classInfo(owner);
  if (info == nullptr)
    return std::nullopt;
  const auto member = info->members.find(name->lexeme);
  if (member == info->members.end() || member->second.symbol.staticMember ||
      member->second.symbol.bindingKind != SemanticBindingKind::Field ||
      substituteType(member->second.symbol.type, classSubstitution(owner))
              .kind != SemanticType::TypeParameter)
    return std::nullopt;
  formal->field = symbolForDeclaration(member->second.symbol.declaration);
  return formal;
}

auto SemanticVisitor::Impl::conditionalRelayReturns(
    const FunctionInfo &function) -> const std::unordered_set<const Expr *> & {
  const auto [entry, inserted] =
      requestSession.conditionalRelayReturns.try_emplace(
          function.bodyDeclaration());
  // References to unordered-map values survive recursive cache insertion;
  // retaining its iterator across conditionalTextField queries would not.
  auto &result = entry->second;
  if (!inserted || function.bodyDeclaration()->body() == nullptr)
    return result;
  std::vector<const ExprPtr *> returns;
  collectBorrowReturnExpressions(function.bodyDeclaration()->body().get(),
                                 returns);
  std::optional<SymbolId> relayed;
  std::vector<const Expr *> found;
  found.reserve(returns.size());
  for (const ExprPtr *returned : returns) {
    const Expr *current = returned != nullptr ? returned->get() : nullptr;
    // A returned move of the field relays it as a returned read does; the
    // call is not resolved yet, so the movement capability is matched as
    // the predeclared relay summary matches it.
    while (current != nullptr) {
      if (const auto *group = exprAs<Grouping>(current))
        current = group->expression() ? group->expression().get() : nullptr;
      else if (const auto *call = exprAs<Call>(current);
               call != nullptr && call->arguments().size() == 1 && [&] {
                 const auto intrinsic = intrinsicCandidate(call->callee());
                 return intrinsic &&
                        intrinsic->record().intrinsic == IntrinsicKind::Move;
               }())
        current = call->arguments().front().get();
      else
        break;
    }
    const auto field =
        current != nullptr ? conditionalTextField(*current) : std::nullopt;
    if (!field || !field->receiver || (relayed && *relayed != field->field))
      return result;
    relayed = field->field;
    found.push_back(current);
  }
  // Publish expression identities into cache-owned storage only after the
  // complete relay has been proved. No local container storage escapes.
  result.insert(found.begin(), found.end());
  return result;
}

auto SemanticVisitor::Impl::contractEnumField(
    const Expr *expression, const FunctionInfo &function) const
    -> std::optional<std::pair<OwnerlessTextField, SemanticType>> {
  auto fields = contractEnumFields(expression, function);
  return fields.size() == 1 ? std::optional{std::move(fields.front())}
                            : std::nullopt;
}

auto SemanticVisitor::Impl::contractEnumFields(
    const Expr *current, const FunctionInfo &function) const
    -> std::vector<std::pair<OwnerlessTextField, SemanticType>> {
  const FunctionInfo *scope = &function;
  while (const auto *group = exprAs<Grouping>(current))
    current = group->expression() ? group->expression().get() : nullptr;
  std::optional<OwnerlessTextField> formal;
  SemanticType owner = SemanticType::Unknown;
  std::vector<std::string> names;
  const auto formalOwner = [&](const Expr &object) {
    formal = textFieldContractFormal(object, function);
    // An instance analysis reads its receiver's fields at their types there.
    if (formal && formal->receiver)
      owner = substituteType(openClassType(scope->ownerClass),
                             instanceTypeSubstitution);
    else if (formal && formal->parameter < scope->parameterTypes.size())
      owner = scope->parameterTypes[formal->parameter];
  };
  const auto *call = exprAs<Call>(current);
  const auto *accessor =
      call == nullptr ? nullptr : exprAs<Get>(call->callee().get());
  if (const auto *get = exprAs<Get>(current);
      get != nullptr && get->access().kind != TokenKind::ARROW &&
      get->object()) {
    formalOwner(*get->object());
    names.push_back(get->name().lexeme);
  } else if (call != nullptr && call->typeArguments().empty() &&
             (accessor == nullptr
                  ? exprAs<Variable>(call->callee().get()) != nullptr
                  : accessor->access().kind != TokenKind::ARROW &&
                        accessor->object() != nullptr)) {
    // A formal's accessor that returns a reference to its fields designates
    // each field its returns name, as `formal.field` does, whatever its
    // arguments. An implicit `accessor()` is the receiver's.
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    const FunctionDemandScope pauseDemand(semanticModel, nullptr);
    const FunctionInfo *callee =
        resolved == nullptr ? nullptr
                            : semanticModel.findFunction(resolved->function);
    if (callee == nullptr && resolved != nullptr &&
        instanceBaseModel != nullptr)
      callee = instanceBaseModel->findFunction(resolved->function);
    if (accessor != nullptr) {
      formalOwner(*accessor->object());
    } else if (callee != nullptr && scope->ownerClass != 0 &&
               callee->ownerClass == scope->ownerClass &&
               !callee->staticMember && !scope->staticMember &&
               contractReceiver(
                   scope->bodyDeclaration()->receiverMutability()) &&
               textPublicationOwner(openClassType(scope->ownerClass))) {
      formal = OwnerlessTextField{.receiver = true};
      owner = substituteType(openClassType(scope->ownerClass),
                             instanceTypeSubstitution);
    }
    std::vector<const FunctionInfo *> visiting;
    if (formal && resolved != nullptr &&
        resolved->borrowOrigin == BorrowOriginKind::Receiver &&
        callee != nullptr)
      if (auto fields = accessorReceiverFields(*callee, visiting))
        names = std::move(*fields);
  } else if (const auto *variable = exprAs<Variable>(current)) {
    // An implicit receiver field, like `this.field`.
    const Symbol *symbol = resolve(variable->name());
    if (symbol != nullptr && scope->ownerClass != 0 &&
        symbol->ownerClass == scope->ownerClass && !symbol->staticMember &&
        symbol->parameterDeclaration == nullptr && !scope->staticMember &&
        contractReceiver(scope->bodyDeclaration()->receiverMutability()) &&
        textPublicationOwner(openClassType(scope->ownerClass))) {
      formal = OwnerlessTextField{.receiver = true};
      owner = substituteType(openClassType(scope->ownerClass),
                             instanceTypeSubstitution);
      names.push_back(variable->name().lexeme);
    }
  }
  if (owner.kind == SemanticType::Reference && owner.arguments.size() == 1)
    owner = SemanticType(owner.arguments.front());
  const ClassInfo *info = formal && !names.empty() ? classInfo(owner) : nullptr;
  std::vector<std::pair<OwnerlessTextField, SemanticType>> fields;
  for (const auto &name : names) {
    const auto member = info == nullptr ? decltype(info->members.end()){}
                                        : info->members.find(name);
    if (info == nullptr || member == info->members.end() ||
        member->second.symbol.staticMember ||
        member->second.symbol.bindingKind != SemanticBindingKind::Field)
      return {};
    auto field = *formal;
    field.field = symbolForDeclaration(member->second.symbol.declaration);
    fields.emplace_back(field, substituteType(member->second.symbol.type,
                                              classSubstitution(owner)));
  }
  return fields;
}

auto SemanticVisitor::Impl::contractPayloadBinding(
    const ExprPtr &subject, const ResolvedPayloadPatternInfo &pattern,
    const PayloadBindingInfo &payload) const
    -> std::optional<OwnerlessTextField> {
  const FunctionInfo *scope = textFieldContractScope();
  const auto field = scope == nullptr || payload.name == nullptr || !subject
                         ? std::nullopt
                         : contractEnumField(subject.get(), *scope);
  if (!field)
    return std::nullopt;
  auto formal = field->first;
  const auto enumeration = enumTypeFor(pattern.type);
  const auto variant = enumeration
                           ? std::find_if(enumeration->enumerators.begin(),
                                          enumeration->enumerators.end(),
                                          [&](const auto &candidate) {
                                            return candidate.variantIndex ==
                                                   pattern.variantIndex;
                                          })
                           : decltype(enumeration->enumerators.begin()){};
  if (!enumeration || variant == enumeration->enumerators.end() ||
      payload.payloadIndex >= variant->payloadSymbols.size() ||
      payload.payloadIndex >= variant->payloadTypes.size())
    return std::nullopt;
  const auto type = substituteType(variant->payloadTypes[payload.payloadIndex],
                                   enumSubstitution(pattern.type));
  const bool conditional =
      type.kind == SemanticType::TypeParameter && scope->ownerClass != 0 &&
      !classInfo(scope->ownerClass).genericParameters.empty();
  if (type.kind != SemanticType::StringView &&
      type.kind != SemanticType::CString && !conditional &&
      !nestedTextClass(type))
    return std::nullopt;
  formal.payload = variant->payloadSymbols[payload.payloadIndex];
  return formal;
}

auto SemanticVisitor::Impl::notePayloadFieldRead(const Expr &expression)
    -> void {
  // A generic class method's type-parameter field is a payload enum only in
  // some instances, so those instances' analyses record its requirements.
  const FunctionInfo *scope = textFieldContractScope();
  if (scope == nullptr)
    scope = instanceFieldContractScope();
  const auto fields =
      scope == nullptr
          ? std::vector<std::pair<OwnerlessTextField, SemanticType>>{}
          : contractEnumFields(&expression, *scope);
  // A switch observes one designated field in place. Its payload bindings
  // name one field, so a switch on an accessor designating several reads
  // them all.
  if (fields.empty() ||
      (fields.size() == 1 && &expression == observedSwitchSubject))
    return;
  // An accessor's returned fields are the accessor's own designation: a
  // caller keeps the call's result dependent on them, and a contract
  // formal's call reads them there (`contractEnumFields`).
  if (fields.front().first.receiver && scope->bodyDeclaration() != nullptr &&
      scope->bodyDeclaration()->body() != nullptr) {
    std::vector<const FunctionInfo *> visiting;
    const auto accessed = accessorReceiverFields(*scope, visiting);
    std::vector<const ExprPtr *> returns;
    if (accessed)
      collectBorrowReturnExpressions(scope->bodyDeclaration()->body().get(),
                                     returns);
    if (std::any_of(
            returns.begin(), returns.end(), [&](const ExprPtr *returned) {
              const Expr *current =
                  returned == nullptr ? nullptr : returned->get();
              while (const auto *group = exprAs<Grouping>(current))
                current =
                    group->expression() ? group->expression().get() : nullptr;
              return current == &expression;
            }))
      return;
  }
  const bool generic = scope->ownerClass != 0 &&
                       !classInfo(scope->ownerClass).genericParameters.empty();
  for (const auto &[formal, fieldType] : fields) {
    const auto enumeration = enumTypeFor(fieldType);
    if (!enumeration)
      continue;
    const auto substitution = enumSubstitution(fieldType);
    for (const auto &variant : enumeration->enumerators)
      for (std::size_t index = 0; index < variant.payloadSymbols.size() &&
                                  index < variant.payloadTypes.size();
           ++index) {
        const auto type =
            substituteType(variant.payloadTypes[index], substitution);
        if (type.kind != SemanticType::StringView &&
            type.kind != SemanticType::CString && !nestedTextClass(type) &&
            !(generic && type.kind == SemanticType::TypeParameter))
          continue;
        auto payload = formal;
        payload.payload = variant.payloadSymbols[index];
        (void)requireOwnerlessTextField(scope->declaration, payload);
      }
  }
}

auto SemanticVisitor::Impl::noteConditionalTextFieldRead(const Expr &expression)
    -> void {
  const auto field = conditionalTextField(expression);
  const FunctionInfo *scope = field ? textFieldContractScope() : nullptr;
  if (scope == nullptr || conditionalRelayReturns(*scope).contains(&expression))
    return;
  (void)requireOwnerlessTextField(scope->declaration, *field);
}

auto SemanticVisitor::Impl::callBorrowedTextFormal(
    const Call &call, const ResolvedCallInfo &resolved) const
    -> std::optional<OwnerlessTextField> {
  if (resolved.borrowOrigin == BorrowOriginKind::Receiver) {
    const auto *member = exprAs<Get>(call.callee().get());
    if (member == nullptr) {
      // An implicit receiver call borrows this method's receiver.
      const FunctionInfo *scope = textFieldContractScope();
      if (scope != nullptr && scope->ownerClass != 0 && !scope->staticMember &&
          contractReceiver(scope->bodyDeclaration()->receiverMutability()) &&
          textPublicationOwner(openClassType(scope->ownerClass)))
        return OwnerlessTextField{.receiver = true};
    } else if (member->access().kind != TokenKind::ARROW && member->object()) {
      return textFieldContractFormal(*member->object());
    }
  } else if (resolved.borrowOrigin == BorrowOriginKind::Argument &&
             resolved.borrowArgument <
                 resolvedCallArgumentCount(call, &resolved)) {
    const ExprPtr *argument =
        resolvedCallArgument(call, &resolved, resolved.borrowArgument);
    if (argument != nullptr && *argument)
      return textFieldContractFormal(**argument);
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::noteReturnedFormalReference(const Expr &returned)
    -> void {
  const Expr *current = &returned;
  while (const auto *group = exprAs<Grouping>(current))
    current = group->expression() ? group->expression().get() : nullptr;
  const auto *call = exprAs<Call>(current);
  const ResolvedCallInfo *resolved =
      call == nullptr ? nullptr : semanticModel.findCall(*call);
  if (resolved == nullptr ||
      resolved->returnType.kind != SemanticType::Reference)
    return;
  const std::optional<OwnerlessTextField> formal =
      callBorrowedTextFormal(*call, *resolved);
  // A method that returns its receiver accessor's relay composes that relay:
  // the reference designates the relayed field or payload, not the whole
  // receiver.
  const FunctionInfo *scope =
      formal && formal->receiver ? textFieldContractScope() : nullptr;
  if (scope != nullptr && scope->returnedTextReference &&
      resolved->returnedTextReference &&
      *resolved->returnedTextReference == *scope->returnedTextReference)
    return;
  if (formal)
    noteTextFieldFormalUse(*current, *formal);
}

auto SemanticVisitor::Impl::consumeTextFieldFormalUse(const ExprPtr &expression)
    -> void {
  const Expr *current = expression.get();
  while (current != nullptr) {
    if (const auto found = requestSession.textFieldFormalUses.find(current);
        found != requestSession.textFieldFormalUses.end()) {
      found->second.consumed = true;
      return;
    }
    const auto *group = exprAs<Grouping>(current);
    if (group == nullptr)
      return;
    current = group->expression().get();
  }
}

auto SemanticVisitor::Impl::requireOwnerlessTextField(
    const FunctionDecl *declaration, OwnerlessTextField field) -> bool {
  const FunctionInfo *function = declaration == nullptr
                                     ? nullptr
                                     : semanticModel.findFunction(*declaration);
  if (function == nullptr || function->declaration == nullptr)
    return false;
  const auto &fields = function->ownerlessTextFields;
  const auto at = std::lower_bound(fields.begin(), fields.end(), field);
  if (at != fields.end() && *at == field)
    return false;
  FunctionInfo updated = *function;
  updated.ownerlessTextFields.insert(
      updated.ownerlessTextFields.begin() + (at - fields.begin()), field);
  semanticModel.record(*updated.declaration, std::move(updated));
  return true;
}

auto SemanticVisitor::Impl::recordTextFieldCall(const Call &call,
                                                const CallableCandidate &target)
    -> void {
  const ExprPtr *receiver = nullptr;
  if (const auto *member = exprAs<Get>(call.callee().get());
      member != nullptr && member->access().kind != TokenKind::ARROW)
    receiver = &member->object();
  recordTextFieldUses(receiver,
                      exprAs<Variable>(call.callee().get()) != nullptr,
                      call.arguments(), target);
}

auto SemanticVisitor::Impl::recordTextFieldOperator(
    const ExprPtr &receiver, std::span<const ExprPtr> arguments,
    const CallableCandidate &target) -> void {
  recordTextFieldUses(&receiver, false, arguments, target);
}

auto SemanticVisitor::Impl::recordTextFieldUses(
    const ExprPtr *receiver, bool implicitReceiver,
    std::span<const ExprPtr> arguments, const CallableCandidate &target)
    -> void {
  // An instance analysis records no relay or forward: its formals carry no
  // contract, and a relay from a generic body is decided by the body's open
  // analysis, or not at all.
  if (instanceAnalysisActive || !target.id || !target.declaration ||
      !textFieldContractFunction(target.declaration, target.bodyDeclaration(),
                                 target.virtualMethod || target.overrideMethod,
                                 target.ownerClass))
    return;
  // A reference to text may view the receiver's or a formal's dependent
  // field. Only a receiver relay that names the field it designates
  // (returnedTextReference) gives a view the caller tracks.
  const bool textReference =
      target.returnType.kind == SemanticType::Reference &&
      target.returnType.arguments.size() == 1 &&
      (target.returnType.arguments.front().kind == SemanticType::StringView ||
       target.returnType.arguments.front().kind == SemanticType::CString);
  // An argument is classified before its formal's class is queried: most
  // calls relay and forward nothing.
  const auto record = [&](const ExprPtr &argument, OwnerlessTextField formal,
                          const SemanticType &formalType) {
    const ExpressionInfo *info =
        argument ? semanticModel.findExpression(*argument) : nullptr;
    if (info == nullptr || info->type != formalType)
      return;
    if (isTextDependentValue(*info)) {
      const auto found =
          requestSession.textDependentValues.find(argument.get());
      // A returned reference to text may view the value's designated field,
      // and a view copied from it would not keep the owner borrowed.
      if (found == requestSession.textDependentValues.end() ||
          !textPublicationOwner(formalType) ||
          (textReference && !(formal.receiver && target.returnedTextReference)))
        return;
      found->second.relayed = true;
      requestSession.textDependentRelays.push_back(
          {.argument = argument.get(),
           .callee = target.id,
           .calleeFormal = formal,
           .paths = textDependentPaths(*argument)});
      return;
    }
    // A formal passed on as the callee's formal or receiver of the same class
    // carries the callee's requirements on it back to the caller. A text
    // reference the callee returns may view any of its text fields, so the
    // use stays a whole use.
    const auto caller = textFieldContractFormal(*argument);
    if (!caller ||
        (textReference && !(formal.receiver && target.returnedTextReference)))
      return;
    requestSession.textFieldForwardUses.push_back(
        {.caller = currentFunctionDeclaration,
         .callerFormal = *caller,
         .callee = target.id,
         .calleeFormal = formal});
    consumeTextFieldFormalUse(argument);
  };
  if (target.ownerClass != 0 && !target.staticMember &&
      contractReceiver(target.declaration->receiverMutability())) {
    const SemanticType owner = openClassType(target.ownerClass);
    if (receiver != nullptr) {
      // A generic class's receiver is an instance of the open owner.
      const SemanticType *object =
          *receiver ? semanticModel.findType(**receiver) : nullptr;
      const bool instance = object != nullptr &&
                            object->kind == SemanticType::Class &&
                            object->classId == target.ownerClass;
      record(*receiver, {.receiver = true}, instance ? *object : owner);
    } else if (const FunctionInfo *scope = textFieldContractScope();
               implicitReceiver && scope != nullptr &&
               scope->ownerClass == target.ownerClass && !scope->staticMember &&
               textPublicationOwner(owner)) {
      // An implicit receiver call forwards this method's receiver.
      requestSession.textFieldForwardUses.push_back(
          {.caller = currentFunctionDeclaration,
           .callerFormal = {.receiver = true},
           .callee = target.id,
           .calleeFormal = {.receiver = true}});
    }
  }
  // A generic class method's type-parameter formal requires every text
  // field and payload of its argument in each instance: nothing relays or
  // forwards there.
  const FunctionInfo *open = nullptr;
  if (target.ownerClass != 0 &&
      !classInfo(target.ownerClass).genericParameters.empty()) {
    const FunctionDemandScope pauseDemand(semanticModel, nullptr);
    open = semanticModel.findFunction(target.id);
    if (open == nullptr && instanceBaseModel != nullptr)
      open = instanceBaseModel->findFunction(target.id);
  }
  for (std::size_t index = 0;
       index < arguments.size() && index < target.parameterTypes.size();
       ++index) {
    if (open == nullptr && target.ownerClass != 0 &&
        !classInfo(target.ownerClass).genericParameters.empty())
      continue;
    if (open != nullptr && typeParameterFormal(*open, index))
      continue;
    SemanticType formalType = target.parameterTypes[index];
    if (formalType.kind == SemanticType::Reference &&
        formalType.arguments.size() == 1)
      formalType = SemanticType(formalType.arguments.front());
    if (formalType.kind != SemanticType::Class ||
        !readOnlyTextFieldFormal(*target.declaration, target.parameterTypes,
                                 index))
      continue;
    record(arguments[index], {.parameter = index}, formalType);
  }
}

auto SemanticVisitor::Impl::operatorTextReferenceRelay(const Unary &unary) const
    -> std::optional<FieldSourcedTextResult> {
  const ResolvedOperatorInfo *resolved = semanticModel.findOperator(unary);
  if (resolved == nullptr || unary.oper().kind != TokenKind::STAR ||
      !resolved->returnedTextReference ||
      resolved->returnType.kind != SemanticType::Reference || !unary.right())
    return std::nullopt;
  return FieldSourcedTextResult{.origin = &unary.right(),
                                .receiver = true,
                                .field = resolved->returnedTextReference->field,
                                .payload =
                                    resolved->returnedTextReference->payload};
}

auto SemanticVisitor::Impl::finalizeTextFieldContracts() -> void {
  // Contracts are request facts: an instance analysis records no use, and its
  // finalization must not look up, and so demand, the functions they name.
  if (instanceAnalysisActive)
    return;
  // An unconsumed use lets the whole formal, and so each text field, outlive
  // the call.
  for (const Expr *expression : requestSession.textFieldFormalOrder) {
    const auto &use = requestSession.textFieldFormalUses.at(expression);
    const FunctionInfo *function =
        use.consumed || use.function == nullptr
            ? nullptr
            : semanticModel.findFunction(*use.function);
    if (function == nullptr)
      continue;
    SemanticType type = use.formal.receiver
                            ? openClassType(function->ownerClass)
                        : use.formal.parameter < function->parameterTypes.size()
                            ? function->parameterTypes[use.formal.parameter]
                            : SemanticType::Unknown;
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      type = SemanticType(type.arguments.front());
    for (const SymbolId field : directTextFieldSymbols(type))
      (void)requireOwnerlessTextField(use.function,
                                      {.receiver = use.formal.receiver,
                                       .parameter = use.formal.parameter,
                                       .field = field});
    for (const auto &payload : textPayloadFields(type, use.formal))
      (void)requireOwnerlessTextField(use.function, payload);
  }
  // Finite monotone closure over exact (formal, field) pairs.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &use : requestSession.textFieldForwardUses) {
      const FunctionInfo *callee = semanticModel.findFunction(use.callee);
      if (callee == nullptr)
        continue;
      const auto fields = callee->ownerlessTextFields;
      for (const auto &field : fields)
        if (field.receiver == use.calleeFormal.receiver &&
            (field.receiver || field.parameter == use.calleeFormal.parameter))
          changed = requireOwnerlessTextField(
                        use.caller, {.receiver = use.callerFormal.receiver,
                                     .parameter = use.callerFormal.parameter,
                                     .field = field.field,
                                     .payload = field.payload}) ||
                    changed;
    }
  }
}

auto SemanticVisitor::Impl::readsConstructedObject(
    const Expr &expression, const ClassInfo &owner,
    const std::vector<Parameter> *formals) const -> bool {
  const auto reads = [&](const ExprPtr &child) {
    return child && readsConstructedObject(*child, owner, formals);
  };
  switch (expression.exprKind()) {
  case ExprKind::This:
    return true;
  case ExprKind::LiteralExpr:
  case ExprKind::QualifiedName:
    return false;
  case ExprKind::Variable: {
    const auto &name = static_cast<const Variable &>(expression).name();
    if (formals != nullptr && std::any_of(formals->begin(), formals->end(),
                                          [&](const Parameter &formal) {
                                            return formal.name.lexeme ==
                                                   name.lexeme;
                                          }))
      return false;
    const auto member = owner.members.find(name.lexeme);
    return member != owner.members.end() && !member->second.symbol.staticMember;
  }
  case ExprKind::Grouping:
    return reads(static_cast<const Grouping &>(expression).expression());
  case ExprKind::Get:
    return reads(static_cast<const Get &>(expression).object());
  case ExprKind::Unary:
    return reads(static_cast<const Unary &>(expression).right());
  case ExprKind::Binary: {
    const auto &binary = static_cast<const Binary &>(expression);
    return reads(binary.left()) || reads(binary.right());
  }
  case ExprKind::Logical: {
    const auto &logical = static_cast<const Logical &>(expression);
    return reads(logical.left()) || reads(logical.right());
  }
  case ExprKind::ConditionalExpr: {
    const auto &conditional = static_cast<const ConditionalExpr &>(expression);
    return reads(conditional.condition()) ||
           reads(conditional.thenExpression()) ||
           reads(conditional.elseExpression());
  }
  case ExprKind::Index: {
    const auto &index = static_cast<const Index &>(expression);
    return reads(index.object()) || reads(index.index());
  }
  case ExprKind::Conversion:
    return reads(static_cast<const Conversion &>(expression).value());
  case ExprKind::Call: {
    const auto &call = static_cast<const Call &>(expression);
    return reads(call.callee()) ||
           std::any_of(call.arguments().begin(), call.arguments().end(), reads);
  }
  case ExprKind::DirectInitializer: {
    const auto &direct = static_cast<const DirectInitializer &>(expression);
    return std::any_of(direct.arguments().begin(), direct.arguments().end(),
                       reads);
  }
  default:
    return true;
  }
}

auto SemanticVisitor::Impl::constructionKeepsPublishedText(
    const SemanticType &type, const ConstructorDecl *constructor) const
    -> bool {
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr)
    return false;
  for (const FieldInfo &field : owner->fields)
    if (field.declaration != nullptr && field.declaration->initializer() &&
        readsConstructedObject(*field.declaration->initializer(), *owner,
                               nullptr))
      return false;
  if (constructor == nullptr)
    return true;
  if (constructor->body() && !constructor->body()->statements().empty())
    return false;
  for (const auto &initializer : constructor->initializers())
    for (const auto &argument : initializer.arguments)
      if (argument &&
          readsConstructedObject(*argument, *owner, &constructor->parameters()))
        return false;
  return true;
}

auto SemanticVisitor::Impl::directTextField(const SemanticType &type,
                                            SymbolId field,
                                            bool conditional) const -> bool {
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr || field == 0 || typeTraits(type).containsBorrowedState)
    return false;
  return std::any_of(
      owner->fields.begin(), owner->fields.end(), [&](const FieldInfo &entry) {
        const auto member =
            entry.declaration == nullptr
                ? owner->members.end()
                : owner->members.find(entry.declaration->name().lexeme);
        // The designation was recorded with the field's tooling symbol.
        if (member == owner->members.end() ||
            symbolForDeclaration(member->second.symbol.declaration) != field)
          return false;
        const auto fieldType =
            substituteType(member->second.symbol.type, classSubstitution(type));
        return fieldType.kind == SemanticType::StringView ||
               fieldType.kind == SemanticType::CString ||
               (conditional && fieldType.kind == SemanticType::TypeParameter);
      });
}

auto SemanticVisitor::Impl::textFieldInitializerDependencies(
    const SemanticType &owner, const ConstructorInitializer &initializer,
    const ExprPtr &argument, SymbolId payload)
    -> std::vector<SemanticLoanDependency> {
  const ClassInfo *info = classInfo(owner);
  if (info == nullptr || initializer.target.name.segments.size() != 1)
    return {};
  const auto member = info->members.find(initializer.target.name.last().lexeme);
  if (member == info->members.end())
    return {};
  const SymbolId field = toolingSymbolFor(member->second.symbol);
  if (payload == 0)
    return directTextField(owner, field)
               ? textFieldOwnerDependency(argument, field)
               : std::vector<SemanticLoanDependency>{};
  // The argument fills one concrete text payload of the field's enum, which
  // the dependency designates through the field.
  const auto fieldType =
      substituteType(member->second.symbol.type, classSubstitution(owner));
  if (!directTextPayload(fieldType, payload) ||
      typeTraits(owner).containsBorrowedState)
    return {};
  auto dependencies = textFieldOwnerDependency(argument, field);
  for (auto &dependency : dependencies)
    dependency.designatedFields.push_back(payload);
  return dependencies;
}

auto SemanticVisitor::Impl::directTextPayload(const SemanticType &type,
                                              SymbolId payload) const -> bool {
  const auto enumeration = enumTypeFor(type);
  if (!enumeration || !enumeration->payload || payload == 0 ||
      containsOpenTypeParameter(type) || typeTraits(type).containsBorrowedState)
    return false;
  const auto substitution = enumSubstitution(type);
  for (const auto &variant : enumeration->enumerators)
    for (std::size_t index = 0; index < variant.payloadSymbols.size() &&
                                index < variant.payloadTypes.size();
         ++index)
      if (variant.payloadSymbols[index] == payload) {
        const auto payloadType =
            substituteType(variant.payloadTypes[index], substitution);
        return payloadType.kind == SemanticType::StringView ||
               payloadType.kind == SemanticType::CString;
      }
  return false;
}

auto SemanticVisitor::Impl::typeParameterFormal(const FunctionInfo &function,
                                                std::size_t index) -> bool {
  if (index >= function.parameterTypes.size())
    return false;
  const SemanticType &type = function.parameterTypes[index];
  return type.kind == SemanticType::TypeParameter ||
         (type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
          type.arguments.front().kind == SemanticType::TypeParameter);
}

auto SemanticVisitor::Impl::contractTextPath(
    const SemanticType &type, const OwnerlessTextField &entry) const -> bool {
  if (entry.payload == 0)
    return directTextField(type, entry.field);
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr || typeTraits(type).containsBorrowedState)
    return false;
  for (const FieldInfo &field : owner->fields) {
    const auto member =
        field.declaration == nullptr
            ? owner->members.end()
            : owner->members.find(field.declaration->name().lexeme);
    if (member != owner->members.end() &&
        symbolForDeclaration(member->second.symbol.declaration) ==
            entry.field) {
      const auto fieldType =
          substituteType(member->second.symbol.type, classSubstitution(type));
      if (directTextPayload(fieldType, entry.payload))
        return true;
      // A payload of a nested text class holds text paths too.
      const auto enumeration = enumTypeFor(fieldType);
      if (!enumeration)
        return false;
      for (const auto &variant : enumeration->enumerators)
        for (std::size_t index = 0; index < variant.payloadSymbols.size() &&
                                    index < variant.payloadTypes.size();
             ++index)
          if (variant.payloadSymbols[index] == entry.payload)
            return nestedTextClass(substituteType(variant.payloadTypes[index],
                                                  enumSubstitution(fieldType)));
      return false;
    }
  }
  return false;
}

auto SemanticVisitor::Impl::textPayloadFields(const SemanticType &type,
                                              OwnerlessTextField formal) const
    -> std::vector<OwnerlessTextField> {
  std::vector<OwnerlessTextField> result;
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr || typeTraits(type).containsBorrowedState)
    return result;
  for (const FieldInfo &field : owner->fields) {
    const auto member =
        field.declaration == nullptr
            ? owner->members.end()
            : owner->members.find(field.declaration->name().lexeme);
    if (member == owner->members.end())
      continue;
    const auto fieldType =
        substituteType(member->second.symbol.type, classSubstitution(type));
    const auto enumeration = enumTypeFor(fieldType);
    if (!enumeration || !enumeration->payload)
      continue;
    for (const auto &variant : enumeration->enumerators)
      for (const SymbolId payload : variant.payloadSymbols)
        if (directTextPayload(fieldType, payload))
          result.push_back(
              {.receiver = formal.receiver,
               .parameter = formal.parameter,
               .field = symbolForDeclaration(member->second.symbol.declaration),
               .payload = payload});
  }
  return result;
}

auto SemanticVisitor::Impl::textFieldOwnerDependency(const ExprPtr &argument,
                                                     SymbolId field)
    -> std::vector<SemanticLoanDependency> {
  auto dependencies = storedBorrowDependencies(argument);
  // A dynamic view has exactly one external owner; the field designation is
  // what the stored value adds to it.
  if (dependencies.size() != 1 ||
      !dependencies.front().designatedFields.empty() ||
      !dependencies.front().conditions.empty() ||
      (dependencies.front().place.root == 0 &&
       !dependencies.front().place.receiver))
    return {};
  publishTextDependency(dependencies.front(), {field});
  return dependencies;
}

auto SemanticVisitor::Impl::publishTextDependency(
    SemanticLoanDependency &dependency, std::vector<SymbolId> path) const
    -> void {
  dependency.designatedFields = std::move(path);
  dependency.access = AccessMode::ReadOnly;
  // A local carrier's loan, a view's or a dependent value's, only carries
  // the owner dependency it copied: the value depends on that owner directly
  // and may outlive the carrier.
  if (const SemanticLoanInfo *carrier =
          semanticModel.findLoan(dependency.parent);
      carrier != nullptr && carrier->kind == SemanticLoanKind::StoredValue &&
      !carrier->entry && carrier->place.root == dependency.place.root &&
      carrier->place.receiver == dependency.place.receiver &&
      carrier->place.projections.size() <=
          dependency.place.projections.size() &&
      std::equal(carrier->place.projections.begin(),
                 carrier->place.projections.end(),
                 dependency.place.projections.begin()))
    dependency.parent = carrier->parent;
}

auto SemanticVisitor::Impl::isOwnerDependentTextExpression(
    const ExprPtr &expression) const -> bool {
  const ExpressionInfo *info =
      expression ? semanticModel.findExpression(*expression) : nullptr;
  return info != nullptr &&
         (info->type.kind == SemanticType::StringView ||
          info->type.kind == SemanticType::CString) &&
         info->traits.containsBorrowedState;
}

auto SemanticVisitor::Impl::localTextView(const Symbol &symbol) const
    -> std::optional<SymbolId> {
  if ((symbol.bindingKind != SemanticBindingKind::LocalVariable &&
       symbol.bindingKind != SemanticBindingKind::Parameter) ||
      (symbol.type.kind != SemanticType::StringView &&
       symbol.type.kind != SemanticType::CString))
    return std::nullopt;
  // Thread-local storage outlives the body that names it.
  const SymbolId identity = symbolForDeclaration(symbol.declaration);
  if (identity == 0 ||
      requestSession.threadLocalDeclarations.contains(identity))
    return std::nullopt;
  return identity;
}

auto SemanticVisitor::Impl::capturedTextSymbol(const Symbol &symbol) const
    -> bool {
  // A copy or move capture of text holds what its closure copied, whose
  // owner the lambda body cannot name. A borrowing capture is a reference,
  // whose referent rules apply instead.
  if (symbol.lambdaCapture)
    return symbol.type.kind == SemanticType::StringView ||
           symbol.type.kind == SemanticType::CString;
  const auto local = localTextView(symbol);
  return local && loanFlow.capturedTextLocals.contains(*local);
}

auto SemanticVisitor::Impl::textValueSources(const Expr *expression) const
    -> TextValueSources {
  TextValueSources sources;
  while (const auto *group = exprAs<Grouping>(expression))
    expression = group->expression().get();
  if (expression == nullptr)
    return sources;
  if (const auto *binary = exprAs<Binary>(expression);
      binary != nullptr && binary->oper().kind == TokenKind::COMMA)
    return textValueSources(binary->right().get());
  if (const auto *conditional = exprAs<ConditionalExpr>(expression)) {
    for (const ExprPtr *arm :
         {&conditional->thenExpression(), &conditional->elseExpression()}) {
      auto part = textValueSources(arm->get());
      sources.captured = sources.captured || part.captured;
      sources.locals.insert(sources.locals.end(), part.locals.begin(),
                            part.locals.end());
    }
    return sources;
  }
  if (const auto *variable = exprAs<Variable>(expression)) {
    if (const Symbol *symbol = resolve(variable->name())) {
      if (capturedTextSymbol(*symbol)) {
        sources.captured = true;
      } else if (const auto local = localTextView(*symbol)) {
        sources.locals.push_back(*local);
      }
    }
  } else if (const auto *call = exprAs<Call>(expression)) {
    // Moving a view yields that view; a call that relays one argument's or
    // its receiver's text yields that text.
    if (call->arguments().size() == 1 &&
        resolvedIntrinsicKind(*call) == IntrinsicKind::Move)
      return textValueSources(call->arguments().front().get());
    if (const auto *lambdaCall = semanticModel.findLambdaCall(*call))
      if (const LambdaInfo *lambda =
              semanticModel.findLambda(lambdaCall->lambda);
          lambda != nullptr && lambda->capturedTextResult) {
        sources.captured = true;
        return sources;
      }
    if (const ResolvedCallInfo *resolved = semanticModel.findCall(*call)) {
      if (resolved->borrowOrigin == BorrowOriginKind::Argument &&
          resolved->borrowFields.empty() &&
          resolved->borrowArgument < resolvedCallArgumentCount(*call, resolved))
        if (const ExprPtr *argument =
                resolvedCallArgument(*call, resolved, resolved->borrowArgument))
          return textValueSources(argument->get());
      if (resolved->borrowOrigin == BorrowOriginKind::Receiver &&
          resolved->borrowFields.empty())
        if (const auto *member = exprAs<Get>(call->callee().get()))
          return textValueSources(member->object().get());
    }
  }
  return sources;
}

auto SemanticVisitor::Impl::noteLocalTextEscape(const ExprPtr &value,
                                                LocalTextEscapeKind kind,
                                                std::string destination)
    -> void {
  // Owner-dependent text was reported or relayed at the site itself.
  if (!value || isOwnerDependentTextExpression(value))
    return;
  // Text a capture provided escapes here (`local` 0); a local view escapes
  // if it is ever given such text, which only the complete body decides.
  const auto sources = textValueSources(value.get());
  if (sources.captured)
    loanFlow.localTextEscapes.push_back(
        {.value = value.get(), .kind = kind, .destination = destination});
  for (const auto local : sources.locals)
    loanFlow.localTextEscapes.push_back({.local = local,
                                         .value = value.get(),
                                         .kind = kind,
                                         .destination = destination});
}

auto SemanticVisitor::Impl::noteLocalTextFlow(const ExprPtr &value,
                                              SymbolId target) -> void {
  if (!value || target == 0)
    return;
  const auto sources = textValueSources(value.get());
  if (sources.captured)
    loanFlow.capturedTextLocals.insert(target);
  for (const auto local : sources.locals)
    loanFlow.localTextFlows.emplace_back(local, target);
}

auto SemanticVisitor::Impl::finalizeCapturedText() -> bool {
  auto &state = loanFlow;
  // A local view given a capture's text anywhere holds it everywhere, and
  // so does each local view it is copied into.
  for (bool grew = !state.capturedTextLocals.empty(); grew;) {
    grew = false;
    for (const auto &[from, to] : state.localTextFlows)
      if (state.capturedTextLocals.contains(from) &&
          state.capturedTextLocals.insert(to).second)
        grew = true;
  }
  bool lambdaReturn = false;
  std::set<std::pair<const Expr *, LocalTextEscapeKind>> reported;
  for (const auto &escape : state.localTextEscapes) {
    if ((escape.local != 0 &&
         !state.capturedTextLocals.contains(escape.local)) ||
        !reported.emplace(escape.value, escape.kind).second)
      continue;
    switch (escape.kind) {
    case LocalTextEscapeKind::Storage:
      reportOwnerDependentTextStorage(*escape.value, escape.destination);
      break;
    case LocalTextEscapeKind::Return:
      reportCapturedTextReturn(*escape.value);
      break;
    case LocalTextEscapeKind::LambdaReturn:
      lambdaReturn = true;
      break;
    case LocalTextEscapeKind::Argument:
      for (auto &use : requestSession.ownerlessTextCallUses)
        if (use.argument != nullptr && use.argument->get() == escape.value) {
          use.ownerless = false;
          use.capturedText = true;
        }
      break;
    }
  }
  state.capturedTextLocals.clear();
  state.localTextFlows.clear();
  state.localTextEscapes.clear();
  return lambdaReturn;
}

auto SemanticVisitor::Impl::ownerlessTextForwardingParameter(
    const ExprPtr &expression) const -> std::optional<std::size_t> {
  const auto *function = currentFunctionInfo();
  const auto *variable = directCallableParameterVariable(expression);
  const auto *symbol = variable ? resolve(variable->name()) : nullptr;
  if (!function || !function->bodyDeclaration() || !symbol ||
      !symbol->parameterDeclaration)
    return std::nullopt;
  const auto &parameters = function->bodyDeclaration()->parameters();
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (&parameters[index] != symbol->parameterDeclaration)
      continue;
    // A mutable binding may have been replaced before the forwarding use.
    // This first contract slice proves exact immutable formal transport only.
    if (parameters[index].pack ||
        parameters[index].mutability == Mutability::Mutable ||
        index >= function->parameterTypes.size())
      return std::nullopt;
    const auto kind = function->parameterTypes[index].kind;
    if (kind != SemanticType::StringView && kind != SemanticType::CString &&
        kind != SemanticType::TypeParameter)
      return std::nullopt;
    return index;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::requireOwnerlessTextParameter(
    const ExprPtr &expression) -> bool {
  const auto index = ownerlessTextForwardingParameter(expression);
  const auto *function = currentFunctionInfo();
  // A formal's or the receiver's text field forwards a field requirement.
  if (!index && function != nullptr) {
    if (const auto field = ownerlessTextForwardingField(expression)) {
      (void)requireOwnerlessTextField(function->declaration, *field);
      return true;
    }
  }
  if (!index || !function || !function->declaration ||
      function->virtualMethod || function->overrideMethod ||
      function->linkage != LanguageLinkage::Gti)
    return false;
  FunctionInfo updated = *function;
  if (std::find(updated.ownerlessTextParameters.begin(),
                updated.ownerlessTextParameters.end(),
                *index) == updated.ownerlessTextParameters.end()) {
    updated.ownerlessTextParameters.push_back(*index);
    std::sort(updated.ownerlessTextParameters.begin(),
              updated.ownerlessTextParameters.end());
    semanticModel.record(*updated.declaration, std::move(updated));
  }
  return true;
}

auto SemanticVisitor::Impl::relayReturnedTextFormal(
    const Expr &result, const SemanticType &type,
    const ConstructorDecl *constructor, const ExprPtr &argument, SymbolId field)
    -> bool {
  // Only the function's own returned value of its return type relays a
  // formal; a construction or call anywhere else keeps the requirement.
  const Expr *returned = contextualReturnValue;
  while (const auto *group = exprAs<Grouping>(returned))
    returned = group->expression() ? group->expression().get() : nullptr;
  if (returned != &result || field == 0 || lambdaDepth != 0 ||
      constructorDepth != 0 || destructorDepth != 0)
    return false;
  auto index = ownerlessTextForwardingParameter(argument);
  const auto *function = currentFunctionInfo();
  // An immutable by-value formal of a nested text class that the
  // constructor stores whole relays its text paths (L-OWN-02 phase 3).
  if (!index && constructor != nullptr && function != nullptr &&
      function->bodyDeclaration() != nullptr)
    if (const auto *variable = directCallableParameterVariable(argument))
      if (const Symbol *symbol = resolve(variable->name());
          symbol != nullptr && symbol->parameterDeclaration != nullptr) {
        const auto &parameters = function->bodyDeclaration()->parameters();
        for (std::size_t position = 0;
             position < parameters.size() &&
             position < function->parameterTypes.size();
             ++position)
          if (&parameters[position] == symbol->parameterDeclaration &&
              !parameters[position].pack &&
              parameters[position].mutability != Mutability::Mutable &&
              nestedTextClass(function->parameterTypes[position]))
            index = position;
      }
  if (!index || !function || !function->declaration ||
      function->virtualMethod || function->overrideMethod ||
      function->linkage != LanguageLinkage::Gti ||
      function->returnType != type || !textPublicationOwner(type) ||
      (constructor != nullptr &&
       !constructionKeepsPublishedText(type, constructor)))
    return false;
  FunctionInfo updated = *function;
  const PublishedTextField entry{.parameter = *index, .field = field};
  const auto position =
      std::lower_bound(updated.publishedTextFields.begin(),
                       updated.publishedTextFields.end(), entry);
  if (position == updated.publishedTextFields.end() || *position != entry) {
    updated.publishedTextFields.insert(position, entry);
    semanticModel.record(*updated.declaration, std::move(updated));
  }
  return true;
}

auto SemanticVisitor::Impl::noteDependentTextReturn(const ExprPtr &value)
    -> void {
  if (!value || lambdaDepth != 0 || constructorDepth != 0 ||
      destructorDepth != 0)
    return;
  const SemanticType &type = currentReturnType;
  if ((type.kind != SemanticType::Class && type.kind != SemanticType::Enum) ||
      typeTraits(type).containsBorrowedState)
    return;
  const ExpressionInfo *info = semanticModel.findExpression(*value);
  if (info == nullptr ||
      !((isTextDependentValue(*info) && info->type == type) ||
        contextualTextValue(*value, type)))
    return;
  const FunctionInfo *function = [&] {
    const FunctionDemandScope pauseDemand(semanticModel, nullptr);
    return currentFunctionInfo();
  }();
  if (function == nullptr || function->declaration == nullptr ||
      function->bodyDeclaration() == nullptr || function->virtualMethod ||
      function->overrideMethod || function->linkage != LanguageLinkage::Gti)
    return;
  const auto dependencies = storedBorrowDependencies(value);
  if (dependencies.size() != 1 ||
      dependencies.front().designatedFields.empty() ||
      !dependencies.front().conditions.empty())
    return;
  const auto &dependency = dependencies.front();
  // The owner is a read-only reference formal's referent, which outlives
  // the call; a view of any local stays rejected.
  const auto &parameters = function->bodyDeclaration()->parameters();
  std::optional<std::size_t> formal;
  for (std::size_t index = 0;
       index < parameters.size() && index < function->parameterTypes.size();
       ++index) {
    const SemanticType &parameter = function->parameterTypes[index];
    const Symbol *symbol = resolve(parameters[index].name);
    if (parameter.kind == SemanticType::Reference &&
        parameter.referenceAccess == AccessMode::ReadOnly && symbol &&
        symbol->parameterDeclaration == &parameters[index] &&
        toolingSymbolFor(*symbol) == dependency.place.root)
      formal = index;
  }
  if (!formal || dependency.access != AccessMode::ReadOnly)
    return;
  const BorrowedFieldOrigin mapping{.resultFields = dependency.designatedFields,
                                    .origin = BorrowOriginKind::Argument,
                                    .parameter = *formal,
                                    .access = AccessMode::ReadOnly};
  if (function->dependentTextReturn
          ? function->returnBorrowFields !=
                std::vector<BorrowedFieldOrigin>{mapping}
          : function->returnBorrowOrigin != BorrowOriginKind::None)
    return;
  FunctionInfo updated = *function;
  updated.dependentTextReturn = true;
  updated.returnBorrowOrigin = BorrowOriginKind::Argument;
  updated.returnBorrowParameter = *formal;
  updated.returnBorrowAccess = AccessMode::ReadOnly;
  updated.returnBorrowFields = {mapping};
  semanticModel.record(*updated.declaration, std::move(updated));
  consumeTextDependentValue(value);
}

auto SemanticVisitor::Impl::publishTextReturnResult(const Call &call,
                                                    ResolvedCallInfo &resolved,
                                                    const FunctionInfo &callee)
    -> void {
  if (!callee.dependentTextReturn || callee.returnBorrowFields.size() != 1 ||
      resolved.returnType.kind == SemanticType::Reference)
    return;
  resolved.borrowOrigin = BorrowOriginKind::Argument;
  resolved.borrowArgument = callee.returnBorrowParameter;
  resolved.borrowAccess = AccessMode::ReadOnly;
  resolved.borrowArguments.clear();
  resolved.borrowFields = callee.returnBorrowFields;
  resolved.dependentTextReturn = true;
  semanticModel.record(call, resolved);
}

auto SemanticVisitor::Impl::publishTextCallResult(const Call &call,
                                                  ResolvedCallInfo &resolved,
                                                  const FunctionInfo &callee)
    -> void {
  // A callee whose body is still being analyzed may relay a formal it has
  // not recorded yet into a text field of its result, so text passed to it
  // must be ownerless here.
  const FunctionDecl *body = callee.bodyDeclaration();
  const bool provisional =
      body != nullptr && bodiesInProgress.contains(body) &&
      !directTextFieldSymbols(resolved.returnType, true).empty();
  if ((callee.publishedTextFields.empty() && !provisional) ||
      resolved.returnType.kind != SemanticType::Class ||
      !textPublicationOwner(resolved.returnType))
    return;
  if (provisional) {
    resolved.provisionalTextRelays = true;
    semanticModel.record(call, resolved);
  }
  std::vector<BorrowedFieldOrigin> published;
  const std::size_t count = resolvedCallArgumentCount(call, &resolved);
  for (std::size_t parameter = 0;
       parameter < count && parameter < resolved.parameterTypes.size();
       ++parameter) {
    const auto kind = resolved.parameterTypes[parameter].kind;
    const ExprPtr *argument = resolvedCallArgument(call, &resolved, parameter);
    // A formal of a nested text class returned whole in a field nests the
    // argument's one text path there (L-OWN-02 phase 3).
    if (kind == SemanticType::Class && argument != nullptr && !provisional &&
        nestedTextClass(resolved.parameterTypes[parameter])) {
      const ExpressionInfo *info = semanticModel.findExpression(**argument);
      const auto paths = info != nullptr && isTextDependentValue(*info)
                             ? textDependentPaths(**argument)
                             : std::vector<std::vector<SymbolId>>{};
      const ClassInfo *result = classInfo(resolved.returnType);
      if (paths.size() != 1 || result == nullptr)
        continue;
      for (const auto &entry : callee.publishedTextFields) {
        if (entry.parameter != parameter)
          continue;
        const bool whole = std::any_of(
            result->fields.begin(), result->fields.end(),
            [&](const FieldInfo &field) {
              const auto member =
                  field.declaration == nullptr
                      ? result->members.end()
                      : result->members.find(field.declaration->name().lexeme);
              return member != result->members.end() &&
                     symbolForDeclaration(member->second.symbol.declaration) ==
                         entry.field &&
                     substituteType(member->second.symbol.type,
                                    classSubstitution(resolved.returnType)) ==
                         resolved.parameterTypes[parameter];
            });
        if (!whole)
          continue;
        if (!published.empty()) {
          report(expressionToken(*argument),
                 "A value can hold dynamic text from one owner; this argument "
                 "would add a second owner dependency.",
                 "GTI-S2045");
          break;
        }
        std::vector<SymbolId> path{entry.field};
        path.insert(path.end(), paths.front().begin(), paths.front().end());
        published.push_back({.resultFields = std::move(path),
                             .origin = BorrowOriginKind::Argument,
                             .parameter = parameter,
                             .sourceFields = paths.front(),
                             .access = AccessMode::ReadOnly});
        consumeTextDependentValue(*argument);
        break;
      }
      continue;
    }
    // An open formal relays or requires here, so that callers of this generic
    // function see the composition; its text instances decide publication.
    const bool conditionalText = kind == SemanticType::TypeParameter;
    if ((kind == SemanticType::StringView || kind == SemanticType::CString) &&
        argument != nullptr &&
        (provisional || std::any_of(callee.publishedTextFields.begin(),
                                    callee.publishedTextFields.end(),
                                    [&](const PublishedTextField &entry) {
                                      return entry.parameter == parameter;
                                    })))
      noteLocalTextEscape(*argument, LocalTextEscapeKind::Storage,
                          provisional
                              ? "passed to a recursive call whose result may "
                                "hold it"
                              : "passed to a function that returns it in a "
                                "field");
    if ((kind != SemanticType::StringView && kind != SemanticType::CString &&
         !conditionalText) ||
        argument == nullptr ||
        (!conditionalText && !isOwnerDependentTextExpression(*argument)))
      continue;
    if (provisional) {
      if (!requireOwnerlessTextParameter(*argument) && !conditionalText)
        reportOwnerDependentTextStorage(
            *argument, "passed to a recursive call whose result may hold it");
      continue;
    }
    for (const auto &entry : callee.publishedTextFields) {
      if (entry.parameter != parameter ||
          !directTextField(resolved.returnType, entry.field, conditionalText))
        continue;
      if (relayReturnedTextFormal(call, resolved.returnType, nullptr, *argument,
                                  entry.field))
        continue;
      if (requireOwnerlessTextParameter(*argument) || conditionalText)
        break;
      if (textFieldOwnerDependency(*argument, entry.field).empty()) {
        reportOwnerDependentTextStorage(
            *argument, "passed to a function that returns it in a field");
        break;
      }
      if (!published.empty()) {
        // Phase 1 of L-OWN-02 publishes one owner per value.
        report(expressionToken(*argument),
               "A value can hold dynamic text from one owner; this argument "
               "would add a second owner dependency.",
               "GTI-S2045");
        break;
      }
      published.push_back({.resultFields = {entry.field},
                           .origin = BorrowOriginKind::Argument,
                           .parameter = parameter,
                           .access = AccessMode::ReadOnly});
    }
  }
  if (published.empty())
    return;
  resolved.borrowOrigin = BorrowOriginKind::Argument;
  resolved.borrowArgument = published.front().parameter;
  resolved.borrowAccess = AccessMode::ReadOnly;
  resolved.borrowFields = std::move(published);
  resolved.dependentTextPublication = true;
  semanticModel.record(call, resolved);
}

auto SemanticVisitor::Impl::recordOwnerlessTextCall(
    const Call &call, const CallableCandidate &target) -> void {
  if (!target.id || !target.declaration)
    return;
  for (std::size_t index = 0;
       index < call.arguments().size() && index < target.parameterTypes.size();
       ++index) {
    const auto &formal = target.parameterTypes[index];
    if (formal.kind != SemanticType::StringView &&
        formal.kind != SemanticType::CString &&
        formal.kind != SemanticType::TypeParameter)
      continue;
    const auto &argument = call.arguments()[index];
    const auto *info = semanticModel.findExpression(*argument);
    const auto forwarded = ownerlessTextForwardingParameter(argument);
    const auto *caller = currentFunctionInfo();
    requestSession.ownerlessTextCallUses.push_back(
        {.caller = currentFunctionDeclaration,
         .callee = target.id,
         .parameter = index,
         .type = formal,
         .forwardedParameter = forwarded,
         .callerFormal =
             forwarded && caller && *forwarded < caller->parameterTypes.size()
                 ? caller->parameterTypes[*forwarded]
                 : SemanticType::Unknown,
         .argument = &argument,
         .ownerless = info &&
                      (info->type.kind == SemanticType::StringView ||
                       info->type.kind == SemanticType::CString) &&
                      !info->traits.containsBorrowedState});
    // A local view passed here may yet be given a capture's text, which the
    // callee's ownerless requirement would then reject.
    noteLocalTextEscape(argument, LocalTextEscapeKind::Argument);
  }
}

auto SemanticVisitor::Impl::finalizeOwnerlessTextContracts() -> void {
  // A relayed nested text value used other than through a field projection
  // may let any text under the formal's prefix escape (L-OWN-02 phase 3).
  for (const Expr *expression : requestSession.contractRelayOrder) {
    auto &use = requestSession.contractRelayUses.at(expression);
    if (!use.projected && use.function != nullptr)
      (void)requireOwnerlessTextField(use.function, use.prefix);
    use.projected = true;
  }
  finalizeTextFieldContracts();
  const auto required = [&](const auto &use) {
    const auto *target = semanticModel.findFunction(use.callee);
    if (!target && instanceBaseModel)
      target = instanceBaseModel->findFunction(use.callee);
    return target &&
           std::find(target->ownerlessTextParameters.begin(),
                     target->ownerlessTextParameters.end(),
                     use.parameter) != target->ownerlessTextParameters.end();
  };
  // Finite monotone closure over exact source formal indices. There is no
  // recursive path expansion or declaration visitation order in this lattice.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &use : requestSession.ownerlessTextCallUses) {
      if (!use.caller || !use.forwardedParameter || !required(use))
        continue;
      const auto *caller = semanticModel.findFunction(*use.caller);
      if (!caller || caller->virtualMethod || caller->overrideMethod ||
          caller->linkage != LanguageLinkage::Gti ||
          std::find(caller->ownerlessTextParameters.begin(),
                    caller->ownerlessTextParameters.end(),
                    *use.forwardedParameter) !=
              caller->ownerlessTextParameters.end())
        continue;
      if (*use.forwardedParameter >= caller->parameterTypes.size() ||
          caller->parameterTypes[*use.forwardedParameter] != use.callerFormal)
        continue;
      FunctionInfo updated = *caller;
      updated.ownerlessTextParameters.push_back(*use.forwardedParameter);
      std::sort(updated.ownerlessTextParameters.begin(),
                updated.ownerlessTextParameters.end());
      semanticModel.record(*updated.declaration, std::move(updated));
      changed = true;
    }
  }
  for (auto &use : requestSession.ownerlessTextCallUses) {
    if (use.diagnosed || use.ownerless || !required(use))
      continue;
    // The requirement is conditional on the selected concrete formal being
    // text. Open bodies retain the obligation; their concrete instances are
    // reanalyzed and independently verified rather than treating an unknown
    // payload as a dynamic text view.
    if (containsOpenTypeParameter(use.type))
      continue;
    const auto *caller =
        use.caller ? semanticModel.findFunction(*use.caller) : nullptr;
    if (caller && use.forwardedParameter &&
        (*use.forwardedParameter >= caller->parameterTypes.size() ||
         caller->parameterTypes[*use.forwardedParameter] != use.callerFormal))
      continue;
    if (caller && use.forwardedParameter &&
        std::find(caller->ownerlessTextParameters.begin(),
                  caller->ownerlessTextParameters.end(),
                  *use.forwardedParameter) !=
            caller->ownerlessTextParameters.end())
      continue;
    if (use.argument) {
      const std::size_t before = diagnostics.size();
      reportOwnerDependentTextStorage(
          *use.argument, "passed to a function requiring ownerless text input",
          use.capturedText);
      // Name the formal whose body stores or forwards the view, so the
      // obligation can be traced to its source.
      const auto *callee = semanticModel.findFunction(use.callee);
      if (diagnostics.size() > before && callee && callee->declaration &&
          use.parameter < callee->declaration->parameters().size())
        diagnostics.back().related.push_back(
            {tokenSpan(callee->declaration->parameters()[use.parameter].name),
             "This parameter requires ownerless text because the function "
             "stores or forwards it."});
    }
    use.diagnosed = true;
  }
  for (auto &use : requestSession.ownerlessTextCallableUses) {
    const auto *target = semanticModel.findFunction(use.target);
    if (!target && instanceBaseModel)
      target = instanceBaseModel->findFunction(use.target);
    if (use.diagnosed || !target || target->ownerlessTextParameters.empty())
      continue;
    report(use.location,
           "This callable conversion cannot preserve its ownerless text "
           "input requirement; call the function directly.",
           "GTI-S2045");
    use.diagnosed = true;
  }
}

auto SemanticVisitor::Impl::reportOwnerDependentTextStorage(
    const ExprPtr &expression, std::string_view destination, bool capturedText)
    -> void {
  if (expression)
    reportOwnerDependentTextStorage(*expression, destination, capturedText);
}

auto SemanticVisitor::Impl::reportOwnerDependentTextStorage(
    const Expr &expression, std::string_view destination, bool capturedText)
    -> void {
  const ExpressionInfo *info = semanticModel.findExpression(expression);
  const bool cString =
      info != nullptr && info->type.kind == SemanticType::CString;
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2045", DiagnosticPhase::Semantics, expressionToken(expression),
      (cString ? "An owner-dependent c_string" : "A dynamic std::string_view") +
          std::string(" cannot be ") + std::string(destination) +
          (cString ? " in the single-owner text-borrow model."
                   : " in the single-owner view model."));
  if (capturedText || capturedTextValue(expression))
    diagnostic.hints.emplace_back(capturedTextHint(cString));
  else
    diagnostic.hints.emplace_back(
        cString ? "Initialize a new local C-string binding "
                  "in a narrow lexical scope; stored and "
                  "reassignable dependency graphs are not "
                  "supported yet."
                : "Initialize a new local view binding in "
                  "a narrow lexical scope; stored and "
                  "reassignable dependency graphs are not "
                  "supported yet.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::capturedTextValue(const Expr &expression) const
    -> bool {
  // An escape decided once its body was complete, or one checked now.
  return std::any_of(
             loanFlow.localTextEscapes.begin(), loanFlow.localTextEscapes.end(),
             [&](const LocalTextEscape &escape) {
               return escape.value == &expression &&
                      (escape.local == 0 ||
                       loanFlow.capturedTextLocals.contains(escape.local));
             }) ||
         textValueSources(&expression).captured;
}

auto SemanticVisitor::Impl::capturedTextHint(bool cString) -> std::string {
  return cString ? "A lambda's C string over its captured text has an owner "
                   "the compiler cannot name: use it in a local binding or "
                   "expression, and return or store only an ownerless C "
                   "string."
                 : "A lambda's view over its captured text has an owner the "
                   "compiler cannot name: use it in a local binding or "
                   "expression, and return or store only an ownerless view.";
}

auto SemanticVisitor::Impl::reportCapturedTextReturn(const Expr &value)
    -> void {
  const ExpressionInfo *info = semanticModel.findExpression(value);
  const bool cString =
      info != nullptr && info->type.kind == SemanticType::CString;
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2045", DiagnosticPhase::Semantics, expressionToken(value),
      std::string(cString ? "An owner-dependent c_string"
                          : "A dynamic std::string_view") +
          " cannot escape without one exact owner dependency.");
  diagnostic.hints.emplace_back(capturedTextHint(cString));
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::lifecycleBoundBorrowCarrierFor(
    const SemanticType &type) const -> bool {
  std::vector<SemanticType> visiting;
  const auto visit = [&](const auto &self,
                         const SemanticType &current) -> bool {
    if (!typeTraits(current).containsBorrowedState ||
        !requiresActiveCleanupFor(current))
      return false;
    if (current.kind == SemanticType::Enum) {
      const auto enumeration = enumTypeFor(current);
      if (!enumeration || !enumeration->payload ||
          std::find(visiting.begin(), visiting.end(), current) !=
              visiting.end())
        return true;
      visiting.push_back(current);
      bool bound = false;
      for (const auto &variant : enumeration->enumerators)
        for (const auto &field : variant.payloadTypes)
          bound =
              bound || field.kind == SemanticType::Unknown || self(self, field);
      visiting.pop_back();
      return bound;
    }
    const auto *owner = classInfo(current);
    // Unknown/recursive cleanup is conservative. Compiler-bound resources and
    // source destructors may use or release a declared dependency. Generated
    // destruction of an unrelated owned field does neither.
    if (owner == nullptr ||
        std::find(visiting.begin(), visiting.end(), current) !=
            visiting.end() ||
        owner->compilerCapability != CompilerCapabilityTypeKind::None ||
        isDefaultLibraryMutexGuardCarrier(*owner) ||
        hasSourceDestructorBody(*owner))
      return true;
    visiting.push_back(current);
    const auto substitution = classSubstitution(current);
    bool bound = false;
    for (const auto &base : owner->bases)
      bound = bound || (!base.interface &&
                        self(self, substituteType(base.type, substitution)));
    for (const auto &field : owner->fields) {
      if (field.declaration == nullptr)
        continue;
      const auto member = owner->members.find(field.declaration->name().lexeme);
      if (member == owner->members.end()) {
        bound = true;
        continue;
      }
      bound = bound || self(self, substituteType(member->second.symbol.type,
                                                 substitution));
    }
    visiting.pop_back();
    return bound;
  };
  return visit(visit, type);
}

auto SemanticVisitor::Impl::relayConstructorTextField(
    const SemanticType &constructedType, const ConstructorDecl &constructor,
    const ConstructorTextInput &input, const ExprPtr &argument,
    std::vector<BorrowedFieldOrigin> &textOrigins, bool contextual) -> void {
  const ExpressionInfo *info = semanticModel.findExpression(*argument);
  if (info == nullptr || !isTextDependentValue(*info))
    return;
  // The stored part of the formal: one text or class field of it, or the
  // whole value. The argument holds one path through it.
  const std::vector<SymbolId> stored =
      input.wholeValue ? std::vector<SymbolId>{}
                       : std::vector<SymbolId>{input.sourceField};
  const auto paths = textDependentPaths(*argument);
  if (paths.size() != 1 || paths.front().empty() ||
      paths.front().size() < stored.size() ||
      !std::equal(stored.begin(), stored.end(), paths.front().begin()))
    return;
  const std::vector<SymbolId> held = paths.front();
  // The formal must reach nothing but this initializer: the value keeps its
  // published text only when no other code reads the formal or `this`.
  const auto &parameters = constructor.parameters();
  if (input.parameter >= parameters.size() ||
      !textPublicationOwner(constructedType) ||
      !constructionKeepsPublishedText(constructedType, &constructor))
    return;
  const std::string &name = parameters[input.parameter].name.lexeme;
  std::size_t mentions = 0;
  const auto count = [&](const auto &self, const Expr *expression) -> void {
    if (expression == nullptr)
      return;
    if (const auto *variable = exprAs<Variable>(expression)) {
      mentions += variable->name().lexeme == name ? 1 : 0;
      return;
    }
    const auto each = [&](const ExprPtr &child) { self(self, child.get()); };
    if (const auto *group = exprAs<Grouping>(expression))
      each(group->expression());
    else if (const auto *get = exprAs<Get>(expression))
      each(get->object());
    else if (const auto *call = exprAs<Call>(expression)) {
      each(call->callee());
      for (const auto &child : call->arguments())
        each(child);
    } else if (const auto *unary = exprAs<Unary>(expression))
      each(unary->right());
    else if (const auto *binary = exprAs<Binary>(expression)) {
      each(binary->left());
      each(binary->right());
    } else if (exprAs<LiteralExpr>(expression) == nullptr &&
               exprAs<QualifiedName>(expression) == nullptr &&
               exprAs<This>(expression) == nullptr)
      mentions += 2;
  };
  for (const auto &initializer : constructor.initializers())
    for (const auto &child : initializer.arguments)
      count(count, child.get());
  if (mentions != 1)
    return;
  auto dependencies = storedBorrowDependencies(argument);
  const auto source = std::find_if(dependencies.begin(), dependencies.end(),
                                   [&](const auto &dependency) {
                                     return dependency.designatedFields == held;
                                   });
  const ClassInfo *owner = classInfo(constructedType);
  const auto member =
      owner == nullptr
          ? decltype(owner->members.end()){}
          : owner->members.find(input.initializer->target.name.last().lexeme);
  if (source == dependencies.end() || owner == nullptr ||
      member == owner->members.end())
    return;
  if (!textOrigins.empty()) {
    report(expressionToken(argument),
           "A value can hold dynamic text from one owner; this argument would "
           "add a second owner dependency.",
           "GTI-S2045");
    return;
  }
  std::vector<SymbolId> path{toolingSymbolFor(member->second.symbol)};
  if (input.payload != 0)
    path.push_back(input.payload);
  path.insert(path.end(), held.begin() + stored.size(), held.end());
  textOrigins.push_back({.resultFields = std::move(path),
                         .origin = BorrowOriginKind::Argument,
                         .parameter = input.parameter,
                         .sourceFields = held,
                         .access = AccessMode::ReadOnly});
  // A contextual construction is its own argument's expression: the one
  // dependent value it denotes is the converted value, which its consumer
  // keeps.
  if (!contextual)
    consumeTextDependentValue(argument);
}

auto SemanticVisitor::Impl::ownerlessTextConstructorInputs(
    const SemanticType &type, const ConstructorDecl *constructor,
    std::span<const SemanticType> parameterTypes) const
    -> std::vector<ConstructorTextInput> {
  // This is a pre-body input contract, not a retained-owner summary. Text
  // stored by value has no nested loan representation: direct parameter
  // publication therefore requires an ownerless argument at every call site.
  // Compute it from the concrete field and formal types, including instances
  // whose generic primary does not yet know that either is a text view.
  std::vector<ConstructorTextInput> result;
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr || constructor == nullptr)
    return result;
  const auto textType = [](const SemanticType &candidate) {
    return candidate.kind == SemanticType::StringView ||
           candidate.kind == SemanticType::CString ||
           candidate.kind == SemanticType::TypeParameter;
  };
  // Whether `call` invokes the movement capability, resolved in `scope`,
  // never in a caller's lexical scope and never by the public spelling.
  const auto moveCall = [&](const Call &call,
                            const std::vector<std::string> &scope) {
    const auto *name = exprAs<QualifiedName>(call.callee().get());
    if (name == nullptr || call.arguments().size() != 1)
      return false;
    const auto &path = name->name();
    const NamePath qualifier(
        std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
    const auto resolved = resolveNamespacePathGlobally(qualifier, scope);
    if (!resolved)
      return false;
    const auto found =
        activeNamespaceSymbols().find(*resolved + "::" + path.last().lexeme);
    return found != activeNamespaceSymbols().end() &&
           found->second.overloads.size() == 1 &&
           found->second.overloads.front().record().intrinsic ==
               IntrinsicKind::Move;
  };
  // `expression` without groupings and one explicit move, and whether it
  // was moved.
  const auto unmove = [&](const ExprPtr &expression,
                          const std::vector<std::string> &scope)
      -> std::pair<const ExprPtr *, bool> {
    const ExprPtr *source = &expression;
    while (const auto *group = exprAs<Grouping>(source->get()))
      source = &group->expression();
    if (const auto *call = exprAs<Call>(source->get());
        call != nullptr && moveCall(*call, scope)) {
      source = &call->arguments().front();
      while (const auto *group = exprAs<Grouping>(source->get()))
        source = &group->expression();
      return {source, true};
    }
    return {source, false};
  };
  const auto formalIndex =
      [&](const Expr *expression) -> std::optional<std::size_t> {
    const auto *variable = exprAs<Variable>(expression);
    if (variable == nullptr)
      return std::nullopt;
    const auto &parameters = constructor->parameters();
    for (std::size_t index = 0;
         index < parameters.size() && index < parameterTypes.size(); ++index)
      if (!parameters[index].pack &&
          parameters[index].name.lexeme == variable->name().lexeme)
        return index;
    return std::nullopt;
  };
  // The formal of `textType` that `expression` names, possibly grouped or
  // explicitly moved.
  const auto formalOf =
      [&](const ExprPtr &expression,
          const SemanticType &stored) -> std::optional<std::size_t> {
    const auto [source, moved] = unmove(expression, owner->namespaceScope);
    (void)moved;
    if (movedVariable(*source) == nullptr)
      return std::nullopt;
    const auto index = formalIndex(source->get());
    return index && parameterTypes[*index] == stored ? index : std::nullopt;
  };
  // The text field of a by-value class formal that `expression` reads, of
  // type `stored`: `formal.field`, or a zero-argument accessor of the formal
  // (moved for a consuming one) whose sole statement returns that field of
  // its receiver, possibly moved.
  const auto formalFieldOf = [&](const ExprPtr &expression,
                                 const SemanticType &stored)
      -> std::optional<std::pair<std::size_t, SymbolId>> {
    const auto [source, sourceMoved] =
        unmove(expression, owner->namespaceScope);
    (void)sourceMoved;
    const ExprPtr *object = nullptr;
    const Token *name = nullptr;
    const Call *accessor = nullptr;
    if (const auto *get = exprAs<Get>(source->get());
        get != nullptr && get->access().kind != TokenKind::ARROW &&
        get->object()) {
      object = &get->object();
      name = &get->name();
    } else if (const auto *call = exprAs<Call>(source->get());
               call != nullptr && call->arguments().empty() &&
               call->typeArguments().empty())
      if (const auto *get = exprAs<Get>(call->callee().get());
          get != nullptr && get->access().kind != TokenKind::ARROW &&
          get->object()) {
        object = &get->object();
        name = &get->name();
        accessor = call;
      }
    if (object == nullptr)
      return std::nullopt;
    const auto [formalExpression, moved] =
        unmove(*object, owner->namespaceScope);
    const auto index = formalIndex(formalExpression->get());
    if (!index || parameterTypes[*index].kind != SemanticType::Class)
      return std::nullopt;
    const SemanticType &formalType = parameterTypes[*index];
    const ClassInfo *formalClass = classInfo(formalType);
    if (formalClass == nullptr)
      return std::nullopt;
    const auto fieldNamed = [&](const Token &field) -> std::optional<SymbolId> {
      const auto member = formalClass->members.find(field.lexeme);
      if (member == formalClass->members.end() ||
          member->second.symbol.staticMember ||
          member->second.symbol.bindingKind != SemanticBindingKind::Field ||
          substituteType(member->second.symbol.type,
                         classSubstitution(formalType)) != stored)
        return std::nullopt;
      const SymbolId symbol =
          symbolForDeclaration(member->second.symbol.declaration);
      // A field of a publishable class type holds text paths of its own.
      return directTextField(formalType, symbol) || nestedTextClass(stored)
                 ? std::optional{symbol}
                 : std::nullopt;
    };
    if (accessor == nullptr) {
      if (const auto field = fieldNamed(*name))
        return std::pair{*index, *field};
      return std::nullopt;
    }
    const auto member = formalClass->members.find(name->lexeme);
    if (member == formalClass->members.end())
      return std::nullopt;
    const ReceiverMutability wanted =
        moved ? ReceiverMutability::Consuming : ReceiverMutability::ReadOnly;
    const CallableCandidate *selected = nullptr;
    for (const CallableCandidate &candidate : member->second.symbol.overloads) {
      if (candidate.staticMember || candidate.parameterPack ||
          !candidate.parameterTypes.empty() ||
          candidate.receiverMutability != wanted || candidate.virtualMethod ||
          candidate.overrideMethod)
        continue;
      if (selected != nullptr)
        return std::nullopt;
      selected = &candidate;
    }
    const FunctionDecl *body =
        selected == nullptr ? nullptr
        : selected->declaration != nullptr && selected->declaration->body()
            ? selected->declaration
            : nullptr;
    const auto *block =
        body == nullptr ? nullptr : stmtAs<BlockStmt>(body->body().get());
    if (block == nullptr || block->statements().size() != 1)
      return std::nullopt;
    const auto *returned =
        stmtAs<ReturnStmt>(block->statements().front().get());
    if (returned == nullptr || !returned->value())
      return std::nullopt;
    const auto [value, valueMoved] =
        unmove(returned->value(), formalClass->namespaceScope);
    (void)valueMoved;
    const auto *get = exprAs<Get>(value->get());
    if (get == nullptr || get->access().kind == TokenKind::ARROW ||
        !get->object() || exprAs<This>(get->object().get()) == nullptr)
      return std::nullopt;
    if (const auto field = fieldNamed(get->name()))
      return std::pair{*index, *field};
    return std::nullopt;
  };
  for (const auto &initializer : constructor->initializers()) {
    if (initializer.target.name.segments.size() != 1 ||
        !initializer.target.arguments.empty() ||
        initializer.arguments.size() != 1)
      continue;
    const auto member =
        owner->members.find(initializer.target.name.last().lexeme);
    if (member == owner->members.end() || member->second.symbol.staticMember ||
        member->second.symbol.bindingKind != SemanticBindingKind::Field)
      continue;
    const auto fieldType = substituteSymbol(member->second.symbol, type).type;
    if (textType(fieldType)) {
      if (const auto index = formalOf(initializer.arguments.front(), fieldType))
        result.push_back({.parameter = *index, .initializer = &initializer});
      else if (const auto relay =
                   formalFieldOf(initializer.arguments.front(), fieldType))
        result.push_back({.parameter = relay->first,
                          .initializer = &initializer,
                          .sourceField = relay->second});
      continue;
    }
    // A class formal stored whole nests every text path of its argument
    // under the field (L-OWN-02); so does a class field of a formal.
    if (nestedTextClass(fieldType)) {
      if (const auto index = formalOf(initializer.arguments.front(), fieldType))
        result.push_back({.parameter = *index,
                          .initializer = &initializer,
                          .wholeValue = true});
      else if (const auto relay =
                   formalFieldOf(initializer.arguments.front(), fieldType))
        result.push_back({.parameter = relay->first,
                          .initializer = &initializer,
                          .sourceField = relay->second});
      continue;
    }
    // A payload construction of the field's own enum stores each formal it
    // takes at that payload. The enum is resolved from the declaration's
    // namespace, as the movement capability is.
    if (fieldType.kind != SemanticType::Enum)
      continue;
    const Expr *argument = initializer.arguments.front().get();
    while (const auto *group = exprAs<Grouping>(argument))
      argument = group->expression() ? group->expression().get() : nullptr;
    const auto *call = exprAs<Call>(argument);
    const auto *callee =
        call == nullptr ? nullptr : exprAs<QualifiedName>(call->callee().get());
    const auto enumeration = enumTypeFor(fieldType);
    if (callee == nullptr || !call->typeArguments().empty() || !enumeration ||
        !enumeration->payload)
      continue;
    // The path spells the enum, with any generic arguments, then the variant.
    const auto &path = callee->name();
    if (path.segments.size() < 2)
      continue;
    const auto enumId = resolveEnumPathGlobally(
        NamePath(
            std::vector<Token>(path.segments.begin(), path.segments.end() - 1)),
        owner->namespaceScope);
    const auto variant = std::find_if(
        enumeration->enumerators.begin(), enumeration->enumerators.end(),
        [&](const auto &candidate) {
          return candidate.declaration != nullptr &&
                 candidate.declaration->name.lexeme ==
                     callee->name().last().lexeme;
        });
    if (!enumId || *enumId != fieldType.enumId ||
        variant == enumeration->enumerators.end() ||
        variant->payloadTypes.size() != call->arguments().size() ||
        variant->payloadSymbols.size() != call->arguments().size())
      continue;
    const auto substitution = enumSubstitution(fieldType);
    for (std::size_t index = 0; index < call->arguments().size(); ++index) {
      const auto payloadType =
          substituteType(variant->payloadTypes[index], substitution);
      const bool nested = nestedTextClass(payloadType);
      if (!textType(payloadType) && !nested)
        continue;
      if (const auto formal = formalOf(call->arguments()[index], payloadType))
        result.push_back({.parameter = *formal,
                          .initializer = &initializer,
                          .payload = variant->payloadSymbols[index],
                          .payloadArgument = call->arguments()[index].get(),
                          .wholeValue = nested});
      else if (const auto relay =
                   formalFieldOf(call->arguments()[index], payloadType))
        result.push_back({.parameter = relay->first,
                          .initializer = &initializer,
                          .payload = variant->payloadSymbols[index],
                          .payloadArgument = call->arguments()[index].get(),
                          .sourceField = relay->second});
    }
  }
  return result;
}

auto SemanticVisitor::Impl::bindingInfo(SemanticType type,
                                        AccessMode access) const
    -> BindingInfo {
  const SemanticTypeTraits traits = typeTraits(type);
  return BindingInfo{
      .type = std::move(type), .access = access, .traits = traits};
}

auto SemanticVisitor::Impl::isMoveOnlyOwnerType(const SemanticType &type) const
    -> bool {
  const SemanticTypeTraits traits = typeTraits(type);
  return !traits.copyable && traits.movable;
}

auto SemanticVisitor::Impl::collectStoredReferencePaths(
    const SemanticType &type, StoredReferenceQuery query) const
    -> std::optional<std::vector<StoredReferencePath>> {
  // Existing generic declaration validation needs symbolic paths such as
  // a field of type T&. That query never certifies completeness. The separate
  // complete-inventory entry point always rejects unresolved type structure.
  const bool requireComplete = query == StoredReferenceQuery::CompleteInventory;
  std::vector<SemanticType> visiting;
  const auto visit = [&](const auto &self, const SemanticType &current)
      -> std::optional<std::vector<StoredReferencePath>> {
    if (requireComplete && (current.kind == SemanticType::Unknown ||
                            containsOpenTypeParameter(current)))
      return std::nullopt;
    if (!current.hasLambdaShape() && current.kind != SemanticType::Class &&
        current.kind != SemanticType::Enum &&
        !typeTraits(current).containsBorrowedState)
      return std::vector<StoredReferencePath>{};
    if (current.hasLambdaShape()) {
      const auto *lambda = semanticModel.findLambda(current.lambdaId);
      const auto types = current.lambdaCaptureTypes();
      if (lambda == nullptr || lambda->declaration == nullptr ||
          lambda->captures.size() != types.size() ||
          lambda->declaration->captures().size() != types.size() ||
          std::find(visiting.begin(), visiting.end(), current) !=
              visiting.end())
        return std::nullopt;
      visiting.push_back(current);
      std::vector<StoredReferencePath> paths;
      bool invalid = false;
      for (std::size_t index = 0; index < types.size(); ++index) {
        if (requireComplete && (types[index].kind == SemanticType::Unknown ||
                                containsOpenTypeParameter(types[index]) ||
                                (types[index].kind == SemanticType::Reference &&
                                 types[index].arguments.size() != 1))) {
          invalid = true;
          continue;
        }
        std::vector<StoredReferencePath> children;
        if (types[index].kind == SemanticType::Reference)
          children.push_back({.reference = types[index]});
        else if (typeTraits(types[index]).containsBorrowedState) {
          auto inventory = self(self, types[index]);
          invalid = invalid || !inventory;
          if (inventory)
            children = std::move(*inventory);
        }
        for (auto &child : children) {
          child.fields.insert(child.fields.begin(),
                              &lambda->declaration->captures()[index]);
          for (auto &condition : child.conditions)
            condition.subobject.insert(condition.subobject.begin(),
                                       &lambda->declaration->captures()[index]);
          paths.push_back(std::move(child));
        }
      }
      visiting.pop_back();
      return invalid ? std::nullopt
                     : std::optional<std::vector<StoredReferencePath>>{
                           std::move(paths)};
    }
    const ClassInfo *owner = classInfo(current);
    if (current.kind == SemanticType::Enum) {
      const auto enumeration = enumTypeFor(current);
      if (!enumeration || std::find(visiting.begin(), visiting.end(),
                                    current) != visiting.end())
        return std::nullopt;
      if (!enumeration->payload)
        return std::vector<StoredReferencePath>{};
      visiting.push_back(current);
      std::vector<StoredReferencePath> paths;
      bool invalid = false;
      for (std::size_t alternative = 0;
           alternative < enumeration->enumerators.size(); ++alternative) {
        const auto &variant = enumeration->enumerators[alternative];
        if (variant.declaration == nullptr ||
            variant.payloadTypes.size() !=
                variant.declaration->payload.size()) {
          invalid = true;
          continue;
        }
        for (std::size_t index = 0; index < variant.payloadTypes.size();
             ++index) {
          const auto &fieldType = variant.payloadTypes[index];
          if (requireComplete && (fieldType.kind == SemanticType::Unknown ||
                                  containsOpenTypeParameter(fieldType) ||
                                  (fieldType.kind == SemanticType::Reference &&
                                   fieldType.arguments.size() != 1))) {
            invalid = true;
            continue;
          }
          std::vector<StoredReferencePath> children;
          if (fieldType.kind == SemanticType::Reference)
            children.push_back({.reference = fieldType});
          else if (typeTraits(fieldType).containsBorrowedState) {
            auto inventory = self(self, fieldType);
            invalid = invalid || !inventory;
            if (inventory)
              children = std::move(*inventory);
          }
          for (auto &child : children) {
            child.fields.insert(child.fields.begin(),
                                &variant.declaration->payload[index]);
            for (auto &condition : child.conditions)
              condition.subobject.insert(condition.subobject.begin(),
                                         &variant.declaration->payload[index]);
            child.conditions.insert(
                child.conditions.begin(),
                {.enumeration = current, .variant = alternative});
            paths.push_back(std::move(child));
          }
        }
      }
      visiting.pop_back();
      return invalid ? std::nullopt
                     : std::optional<std::vector<StoredReferencePath>>{
                           std::move(paths)};
    }
    // A validated native layout is a complete, passive field inventory. Raw
    // pointers and native callbacks do not carry checked GTI loans. Traverse
    // those fields normally; neither an unvalidated record nor an ordinary
    // union establishes the same completeness guarantee.
    const bool validatedNativeRecord =
        owner != nullptr && owner->cAbiRecord && owner->cAbiLayout.has_value();
    if (owner == nullptr ||
        (owner->kind == ClassKind::Union && !validatedNativeRecord) ||
        (owner->cAbiRecord && !validatedNativeRecord) || owner->cOpaqueHandle ||
        // A synchronization guard carries capability-produced exclusive
        // access in addition to its declared shared resource reference.
        // Nested field maps currently preserve the latter, not the former.
        // Keep this existing boundary rather than silently downgrade a lock
        // obligation when general cleanup-bearing carriers are composed.
        (!visiting.empty() && isDefaultLibraryMutexGuardCarrier(*owner)) ||
        std::find(visiting.begin(), visiting.end(), current) !=
            visiting.end()) {
      return std::nullopt;
    }
    visiting.push_back(current);
    const GenericSubstitution substitution = classSubstitution(current);
    std::vector<StoredReferencePath> paths;
    bool invalid = false;
    for (const ClassBaseTypeInfo &base : owner->bases) {
      invalid = invalid || (!base.interface &&
                            typeTraits(substituteType(base.type, substitution))
                                .containsBorrowedState);
    }
    for (const FieldInfo &field : owner->fields) {
      if (field.declaration == nullptr) {
        continue;
      }
      const auto member = owner->members.find(field.declaration->name().lexeme);
      if (member == owner->members.end()) {
        invalid = invalid || requireComplete;
        continue;
      }
      const SemanticType fieldType = substituteType(
          substituteType(member->second.symbol.type, substitution),
          GenericSubstitution{.types = instanceTypeSubstitution,
                              .values = instanceValueSubstitution});
      if (requireComplete && (fieldType.kind == SemanticType::Unknown ||
                              containsOpenTypeParameter(fieldType))) {
        invalid = true;
        continue;
      }
      std::vector<StoredReferencePath> children;
      if (fieldType.kind == SemanticType::Reference) {
        if (fieldType.arguments.size() != 1) {
          invalid = true;
          continue;
        }
        children.push_back(StoredReferencePath{.reference = fieldType});
      } else if (typeTraits(fieldType).containsBorrowedState) {
        auto inventory = self(self, fieldType);
        if (!inventory) {
          invalid = true;
          continue;
        }
        children = std::move(*inventory);
      }
      for (StoredReferencePath &child : children) {
        child.fields.insert(child.fields.begin(), field.declaration);
        for (auto &condition : child.conditions)
          condition.subobject.insert(condition.subobject.begin(),
                                     field.declaration);
        paths.push_back(std::move(child));
      }
    }
    visiting.pop_back();
    return invalid ? std::nullopt
                   : std::optional<std::vector<StoredReferencePath>>{
                         std::move(paths)};
  };
  return visit(visit, type);
}

auto SemanticVisitor::Impl::storedReferenceInventory(const SemanticType &type)
    const -> std::optional<std::vector<StoredReferencePath>> {
  return collectStoredReferencePaths(type,
                                     StoredReferenceQuery::CompleteInventory);
}

auto SemanticVisitor::Impl::storedBorrowAlternativeInventory(
    const SemanticType &type)
    -> std::optional<std::vector<BorrowAlternativePath>> {
  if (!storedReferenceInventory(type))
    return std::nullopt;
  std::vector<SemanticType> visiting;
  const auto visit = [&](const auto &self, const SemanticType &current)
      -> std::optional<std::vector<BorrowAlternativePath>> {
    if (current.kind == SemanticType::Unknown ||
        containsOpenTypeParameter(current))
      return std::nullopt;
    std::vector<BorrowAlternativePath> result;
    if (current.kind == SemanticType::Reference)
      return current.arguments.size() == 1 ? std::optional{std::move(result)}
                                           : std::nullopt;
    if (std::find(visiting.begin(), visiting.end(), current) != visiting.end())
      return std::nullopt;
    visiting.push_back(current);
    const auto appendChildren = [&](const SemanticType &child, SymbolId field,
                                    const BorrowDependencyCondition *outer) {
      if (!typeTraits(child).containsBorrowedState)
        return true;
      auto children = self(self, child);
      if (!children || field == 0)
        return false;
      for (auto &path : *children) {
        for (auto &condition : path.conditions)
          condition.subobject.insert(condition.subobject.begin(), field);
        if (outer != nullptr)
          path.conditions.insert(path.conditions.begin(), *outer);
        result.push_back(std::move(path));
      }
      return true;
    };
    bool complete = true;
    if (current.kind == SemanticType::Enum) {
      const auto enumeration = enumTypeFor(current);
      complete = enumeration.has_value();
      if (enumeration && enumeration->payload)
        for (std::size_t index = 0; index < enumeration->enumerators.size();
             ++index) {
          const auto &variant = enumeration->enumerators[index];
          const BorrowDependencyCondition outer{.enumeration = current,
                                                .variant = index};
          result.push_back({.conditions = {outer}});
          if (variant.payloadTypes.size() != variant.payloadSymbols.size()) {
            complete = false;
            continue;
          }
          for (std::size_t field = 0; field < variant.payloadTypes.size();
               ++field)
            complete &= appendChildren(variant.payloadTypes[field],
                                       variant.payloadSymbols[field], &outer);
        }
    } else if (typeTraits(current).containsBorrowedState) {
      if (current.hasLambdaShape()) {
        const auto *lambda = semanticModel.findLambda(current.lambdaId);
        const auto captures = current.lambdaCaptureTypes();
        complete = lambda != nullptr && lambda->declaration != nullptr &&
                   lambda->declaration->captures().size() == captures.size();
        if (complete)
          for (std::size_t index = 0; index < captures.size(); ++index) {
            const std::vector<StoredField> path{
                &lambda->declaration->captures()[index]};
            const auto fields = storedFieldSymbols(current, path);
            complete &=
                fields.size() == 1 &&
                appendChildren(captures[index], fields.front(), nullptr);
          }
      } else if (const auto *owner = classInfo(current)) {
        const auto substitution = classSubstitution(current);
        for (const auto &field : owner->fields) {
          if (field.declaration == nullptr)
            continue;
          const auto member =
              owner->members.find(field.declaration->name().lexeme);
          const std::vector<StoredField> path{field.declaration};
          const auto fields = storedFieldSymbols(current, path);
          if (member == owner->members.end() || fields.size() != 1) {
            complete = false;
            continue;
          }
          complete &= appendChildren(
              substituteType(member->second.symbol.type, substitution),
              fields.front(), nullptr);
        }
      } else {
        complete = false;
      }
    }
    visiting.pop_back();
    return complete ? std::optional{std::move(result)} : std::nullopt;
  };
  return visit(visit, type);
}

auto SemanticVisitor::Impl::storedReferencePaths(const SemanticType &type) const
    -> std::vector<StoredReferencePath> {
  auto symbolic =
      collectStoredReferencePaths(type, StoredReferenceQuery::SymbolicPaths);
  return symbolic ? std::move(*symbolic) : std::vector<StoredReferencePath>{};
}

auto SemanticVisitor::Impl::storedReferencePath(const SemanticType &type) const
    -> std::optional<StoredReferencePath> {
  auto paths = storedReferencePaths(type);
  if (paths.size() != 1 || !paths.front().conditions.empty()) {
    return std::nullopt;
  }
  return std::move(paths.front());
}

auto SemanticVisitor::Impl::completeBorrowDependencySet(
    const ExprPtr &expression) const
    -> std::optional<SemanticBorrowDependencySet> {
  if (!expression)
    return std::nullopt;
  if (const auto *construction =
          semanticModel.findContextualConstruction(*expression);
      construction && construction->contextualBorrowDependencies)
    return construction->contextualBorrowDependencies;
  if (const auto *complete = semanticModel.findBorrowDependencySet(*expression))
    return *complete;
  if (const auto *group = exprAs<Grouping>(expression.get()))
    return completeBorrowDependencySet(group->expression());
  if (const auto *field = exprAs<Get>(expression.get())) {
    const auto owner = completeBorrowDependencySet(field->object());
    const auto *place = semanticModel.findPlace(*expression);
    if (owner && place && !place->projections.empty() &&
        place->projections.back().kind == PlaceProjectionKind::Field) {
      const auto symbol = place->projections.back().field;
      SemanticBorrowDependencySet result;
      for (auto dependency : owner->dependencies) {
        if (dependency.designatedFields.empty() ||
            dependency.designatedFields.front() != symbol)
          return std::nullopt;
        dependency.designatedFields.erase(dependency.designatedFields.begin());
        bool qualified = true;
        for (auto &condition : dependency.conditions) {
          if (condition.subobject.empty() ||
              condition.subobject.front() != symbol) {
            qualified = false;
            break;
          }
          condition.subobject.erase(condition.subobject.begin());
        }
        if (!qualified)
          return std::nullopt;
        result.dependencies.push_back(std::move(dependency));
      }
      for (const auto &alternative : owner->possibleAlternatives) {
        BorrowAlternativePath projected;
        for (auto condition : alternative.conditions) {
          if (condition.subobject.empty() ||
              condition.subobject.front() != symbol)
            return std::nullopt;
          condition.subobject.erase(condition.subobject.begin());
          projected.conditions.push_back(std::move(condition));
        }
        if (!projected.conditions.empty())
          result.possibleAlternatives.push_back(std::move(projected));
      }
      if (!result.possibleAlternatives.empty())
        return result;
    }
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary && binary->oper().kind == TokenKind::COMMA)
    return completeBorrowDependencySet(binary->right());
  if (const auto *construction = semanticModel.findConstruction(*expression);
      construction && construction->directInitialization) {
    if (const auto *call = exprAs<Call>(expression.get());
        call && call->arguments().size() == 1)
      return completeBorrowDependencySet(call->arguments().front());
    if (const auto *direct = exprAs<DirectInitializer>(expression.get());
        direct && direct->arguments().size() == 1)
      return completeBorrowDependencySet(direct->arguments().front());
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::conditionalReferenceReturn(
    const SemanticType &result, std::span<const SemanticType> parameters,
    const SemanticType &receiver) const -> bool {
  if (result.kind != SemanticType::Reference || result.arguments.size() != 1)
    return false;
  const auto conditional = [&](const auto &type) {
    const auto inventory = storedReferencePaths(
        type.kind == SemanticType::Reference && type.arguments.size() == 1
            ? type.arguments.front()
            : type);
    return std::any_of(
        inventory.begin(), inventory.end(),
        [](const auto &path) { return !path.conditions.empty(); });
  };
  return conditional(receiver) ||
         std::any_of(parameters.begin(), parameters.end(), conditional);
}

auto SemanticVisitor::Impl::completeFunctionReturnBorrow(
    const CallableCandidate &function,
    const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments, const SourceSpan &site)
    -> std::optional<FunctionBorrowDependencySummary> {
  if (function.virtualMethod ||
      (function.returnType.kind != SemanticType::Enum &&
       !(function.returnType.kind == SemanticType::Class &&
         typeTraits(function.returnType).containsBorrowedState) &&
       !conditionalReferenceReturn(function.returnType, function.parameterTypes,
                                   function.dispatchOwner)) ||
      containsOpenTypeParameter(function.returnType) || !function.declaration ||
      function.declaration->hasCLinkage() ||
      std::any_of(function.parameterTypes.begin(),
                  function.parameterTypes.end(), containsOpenTypeParameter) ||
      std::any_of(typeArguments.begin(), typeArguments.end(),
                  containsOpenTypeParameter) ||
      std::any_of(function.dispatchOwner.arguments.begin(),
                  function.dispatchOwner.arguments.end(),
                  containsOpenTypeParameter) ||
      std::any_of(valueArguments.begin(), valueArguments.end(),
                  [](const auto &value) {
                    return value.kind != CompileTimeValue::UInt64;
                  }) ||
      std::any_of(function.dispatchOwner.valueArguments.begin(),
                  function.dispatchOwner.valueArguments.end(),
                  [](const auto &value) {
                    return value.kind != CompileTimeValue::UInt64;
                  }))
    return std::nullopt;
  const auto *declaration = semanticModel.findFunction(function.id);
  if (!declaration && instanceBaseModel)
    declaration = instanceBaseModel->findFunction(function.id);
  if (!declaration || !declaration->bodyDeclaration() ||
      !declaration->bodyDeclaration()->body())
    return std::nullopt;
  const bool concrete = !typeArguments.empty() || !valueArguments.empty() ||
                        !function.dispatchOwner.arguments.empty() ||
                        !function.dispatchOwner.valueArguments.empty();
  if (!concrete && declaration &&
      declaration->returnType == function.returnType &&
      declaration->parameterTypes == function.parameterTypes &&
      (!instanceAnalysisActive || declaration->completeReturnBorrow))
    return declaration->completeReturnBorrow;

  const InstanceAnalysisKey key{.kind = InstanceAnalysisKind::Function,
                                .declaration = function.id,
                                .classTypes = function.dispatchOwner.arguments,
                                .classValues =
                                    function.dispatchOwner.valueArguments,
                                .types = typeArguments,
                                .values = valueArguments};
  return completeBorrowQuery(key, function.returnType, function.parameterTypes,
                             function.declaration->name(), site);
}

auto SemanticVisitor::Impl::completeConstructorBorrow(
    const ResolvedConstructionInfo &construction, const SourceSpan &site)
    -> std::optional<FunctionBorrowDependencySummary> {
  if (!construction.declaration || !construction.declaration->body() ||
      !construction.declaration->body()->statements().empty() ||
      containsOpenTypeParameter(construction.constructedType) ||
      std::any_of(construction.parameterTypes.begin(),
                  construction.parameterTypes.end(), containsOpenTypeParameter))
    return std::nullopt;
  const InstanceAnalysisKey key{
      .kind = InstanceAnalysisKind::Constructor,
      .declaration = construction.constructor,
      .classTypes = construction.constructedType.arguments,
      .classValues = construction.constructedType.valueArguments,
      .types = construction.typeArguments,
      .values = construction.valueArguments};
  return completeBorrowQuery(
      key, construction.constructedType, construction.parameterTypes,
      construction.declaration->name(), site, construction.declaration);
}

auto SemanticVisitor::Impl::completeBorrowQuery(
    const InstanceAnalysisKey &key, const SemanticType &result,
    std::span<const SemanticType> parameters, const Token &name,
    const SourceSpan &site, const ConstructorDecl *constructor)
    -> std::optional<FunctionBorrowDependencySummary> {
  // Reuse ordinary typed body analysis for both callable and constructor
  // publication. Only normalized declaration-relative facts leave the worker.
  if (!requestSession.completeReturnQueries)
    requestSession.completeReturnQueries =
        std::make_shared<CompleteReturnQueryState>();
  const auto state = requestSession.completeReturnQueries;
  if (const auto found = state->entries.find(key);
      found != state->entries.end())
    return found->second->complete ? found->second->result : std::nullopt;
  const bool discovered = state->discovered.contains(key);
  const bool depthLimit =
      state->depth >= GenericInstantiationLimits::maximumDepth;
  const bool instanceLimit =
      !discovered && state->discovered.size() >=
                         GenericInstantiationLimits::maximumDistinctInstances;
  if (depthLimit || instanceLimit) {
    auto diagnostic = makeDiagnostic(
        DiagnosticId::GenericInstantiationLimit, site,
        {depthLimit ? "result-summary-depth" : "result-summary-instance",
         std::to_string(
             depthLimit
                 ? GenericInstantiationLimits::maximumDepth
                 : GenericInstantiationLimits::maximumDistinctInstances)});
    diagnostic.related.push_back(
        {tokenSpan(name),
         "Declaration requiring this concrete result summary."});
    diagnostic.hints.emplace_back(
        "Ensure type/value-changing generic recursion terminates through a "
        "selected compile-time branch or exact specialization; runtime "
        "conditions do not limit instantiation.");
    diagnostics.push_back(std::move(diagnostic));
    return std::nullopt;
  }
  auto entry = std::make_shared<CompleteReturnQuery>();
  entry->key = key;
  state->entries.emplace(key, entry);
  state->discovered.insert(key);
  ++state->depth;
  auto worker = std::make_unique<Impl>(
      target, sourceGraph, false, workMetrics, sources, rawPointerAudit, false,
      TrustedBodyAnalysis::Complete, sourceDebugProfile);
  if (state->idleCatalogs.empty())
    worker->catalog = catalog.snapshotForInstanceAnalysis();
  else {
    worker->catalog = std::move(state->idleCatalogs.back());
    state->idleCatalogs.pop_back();
  }
  worker->semanticModel = semanticModel;
  worker->macroLookup = macroLookup;
  worker->macroLookupBase = macroLookupBase;
  worker->nextExpansion = nextExpansion;
  worker->valueFlow = valueFlow;
  for (const auto *definition : constexprDefinitionsAnalyzed)
    if (declaredBeforeCurrent(definition))
      worker->constexprDefinitionsAnalyzed.insert(definition);
  for (const auto *definition : constexprConstructorsAnalyzed)
    if (declaredBeforeCurrent(definition))
      worker->constexprConstructorsAnalyzed.insert(definition);
  worker->genericDefaultConstants = genericDefaultConstants;
  worker->defaultArgumentSyntax = defaultArgumentSyntax;
  worker->defaultArgumentDependencies = defaultArgumentDependencies;
  worker->nextLambdaId = nextLambdaId;
  worker->requestSession.constexprWork = requestSession.constexprWork;
  worker->requestSession.rawResultQueries = requestSession.rawResultQueries;
  worker->requestSession.completeReturnQueries = state;
  const auto analysis =
      key.kind == InstanceAnalysisKind::Constructor
          ? worker->analyzeConstructorInstance(key.declaration, key.classTypes,
                                               key.classValues, key.types,
                                               key.values)
          : worker->analyzeFunctionInstance(key.declaration, key.classTypes,
                                            key.classValues, key.types,
                                            key.values);
  if (analysis.valid()) {
    if (key.kind == InstanceAnalysisKind::Constructor) {
      const auto *analyzed =
          constructor ? analysis.model.findConstructor(*constructor) : nullptr;
      if (analyzed && std::equal(parameters.begin(), parameters.end(),
                                 analyzed->parameterTypes.begin(),
                                 analyzed->parameterTypes.end()))
        entry->result = analyzed->completeBorrow;
    } else {
      const auto *analyzed = analysis.model.findFunction(key.declaration);
      if (analyzed && analyzed->returnType == result &&
          std::equal(parameters.begin(), parameters.end(),
                     analyzed->parameterTypes.begin(),
                     analyzed->parameterTypes.end()))
        entry->result = analyzed->completeReturnBorrow;
    }
  } else {
    // A worker's resource exhaustion is a compilation diagnostic, not an
    // unproved-but-otherwise-valid result. Preserve its original source span
    // while unwinding the demanded query chain.
    for (const auto &diagnostic : analysis.diagnostics)
      if (diagnostic.code == "GTI-S2105")
        diagnostics.push_back(diagnostic);
  }
  state->idleCatalogs.push_back(std::move(worker->catalog));
  entry->complete = true;
  --state->depth;
  state->completed.push_back(key);
  while (state->completed.size() >
         CompleteReturnQueryState::maximumRetainedResults) {
    state->entries.erase(state->completed.front());
    state->completed.erase(state->completed.begin());
  }
  return entry->result;
}

auto SemanticVisitor::Impl::normalizedCompleteReturnBorrow(
    const FunctionInfo &function, const SemanticBorrowDependencySet &complete)
    -> std::optional<FunctionBorrowDependencySummary> {
  const auto *syntax = function.bodyDeclaration();
  if (!syntax || function.parameterPack ||
      syntax->parameters().size() != function.parameterTypes.size())
    return std::nullopt;
  return normalizedCompleteBorrow(function, syntax->parameters(),
                                  syntax->receiverMutability(), complete);
}

auto SemanticVisitor::Impl::normalizedCompleteBorrow(
    const FunctionInfo &function, std::span<const Parameter> parameters,
    ReceiverMutability receiverMutability,
    const SemanticBorrowDependencySet &complete)
    -> std::optional<FunctionBorrowDependencySummary> {
  if (parameters.size() != function.parameterTypes.size())
    return std::nullopt;
  const bool directReference =
      function.returnType.kind == SemanticType::Reference &&
      function.returnType.arguments.size() == 1;
  std::optional<std::vector<StoredReferencePath>> inventory;
  if (directReference) {
    inventory.emplace();
    inventory->push_back({.reference = function.returnType});
  } else {
    inventory = storedReferenceInventory(function.returnType);
  }
  if (!inventory)
    return std::nullopt;
  const auto alternatives =
      directReference ? std::optional{std::vector<BorrowAlternativePath>{}}
                      : storedBorrowAlternativeInventory(function.returnType);
  if (!alternatives ||
      (alternatives->empty() != complete.possibleAlternatives.empty()))
    return std::nullopt;
  for (const auto &alternative : complete.possibleAlternatives)
    if (std::find(alternatives->begin(), alternatives->end(), alternative) ==
        alternatives->end())
      return std::nullopt;

  struct ResultPath {
    std::vector<SymbolId> fields;
    SemanticType reference;
    std::vector<BorrowDependencyCondition> conditions;
  };
  std::vector<ResultPath> resultPaths;
  for (const auto &path : *inventory) {
    auto fields = storedFieldSymbols(function.returnType, path.fields);
    auto conditions =
        storedConditionSymbols(function.returnType, path.conditions);
    if (fields.size() != path.fields.size() || !conditions)
      return std::nullopt;
    resultPaths.push_back(
        {std::move(fields), path.reference, std::move(*conditions)});
  }
  const auto active = [&](const auto &conditions) {
    if (conditions.empty())
      return true;
    return std::any_of(
        complete.possibleAlternatives.begin(),
        complete.possibleAlternatives.end(), [&](const auto &alternative) {
          return std::all_of(
              conditions.begin(), conditions.end(), [&](const auto &condition) {
                return std::find(alternative.conditions.begin(),
                                 alternative.conditions.end(),
                                 condition) != alternative.conditions.end();
              });
        });
  };
  // An exact empty alternative needs no invented origin. Conversely every
  // active reference-bearing path must have at least one checked origin.
  for (const auto &path : resultPaths)
    if (active(path.conditions) &&
        std::none_of(complete.dependencies.begin(), complete.dependencies.end(),
                     [&](const auto &dependency) {
                       return dependency.designatedFields == path.fields &&
                              dependency.conditions == path.conditions &&
                              dependency.access ==
                                  path.reference.referenceAccess;
                     }))
      return std::nullopt;

  FunctionBorrowDependencySummary summary{.possibleAlternatives =
                                              complete.possibleAlternatives};
  for (const auto &dependency : complete.dependencies) {
    const auto resultPath = std::find_if(
        resultPaths.begin(), resultPaths.end(), [&](const auto &path) {
          return path.fields == dependency.designatedFields &&
                 path.conditions == dependency.conditions &&
                 path.reference.kind == SemanticType::Reference &&
                 path.reference.arguments.size() == 1 &&
                 path.reference.referenceAccess == dependency.access;
        });
    if (resultPath == resultPaths.end() || !active(dependency.conditions))
      return std::nullopt;
    BorrowedFieldOrigin normalized{.resultFields = dependency.designatedFields,
                                   .access = dependency.access,
                                   .resultConditions = dependency.conditions};
    SemanticType sourceType = SemanticType::Unknown;
    bool physicalCallerStorage = false;
    AccessMode sourceAccess = AccessMode::ReadOnly;
    if (dependency.place.receiver) {
      if (dependency.place.root != 0 || function.ownerClass == 0 ||
          function.staticMember)
        return std::nullopt;
      normalized.origin = BorrowOriginKind::Receiver;
      sourceType = openClassType(function.ownerClass);
      physicalCallerStorage =
          receiverMutability != ReceiverMutability::Consuming;
      sourceAccess = receiverAllowsMutation(receiverMutability)
                         ? AccessMode::Mutable
                         : AccessMode::ReadOnly;
    } else {
      for (std::size_t index = 0; index < function.parameterTypes.size();
           ++index) {
        const auto *binding = semanticModel.findBinding(parameters[index]);
        if (!binding || binding->symbol == 0 ||
            binding->symbol != dependency.place.root)
          continue;
        if (normalized.origin != BorrowOriginKind::None)
          return std::nullopt;
        normalized.origin = BorrowOriginKind::Argument;
        normalized.parameter = index;
        sourceType = function.parameterTypes[index];
        if (sourceType.kind == SemanticType::Reference &&
            sourceType.arguments.size() == 1) {
          physicalCallerStorage = true;
          sourceAccess = sourceType.referenceAccess;
          sourceType = SemanticType(sourceType.arguments.front());
        }
      }
      // Global or static storage outlives every caller: the result depends
      // on that exact static place, as a legacy Global origin does.
      if (normalized.origin == BorrowOriginKind::None)
        if (const auto *global =
                semanticModel.database().findSymbol(dependency.place.root);
            global != nullptr &&
            (global->threadLocal ||
             global->kind == SymbolKind::GlobalVariable ||
             (global->kind == SymbolKind::Field && global->staticMember))) {
          normalized.origin = BorrowOriginKind::Global;
          normalized.globalRoot = dependency.place.root;
          sourceType = global->type;
          physicalCallerStorage = true;
          sourceAccess = global->mutableBinding ? AccessMode::Mutable
                                                : AccessMode::ReadOnly;
        }
      if (normalized.origin == BorrowOriginKind::None)
        return std::nullopt;
    }

    bool externalReferent = false;
    for (const auto &projection : dependency.place.projections) {
      // A class's checked pointee accessor (an owning handle's operator*)
      // reaches storage that class owns: the path ends at the handle, whose
      // storage then contains the referent.
      if (projection.kind == PlaceProjectionKind::Dereference &&
          sourceType.kind == SemanticType::Class && !externalReferent)
        break;
      if (projection.kind != PlaceProjectionKind::Field ||
          projection.field == 0)
        return std::nullopt;
      SemanticType fieldType = SemanticType::Unknown;
      if (sourceType.kind == SemanticType::Enum) {
        const auto enumeration = enumTypeFor(sourceType);
        if (!enumeration || !enumeration->payload)
          return std::nullopt;
        for (const auto &variant : enumeration->enumerators) {
          if (variant.payloadSymbols.size() != variant.payloadTypes.size())
            return std::nullopt;
          for (std::size_t index = 0; index < variant.payloadTypes.size();
               ++index)
            if (variant.payloadSymbols[index] == projection.field) {
              if (fieldType != SemanticType::Unknown)
                return std::nullopt;
              fieldType = variant.payloadTypes[index];
              normalized.sourceConditions.push_back(
                  {.subobject = normalized.sourceFields,
                   .enumeration = sourceType,
                   .variant = variant.variantIndex});
            }
        }
      } else if (const auto *owner = classInfo(sourceType)) {
        for (const auto &[_, member] : owner->members) {
          if (member.symbol.staticMember ||
              member.symbol.bindingKind != SemanticBindingKind::Field ||
              toolingSymbolFor(member.symbol) != projection.field)
            continue;
          if (fieldType != SemanticType::Unknown)
            return std::nullopt;
          fieldType = substituteSymbol(member.symbol, sourceType).type;
        }
      }
      if (fieldType == SemanticType::Unknown)
        return std::nullopt;
      normalized.sourceFields.push_back(projection.field);
      if (fieldType.kind == SemanticType::Reference) {
        if (fieldType.arguments.size() != 1)
          return std::nullopt;
        // This path designates the external referent, not the lifetime of the
        // by-value carrier shell. A readonly shell does not rewrite the
        // independently checked reference field's declared access mode.
        externalReferent = true;
        sourceAccess = fieldType.referenceAccess;
        fieldType = SemanticType(fieldType.arguments.front());
      }
      sourceType = std::move(fieldType);
    }
    // Caller storage reached without crossing a stored reference may hold
    // the referent anywhere inside it, as an accessor's receiver or an owned
    // element buffer does. The result then depends on that whole storage,
    // exactly as an ordinary receiver or argument borrow origin does. Storage
    // that itself stores references needs an exact path instead, so that an
    // external referent is never confused with the carrier's own storage.
    const auto containedInventory = storedReferenceInventory(sourceType);
    const bool containedReferent = physicalCallerStorage && !externalReferent &&
                                   containedInventory &&
                                   containedInventory->empty();
    if ((!physicalCallerStorage && !externalReferent) ||
        (sourceType != resultPath->reference.arguments.front() &&
         !containedReferent) ||
        (dependency.access == AccessMode::Mutable &&
         sourceAccess != AccessMode::Mutable))
      return std::nullopt;

    // The entry authority is caller-owned. A local cleanup-bound capability
    // cannot escape merely because its underlying place has that same root.
    std::unordered_set<SemanticLoanId> visiting;
    std::unordered_set<SemanticLoanId> checked;
    std::vector<std::pair<SemanticLoanId, bool>> pending{
        {dependency.parent, false}};
    while (!pending.empty()) {
      const auto [id, completed] = pending.back();
      pending.pop_back();
      if (id == 0 || checked.contains(id))
        continue;
      if (completed) {
        visiting.erase(id);
        checked.insert(id);
        continue;
      }
      if (!visiting.insert(id).second)
        return std::nullopt;
      const auto *loan = semanticModel.findLoan(id);
      if (!loan || (loan->lifecycleBound && !loan->entry))
        return std::nullopt;
      pending.emplace_back(id, true);
      pending.emplace_back(loan->parent, false);
      pending.emplace_back(loan->storedDependency, false);
      for (const auto &additional : loan->additionalDependencies)
        pending.emplace_back(additional.parent, false);
    }
    if (function.explicitReturnBorrowOrigin) {
      if (normalized.origin != function.returnBorrowOrigin)
        return std::nullopt;
      if (normalized.origin == BorrowOriginKind::Argument &&
          normalized.parameter != function.returnBorrowParameter &&
          std::find(function.returnBorrowParameters.begin(),
                    function.returnBorrowParameters.end(),
                    normalized.parameter) ==
              function.returnBorrowParameters.end())
        return std::nullopt;
      if (!function.returnBorrowFields.empty() &&
          std::find(function.returnBorrowFields.begin(),
                    function.returnBorrowFields.end(),
                    normalized) == function.returnBorrowFields.end())
        return std::nullopt;
    }
    if (std::find(summary.dependencies.begin(), summary.dependencies.end(),
                  normalized) == summary.dependencies.end())
      summary.dependencies.push_back(std::move(normalized));
  }
  return summary;
}

auto SemanticVisitor::Impl::instantiateCompleteCallBorrow(
    const Call &call, const ResolvedCallInfo &resolved,
    const FunctionBorrowDependencySummary &summary, bool *noNormalResult)
    -> std::optional<SemanticBorrowDependencySet> {
  std::vector<const ExprPtr *> arguments;
  for (std::size_t index = 0;
       index < resolvedCallArgumentCount(call, &resolved); ++index)
    arguments.push_back(resolvedCallArgument(call, &resolved, index));
  const auto *member = exprAs<Get>(call.callee().get());
  return instantiateCompleteBorrowArguments(
      resolved, summary, arguments, member ? &member->object() : nullptr,
      noNormalResult);
}

auto SemanticVisitor::Impl::instantiateCompleteBorrowArguments(
    const ResolvedCallInfo &resolved,
    const FunctionBorrowDependencySummary &summary,
    std::span<const ExprPtr *const> arguments, const ExprPtr *receiver,
    bool *noNormalResult) -> std::optional<SemanticBorrowDependencySet> {
  if (noNormalResult)
    *noNormalResult = false;
  if (resolved.dispatch != CallDispatch::Static ||
      resolved.intrinsic != IntrinsicKind::None || resolved.neverReturns)
    return std::nullopt;
  using Availability = BorrowSupplierAvailability;
  std::vector<Availability> availability(summary.dependencies.size(),
                                         Availability::Unknown);
  std::vector<std::vector<SemanticLoanDependency>> supplying(
      summary.dependencies.size());
  const auto contains = [](const auto &all, const auto &required) {
    return std::all_of(required.begin(), required.end(), [&](const auto &item) {
      return std::find(all.begin(), all.end(), item) != all.end();
    });
  };
  for (std::size_t index = 0; index < summary.dependencies.size(); ++index) {
    const auto &mapping = summary.dependencies[index];
    if (mapping.origin == BorrowOriginKind::Global) {
      // Static storage needs no caller input: every caller's result depends
      // on that exact place, in the loan-flow domain every place shares.
      if (mapping.globalRoot == 0)
        continue;
      SemanticLoanDependency dependency{.place = {.root = mapping.globalRoot},
                                        .designatedFields =
                                            mapping.resultFields,
                                        .access = mapping.access,
                                        .protectsStorage = true,
                                        .conditions = mapping.resultConditions};
      for (const auto field : mapping.sourceFields)
        dependency.place.projections.push_back(
            {.kind = PlaceProjectionKind::Field, .field = field});
      supplying[index].push_back(std::move(dependency));
      availability[index] = Availability::Available;
      continue;
    }
    const ExprPtr *argument = nullptr;
    SemanticType formal = SemanticType::Unknown;
    bool physical = false;
    if (mapping.origin == BorrowOriginKind::Argument &&
        mapping.parameter < resolved.parameterTypes.size() &&
        mapping.parameter < arguments.size()) {
      argument = arguments[mapping.parameter];
      formal = resolved.parameterTypes[mapping.parameter];
      if (formal.kind == SemanticType::Reference &&
          formal.arguments.size() == 1) {
        physical = true;
        formal = SemanticType(formal.arguments.front());
      }
    } else if (mapping.origin == BorrowOriginKind::Receiver) {
      argument = receiver;
      formal = resolved.dispatchOwner;
      physical = resolved.receiverMode != CallReceiverMode::Consuming &&
                 resolved.receiverMode != CallReceiverMode::None;
    }
    if (!argument || !*argument)
      continue;
    const auto inventory = storedReferenceInventory(formal);
    if (!inventory)
      continue;
    const auto input = completeBorrowDependencySet(*argument);
    std::optional<std::vector<SymbolId>> externalPrefix;
    std::vector<BorrowDependencyCondition> externalConditions;
    SemanticType externalType = SemanticType::Unknown;
    bool ambiguous = false;
    for (const auto &path : *inventory) {
      const auto fields = storedFieldSymbols(formal, path.fields);
      if (fields.size() != path.fields.size() || fields.empty()) {
        ambiguous = true;
        break;
      }
      if (mapping.sourceFields.size() < fields.size() ||
          !std::equal(fields.begin(), fields.end(),
                      mapping.sourceFields.begin()))
        continue;
      const auto conditions = storedConditionSymbols(formal, path.conditions);
      if (externalPrefix || !conditions ||
          path.reference.kind != SemanticType::Reference ||
          path.reference.arguments.size() != 1) {
        ambiguous = true;
        break;
      }
      externalPrefix = fields;
      externalConditions = *conditions;
      externalType = path.reference.arguments.front();
    }
    if (ambiguous || (!physical && !externalPrefix))
      continue;
    const auto &inputGuard =
        externalPrefix ? externalConditions : mapping.sourceConditions;
    // Only an actual complete alternative domain can disprove a guard. A
    // scalar reference's empty alternative list says nothing about its
    // referent's tag, nor does a missing input fact prove an impossible case.
    if (input && !inputGuard.empty() && !input->possibleAlternatives.empty() &&
        std::none_of(input->possibleAlternatives.begin(),
                     input->possibleAlternatives.end(), [&](const auto &path) {
                       return contains(path.conditions, inputGuard);
                     })) {
      availability[index] = Availability::Impossible;
      continue;
    }
    std::vector<SemanticLoanDependency> sources;
    std::size_t prefix = 0;
    SemanticType projectedType = formal;
    if (externalPrefix) {
      if (!input)
        continue;
      prefix = externalPrefix->size();
      projectedType = externalType;
      for (const auto &dependency : input->dependencies)
        if (dependency.designatedFields == *externalPrefix &&
            dependency.conditions == externalConditions)
          sources.push_back(dependency);
      // A scalar result is a new reborrow of the input carrier's external
      // permission, not a whole-carrier ownership transfer. Retain that
      // exact live authority so an exclusive input cannot become a competing
      // sibling of its own result. Fresh producers without a retained loan
      // keep the ancestry in their complete producer fact.
      if (resolved.returnType.kind == SemanticType::Reference) {
        const auto retained = retainedLoanSource(*argument);
        const auto *loan =
            retained ? semanticModel.findLoan(retained->loan) : nullptr;
        if (loan && loan->kind == SemanticLoanKind::StoredValue) {
          const auto matches = [&](const SemanticLoanDependency &source,
                                   const SemanticLoanDependency &candidate) {
            auto place = candidate.place;
            if (loan->entry)
              for (const auto field : candidate.designatedFields)
                place.projections.push_back(
                    {.kind = PlaceProjectionKind::Field, .field = field});
            return source.place == place &&
                   source.designatedFields == candidate.designatedFields &&
                   source.conditions == candidate.conditions &&
                   source.access == candidate.access &&
                   source.protectsStorage == candidate.protectsStorage;
          };
          const SemanticLoanDependency primary{
              .place = loan->place,
              .designatedFields = loan->designatedFields,
              .access = loan->access,
              .protectsStorage = loan->protectsStorage,
              .conditions = loan->conditions};
          for (auto &source : sources)
            if (matches(source, primary) ||
                std::any_of(loan->additionalDependencies.begin(),
                            loan->additionalDependencies.end(),
                            [&](const auto &candidate) {
                              return matches(source, candidate);
                            }))
              source.parent = loan->id;
        }
      }
    } else if (!hasStableBorrowStorage(*argument)) {
      // A relayed reference value, such as ?carrier through a checked
      // borrowed_from contract, has no stable place of its own; its complete
      // external dependency set is recorded on the expression itself.
      if (!input || input->dependencies.empty() ||
          std::any_of(input->dependencies.begin(), input->dependencies.end(),
                      [](const auto &dependency) {
                        return !dependency.designatedFields.empty() ||
                               !dependency.conditions.empty();
                      }))
        continue;
      sources = input->dependencies;
    } else {
      // A reference returned by an ordinary call reborrows its one declared
      // origin, so the construction depends on that origin's referent, with
      // no more access than the returned reference grants.
      const ExprPtr *origin = argument;
      while (const auto *call = exprAs<Call>(origin->get())) {
        const auto *selected = semanticModel.findCall(*call);
        const ExprPtr *source = storedBorrowSource(*origin);
        if (!selected || !source || !*source ||
            selected->intrinsic != IntrinsicKind::None ||
            selected->returnType.kind != SemanticType::Reference ||
            (selected->borrowOrigin != BorrowOriginKind::Argument &&
             selected->borrowOrigin != BorrowOriginKind::Receiver) ||
            !selected->borrowFields.empty() ||
            !selected->borrowArguments.empty() ||
            completeBorrowDependencySet(*origin))
          break;
        origin = source;
      }
      const auto *argumentInfo = semanticModel.findExpression(**argument);
      const auto actualPlace = semanticPlace(*origin);
      if (const auto selected = selectedReferenceDependencies(actualPlace)) {
        sources = *selected;
      } else if (const auto source = retainedLoanSource(*origin);
                 source && actualPlace.root &&
                 actualPlace.root->type.kind == SemanticType::Reference) {
        const auto *loan = semanticModel.findLoan(source->loan);
        if (!loan || loan->kind != SemanticLoanKind::Reference ||
            !loan->designatedFields.empty() || !loan->conditions.empty())
          continue;
        // A field of the reference's referent is a sub-place of the same
        // loan's storage; keep the argument's own field path.
        auto place = loan->place;
        const auto projected = loanPlace(actualPlace);
        if (projected.valid() && projected.domain == place.domain &&
            projected.root == place.root &&
            projected.receiver == place.receiver &&
            projected.projections.size() > place.projections.size() &&
            std::equal(place.projections.begin(), place.projections.end(),
                       projected.projections.begin()) &&
            std::all_of(projected.projections.begin() +
                            place.projections.size(),
                        projected.projections.end(), [](const auto &item) {
                          return item.kind == PlaceProjectionKind::Field &&
                                 item.field != 0;
                        }))
          place = projected;
        sources.push_back({.place = std::move(place),
                           .access = loan->access,
                           .protectsStorage = loan->protectsStorage,
                           .parent = loan->id});
        for (auto dependency : loan->additionalDependencies) {
          dependency.parent = loan->id;
          sources.push_back(std::move(dependency));
        }
      } else if (actualPlace.root == nullptr && !actualPlace.receiver &&
                 input && !input->dependencies.empty() &&
                 std::all_of(input->dependencies.begin(),
                             input->dependencies.end(),
                             [](const auto &dependency) {
                               return dependency.designatedFields.empty() &&
                                      dependency.conditions.empty();
                             })) {
        // A relayed reference with no place of its own, such as the result
        // of `?` through a borrowed_from contract, carries its complete
        // external dependency set on the expression itself.
        sources = input->dependencies;
      } else {
        auto place = loanPlace(actualPlace);
        if (!place.valid() || !argumentInfo)
          continue;
        sources.push_back({.place = std::move(place),
                           .access = argumentInfo->access,
                           .protectsStorage = true});
      }
      if (origin != argument && argumentInfo &&
          argumentInfo->access == AccessMode::ReadOnly)
        for (auto &source : sources)
          source.access = AccessMode::ReadOnly;
    }
    if (sources.empty())
      continue;
    // An ordinary subfield of a known referent retains that referent's owner.
    // Crossing another stored reference needs its own complete producer fact;
    // never pretend the intermediate object owns that second external target.
    bool ordinarySuffix = true;
    for (std::size_t field = prefix; field < mapping.sourceFields.size();
         ++field) {
      SemanticType next = SemanticType::Unknown;
      if (const auto enumeration = enumTypeFor(projectedType);
          enumeration && enumeration->payload) {
        for (const auto &variant : enumeration->enumerators)
          for (std::size_t payload = 0; payload < variant.payloadTypes.size();
               ++payload)
            if (payload < variant.payloadSymbols.size() &&
                variant.payloadSymbols[payload] == mapping.sourceFields[field])
              next = variant.payloadTypes[payload];
      } else if (const auto *owner = classInfo(projectedType)) {
        for (const auto &[_, member] : owner->members)
          if (!member.symbol.staticMember &&
              member.symbol.bindingKind == SemanticBindingKind::Field &&
              toolingSymbolFor(member.symbol) == mapping.sourceFields[field])
            next = substituteSymbol(member.symbol, projectedType).type;
      }
      if (next == SemanticType::Unknown ||
          next.kind == SemanticType::Reference) {
        ordinarySuffix = false;
        break;
      }
      projectedType = std::move(next);
    }
    if (!ordinarySuffix)
      continue;
    bool complete = true;
    for (auto dependency : sources) {
      if (!dependency.place.valid() ||
          (mapping.access == AccessMode::Mutable &&
           dependency.access != AccessMode::Mutable) ||
          (!externalPrefix && (!dependency.designatedFields.empty() ||
                               !dependency.conditions.empty()))) {
        complete = false;
        break;
      }
      for (std::size_t field = prefix; field < mapping.sourceFields.size();
           ++field)
        dependency.place.projections.push_back(
            {.kind = PlaceProjectionKind::Field,
             .field = mapping.sourceFields[field]});
      dependency.designatedFields = mapping.resultFields;
      dependency.conditions = mapping.resultConditions;
      dependency.access = mapping.access;
      if (std::find(supplying[index].begin(), supplying[index].end(),
                    dependency) == supplying[index].end())
        supplying[index].push_back(std::move(dependency));
    }
    if (complete)
      availability[index] = Availability::Available;
  }
  const auto domain = restrictCompleteBorrowResultDomain(summary, availability);
  if (noNormalResult)
    *noNormalResult = domain.status == BorrowResultDomainStatus::NoNormalResult;
  if (domain.status != BorrowResultDomainStatus::Known)
    return std::nullopt;
  SemanticBorrowDependencySet result{.possibleAlternatives =
                                         domain.possibleAlternatives};
  for (const auto index : domain.dependencies)
    for (const auto &dependency : supplying[index])
      if (std::find(result.dependencies.begin(), result.dependencies.end(),
                    dependency) == result.dependencies.end())
        result.dependencies.push_back(dependency);
  return result;
}

auto SemanticVisitor::Impl::recordCompleteConstructorBorrow(
    const ConstructorDecl &constructor) -> void {
  const auto *record = semanticModel.findConstructor(constructor);
  if (!record || !currentClass || !constructor.body() ||
      !constructor.body()->statements().empty())
    return;
  const auto type = openClassType(*currentClass);
  const auto inventory = storedReferenceInventory(type);
  if (!inventory || inventory->empty() ||
      std::none_of(inventory->begin(), inventory->end(),
                   [](const auto &path) { return !path.conditions.empty(); }))
    return;
  // Publish one complete stored carrier rather than a selected subset of
  // sibling fields. All dependencies must share its exact field prefix.
  if (inventory->front().fields.empty() ||
      std::any_of(inventory->begin(), inventory->end(), [&](const auto &path) {
        return path.fields.empty() ||
               path.fields.front() != inventory->front().fields.front();
      }))
    return;
  SemanticBorrowDependencySet complete;
  const auto *owner = classInfo(type);
  if (!owner)
    return;
  for (const auto &field : owner->fields) {
    if (!field.declaration)
      return;
    const auto member = owner->members.find(field.declaration->name().lexeme);
    if (member == owner->members.end())
      return;
    const auto fieldType = substituteSymbol(member->second.symbol, type).type;
    if (!typeTraits(fieldType).containsBorrowedState)
      continue;
    const auto fieldSymbol = toolingSymbolFor(member->second.symbol);
    const auto initialized = std::find_if(
        constructor.initializers().begin(), constructor.initializers().end(),
        [&](const auto &initializer) {
          return initializer.target.name.segments.size() == 1 &&
                 initializer.target.name.last().lexeme ==
                     field.declaration->name().lexeme;
        });
    if (!fieldSymbol || initialized == constructor.initializers().end() ||
        initialized->arguments.size() != 1)
      return;
    const auto input =
        completeBorrowDependencySet(initialized->arguments.front());
    if (!input)
      return;
    for (auto dependency : input->dependencies) {
      dependency.designatedFields.insert(dependency.designatedFields.begin(),
                                         fieldSymbol);
      for (auto &condition : dependency.conditions)
        condition.subobject.insert(condition.subobject.begin(), fieldSymbol);
      complete.dependencies.push_back(std::move(dependency));
    }
    for (auto alternative : input->possibleAlternatives) {
      for (auto &condition : alternative.conditions)
        condition.subobject.insert(condition.subobject.begin(), fieldSymbol);
      complete.possibleAlternatives.push_back(std::move(alternative));
    }
  }
  FunctionInfo publication;
  publication.returnType = type;
  for (const auto &parameter : constructor.parameters())
    publication.parameterTypes.push_back(typeOf(parameter));
  const auto summary =
      normalizedCompleteBorrow(publication, constructor.parameters(),
                               ReceiverMutability::Consuming, complete);
  if (!summary)
    return;
  auto updated = *record;
  updated.parameterTypes = publication.parameterTypes;
  updated.completeBorrow = summary;
  const auto *lifecycle =
      owner->declaration ? semanticModel.findClassLifecycle(*owner->declaration)
                         : nullptr;
  if (!lifecycle)
    return;
  auto updatedLifecycle = *lifecycle;
  for (auto &candidate : updatedLifecycle.constructors)
    if (candidate.id == updated.id)
      candidate = updated;
  semanticModel.record(*owner->declaration, std::move(updatedLifecycle));
}

auto SemanticVisitor::Impl::recordCompleteFunctionReturnBorrow(
    const FunctionInfo &function) -> void {
  // Reanalysis must not retain a primary/earlier body's complete fact when
  // this body now has unknown, recursive or nonreturning result evidence.
  const FunctionId id = function.id;
  FunctionInfo cleared = function;
  cleared.completeReturnBorrow.reset();
  if (cleared.declaration)
    semanticModel.record(*cleared.declaration, cleared);
  const FunctionDecl *syntax = cleared.bodyDeclaration();
  if (!syntax || !syntax->body() ||
      (cleared.returnType.kind != SemanticType::Enum &&
       !(cleared.returnType.kind == SemanticType::Class &&
         typeTraits(cleared.returnType).containsBorrowedState) &&
       !conditionalReferenceReturn(cleared.returnType, cleared.parameterTypes,
                                   cleared.ownerClass
                                       ? openClassType(cleared.ownerClass)
                                       : SemanticType::Unknown)) ||
      containsOpenTypeParameter(cleared.returnType) || cleared.neverReturns ||
      loanFlow.deferredGenericFlow || loanFlow.unknownImplicitResultReturn)
    return;

  // Expression facts were recorded at each return's own evaluation point,
  // before a later move or replacement could change its binding. Reuse the
  // existing structural flow walk rather than accumulating mutable state
  // across nested lambdas, demanded bodies or concrete instance analyses.
  std::vector<const ReturnStmt *> returns;
  (void)summarizeFlow(syntax->body()->statements(), &returns, true);
  if (returns.empty() && loanFlow.implicitResultReturns.empty())
    return;
  FunctionBorrowDependencySummary summary;
  const auto merge = [&](const FunctionBorrowDependencySummary &normalized) {
    for (const auto &dependency : normalized.dependencies)
      if (std::find(summary.dependencies.begin(), summary.dependencies.end(),
                    dependency) == summary.dependencies.end())
        summary.dependencies.push_back(dependency);
    for (const auto &alternative : normalized.possibleAlternatives) {
      if (std::find(summary.possibleAlternatives.begin(),
                    summary.possibleAlternatives.end(),
                    alternative) == summary.possibleAlternatives.end())
        summary.possibleAlternatives.push_back(alternative);
    }
  };
  for (const auto *returned : returns) {
    const auto *info = returned->value()
                           ? semanticModel.findExpression(*returned->value())
                           : nullptr;
    const auto complete = completeBorrowDependencySet(returned->value());
    const auto *contextual =
        returned->value()
            ? semanticModel.findContextualConstruction(*returned->value())
            : nullptr;
    // A contextual constructor preserves its source expression as the exact
    // input. The selected construction, not that input's type, describes the
    // returned object; its complete facts remain separate from input facts.
    const bool contextualResult =
        contextual && contextual->contextualBorrowDependencies &&
        contextual->constructedType == cleared.returnType;
    const bool directReference =
        cleared.returnType.kind == SemanticType::Reference &&
        cleared.returnType.arguments.size() == 1;
    if (!info || !complete ||
        (directReference
             ? (info->type != cleared.returnType.arguments.front() ||
                info->category != ValueCategory::Place ||
                (cleared.returnType.referenceAccess == AccessMode::Mutable &&
                 info->access != AccessMode::Mutable))
             : (!contextualResult && info->type != cleared.returnType)))
      return;
    const auto normalized = normalizedCompleteReturnBorrow(cleared, *complete);
    // This source contract is still checked independently against executable
    // MIR return/call facts. Collecting it does not bypass payload admission,
    // ordinary escape checking, or the conditional loan proofs.
    if (!normalized)
      return;
    merge(*normalized);
  }
  // A synthetic early return (prefix `?`) contributes its residual
  // construction's set, recorded at its own evaluation point.
  for (const auto &implicit : loanFlow.implicitResultReturns) {
    const auto normalized = normalizedCompleteReturnBorrow(cleared, implicit);
    if (!normalized)
      return;
    merge(*normalized);
  }
  // Reload after nested body demands and restoration-summary publication.
  const auto *current = semanticModel.findFunction(id);
  if (!current || !current->declaration)
    return;
  FunctionInfo updated = *current;
  // The exact complete source contract also owns the field mapping used by
  // call lowering. Leaving the legacy receiver-only spelling here would
  // incorrectly describe an external referent as the receiver's own storage.
  if (updated.returnType.kind == SemanticType::Class ||
      (updated.returnType.kind == SemanticType::Reference &&
       updated.ownerClass))
    updated.returnBorrowFields = summary.dependencies;
  // A result that depends only on one static place has no caller input to
  // summarize; its declaration borrows that exact global place.
  if (updated.returnBorrowOrigin == BorrowOriginKind::None &&
      !summary.dependencies.empty() &&
      std::all_of(summary.dependencies.begin(), summary.dependencies.end(),
                  [&](const BorrowedFieldOrigin &mapping) {
                    return mapping.origin == BorrowOriginKind::Global &&
                           mapping.globalRoot ==
                               summary.dependencies.front().globalRoot &&
                           mapping.sourceFields ==
                               summary.dependencies.front().sourceFields;
                  })) {
    const auto &global = summary.dependencies.front();
    BorrowOriginPlace place{.root = global.globalRoot};
    for (const auto field : global.sourceFields)
      place.projections.push_back(
          {.kind = PlaceProjectionKind::Field, .field = field});
    updated.returnBorrowOrigin = BorrowOriginKind::Global;
    updated.returnBorrowParameter = 0;
    updated.returnBorrowAccess = global.access;
    updated.returnBorrowPlace = std::move(place);
  }
  updated.completeReturnBorrow = std::move(summary);
  semanticModel.record(*updated.declaration, std::move(updated));
}

auto SemanticVisitor::Impl::payloadBorrowDependencySet(
    const Call &call, const EnumeratorRecord &enumerator)
    -> std::optional<SemanticBorrowDependencySet> {
  if (!enumerator.declaration ||
      call.arguments().size() != enumerator.payloadTypes.size() ||
      enumerator.declaration->payload.size() != enumerator.payloadTypes.size())
    return std::nullopt;
  const BorrowDependencyCondition selected{.enumeration =
                                               enumerator.symbol.type,
                                           .variant = enumerator.variantIndex};
  SemanticBorrowDependencySet result{
      .possibleAlternatives = {{.conditions = {selected}}}};
  for (std::size_t index = 0; index < enumerator.payloadTypes.size(); ++index) {
    const auto &field = enumerator.payloadTypes[index];
    const auto &argument = call.arguments()[index];
    // Text-view provenance is expression-dependent, not represented by the
    // type's ordinary reference-field inventory. An empty inventory cannot
    // certify an owner-dependent text value as dependency-free, except a
    // constructor's text input, whose owner each call publishes.
    const auto &inputs = requestSession.constructorTextPayloadArguments;
    if ((field == SemanticType::StringView || field == SemanticType::CString) &&
        isOwnerDependentTextExpression(argument) &&
        std::find(inputs.begin(), inputs.end(), argument.get()) == inputs.end())
      return std::nullopt;
    const StoredField storedField = &enumerator.declaration->payload[index];
    const auto symbols = storedFieldSymbols(
        enumerator.symbol.type, std::span<const StoredField>(&storedField, 1));
    if (symbols.size() != 1)
      return std::nullopt;
    SemanticBorrowDependencySet input;
    if (field.kind == SemanticType::Reference) {
      const auto *info = semanticModel.findExpression(*argument);
      // Use the same temporary-storage proof as validateReferenceBinding:
      // Place alone is insufficient because a projection may still derive
      // from an owner destroyed at the end of this full expression.
      if (field.arguments.size() != 1 || !info ||
          info->category != ValueCategory::Place ||
          !hasStableBorrowStorage(argument) ||
          (field.referenceAccess == AccessMode::Mutable &&
           info->access != AccessMode::Mutable))
        return std::nullopt;
      if (auto selectedOrigins =
              selectedReferenceDependencies(semanticPlace(argument))) {
        for (auto dependency : *selectedOrigins) {
          if ((dependency.place.root == 0 && !dependency.place.receiver) ||
              (!dependency.protectsStorage &&
               !hasCheckedExternalBorrowOrigin(dependency)) ||
              semanticModel.findForeignStorage(dependency.place.root) !=
                  nullptr ||
              (field.referenceAccess == AccessMode::Mutable &&
               dependency.access != AccessMode::Mutable))
            return std::nullopt;
          dependency.access = field.referenceAccess;
          input.dependencies.push_back(std::move(dependency));
        }
      } else {
        const auto place = retainedLoanPlace(argument);
        const auto source = retainedLoanSource(argument);
        if ((place.root == 0 && !place.receiver) ||
            semanticModel.findForeignStorage(place.root) != nullptr)
          return std::nullopt;
        input.dependencies.push_back(
            {.place = place,
             .access = field.referenceAccess,
             .protectsStorage = hasStableBorrowStorage(argument),
             .parent = source ? source->loan : SemanticLoanId{0}});
      }
    } else {
      const auto inventory = storedReferenceInventory(field);
      if (!inventory)
        return std::nullopt;
      if (inventory->empty()) {
        // A dependency-free field needs no nested active-tag evidence.
        // This is established by its complete concrete layout, never by an
        // absent expression-origin record.
      } else if (auto complete = completeBorrowDependencySet(argument)) {
        input = std::move(*complete);
      } else {
        input.dependencies = storedBorrowDependencies(argument);
        // Existing unconditional class carriers supply exact field-origin
        // maps. Conditional inputs require their producer's complete set;
        // a missing map never proves the selected alternative empty.
        if (input.dependencies.size() != inventory->size())
          return std::nullopt;
        for (std::size_t path = 0; path < inventory->size(); ++path) {
          const auto &required = (*inventory)[path];
          auto &dependency = input.dependencies[path];
          const auto fields = storedFieldSymbols(field, required.fields);
          if (!required.conditions.empty() || !dependency.conditions.empty() ||
              fields.size() != required.fields.size() ||
              (dependency.designatedFields != fields &&
               !(inventory->size() == 1 &&
                 dependency.designatedFields.empty())) ||
              dependency.access != required.reference.referenceAccess)
            return std::nullopt;
          dependency.designatedFields = fields;
        }
      }
    }
    for (auto &dependency : input.dependencies) {
      dependency.designatedFields.insert(dependency.designatedFields.begin(),
                                         symbols.front());
      for (auto &condition : dependency.conditions)
        condition.subobject.insert(condition.subobject.begin(),
                                   symbols.front());
      dependency.conditions.insert(dependency.conditions.begin(), selected);
      result.dependencies.push_back(std::move(dependency));
    }
    for (auto &alternative : input.possibleAlternatives) {
      for (auto &condition : alternative.conditions)
        condition.subobject.insert(condition.subobject.begin(),
                                   symbols.front());
      alternative.conditions.insert(alternative.conditions.begin(), selected);
      if (std::find(result.possibleAlternatives.begin(),
                    result.possibleAlternatives.end(),
                    alternative) == result.possibleAlternatives.end())
        result.possibleAlternatives.push_back(std::move(alternative));
    }
  }
  return result;
}

auto SemanticVisitor::Impl::selectedPayloadReferenceDependencies(
    const ResolvedPayloadPatternInfo &pattern,
    const PayloadBindingInfo &binding,
    const SemanticBorrowDependencySet &source)
    -> std::optional<SemanticBorrowDependencySet> {
  const auto enumeration = enumTypeFor(pattern.type);
  if (!enumeration || !enumeration->payload ||
      !isStoredReferenceExtraction(binding.extractionKind) ||
      binding.binding.type.kind != SemanticType::Reference ||
      binding.binding.type.arguments.size() != 1)
    return std::nullopt;
  const auto variant =
      std::find_if(enumeration->enumerators.begin(),
                   enumeration->enumerators.end(), [&](const auto &candidate) {
                     return candidate.variantIndex == pattern.variantIndex &&
                            candidate.declaration == pattern.declaration;
                   });
  if (variant == enumeration->enumerators.end() ||
      variant->payloadTypes.size() != variant->payloadSymbols.size() ||
      binding.payloadIndex >= variant->payloadTypes.size())
    return std::nullopt;
  const auto &field = variant->payloadTypes[binding.payloadIndex];
  const auto symbol = variant->payloadSymbols[binding.payloadIndex];
  const auto access = binding.binding.type.referenceAccess;
  if (symbol == 0 || field.kind != SemanticType::Reference ||
      field.arguments.size() != 1 ||
      field.arguments != binding.binding.type.arguments ||
      (access == AccessMode::Mutable &&
       field.referenceAccess != AccessMode::Mutable))
    return std::nullopt;
  const BorrowDependencyCondition guard{.enumeration = pattern.type,
                                        .variant = pattern.variantIndex};
  if (std::none_of(source.possibleAlternatives.begin(),
                   source.possibleAlternatives.end(), [&](const auto &path) {
                     return std::find(path.conditions.begin(),
                                      path.conditions.end(),
                                      guard) != path.conditions.end();
                   }))
    return std::nullopt;
  SemanticBorrowDependencySet projected;
  for (auto dependency : source.dependencies) {
    if (dependency.designatedFields != std::vector<SymbolId>{symbol})
      continue;
    // A direct reference is the end of the carrier's stored path. Its outer
    // guard belongs to address selection; the resulting scalar capability
    // retains every possible external owner without a new conditional path.
    if (dependency.conditions !=
            std::vector<BorrowDependencyCondition>{guard} ||
        dependency.access != field.referenceAccess ||
        !dependency.place.valid() ||
        semanticModel.findForeignStorage(dependency.place.root) != nullptr ||
        (!dependency.protectsStorage &&
         !hasCheckedExternalBorrowOrigin(dependency)))
      return std::nullopt;
    dependency.designatedFields.clear();
    dependency.conditions.clear();
    dependency.access = access;
    if (std::find(projected.dependencies.begin(), projected.dependencies.end(),
                  dependency) == projected.dependencies.end())
      projected.dependencies.push_back(std::move(dependency));
  }
  // A reference result is never justified by an empty or missing source set.
  if (projected.dependencies.empty())
    return std::nullopt;
  return projected;
}

auto SemanticVisitor::Impl::joinedBorrowDependencySets(
    const std::optional<SemanticBorrowDependencySet> &left,
    const std::optional<SemanticBorrowDependencySet> &right) const
    -> std::optional<SemanticBorrowDependencySet> {
  if (!left || !right)
    return std::nullopt;
  auto joined = *left;
  for (const auto &dependency : right->dependencies)
    if (std::find(joined.dependencies.begin(), joined.dependencies.end(),
                  dependency) == joined.dependencies.end())
      joined.dependencies.push_back(dependency);
  for (const auto &alternative : right->possibleAlternatives)
    if (std::find(joined.possibleAlternatives.begin(),
                  joined.possibleAlternatives.end(),
                  alternative) == joined.possibleAlternatives.end())
      joined.possibleAlternatives.push_back(alternative);
  return joined;
}

auto SemanticVisitor::Impl::invalidateBorrowDependencySet(
    const SemanticPlace &place) -> void {
  if (place.root == nullptr)
    return;
  place.root->completeBorrowSet.reset();
  // A mutable reference can change its referent's active alternative. Clear
  // the referent's fact, not only the reference binding's physical shell.
  if (place.root->type.kind != SemanticType::Reference)
    return;
  const auto origin = loanPlace(place);
  if (!origin.root)
    return;
  for (auto &scope : scopes)
    for (auto &[_, symbol] : scope)
      if (symbol.completeBorrowSet && toolingSymbolFor(symbol) == origin.root)
        symbol.completeBorrowSet.reset();
}

auto SemanticVisitor::Impl::storedCarrierConstructorParameter(
    const SemanticType &type, const ConstructorInfo &constructor,
    std::span<const SemanticType> parameterTypes) const
    -> std::optional<std::size_t> {
  const auto parameters =
      storedCarrierConstructorParameters(type, constructor, parameterTypes);
  if (parameters.size() != 1) {
    return std::nullopt;
  }
  return parameters.front();
}

auto SemanticVisitor::Impl::storedCarrierConstructorParameters(
    const SemanticType &type, const ConstructorInfo &constructor,
    std::span<const SemanticType> parameterTypes) const
    -> std::vector<std::size_t> {
  const auto bindings = storedCarrierConstructorBindings(
      type, constructor.declaration, parameterTypes);
  std::vector<std::size_t> result;
  for (const auto &binding : bindings)
    if (std::find(result.begin(), result.end(), binding.parameter) ==
        result.end())
      result.push_back(binding.parameter);
  return result;
}

auto SemanticVisitor::Impl::storedCarrierConstructorBindings(
    const SemanticType &type, const ConstructorDecl *constructor,
    std::span<const SemanticType> parameterTypes) const
    -> std::vector<StoredReferenceBinding> {
  // Summaries are needed before constructor bodies are analyzed. Compose only
  // explicit nominal constructor calls whose argument trees terminate in
  // exact enclosing parameters. Ordinary concrete call analysis still owns
  // overload viability, access, movement and exact initialized-field typing.
  // Unambiguous reference-returning calls may compose a declared argument
  // origin. Defaults, packs, arbitrary expressions and recursive summaries
  // deliberately do not acquire a guessed dependency contract here.
  std::vector<const ConstructorDecl *> visiting;
  const auto summarize = [&](const auto &self, const SemanticType &type,
                             const ConstructorDecl *constructor,
                             std::span<const SemanticType> parameterTypes)
      -> std::vector<StoredReferenceBinding> {
    const auto paths = storedReferencePaths(type);
    const ClassInfo *owner = classInfo(type);
    if (paths.empty() || owner == nullptr || constructor == nullptr ||
        visiting.size() >= 64 ||
        std::find(visiting.begin(), visiting.end(), constructor) !=
            visiting.end())
      return {};
    visiting.push_back(constructor);
    struct Pop {
      std::vector<const ConstructorDecl *> &stack;
      ~Pop() { stack.pop_back(); }
    } pop{visiting};
    const auto parameterSource =
        [&](const ExprPtr &expression,
            const SemanticType &expected) -> std::optional<std::size_t> {
      const ExprPtr *argument = &expression;
      while (const auto *group = exprAs<Grouping>(argument->get()))
        argument = &group->expression();
      if (const Call *call = directCall(*argument)) {
        if (expected.kind == SemanticType::Reference) {
          // A declared checked-reference contract can change the referent
          // type while preserving its source. This is not overload selection:
          // require one named candidate and let ordinary body analysis check
          // its concrete arguments, result type, and permission independently.
          const Symbol *callee = nullptr;
          if (const auto *name = exprAs<QualifiedName>(call->callee().get())) {
            const auto &path = name->name();
            const NamePath qualifier(std::vector<Token>(
                path.segments.begin(), path.segments.end() - 1));
            const auto scope =
                resolveNamespacePathGlobally(qualifier, owner->namespaceScope);
            if (scope) {
              const auto found = activeNamespaceSymbols().find(
                  *scope + "::" + path.last().lexeme);
              if (found != activeNamespaceSymbols().end())
                callee = &found->second;
            }
          } else if (const auto *name =
                         exprAs<Variable>(call->callee().get())) {
            if (owner->members.contains(name->name().lexeme) ||
                std::any_of(constructor->parameters().begin(),
                            constructor->parameters().end(),
                            [&](const auto &p) {
                              return p.name.lexeme == name->name().lexeme;
                            }))
              return std::nullopt;
            for (std::size_t depth = owner->namespaceScope.size() + 1;
                 depth > 0 && callee == nullptr; --depth) {
              const auto found = activeNamespaceSymbols().find(qualifiedName(
                  owner->namespaceScope, depth - 1, name->name().lexeme));
              if (found != activeNamespaceSymbols().end())
                callee = &found->second;
            }
          }
          if (callee == nullptr || callee->overloads.size() != 1)
            return std::nullopt;
          const auto &candidate = callee->overloads.front();
          if (candidate.returnType != expected || candidate.parameterPack ||
              candidate.parameterTypes.size() != call->arguments().size() ||
              candidate.returnBorrowOrigin != BorrowOriginKind::Argument ||
              !candidate.returnBorrowParameters.empty() ||
              !candidate.returnBorrowFields.empty() ||
              candidate.returnBorrowParameter >= call->arguments().size())
            return std::nullopt;
          argument = &call->arguments()[candidate.returnBorrowParameter];
          while (const auto *group = exprAs<Grouping>(argument->get()))
            argument = &group->expression();
          const auto *source = exprAs<Variable>(argument->get());
          if (source == nullptr)
            return std::nullopt;
          const auto &parameters = constructor->parameters();
          for (std::size_t index = 0;
               index < parameters.size() && index < parameterTypes.size();
               ++index)
            if (parameters[index].name.lexeme == source->name().lexeme &&
                parameterTypes[index].kind == SemanticType::Reference &&
                parameterTypes[index].referenceAccess ==
                    expected.referenceAccess &&
                !parameters[index].pack)
              return index;
          return std::nullopt;
        }
        const auto intrinsic = intrinsicCandidate(call->callee());
        if (!intrinsic ||
            intrinsic->record().intrinsic != IntrinsicKind::Move ||
            call->arguments().size() != 1 ||
            expected.kind == SemanticType::Reference)
          return std::nullopt;
        argument = &call->arguments().front();
      }
      const Variable *source = movedVariable(*argument);
      if (source == nullptr)
        return std::nullopt;
      const auto &parameters = constructor->parameters();
      for (std::size_t index = 0;
           index < parameters.size() && index < parameterTypes.size(); ++index)
        if (parameters[index].name.lexeme == source->name().lexeme &&
            parameterTypes[index] == expected && !parameters[index].pack)
          return index;
      return std::nullopt;
    };
    struct ArgumentBindings {
      bool valid = false;
      std::vector<StoredReferenceBinding> bindings;
    };
    const auto argumentBindings = [&](const auto &argumentSelf,
                                      const ExprPtr &expression,
                                      const SemanticType &expected,
                                      std::size_t depth) -> ArgumentBindings {
      if (depth >= 64)
        return {};
      if (const auto parameter = parameterSource(expression, expected)) {
        ArgumentBindings result{.valid = true};
        if (expected.kind == SemanticType::Reference) {
          result.bindings.push_back(
              {.destination = {.fields = {}, .reference = expected},
               .parameter = *parameter,
               .sourceFields = {}});
        } else {
          for (const auto &path : storedReferencePaths(expected))
            result.bindings.push_back({.destination = path,
                                       .parameter = *parameter,
                                       .sourceFields = path.fields});
        }
        return result;
      }
      const Call *call = directCall(expression);
      const ClassInfo *nested = classInfo(expected);
      if (call == nullptr || nested == nullptr ||
          expected.kind != SemanticType::Class ||
          storedReferencePaths(expected).empty())
        return {};
      std::optional<ClassId> target;
      if (const auto *name = exprAs<Variable>(call->callee().get())) {
        if (owner->members.contains(name->name().lexeme) ||
            std::any_of(constructor->parameters().begin(),
                        constructor->parameters().end(), [&](const auto &p) {
                          return p.name.lexeme == name->name().lexeme;
                        }))
          return {};
        target = resolveClassPathGlobally(NamePath(name->name()),
                                          owner->namespaceScope);
      } else if (const auto *name =
                     exprAs<QualifiedName>(call->callee().get())) {
        target = resolveClassPathGlobally(name->name(), owner->namespaceScope);
      }
      // Call analysis checks visibility and exact generic arguments. This
      // declaration summary must still find a private implementation type
      // when queried from a caller in a different source unit.
      if (!target || *target != nested->id)
        return {};
      ArgumentBindings result;
      bool matched = false;
      for (const auto &candidate : nested->constructors) {
        if (candidate.declaration == nullptr || candidate.parameterPack ||
            !candidate.genericParameters.empty() ||
            candidate.parameterTypes.size() != call->arguments().size())
          continue;
        auto nestedTypes = candidate.parameterTypes;
        std::vector<ArgumentBindings> arguments;
        for (auto &parameter : nestedTypes) {
          parameter = substituteType(parameter, classSubstitution(expected));
          auto argument =
              argumentSelf(argumentSelf, call->arguments()[arguments.size()],
                           parameter, depth + 1);
          if (!argument.valid)
            break;
          arguments.push_back(std::move(argument));
        }
        if (arguments.size() != nestedTypes.size())
          continue;
        if (matched)
          return {}; // Do not select an ambiguous dependency producer.
        matched = true;
        const auto childBindings =
            self(self, expected, candidate.declaration, nestedTypes);
        if (childBindings.empty())
          return {};
        for (const auto &child : childBindings) {
          if (child.parameter >= arguments.size())
            return {};
          const auto &argument = arguments[child.parameter];
          const auto origin = std::find_if(
              argument.bindings.begin(), argument.bindings.end(),
              [&](const auto &binding) {
                return binding.destination.fields == child.sourceFields &&
                       binding.destination.reference ==
                           child.destination.reference;
              });
          if (origin == argument.bindings.end())
            return {};
          result.bindings.push_back({.destination = child.destination,
                                     .parameter = origin->parameter,
                                     .sourceFields = origin->sourceFields});
        }
        result.valid = true;
      }
      return result;
    };
    std::vector<StoredReferenceBinding> result;
    for (const StoredReferencePath &path : paths) {
      if (path.fields.empty()) {
        return {};
      }
      const VariableDecl *field = path.fields.front().declaration;
      if (field == nullptr)
        return {};
      const auto member = owner->members.find(field->name().lexeme);
      if (member == owner->members.end()) {
        return {};
      }
      const SemanticType fieldType =
          substituteType(member->second.symbol.type, classSubstitution(type));
      std::optional<std::size_t> selected;
      std::vector<StoredField> sourceFields(path.fields.begin() + 1,
                                            path.fields.end());
      for (const ConstructorInitializer &initializer :
           constructor->initializers()) {
        if (initializer.target.name.segments.size() != 1 ||
            initializer.target.name.last().lexeme != field->name().lexeme ||
            initializer.arguments.size() != 1) {
          continue;
        }
        const ExprPtr *argument = &initializer.arguments.front();
        while (const auto *group = exprAs<Grouping>(argument->get())) {
          argument = &group->expression();
        }
        const auto bindings =
            argumentBindings(argumentBindings, *argument, fieldType, 0);
        if (!bindings.valid)
          return {};
        const auto origin = std::find_if(
            bindings.bindings.begin(), bindings.bindings.end(),
            [&](const auto &binding) {
              return binding.destination.fields == sourceFields &&
                     binding.destination.reference == path.reference;
            });
        if (origin == bindings.bindings.end())
          return {};
        selected = origin->parameter;
        sourceFields = origin->sourceFields;
      }
      if (!selected) {
        return {};
      }
      result.push_back({.destination = path,
                        .parameter = *selected,
                        .sourceFields = std::move(sourceFields)});
    }
    return result;
  };
  return summarize(summarize, type, constructor, parameterTypes);
}

auto SemanticVisitor::Impl::storedFieldSymbols(
    SemanticType type, std::span<const StoredField> fields)
    -> std::vector<SymbolId> {
  std::vector<SymbolId> result;
  for (const auto &field : fields) {
    if (field.payload != nullptr) {
      const auto enumeration = enumTypeFor(type);
      if (!enumeration || !enumeration->payload)
        return {};
      bool found = false;
      for (const auto &variant : enumeration->enumerators) {
        if (variant.declaration == nullptr ||
            variant.payloadSymbols.size() != variant.payloadTypes.size())
          return {};
        for (std::size_t index = 0; index < variant.payloadTypes.size();
             ++index) {
          if (&variant.declaration->payload[index] != field.payload)
            continue;
          result.push_back(variant.payloadSymbols[index]);
          type = variant.payloadTypes[index];
          found = true;
          break;
        }
        if (found)
          break;
      }
      if (!found)
        return {};
      continue;
    }
    if (field.capture != nullptr) {
      const auto *lambda = type.hasLambdaShape()
                               ? semanticModel.findLambda(type.lambdaId)
                               : nullptr;
      if (lambda == nullptr)
        return {};
      const auto capture =
          std::find_if(lambda->captures.begin(), lambda->captures.end(),
                       [&](const auto &candidate) {
                         return candidate.capture.lexeme == field.name().lexeme;
                       });
      if (capture == lambda->captures.end() || capture->bindingSymbol == 0)
        return {};
      const auto index = std::size_t(capture - lambda->captures.begin());
      const auto types = type.lambdaCaptureTypes();
      if (index >= types.size())
        return {};
      result.push_back(capture->bindingSymbol);
      type = types[index];
      continue;
    }
    const auto *owner = classInfo(type);
    if (owner == nullptr)
      return {};
    const auto member = owner->members.find(field.name().lexeme);
    if (member == owner->members.end())
      return {};
    const auto symbol = toolingSymbolFor(member->second.symbol);
    if (symbol == 0)
      return {};
    result.push_back(symbol);
    type = substituteType(member->second.symbol.type, classSubstitution(type));
  }
  return result;
}

auto SemanticVisitor::Impl::isDirectStoredReferenceType(
    const SemanticType &type) const -> bool {
  return !storedReferencePaths(type).empty();
}

auto SemanticVisitor::Impl::storedConditionSymbols(
    const SemanticType &type,
    std::span<const StoredReferenceCondition> conditions)
    -> std::optional<std::vector<BorrowDependencyCondition>> {
  std::vector<BorrowDependencyCondition> result;
  for (const auto &condition : conditions) {
    auto fields = storedFieldSymbols(type, condition.subobject);
    const auto enumeration = enumTypeFor(condition.enumeration);
    if (fields.size() != condition.subobject.size() || !enumeration ||
        !enumeration->payload ||
        condition.variant >= enumeration->enumerators.size())
      return std::nullopt;
    result.push_back({.subobject = std::move(fields),
                      .enumeration = condition.enumeration,
                      .variant = condition.variant});
  }
  return result;
}

auto SemanticVisitor::Impl::storedReferenceReferent(
    const SemanticType &type) const -> std::optional<SemanticType> {
  const auto path = storedReferencePath(type);
  if (!path) {
    return std::nullopt;
  }
  const SemanticType &reference = path->reference;
  if (reference.kind != SemanticType::Reference ||
      reference.arguments.size() != 1) {
    return std::nullopt;
  }
  return reference.arguments.front();
}

auto SemanticVisitor::Impl::isBorrowReturningType(
    const SemanticType &type) const -> bool {
  return type.kind == SemanticType::Reference ||
         isDirectStoredReferenceType(type);
}

auto SemanticVisitor::Impl::functionReturnBorrowSummary(
    const SemanticType &returnType,
    const std::vector<SemanticType> &parameterTypes, ClassId ownerClass,
    bool staticMember, IntrinsicKind intrinsic,
    std::optional<std::size_t> exactReturnParameter) const
    -> FunctionReturnBorrowSummary {
  FunctionReturnBorrowSummary summary;
  if (!isBorrowReturningType(returnType) || intrinsic != IntrinsicKind::None) {
    return summary;
  }

  summary.access = borrowAccess(returnType);
  if (ownerClass != 0 && !staticMember) {
    summary.origin = BorrowOriginKind::Receiver;
    return summary;
  }

  // An erased object is opaque: a typed referent is reached through it only
  // by exact unsafe restoration. A reference to one is therefore a weak
  // candidate, considered only when no typed reference parameter is
  // eligible, so a signature-erasing thunk's erased receiver never competes
  // with the signature's own parameters while restoration keeps its origin.
  const auto erasedReferent = [&](const SemanticType &parameter) {
    const ClassInfo *owner = parameter.kind == SemanticType::Reference &&
                                     parameter.arguments.size() == 1
                                 ? classInfo(parameter.arguments.front())
                                 : nullptr;
    return owner != nullptr && owner->compilerCapability ==
                                   CompilerCapabilityTypeKind::ErasedObject;
  };
  const bool typedCandidate = std::any_of(
      parameterTypes.begin(), parameterTypes.end(),
      [&](const SemanticType &parameter) {
        return parameter.kind == SemanticType::Reference &&
               parameter.referenceAccess == borrowAccess(returnType) &&
               !erasedReferent(parameter);
      });
  const auto readOnlyDependency = [&](const SemanticType &parameter) {
    const bool readOnlyReference =
        parameter.kind == SemanticType::Reference &&
        parameter.referenceAccess == AccessMode::ReadOnly &&
        (!typedCandidate || !erasedReferent(parameter)) &&
        (parameter.arguments.empty() ||
         !typeTraits(parameter.arguments.front()).containsBorrowedState ||
         isDirectStoredReferenceType(parameter.arguments.front()));
    const bool readOnlyBorrowedValue =
        isDirectStoredReferenceType(parameter) &&
        borrowAccess(parameter) == AccessMode::ReadOnly;
    return readOnlyReference || readOnlyBorrowedValue;
  };
  const auto mutableDependency = [&](const SemanticType &parameter) {
    return (parameter.kind == SemanticType::Reference &&
            parameter.referenceAccess == AccessMode::Mutable) ||
           (isDirectStoredReferenceType(parameter) &&
            borrowAccess(parameter) == AccessMode::Mutable);
  };
  const auto eligibleDependency = [&](const SemanticType &parameter) {
    return summary.access == AccessMode::Mutable
               ? mutableDependency(parameter)
               : readOnlyDependency(parameter);
  };

  if (exactReturnParameter && *exactReturnParameter < parameterTypes.size() &&
      returnType == parameterTypes[*exactReturnParameter]) {
    const SemanticType &parameter = parameterTypes[*exactReturnParameter];
    if (eligibleDependency(parameter)) {
      summary.origin = BorrowOriginKind::Argument;
      summary.parameter = *exactReturnParameter;
      return summary;
    }
    if (mutableDependency(parameter)) {
      summary.hasMutableCandidate = true;
      return summary;
    }
  }

  for (std::size_t index = 0; index < parameterTypes.size(); ++index) {
    const SemanticType &parameter = parameterTypes[index];
    if (eligibleDependency(parameter)) {
      if (summary.eligibleParameters == 0) {
        summary.parameter = index;
      }
      ++summary.eligibleParameters;
      summary.parameters.push_back(index);
    } else if (summary.access == AccessMode::ReadOnly &&
               mutableDependency(parameter)) {
      summary.hasMutableCandidate = true;
    }
  }

  if (summary.eligibleParameters == 1) {
    summary.origin = BorrowOriginKind::Argument;
    summary.parameters.clear();
  } else if (summary.eligibleParameters > 1 &&
             summary.access == AccessMode::ReadOnly &&
             storedReferencePaths(returnType).size() > 1) {
    summary.origin = BorrowOriginKind::Argument;
  } else {
    summary.parameters.clear();
  }
  return summary;
}

auto SemanticVisitor::Impl::applyFunctionReturnBorrowSummary(
    CallableCandidate &function) const -> void {
  FunctionReturnBorrowSummary summary = functionReturnBorrowSummary(
      function.returnType, function.parameterTypes, function.ownerClass,
      function.staticMember, function.record().intrinsic);
  if (function.declaration != nullptr) {
    if (const auto *attribute =
            function.declaration->findFunctionAttribute("borrowed_from")) {
      // Resolve source spelling to the existing declaration-relative origin.
      // Malformed contracts are diagnosed at the declaration, never inferred
      // afresh by HIR/MIR or the backend.
      (void)attribute;
      const auto contract = genericReturnBorrowContract(
          *function.declaration, function.returnType, function.parameterTypes,
          function.ownerClass, function.staticMember);
      summary = {.origin = contract.origin,
                 .parameter = contract.parameter,
                 .access = contract.access};
    }
  }
  function.returnBorrowOrigin = summary.origin;
  function.returnBorrowParameter = summary.parameter;
  function.returnBorrowAccess = summary.access;
  function.returnBorrowParameters = summary.parameters;
  function.returnBorrowPlace = summary.place;
}

auto SemanticVisitor::Impl::isConditionalBorrowResult(
    const SemanticType &type) const -> bool {
  const auto inventory = storedReferenceInventory(type);
  return inventory &&
         std::any_of(inventory->begin(), inventory->end(),
                     [](const auto &path) { return !path.conditions.empty(); });
}

auto SemanticVisitor::Impl::genericReturnBorrowContract(
    const FunctionDecl &syntax, const SemanticType &returnType,
    std::span<const SemanticType> parameterTypes, ClassId ownerClass,
    bool staticMember) const -> GenericReturnBorrowContract {
  GenericReturnBorrowContract contract;
  const auto *attribute = syntax.findFunctionAttribute("borrowed_from");
  if (attribute == nullptr || !attribute->argument)
    return contract;
  const bool reference = returnType.kind == SemanticType::Reference &&
                         returnType.arguments.size() == 1;
  const bool conditional = !reference && isConditionalBorrowResult(returnType);
  if (!reference && !conditional) {
    return contract;
  }
  // Conditional results retain access per guarded reference path in their
  // complete contract. This scalar compatibility field is not a proof of
  // their individual permissions; normalization checks each actual mapping.
  contract.access =
      reference ? returnType.referenceAccess : AccessMode::ReadOnly;
  if (attribute->argument->kind == TokenKind::THIS) {
    if (ownerClass != 0 && !staticMember)
      contract.origin = BorrowOriginKind::Receiver;
    else
      contract.missing = true;
    return contract;
  }
  // Expanded parameter positions, so the index agrees with MIR's bindings.
  std::vector<SemanticType> expanded;
  for (const SemanticType &type : parameterTypes)
    SemanticType::appendExpandedArgument(expanded, type);
  const auto &parameters = syntax.parameters();
  std::size_t position = 0;
  for (std::size_t index = 0;
       index < parameters.size() && index < parameterTypes.size(); ++index) {
    const std::size_t width =
        parameters[index].pack ? expanded.size() - (parameterTypes.size() - 1)
                               : 1;
    if (parameters[index].name.lexeme == attribute->argument->lexeme) {
      std::optional<std::size_t> selected;
      for (std::size_t element = position;
           element < position + width && element < expanded.size(); ++element) {
        const SemanticType &type = expanded[element];
        // A by-value carrier transfers its external dependency, never the
        // lifetime of its own parameter storage. The executable return proof
        // still requires the exact designated external referent.
        const auto inventory = storedReferenceInventory(type);
        const bool compatibleExternal =
            inventory &&
            std::any_of(
                inventory->begin(), inventory->end(), [&](const auto &path) {
                  return path.reference.kind == SemanticType::Reference &&
                         path.reference.arguments.size() == 1 &&
                         (contract.access == AccessMode::ReadOnly ||
                          path.reference.referenceAccess ==
                              AccessMode::Mutable);
                });
        const bool eligible =
            conditional ? (type.kind == SemanticType::Reference ||
                           (inventory && !inventory->empty()))
                        : (type.kind == SemanticType::Reference ||
                           compatibleExternal) &&
                              (contract.access == AccessMode::ReadOnly ||
                               compatibleExternal ||
                               borrowAccess(type) == AccessMode::Mutable);
        if (!eligible)
          continue;
        if (selected)
          contract.ambiguous = true;
        selected = element;
      }
      if (selected && !contract.ambiguous) {
        contract.origin = BorrowOriginKind::Argument;
        contract.parameter = *selected;
      } else if (!selected) {
        contract.missing = true;
      }
      return contract;
    }
    position += width;
  }
  contract.missing = true;
  return contract;
}

auto SemanticVisitor::Impl::resolveGenericReturnBorrowContract(
    const FunctionInfo &declaration, FunctionInfo &instance) -> void {
  instance.returnBorrowOrigin = BorrowOriginKind::None;
  instance.returnBorrowParameter = 0;
  instance.returnBorrowParameters.clear();
  instance.returnBorrowFields.clear();
  instance.completeReturnBorrow.reset();
  instance.returnBorrowPlace.reset();
  const FunctionDecl *syntax = declaration.bodyDeclaration();
  if (syntax == nullptr) {
    return;
  }
  const GenericReturnBorrowContract contract = genericReturnBorrowContract(
      *syntax, instance.returnType, instance.parameterTypes,
      instance.ownerClass, instance.staticMember);
  instance.returnBorrowAccess = contract.access;
  instance.returnBorrowOrigin = contract.origin;
  instance.returnBorrowParameter = contract.parameter;
  if (contract.ambiguous || contract.missing) {
    const auto *attribute = syntax->findFunctionAttribute("borrowed_from");
    report(attribute && attribute->argument ? *attribute->argument
                                            : syntax->name(),
           contract.ambiguous
               ? "The borrowed_from pack must expand to exactly one reference "
                 "or single-dependency carrier with sufficient access in "
                 "this instance."
               : "The borrowed_from origin must be a reference parameter or "
                 "single-dependency carrier with sufficient access in this "
                 "instance.",
           "GTI-S2017");
  }
}

auto SemanticVisitor::Impl::reportMissingLambdaReturnBorrowSummary(
    const Lambda &lambda, const FunctionReturnBorrowSummary &summary) -> void {
  const Token location = lambda.returnType().name.last();
  Diagnostic diagnostic;
  if (summary.eligibleParameters > 1) {
    diagnostic = makeDiagnostic(
        "GTI-S2017", DiagnosticPhase::Semantics, location,
        "A lambda's borrowed return dependency is ambiguous; exactly one "
        "read-only reference parameter may provide its owner.");
    diagnostic.hints.emplace_back(
        "Reduce the lambda to one read-only reference parameter, or return "
        "an owned value.");
  } else if (summary.hasMutableCandidate ||
             summary.access == AccessMode::Mutable) {
    diagnostic = makeDiagnostic(
        "GTI-S2017", DiagnosticPhase::Semantics, location,
        "A lambda's borrowed return cannot derive from a mutable parameter "
        "because escaping mutable dependency graphs are not supported.");
    diagnostic.hints.emplace_back(
        "Return a read-only reference that borrows one read-only reference "
        "parameter, or return an owned value.");
  } else {
    diagnostic = makeDiagnostic(
        "GTI-S2017", DiagnosticPhase::Semantics, location,
        "A lambda's borrowed return requires exactly one read-only reference "
        "parameter that outlives the result; captures are not result "
        "origins.");
    diagnostic.hints.emplace_back(
        "Add one read-only reference parameter the result borrows from, or "
        "return an owned value.");
  }
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::callableResultOriginContract(const Call &call) const
    -> bool {
  const SemanticType callee = semanticModel.typeOf(*call.callee());
  if (callee.kind == SemanticType::Lambda) {
    const LambdaInfo *lambda = semanticModel.findLambda(callee.lambdaId);
    if (lambda == nullptr && instanceBaseModel != nullptr)
      lambda = instanceBaseModel->findLambda(callee.lambdaId);
    return lambda != nullptr &&
           lambda->returnBorrowOrigin == BorrowOriginKind::Argument &&
           lambda->returnBorrowAccess == AccessMode::ReadOnly;
  }
  if (callee.hasFunctionItemShape()) {
    const FunctionInfo *function =
        semanticModel.findFunction(callee.functionId);
    if (function == nullptr && instanceBaseModel != nullptr)
      function = instanceBaseModel->findFunction(callee.functionId);
    return function != nullptr &&
           function->returnBorrowOrigin == BorrowOriginKind::Argument &&
           function->returnBorrowAccess == AccessMode::ReadOnly;
  }
  return false;
}

auto SemanticVisitor::Impl::reportMissingFunctionReturnBorrowSummary(
    const FunctionDecl &function, const FunctionInfo &info) -> void {
  const FunctionReturnBorrowSummary summary = functionReturnBorrowSummary(
      info.returnType, info.parameterTypes, info.ownerClass, info.staticMember,
      info.intrinsic);
  const std::string code = info.returnType.kind == SemanticType::Reference
                               ? "GTI-S2017"
                               : "GTI-S2045";
  Diagnostic diagnostic;
  if (summary.eligibleParameters > 1) {
    const std::string dependencyDescription =
        summary.access == AccessMode::Mutable
            ? "mutable reference or direct mutable borrowed-value"
            : "read-only reference or direct borrowed-value";
    diagnostic = makeDiagnostic(
        code, DiagnosticPhase::Semantics, function.name(),
        "Borrowed return dependency is ambiguous; exactly one " +
            dependencyDescription + " parameter may provide its owner.");
    for (std::size_t index = 0; index < info.parameterTypes.size() &&
                                index < function.parameters().size();
         ++index) {
      const SemanticType &parameter = info.parameterTypes[index];
      const bool eligible =
          summary.access == AccessMode::Mutable
              ? ((parameter.kind == SemanticType::Reference &&
                  parameter.referenceAccess == AccessMode::Mutable) ||
                 (isDirectStoredReferenceType(parameter) &&
                  borrowAccess(parameter) == AccessMode::Mutable))
              : ((parameter.kind == SemanticType::Reference &&
                  parameter.referenceAccess == AccessMode::ReadOnly &&
                  (parameter.arguments.empty() ||
                   !typeTraits(parameter.arguments.front())
                        .containsBorrowedState)) ||
                 (isDirectStoredReferenceType(parameter) &&
                  borrowAccess(parameter) == AccessMode::ReadOnly));
      if (eligible) {
        diagnostic.related.push_back(
            {tokenSpan(function.parameters()[index].sourceAnchor()),
             "Eligible owner dependency source is here."});
      }
    }
    diagnostic.hints.emplace_back(
        summary.access == AccessMode::Mutable
            ? "Name the owning parameter with [[borrowed_from(name)]], return "
              "the borrowed value from a mutable instance method, or reduce "
              "the function to one mutable dependency source."
            : "Name the owning parameter with [[borrowed_from(name)]], return "
              "the borrowed value from an instance method, or reduce the "
              "function to one read-only dependency source.");
  } else if (summary.hasMutableCandidate) {
    diagnostic = makeDiagnostic(
        code, DiagnosticPhase::Semantics, function.name(),
        "A borrowed return cannot derive from a mutable parameter because "
        "escaping mutable dependency graphs are not supported.");
    diagnostic.hints.emplace_back(
        "Use one read-only reference or immutable borrowed-value parameter "
        "for this return dependency.");
  } else if (summary.access == AccessMode::Mutable) {
    diagnostic = makeDiagnostic(
        code, DiagnosticPhase::Semantics, function.name(),
        "A mutable borrowed return requires either one exact global/static "
        "storage origin or exactly one mutable reference or direct mutable "
        "borrowed-value parameter that outlives the result.");
    diagnostic.hints.emplace_back(
        "Return one exact mutable global place, the exact mutable parameter, "
        "its proven referent, or a direct mutable carrier constructed from "
        "it.");
  } else {
    diagnostic = makeDiagnostic(
        code, DiagnosticPhase::Semantics, function.name(),
        "A free or static borrowed return requires either one exact "
        "global/static storage origin or exactly one read-only reference "
        "or direct borrowed-value parameter that outlives the result.");
    diagnostic.hints.emplace_back(
        "Return the same global/static place on every path, add one "
        "read-only source parameter, or make this an instance method whose "
        "result borrows from its receiver.");
  }
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::borrowAccess(const SemanticType &type) const
    -> AccessMode {
  if (type.kind == SemanticType::Reference) {
    return type.referenceAccess;
  }
  const ClassInfo *owner = classInfo(type);
  if (owner != nullptr && isDefaultLibraryMutexGuardCarrier(*owner)) {
    return AccessMode::Mutable;
  }
  const auto paths = storedReferencePaths(type);
  return paths.empty() ? AccessMode::ReadOnly
                       : paths.front().reference.referenceAccess;
}

auto SemanticVisitor::Impl::nestsBorrowedState(const SemanticType &type) const
    -> bool {
  if (type.kind == SemanticType::Reference) {
    return !type.arguments.empty() &&
           typeTraits(type.arguments.front()).containsBorrowedState;
  }
  return typeTraits(type).containsBorrowedState &&
         !isDirectStoredReferenceType(type);
}

// A carrier composes its fields' origins when it has several reference
// paths, or when a single path runs through a closure capture or ends in an
// opaque erased object: those are environment fields, not another spelling of
// the value itself. A plain single-path class (a span, a view) keeps the
// child-reborrow path, so its reborrows stay children of their source loan.
auto SemanticVisitor::Impl::composedCarrierPaths(
    std::span<const StoredReferencePath> paths) const -> bool {
  // Conditional paths need complete-set/tag proof, not this legacy
  // unconditional constructor-field mapping.
  if (std::any_of(paths.begin(), paths.end(),
                  [](const auto &path) { return !path.conditions.empty(); }))
    return false;
  if (paths.size() > 1)
    return true;
  if (paths.empty())
    return false;
  const auto &path = paths.front();
  if (std::any_of(
          path.fields.begin(), path.fields.end(),
          [](const StoredField &field) { return field.capture != nullptr; }))
    return true;
  const auto *referent = path.reference.kind == SemanticType::Reference &&
                                 path.reference.arguments.size() == 1
                             ? classInfo(path.reference.arguments.front())
                             : nullptr;
  return referent != nullptr && referent->compilerCapability ==
                                    CompilerCapabilityTypeKind::ErasedObject;
}

auto SemanticVisitor::Impl::isBorrowedCarrierReference(
    const SemanticType &type) const -> bool {
  if (type.kind != SemanticType::Reference || type.arguments.size() != 1) {
    return false;
  }
  const auto *referent = classInfo(type.arguments.front());
  // Checked erasure hides the referent's layout, not its physical lifetime.
  // Treat the opaque reference as a shell borrow even when its original
  // object's nested reference fields are no longer visible in the type.
  return type.arguments.front().kind == SemanticType::StringView ||
         type.arguments.front().kind == SemanticType::CString ||
         isDirectStoredReferenceType(type.arguments.front()) ||
         (referent != nullptr && referent->compilerCapability ==
                                     CompilerCapabilityTypeKind::ErasedObject);
}

auto SemanticVisitor::Impl::isDirectOwnerType(const SemanticType &type)
    -> bool {
  return type.kind == SemanticType::UniqueOwner;
}

auto SemanticVisitor::Impl::trustedSynchronizationOperation(
    SourceUnitId sourceUnit, const std::vector<std::string> &scope,
    std::string_view name) const -> SynchronizationOperation {
  if (scope.size() != 1 || scope.front() != "gti_internal") {
    return {};
  }
  if (isMutexLibraryUnit(sourceUnit)) {
    if (name == "mutex_lock") {
      return {.kind = SynchronizationOperationKind::MutexLock};
    }
    if (name == "mutex_unlock") {
      return {.kind = SynchronizationOperationKind::MutexUnlock};
    }
    return {};
  }
  if (!isAtomicLibraryUnit(sourceUnit)) {
    return {};
  }
  const auto operation =
      [](SynchronizationOperationKind kind, AtomicMemoryOrder order,
         std::optional<AtomicMemoryOrder> failureOrder = std::nullopt) {
        return SynchronizationOperation{
            .kind = kind, .order = order, .failureOrder = failureOrder};
      };
  if (name == "atomic_load_relaxed") {
    return operation(SynchronizationOperationKind::AtomicLoad,
                     AtomicMemoryOrder::Relaxed);
  }
  if (name == "atomic_load_acquire") {
    return operation(SynchronizationOperationKind::AtomicLoad,
                     AtomicMemoryOrder::Acquire);
  }
  if (name == "atomic_load") {
    return operation(SynchronizationOperationKind::AtomicLoad,
                     AtomicMemoryOrder::SequentiallyConsistent);
  }
  if (name == "atomic_wait_relaxed") {
    return operation(SynchronizationOperationKind::AtomicWait,
                     AtomicMemoryOrder::Relaxed);
  }
  if (name == "atomic_wait_acquire") {
    return operation(SynchronizationOperationKind::AtomicWait,
                     AtomicMemoryOrder::Acquire);
  }
  if (name == "atomic_wait") {
    return operation(SynchronizationOperationKind::AtomicWait,
                     AtomicMemoryOrder::SequentiallyConsistent);
  }
  if (name == "atomic_notify_one" || name == "atomic_notify_all") {
    return {.kind = SynchronizationOperationKind::AtomicNotify};
  }
  if (name == "atomic_store_relaxed") {
    return operation(SynchronizationOperationKind::AtomicStore,
                     AtomicMemoryOrder::Relaxed);
  }
  if (name == "atomic_store_release") {
    return operation(SynchronizationOperationKind::AtomicStore,
                     AtomicMemoryOrder::Release);
  }
  if (name == "atomic_store") {
    return operation(SynchronizationOperationKind::AtomicStore,
                     AtomicMemoryOrder::SequentiallyConsistent);
  }
  if (name == "atomic_exchange_relaxed") {
    return operation(SynchronizationOperationKind::AtomicReadModifyWrite,
                     AtomicMemoryOrder::Relaxed);
  }
  if (name == "atomic_exchange_acquire") {
    return operation(SynchronizationOperationKind::AtomicReadModifyWrite,
                     AtomicMemoryOrder::Acquire);
  }
  if (name == "atomic_exchange_release") {
    return operation(SynchronizationOperationKind::AtomicReadModifyWrite,
                     AtomicMemoryOrder::Release);
  }
  if (name == "atomic_exchange_acq_rel") {
    return operation(SynchronizationOperationKind::AtomicReadModifyWrite,
                     AtomicMemoryOrder::AcquireRelease);
  }
  if (name == "atomic_exchange") {
    return operation(SynchronizationOperationKind::AtomicReadModifyWrite,
                     AtomicMemoryOrder::SequentiallyConsistent);
  }
  const auto compareExchange = [&](std::string_view candidate,
                                   AtomicMemoryOrder success,
                                   AtomicMemoryOrder failure) {
    return name == candidate
               ? operation(SynchronizationOperationKind::AtomicCompareExchange,
                           success, failure)
               : SynchronizationOperation{};
  };
  for (const auto &[candidate, success, failure] :
       {std::tuple<std::string_view, AtomicMemoryOrder, AtomicMemoryOrder>{
            "atomic_compare_exchange_relaxed_relaxed",
            AtomicMemoryOrder::Relaxed, AtomicMemoryOrder::Relaxed},
        {"atomic_compare_exchange_acquire_relaxed", AtomicMemoryOrder::Acquire,
         AtomicMemoryOrder::Relaxed},
        {"atomic_compare_exchange_acquire_acquire", AtomicMemoryOrder::Acquire,
         AtomicMemoryOrder::Acquire},
        {"atomic_compare_exchange_release_relaxed", AtomicMemoryOrder::Release,
         AtomicMemoryOrder::Relaxed},
        {"atomic_compare_exchange_acq_rel_relaxed",
         AtomicMemoryOrder::AcquireRelease, AtomicMemoryOrder::Relaxed},
        {"atomic_compare_exchange_acq_rel_acquire",
         AtomicMemoryOrder::AcquireRelease, AtomicMemoryOrder::Acquire},
        {"atomic_compare_exchange_seq_cst_relaxed",
         AtomicMemoryOrder::SequentiallyConsistent, AtomicMemoryOrder::Relaxed},
        {"atomic_compare_exchange_seq_cst_acquire",
         AtomicMemoryOrder::SequentiallyConsistent, AtomicMemoryOrder::Acquire},
        {"atomic_compare_exchange", AtomicMemoryOrder::SequentiallyConsistent,
         AtomicMemoryOrder::SequentiallyConsistent}}) {
    SynchronizationOperation result =
        compareExchange(candidate, success, failure);
    if (result.kind != SynchronizationOperationKind::None) {
      return result;
    }
  }
  return {};
}

auto SemanticVisitor::Impl::trustedIntrinsicKind(
    SourceUnitId sourceUnit, const std::vector<std::string> &scope,
    std::string_view name) const -> IntrinsicKind {
  if (scope.size() != 1) {
    return IntrinsicKind::None;
  }
  if (isThreadLibraryUnit(sourceUnit)) {
    if (scope.front() != "gti_internal") {
      return IntrinsicKind::None;
    }
    if (name == "thread_handle_empty" || name == "thread_result_handle_empty") {
      return IntrinsicKind::ThreadHandleEmpty;
    }
    if (name == "thread_spawn" || name == "thread_result_spawn") {
      return IntrinsicKind::ThreadSpawn;
    }
    if (name == "thread_joinable" || name == "thread_result_joinable") {
      return IntrinsicKind::ThreadJoinable;
    }
    if (name == "thread_join" || name == "thread_result_join") {
      return IntrinsicKind::ThreadJoin;
    }
    if (name == "thread_result_take") {
      return IntrinsicKind::ThreadResultTake;
    }
    if (name == "thread_auto_join" || name == "thread_result_auto_join") {
      return IntrinsicKind::ThreadAutoJoin;
    }
    if (name == "thread_scope") {
      return IntrinsicKind::ThreadScope;
    }
    return IntrinsicKind::None;
  }
  if (isAtomicLibraryUnit(sourceUnit)) {
    switch (trustedSynchronizationOperation(sourceUnit, scope, name).kind) {
    case SynchronizationOperationKind::AtomicLoad:
      return IntrinsicKind::AtomicLoad;
    case SynchronizationOperationKind::AtomicStore:
      return IntrinsicKind::AtomicStore;
    case SynchronizationOperationKind::AtomicReadModifyWrite:
      return IntrinsicKind::AtomicExchange;
    case SynchronizationOperationKind::AtomicCompareExchange:
      return IntrinsicKind::AtomicCompareExchange;
    case SynchronizationOperationKind::AtomicWait:
      return IntrinsicKind::AtomicWait;
    case SynchronizationOperationKind::AtomicNotify:
      return name == "atomic_notify_one" ? IntrinsicKind::AtomicNotifyOne
                                         : IntrinsicKind::AtomicNotifyAll;
    default:
      return IntrinsicKind::None;
    }
  }
  if (isMutexLibraryUnit(sourceUnit)) {
    if (scope.front() != "gti_internal") {
      return IntrinsicKind::None;
    }
    if (name == "mutex_lock") {
      return IntrinsicKind::MutexLock;
    }
    if (name == "mutex_unlock") {
      return IntrinsicKind::MutexUnlock;
    }
    if (name == "mutex_guard_active") {
      return IntrinsicKind::MutexGuardActive;
    }
    if (name == "mutex_guard_read") {
      return IntrinsicKind::MutexGuardRead;
    }
    if (name == "mutex_guard_read_mut") {
      return IntrinsicKind::MutexGuardReadMut;
    }
    if (name == "mutex_lock_failure") {
      return IntrinsicKind::MutexLockFailure;
    }
    return IntrinsicKind::None;
  }
  if (isMemoryLibraryUnit(sourceUnit)) {
    if (scope.front() != "gti_internal") {
      return IntrinsicKind::None;
    }
    if (name == "erase_reference")
      return IntrinsicKind::EraseReference;
    if (name == "erase_reference_mut")
      return IntrinsicKind::EraseReferenceMut;
    if (name == "restore_reference")
      return IntrinsicKind::RestoreReference;
    if (name == "restore_reference_mut")
      return IntrinsicKind::RestoreReferenceMut;
    if (name == "object_size") {
      return IntrinsicKind::ObjectSize;
    }
    if (name == "object_alignment") {
      return IntrinsicKind::ObjectAlignment;
    }
    if (name == "object_destroy") {
      return IntrinsicKind::ObjectDestroy;
    }
    if (name == "object_construct") {
      return IntrinsicKind::ObjectConstruct;
    }
    if (name == "object_relocate") {
      return IntrinsicKind::ObjectRelocate;
    }
    if (name == "object_relocate_range") {
      return IntrinsicKind::ObjectRelocateRange;
    }
    if (name == "text_view_from_address") {
      return IntrinsicKind::TextViewFromAddress;
    }
    if (name == "c_string_from_address") {
      return IntrinsicKind::CStringFromAddress;
    }
    if (name == "allocate_unique_owner") {
      return IntrinsicKind::AllocateUniqueOwner;
    }
    if (name == "unique_owner_borrow") {
      return IntrinsicKind::UniqueOwnerBorrow;
    }
    if (name == "unique_owner_borrow_mut") {
      return IntrinsicKind::UniqueOwnerBorrowMut;
    }
    if (name == "unique_owner_is_null") {
      return IntrinsicKind::UniqueOwnerIsNull;
    }
    if (name == "unique_owner_swap") {
      return IntrinsicKind::UniqueOwnerSwap;
    }
    if (name == "unique_owner_upcast") {
      return IntrinsicKind::UniqueOwnerUpcast;
    }
    if (name == "shared_owner_empty") {
      return IntrinsicKind::SharedOwnerEmpty;
    }
    if (name == "shared_owner_from_unique") {
      return IntrinsicKind::SharedOwnerFromUnique;
    }
    if (name == "shared_owner_clone") {
      return IntrinsicKind::SharedOwnerClone;
    }
    if (name == "shared_owner_borrow") {
      return IntrinsicKind::SharedOwnerBorrow;
    }
    if (name == "shared_owner_is_null") {
      return IntrinsicKind::SharedOwnerIsNull;
    }
    if (name == "shared_owner_use_count") {
      return IntrinsicKind::SharedOwnerUseCount;
    }
    if (name == "shared_owner_swap") {
      return IntrinsicKind::SharedOwnerSwap;
    }
    if (name == "shared_owner_upcast") {
      return IntrinsicKind::SharedOwnerUpcast;
    }
    return IntrinsicKind::None;
  }
  if (isFunctionalLibraryUnit(sourceUnit) && scope.front() == "gti_internal") {
    if (name == "function_pointer_from_item")
      return IntrinsicKind::FunctionPointerFromItem;
    if (name == "function_pointer_invoke")
      return IntrinsicKind::FunctionPointerInvoke;
    return IntrinsicKind::None;
  }
  if (isOptionalLibraryUnit(sourceUnit)) {
    if (scope.front() != "gti_internal") {
      return IntrinsicKind::None;
    }
    if (name == "optional_storage_has_value") {
      return IntrinsicKind::OptionalStorageHasValue;
    }
    if (name == "optional_storage_construct") {
      return IntrinsicKind::OptionalStorageConstruct;
    }
    if (name == "optional_storage_read") {
      return IntrinsicKind::OptionalStorageRead;
    }
    if (name == "optional_storage_read_mut") {
      return IntrinsicKind::OptionalStorageReadMut;
    }
    if (name == "optional_storage_reset") {
      return IntrinsicKind::OptionalStorageReset;
    }
    return IntrinsicKind::None;
  }
  if (isPolymorphicLibraryUnit(sourceUnit)) {
    if (scope.front() != "gti_internal") {
      return IntrinsicKind::None;
    }
    if (name == "polymorphic_storage_has_value") {
      return IntrinsicKind::PolymorphicStorageHasValue;
    }
    if (name == "polymorphic_storage_construct") {
      return IntrinsicKind::PolymorphicStorageConstruct;
    }
    if (name == "polymorphic_storage_read") {
      return IntrinsicKind::PolymorphicStorageRead;
    }
    if (name == "polymorphic_storage_read_mut") {
      return IntrinsicKind::PolymorphicStorageReadMut;
    }
    if (name == "polymorphic_storage_reset") {
      return IntrinsicKind::PolymorphicStorageReset;
    }
    return IntrinsicKind::None;
  }
  if (isMetaLibraryUnit(sourceUnit)) {
    if (scope.front() != "gti_internal") {
      return IntrinsicKind::None;
    }
    if (name == "meta_field_count") {
      return IntrinsicKind::MetaFieldCount;
    }
    if (name == "meta_field_name") {
      return IntrinsicKind::MetaFieldName;
    }
    if (name == "meta_field_is_public") {
      return IntrinsicKind::MetaFieldIsPublic;
    }
    if (name == "meta_field_has_default") {
      return IntrinsicKind::MetaFieldHasDefault;
    }
    if (name == "meta_type_name") {
      return IntrinsicKind::MetaTypeName;
    }
    if (name == "meta_type_kind") {
      return IntrinsicKind::MetaTypeKind;
    }
    if (name == "meta_field_attribute_count") {
      return IntrinsicKind::MetaFieldAttributeCount;
    }
    if (name == "meta_field_has_attribute") {
      return IntrinsicKind::MetaFieldHasAttribute;
    }
    if (name == "meta_type_attribute_count") {
      return IntrinsicKind::MetaTypeAttributeCount;
    }
    if (name == "meta_type_has_attribute") {
      return IntrinsicKind::MetaTypeHasAttribute;
    }
    if (name == "meta_type_trait") {
      return IntrinsicKind::MetaTypeTrait;
    }
    if (name == "meta_type_relation") {
      return IntrinsicKind::MetaTypeRelation;
    }
    if (name == "meta_enumerator_count") {
      return IntrinsicKind::MetaEnumeratorCount;
    }
    if (name == "meta_enumerator_name") {
      return IntrinsicKind::MetaEnumeratorName;
    }
    if (name == "meta_enumerator_value") {
      return IntrinsicKind::MetaEnumeratorValue;
    }
    if (name == "meta_payload_count") {
      return IntrinsicKind::MetaPayloadCount;
    }
    if (name == "meta_payload_name") {
      return IntrinsicKind::MetaPayloadName;
    }
    if (name == "meta_base_count") {
      return IntrinsicKind::MetaBaseCount;
    }
    if (name == "meta_argument_count") {
      return IntrinsicKind::MetaArgumentCount;
    }
    static constexpr std::pair<std::string_view, IntrinsicKind> callables[] = {
        {"meta_method_count", IntrinsicKind::MetaMethodCount},
        {"meta_method_name", IntrinsicKind::MetaMethodName},
        {"meta_method_trait", IntrinsicKind::MetaMethodTrait},
        {"meta_method_parameter_count",
         IntrinsicKind::MetaMethodParameterCount},
        {"meta_parameter_name", IntrinsicKind::MetaParameterName},
        {"meta_constructor_count", IntrinsicKind::MetaConstructorCount},
        {"meta_constructor_parameter_count",
         IntrinsicKind::MetaConstructorParameterCount},
        {"meta_constructor_parameter_name",
         IntrinsicKind::MetaConstructorParameterName},
        {"meta_declaration_line", IntrinsicKind::MetaDeclarationLine},
        {"meta_declaration_file", IntrinsicKind::MetaDeclarationFile},
        {"meta_type_primary_name", IntrinsicKind::MetaTypePrimaryName},
        {"meta_construct", IntrinsicKind::MetaConstruct},
    };
    for (const auto &[candidate, kind] : callables) {
      if (name == candidate) {
        return kind;
      }
    }
    return IntrinsicKind::None;
  }
  if (isSourceLocationLibraryUnit(sourceUnit)) {
    if (scope.front() != "gti_internal") {
      return IntrinsicKind::None;
    }
    if (name == "source_file_name") {
      return IntrinsicKind::SourceFileName;
    }
    if (name == "source_function_name") {
      return IntrinsicKind::SourceFunctionName;
    }
    if (name == "source_line") {
      return IntrinsicKind::SourceLine;
    }
    if (name == "source_column") {
      return IntrinsicKind::SourceColumn;
    }
    if (name == "source_expression_text" ||
        name == "source_expression_text_or") {
      return IntrinsicKind::SourceExpressionText;
    }
    return IntrinsicKind::None;
  }
  if (isSimdLibraryUnit(sourceUnit) && scope.front() == "gti_internal") {
    if (name == "simd_add")
      return IntrinsicKind::SimdAdd;
    if (name == "simd_subtract")
      return IntrinsicKind::SimdSubtract;
    if (name == "simd_multiply")
      return IntrinsicKind::SimdMultiply;
    if (name == "simd_divide")
      return IntrinsicKind::SimdDivide;
    if (name == "simd_equal")
      return IntrinsicKind::SimdEqual;
    if (name == "simd_less")
      return IntrinsicKind::SimdLess;
    return IntrinsicKind::None;
  }
  if (isNumericLibraryUnit(sourceUnit)) {
    if (scope.front() != "gti_internal") {
      return IntrinsicKind::None;
    }
    if (name == "integer_wrapping_add") {
      return IntrinsicKind::IntegerWrappingAdd;
    }
    if (name == "integer_wrapping_sub") {
      return IntrinsicKind::IntegerWrappingSubtract;
    }
    if (name == "integer_wrapping_mul") {
      return IntrinsicKind::IntegerWrappingMultiply;
    }
    if (name == "integer_saturating_add") {
      return IntrinsicKind::IntegerSaturatingAdd;
    }
    if (name == "integer_saturating_sub") {
      return IntrinsicKind::IntegerSaturatingSubtract;
    }
    if (name == "integer_saturating_mul") {
      return IntrinsicKind::IntegerSaturatingMultiply;
    }
    if (name == "integer_add_overflows") {
      return IntrinsicKind::IntegerAddOverflows;
    }
    if (name == "integer_sub_overflows") {
      return IntrinsicKind::IntegerSubtractOverflows;
    }
    if (name == "integer_mul_overflows") {
      return IntrinsicKind::IntegerMultiplyOverflows;
    }
    return IntrinsicKind::None;
  }
  if (!isPreludeUnit(sourceUnit)) {
    return IntrinsicKind::None;
  }
  if (scope.front() == "std") {
    return name == "move" ? IntrinsicKind::Move : IntrinsicKind::None;
  }
  if (scope.front() != "gti_internal") {
    return IntrinsicKind::None;
  }
  if (name == "index_bounds_check") {
    return IntrinsicKind::StorageBoundsCheck;
  }
  if (name == "restoring_exchange") {
    return IntrinsicKind::RestoringExchange;
  }
  if (name == "enum_underlying_value") {
    return IntrinsicKind::EnumUnderlyingValue;
  }
  if (name == "restoring_swap") {
    return IntrinsicKind::RestoringSwap;
  }
  if (name == "restoring_indexed_swap") {
    return IntrinsicKind::RestoringIndexedSwap;
  }
  if (name == "require_key") {
    return IntrinsicKind::RequireKey;
  }
  if (name == "require_allocation") {
    return IntrinsicKind::RequireAllocation;
  }
  if (name == "integer_wrapping_add") {
    return IntrinsicKind::IntegerWrappingAdd;
  }
  if (name == "integer_wrapping_sub") {
    return IntrinsicKind::IntegerWrappingSubtract;
  }
  if (name == "require_container_argument") {
    return IntrinsicKind::RequireContainerArgument;
  }
  if (name == "require_vacant_storage") {
    return IntrinsicKind::RequireVacantStorage;
  }
  if (name == "require_live_storage") {
    return IntrinsicKind::RequireLiveStorage;
  }
  if (name == "require_relocation_capacity") {
    return IntrinsicKind::RequireRelocationCapacity;
  }
  if (name == "require_relocation_source") {
    return IntrinsicKind::RequireRelocationSource;
  }
  if (name == "require_relocation_destination") {
    return IntrinsicKind::RequireRelocationDestination;
  }
  if (name == "panic") {
    return IntrinsicKind::Panic;
  }
  if (name == "fail_empty_owner") {
    return IntrinsicKind::FailEmptyOwner;
  }
  if (name == "fail_inactive_value") {
    return IntrinsicKind::FailInactiveValue;
  }
  if (name == "fail_inactive_error") {
    return IntrinsicKind::FailInactiveError;
  }
  return IntrinsicKind::None;
}

auto SemanticVisitor::Impl::intrinsicCandidate(const ExprPtr &callee) const
    -> std::optional<CallableCandidate> {
  const std::optional<Symbol> symbol = resolveExpressionSymbol(callee);
  if (!symbol || symbol->type != SemanticType::Function) {
    return std::nullopt;
  }
  const CallableCandidate *result = nullptr;
  for (const CallableCandidate &candidate : symbol->overloads) {
    if (candidate.record().intrinsic == IntrinsicKind::None) {
      continue;
    }
    if (result != nullptr) {
      return std::nullopt;
    }
    result = &candidate;
  }
  return result == nullptr ? std::nullopt
                           : std::optional<CallableCandidate>(*result);
}

auto SemanticVisitor::Impl::resolvedIntrinsicKind(const Call &call) const
    -> IntrinsicKind {
  const ResolvedCallInfo *resolved = semanticModel.findCall(call);
  return resolved == nullptr ? IntrinsicKind::None : resolved->intrinsic;
}

auto SemanticVisitor::Impl::movedVariable(const ExprPtr &expression)
    -> const Variable * {
  const Expr *candidate = expression.get();
  while (const auto *grouping = exprAs<Grouping>(candidate)) {
    candidate = grouping->expression().get();
  }
  return exprAs<Variable>(candidate);
}

auto SemanticVisitor::Impl::operatorBorrowSource(
    const ExprPtr &expression, const ResolvedOperatorInfo &resolved) const
    -> const ExprPtr * {
  // A conversion's receiver is the converted value and it has no parameters.
  if (const ExprPtr *receiver =
          expression ? explicitConversionReceiver(*expression, resolved)
                     : nullptr) {
    return resolved.borrowOrigin == BorrowOriginKind::Receiver ? receiver
                                                               : nullptr;
  }
  if (resolved.borrowOrigin == BorrowOriginKind::Receiver) {
    if (const auto *call = exprAs<Call>(expression.get())) {
      return &call->callee();
    }
    if (const auto *binary = exprAs<Binary>(expression.get())) {
      return &binary->left();
    }
    if (const auto *member = exprAs<Get>(expression.get())) {
      return &member->object();
    }
    if (const auto *index = exprAs<Index>(expression.get())) {
      return &index->object();
    }
    if (const auto *postfix = exprAs<Postfix>(expression.get())) {
      return &postfix->expression();
    }
    if (const auto *unary = exprAs<Unary>(expression.get())) {
      return &unary->right();
    }
    return nullptr;
  }
  if (resolved.borrowOrigin != BorrowOriginKind::Argument) {
    return nullptr;
  }
  if (const auto *call = exprAs<Call>(expression.get())) {
    return resolved.borrowArgument < call->arguments().size()
               ? &call->arguments()[resolved.borrowArgument]
               : nullptr;
  }
  if (const auto *binary = exprAs<Binary>(expression.get())) {
    return resolved.borrowArgument == 0 ? &binary->right() : nullptr;
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    return resolved.borrowArgument == 0 ? &index->index() : nullptr;
  }
  return nullptr;
}

auto SemanticVisitor::Impl::storedBorrowDependencies(
    const AnalyzedCallArgument &argument)
    -> std::vector<SemanticLoanDependency> {
  if (argument.expression == nullptr)
    return {};
  if (!argument.forwardedPackElement)
    return storedBorrowDependencies(*argument.expression);
  const auto *expansion = exprAs<PackExpansion>(argument.expression->get());
  const auto *carrier =
      expansion == nullptr ? nullptr : resolve(expansion->name());
  if (carrier == nullptr || argument.packPath.empty())
    return {};
  SemanticLoanPlace place{.root = toolingSymbolFor(*carrier)};
  for (const auto index : argument.packPath)
    place.projections.push_back(
        {.kind = PlaceProjectionKind::ConstantIndex, .index = index});
  for (const auto &loan : semanticModel.loans()) {
    if (!loan.entry || loan.kind != SemanticLoanKind::StoredValue ||
        loan.place != place)
      continue;
    std::vector<SemanticLoanDependency> result{
        {.place = loan.place,
         .designatedFields = loan.designatedFields,
         .access = loan.access,
         .protectsStorage = loan.protectsStorage,
         .parent = loan.id,
         .conditions = loan.conditions}};
    result.insert(result.end(), loan.additionalDependencies.begin(),
                  loan.additionalDependencies.end());
    for (auto &dependency : result) {
      dependency.parent = loan.id;
      for (const auto field : dependency.designatedFields)
        dependency.place.projections.push_back(
            {.kind = PlaceProjectionKind::Field, .field = field});
    }
    return result;
  }
  return {};
}

auto SemanticVisitor::Impl::storedBorrowDependencies(const ExprPtr &expression)
    -> std::vector<SemanticLoanDependency> {
  if (!expression) {
    return {};
  }
  // A contextual construction publishing its argument's owner (L-OWN-02) is
  // the converted value: the argument's own dependency at the published path.
  if (const auto *contextual =
          contextualTextArgument == expression.get()
              ? nullptr
              : semanticModel.findContextualConstruction(*expression);
      contextual != nullptr && contextual->dependentTextPublication) {
    if (contextual->borrowFields.size() != 1)
      return {};
    const Expr *enclosing =
        std::exchange(contextualTextArgument, expression.get());
    auto sources = storedBorrowDependencies(expression);
    contextualTextArgument = enclosing;
    const auto &mapping = contextual->borrowFields.front();
    const auto source = std::find_if(
        sources.begin(), sources.end(), [&](const auto &dependency) {
          return dependency.designatedFields == mapping.sourceFields;
        });
    if (source == sources.end() ||
        (mapping.sourceFields.empty() && sources.size() != 1))
      return {};
    publishTextDependency(*source, mapping.resultFields);
    return {std::move(*source)};
  }
  if (const auto *group = exprAs<Grouping>(expression.get())) {
    return storedBorrowDependencies(group->expression());
  }
  // A view read through a reference relay holds what its receiver holds in
  // the designated field, as a read of that field would (L-OWN-02).
  if (const auto *call = exprAs<Call>(expression.get()))
    if (const auto *resolved = semanticModel.findCall(*call))
      if (const auto relay = textReferenceRelay(*call, *resolved);
          relay && relay->origin != nullptr) {
        auto sources = storedBorrowDependencies(*relay->origin);
        for (auto &dependency : sources)
          if (dependency.designatedFields ==
              std::vector<SymbolId>{relay->field}) {
            dependency.designatedFields.clear();
            dependency.access = AccessMode::ReadOnly;
            return {std::move(dependency)};
          }
      }
  // Complete producer/call evidence owns the whole set, including a known
  // empty alternative. Do not reduce it through legacy single-source field
  // inference or fall back to stale carrier facts when it is empty.
  if (auto complete = completeBorrowDependencySet(expression))
    return std::move(complete->dependencies);
  if (const auto dependencies =
          selectedReferenceDependencies(semanticPlace(expression)))
    return *dependencies;
  // Recorded expression dependencies describe closure environments and
  // erased referents, whose dependency layout the type alone cannot name.
  // Ordinary carriers (a fallible borrow, a view) keep the structural
  // derivation below, which aliases their external referent's loan.
  if (const auto dependencies =
          semanticModel.findBorrowDependencies(*expression);
      !dependencies.empty() && composedCarrierPaths(storedReferencePaths(
                                   semanticModel.typeOf(*expression))))
    return {dependencies.begin(), dependencies.end()};
  // A direct initialization that publishes re-roots its argument's text
  // below, rather than being that argument (L-OWN-02).
  if (const auto *construction = semanticModel.findConstruction(*expression);
      construction != nullptr && construction->directInitialization &&
      !construction->dependentTextPublication) {
    if (const auto *call = exprAs<Call>(expression.get()))
      return storedBorrowDependencies(call->arguments().front());
    if (const auto *direct = exprAs<DirectInitializer>(expression.get()))
      return storedBorrowDependencies(direct->arguments().front());
  }
  if (const auto *construction = semanticModel.findConstruction(*expression);
      construction != nullptr && construction->dependentTextPublication) {
    // Each published text field designates its argument's sole owner. A
    // designated list names its inputs in field order.
    const auto *call = exprAs<Call>(expression.get());
    const auto *direct = exprAs<DirectInitializer>(expression.get());
    const auto *arguments = call != nullptr     ? &call->arguments()
                            : direct != nullptr ? &direct->arguments()
                                                : nullptr;
    const auto argument = [&](std::size_t parameter) -> const ExprPtr * {
      if (arguments == nullptr)
        return nullptr;
      if (construction->designatedArguments.empty())
        return parameter < arguments->size() ? &(*arguments)[parameter]
                                             : nullptr;
      if (parameter >= construction->designatedArguments.size())
        return nullptr;
      const auto found = std::find_if(
          arguments->begin(), arguments->end(), [&](const ExprPtr &written) {
            return written.get() ==
                   construction->designatedArguments[parameter];
          });
      return found == arguments->end() ? nullptr : &*found;
    };
    std::vector<SemanticLoanDependency> dependencies;
    for (const auto &mapping : construction->borrowFields) {
      const ExprPtr *input = argument(mapping.parameter);
      if (input == nullptr)
        return {};
      auto sources = storedBorrowDependencies(*input);
      // A re-rooted input names the argument's own designated field.
      const auto source = std::find_if(
          sources.begin(), sources.end(), [&](const auto &dependency) {
            return dependency.designatedFields == mapping.sourceFields;
          });
      if (source == sources.end() ||
          (mapping.sourceFields.empty() && sources.size() != 1))
        return {};
      publishTextDependency(*source, mapping.resultFields);
      dependencies.push_back(std::move(*source));
    }
    return dependencies;
  }
  // A published payload designates its argument's sole owner. A dependent
  // enum value cannot be reassigned, so its alternative is fixed for the
  // dependency's lifetime and the payload symbol alone names it.
  if (const auto *call = exprAs<Call>(expression.get()))
    if (const auto *payload = semanticModel.findPayloadConstruction(*call);
        payload != nullptr && payload->dependentTextPublication) {
      std::vector<SemanticLoanDependency> dependencies;
      for (const auto &mapping : payload->borrowFields) {
        if (mapping.parameter >= call->arguments().size())
          return {};
        auto sources =
            storedBorrowDependencies(call->arguments()[mapping.parameter]);
        if (sources.size() != 1 || !sources.front().designatedFields.empty())
          return {};
        publishTextDependency(sources.front(), mapping.resultFields);
        dependencies.push_back(std::move(sources.front()));
      }
      return dependencies;
    }
  if (const ResolvedConstructionInfo *construction =
          semanticModel.findConstruction(*expression);
      construction != nullptr &&
      !storedCarrierConstructorBindings(construction->constructedType,
                                        construction->declaration,
                                        construction->parameterTypes)
           .empty()) {
    const auto *call = exprAs<Call>(expression.get());
    const auto *initializer = exprAs<DirectInitializer>(expression.get());
    const auto *arguments = call != nullptr          ? &call->arguments()
                            : initializer != nullptr ? &initializer->arguments()
                                                     : nullptr;
    if (arguments == nullptr) {
      return {};
    }
    std::vector<SemanticLoanDependency> dependencies;
    const auto bindings = storedCarrierConstructorBindings(
        construction->constructedType, construction->declaration,
        construction->parameterTypes);
    for (const auto &binding : bindings) {
      const auto parameter = binding.parameter;
      if (parameter >= arguments->size()) {
        return {};
      }
      auto sources = storedBorrowDependencies((*arguments)[parameter]);
      if (sources.empty()) {
        return {};
      }
      const auto sourceFields = storedFieldSymbols(
          construction->parameterTypes[parameter], binding.sourceFields);
      auto source = sources.begin();
      if (!binding.sourceFields.empty()) {
        source = std::find_if(
            sources.begin(), sources.end(), [&](const auto &candidate) {
              return candidate.designatedFields == sourceFields;
            });
        // The existing single-reference path has one unambiguous source even
        // when it has not needed a designated-field identity at its call site.
        if (source == sources.end() && sources.size() == 1 &&
            sources.front().designatedFields.empty())
          source = sources.begin();
      }
      if (source == sources.end() ||
          (binding.sourceFields.empty() && sources.size() != 1))
        return {};
      auto dependency = *source;
      dependency.designatedFields = storedFieldSymbols(
          construction->constructedType, binding.destination.fields);
      if (dependency.designatedFields.empty())
        return {};
      dependency.access = binding.destination.reference.referenceAccess;
      dependencies.push_back(std::move(dependency));
    }
    return dependencies;
  }
  // A function relaying its reference formal's referent text returns a
  // value that views the borrowed argument itself at the relayed path.
  if (const auto *call = exprAs<Call>(expression.get()))
    if (const auto *resolved = semanticModel.findCall(*call);
        resolved != nullptr && resolved->dependentTextReturn &&
        resolved->borrowFields.size() == 1 &&
        resolved->borrowArgument < resolvedCallArgumentCount(*call, resolved)) {
      const ExprPtr &argument =
          *resolvedCallArgument(*call, resolved, resolved->borrowArgument);
      SemanticLoanDependency dependency{
          .place = retainedLoanPlace(argument),
          .designatedFields = resolved->borrowFields.front().resultFields,
          .access = AccessMode::ReadOnly,
          .protectsStorage = hasStableBorrowStorage(argument),
          .parent = accessLoan(argument)};
      if (!dependency.place.valid())
        return {};
      return {std::move(dependency)};
    }
  {
    const auto *call = exprAs<Call>(expression.get());
    const auto *resolved =
        call == nullptr ? nullptr : semanticModel.findCall(*call);
    const auto *operation = semanticModel.findOperator(*expression);
    const std::span<const BorrowedFieldOrigin> mappings =
        operation != nullptr
            ? std::span<const BorrowedFieldOrigin>(operation->borrowFields)
        : resolved != nullptr
            ? std::span<const BorrowedFieldOrigin>(resolved->borrowFields)
            : std::span<const BorrowedFieldOrigin>{};
    if (!mappings.empty()) {
      std::vector<SemanticLoanDependency> dependencies;
      for (const auto &mapping : mappings) {
        const ExprPtr *origin = nullptr;
        if (operation != nullptr) {
          auto selected = *operation;
          selected.borrowOrigin = mapping.origin;
          selected.borrowArgument = mapping.parameter;
          origin = operatorBorrowSource(expression, selected);
        } else if (mapping.origin == BorrowOriginKind::Argument &&
                   mapping.parameter <
                       resolvedCallArgumentCount(*call, resolved))
          origin = resolvedCallArgument(*call, resolved, mapping.parameter);
        else if (mapping.origin == BorrowOriginKind::Receiver)
          if (const auto *member = exprAs<Get>(call->callee().get()))
            origin = &member->object();
        if (origin == nullptr)
          return {};
        auto sources = storedBorrowDependencies(*origin);
        auto selected = std::find_if(
            sources.begin(), sources.end(), [&](const auto &dependency) {
              return !dependency.designatedFields.empty() &&
                     mapping.sourceFields.size() >=
                         dependency.designatedFields.size() &&
                     std::equal(dependency.designatedFields.begin(),
                                dependency.designatedFields.end(),
                                mapping.sourceFields.begin());
            });
        std::size_t matchedFields =
            selected == sources.end() ? 0 : selected->designatedFields.size();
        if (selected == sources.end() && sources.size() == 1 &&
            sources.front().designatedFields.empty()) {
          selected = sources.begin();
          const auto *type = semanticModel.findType(**origin);
          const auto paths = type == nullptr
                                 ? std::vector<StoredReferencePath>{}
                                 : storedReferencePaths(*type);
          if (paths.size() == 1) {
            const auto fields = storedFieldSymbols(*type, paths.front().fields);
            if (mapping.sourceFields.size() >= fields.size() &&
                std::equal(fields.begin(), fields.end(),
                           mapping.sourceFields.begin()))
              matchedFields = fields.size();
          }
        }
        if (selected == sources.end() ||
            (mapping.access == AccessMode::Mutable &&
             selected->access != AccessMode::Mutable))
          return {};
        auto dependency = *selected;
        for (std::size_t index = matchedFields;
             index < mapping.sourceFields.size(); ++index)
          dependency.place.projections.push_back(
              {.kind = PlaceProjectionKind::Field,
               .field = mapping.sourceFields[index]});
        dependency.designatedFields = mapping.resultFields;
        dependency.access = mapping.access;
        dependencies.push_back(std::move(dependency));
      }
      return dependencies;
    }
    const auto *resultType = operation != nullptr  ? &operation->returnType
                             : resolved != nullptr ? &resolved->returnType
                                                   : nullptr;
    const bool compactReceiver =
        operation != nullptr
            ? operation->borrowOrigin == BorrowOriginKind::Receiver &&
                  operation->borrowArguments.empty()
            : resolved != nullptr &&
                  resolved->intrinsic == IntrinsicKind::None &&
                  resolved->borrowOrigin == BorrowOriginKind::Receiver &&
                  resolved->borrowArguments.empty();
    if (compactReceiver && resultType != nullptr &&
        resultType->kind == SemanticType::Class) {
      if (const auto path = storedReferencePath(*resultType)) {
        // A compact receiver-result contract borrows the receiver storage.
        // Rebase that exact place to the returned adapter's field identity,
        // rather than copying field identities from a containing wrapper.
        // The receiver may itself be reached through a stored reference.
        auto place = retainedLoanPlace(expression);
        if (place.root != 0 || place.receiver)
          return {{.place = std::move(place),
                   .designatedFields =
                       storedFieldSymbols(*resultType, path->fields),
                   .access = path->reference.referenceAccess,
                   .protectsStorage = hasStableBorrowStorage(expression)}};
      }
    }
  }
  if (const auto *call = exprAs<Call>(expression.get())) {
    if (const auto *resolved = semanticModel.findCall(*call);
        resolved != nullptr && !resolved->borrowArguments.empty()) {
      std::vector<SemanticLoanDependency> dependencies;
      for (const auto parameter : resolved->borrowArguments) {
        if (parameter >= resolvedCallArgumentCount(*call, resolved)) {
          return {};
        }
        const auto sources = storedBorrowDependencies(
            *resolvedCallArgument(*call, resolved, parameter));
        if (sources.empty()) {
          return {};
        }
        dependencies.insert(dependencies.end(), sources.begin(), sources.end());
      }
      return dependencies;
    }
  }
  if (const auto source = retainedLoanSource(expression)) {
    const SemanticLoanInfo *loan = semanticModel.findLoan(source->loan);
    if (loan != nullptr && loan->kind == SemanticLoanKind::Reference &&
        loan->storedDependency != 0) {
      loan = semanticModel.findLoan(loan->storedDependency);
    }
    if (loan != nullptr && loan->kind == SemanticLoanKind::StoredValue) {
      // A borrowed carrier read through this expression lends its loan as
      // the authority of each dependency. A carrier consumed by a move ends
      // instead: the new owner forwards the carrier's own ancestry rather
      // than becoming a child of a loan that retires with the move.
      const bool transferred = source->transfer;
      std::vector<SemanticLoanDependency> dependencies{
          {.place = loan->place,
           .designatedFields = loan->designatedFields,
           .access = loan->access,
           .protectsStorage = loan->protectsStorage,
           .parent = transferred ? loan->parent : loan->id,
           .conditions = loan->conditions}};
      dependencies.insert(dependencies.end(),
                          loan->additionalDependencies.begin(),
                          loan->additionalDependencies.end());
      if (!transferred)
        for (auto &dependency : dependencies)
          dependency.parent = loan->id;
      if (loan->entry && loan->kind == SemanticLoanKind::StoredValue)
        for (auto &dependency : dependencies)
          for (const auto field : dependency.designatedFields)
            dependency.place.projections.push_back(
                {.kind = PlaceProjectionKind::Field, .field = field});
      // A projected carrier keeps only the fields below that projection.
      // Rebase their identities to the projected value's own layout, so a
      // constructor wrapping it can prepend its destination field without
      // confusing sibling paths on the original receiver.
      const ExprPtr *projected = &expression;
      std::unordered_set<const Expr *> visited;
      while (exprAs<Call>(projected->get()) != nullptr &&
             visited.insert(projected->get()).second) {
        const ExprPtr *origin = storedBorrowSource(*projected);
        if (origin == nullptr)
          break;
        projected = origin;
      }
      const auto *place = semanticModel.findPlace(**projected);
      std::vector<SymbolId> prefix;
      if (place != nullptr)
        for (const auto &projection : place->projections) {
          if (projection.kind != PlaceProjectionKind::Field)
            break;
          prefix.push_back(projection.field);
        }
      if (!prefix.empty()) {
        std::erase_if(dependencies, [&](const auto &dependency) {
          return dependency.designatedFields.size() < prefix.size() ||
                 !std::equal(prefix.begin(), prefix.end(),
                             dependency.designatedFields.begin());
        });
        for (auto &dependency : dependencies)
          dependency.designatedFields.erase(
              dependency.designatedFields.begin(),
              dependency.designatedFields.begin() + prefix.size());
      }
      return dependencies;
    }
  }
  SemanticLoanPlace place = retainedLoanPlace(expression);
  if (place.root == 0 && !place.receiver) {
    return {};
  }
  return {{.place = std::move(place),
           .access = semanticModel.findExpression(*expression) == nullptr
                         ? AccessMode::ReadOnly
                         : semanticModel.findExpression(*expression)->access,
           .protectsStorage = hasStableBorrowStorage(expression),
           .parent = retainedLoanSource(expression)
                         ? retainedLoanSource(expression)->loan
                         : SemanticLoanId{0}}};
}

auto SemanticVisitor::Impl::resolvedBorrowVariable(
    const Variable &variable) const -> const Symbol * {
  const auto *info = semanticModel.findExpression(variable);
  if (info == nullptr)
    return resolve(variable.name());
  const auto identity = semanticModel.findResolvedSymbol(variable);
  if (identity == 0)
    return nullptr;
  const auto exact = [&](const Symbol &symbol) {
    return symbolForDeclaration(symbol.declaration) == identity;
  };
  // Initializers are resolved before their new binding is installed. A later
  // retention query must use that resolved identity, even when the new local
  // shadows the initializer's spelling. Never rebind an analyzed expression.
  for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
    if (const auto found = scope->find(variable.name().lexeme);
        found != scope->end() && exact(found->second))
      return &found->second;
  const auto &symbols = currentNamespaceSymbols();
  for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth)
    if (const auto found = symbols.find(
            qualifiedName(currentNamespace, depth - 1, variable.name().lexeme));
        found != symbols.end() && exact(found->second))
      return &found->second;
  return nullptr;
}

auto SemanticVisitor::Impl::storedBorrowSource(const ExprPtr &expression) const
    -> const ExprPtr * {
  if (!expression) {
    return nullptr;
  }
  if (const auto complete = completeBorrowDependencySet(expression);
      complete && complete->possibleAlternatives.empty() &&
      !complete->dependencies.empty() &&
      std::all_of(complete->dependencies.begin(), complete->dependencies.end(),
                  [](const auto &dependency) {
                    return dependency.designatedFields.empty() &&
                           dependency.conditions.empty();
                  }))
    // A complete scalar result already identifies its external referents.
    // Its argument expression may contain other alternatives/dependencies.
    return nullptr;
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::QUESTION) {
    if (const auto *plan = semanticModel.findTryablePropagation(*unary);
        plan != nullptr &&
        plan->output.returnType.kind == SemanticType::Reference &&
        plan->output.borrowOrigin == BorrowOriginKind::Argument &&
        plan->output.borrowArgument == 0)
      return &unary->right();
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    const Symbol *symbol = resolvedBorrowVariable(*variable);
    if (symbol != nullptr &&
        (symbol->type.kind == SemanticType::Reference ||
         typeTraits(symbol->type).containsBorrowedState) &&
        symbol->variableDeclaration != nullptr &&
        symbol->variableDeclaration->initializer()) {
      return &symbol->variableDeclaration->initializer();
    }
    return nullptr;
  }
  if (const ResolvedOperatorInfo *resolved =
          semanticModel.findOperator(*expression)) {
    return operatorBorrowSource(expression, *resolved);
  }
  const ResolvedConstructionInfo *construction =
      semanticModel.findConstruction(*expression);
  if (construction != nullptr &&
      construction->borrowOrigin == BorrowOriginKind::Argument) {
    if (const auto *call = exprAs<Call>(expression.get())) {
      return construction->borrowArgument < call->arguments().size()
                 ? &call->arguments()[construction->borrowArgument]
                 : nullptr;
    }
    if (const auto *initializer = exprAs<DirectInitializer>(expression.get())) {
      return construction->borrowArgument < initializer->arguments().size()
                 ? &initializer->arguments()[construction->borrowArgument]
                 : nullptr;
    }
  }
  const auto *call = exprAs<Call>(expression.get());
  if (call == nullptr) {
    return nullptr;
  }
  // An origin index past the written arguments names an element of a
  // forwarded pack; the expansion expression is that element's source.
  const auto writtenOrForwarded = [&](std::size_t index) -> const ExprPtr * {
    if (index < call->arguments().size())
      return &call->arguments()[index];
    if (!call->arguments().empty() &&
        exprAs<PackExpansion>(call->arguments().back().get()) != nullptr)
      return &call->arguments().back();
    return nullptr;
  };
  const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
  if (resolved == nullptr) {
    // A lambda value's result borrows the argument its contract names; a
    // generic callable call has no source until its instance selects one.
    const ResolvedLambdaCallInfo *lambdaCall =
        semanticModel.findLambdaCall(*call);
    if (lambdaCall != nullptr &&
        lambdaCall->borrowOrigin == BorrowOriginKind::Argument) {
      return writtenOrForwarded(lambdaCall->borrowArgument);
    }
    return nullptr;
  }
  if (resolved->borrowOrigin == BorrowOriginKind::Argument) {
    if (const ExprPtr *argument =
            resolvedCallArgument(*call, resolved, resolved->borrowArgument))
      return argument;
    return resolved->argumentReceiver == nullptr
               ? writtenOrForwarded(resolved->borrowArgument)
               : nullptr;
  }
  if (resolved->borrowOrigin == BorrowOriginKind::Receiver) {
    if (const auto *member = exprAs<Get>(call->callee().get())) {
      return &member->object();
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::borrowedOwnerVariable(
    const ExprPtr &expression) const -> const Variable * {
  std::unordered_set<const Expr *> visiting;
  return borrowedOwnerVariable(expression, visiting);
}

auto SemanticVisitor::Impl::borrowedOwnerVariable(
    const ExprPtr &expression, std::unordered_set<const Expr *> &visiting) const
    -> const Variable * {
  if (!expression || !visiting.insert(expression.get()).second) {
    return nullptr;
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    if (const ExprPtr *source = storedBorrowSource(expression)) {
      if (const Variable *owner = borrowedOwnerVariable(*source, visiting)) {
        return owner;
      }
    }
    return variable;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return borrowedOwnerVariable(grouping->expression(), visiting);
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return borrowedOwnerVariable(binary->right(), visiting);
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    return borrowedOwnerVariable(index->object(), visiting);
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    return borrowedOwnerVariable(member->object(), visiting);
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::STAR) {
    return borrowedOwnerVariable(unary->right(), visiting);
  }
  if (const ExprPtr *source = storedBorrowSource(expression)) {
    return borrowedOwnerVariable(*source, visiting);
  }
  return nullptr;
}

auto SemanticVisitor::Impl::directStorageVariable(
    const ExprPtr &expression) const -> const Variable * {
  if (!expression) {
    return nullptr;
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    return variable;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return directStorageVariable(grouping->expression());
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return directStorageVariable(binary->right());
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    return directStorageVariable(index->object());
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    return directStorageVariable(member->object());
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::STAR) {
    return directStorageVariable(unary->right());
  }
  return nullptr;
}

auto SemanticVisitor::Impl::hasRetainedBorrow(const Variable &variable) const
    -> bool {
  const Symbol *symbol = resolve(variable.name());
  return symbol != nullptr && !symbol->storageLoans.empty();
}

auto SemanticVisitor::Impl::projectedStoredDependency(
    const SemanticLoanInfo &loan,
    std::span<const PlaceProjection> projections) const
    -> std::optional<SemanticLoanDependency> {
  if (loan.kind != SemanticLoanKind::StoredValue)
    return std::nullopt;
  const auto select = [&](SemanticLoanDependency dependency)
      -> std::optional<SemanticLoanDependency> {
    const auto &fields = dependency.designatedFields;
    if (fields.empty() || projections.size() < fields.size())
      return std::nullopt;
    for (std::size_t index = 0; index < fields.size(); ++index)
      if (projections[index].kind != PlaceProjectionKind::Field ||
          projections[index].field != fields[index])
        return std::nullopt;
    // Entry dependencies name caller-owned referents through their carrier's
    // field path. Concrete local dependencies already name the external owner.
    const auto offset = loan.entry ? 0 : fields.size();
    dependency.place.projections.insert(dependency.place.projections.end(),
                                        projections.begin() + offset,
                                        projections.end());
    return dependency;
  };
  if (auto primary = select({.place = loan.place,
                             .designatedFields = loan.designatedFields,
                             .access = loan.access,
                             .protectsStorage = loan.protectsStorage,
                             .conditions = loan.conditions}))
    return primary;
  for (const auto &dependency : loan.additionalDependencies)
    if (auto selected = select(dependency))
      return selected;
  return std::nullopt;
}

auto SemanticVisitor::Impl::retainedLoanSource(const ExprPtr &expression) const
    -> std::optional<RetainedLoanSource> {
  if (!expression) {
    return std::nullopt;
  }
  if (const auto complete = completeBorrowDependencySet(expression);
      complete && complete->possibleAlternatives.empty() &&
      !complete->dependencies.empty() &&
      std::all_of(complete->dependencies.begin(), complete->dependencies.end(),
                  [](const auto &dependency) {
                    return dependency.designatedFields.empty() &&
                           dependency.conditions.empty();
                  })) {
    if (complete->dependencies.size() != 1 ||
        complete->dependencies.front().parent == 0)
      return std::nullopt;
    const auto &dependency = complete->dependencies.front();
    return RetainedLoanSource{.loan = dependency.parent,
                              .dependency = dependency};
  }
  const auto mappedDependency = [&](const SemanticLoanInfo &loan,
                                    const ExprPtr &origin,
                                    const std::vector<SymbolId> &sourceFields)
      -> std::optional<SemanticLoanDependency> {
    std::vector<PlaceProjection> projections;
    if (const auto *place = semanticModel.findPlace(*origin);
        !loan.entry && place != nullptr &&
        std::find(loan.carriers.begin(), loan.carriers.end(), place->root) !=
            loan.carriers.end() &&
        std::all_of(place->projections.begin(), place->projections.end(),
                    [](const auto &projection) {
                      return projection.kind == PlaceProjectionKind::Field;
                    }))
      projections = place->projections;
    for (const auto field : sourceFields)
      projections.push_back(
          {.kind = PlaceProjectionKind::Field, .field = field});
    if (auto selected = projectedStoredDependency(loan, projections))
      return selected;
    if (loan.kind != SemanticLoanKind::StoredValue ||
        !loan.designatedFields.empty() || !loan.additionalDependencies.empty())
      return std::nullopt;
    // The compact single-origin form omits a designated field, but the
    // concrete receiver type still identifies its sole stored-reference path.
    // Preserve any suffix beneath that reference as a distinct child place;
    // aliasing the whole-owner loan would lose a projected call's identity.
    const auto *recordedType = semanticModel.findType(*origin);
    if (recordedType == nullptr)
      return std::nullopt;
    auto type = *recordedType;
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      type = SemanticType(type.arguments.front());
    const auto paths = storedReferencePaths(type);
    if (paths.size() != 1)
      return std::nullopt;
    std::vector<SymbolId> fields;
    for (const auto &field : paths.front().fields) {
      const auto symbol = symbolForDeclaration(field.name());
      if (symbol == 0)
        return std::nullopt;
      fields.push_back(symbol);
    }
    if (sourceFields.size() < fields.size() ||
        !std::equal(fields.begin(), fields.end(), sourceFields.begin()))
      return std::nullopt;
    SemanticLoanDependency dependency{.place = loan.place,
                                      .access = loan.access,
                                      .protectsStorage = loan.protectsStorage};
    for (std::size_t index = fields.size(); index < sourceFields.size();
         ++index)
      dependency.place.projections.push_back(
          {.kind = PlaceProjectionKind::Field, .field = sourceFields[index]});
    return dependency;
  };
  const auto selectedDependency =
      [&](std::optional<RetainedLoanSource> source) {
        const auto *loan =
            source ? semanticModel.findLoan(source->loan) : nullptr;
        const auto *place = semanticModel.findPlace(*expression);
        if (loan != nullptr && place != nullptr) {
          if (auto dependency =
                  projectedStoredDependency(*loan, place->projections)) {
            source->dependency = std::move(dependency);
            return source;
          }
        }
        if (loan == nullptr || !loan->entry ||
            loan->kind != SemanticLoanKind::StoredValue ||
            loan->designatedFields.empty() || place == nullptr ||
            place->projections.empty())
          return source;
        const auto overlaps = [&](const std::vector<SymbolId> &fields) {
          for (std::size_t index = 0;
               index < std::min(fields.size(), place->projections.size());
               ++index) {
            if (place->projections[index].kind != PlaceProjectionKind::Field)
              return true;
            if (place->projections[index].field != fields[index])
              return false;
          }
          return true;
        };
        if (overlaps(loan->designatedFields) ||
            std::any_of(loan->additionalDependencies.begin(),
                        loan->additionalDependencies.end(),
                        [&](const auto &dependency) {
                          return overlaps(dependency.designatedFields);
                        }))
          return source;
        // An abstract receiver dependency protects its reference fields, not an
        // unrelated owned field. Borrowing that field starts a separate loan.
        return std::optional<RetainedLoanSource>{};
      };
  if (exprAs<This>(expression.get()) != nullptr) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      const auto found = scope->find(std::string(receiverStateName));
      if (found != scope->end() && found->second.retainedLoan != 0)
        return RetainedLoanSource{.loan = found->second.retainedLoan};
    }
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    const Symbol *symbol = resolvedBorrowVariable(*variable);
    if (symbol != nullptr && symbol->retainedLoan != 0) {
      return RetainedLoanSource{.loan = symbol->retainedLoan,
                                .carrier = variable};
    }
    if (symbol != nullptr &&
        symbol->bindingKind == SemanticBindingKind::Field &&
        !symbol->staticMember) {
      for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        const auto found = scope->find(std::string(receiverStateName));
        if (found != scope->end() && found->second.retainedLoan != 0)
          return selectedDependency(
              RetainedLoanSource{.loan = found->second.retainedLoan});
      }
    }
    return std::nullopt;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return retainedLoanSource(grouping->expression());
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return retainedLoanSource(binary->right());
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    return selectedDependency(retainedLoanSource(member->object()));
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::STAR &&
      unary->rangeObject()) {
    auto source = retainedLoanSource(unary->right());
    const auto *loan = source ? semanticModel.findLoan(source->loan) : nullptr;
    const auto *iterator = source && source->carrier != nullptr
                               ? resolve(source->carrier->name())
                               : nullptr;
    if (loan != nullptr && loan->kind == SemanticLoanKind::Reference &&
        loan->storedDependency != 0 && iterator != nullptr &&
        iterator->variableDeclaration != nullptr &&
        iterator->variableDeclaration->isRangeIterator()) {
      // The synthesized cursor retains the exact range-storage loan. Its
      // separate stored dependency may protect a resource used by that range;
      // following it would substitute the resource for the element's owner.
      return source;
    }
  }
  if (const auto *resolved = semanticModel.findOperator(*expression);
      resolved != nullptr && !resolved->borrowFields.empty()) {
    const ExprPtr *origin = operatorBorrowSource(expression, *resolved);
    auto source = origin == nullptr ? std::optional<RetainedLoanSource>{}
                                    : retainedLoanSource(*origin);
    const auto *loan = source ? semanticModel.findLoan(source->loan) : nullptr;
    if (loan != nullptr && loan->kind == SemanticLoanKind::Reference &&
        loan->storedDependency != 0) {
      // A mapped result follows a stored external reference. An alias's
      // separate shell loan protects the alias object, not this referent.
      if (const auto *dependency =
              semanticModel.findLoan(loan->storedDependency)) {
        source->loan = dependency->id;
        source->dependency.reset();
        loan = dependency;
      }
    }
    if (loan != nullptr && resolved->borrowFields.size() == 1 &&
        resolved->returnType.kind == SemanticType::Reference) {
      source->dependency = mappedDependency(
          *loan, *origin, resolved->borrowFields.front().sourceFields);
    }
    return source;
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    return retainedLoanSource(index->object());
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::STAR) {
    if (unary->rangeObject()) {
      const SemanticType *rangeType =
          semanticModel.findType(*unary->rangeObject());
      const std::optional<RetainedLoanSource> rangeSource =
          retainedLoanSource(unary->rangeObject());
      const SemanticLoanInfo *rangeLoan =
          rangeSource ? semanticModel.findLoan(rangeSource->loan) : nullptr;
      if ((rangeType != nullptr && rangeType->kind == SemanticType::Array) ||
          (rangeLoan != nullptr && rangeLoan->access == AccessMode::Mutable)) {
        return rangeSource;
      }
    }
    return retainedLoanSource(unary->right());
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::QUESTION) {
    if (const auto *plan = semanticModel.findTryablePropagation(*unary);
        plan != nullptr &&
        plan->output.returnType.kind == SemanticType::Reference &&
        plan->output.borrowOrigin == BorrowOriginKind::Argument &&
        plan->output.borrowArgument == 0)
      return retainedLoanSource(unary->right());
  }
  if (const ResolvedOperatorInfo *resolved =
          semanticModel.findOperator(*expression)) {
    const ExprPtr *source = operatorBorrowSource(expression, *resolved);
    return source == nullptr ? std::nullopt : retainedLoanSource(*source);
  }
  const ResolvedConstructionInfo *construction =
      semanticModel.findConstruction(*expression);
  if (construction != nullptr &&
      construction->borrowOrigin == BorrowOriginKind::Argument) {
    if (const auto *call = exprAs<Call>(expression.get())) {
      return construction->borrowArgument < call->arguments().size()
                 ? retainedLoanSource(
                       call->arguments()[construction->borrowArgument])
                 : std::nullopt;
    }
    if (const auto *initializer = exprAs<DirectInitializer>(expression.get())) {
      return construction->borrowArgument < initializer->arguments().size()
                 ? retainedLoanSource(
                       initializer->arguments()[construction->borrowArgument])
                 : std::nullopt;
    }
  }
  const auto *call = exprAs<Call>(expression.get());
  if (call == nullptr) {
    return std::nullopt;
  }
  if (resolvedIntrinsicKind(*call) == IntrinsicKind::Move) {
    if (call->arguments().size() != 1) {
      return std::nullopt;
    }
    std::optional<RetainedLoanSource> source =
        retainedLoanSource(call->arguments().front());
    if (source) {
      source->transfer = true;
    }
    return source;
  }
  const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
  if (resolved == nullptr) {
    return std::nullopt;
  }
  if (resolved->borrowFields.size() == 1 &&
      (resolved->returnType.kind == SemanticType::Reference ||
       (resolved->returnType.kind == SemanticType::Class &&
        resolved->intrinsic == IntrinsicKind::None &&
        !resolved->borrowFields.front().sourceFields.empty() &&
        storedReferencePath(resolved->returnType).has_value()))) {
    const auto &mapping = resolved->borrowFields.front();
    const ExprPtr *origin = nullptr;
    if (mapping.origin == BorrowOriginKind::Argument &&
        mapping.parameter < resolvedCallArgumentCount(*call, resolved))
      origin = resolvedCallArgument(*call, resolved, mapping.parameter);
    else if (mapping.origin == BorrowOriginKind::Receiver)
      if (const auto *member = exprAs<Get>(call->callee().get()))
        origin = &member->object();
    if (origin != nullptr) {
      auto source = retainedLoanSource(*origin);
      const auto *loan =
          source ? semanticModel.findLoan(source->loan) : nullptr;
      if (loan != nullptr && loan->kind == SemanticLoanKind::Reference &&
          loan->storedDependency != 0) {
        if (const auto *dependency =
                semanticModel.findLoan(loan->storedDependency)) {
          source->loan = dependency->id;
          source->dependency.reset();
          loan = dependency;
        }
      }
      if (loan != nullptr) {
        source->dependency =
            mappedDependency(*loan, *origin, mapping.sourceFields);
        return source;
      }
    }
  }
  if (resolved->borrowOrigin == BorrowOriginKind::Argument &&
      resolved->borrowArgument < resolvedCallArgumentCount(*call, resolved)) {
    return retainedLoanSource(
        *resolvedCallArgument(*call, resolved, resolved->borrowArgument));
  }
  if (resolved->borrowOrigin == BorrowOriginKind::Receiver) {
    const auto *member = exprAs<Get>(call->callee().get());
    if (member != nullptr) {
      return retainedLoanSource(member->object());
    }
    if (currentClass) {
      // An implicit receiver has the same stored-owner authority as `this`.
      // Omitting that authority would create an unrelated local reference
      // while lowering correctly retained the receiver's dependency.
      for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        const auto receiver = scope->find(std::string(receiverStateName));
        if (receiver != scope->end() && receiver->second.retainedLoan != 0)
          return RetainedLoanSource{.loan = receiver->second.retainedLoan};
      }
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::appendUniqueLoan(std::vector<SemanticLoanId> &loans,
                                             SemanticLoanId loan) -> void {
  if (loan != 0 && std::find(loans.begin(), loans.end(), loan) == loans.end()) {
    loans.push_back(loan);
  }
}

auto SemanticVisitor::Impl::selectedReferenceDependencies(
    const SemanticPlace &place) const
    -> std::optional<std::vector<SemanticLoanDependency>> {
  if (place.root == nullptr ||
      place.root->type.kind != SemanticType::Reference ||
      place.root->retainedLoan == 0)
    return std::nullopt;
  const auto *loan = semanticModel.findLoan(place.root->retainedLoan);
  if (loan == nullptr || loan->kind != SemanticLoanKind::Reference ||
      !loan->referenceSelection)
    return std::nullopt;
  std::vector<SemanticLoanDependency> result;
  const auto append = [&](SemanticLoanDependency dependency) {
    // The selected alternative was proved when the scalar reference was
    // published. Its external origins remain unconditional obligations of
    // this access; the immutable selection retains the original tag/path.
    dependency.place.projections.insert(dependency.place.projections.end(),
                                        place.projections.begin(),
                                        place.projections.end());
    dependency.parent = loan->id;
    if (std::find(result.begin(), result.end(), dependency) == result.end())
      result.push_back(std::move(dependency));
  };
  append({.place = loan->place,
          .designatedFields = loan->designatedFields,
          .access = loan->access,
          .protectsStorage = loan->protectsStorage,
          .conditions = loan->conditions});
  for (auto dependency : loan->additionalDependencies) {
    // A shared reborrow may narrow an exclusive selected capability. It
    // cannot retain exclusive access through a secondary origin.
    if (loan->access == AccessMode::ReadOnly)
      dependency.access = AccessMode::ReadOnly;
    append(std::move(dependency));
  }
  return result;
}

auto SemanticVisitor::Impl::hasCheckedExternalBorrowOrigin(
    const SemanticLoanDependency &dependency) -> bool {
  if (!dependency.place.valid() || dependency.parent == 0 ||
      semanticModel.findForeignStorage(dependency.place.root) != nullptr)
    return false;
  std::unordered_set<SemanticLoanId> visiting;
  std::unordered_set<SemanticLoanId> checked;
  std::vector<std::pair<SemanticLoanId, bool>> pending{
      {dependency.parent, false}};
  const auto access = dependency.access;
  while (!pending.empty()) {
    const auto [id, leaving] = pending.back();
    pending.pop_back();
    if (leaving) {
      visiting.erase(id);
      checked.insert(id);
      continue;
    }
    if (checked.contains(id))
      continue;
    if (id == 0 || !visiting.insert(id).second)
      return false;
    const auto *loan = semanticModel.findLoan(id);
    const auto flow = loanFlow.records.find(id);
    if (!loan || flow == loanFlow.records.end() || !flow->second.active)
      return false;
    if (loan->entry && loan->kind == SemanticLoanKind::StoredValue) {
      // Resolve the entry against this exact body's formal/receiver, not a
      // similarly named binding from another demanded generic instance.
      const auto *function = currentFunctionInfo();
      const auto *syntax = function ? function->bodyDeclaration() : nullptr;
      if (!syntax || loan->parent != 0 || loan->storedDependency != 0 ||
          !loan->place.projections.empty())
        return false;
      SemanticType carrier = SemanticType::Unknown;
      if (loan->place.receiver) {
        if (loan->place.root != 0 || !function->ownerClass ||
            function->staticMember || loan->entryDeclaration != syntax)
          return false;
        carrier = openClassType(function->ownerClass);
      } else {
        if (function->parameterTypes.size() != syntax->parameters().size())
          return false;
        for (std::size_t index = 0; index < syntax->parameters().size();
             ++index)
          if (const auto *binding =
                  semanticModel.findBinding(syntax->parameters()[index]);
              binding && binding->symbol == loan->place.root) {
            if (carrier != SemanticType::Unknown)
              return false;
            carrier = function->parameterTypes[index];
          }
        if (carrier.kind == SemanticType::Reference &&
            carrier.arguments.size() == 1)
          carrier = SemanticType(carrier.arguments.front());
      }
      const auto inventory = storedReferenceInventory(carrier);
      if (!inventory)
        return false;
      const auto matches = [&](const SemanticLoanDependency &entry) {
        if (entry.place != loan->place || entry.parent != 0 ||
            entry.designatedFields.empty() ||
            (access == AccessMode::Mutable &&
             entry.access != AccessMode::Mutable))
          return false;
        auto external = entry.place;
        for (const auto field : entry.designatedFields)
          external.projections.push_back(
              {.kind = PlaceProjectionKind::Field, .field = field});
        if (external != dependency.place)
          return false;
        for (const auto &path : *inventory) {
          const auto fields = storedFieldSymbols(carrier, path.fields);
          const auto guards = storedConditionSymbols(carrier, path.conditions);
          if (fields == entry.designatedFields && guards &&
              *guards == entry.conditions &&
              path.reference.kind == SemanticType::Reference &&
              path.reference.arguments.size() == 1 &&
              path.reference.referenceAccess == entry.access)
            return true;
        }
        return false;
      };
      if (!matches({.place = loan->place,
                    .designatedFields = loan->designatedFields,
                    .access = loan->access,
                    .conditions = loan->conditions}) &&
          !std::any_of(loan->additionalDependencies.begin(),
                       loan->additionalDependencies.end(), matches))
        return false;
      visiting.erase(id);
      checked.insert(id);
      continue;
    }
    pending.emplace_back(id, true);
    bool found = false;
    const auto follows = [&](const SemanticLoanDependency &edge) {
      if (edge.place != dependency.place)
        return true;
      found = true;
      if (access == AccessMode::Mutable && edge.access != AccessMode::Mutable)
        return false;
      // A checked local reference may eventually reach ordinary protected
      // storage. Otherwise follow its exact matching ancestry; unrelated
      // additional owners cannot justify this origin.
      if (!edge.protectsStorage)
        pending.emplace_back(edge.parent, false);
      return true;
    };
    if (!follows({.place = loan->place,
                  .access = loan->access,
                  .protectsStorage = loan->protectsStorage,
                  .parent = loan->parent}))
      return false;
    for (const auto &edge : loan->additionalDependencies)
      if (!follows(edge))
        return false;
    if (!found)
      return false;
  }
  return true;
}

auto SemanticVisitor::Impl::loanPlaces(const SemanticPlace &place)
    -> std::vector<SemanticLoanPlace> {
  std::vector<SemanticLoanPlace> result;
  if (const auto dependencies = selectedReferenceDependencies(place)) {
    for (const auto &dependency : *dependencies)
      appendUniquePlace(result, dependency.place);
  } else {
    appendUniquePlace(result, loanPlace(place));
  }
  return result;
}

auto SemanticVisitor::Impl::loanPlaces(const ExprPtr &expression,
                                       bool transient)
    -> std::vector<SemanticLoanPlace> {
  const auto place = semanticPlace(expression);
  if (selectedReferenceDependencies(place))
    return loanPlaces(place);
  std::vector<SemanticLoanPlace> result;
  appendUniquePlace(result, transient ? transientLoanPlace(expression)
                                      : loanPlace(expression));
  return result;
}

auto SemanticVisitor::Impl::loanPlace(SemanticPlace place)
    -> SemanticLoanPlace {
  if (place.root == nullptr) {
    return {};
  }

  SemanticLoanPlace result;
  if (place.root->retainedLoan != 0)
    if (const auto *loan = semanticModel.findLoan(place.root->retainedLoan))
      if (const auto dependency =
              projectedStoredDependency(*loan, place.projections))
        return dependency->place;
  const bool accessesBorrowedReferent =
      place.root->type.kind == SemanticType::Reference ||
      (!place.projections.empty() &&
       place.projections.front().kind == PlaceProjectionKind::Dereference);
  if (accessesBorrowedReferent && place.root->retainedLoan != 0) {
    if (const SemanticLoanInfo *loan =
            semanticModel.findLoan(place.root->retainedLoan)) {
      result = loan->place;
    }
  }
  if (result.root == 0 && !result.receiver) {
    result.root = place.receiver ? 0 : place.rootIdentity;
    if (result.root == 0 && !place.receiver) {
      result.root = toolingSymbolFor(*place.root);
    }
    result.receiver = place.receiver;
  }
  for (const PlaceProjection &projection : place.projections) {
    result.projections.push_back(projection);
  }
  return result;
}

auto SemanticVisitor::Impl::loanPlace(const BorrowOriginPlace &place) const
    -> SemanticLoanPlace {
  // Loan-flow places share one analysis-local domain, including globals.
  // HIR qualifies all of them with the concrete snapshot/body domain. Giving
  // only globals a snapshot here makes disjoint local carriers MayAlias.
  return {.root = place.root, .projections = place.projections};
}

auto SemanticVisitor::Impl::loanPlace(const ExprPtr &expression)
    -> SemanticLoanPlace {
  SemanticPlace place = semanticPlace(expression);
  if (place.root != nullptr) {
    return loanPlace(std::move(place));
  }
  if (const auto *call = exprAs<Call>(expression.get())) {
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    if (resolved != nullptr &&
        isBorrowedCarrierReference(resolved->returnType)) {
      if (resolved->borrowOrigin == BorrowOriginKind::Receiver) {
        if (const auto *member = exprAs<Get>(call->callee().get()))
          return loanPlace(member->object());
        if (currentClass)
          return {.receiver = true};
      } else if (resolved->borrowOrigin == BorrowOriginKind::Argument &&
                 resolved->borrowArgument <
                     resolvedCallArgumentCount(*call, resolved)) {
        return loanPlace(
            *resolvedCallArgument(*call, resolved, resolved->borrowArgument));
      }
    }
    if (resolved != nullptr &&
        resolved->borrowOrigin == BorrowOriginKind::Global &&
        resolved->borrowPlace) {
      return loanPlace(*resolved->borrowPlace);
    }
  }
  if (const Variable *owner = borrowedOwnerVariable(expression)) {
    if (Symbol *symbol = resolveMutable(owner->name())) {
      return loanPlace(placeForSymbol(*symbol));
    }
  }
  if (currentClass && isReceiverDerivedBorrow(expression)) {
    return {.receiver = true};
  }
  return {};
}

auto SemanticVisitor::Impl::loanPlacesOverlap(const SemanticLoanPlace &left,
                                              const SemanticLoanPlace &right)
    -> bool {
  return placesMayOverlap(left, right);
}

auto SemanticVisitor::Impl::loanIsAncestor(SemanticLoanId ancestor,
                                           SemanticLoanId descendant) const
    -> bool {
  if (ancestor == 0 || descendant == 0) {
    return false;
  }
  std::vector<SemanticLoanId> pending{descendant};
  std::unordered_set<SemanticLoanId> visited;
  while (!pending.empty()) {
    const auto current = pending.back();
    pending.pop_back();
    if (current == 0 || !visited.insert(current).second)
      continue;
    if (current == ancestor) {
      return true;
    }
    const SemanticLoanInfo *loan = semanticModel.findLoan(current);
    if (loan != nullptr) {
      pending.push_back(loan->parent);
      for (const auto &dependency : loan->additionalDependencies)
        pending.push_back(dependency.parent);
    }
  }
  return false;
}

auto SemanticVisitor::Impl::accessLoan(const SemanticPlace &place) const
    -> SemanticLoanId {
  if (place.root == nullptr || place.root->retainedLoan == 0) {
    return 0;
  }
  if (const auto *loan = semanticModel.findLoan(place.root->retainedLoan))
    if (projectedStoredDependency(*loan, place.projections))
      return loan->id;
  return place.root->type.kind == SemanticType::Reference ||
                 (!place.projections.empty() &&
                  place.projections.front().kind ==
                      PlaceProjectionKind::Dereference)
             ? place.root->retainedLoan
             : 0;
}

auto SemanticVisitor::Impl::accessLoan(const ExprPtr &expression) const
    -> SemanticLoanId {
  if (const std::optional<RetainedLoanSource> source =
          retainedLoanSource(expression)) {
    return source->loan;
  }
  return 0;
}

auto SemanticVisitor::Impl::conflictingLoans(const SemanticLoanPlace &place,
                                             AccessMode requested,
                                             SemanticLoanId throughLoan) const
    -> std::vector<SemanticLoanId> {
  std::vector<SemanticLoanId> result;
  const auto *accessParent = semanticModel.findLoan(throughLoan);
  const bool carrierAccess =
      accessParent != nullptr && !place.receiver && place.projections.empty() &&
      std::find(accessParent->carriers.begin(), accessParent->carriers.end(),
                place.root) != accessParent->carriers.end();
  const auto *parent =
      carrierAccess && accessParent->kind == SemanticLoanKind::Reference &&
              accessParent->storedDependency != 0
          ? semanticModel.findLoan(accessParent->storedDependency)
          : accessParent;
  // A call through the exact named carrier uses its external capability as
  // well as its physical shell. An exclusive parent stays suspended while a
  // child is live even when the child's source is the external referent.
  for (const auto &[id, flow] : loanFlow.records) {
    if (!flow.active) {
      continue;
    }
    const SemanticLoanInfo *loan = semanticModel.findLoan(id);
    if (loan == nullptr || loanIsAncestor(id, throughLoan) ||
        (carrierAccess && parent != nullptr &&
         loanIsAncestor(id, parent->id))) {
      continue;
    }
    const auto parentIsExclusive = [&] {
      if (parent == nullptr)
        return false;
      if (carrierAccess)
        return parent->access == AccessMode::Mutable ||
               std::any_of(parent->additionalDependencies.begin(),
                           parent->additionalDependencies.end(),
                           [](const auto &dependency) {
                             return dependency.access == AccessMode::Mutable;
                           });
      if (parent->entry && parent->kind == SemanticLoanKind::StoredValue &&
          !parent->designatedFields.empty())
        if (const auto dependency =
                projectedStoredDependency(*parent, place.projections))
          return dependency->access == AccessMode::Mutable;
      return (parent->access == AccessMode::Mutable &&
              loanPlacesOverlap(parent->place, place)) ||
             std::any_of(parent->additionalDependencies.begin(),
                         parent->additionalDependencies.end(),
                         [&](const auto &dependency) {
                           return dependency.access == AccessMode::Mutable &&
                                  loanPlacesOverlap(dependency.place, place);
                         });
    };
    const bool suspendedParentUse = parent != nullptr &&
                                    loanIsAncestor(parent->id, id) &&
                                    parentIsExclusive();
    // A plain reference's exclusive permission is suspended by shared
    // children, not its ability to lend/read shared access. A reference to a
    // borrowed carrier has a separate dependency contract: reading that shell
    // can invoke operations on its stored exclusive referents, so it does not
    // acquire this permission merely from the receiver's spelling.
    const bool sharedReferenceAccess =
        requested == AccessMode::ReadOnly && accessParent != nullptr &&
        accessParent->kind == SemanticLoanKind::Reference &&
        accessParent->storedDependency == 0;
    // Permission belongs to the overlapping dependency, not to whichever
    // reference happened to be the carrier's first field. In particular, a
    // shared first field cannot hide an exclusive later field, and an
    // exclusive first field cannot prohibit reads of unrelated shared owners.
    const auto conflicts = [&](const SemanticLoanPlace &source,
                               AccessMode access, bool protectsStorage) {
      const bool overlaps =
          loanPlacesOverlap(place, source) ||
          (carrierAccess && suspendedParentUse &&
           (loanPlacesOverlap(parent->place, source) ||
            std::any_of(parent->additionalDependencies.begin(),
                        parent->additionalDependencies.end(),
                        [&](const auto &dependency) {
                          return dependency.access == AccessMode::Mutable &&
                                 loanPlacesOverlap(dependency.place, source);
                        })));
      // Abstract entry dependencies do not protect the enum shell's bytes,
      // but a selected child still suspends its checked parent's exclusive
      // authority over the external referent. This is permission conflict,
      // not a new claim that the referent lives inside the carrier.
      const bool publishedExternalExclusion = [&] {
        if (loan->entry || loan->kind != SemanticLoanKind::StoredValue ||
            !loan->lifecycleBound || loan->access != AccessMode::Mutable ||
            access != AccessMode::Mutable || protectsStorage ||
            !loan->designatedFields.empty() || !loan->conditions.empty() ||
            !loan->additionalDependencies.empty())
          return false;
        const auto *lifetimeParent = semanticModel.findLoan(loan->parent);
        if (lifetimeParent == nullptr || !lifetimeParent->entry ||
            lifetimeParent->kind != SemanticLoanKind::StoredValue ||
            lifetimeParent->access != AccessMode::ReadOnly ||
            lifetimeParent->protectsStorage)
          return false;
        const auto selected =
            projectedStoredDependency(*lifetimeParent, source.projections);
        return selected && selected->access == AccessMode::ReadOnly &&
               selected->conditions.empty() && selected->place == source;
      }();
      // A checked result can retain exclusive external permission while its
      // shared entry parent supplies only an address and lifetime. That does
      // not make the external referent part of the carrier's physical bytes.
      const bool protectsPermission =
          (loan->referenceSelection && suspendedParentUse) ||
          publishedExternalExclusion;
      return (protectsStorage || protectsPermission) && overlaps &&
             ((suspendedParentUse && !sharedReferenceAccess) ||
              requested == AccessMode::Mutable ||
              access == AccessMode::Mutable);
    };
    if (conflicts(loan->place, loan->access, loan->protectsStorage) ||
        std::any_of(loan->additionalDependencies.begin(),
                    loan->additionalDependencies.end(),
                    [&](const SemanticLoanDependency &dependency) {
                      return conflicts(dependency.place, dependency.access,
                                       dependency.protectsStorage);
                    })) {
      result.push_back(id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

auto SemanticVisitor::Impl::reportLoanPlaceConflict(
    const Token &token, const SemanticLoanPlace &stable, AccessMode requested,
    std::string message, SemanticLoanId throughLoan) -> void {
  reportLoanPlaceConflicts(token, std::span(&stable, 1), requested,
                           std::move(message), throughLoan);
}

auto SemanticVisitor::Impl::reportLoanPlaceConflicts(
    const Token &token, std::span<const SemanticLoanPlace> places,
    AccessMode requested, std::string message, SemanticLoanId throughLoan)
    -> void {
  std::vector<SemanticLoanId> conflicts;
  for (const auto &place : places) {
    if (place.root == 0 && !place.receiver)
      continue;
    for (const auto conflict : conflictingLoans(place, requested, throughLoan))
      appendUniqueLoan(conflicts, conflict);
  }
  if (throughLoan != 0 && std::any_of(conflicts.begin(), conflicts.end(),
                                      [&](SemanticLoanId candidate) {
                                        return loanIsAncestor(throughLoan,
                                                              candidate);
                                      })) {
    message = "Cannot use a parent loan while an overlapping child "
              "reborrow may still be live.";
  }
  reportBorrowConflict(token, conflicts, std::move(message));
}

auto SemanticVisitor::Impl::reportLoanPlaceConflict(
    const Token &token, const SemanticPlace &place, AccessMode requested,
    std::string message, SemanticLoanId throughLoan) -> void {
  if (throughLoan == 0) {
    throughLoan = accessLoan(place);
  }
  reportLoanPlaceConflicts(token, loanPlaces(place), requested,
                           std::move(message), throughLoan);
}

auto SemanticVisitor::Impl::validateLoanPlaceAccess(const Expr &expression,
                                                    OccurrenceRole roles)
    -> void {
  const ExprKind kind = expression.exprKind();
  if (suppressLoanPlaceAccessDepth != 0 ||
      std::find(suppressedProjectedPlaceReads.begin(),
                suppressedProjectedPlaceReads.end(),
                &expression) != suppressedProjectedPlaceReads.end() ||
      kind == ExprKind::Assign || kind == ExprKind::Set ||
      kind == ExprKind::IndexSet || kind == ExprKind::DereferenceSet ||
      kind == ExprKind::PlaceSet ||
      (analyzingCallCallee && kind == ExprKind::Get)) {
    return;
  }
  SemanticPlace place = semanticPlace(&expression);
  if (place.root == nullptr) {
    return;
  }
  const AccessMode requested = hasRole(roles, OccurrenceRole::Write)
                                   ? AccessMode::Mutable
                                   : AccessMode::ReadOnly;
  reportLoanPlaceConflict(
      expressionToken(expression), place, requested,
      requested == AccessMode::Mutable
          ? "Cannot mutate storage while an overlapping borrow may still "
            "be live."
          : "Cannot read storage while an overlapping mutable borrow or "
            "child reborrow may still be live.");
}

auto SemanticVisitor::Impl::validateMutableCallArgumentLoans(
    std::span<const SemanticType> parameterTypes,
    std::span<const ExprPtr> arguments, const Token &location) -> void {
  const std::size_t count = std::min(parameterTypes.size(), arguments.size());
  for (std::size_t index = 0; index < count; ++index) {
    const SemanticType &parameter = parameterTypes[index];
    if (parameter.kind != SemanticType::Reference ||
        parameter.referenceAccess != AccessMode::Mutable) {
      continue;
    }
    const auto origins = loanPlaces(arguments[index], true);
    invalidateBorrowDependencySet(semanticPlace(arguments[index]));
    for (const auto &origin : origins)
      recordMutableBindingUse(origin.root);
    if (origins.empty()) {
      continue;
    }
    const SemanticLoanId throughLoan = accessLoan(arguments[index]);
    if (suppressLoanPlaceAccessDepth == 0 &&
        std::any_of(origins.begin(), origins.end(), [&](const auto &origin) {
          return !conflictingLoans(origin, AccessMode::ReadOnly, throughLoan)
                      .empty();
        })) {
      // Ordinary argument analysis already diagnosed this suspended-parent
      // or mutable-loan read. Retain the mutable boundary check only when it
      // adds a conflict that a read-only expression access cannot see.
      continue;
    }
    reportLoanPlaceConflicts(
        location, origins, AccessMode::Mutable,
        "Mutable reference argument overlaps a borrow that may still be "
        "live.",
        throughLoan);
  }
}

auto SemanticVisitor::Impl::transientBorrowProduction(const ExprPtr &expression)
    -> std::optional<TransientBorrowProduction> {
  if (!expression) {
    return std::nullopt;
  }
  if (const auto complete = completeBorrowDependencySet(expression);
      complete && complete->possibleAlternatives.empty() &&
      !complete->dependencies.empty() &&
      std::all_of(complete->dependencies.begin(), complete->dependencies.end(),
                  [](const auto &dependency) {
                    return dependency.designatedFields.empty() &&
                           dependency.conditions.empty();
                  })) {
    if (complete->dependencies.size() != 1)
      return std::nullopt;
    const auto &dependency = complete->dependencies.front();
    return TransientBorrowProduction{.place = dependency.place,
                                     .access = dependency.access};
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return transientBorrowProduction(grouping->expression());
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return transientBorrowProduction(binary->right());
  }

  const auto fromSource =
      [&](const ExprPtr &source, AccessMode resultAccess, bool producerMutable,
          bool valueCarrier) -> std::optional<TransientBorrowProduction> {
    std::optional<TransientBorrowProduction> nested =
        transientBorrowProduction(source);
    SemanticLoanPlace place =
        nested ? std::move(nested->place) : loanPlace(source);
    if (place.root == 0 && !place.receiver) {
      return std::nullopt;
    }
    const SemanticPlace sourcePlace = semanticPlace(source);
    const bool globalStorage =
        (nested && nested->globalStorage) ||
        (sourcePlace.root != nullptr &&
         (sourcePlace.root->bindingKind ==
              SemanticBindingKind::GlobalVariable ||
          sourcePlace.root->bindingKind == SemanticBindingKind::StaticField ||
          (sourcePlace.root->variableDeclaration != nullptr &&
           sourcePlace.root->variableDeclaration->isThreadLocal())));
    return TransientBorrowProduction{
        .place = std::move(place),
        .access = producerMutable || resultAccess == AccessMode::Mutable ||
                          (nested && nested->access == AccessMode::Mutable)
                      ? AccessMode::Mutable
                      : AccessMode::ReadOnly,
        .globalStorage = globalStorage,
        .retainsValueCarrier =
            valueCarrier || (nested && nested->retainsValueCarrier)};
  };

  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::QUESTION) {
    if (const auto *plan = semanticModel.findTryablePropagation(*unary);
        plan != nullptr &&
        plan->output.returnType.kind == SemanticType::Reference &&
        plan->output.borrowOrigin == BorrowOriginKind::Argument &&
        plan->output.borrowArgument == 0)
      return fromSource(unary->right(), plan->output.borrowAccess, false,
                        false);
  }

  if (const ResolvedOperatorInfo *resolved =
          semanticModel.findOperator(*expression);
      resolved != nullptr && resolved->borrowOrigin != BorrowOriginKind::None) {
    const ExprPtr *source = operatorBorrowSource(expression, *resolved);
    if (source == nullptr) {
      return std::nullopt;
    }
    const bool mutableProducer =
        (resolved->borrowOrigin == BorrowOriginKind::Receiver &&
         resolved->declaration != nullptr &&
         resolved->declaration->receiverMutability() ==
             ReceiverMutability::Mutable) ||
        (resolved->borrowOrigin == BorrowOriginKind::Argument &&
         resolved->borrowArgument < resolved->parameterTypes.size() &&
         resolved->parameterTypes[resolved->borrowArgument].kind ==
             SemanticType::Reference &&
         resolved->parameterTypes[resolved->borrowArgument].referenceAccess ==
             AccessMode::Mutable);
    return fromSource(*source, resolved->borrowAccess, mutableProducer,
                      resolved->returnType.kind != SemanticType::Reference);
  }
  if (const ResolvedConstructionInfo *resolved =
          semanticModel.findConstruction(*expression);
      resolved != nullptr &&
      resolved->borrowOrigin == BorrowOriginKind::Argument) {
    const ExprPtr *source = nullptr;
    if (const auto *call = exprAs<Call>(expression.get());
        call != nullptr &&
        resolved->borrowArgument < call->arguments().size()) {
      source = &call->arguments()[resolved->borrowArgument];
    } else if (const auto *initializer =
                   exprAs<DirectInitializer>(expression.get());
               initializer != nullptr &&
               resolved->borrowArgument < initializer->arguments().size()) {
      source = &initializer->arguments()[resolved->borrowArgument];
    }
    if (source == nullptr) {
      return std::nullopt;
    }
    const bool mutableProducer =
        resolved->borrowArgument < resolved->parameterTypes.size() &&
        resolved->parameterTypes[resolved->borrowArgument].kind ==
            SemanticType::Reference &&
        resolved->parameterTypes[resolved->borrowArgument].referenceAccess ==
            AccessMode::Mutable;
    return fromSource(*source, resolved->borrowAccess, mutableProducer, true);
  }
  if (const auto *call = exprAs<Call>(expression.get())) {
    if (const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
        resolved != nullptr &&
        resolved->borrowOrigin != BorrowOriginKind::None) {
      if (resolved->borrowOrigin == BorrowOriginKind::Argument &&
          resolved->borrowArgument <
              resolvedCallArgumentCount(*call, resolved)) {
        const bool mutableProducer =
            resolved->borrowArgument < resolved->parameterTypes.size() &&
            resolved->parameterTypes[resolved->borrowArgument].kind ==
                SemanticType::Reference &&
            resolved->parameterTypes[resolved->borrowArgument]
                    .referenceAccess == AccessMode::Mutable;
        return fromSource(
            *resolvedCallArgument(*call, resolved, resolved->borrowArgument),
            resolved->borrowAccess, mutableProducer,
            resolved->returnType.kind != SemanticType::Reference);
      }
      if (resolved->borrowOrigin == BorrowOriginKind::Receiver) {
        const bool mutableProducer =
            resolved->declaration != nullptr &&
            resolved->declaration->receiverMutability() ==
                ReceiverMutability::Mutable;
        if (const auto *member = exprAs<Get>(call->callee().get())) {
          return fromSource(
              member->object(), resolved->borrowAccess, mutableProducer,
              resolved->returnType.kind != SemanticType::Reference);
        }
        if (currentClass) {
          SemanticLoanPlace place = loanPlace(
              SemanticPlace{.root = receiverStateSymbol(), .receiver = true});
          if (place.root != 0 || place.receiver) {
            return TransientBorrowProduction{
                .place = std::move(place),
                .access = mutableProducer ||
                                  resolved->borrowAccess == AccessMode::Mutable
                              ? AccessMode::Mutable
                              : AccessMode::ReadOnly,
                .retainsValueCarrier =
                    resolved->returnType.kind != SemanticType::Reference};
          }
        }
      }
      if (resolved->borrowOrigin == BorrowOriginKind::Global &&
          resolved->borrowPlace) {
        return TransientBorrowProduction{
            .place = loanPlace(*resolved->borrowPlace),
            .access = resolved->borrowAccess,
            .globalStorage = true,
            .retainsValueCarrier =
                resolved->returnType.kind != SemanticType::Reference};
      }
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::transientLoanPlace(const ExprPtr &expression)
    -> SemanticLoanPlace {
  std::optional<TransientBorrowProduction> production =
      transientBorrowProduction(expression);
  return production ? std::move(production->place) : loanPlace(expression);
}

auto SemanticVisitor::Impl::retainedLoanPlace(const ExprPtr &expression)
    -> SemanticLoanPlace {
  // Exact field contracts compose across accessor calls. Reuse the same
  // dependency substitution as carrier-valued calls, including the suffix
  // beneath a stored external reference; the receiver shell is not the
  // returned reference's owner merely because a call hides its field syntax.
  if (expression) {
    const auto *call = exprAs<Call>(expression.get());
    const auto *resolved =
        call == nullptr ? nullptr : semanticModel.findCall(*call);
    const auto *operation = semanticModel.findOperator(*expression);
    const auto mappedExternalResult = [&](const auto &record) {
      return record.borrowFields.size() == 1 &&
             (record.returnType.kind == SemanticType::Reference ||
              (record.returnType.kind == SemanticType::Class &&
               !record.borrowFields.front().sourceFields.empty() &&
               storedReferencePath(record.returnType).has_value()));
    };
    const bool mappedResult =
        (resolved != nullptr && mappedExternalResult(*resolved) &&
         (resolved->returnType.kind == SemanticType::Reference ||
          (resolved->intrinsic == IntrinsicKind::None &&
           semanticModel.findConstruction(*expression) == nullptr))) ||
        (operation != nullptr && mappedExternalResult(*operation));
    if (mappedResult) {
      const auto dependencies = storedBorrowDependencies(expression);
      if (dependencies.size() == 1)
        return dependencies.front().place;
    }
  }
  SemanticLoanPlace direct = loanPlace(expression);
  std::optional<TransientBorrowProduction> production =
      transientBorrowProduction(expression);
  if (!production) {
    return direct;
  }
  if (direct.root == production->place.root &&
      direct.receiver == production->place.receiver &&
      direct.projections.size() > production->place.projections.size()) {
    // Operator syntax such as checked dereference contributes a stable
    // caller-visible projection beyond its receiver borrow origin.
    return direct;
  }
  return std::move(production->place);
}

auto SemanticVisitor::Impl::operatorReceiverExpression(
    const ExprPtr &expression) -> const ExprPtr * {
  if (const auto *call = exprAs<Call>(expression.get())) {
    return &call->callee();
  }
  if (const auto *binary = exprAs<Binary>(expression.get())) {
    return &binary->left();
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    return &member->object();
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    return &index->object();
  }
  if (const auto *postfix = exprAs<Postfix>(expression.get())) {
    return &postfix->expression();
  }
  if (const auto *unary = exprAs<Unary>(expression.get())) {
    return &unary->right();
  }
  return nullptr;
}

auto SemanticVisitor::Impl::appendUniquePlace(
    std::vector<SemanticLoanPlace> &places, SemanticLoanPlace place) -> void {
  if ((place.root != 0 || place.receiver) &&
      std::find(places.begin(), places.end(), place) == places.end()) {
    places.push_back(std::move(place));
  }
}

auto SemanticVisitor::Impl::appendLoanPlaces(
    std::vector<SemanticLoanPlace> &places, const SemanticPlace &place)
    -> void {
  for (auto origin : loanPlaces(place))
    appendUniquePlace(places, std::move(origin));
}

auto SemanticVisitor::Impl::appendLoanPlaces(
    std::vector<SemanticLoanPlace> &places, const ExprPtr &expression,
    bool transient) -> void {
  for (auto origin : loanPlaces(expression, transient))
    appendUniquePlace(places, std::move(origin));
}

auto SemanticVisitor::Impl::appendTransientMutationPlaces(
    const ExprPtr &expression, std::vector<SemanticLoanPlace> &places) -> void {
  if (!expression) {
    return;
  }

  if (const auto *assign = exprAs<Assign>(expression.get());
      assign != nullptr && assign->path().segments.size() == 1) {
    if (Symbol *target = resolveMutable(assign->name())) {
      appendLoanPlaces(places, placeForSymbol(*target));
    }
  } else if (const auto *set = exprAs<Set>(expression.get())) {
    appendLoanPlaces(places, semanticPlace(*set));
  } else if (const auto *indexSet = exprAs<IndexSet>(expression.get())) {
    appendLoanPlaces(places, semanticPlace(*indexSet));
  } else if (const auto *set = exprAs<PlaceSet>(expression.get())) {
    appendLoanPlaces(places, set->target(), true);
  } else if (const auto *dereferenceSet =
                 exprAs<DereferenceSet>(expression.get())) {
    SemanticPlace target = semanticPlace(dereferenceSet->object());
    if (target.root != nullptr) {
      target.projections.push_back({.kind = PlaceProjectionKind::Dereference});
      appendLoanPlaces(places, target);
    }
  } else if (const auto *postfix = exprAs<Postfix>(expression.get())) {
    if (postfix->oper().kind == TokenKind::PLUS_PLUS ||
        postfix->oper().kind == TokenKind::MINUS_MINUS) {
      appendLoanPlaces(places, postfix->expression(), true);
    }
  } else if (const auto *unary = exprAs<Unary>(expression.get())) {
    if (unary->oper().kind == TokenKind::PLUS_PLUS ||
        unary->oper().kind == TokenKind::MINUS_MINUS) {
      appendLoanPlaces(places, unary->right(), true);
    }
  }

  if (const ResolvedOperatorInfo *resolved =
          semanticModel.findOperator(*expression)) {
    if (resolved->declaration != nullptr &&
        receiverAllowsMutation(resolved->declaration->receiverMutability())) {
      const ExprPtr *conversion =
          explicitConversionReceiver(*expression, *resolved);
      if (const ExprPtr *receiver =
              conversion != nullptr ? conversion
                                    : operatorReceiverExpression(expression)) {
        appendLoanPlaces(places, *receiver, true);
      }
    }
    if (const auto *call = exprAs<Call>(expression.get())) {
      const std::size_t count =
          std::min(resolved->parameterTypes.size(), call->arguments().size());
      for (std::size_t index = 0; index < count; ++index) {
        if (resolved->parameterTypes[index].kind == SemanticType::Reference &&
            resolved->parameterTypes[index].referenceAccess ==
                AccessMode::Mutable) {
          appendLoanPlaces(places, call->arguments()[index], true);
        }
      }
    } else if (const auto *binary = exprAs<Binary>(expression.get())) {
      if (!resolved->parameterTypes.empty() &&
          resolved->parameterTypes.front().kind == SemanticType::Reference &&
          resolved->parameterTypes.front().referenceAccess ==
              AccessMode::Mutable) {
        appendLoanPlaces(places, binary->right(), true);
      }
    } else if (const auto *index = exprAs<Index>(expression.get())) {
      if (!resolved->parameterTypes.empty() &&
          resolved->parameterTypes.front().kind == SemanticType::Reference &&
          resolved->parameterTypes.front().referenceAccess ==
              AccessMode::Mutable) {
        appendLoanPlaces(places, index->index(), true);
      }
    }
  }

  if (const ResolvedConstructionInfo *resolved =
          semanticModel.findConstruction(*expression)) {
    const ExprList *arguments = nullptr;
    if (const auto *call = exprAs<Call>(expression.get())) {
      arguments = &call->arguments();
    } else if (const auto *initializer =
                   exprAs<DirectInitializer>(expression.get())) {
      arguments = &initializer->arguments();
    }
    if (arguments != nullptr) {
      const std::size_t count =
          std::min(resolved->parameterTypes.size(), arguments->size());
      for (std::size_t index = 0; index < count; ++index) {
        if (resolved->parameterTypes[index].kind == SemanticType::Reference &&
            resolved->parameterTypes[index].referenceAccess ==
                AccessMode::Mutable) {
          appendLoanPlaces(places, (*arguments)[index], true);
        }
      }
    }
  }

  const auto *call = exprAs<Call>(expression.get());
  const ResolvedCallInfo *resolved =
      call == nullptr ? nullptr : semanticModel.findCall(*call);
  const auto *qualifiedCallee =
      call == nullptr ? nullptr : exprAs<QualifiedName>(call->callee().get());
  const bool syntacticMove =
      qualifiedCallee != nullptr &&
      qualifiedCallee->name().segments.size() == 2 &&
      qualifiedCallee->name().segments[0].lexeme == "std" &&
      qualifiedCallee->name().segments[1].lexeme == "move";
  if (call != nullptr && call->arguments().size() == 1 &&
      (syntacticMove ||
       (resolved != nullptr && resolved->intrinsic == IntrinsicKind::Move))) {
    appendLoanPlaces(places, call->arguments().front(), true);
  }
  if (call != nullptr && resolved != nullptr) {
    if (resolved->declaration != nullptr &&
        resolved->declaration->receiverMutability() ==
            ReceiverMutability::Mutable) {
      if (const auto *member = exprAs<Get>(call->callee().get())) {
        appendLoanPlaces(places, member->object(), true);
      } else if (currentClass) {
        appendLoanPlaces(places, SemanticPlace{.root = receiverStateSymbol(),
                                               .receiver = true});
      }
    }
    const std::size_t count =
        std::min(resolved->parameterTypes.size(),
                 resolvedCallArgumentCount(*call, resolved));
    for (std::size_t index = 0; index < count; ++index) {
      if (resolved->parameterTypes[index].kind == SemanticType::Reference &&
          resolved->parameterTypes[index].referenceAccess ==
              AccessMode::Mutable) {
        appendLoanPlaces(places, *resolvedCallArgument(*call, resolved, index),
                         true);
      }
    }
  }

  if (const auto *assign = exprAs<Assign>(expression.get())) {
    appendTransientMutationPlaces(assign->value(), places);
  } else if (const auto *array = exprAs<ArrayInitializer>(expression.get())) {
    for (const ExprPtr &element : array->elements()) {
      appendTransientMutationPlaces(element, places);
    }
  } else if (const auto *binary = exprAs<Binary>(expression.get())) {
    appendTransientMutationPlaces(binary->left(), places);
    appendTransientMutationPlaces(binary->right(), places);
  } else if (const auto *conditional =
                 exprAs<ConditionalExpr>(expression.get())) {
    appendTransientMutationPlaces(conditional->condition(), places);
    appendTransientMutationPlaces(conditional->thenExpression(), places);
    appendTransientMutationPlaces(conditional->elseExpression(), places);
  } else if (call != nullptr) {
    appendTransientMutationPlaces(call->callee(), places);
    for (const ExprPtr &argument : call->arguments()) {
      appendTransientMutationPlaces(argument, places);
    }
  } else if (const auto *conversion = exprAs<Conversion>(expression.get())) {
    appendTransientMutationPlaces(conversion->value(), places);
  } else if (const auto *initializer =
                 exprAs<DirectInitializer>(expression.get())) {
    for (const ExprPtr &argument : initializer->arguments()) {
      appendTransientMutationPlaces(argument, places);
    }
  } else if (const auto *dereferenceSet =
                 exprAs<DereferenceSet>(expression.get())) {
    appendTransientMutationPlaces(dereferenceSet->object(), places);
    appendTransientMutationPlaces(dereferenceSet->value(), places);
  } else if (const auto *set = exprAs<PlaceSet>(expression.get())) {
    appendTransientMutationPlaces(set->target(), places);
    appendTransientMutationPlaces(set->value(), places);
  } else if (const auto *member = exprAs<Get>(expression.get())) {
    appendTransientMutationPlaces(member->object(), places);
  } else if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    appendTransientMutationPlaces(grouping->expression(), places);
  } else if (const auto *index = exprAs<Index>(expression.get())) {
    appendTransientMutationPlaces(index->object(), places);
    appendTransientMutationPlaces(index->index(), places);
  } else if (const auto *indexSet = exprAs<IndexSet>(expression.get())) {
    appendTransientMutationPlaces(indexSet->object(), places);
    appendTransientMutationPlaces(indexSet->index(), places);
    appendTransientMutationPlaces(indexSet->value(), places);
  } else if (const auto *logical = exprAs<Logical>(expression.get())) {
    appendTransientMutationPlaces(logical->left(), places);
    appendTransientMutationPlaces(logical->right(), places);
  } else if (const auto *postfix = exprAs<Postfix>(expression.get())) {
    appendTransientMutationPlaces(postfix->expression(), places);
  } else if (const auto *set = exprAs<Set>(expression.get())) {
    appendTransientMutationPlaces(set->object(), places);
    appendTransientMutationPlaces(set->value(), places);
  } else if (const auto *unary = exprAs<Unary>(expression.get())) {
    appendTransientMutationPlaces(unary->right(), places);
  }
}

// Collects the stable places this call operand reads while it is being
// evaluated (named place reads and the receiver places reached through
// nested calls) together with every transient borrow production anywhere
// in the operand. MIR evaluates the reads as instruction-operand reads,
// and a production's unretained result loan stays live through the
// enclosing full-expression even when a nested call consumes its carrier,
// so either side conflicts with a sibling operand on the same owner.
auto SemanticVisitor::Impl::appendTransientReadPlaces(
    const ExprPtr &expression, std::vector<SemanticLoanPlace> &places,
    std::vector<TransientBorrowProduction> &productions) -> void {
  if (!expression) {
    return;
  }

  const auto appendProduction = [&](TransientBorrowProduction production) {
    if (production.place.root == 0 && !production.place.receiver) {
      return;
    }
    const auto existing =
        std::find_if(productions.begin(), productions.end(),
                     [&](const TransientBorrowProduction &candidate) {
                       return candidate.place == production.place &&
                              candidate.retainsValueCarrier ==
                                  production.retainsValueCarrier;
                     });
    if (existing == productions.end()) {
      productions.push_back(std::move(production));
    } else {
      // A stored shared reference must not erase the stronger access of the
      // operation that produced it from a mutable receiver or parameter.
      if (production.access == AccessMode::Mutable) {
        existing->access = AccessMode::Mutable;
      }
      existing->globalStorage |= production.globalStorage;
    }
  };
  if (auto production = transientBorrowProduction(expression)) {
    appendProduction(std::move(*production));
  }
  const ExpressionInfo *info = semanticModel.findExpression(*expression);
  if (info != nullptr && info->category == ValueCategory::Value &&
      info->traits.containsBorrowedState) {
    // A fresh composed carrier keeps every stored owner borrowed until the
    // full expression ends, even if a nested method has already returned a
    // scalar. Named Place carriers retain their ordinary last-use lifetime.
    for (const auto &dependency : storedBorrowDependencies(expression)) {
      appendProduction({.place = dependency.place,
                        .access = dependency.access,
                        .retainsValueCarrier = true});
    }
  }

  // A place chain contributes only its deepest resolved place: reading
  // `parent.right` is not a whole-`parent` read, so the chain's prefixes
  // must not be collected while unwinding to the root.
  const ExprKind kind = expression->exprKind();
  if (kind == ExprKind::Variable || kind == ExprKind::This) {
    appendLoanPlaces(places, expression);
    return;
  }
  if (kind == ExprKind::Get || kind == ExprKind::Index) {
    auto origins = loanPlaces(expression);
    const bool stablePlace = !origins.empty();
    if (stablePlace) {
      for (auto &origin : origins)
        appendUniquePlace(places, std::move(origin));
    }
    if (const auto *member = exprAs<Get>(expression.get())) {
      if (!stablePlace) {
        appendTransientReadPlaces(member->object(), places, productions);
      } else {
        // The projection itself is the only place read, but a call producing
        // its carrier can still retain a loan through the full expression.
        std::vector<SemanticLoanPlace> ignoredReads;
        appendTransientReadPlaces(member->object(), ignoredReads, productions);
      }
      return;
    }
    if (const auto *index = exprAs<Index>(expression.get())) {
      if (!stablePlace) {
        appendTransientReadPlaces(index->object(), places, productions);
      }
      appendTransientReadPlaces(index->index(), places, productions);
      return;
    }
  }

  if (const auto *assign = exprAs<Assign>(expression.get())) {
    appendTransientReadPlaces(assign->value(), places, productions);
  } else if (const auto *array = exprAs<ArrayInitializer>(expression.get())) {
    for (const ExprPtr &element : array->elements()) {
      appendTransientReadPlaces(element, places, productions);
    }
  } else if (const auto *binary = exprAs<Binary>(expression.get())) {
    appendTransientReadPlaces(binary->left(), places, productions);
    appendTransientReadPlaces(binary->right(), places, productions);
  } else if (const auto *conditional =
                 exprAs<ConditionalExpr>(expression.get())) {
    appendTransientReadPlaces(conditional->condition(), places, productions);
    appendTransientReadPlaces(conditional->thenExpression(), places,
                              productions);
    appendTransientReadPlaces(conditional->elseExpression(), places,
                              productions);
  } else if (const auto *call = exprAs<Call>(expression.get())) {
    if (const auto *member = exprAs<Get>(call->callee().get())) {
      // A method callee is not itself a place read; the receiver object
      // carries the operand read.
      appendTransientReadPlaces(member->object(), places, productions);
    } else {
      appendTransientReadPlaces(call->callee(), places, productions);
      if (const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
          resolved != nullptr && currentClass &&
          (resolved->receiverMode == CallReceiverMode::Value ||
           resolved->receiverMode == CallReceiverMode::ReadOnlyPlace ||
           resolved->receiverMode == CallReceiverMode::Consuming)) {
        appendUniquePlace(places,
                          loanPlace(SemanticPlace{.root = receiverStateSymbol(),
                                                  .receiver = true}));
      }
    }
    for (const ExprPtr &argument : call->arguments()) {
      appendTransientReadPlaces(argument, places, productions);
    }
  } else if (const auto *conversion = exprAs<Conversion>(expression.get())) {
    appendTransientReadPlaces(conversion->value(), places, productions);
  } else if (const auto *initializer =
                 exprAs<DirectInitializer>(expression.get())) {
    for (const ExprPtr &argument : initializer->arguments()) {
      appendTransientReadPlaces(argument, places, productions);
    }
  } else if (const auto *dereferenceSet =
                 exprAs<DereferenceSet>(expression.get())) {
    appendTransientReadPlaces(dereferenceSet->object(), places, productions);
    appendTransientReadPlaces(dereferenceSet->value(), places, productions);
  } else if (const auto *set = exprAs<PlaceSet>(expression.get())) {
    appendTransientReadPlaces(set->target(), places, productions);
    appendTransientReadPlaces(set->value(), places, productions);
  } else if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    appendTransientReadPlaces(grouping->expression(), places, productions);
  } else if (const auto *indexSet = exprAs<IndexSet>(expression.get())) {
    appendTransientReadPlaces(indexSet->object(), places, productions);
    appendTransientReadPlaces(indexSet->index(), places, productions);
    appendTransientReadPlaces(indexSet->value(), places, productions);
  } else if (const auto *logical = exprAs<Logical>(expression.get())) {
    appendTransientReadPlaces(logical->left(), places, productions);
    appendTransientReadPlaces(logical->right(), places, productions);
  } else if (const auto *postfix = exprAs<Postfix>(expression.get())) {
    appendTransientReadPlaces(postfix->expression(), places, productions);
  } else if (const auto *set = exprAs<Set>(expression.get())) {
    appendTransientReadPlaces(set->object(), places, productions);
    appendTransientReadPlaces(set->value(), places, productions);
  } else if (const auto *unary = exprAs<Unary>(expression.get())) {
    appendTransientReadPlaces(unary->right(), places, productions);
  }
}

auto SemanticVisitor::Impl::reportCommaTransientConflict(
    const ExprPtr &expression, const Token &location) -> bool {
  if (!expression) {
    return false;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return reportCommaTransientConflict(grouping->expression(), location);
  }
  const auto *binary = exprAs<Binary>(expression.get());
  if (binary == nullptr || binary->oper().kind != TokenKind::COMMA) {
    return false;
  }
  if (reportCommaTransientConflict(binary->left(), location) ||
      reportCommaTransientConflict(binary->right(), location)) {
    return true;
  }

  const std::optional<TransientBorrowProduction> left =
      transientBorrowProduction(binary->left());
  const std::optional<TransientBorrowProduction> right =
      transientBorrowProduction(binary->right());
  if (left && right &&
      (left->access == AccessMode::Mutable ||
       right->access == AccessMode::Mutable) &&
      loanPlacesOverlap(left->place, right->place)) {
    report(location,
           "A comma expression creates overlapping transient borrowed "
           "results while at least one result is mutable.",
           "GTI-S2017");
    return true;
  }

  if (left) {
    std::vector<SemanticLoanPlace> rightMutations;
    appendTransientMutationPlaces(binary->right(), rightMutations);
    if (std::any_of(rightMutations.begin(), rightMutations.end(),
                    [&](const SemanticLoanPlace &mutation) {
                      return loanPlacesOverlap(left->place, mutation);
                    })) {
      report(location,
             "The right side of a comma expression mutates storage "
             "borrowed by its left side within the same full expression.",
             "GTI-S2017");
      return true;
    }
  }
  return false;
}

auto SemanticVisitor::Impl::validateSequencedTransientLoans(
    const ExprPtr &left, const ExprPtr &right, const Token &location) -> void {
  std::vector<SemanticLoanPlace> leftReads;
  std::vector<TransientBorrowProduction> productions;
  appendTransientReadPlaces(left, leftReads, productions);
  if (productions.empty()) {
    return;
  }
  std::vector<SemanticLoanPlace> rightReads;
  std::vector<TransientBorrowProduction> rightProductions;
  appendTransientReadPlaces(right, rightReads, rightProductions);
  std::vector<SemanticLoanPlace> rightMutations;
  appendTransientMutationPlaces(right, rightMutations);
  for (const auto &production : productions) {
    // A scalar read through a temporary reference ends that read's loan.
    // A returned carrier, even after projecting a copied scalar from it,
    // survives until the enclosing full-expression cleanup boundary.
    if (!production.retainsValueCarrier) {
      continue;
    }
    const auto overlaps = [&](const SemanticLoanPlace &place) {
      return loanPlacesOverlap(production.place, place);
    };
    if (std::any_of(rightMutations.begin(), rightMutations.end(), overlaps) ||
        (production.access == AccessMode::Mutable &&
         std::any_of(rightReads.begin(), rightReads.end(), overlaps))) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2017", DiagnosticPhase::Semantics, location,
          "A later operand accesses storage that is still borrowed by an "
          "earlier operand of the same full expression.");
      diagnostic.related.push_back({tokenSpan(expressionToken(left)),
                                    "This operand produces a borrow retained "
                                    "until the full expression ends."});
      diagnostic.hints.emplace_back(
          "Finish the first operation in a separate statement before "
          "accessing the owner again.");
      diagnostics.emplace_back(std::move(diagnostic));
      return;
    }
  }
}

auto SemanticVisitor::Impl::validateForeignStorageBorrow(
    const ExprPtr &expression) -> bool {
  const SemanticPlace place = semanticPlace(expression);
  const SymbolId root = place.rootIdentity != 0 ? place.rootIdentity
                        : place.root != nullptr ? toolingSymbolFor(*place.root)
                                                : 0;
  if (semanticModel.findForeignStorage(root) == nullptr) {
    return true;
  }
  report(
      expressionToken(expression),
      "Foreign storage has no GTI owner lifetime for a checked reference; use "
      "an explicit raw-to-checked operation with a valid owner witness.",
      "GTI-S2054");
  return false;
}

auto SemanticVisitor::Impl::validateNativePointerSlotBorrow(
    const ExprPtr &expression, const SemanticType &reference) -> bool {
  if (reference.kind != SemanticType::Reference ||
      reference.arguments.size() != 1 ||
      reference.arguments.front().kind != SemanticType::RawPointer)
    return true;
  const Expr *slot = expression.get();
  while (const auto *grouping = exprAs<Grouping>(slot))
    slot = grouping->expression().get();
  const auto typeOf = [&](const Expr *operand) {
    const SemanticType *type =
        operand == nullptr ? nullptr : semanticModel.findType(*operand);
    if (type == nullptr)
      return SemanticType(SemanticType::Unknown);
    if (type->kind == SemanticType::Reference && type->arguments.size() == 1)
      return type->arguments.front();
    return *type;
  };
  // A fixed array's own elements, including the hidden range-for element of
  // a fixed array, and the fields of a native record or union.
  bool native = false;
  if (const auto *index = exprAs<Index>(slot))
    native = typeOf(index->object().get()).kind == SemanticType::Array;
  else if (const auto *element = exprAs<Unary>(slot);
           element != nullptr && element->oper().kind == TokenKind::STAR &&
           element->rangeObject())
    native = typeOf(element->rangeObject().get()).kind == SemanticType::Array;
  else if (const auto *field = exprAs<Get>(slot)) {
    // Through `.`, a raw `->` or an overloaded `operator->`.
    SemanticType object = memberAccessObjectType(*field);
    if (object.kind == SemanticType::Reference &&
        object.arguments.size() == 1) {
      SemanticType referent = object.arguments.front();
      object = std::move(referent);
    }
    const ClassInfo *owner = classInfo(object);
    native = owner != nullptr &&
             (owner->cAbiRecord || owner->kind == ClassKind::Union);
  }
  if (!native)
    return true;
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2056", DiagnosticPhase::Semantics, expressionToken(expression),
      "A checked reference cannot name a raw pointer stored with native "
      "layout, such as an element of a fixed pointer array or a field of a "
      "native record or union.");
  diagnostic.hints.emplace_back(
      "Copy the pointer into a local and borrow that, or take the slot's "
      "address as a pointer-to-pointer.");
  diagnostics.emplace_back(std::move(diagnostic));
  return false;
}

auto SemanticVisitor::Impl::validateCallPlaceExclusivityImpl(
    std::span<const SemanticType> parameterTypes,
    std::span<const ExprPtr *> arguments, const Token &location,
    std::optional<SemanticLoanPlace> receiver, AccessMode receiverAccess,
    bool transientReceiver, std::span<const SemanticLoanPlace> receiverOrigins)
    -> void {
  struct CallPlace {
    // These are possible owners of one evaluated reference, not separate
    // simultaneous call accesses. Compare with other operands, never itself.
    std::vector<SemanticLoanPlace> origins;
    AccessMode access = AccessMode::ReadOnly;
    std::size_t argument = 0;
    bool receiver = false;
    bool transient = false;
  };
  std::vector<CallPlace> places;
  std::vector<std::pair<std::size_t, SemanticLoanPlace>> mutations;
  const std::size_t count = std::min(parameterTypes.size(), arguments.size());
  const auto argumentAnchor = [](const ExprPtr &expression) {
    if (const auto *argumentCall = exprAs<Call>(expression.get())) {
      return callableToken(argumentCall->callee());
    }
    return expressionToken(expression);
  };
  const auto reportBorrowMutation = [&](std::size_t mutation,
                                        std::optional<std::size_t> borrowed) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2017", DiagnosticPhase::Semantics,
        argumentAnchor(*arguments[mutation]),
        "A later call operand mutates storage borrowed by an earlier "
        "operand; the transient loan remains active through the "
        "enclosing full-expression.");
    diagnostic.related.push_back(
        {tokenSpan(borrowed ? argumentAnchor(*arguments[*borrowed]) : location),
         borrowed ? "This earlier operand produces the retained borrow."
                  : "The receiver evaluation produces the retained borrow."});
    diagnostic.hints.emplace_back(
        "Complete the mutation before creating the borrow, or finish "
        "using the borrow in a separate statement first.");
    diagnostics.emplace_back(std::move(diagnostic));
  };
  for (std::size_t index = 0; index < count; ++index) {
    if (reportCommaTransientConflict(*arguments[index], location)) {
      return;
    }
    const SemanticType &parameter = parameterTypes[index];
    std::vector<SemanticLoanPlace> argumentMutations;
    appendTransientMutationPlaces(*arguments[index], argumentMutations);
    for (SemanticLoanPlace &place : argumentMutations) {
      mutations.emplace_back(index, std::move(place));
    }
    const bool referenceParameter = parameter.kind == SemanticType::Reference;
    if (referenceParameter &&
        (!validateForeignStorageBorrow(*arguments[index]) ||
         !validateNativePointerSlotBorrow(*arguments[index], parameter))) {
      continue;
    }
    const ExpressionInfo *argumentInfo =
        *arguments[index] ? semanticModel.findExpression(**arguments[index])
                          : nullptr;
    const bool borrowedCarrierParameter =
        !referenceParameter && (typeTraits(parameter).containsBorrowedState ||
                                (argumentInfo != nullptr &&
                                 argumentInfo->traits.containsBorrowedState));
    if (!referenceParameter && !borrowedCarrierParameter) {
      continue;
    }
    if (borrowedCarrierParameter &&
        composedCarrierPaths(storedReferencePaths(parameter))) {
      for (const auto &dependency :
           storedBorrowDependencies(*arguments[index])) {
        places.push_back({.origins = {dependency.place},
                          .access = dependency.access,
                          .argument = index});
      }
      continue;
    }
    if (referenceParameter) {
      if (selectedReferenceDependencies(semanticPlace(*arguments[index]))) {
        places.push_back({.origins = loanPlaces(*arguments[index]),
                          .access = parameter.referenceAccess,
                          .argument = index});
        continue;
      }
    }
    std::optional<TransientBorrowProduction> production =
        transientBorrowProduction(*arguments[index]);
    if (!production && borrowedCarrierParameter) {
      if (const std::optional<RetainedLoanSource> retained =
              retainedLoanSource(*arguments[index])) {
        if (const SemanticLoanInfo *loan =
                semanticModel.findLoan(retained->loan)) {
          production = TransientBorrowProduction{.place = loan->place,
                                                 .access = loan->access};
        }
      }
    }
    if (!production && borrowedCarrierParameter) {
      continue;
    }
    SemanticLoanPlace place = production ? std::move(production->place)
                                         : loanPlace(*arguments[index]);
    if (place.root != 0 || place.receiver) {
      places.push_back(
          {.origins = {std::move(place)},
           .access = production && production->access == AccessMode::Mutable
                         ? AccessMode::Mutable
                     : referenceParameter ? parameter.referenceAccess
                                          : AccessMode::ReadOnly,
           .argument = index});
    }
  }
  if (receiver && semanticModel.findForeignStorage(receiver->root) != nullptr) {
    report(location,
           "Foreign storage cannot provide a checked method receiver; use an "
           "explicit raw-to-checked operation with a valid owner witness.",
           "GTI-S2054");
  }
  if (receiver && (receiver->root != 0 || receiver->receiver)) {
    std::vector<SemanticLoanPlace> origins(receiverOrigins.begin(),
                                           receiverOrigins.end());
    if (origins.empty())
      origins.push_back(std::move(*receiver));
    places.push_back({.origins = std::move(origins),
                      .access = receiverAccess,
                      .argument = arguments.size(),
                      .receiver = true,
                      .transient = transientReceiver});
  }
  const auto overlaps = [&](const CallPlace &access,
                            const SemanticLoanPlace &origin) {
    return std::any_of(access.origins.begin(), access.origins.end(),
                       [&](const auto &candidate) {
                         return loanPlacesOverlap(candidate, origin);
                       });
  };
  for (std::size_t left = 0; left < places.size(); ++left) {
    for (std::size_t right = left + 1; right < places.size(); ++right) {
      if ((places[left].access == AccessMode::Mutable ||
           places[right].access == AccessMode::Mutable) &&
          std::any_of(places[right].origins.begin(),
                      places[right].origins.end(), [&](const auto &origin) {
                        return overlaps(places[left], origin);
                      })) {
        report(location,
               "Call creates overlapping reference accesses while at least "
               "one access is mutable.",
               "GTI-S2017");
        return;
      }
    }
  }
  for (const CallPlace &retained : places) {
    if (retained.receiver && !retained.transient) {
      continue;
    }
    for (const auto &[argument, mutation] : mutations) {
      if (argument != retained.argument && overlaps(retained, mutation)) {
        const std::size_t retainedSequence =
            retained.receiver ? 0 : retained.argument + 1;
        const std::size_t mutationSequence = argument + 1;
        if (retainedSequence < mutationSequence) {
          reportBorrowMutation(
              argument, retained.receiver ? std::nullopt
                                          : std::optional(retained.argument));
          return;
        }
        // Earlier operand effects finish before this borrow begins. Ordinary
        // call-input staging preserves that order in HIR/MIR; any loan that
        // actually survives the earlier operand is checked independently
        // below and by simultaneous parameter exclusivity above.
      }
    }
  }
  // A transient borrow production's unretained result loan stays live from
  // its producing operand through the enclosing full-expression, even when
  // a nested call consumes the carrier inside its own operand. A later
  // operand of the same call therefore may not mutate storage under any
  // such loan, or read storage under an exclusive one. A reference
  // parameter's loan is instead materialized at the call itself, after
  // operand evaluation, so plain reference arguments do not participate.
  const bool transientReceiverProduction =
      std::any_of(places.begin(), places.end(), [](const CallPlace &place) {
        return place.receiver && place.transient;
      });
  if (count < 2 && !transientReceiverProduction) {
    return;
  }
  struct RetainedProduction {
    std::size_t sequence = 0;
    std::optional<std::size_t> argument;
    SemanticLoanPlace place;
    AccessMode access = AccessMode::ReadOnly;
  };
  std::vector<RetainedProduction> productions;
  std::vector<std::pair<std::size_t, SemanticLoanPlace>> reads;
  for (std::size_t index = 0; index < count; ++index) {
    std::vector<SemanticLoanPlace> argumentReads;
    std::vector<TransientBorrowProduction> argumentProductions;
    appendTransientReadPlaces(*arguments[index], argumentReads,
                              argumentProductions);
    for (SemanticLoanPlace &place : argumentReads) {
      reads.emplace_back(index, std::move(place));
    }
    for (TransientBorrowProduction &production : argumentProductions) {
      productions.push_back({.sequence = index + 1,
                             .argument = index,
                             .place = std::move(production.place),
                             .access = production.access});
    }
  }
  for (const CallPlace &retained : places) {
    if (retained.receiver && retained.transient) {
      for (const auto &origin : retained.origins)
        productions.push_back({.sequence = 0,
                               .argument = std::nullopt,
                               .place = origin,
                               .access = retained.access});
    }
  }
  for (const RetainedProduction &retained : productions) {
    for (const auto &[argument, mutation] : mutations) {
      if ((retained.argument && *retained.argument == argument) ||
          argument + 1 <= retained.sequence ||
          !loanPlacesOverlap(retained.place, mutation)) {
        continue;
      }
      reportBorrowMutation(argument, retained.argument);
      return;
    }
    if (retained.access != AccessMode::Mutable) {
      continue;
    }
    for (const auto &[argument, read] : reads) {
      if ((retained.argument && *retained.argument == argument) ||
          argument + 1 <= retained.sequence ||
          !loanPlacesOverlap(retained.place, read)) {
        continue;
      }
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2017", DiagnosticPhase::Semantics,
          argumentAnchor(*arguments[argument]),
          "Cannot read storage while an exclusive mutable borrow created "
          "by an earlier operand of the same call is still live.");
      if (retained.argument) {
        diagnostic.related.push_back(
            {tokenSpan(argumentAnchor(*arguments[*retained.argument])),
             "The exclusive mutable borrow created here stays live until "
             "the call completes."});
      }
      const auto *borrowCall =
          retained.argument ? exprAs<Call>(arguments[*retained.argument]->get())
                            : nullptr;
      if (borrowCall != nullptr &&
          exprAs<Get>(borrowCall->callee().get()) != nullptr) {
        diagnostic.hints.emplace_back(
            "Whole-container algorithms take the container itself; pass "
            "the container once instead of borrowing an overlapping "
            "iterator from it.");
      }
      diagnostics.emplace_back(std::move(diagnostic));
      return;
    }
  }
}

auto SemanticVisitor::Impl::validateCallPlaceExclusivity(
    std::span<const SemanticType> parameterTypes,
    std::span<const ExprPtr> arguments, const Token &location,
    std::optional<SemanticLoanPlace> receiver, AccessMode receiverAccess,
    bool transientReceiver, std::span<const SemanticLoanPlace> receiverOrigins)
    -> void {
  std::vector<const ExprPtr *> argumentReferences;
  argumentReferences.reserve(arguments.size());
  for (const ExprPtr &argument : arguments) {
    argumentReferences.push_back(&argument);
  }
  validateCallPlaceExclusivityImpl(parameterTypes, argumentReferences, location,
                                   std::move(receiver), receiverAccess,
                                   transientReceiver, receiverOrigins);
}

auto SemanticVisitor::Impl::noteUnsupportedNestedLoanControlFlow() -> void {
  if (!loanFlow.activeConditionalArms.empty()) {
    const ActiveLoanFlowConditionalArm active =
        loanFlow.activeConditionalArms.back();
    if (active.conditional < loanFlow.conditionals.size()) {
      LoanFlowConditional &conditional =
          loanFlow.conditionals[active.conditional];
      LoanFlowConditionalArm &arm =
          active.thenBranch ? conditional.thenArm : conditional.elseArm;
      arm.containsUnsupportedControlFlow = true;
    }
  }
  if (!loanFlow.activeSwitchArms.empty()) {
    const ActiveLoanFlowSwitchArm active = loanFlow.activeSwitchArms.back();
    if (active.switchIndex < loanFlow.switches.size() &&
        active.armIndex < loanFlow.switches[active.switchIndex].arms.size()) {
      loanFlow.switches[active.switchIndex]
          .arms[active.armIndex]
          .containsUnsupportedControlFlow = true;
    }
  }
}

auto SemanticVisitor::Impl::recordLoanFlowBreak(
    const LoopControlStmt &statement) -> void {
  if (valueControlFlow.empty() ||
      valueControlFlow.back().statement == nullptr ||
      loanFlow.currentRegion == 0 || loanFlow.currentStatement == nullptr) {
    return;
  }
  loanFlow.breaks.push_back({.statement = &statement,
                             .target = valueControlFlow.back().statement,
                             .region = loanFlow.currentRegion,
                             .order = loanFlow.currentOrder,
                             .sequence = loanFlow.nextSequence++,
                             .path = loanFlow.activeConditionalArms,
                             .switchPath = loanFlow.activeSwitchArms});
}

auto SemanticVisitor::Impl::beginLoanFlowLoop(const Stmt &statement) -> void {
  // The semantic loop body is visited once. An incoming exact alternative is
  // not a proof for every backedge iteration of a binding the body can write.
  // Until conditional sets join the existing loop fixed-point contract, keep
  // that fact unknown; a dominating construction inside the body can
  // establish a fresh complete set. A binding nothing can write keeps its
  // fact: only a move ends it, and value flow then rejects any read reached
  // through the backedge, while loan flow keeps a read-only reference's
  // referent unwritable for as long as a later iteration reads it. This never
  // changes the actual loans or their availability/endpoint analysis.
  LoanFlowLoop loop{.region = loanFlow.currentRegion,
                    .order = loanFlow.currentOrder,
                    .statement = &statement,
                    .firstNestedLoan = nextSemanticLoanId};
  for (auto &scope : scopes)
    for (auto &[_, symbol] : scope)
      if (symbol.assignable && symbol.completeBorrowSet) {
        if (const SymbolId id = symbolForDeclaration(symbol.declaration))
          loop.droppedBorrowSets.push_back(id);
        symbol.completeBorrowSet.reset();
      }
  const std::size_t index = loanFlow.loops.size();
  loanFlow.loops.push_back(std::move(loop));
  loanFlow.activeLoops.push_back(index);
}

auto SemanticVisitor::Impl::endLoanFlowLoop() -> void {
  if (!loanFlow.activeLoops.empty()) {
    loanFlow.activeLoops.pop_back();
  }
}

auto SemanticVisitor::Impl::attachLoanCarrier(SemanticLoanId loan,
                                              Symbol &carrier,
                                              SymbolId carrierSymbol,
                                              bool transfer) -> void {
  const auto found = loanFlow.records.find(loan);
  if (found == loanFlow.records.end()) {
    return;
  }
  LoanFlowRecord &flow = found->second;
  if (carrierSymbol == 0) {
    flow.confinedToRegion = false;
  }
  if (loanFlow.currentRegion != 0 && loanFlow.currentRegion != flow.region) {
    flow.confinedToRegion = false;
  }
  if (!transfer && !flow.carriers.empty()) {
    flow.hasAliases = true;
  }
  flow.carriers.insert(carrierSymbol);
  loanFlow.carrierLoans.insert_or_assign(carrierSymbol, loan);
  carrier.retainedLoan = loan;
  semanticModel.recordLoanCarrier(loan, carrierSymbol);
}

auto SemanticVisitor::Impl::registerStorageLoan(SymbolId owner, bool receiver,
                                                SemanticLoanId loan) -> void {
  if (receiver) {
    appendUniqueLoan(loanFlow.receiverStorageLoans, loan);
    return;
  }
  if (owner == 0) {
    return;
  }
  for (Scope &scope : scopes) {
    for (auto &[_, symbol] : scope) {
      if (toolingSymbolFor(symbol) == owner) {
        appendUniqueLoan(symbol.storageLoans, loan);
        return;
      }
    }
  }
}

auto SemanticVisitor::Impl::createRetainedLoan(
    Symbol &carrier, SymbolId carrierSymbol, SemanticLoanKind kind,
    const Expr *origin, SemanticLoanPlace place, SymbolId owner,
    bool receiverOrigin, AccessMode access, bool protectsStorage,
    SemanticLoanId parent, bool entry, std::vector<SymbolId> designatedFields,
    std::vector<BorrowDependencyCondition> conditions) -> SemanticLoanId {
  if (semanticModel.findForeignStorage(place.root) != nullptr &&
      origin != nullptr) {
    report(expressionToken(*origin),
           "Foreign storage has no GTI owner lifetime for a checked reference; "
           "use "
           "an explicit raw-to-checked operation with a valid owner witness.",
           "GTI-S2054");
    return 0;
  }
  const SemanticLoanId id = nextSemanticLoanId++;
  const bool lifecycleBound = lifecycleBoundBorrowCarrierFor(carrier.type);
  semanticModel.recordLoan({.id = id,
                            .kind = kind,
                            .origin = origin,
                            .owner = owner,
                            .receiverOrigin = receiverOrigin,
                            .parent = parent,
                            .entry = entry,
                            .place = std::move(place),
                            .access = access,
                            .protectsStorage = protectsStorage,
                            .lifecycleBound = lifecycleBound,
                            .designatedFields = std::move(designatedFields),
                            .conditions = std::move(conditions),
                            .carriers = {carrierSymbol}});
  loanFlow.records.emplace(
      id, LoanFlowRecord{.id = id,
                         .region = loanFlow.currentRegion,
                         .lastUseOrder = loanFlow.currentOrder,
                         .lastUse = loanFlow.currentStatement,
                         .carriers = {carrierSymbol},
                         .confinedToRegion = carrierSymbol != 0,
                         .lifecycleBound = lifecycleBound});
  loanFlow.carrierLoans.insert_or_assign(carrierSymbol, id);
  carrier.retainedLoan = id;
  if (protectsStorage) {
    registerStorageLoan(owner, receiverOrigin, id);
  }
  return id;
}

auto SemanticVisitor::Impl::recordStoredEntryDependency(
    Symbol &carrier, SymbolId carrierSymbol, const SemanticType &type,
    bool receiver, const Stmt *declaration) -> SemanticLoanId {
  const SemanticLoanPlace place{.root = carrierSymbol, .receiver = receiver};
  const auto paths = storedReferencePaths(type);
  const bool abstractEntry =
      receiver || carrier.type.kind == SemanticType::Reference;
  const bool textEntry =
      abstractEntry && (type.kind == SemanticType::StringView ||
                        type.kind == SemanticType::CString);
  std::vector<SymbolId> designatedFields;
  std::vector<BorrowDependencyCondition> conditions;
  std::vector<SemanticLoanDependency> additional;
  for (const auto &path : paths) {
    auto fields = storedFieldSymbols(type, path.fields);
    auto qualified = storedConditionSymbols(type, path.conditions);
    if (fields.size() != path.fields.size() || !qualified)
      return 0;
    if (designatedFields.empty()) {
      designatedFields = std::move(fields);
      conditions = std::move(*qualified);
    } else
      additional.push_back({.place = place,
                            .designatedFields = std::move(fields),
                            .access = path.reference.referenceAccess,
                            .conditions = std::move(*qualified)});
  }
  if (designatedFields.empty() && !textEntry)
    return 0;
  const SemanticLoanId id = createRetainedLoan(
      carrier, carrierSymbol, SemanticLoanKind::StoredValue, nullptr, place,
      carrierSymbol, receiver,
      textEntry       ? AccessMode::ReadOnly
      : abstractEntry ? paths.front().reference.referenceAccess
                      : borrowAccess(type),
      false, 0, true, std::move(designatedFields), std::move(conditions));
  SemanticLoanInfo loan = *semanticModel.findLoan(id);
  loan.additionalDependencies = std::move(additional);
  if (abstractEntry) {
    // Abstract caller-owned referents are not another alias of the physical
    // carrier parameter. Its Reference entry retains this separate authority.
    loan.carriers.clear();
    // The caller owns the carrier and any release obligation it contains.
    // This entry describes its declared reference fields only; it neither
    // acquires that obligation nor inherits a capability-produced loan's
    // stronger access (such as the loan of a live lock guard).
    loan.lifecycleBound = false;
    loanFlow.records.at(id).lifecycleBound = false;
    // Only receiver entries lack an ordinary binding to select their body.
    // A reference formal is selected through its physical Parameter loan,
    // including inside lambdas where the enclosing function is not its owner.
    loan.entryDeclaration =
        receiver ? (declaration != nullptr ? declaration
                                           : currentFunctionDeclaration)
                 : nullptr;
  }
  semanticModel.recordLoan(std::move(loan));
  const auto inventory = storedReferenceInventory(type);
  auto alternatives = storedBorrowAlternativeInventory(type);
  if (inventory && alternatives &&
      std::any_of(inventory->begin(), inventory->end(),
                  [](const auto &path) { return !path.conditions.empty(); })) {
    SemanticBorrowDependencySet complete{.possibleAlternatives =
                                             std::move(*alternatives)};
    const auto *entry = semanticModel.findLoan(id);
    const auto append = [&](SemanticLoanDependency dependency) {
      // The declaration entry loan designates abstract external referents
      // through field paths. A complete value fact names each exact external
      // path explicitly, not the by-value carrier's local physical shell.
      for (const auto field : dependency.designatedFields)
        dependency.place.projections.push_back(
            {.kind = PlaceProjectionKind::Field, .field = field});
      dependency.parent = id;
      complete.dependencies.push_back(std::move(dependency));
    };
    append({.place = entry->place,
            .designatedFields = entry->designatedFields,
            .access = entry->access,
            .protectsStorage = entry->protectsStorage,
            .conditions = entry->conditions});
    for (const auto &dependency : entry->additionalDependencies)
      append(dependency);
    carrier.completeBorrowSet = std::move(complete);
  }
  return id;
}

auto SemanticVisitor::Impl::recordParameterEntryLoan(const Parameter &parameter)
    -> void {
  Symbol *carrier = resolveMutable(parameter.name);
  if (carrier == nullptr) {
    return;
  }
  const bool referenceCarrier = carrier->type.kind == SemanticType::Reference &&
                                carrier->type.arguments.size() == 1;
  const auto &carrierValueType =
      referenceCarrier ? carrier->type.arguments.front() : carrier->type;
  const auto conditionalInventory = storedReferenceInventory(carrierValueType);
  const bool conditionalCarrier =
      conditionalInventory &&
      std::any_of(conditionalInventory->begin(), conditionalInventory->end(),
                  [](const auto &path) { return !path.conditions.empty(); });
  if (!referenceCarrier && carrierValueType.kind == SemanticType::Enum) {
    auto alternatives = storedBorrowAlternativeInventory(carrierValueType);
    const auto &inventory = conditionalInventory;
    if (inventory && inventory->empty() && alternatives)
      carrier->completeBorrowSet = SemanticBorrowDependencySet{
          .possibleAlternatives = std::move(*alternatives)};
  }
  const auto carriesOpaqueWitness = [&](const SemanticType &type) {
    const auto *owner =
        type.kind == SemanticType::Reference && type.arguments.size() == 1
            ? classInfo(type.arguments.front())
            : nullptr;
    return owner != nullptr && owner->compilerCapability ==
                                   CompilerCapabilityTypeKind::ErasedObject;
  };
  if (parameter.pack && carrier->type.kind == SemanticType::TypePack &&
      carrier->type.concretePack) {
    // Elements cannot be named independently in the source language. Their
    // entry capabilities remain live for the whole call and whole-pack
    // forwarding reborrows each exact element, never the aggregate binding.
    const SymbolId symbol = toolingSymbolFor(*carrier);
    for (std::size_t index = 0; index < carrier->type.arguments.size();
         ++index) {
      const auto &element = carrier->type.arguments[index];
      const auto paths = storedReferencePaths(element);
      if (!paths.empty()) {
        const SemanticLoanPlace place{
            .root = symbol,
            .projections = {
                {.kind = PlaceProjectionKind::ConstantIndex, .index = index}}};
        SemanticLoanInfo loan{
            .id = nextSemanticLoanId++,
            .kind = SemanticLoanKind::StoredValue,
            .owner = symbol,
            .entry = true,
            .place = place,
            .access = paths.front().reference.referenceAccess,
            .protectsStorage = false,
            .lifecycleBound = lifecycleBoundBorrowCarrierFor(element),
            .designatedFields =
                storedFieldSymbols(element, paths.front().fields),
            .carriers = {symbol}};
        for (std::size_t field = 1; field < paths.size(); ++field)
          loan.additionalDependencies.push_back(
              {.place = place,
               .designatedFields =
                   storedFieldSymbols(element, paths[field].fields),
               .access = paths[field].reference.referenceAccess,
               .protectsStorage = false});
        // Pack elements are individually identified, but cannot be named or
        // reassigned independently by source. Do not overwrite the whole
        // pack binding's single-carrier flow entry with its last element.
        semanticModel.recordLoan(std::move(loan));
        continue;
      }
      if (element.kind != SemanticType::Reference ||
          (element.referenceAccess != AccessMode::Mutable &&
           !carriesOpaqueWitness(element))) {
        continue;
      }
      semanticModel.recordLoan(
          {.id = nextSemanticLoanId++,
           .kind = SemanticLoanKind::Reference,
           .owner = symbol,
           .entry = true,
           .place = {.root = symbol,
                     .projections = {{.kind =
                                          PlaceProjectionKind::ConstantIndex,
                                      .index = index}}},
           .access = element.referenceAccess,
           .protectsStorage = true,
           .carriers = {symbol}});
    }
    return;
  }
  const FunctionInfo *function =
      currentLambdaReturn ? nullptr : currentFunctionInfo();
  const std::optional<ReturnBorrowContract> contract =
      currentReturnBorrowContract();
  const bool summarizedParameterSource =
      (contract && contract->originParameter() == &parameter) ||
      (function != nullptr && function->declaration != nullptr &&
       function->returnBorrowOrigin == BorrowOriginKind::Argument &&
       std::any_of(
           function->returnBorrowParameters.begin(),
           function->returnBorrowParameters.end(), [&](std::size_t index) {
             return index < function->bodyDeclaration()->parameters().size() &&
                    &function->bodyDeclaration()->parameters()[index] ==
                        &parameter;
           }));
  const bool dynamicTextCarrier =
      carrier->type.kind == SemanticType::StringView ||
      carrier->type.kind == SemanticType::CString;
  const bool storedValueCarrier = isDirectStoredReferenceType(carrier->type) ||
                                  dynamicTextCarrier ||
                                  (!referenceCarrier && conditionalCarrier);
  if (!referenceCarrier && !storedValueCarrier) {
    return;
  }
  const bool summarizedReadOnlySource =
      referenceCarrier &&
      carrier->type.referenceAccess == AccessMode::ReadOnly &&
      summarizedParameterSource;
  const bool borrowedShell =
      referenceCarrier &&
      (isDirectStoredReferenceType(carrier->type.arguments.front()) ||
       conditionalCarrier ||
       carrier->type.arguments.front().kind == SemanticType::StringView ||
       carrier->type.arguments.front().kind == SemanticType::CString);
  if (referenceCarrier &&
      carrier->type.referenceAccess != AccessMode::Mutable &&
      !summarizedReadOnlySource && !borrowedShell &&
      !carriesOpaqueWitness(carrier->type)) {
    return;
  }
  // An opaque checked referent may contain dependencies that its current type
  // intentionally hides. Keep an entry witness even for shared parameters;
  // exact-type restoration can then retain those dependencies in any callable.
  const SymbolId carrierSymbol = toolingSymbolFor(*carrier);
  const SemanticLoanPlace place{.root = carrierSymbol};
  const AccessMode access = referenceCarrier ? carrier->type.referenceAccess
                                             : borrowAccess(carrier->type);
  if (storedValueCarrier && !dynamicTextCarrier) {
    const auto id = recordStoredEntryDependency(*carrier, carrierSymbol,
                                                carrier->type, false);
    if (id != 0)
      semanticModel.recordBindingLoan(parameter, id);
    return;
  }
  const SemanticLoanId dependency =
      borrowedShell
          ? recordStoredEntryDependency(*carrier, carrierSymbol,
                                        carrier->type.arguments.front(), false)
          : 0;
  const SemanticLoanId id = createRetainedLoan(
      *carrier, carrierSymbol,
      storedValueCarrier ? SemanticLoanKind::StoredValue
                         : SemanticLoanKind::Reference,
      nullptr, place, carrierSymbol, false, access,
      // A by-value stored-reference parameter carries an owner dependency,
      // but its own binding is the cursor/carrier object rather than the
      // unnameable caller-side owner storage.  Protecting the carrier as if
      // it were that owner would incorrectly reject increment, copy, and
      // move of the iterator itself.  The caller's retained loan continues
      // to protect the actual owner for the duration of the call.
      referenceCarrier, 0, true);
  if (dependency != 0) {
    SemanticLoanInfo loan = *semanticModel.findLoan(id);
    loan.storedDependency = dependency;
    semanticModel.recordLoan(std::move(loan));
  }
  semanticModel.recordBindingLoan(parameter, id);
}

auto SemanticVisitor::Impl::receiverStorageBorrow(
    const ExprPtr &expression) const -> bool {
  if (!expression)
    return false;
  if (const auto *grouping = exprAs<Grouping>(expression.get()))
    return receiverStorageBorrow(grouping->expression());
  const auto borrowedResult = [&](const SemanticType &type) {
    return type.kind == SemanticType::Reference ||
           (type.kind == SemanticType::Class &&
            storedReferencePath(type).has_value());
  };
  const auto borrowedReceiver = [&](const ExprPtr *receiver) {
    const auto *info = receiver == nullptr
                           ? nullptr
                           : semanticModel.findExpression(**receiver);
    return info != nullptr && info->traits.containsBorrowedState;
  };
  // Ordinary accessors without a stored-field mapping borrow their
  // receiver's physical storage. Intrinsics already define their own
  // provenance: extracting a fallible borrowed success, for example,
  // retains its external referent rather than the expected object's shell.
  if (const auto *operation = semanticModel.findOperator(*expression))
    return operation->declaration != nullptr &&
           borrowedResult(operation->returnType) &&
           operation->borrowOrigin == BorrowOriginKind::Receiver &&
           operation->borrowArguments.empty() &&
           operation->borrowFields.empty() &&
           borrowedReceiver(operatorBorrowSource(expression, *operation));
  const auto *call = exprAs<Call>(expression.get());
  const auto *resolved =
      call == nullptr ? nullptr : semanticModel.findCall(*call);
  if (resolved == nullptr || resolved->declaration == nullptr ||
      resolved->intrinsic != IntrinsicKind::None ||
      !borrowedResult(resolved->returnType) ||
      resolved->borrowOrigin != BorrowOriginKind::Receiver ||
      !resolved->borrowArguments.empty() || !resolved->borrowFields.empty())
    return false;
  if (const auto *member = exprAs<Get>(call->callee().get()))
    return borrowedReceiver(&member->object());
  return currentClass &&
         typeTraits(openClassType(*currentClass)).containsBorrowedState;
}

auto SemanticVisitor::Impl::recordRetainedBorrow(const VariableDecl &binding,
                                                 const ExprPtr &expression)
    -> void {
  // A terminal initializer never publishes this binding or a retained loan.
  if (expressionNeverReturns(expression.get()))
    return;
  if (expression && semanticModel.directInitializationSource(*expression)) {
    // A transparent result wrapper has no separate invocation contract or
    // lifetime. Retain the producer's complete field-wise loan, not the
    // wrapper's one-argument construction summary. HIR maps both source
    // expressions to that same value and publication.
    if (const auto *group = exprAs<Grouping>(expression.get()))
      return recordRetainedBorrow(binding, group->expression());
    if (const auto *comma = semanticModel.builtinComma(*expression))
      return recordRetainedBorrow(binding, comma->right());
    if (const auto *call = exprAs<Call>(expression.get()))
      return recordRetainedBorrow(binding, call->arguments().front());
    if (const auto *direct = exprAs<DirectInitializer>(expression.get()))
      return recordRetainedBorrow(binding, direct->arguments().front());
  }
  Symbol *carrier = resolveMutable(binding.name());
  if (carrier == nullptr ||
      carrier->bindingKind != SemanticBindingKind::LocalVariable) {
    return;
  }

  const SymbolId carrierSymbol = toolingSymbolFor(*carrier);
  const ExpressionInfo *info =
      expression ? semanticModel.findExpression(*expression) : nullptr;
  // A fallible borrow (`expected<T&, E>`) is a transparent carrier of its
  // external referent: its binding aliases that referent's loan below, so
  // a reference extracted from it later relays the same caller-owned
  // authority. It does not take a dependency loan of its own here.
  // Reference bindings are published by the checked Reference/reborrow path
  // below. Aggregate field designations describe a value's stored layout;
  // they cannot be copied onto a reference loan to the external referent.
  // Moving a named carrier transfers its exact group; the checked path below
  // continues the source's loan in the destination rather than publishing a
  // second group whose source endpoint would retire the shared storage.
  const auto movedCarrier = expression ? retainedLoanSource(expression)
                                       : std::optional<RetainedLoanSource>{};
  // A construction or call that publishes dependent text re-roots a moved
  // carrier's paths under its own result: a new value, not the carrier's
  // continued loan (L-OWN-02).
  const bool republished = [&] {
    if (!expression)
      return false;
    const Expr *current = expression.get();
    while (const auto *group = exprAs<Grouping>(current))
      current = group->expression() ? group->expression().get() : nullptr;
    if (current == nullptr)
      return false;
    if (const auto *construction = semanticModel.findConstruction(*current);
        construction != nullptr && construction->dependentTextPublication)
      return true;
    if (const auto *contextual =
            semanticModel.findContextualConstruction(*current);
        contextual != nullptr && contextual->dependentTextPublication)
      return true;
    if (const auto *call = exprAs<Call>(current)) {
      if (const auto *payload = semanticModel.findPayloadConstruction(*call);
          payload != nullptr && payload->dependentTextPublication)
        return true;
      if (const auto *resolved = semanticModel.findCall(*call);
          resolved != nullptr &&
          (resolved->dependentTextPublication || resolved->dependentTextReturn))
        return true;
    }
    return false;
  }();
  const bool namedCarrierTransfer =
      !republished && movedCarrier && movedCarrier->transfer &&
      movedCarrier->carrier != nullptr && movedCarrier->loan != 0 &&
      loanFlow.records.contains(movedCarrier->loan);
  if (expression && carrier->type.kind != SemanticType::Reference &&
      !namedCarrierTransfer) {
    const auto *contextual =
        semanticModel.findContextualConstruction(*expression);
    const auto *complete =
        contextual && contextual->contextualBorrowDependencies
            ? &*contextual->contextualBorrowDependencies
            : semanticModel.findBorrowDependencySet(*expression);
    if (complete && complete->dependencies.empty() &&
        !complete->possibleAlternatives.empty()) {
      // A verified selected alternative with no external dependency has no
      // origin-bearing loan to create. Keep the complete value fact on the
      // binding; unknown/missing evidence must still take the checked path.
      return;
    }
    // A dependent text value designates its text fields' owners through the
    // construction or carrier it was produced from (L-OWN-02).
    const auto textDependencies =
        !complete && info != nullptr &&
                (isTextDependentValue(*info) ||
                 contextualTextValue(*expression, carrier->type))
            ? storedBorrowDependencies(expression)
            : std::vector<SemanticLoanDependency>{};
    const auto dependencies =
        complete
            ? std::span<const SemanticLoanDependency>(complete->dependencies)
        : !textDependencies.empty()
            ? std::span<const SemanticLoanDependency>(textDependencies)
            : semanticModel.findBorrowDependencies(*expression);
    if (!dependencies.empty()) {
      // A named shared copy preserves the complete dependency group; it is
      // not a new constructor/call publication. Reuse the existing alias
      // endpoint machinery so every copy keeps each external owner live.
      // Keep the exact field/tag/origin contract, not merely a matching first
      // referent, and never duplicate an exclusive or cleanup-bound carrier.
      const Expr *copyExpression = expression.get();
      while (const auto *group = exprAs<Grouping>(copyExpression))
        copyExpression = group->expression().get();
      const auto *named = exprAs<Variable>(copyExpression);
      const auto *copied = named ? resolvedBorrowVariable(*named) : nullptr;
      const auto *retained = copied && copied->retainedLoan != 0
                                 ? semanticModel.findLoan(copied->retainedLoan)
                                 : nullptr;
      // A copy of a dependent text value aliases its owner dependency, as a
      // copied dynamic view does: both bindings carry the one loan.
      if (copied && retained && !textDependencies.empty() &&
          copied->bindingKind == SemanticBindingKind::LocalVariable &&
          copied->valueState == ValueState::Available &&
          copied->type == carrier->type &&
          retained->kind == SemanticLoanKind::StoredValue && !retained->entry &&
          !retained->lifecycleBound &&
          loanFlow.records.contains(retained->id)) {
        attachLoanCarrier(retained->id, *carrier, carrierSymbol, false);
        semanticModel.recordBindingLoan(binding, retained->id);
        return;
      }
      if (copied && retained && complete && copied->completeBorrowSet &&
          copied->bindingKind == SemanticBindingKind::LocalVariable &&
          copied->valueState == ValueState::Available &&
          copied->type == carrier->type && info && info->traits.copyable &&
          retained->kind == SemanticLoanKind::StoredValue && !retained->entry &&
          loanFlow.records.contains(retained->id) &&
          !retained->lifecycleBound &&
          !lifecycleBoundBorrowCarrierFor(carrier->type) &&
          *complete == *copied->completeBorrowSet &&
          std::all_of(dependencies.begin(), dependencies.end(),
                      [](const auto &dependency) {
                        return dependency.access == AccessMode::ReadOnly;
                      })) {
        std::vector<SemanticLoanDependency> expected{
            {.place = retained->place,
             .designatedFields = retained->designatedFields,
             .access = retained->access,
             .protectsStorage = retained->protectsStorage,
             .parent = retained->parent,
             .conditions = retained->conditions}};
        expected.insert(expected.end(),
                        retained->additionalDependencies.begin(),
                        retained->additionalDependencies.end());
        if (std::equal(expected.begin(), expected.end(), dependencies.begin(),
                       dependencies.end())) {
          attachLoanCarrier(retained->id, *carrier, carrierSymbol, false);
          semanticModel.recordBindingLoan(binding, retained->id);
          return;
        }
      }
      for (const auto &dependency : dependencies)
        reportBorrowConflict(
            expressionToken(expression),
            conflictingLoans(dependency.place, dependency.access,
                             dependency.parent),
            "Cannot retain this value while an overlapping borrow may still "
            "be live.");
      const auto &primary = dependencies.front();
      const auto id = createRetainedLoan(
          *carrier, carrierSymbol, SemanticLoanKind::StoredValue,
          expression.get(), primary.place, primary.place.root,
          primary.place.receiver, primary.access, primary.protectsStorage,
          primary.parent, false, primary.designatedFields, primary.conditions);
      auto loan = *semanticModel.findLoan(id);
      loan.additionalDependencies.assign(dependencies.begin() + 1,
                                         dependencies.end());
      semanticModel.recordLoan(std::move(loan));
      for (const auto &dependency : dependencies) {
        if (dependency.protectsStorage)
          registerStorageLoan(dependency.place.root, dependency.place.receiver,
                              id);
        if (dependency.parent != 0)
          recordSemanticLoanUseForLoan(dependency.parent);
      }
      semanticModel.recordBindingLoan(binding, id);
      return;
    }
  }
  const auto *symbolicRangeCall =
      binding.isRangeIterator() ? exprAs<Call>(expression.get()) : nullptr;
  const ResolvedCallInfo *symbolicRangeResolution =
      symbolicRangeCall == nullptr ? nullptr
                                   : semanticModel.findCall(*symbolicRangeCall);
  const bool symbolicRangeIterator =
      carrier->type.kind == SemanticType::TypeParameter &&
      symbolicRangeResolution != nullptr &&
      symbolicRangeResolution->borrowOrigin != BorrowOriginKind::None;
  const AccessMode carrierAccess =
      symbolicRangeIterator ? symbolicRangeResolution->borrowAccess
      : carrier->type.kind == SemanticType::Reference
          ? carrier->type.referenceAccess
          : borrowAccess(carrier->type);
  if (expression && carrier->type.kind == SemanticType::Reference) {
    const auto complete = completeBorrowDependencySet(expression);
    if (complete && complete->possibleAlternatives.empty() &&
        complete->dependencies.size() > 1 &&
        std::all_of(complete->dependencies.begin(),
                    complete->dependencies.end(), [](const auto &dependency) {
                      return dependency.designatedFields.empty() &&
                             dependency.conditions.empty();
                    })) {
      // A scalar result may select any of several external origins. It has
      // no aggregate field layout, and cannot reuse one input's loan identity.
      // Publish a checked reference loan retaining every possible origin, so
      // ordinary binding uses keep each owner's storage protection live.
      auto dependencies = complete->dependencies;
      for (auto &dependency : dependencies) {
        if (carrierAccess == AccessMode::Mutable &&
            dependency.access != AccessMode::Mutable)
          return; // Ordinary reference binding owns the access diagnostic.
        reportBorrowConflict(
            expressionToken(expression),
            conflictingLoans(dependency.place, carrierAccess,
                             dependency.parent),
            "Cannot retain this reference while an overlapping borrow may "
            "still be live.");
        dependency.access = carrierAccess;
      }
      const auto &primary = dependencies.front();
      const auto id = createRetainedLoan(
          *carrier, carrierSymbol, SemanticLoanKind::Reference,
          expression.get(), primary.place, primary.place.root,
          primary.place.receiver, carrierAccess, primary.protectsStorage,
          primary.parent);
      if (id == 0)
        return;
      auto loan = *semanticModel.findLoan(id);
      loan.additionalDependencies.assign(dependencies.begin() + 1,
                                         dependencies.end());
      semanticModel.recordLoan(std::move(loan));
      for (const auto &dependency : dependencies) {
        if (dependency.protectsStorage)
          registerStorageLoan(dependency.place.root, dependency.place.receiver,
                              id);
        if (dependency.parent != 0)
          recordSemanticLoanUseForLoan(dependency.parent);
      }
      semanticModel.recordBindingLoan(binding, id);
      return;
    }
  }
  const bool retainedStoredBorrow =
      (carrier->type.kind != SemanticType::Reference && info != nullptr &&
       info->traits.containsBorrowedState) ||
      symbolicRangeIterator;
  const std::optional<TransientBorrowProduction> retainedProduction =
      transientBorrowProduction(expression);
  const bool lexicalGlobalMutableBorrow =
      carrierAccess == AccessMode::Mutable && retainedProduction &&
      retainedProduction->globalStorage;
  const auto *construction =
      expression ? semanticModel.findConstruction(*expression) : nullptr;
  const auto *call = exprAs<Call>(expression.get());
  const auto *resolved =
      call == nullptr ? nullptr : semanticModel.findCall(*call);
  const auto storedPath =
      retainedStoredBorrow ? storedReferencePath(carrier->type) : std::nullopt;
  const auto shellOrigin = construction != nullptr ? construction->borrowOrigin
                           : resolved != nullptr   ? resolved->borrowOrigin
                                                   : BorrowOriginKind::None;
  const auto shellArgument = construction != nullptr
                                 ? construction->borrowArgument
                             : resolved != nullptr ? resolved->borrowArgument
                                                   : std::size_t{0};
  const auto *shellParameters =
      construction != nullptr ? &construction->parameterTypes
      : resolved != nullptr && resolved->intrinsic == IntrinsicKind::None
          ? &resolved->parameterTypes
          : nullptr;
  const auto *shellFields = construction != nullptr
                                ? &construction->borrowFields
                            : resolved != nullptr ? &resolved->borrowFields
                                                  : nullptr;
  const auto *shellArguments =
      construction != nullptr ? &construction->borrowArguments
      : resolved != nullptr   ? &resolved->borrowArguments
                              : nullptr;
  if (shellParameters != nullptr && storedPath &&
      shellOrigin == BorrowOriginKind::Argument && shellArguments != nullptr &&
      shellArguments->empty() && shellFields != nullptr &&
      shellFields->size() <= 1 &&
      (shellFields->empty() ||
       (shellFields->front().sourceFields.empty() &&
        shellFields->front().origin == BorrowOriginKind::Argument &&
        shellFields->front().parameter == shellArgument)) &&
      shellArgument < shellParameters->size() &&
      isBorrowedCarrierReference((*shellParameters)[shellArgument]) &&
      storedPath->reference.arguments ==
          (*shellParameters)[shellArgument].arguments) {
    const auto *initializer = exprAs<DirectInitializer>(expression.get());
    const ResolvedCallArguments arguments =
        call != nullptr ? ResolvedCallArguments(*call, resolved)
        : initializer != nullptr
            ? ResolvedCallArguments(initializer->arguments())
            : ResolvedCallArguments{};
    if (shellArgument < arguments.size()) {
      const auto &argument = arguments[shellArgument];
      const auto source = retainedLoanSource(argument);
      const auto *sourceLoan =
          source ? semanticModel.findLoan(source->loan) : nullptr;
      if (sourceLoan != nullptr) {
        // Storing Carrier& borrows the actual carrier object as well as its
        // external owner. It is not a by-value copy of the carrier's view.
        const auto dependency =
            sourceLoan->kind == SemanticLoanKind::StoredValue
                ? sourceLoan->id
                : sourceLoan->storedDependency;
        // Storing the referent's address retains its physical reference even
        // when access is shared. Otherwise its semantic endpoint can retire
        // the named accessor before the adapter and its copied results.
        const auto parent = sourceLoan->kind == SemanticLoanKind::Reference
                                ? sourceLoan->id
                                : SemanticLoanId{0};
        SemanticLoanPlace shell = loanPlace(argument);
        if (dependency != 0 && (shell.root != 0 || shell.receiver)) {
          reportBorrowConflict(
              expressionToken(argument),
              conflictingLoans(shell, carrierAccess, parent),
              "Cannot retain this carrier while an overlapping borrow of its "
              "storage may still be live.");
          const auto owner = shell.root;
          const auto receiver = shell.receiver;
          const auto id = createRetainedLoan(
              *carrier, carrierSymbol, SemanticLoanKind::StoredValue,
              expression.get(), std::move(shell), owner, receiver,
              carrierAccess, true, parent);
          auto loan = *semanticModel.findLoan(id);
          loan.storedDependency = dependency;
          semanticModel.recordLoan(std::move(loan));
          semanticModel.recordBindingLoan(binding, id);
          recordSemanticLoanUseForLoan(dependency);
          return;
        }
      }
    }
  }
  // Multi-path carriers compose their fields' origins. A closure is composed
  // the same way even with one borrowing capture, because its capture is a
  // field of the environment, not the value's own spelling; an ordinary
  // single-path carrier keeps the child-reborrow path below.
  const auto carrierPaths = storedReferencePaths(carrier->type);
  if (composedCarrierPaths(carrierPaths) &&
      ((construction != nullptr && (!construction->borrowArguments.empty() ||
                                    !construction->borrowFields.empty())) ||
       (resolved != nullptr && (!resolved->borrowArguments.empty() ||
                                !resolved->borrowFields.empty())))) {
    auto dependencies = storedBorrowDependencies(expression);
    if (dependencies.empty()) {
      report(expressionToken(expression),
             "Every dependency of a composed borrowed value must have a "
             "checked stable owner.",
             "GTI-S2045");
      return;
    }
    // Consuming a complete carrier transfers its capabilities rather than
    // competing with them. Constructors and ordinary functions use the same
    // exact moved by-value input rule; lvalues and reference parameters still
    // obey copy/reborrow rules. Exclude only that input's loan, not its live
    // children or unrelated overlapping loans.
    std::vector<SemanticLoanId> transferredInputs;
    std::vector<SemanticLoanId> entryReferenceInputs;
    if (construction != nullptr ||
        (resolved != nullptr && resolved->intrinsic == IntrinsicKind::None)) {
      const auto *initializer = exprAs<DirectInitializer>(expression.get());
      const ResolvedCallArguments arguments =
          call != nullptr ? ResolvedCallArguments(*call, resolved)
          : initializer != nullptr
              ? ResolvedCallArguments(initializer->arguments())
              : ResolvedCallArguments{};
      auto parameters = construction != nullptr ? construction->borrowArguments
                                                : resolved->borrowArguments;
      const auto &fields = construction != nullptr ? construction->borrowFields
                                                   : resolved->borrowFields;
      const auto &parameterTypes = construction != nullptr
                                       ? construction->parameterTypes
                                       : resolved->parameterTypes;
      for (const auto &field : fields)
        if (field.origin == BorrowOriginKind::Argument &&
            std::find(parameters.begin(), parameters.end(), field.parameter) ==
                parameters.end())
          parameters.push_back(field.parameter);
      if (arguments.size() != 0)
        for (const auto parameter : parameters) {
          if (parameter < arguments.size() &&
              parameter < parameterTypes.size() &&
              parameterTypes[parameter].kind == SemanticType::Reference) {
            const auto source = retainedLoanSource(arguments[parameter]);
            const auto *loan =
                source ? semanticModel.findLoan(source->loan) : nullptr;
            // An exact reference formal is the callee's entry permission,
            // not a competing borrow of its own referent. Argument checking
            // has already validated this reborrow, including any live child.
            // These entry capabilities last through the whole invocation;
            // the new composed loan still protects each place from parent
            // access for its own lifetime. Never exempt local children or a
            // carrier's unrelated dependency merely because it overlaps.
            if (loan != nullptr && loan->entry &&
                loan->kind == SemanticLoanKind::Reference &&
                loan->storedDependency == 0 &&
                loan->place == retainedLoanPlace(arguments[parameter]))
              entryReferenceInputs.push_back(loan->id);
          }
          if (parameter < arguments.size() &&
              parameter < parameterTypes.size() &&
              (parameterTypes[parameter].kind == SemanticType::Class ||
               parameterTypes[parameter].kind == SemanticType::Lambda) &&
              semanticModel.typeOf(*arguments[parameter]) ==
                  parameterTypes[parameter])
            if (const auto source = retainedLoanSource(arguments[parameter]);
                source && source->transfer)
              transferredInputs.push_back(source->loan);
        }
    }
    // The composed value's own source loan (a call result or constructed
    // temporary) and its ancestors, including the transient receiver
    // reborrows of the producing call, are the reborrow being retained, not
    // competing borrows of the owner; they are exempt as for any reborrow.
    const auto composedOrigin = retainedLoanSource(expression);
    const SemanticLoanId composedThrough =
        composedOrigin ? composedOrigin->loan : SemanticLoanId{0};
    for (const auto &dependency : dependencies) {
      auto conflicts = conflictingLoans(
          dependency.place, dependency.access,
          composedThrough != 0 ? composedThrough : dependency.parent);
      std::erase_if(conflicts, [&](SemanticLoanId id) {
        return std::find(transferredInputs.begin(), transferredInputs.end(),
                         id) != transferredInputs.end() ||
               std::find(entryReferenceInputs.begin(),
                         entryReferenceInputs.end(),
                         id) != entryReferenceInputs.end();
      });
      reportBorrowConflict(
          expressionToken(expression), conflicts,
          "Cannot compose borrowed values while an overlapping mutable "
          "borrow may still be live.");
    }
    // Several fields can share one cleanup-bearing input's external loan.
    // Retain that ancestry just as for a single-field carrier; recording all
    // field identities must not sever the input's lifetime dependency.
    SemanticLoanId parent = 0;
    const auto origin = retainedLoanSource(expression);
    const auto *originLoan =
        origin ? semanticModel.findLoan(origin->loan) : nullptr;
    const auto *fields = construction != nullptr ? &construction->borrowFields
                                                 : &resolved->borrowFields;
    const auto originKind = construction != nullptr ? construction->borrowOrigin
                                                    : resolved->borrowOrigin;
    const auto argument = construction != nullptr ? construction->borrowArgument
                                                  : resolved->borrowArgument;
    const auto &parameters = construction != nullptr
                                 ? construction->borrowArguments
                                 : resolved->borrowArguments;
    if (originLoan != nullptr && !origin->transfer &&
        originLoan->kind == SemanticLoanKind::StoredValue &&
        originLoan->access == AccessMode::ReadOnly &&
        lifecycleBoundBorrowCarrierFor(carrier->type) &&
        originKind == BorrowOriginKind::Argument &&
        std::all_of(parameters.begin(), parameters.end(),
                    [&](auto parameter) { return parameter == argument; }) &&
        std::all_of(fields->begin(), fields->end(), [&](const auto &field) {
          return field.origin == BorrowOriginKind::Argument &&
                 field.parameter == argument;
        }))
      parent = originLoan->id;
    const auto primary = dependencies.front();
    if (originLoan != nullptr && !origin->transfer && originLoan->entry &&
        originLoan->kind == SemanticLoanKind::Reference &&
        originLoan->storedDependency == 0 &&
        originLoan->access == AccessMode::Mutable &&
        originLoan->place == primary.place)
      // The primary reference is still a reborrow of its exact entry
      // permission. The other fields retain independent origins; they are
      // not projections under this parent.
      parent = originLoan->id;
    const SemanticLoanId id = createRetainedLoan(
        *carrier, carrierSymbol, SemanticLoanKind::StoredValue,
        expression.get(), primary.place, primary.place.root,
        primary.place.receiver, primary.access, primary.protectsStorage, parent,
        false, primary.designatedFields, primary.conditions);
    SemanticLoanInfo loan = *semanticModel.findLoan(id);
    loan.additionalDependencies.assign(dependencies.begin() + 1,
                                       dependencies.end());
    semanticModel.recordLoan(std::move(loan));
    for (const auto &dependency : dependencies) {
      if (dependency.protectsStorage) {
        registerStorageLoan(dependency.place.root, dependency.place.receiver,
                            id);
      }
    }
    semanticModel.recordBindingLoan(binding, id);
    return;
  }
  auto source = retainedLoanSource(expression);
  if (binding.isRangeIterator() && resolved != nullptr &&
      resolved->borrowOrigin == BorrowOriginKind::Receiver)
    if (const auto *member = exprAs<Get>(call->callee().get()))
      // A synthesized cursor retains the range loan, not the external field
      // selected by the ordinary begin() result's return contract.
      source = retainedLoanSource(member->object());
  if (source) {
    const SemanticLoanInfo *sourceLoan = semanticModel.findLoan(source->loan);
    // Consuming a carrier can transfer its owned shell, but extracting a
    // reference publishes a distinct checked child of its external loan.
    // Reusing the StoredValue identity would attach the old field layout to
    // a scalar reference and could retire the owner's authority too early.
    if (sourceLoan != nullptr &&
        carrier->type.kind == SemanticType::Reference &&
        sourceLoan->kind == SemanticLoanKind::StoredValue)
      source->transfer = false;
    const bool storedReceiverShell = retainedStoredBorrow &&
                                     !binding.isRangeIterator() &&
                                     receiverStorageBorrow(expression);
    if (sourceLoan != nullptr && retainedStoredBorrow &&
        !binding.isRangeIterator() && !storedReceiverShell &&
        sourceLoan->kind == SemanticLoanKind::Reference &&
        sourceLoan->storedDependency != 0) {
      // Copying a view out of a reference copies its external dependency,
      // not a dependency on the source view object's physical storage.
      const auto *dependency =
          semanticModel.findLoan(sourceLoan->storedDependency);
      if (dependency != nullptr && dependency->access == AccessMode::ReadOnly)
        sourceLoan = dependency;
    }
    if (sourceLoan == nullptr) {
      return;
    }
    if (sourceLoan->referenceSelection &&
        carrier->type.kind == SemanticType::Reference) {
      const auto selectedPlace = semanticPlace(expression);
      auto dependencies = selectedReferenceDependencies(selectedPlace);
      if (!dependencies || !selectedPlace.projections.empty() ||
          source->transfer) {
        report(expressionToken(expression),
               "A selected reference can currently be reborrowed only at "
               "the same referent; projected or consuming aliases require "
               "an exact checked address and dependency transfer.",
               "GTI-S2054");
        return;
      }
      const auto origin = *sourceLoan;
      for (auto &dependency : *dependencies) {
        if (carrierAccess == AccessMode::Mutable &&
            dependency.access != AccessMode::Mutable)
          return; // Ordinary reference binding owns the access diagnostic.
        reportBorrowConflict(
            expressionToken(expression),
            conflictingLoans(dependency.place, carrierAccess, origin.id),
            "Cannot reborrow this selected reference while an overlapping "
            "child borrow may still be live.");
        dependency.access = carrierAccess;
      }
      const auto &primary = dependencies->front();
      const auto child = createRetainedLoan(
          *carrier, carrierSymbol, SemanticLoanKind::Reference,
          expression.get(), primary.place, primary.place.root,
          primary.place.receiver, carrierAccess, primary.protectsStorage,
          origin.id);
      if (child == 0)
        return;
      auto loan = *semanticModel.findLoan(child);
      loan.referenceSelection = origin.referenceSelection;
      loan.additionalDependencies.assign(dependencies->begin() + 1,
                                         dependencies->end());
      semanticModel.recordLoan(std::move(loan));
      for (const auto &dependency : *dependencies)
        if (dependency.protectsStorage)
          registerStorageLoan(dependency.place.root, dependency.place.receiver,
                              child);
      semanticModel.recordBindingLoan(binding, child);
      return;
    }
    const auto childConflicts = [&](const SemanticLoanPlace &place) {
      auto conflicts = conflictingLoans(place, carrierAccess, sourceLoan->id);
      const auto *binding = source->carrier == nullptr
                                ? nullptr
                                : resolve(source->carrier->name());
      const auto *alias = binding == nullptr
                              ? nullptr
                              : semanticModel.findLoan(binding->retainedLoan);
      if (source->dependency && alias != nullptr &&
          alias->kind == SemanticLoanKind::Reference &&
          alias->storedDependency == sourceLoan->id) {
        // A mapped external reborrow is made through this exact reference
        // alias. Its physical shell is not a competing borrow of the stored
        // referent. Unrelated aliases, even to the same owner, remain checked.
        std::erase_if(conflicts, [&](SemanticLoanId conflict) {
          return loanIsAncestor(conflict, alias->id);
        });
      }
      return conflicts;
    };
    if (carrier->type.kind == SemanticType::Reference && resolved != nullptr &&
        resolved->restoresReferenceResult &&
        !resolved->restoredBorrowFields.empty()) {
      // Restoration changes the referent's visible dependency layout. Even a
      // shared result is a distinct reference publication, not another name
      // for the opaque parameter. Retain its checked witness explicitly so
      // later aliases and helper returns preserve the same nested authority.
      if (carrierAccess == AccessMode::Mutable &&
          sourceLoan->access != AccessMode::Mutable)
        return;
      const auto origin = *sourceLoan;
      const auto dependency =
          origin.kind == SemanticLoanKind::StoredValue
              ? origin.id
              : (origin.storedDependency != 0 ? origin.storedDependency
                                              : origin.id);
      reportBorrowConflict(
          expressionToken(expression), childConflicts(origin.place),
          "Cannot restore this reference while an overlapping child borrow "
          "may still be live.");
      const auto child = createRetainedLoan(
          *carrier, carrierSymbol, SemanticLoanKind::Reference,
          expression.get(), origin.place, origin.owner, origin.receiverOrigin,
          carrierAccess, true, origin.id);
      auto loan = *semanticModel.findLoan(child);
      loan.storedDependency = dependency;
      semanticModel.recordLoan(std::move(loan));
      semanticModel.recordBindingLoan(binding, child);
      recordSemanticLoanUseForLoan(dependency);
      return;
    }
    if (source->dependency && carrier->type.kind == SemanticType::Reference) {
      const auto &selected = *source->dependency;
      if (carrierAccess == AccessMode::Mutable &&
          selected.access != AccessMode::Mutable)
        return; // Reference binding owns the permission diagnostic.
      reportBorrowConflict(
          expressionToken(expression), childConflicts(selected.place),
          "Cannot create a child reborrow while another overlapping child "
          "loan may still be live.");
      const auto storedDependency = sourceLoan->storedDependency;
      const auto child = createRetainedLoan(
          *carrier, carrierSymbol, SemanticLoanKind::Reference,
          expression.get(), selected.place, selected.place.root,
          selected.place.receiver, carrierAccess, true, sourceLoan->id);
      if (storedDependency != 0) {
        auto loan = *semanticModel.findLoan(child);
        loan.storedDependency = storedDependency;
        semanticModel.recordLoan(std::move(loan));
      }
      semanticModel.recordBindingLoan(binding, child);
      return;
    }
    if (((carrier->type.kind == SemanticType::Reference &&
          carrier->type.arguments.size() == 1 &&
          (typeTraits(carrier->type.arguments.front()).containsBorrowedState ||
           isBorrowedCarrierReference(carrier->type) ||
           receiverStorageBorrow(expression)) &&
          sourceLoan->kind == SemanticLoanKind::StoredValue) ||
         (storedReceiverShell &&
          (sourceLoan->kind == SemanticLoanKind::StoredValue ||
           sourceLoan->storedDependency != 0))) &&
        !source->transfer) {
      // Referencing the carrier is not a copy of its stored dependency.
      // Protect the addressable shell and retain its existing external-owner
      // authority through a separate edge. Neither permission upgrades the
      // other; ordinary reference binding already checks shell access.
      // Opaque checked erasure hides fields, not the original object's
      // identity: it must retain both its shell and its nested dependencies.
      SemanticLoanPlace shell = retainedLoanPlace(expression);
      if (shell.root == 0 && !shell.receiver) {
        report(expressionToken(expression),
               "A reference to borrowed-state storage requires a stable "
               "carrier object.",
               "GTI-S2017");
        return;
      }
      const SemanticLoanId dependency =
          sourceLoan->kind == SemanticLoanKind::StoredValue
              ? sourceLoan->id
              : sourceLoan->storedDependency;
      const SemanticLoanId parent =
          sourceLoan->kind == SemanticLoanKind::Reference &&
                  sourceLoan->access == AccessMode::Mutable
              ? sourceLoan->id
              : 0;
      reportBorrowConflict(
          expressionToken(expression),
          conflictingLoans(shell, carrierAccess, parent),
          "Cannot borrow this carrier while an overlapping borrow of its "
          "storage may still be live.");
      const SymbolId owner = shell.root;
      const bool receiver = shell.receiver;
      const SemanticLoanId reference =
          createRetainedLoan(*carrier, carrierSymbol,
                             storedReceiverShell ? SemanticLoanKind::StoredValue
                                                 : SemanticLoanKind::Reference,
                             expression.get(), std::move(shell), owner,
                             receiver, carrierAccess, true, parent);
      SemanticLoanInfo loan = *semanticModel.findLoan(reference);
      loan.storedDependency = dependency;
      semanticModel.recordLoan(std::move(loan));
      semanticModel.recordBindingLoan(binding, reference);
      recordSemanticLoanUseForLoan(dependency);
      return;
    }
    if (source->transfer) {
      const bool pathConditionalTransfer =
          !loanFlow.activeConditionalArms.empty() ||
          !loanFlow.activeSwitchArms.empty() ||
          std::any_of(valueControlFlow.begin(), valueControlFlow.end(),
                      [](const ValueControlFlowContext &context) {
                        return context.kind == ValueControlFlowKind::Loop;
                      });
      if (!pathConditionalTransfer && source->carrier != nullptr) {
        if (Symbol *sourceCarrier = resolveMutable(source->carrier->name())) {
          const SymbolId sourceSymbol = toolingSymbolFor(*sourceCarrier);
          loanFlow.carrierLoans.erase(sourceSymbol);
          if (auto flow = loanFlow.records.find(source->loan);
              flow != loanFlow.records.end()) {
            flow->second.carriers.erase(sourceSymbol);
          }
          if (sourceLoan->lifecycleBound ||
              sourceLoan->access == AccessMode::Mutable ||
              std::any_of(sourceLoan->additionalDependencies.begin(),
                          sourceLoan->additionalDependencies.end(),
                          [](const auto &dependency) {
                            return dependency.access == AccessMode::Mutable;
                          })) {
            semanticModel.removeLoanCarrier(source->loan, sourceSymbol);
            // The active flow/carrier set above forgets the consumed shell.
            // Keep a cleanup-bound declaration's original loan identity in
            // the immutable model: MIR lowers that initialization before the
            // later move, and earlier shell references still depend on it.
            // This does not restore availability of the moved-from binding.
            if (!sourceLoan->lifecycleBound &&
                sourceCarrier->variableDeclaration != nullptr) {
              semanticModel.recordBindingLoan(
                  *sourceCarrier->variableDeclaration, 0);
            }
            if (!sourceLoan->lifecycleBound &&
                sourceCarrier->parameterDeclaration != nullptr) {
              semanticModel.recordBindingLoan(
                  *sourceCarrier->parameterDeclaration, 0);
            }
          }
          sourceCarrier->retainedLoan = 0;
        }
      }
      attachLoanCarrier(source->loan, *carrier, carrierSymbol, true);
      semanticModel.recordBindingLoan(binding, source->loan);
      return;
    }
    if (retainedStoredBorrow && binding.isRangeIterator() &&
        sourceLoan->access == AccessMode::Mutable &&
        carrierAccess == AccessMode::Mutable) {
      attachLoanCarrier(sourceLoan->id, *carrier, carrierSymbol, false);
      semanticModel.recordBindingLoan(binding, sourceLoan->id);
      return;
    }
    // A single-path carrier may retain a child of an exclusive dependency
    // with either access mode. A shared child does not copy the exclusive
    // capability: the normal child-loan path below suspends its parent.
    // Copies of that shared child alias the child loan, keeping its parent
    // suspended until every alias reaches its checked endpoint.
    const bool exactStoredReborrow =
        retainedStoredBorrow && isDirectStoredReferenceType(carrier->type) &&
        (sourceLoan->access == AccessMode::Mutable ||
         carrierAccess == AccessMode::ReadOnly);
    // A shared ancestry (a view of a view of a shared element) retains no
    // exclusive capability; only an exclusive source or an exclusive
    // ancestor is the mutable reborrow this value cannot carry.
    const auto mutableAncestry = [&]() {
      std::unordered_set<SemanticLoanId> seen;
      for (SemanticLoanId current = sourceLoan->parent;
           current != 0 && seen.insert(current).second;) {
        const auto *ancestor = semanticModel.findLoan(current);
        if (ancestor == nullptr)
          return false;
        if (ancestor->access == AccessMode::Mutable)
          return true;
        current = ancestor->parent;
      }
      return false;
    };
    if (retainedStoredBorrow && !exactStoredReborrow &&
        (sourceLoan->access == AccessMode::Mutable || mutableAncestry())) {
      report(expressionToken(expression),
             "A local borrowed-state value cannot retain a mutable parent "
             "or child reborrow.",
             "GTI-S2017");
      diagnostics.back().hints.emplace_back(
          "Use the borrowed-state value only as a non-retained temporary, "
          "or borrow from ordinary read-only owner storage.");
      return;
    }
    if (sourceLoan->access == AccessMode::ReadOnly &&
        carrierAccess == AccessMode::Mutable) {
      const auto sharedArgument = [](const auto &record) {
        return record.borrowOrigin == BorrowOriginKind::Argument &&
               record.borrowArgument < record.parameterTypes.size() &&
               record.parameterTypes[record.borrowArgument].kind ==
                   SemanticType::Reference &&
               record.parameterTypes[record.borrowArgument].referenceAccess ==
                   AccessMode::ReadOnly;
      };
      const bool ordinaryCall =
          resolved != nullptr && resolved->function != 0 &&
          resolved->declaration != nullptr && resolved->declaration->body() &&
          !resolved->declaration->isVirtual() &&
          !resolved->declaration->runtimeBinding() &&
          !resolved->nativeCalleeType && !resolved->callableBoundary &&
          resolved->intrinsic == IntrinsicKind::None &&
          resolved->returnType == carrier->type &&
          resolved->borrowAccess == AccessMode::Mutable &&
          resolved->borrowArguments.empty() && resolved->borrowFields.empty() &&
          ((resolved->borrowOrigin == BorrowOriginKind::Receiver &&
            resolved->declaration->receiverMutability() ==
                ReceiverMutability::ReadOnly) ||
           sharedArgument(*resolved));
      const bool ordinaryConstruction =
          construction != nullptr && construction->constructor != 0 &&
          construction->declaration != nullptr &&
          construction->declaration->body() &&
          construction->constructedType == carrier->type &&
          construction->borrowAccess == AccessMode::Mutable &&
          construction->borrowArguments.empty() &&
          construction->borrowFields.empty() && sharedArgument(*construction);
      const bool publishedExternalExclusion =
          retainedStoredBorrow && sourceLoan->entry &&
          sourceLoan->kind == SemanticLoanKind::StoredValue &&
          !sourceLoan->protectsStorage && storedPath &&
          storedPath->conditions.empty() &&
          lifecycleBoundBorrowCarrierFor(carrier->type) && source->dependency &&
          source->dependency->access == AccessMode::ReadOnly &&
          source->dependency->conditions.empty() &&
          !source->dependency->designatedFields.empty() &&
          (ordinaryCall || ordinaryConstruction);
      if (publishedExternalExclusion) {
        const auto parent = sourceLoan->id;
        const auto place = source->dependency->place;
        const auto token = expressionToken(expression);
        auto conflicts = childConflicts(place);
        // The selected call can already have reported this exact loan at
        // this source token. Preserve that cause rather than duplicating it
        // when its result is retained in the same statement.
        std::erase_if(conflicts, [&](SemanticLoanId conflict) {
          return std::any_of(
              loanFlow.conflicts.begin(), loanFlow.conflicts.end(),
              [&](const PendingBorrowConflict &pending) {
                return pending.statement == loanFlow.currentStatement &&
                       pending.region == loanFlow.currentRegion &&
                       pending.order == loanFlow.currentOrder &&
                       pending.diagnostic.primary == tokenSpan(token) &&
                       std::find(pending.loans.begin(), pending.loans.end(),
                                 conflict) != pending.loans.end();
              });
        });
        reportBorrowConflict(
            token, conflicts,
            "Cannot retain this value while an overlapping borrow may still "
            "be live.");
        const auto child = createRetainedLoan(
            *carrier, carrierSymbol, SemanticLoanKind::StoredValue,
            expression.get(), place, place.root, place.receiver,
            AccessMode::Mutable, false, parent);
        semanticModel.recordBindingLoan(binding, child);
        recordSemanticLoanUseForLoan(parent);
      }
      // Ordinary reference upgrades remain owned by validateReferenceBinding.
      // The checked stored result above has distinct callee-granted exclusion;
      // its parent retains its actual shared input permission.
      return;
    }
    // A value containing a reference is a distinct carrier publication, not
    // another spelling of the reference itself. Retain its StoredValue loan
    // below even for shared inputs, so a later reference to that carrier can
    // protect its physical storage independently from its external referent.
    // Range-for bindings (range owner, cursor, element) keep the shared path
    // and their ancestry: the range verifier pairs the element loan with the
    // range loan and requires the cursor to carry no binding of its own.
    const bool rangeForBinding =
        binding.isRangeBinding() || binding.isRangeOwner() ||
        binding.isRangeIterator() || binding.isRangeElementOwner();
    // A composed carrier (a closure environment, an erased referent) and a
    // carrier constructed here from the shared input take that distinct
    // publication: the new object has its own storage and dependency. A
    // carrier that arrives as a call result or by name (a fallible borrow,
    // a view) keeps the shared path, aliasing its external referent's loan.
    if (sourceLoan->access == AccessMode::ReadOnly &&
        !(retainedStoredBorrow &&
          sourceLoan->kind == SemanticLoanKind::Reference && !rangeForBinding &&
          (construction != nullptr ||
           composedCarrierPaths(storedReferencePaths(carrier->type))))) {
      if (lifecycleBoundBorrowCarrierFor(carrier->type) ||
          sourceLoan->lifecycleBound) {
        // A new cleanup-bearing value owns a distinct release obligation even
        // when its external referent already has a shared parameter loan.
        // Conversely, a child view of such a value borrows its obligation;
        // it must not become another carrier that can retire the same loan.
        const auto sourceRecord = *sourceLoan;
        const auto place =
            source->dependency ? source->dependency->place : sourceRecord.place;
        const auto id = createRetainedLoan(
            *carrier, carrierSymbol,
            retainedStoredBorrow ? SemanticLoanKind::StoredValue
                                 : SemanticLoanKind::Reference,
            expression.get(), place, place.root, place.receiver, carrierAccess,
            source->dependency ? source->dependency->protectsStorage
                               : sourceRecord.protectsStorage,
            sourceRecord.id);
        if (sourceRecord.storedDependency != 0) {
          auto loan = *semanticModel.findLoan(id);
          loan.storedDependency = sourceRecord.storedDependency;
          semanticModel.recordLoan(std::move(loan));
        }
        semanticModel.recordBindingLoan(binding, id);
        return;
      }
      if (retainedStoredBorrow && isDirectStoredReferenceType(carrier->type) &&
          sourceLoan->entry &&
          sourceLoan->kind == SemanticLoanKind::StoredValue &&
          resolved != nullptr &&
          resolved->borrowOrigin != BorrowOriginKind::None) {
        // A method-produced view has its own value publication. It retains
        // the shared external owner, but is not a Load/Move copy of the
        // parameter's carrier shell (its offset/length may differ).
        const auto id = createRetainedLoan(
            *carrier, carrierSymbol, SemanticLoanKind::StoredValue,
            expression.get(), sourceLoan->place, sourceLoan->owner,
            sourceLoan->receiverOrigin, AccessMode::ReadOnly,
            sourceLoan->protectsStorage);
        semanticModel.recordBindingLoan(binding, id);
        return;
      }
      attachLoanCarrier(sourceLoan->id, *carrier, carrierSymbol, false);
      semanticModel.recordBindingLoan(binding, sourceLoan->id);
      return;
    }

    SemanticLoanPlace childPlace = retainedLoanPlace(expression);
    const auto *rangeElement = dynamic_cast<const Unary *>(expression.get());
    if (carrierAccess == AccessMode::Mutable && rangeElement != nullptr &&
        rangeElement->oper().kind == TokenKind::STAR &&
        rangeElement->rangeObject()) {
      // A mutable range element is a child of the exclusive range loan,
      // even though its runtime address is produced through the iterator.
      // Keep semantic overlap authority rooted at the protected owner; MIR
      // separately retains the concrete dereference result address.
      childPlace = sourceLoan->place;
    }
    if (childPlace.root == 0 && !childPlace.receiver) {
      childPlace = sourceLoan->place;
    }
    reportBorrowConflict(
        expressionToken(expression), childConflicts(childPlace),
        "Cannot create a child reborrow while another overlapping child "
        "loan may still be live.");
    const SemanticLoanKind kind = retainedStoredBorrow
                                      ? SemanticLoanKind::StoredValue
                                      : SemanticLoanKind::Reference;
    const SemanticLoanId dependency = sourceLoan->storedDependency;
    // A returned adapter's field identity belongs to its result layout, not
    // its exclusive parent's layout. Keep the selected origin and permission
    // while publishing that exact destination path on the child loan.
    const auto dependencies = retainedStoredBorrow
                                  ? storedBorrowDependencies(expression)
                                  : std::vector<SemanticLoanDependency>{};
    const auto fields = dependencies.size() == 1
                            ? dependencies.front().designatedFields
                            : std::vector<SymbolId>{};
    // A root shared reference can be copied into an independent value
    // dependency. A child or cleanup-bound reference still retains ancestry:
    // copying its read permission cannot release an exclusive ancestor or
    // extend a cleanup-protected lifetime. Range-for bindings always keep
    // their ancestry so the element loan stays under its range loan.
    // Only a shared reference to an opaque erased object is published as an
    // independent root: erasure hides the referent's layout and the copy's
    // read permission protects the shell on its own. Every other carrier
    // keeps its ancestry, as MIR expects when it pairs the binding's loan
    // with the producing call's result chain.
    const auto *opaqueReferent =
        carrier->type.kind == SemanticType::Reference &&
                carrier->type.arguments.size() == 1
            ? classInfo(carrier->type.arguments.front())
            : nullptr;
    const bool opaqueCarrier = opaqueReferent != nullptr &&
                               opaqueReferent->compilerCapability ==
                                   CompilerCapabilityTypeKind::ErasedObject;
    // A carrier built directly from a caller-owned shared root (its
    // constructor arguments name the entry reference itself) or from an
    // opaque erased referent publishes an independent root: the copy's read
    // permission protects its referent on its own. A carrier built through
    // an accessor call, or from a body-local source such as a range loan,
    // keeps its ancestry, which MIR pairs with the producing call chain.
    const auto directCarrierSource = [&](const auto &self,
                                         const Expr *candidate) -> bool {
      while (const auto *group = exprAs<Grouping>(candidate))
        candidate = group->expression().get();
      if (exprAs<Variable>(candidate) != nullptr)
        return true;
      // A call that returns its carrier by value (an expected result, a
      // view) publishes an independent root; only a reference-returning
      // accessor keeps the chain from its result to the entry.
      if (const auto *call = exprAs<Call>(candidate);
          call != nullptr &&
          semanticModel.findConstruction(*candidate) == nullptr) {
        const auto intrinsic = intrinsicCandidate(call->callee());
        if (!(intrinsic &&
              intrinsic->record().intrinsic == IntrinsicKind::Move)) {
          const auto *resolved = semanticModel.findCall(*call);
          return resolved != nullptr &&
                 resolved->returnType.kind != SemanticType::Reference;
        }
      }
      const auto *arguments = [&]() -> const std::vector<ExprPtr> * {
        if (const auto *call = exprAs<Call>(candidate))
          return &call->arguments();
        if (const auto *direct = exprAs<DirectInitializer>(candidate))
          return &direct->arguments();
        return nullptr;
      }();
      return arguments != nullptr && !arguments->empty() &&
             std::all_of(arguments->begin(), arguments->end(),
                         [&](const ExprPtr &argument) {
                           return self(self, argument.get());
                         });
    };
    // A carrier whose cleanup reads its referent keeps its ancestry too: the
    // referent must outlive the carrier's destructor, which MIR proves
    // through the paired parent loan.
    const auto parent =
        sourceLoan->access == AccessMode::ReadOnly && sourceLoan->parent == 0 &&
                !sourceLoan->lifecycleBound &&
                !lifecycleBoundBorrowCarrierFor(carrier->type) &&
                !rangeForBinding &&
                ((sourceLoan->entry &&
                  directCarrierSource(directCarrierSource, expression.get())) ||
                 opaqueCarrier)
            ? SemanticLoanId{0}
            : sourceLoan->id;
    const SemanticLoanId child = createRetainedLoan(
        *carrier, carrierSymbol, kind, expression.get(), std::move(childPlace),
        sourceLoan->owner, sourceLoan->receiverOrigin, carrierAccess,
        sourceLoan->protectsStorage ||
            (source->dependency && sourceLoan->access == AccessMode::Mutable),
        parent, false, fields);
    if (dependency != 0) {
      SemanticLoanInfo loan = *semanticModel.findLoan(child);
      loan.storedDependency = dependency;
      semanticModel.recordLoan(std::move(loan));
    }
    semanticModel.recordBindingLoan(binding, child);
    return;
  }

  SemanticLoanPlace place = retainedLoanPlace(expression);
  const Variable *owner = borrowedOwnerVariable(expression);
  const bool receiverOrigin =
      owner == nullptr && currentClass && isReceiverDerivedBorrow(expression);
  if (owner == nullptr && !receiverOrigin && place.root == 0 &&
      !place.receiver) {
    return;
  }

  Symbol *ownerSymbol =
      owner == nullptr ? nullptr : resolvePlaceSymbol(owner->name());
  const bool stableStorage = hasStableBorrowStorage(expression);
  const bool protectsStorage = stableStorage;
  const SymbolId ownerId =
      ownerSymbol == nullptr ? place.root : toolingSymbolFor(*ownerSymbol);
  if (place.root == 0 && !place.receiver) {
    place.root = ownerId;
    place.receiver = receiverOrigin;
  }
  reportBorrowConflict(
      expressionToken(expression), conflictingLoans(place, carrierAccess),
      carrierAccess == AccessMode::Mutable
          ? "Cannot create a mutable borrow while an overlapping borrow "
            "may still be live."
          : "Cannot create a read-only borrow while an overlapping mutable "
            "borrow may still be live.");
  const SemanticLoanId id =
      createRetainedLoan(*carrier, carrierSymbol,
                         retainedStoredBorrow ? SemanticLoanKind::StoredValue
                                              : SemanticLoanKind::Reference,
                         expression.get(), std::move(place), ownerId,
                         receiverOrigin, carrierAccess, protectsStorage);
  if (lexicalGlobalMutableBorrow) {
    loanFlow.records.at(id).confinedToRegion = false;
  }
  semanticModel.recordBindingLoan(binding, id);
}

auto SemanticVisitor::Impl::recordSemanticLoanUseForLoan(SemanticLoanId loan)
    -> void {
  const auto found = loanFlow.records.find(loan);
  if (found == loanFlow.records.end()) {
    return;
  }
  LoanFlowRecord &flow = found->second;
  if (loanFlow.currentRegion == 0 || loanFlow.currentStatement == nullptr) {
    flow.confinedToRegion = false;
    return;
  }
  loanFlow.uses.push_back({.loan = loan,
                           .sequence = loanFlow.nextSequence++,
                           .region = loanFlow.currentRegion,
                           .order = loanFlow.currentOrder,
                           .statement = loanFlow.currentStatement,
                           .path = loanFlow.activeConditionalArms,
                           .switchPath = loanFlow.activeSwitchArms});
  if (loanFlow.currentRegion == flow.region &&
      loanFlow.currentOrder >= flow.lastUseOrder) {
    flow.lastUseOrder = loanFlow.currentOrder;
    flow.lastUse = loanFlow.currentStatement;
    return;
  }

  // Choose the outermost matching loop before a nested switch or
  // conditional. A pre-existing owner dependency must survive the loop
  // backedge, and a switch-local `break` must not be mistaken for that loop
  // exit. Loans created inside the loop fail the firstNestedLoan test and
  // may instead project to a nested structured control statement.
  const auto loopBoundary =
      std::find_if(loanFlow.activeLoops.begin(), loanFlow.activeLoops.end(),
                   [&](std::size_t index) {
                     if (index >= loanFlow.loops.size()) {
                       return false;
                     }
                     const LoanFlowLoop &candidate = loanFlow.loops[index];
                     return candidate.region == flow.region &&
                            candidate.statement != nullptr &&
                            loan < candidate.firstNestedLoan;
                   });
  if (loopBoundary != loanFlow.activeLoops.end()) {
    const LoanFlowLoop &loop = loanFlow.loops[*loopBoundary];
    if (loop.order >= flow.lastUseOrder) {
      flow.lastUseOrder = loop.order;
      flow.lastUse = loop.statement;
    }
    return;
  }

  const auto statementBoundary = std::find_if(
      loanFlow.statementBoundaries.begin(), loanFlow.statementBoundaries.end(),
      [&](const LoanFlowStatementBoundary &candidate) {
        return candidate.region == flow.region;
      });
  if (statementBoundary != loanFlow.statementBoundaries.end() &&
      statementBoundary->statement != nullptr) {
    if (statementBoundary->order >= flow.lastUseOrder) {
      flow.lastUseOrder = statementBoundary->order;
      flow.lastUse = statementBoundary->statement;
    }
    return;
  }

  const auto switchBoundary = std::find_if(
      loanFlow.switchBoundaries.begin(), loanFlow.switchBoundaries.end(),
      [&](const LoanFlowSwitchBoundary &candidate) {
        return candidate.region == flow.region;
      });
  if (switchBoundary != loanFlow.switchBoundaries.end() &&
      switchBoundary->statement != nullptr) {
    if (switchBoundary->order >= flow.lastUseOrder) {
      flow.lastUseOrder = switchBoundary->order;
      flow.lastUse = switchBoundary->statement;
    }
    return;
  }

  flow.confinedToRegion = false;
}

auto SemanticVisitor::Impl::recordSemanticLoanUse(SymbolId carrier) -> void {
  const auto retained = loanFlow.carrierLoans.find(carrier);
  if (retained == loanFlow.carrierLoans.end()) {
    return;
  }
  recordSemanticLoanAuthorityUse(retained->second);
}

auto SemanticVisitor::Impl::recordSemanticLoanAuthorityUse(SemanticLoanId root)
    -> void {
  std::vector<SemanticLoanId> pending{root};
  std::unordered_set<SemanticLoanId> visited;
  while (!pending.empty()) {
    const SemanticLoanId loan = pending.back();
    pending.pop_back();
    if (loan == 0 || !visited.insert(loan).second) {
      continue;
    }
    recordSemanticLoanUseForLoan(loan);
    const SemanticLoanInfo *info = semanticModel.findLoan(loan);
    if (info != nullptr) {
      pending.push_back(info->parent);
      pending.push_back(info->storedDependency);
      for (const auto &dependency : info->additionalDependencies)
        pending.push_back(dependency.parent);
    }
  }
}

auto SemanticVisitor::Impl::supportsProvenLoanEnd(
    const LoanFlowRecord &loan) const -> bool {
  if (stmtAs<VariableDecl>(loan.lastUse) != nullptr ||
      stmtAs<ExpressionStmt>(loan.lastUse) != nullptr ||
      stmtAs<BlockStmt>(loan.lastUse) != nullptr ||
      stmtAs<IfStmt>(loan.lastUse) != nullptr ||
      stmtAs<SwitchStmt>(loan.lastUse) != nullptr) {
    return true;
  }
  const auto loop = std::find_if(loanFlow.loops.begin(), loanFlow.loops.end(),
                                 [&](const LoanFlowLoop &candidate) {
                                   return candidate.statement == loan.lastUse &&
                                          loan.id < candidate.firstNestedLoan;
                                 });
  return loop != loanFlow.loops.end();
}

auto SemanticVisitor::Impl::canEndLoanEarly(const LoanFlowRecord &loan) const
    -> bool {
  const SemanticLoanInfo *semanticLoan = semanticModel.findLoan(loan.id);
  // Every carrier contributes uses to this loan-wide flow record. That
  // supports a common last-use endpoint for shared readers, but not the
  // state transitions required by mutable or exclusive aliases.
  const bool supportedMultiplicity =
      !loan.hasAliases ||
      (semanticLoan != nullptr && semanticLoan->access == AccessMode::ReadOnly);
  return !loan.lifecycleBound && supportedMultiplicity &&
         loan.confinedToRegion && loan.region != 0 && loan.lastUse != nullptr &&
         supportsProvenLoanEnd(loan);
}

auto SemanticVisitor::Impl::supportsConditionalArmLoanEnd(const Stmt *statement)
    -> bool {
  return stmtAs<VariableDecl>(statement) != nullptr ||
         stmtAs<ExpressionStmt>(statement) != nullptr;
}

auto SemanticVisitor::Impl::pathStartsWith(
    const std::vector<ActiveLoanFlowConditionalArm> &path,
    const std::vector<ActiveLoanFlowConditionalArm> &prefix) -> bool {
  if (prefix.size() > path.size()) {
    return false;
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    if (path[index].conditional != prefix[index].conditional ||
        path[index].thenBranch != prefix[index].thenBranch) {
      return false;
    }
  }
  return true;
}

auto SemanticVisitor::Impl::pathStartsWith(
    const std::vector<ActiveLoanFlowSwitchArm> &path,
    const std::vector<ActiveLoanFlowSwitchArm> &prefix) -> bool {
  return prefix.size() <= path.size() &&
         std::equal(prefix.begin(), prefix.end(), path.begin());
}

auto SemanticVisitor::Impl::sameStructuredPath(
    const std::vector<ActiveLoanFlowConditionalArm> &left,
    const std::vector<ActiveLoanFlowSwitchArm> &leftSwitch,
    const std::vector<ActiveLoanFlowConditionalArm> &right,
    const std::vector<ActiveLoanFlowSwitchArm> &rightSwitch) -> bool {
  return left == right && leftSwitch == rightSwitch;
}

auto SemanticVisitor::Impl::structuredPathsMayOverlap(
    const std::vector<ActiveLoanFlowConditionalArm> &left,
    const std::vector<ActiveLoanFlowSwitchArm> &leftSwitch,
    const std::vector<ActiveLoanFlowConditionalArm> &right,
    const std::vector<ActiveLoanFlowSwitchArm> &rightSwitch) -> bool {
  const bool conditionalOverlap =
      pathStartsWith(left, right) || pathStartsWith(right, left);
  const bool switchOverlap = pathStartsWith(leftSwitch, rightSwitch) ||
                             pathStartsWith(rightSwitch, leftSwitch);
  return conditionalOverlap && switchOverlap;
}

auto SemanticVisitor::Impl::pathContainsUnsupportedControlFlow(
    const std::vector<ActiveLoanFlowConditionalArm> &path,
    const std::vector<ActiveLoanFlowSwitchArm> &switchPath) const -> bool {
  for (const ActiveLoanFlowConditionalArm &active : path) {
    if (active.conditional >= loanFlow.conditionals.size()) {
      return true;
    }
    const LoanFlowConditional &conditional =
        loanFlow.conditionals[active.conditional];
    const LoanFlowConditionalArm &arm =
        active.thenBranch ? conditional.thenArm : conditional.elseArm;
    if (arm.containsUnsupportedControlFlow) {
      return true;
    }
  }
  for (const ActiveLoanFlowSwitchArm &active : switchPath) {
    if (active.switchIndex >= loanFlow.switches.size() ||
        active.armIndex >= loanFlow.switches[active.switchIndex].arms.size() ||
        loanFlow.switches[active.switchIndex]
            .arms[active.armIndex]
            .containsUnsupportedControlFlow) {
      return true;
    }
  }
  return false;
}

auto SemanticVisitor::Impl::pathLocalBreakEndPlan(
    SemanticLoanId id, const LoanFlowRecord &loan,
    std::size_t conflictIndex) const -> std::optional<LoanFlowBreakEndPlan> {
  if (!canEndLoanEarly(loan) || conflictIndex >= loanFlow.conflicts.size()) {
    return std::nullopt;
  }
  const PendingBorrowConflict &conflict = loanFlow.conflicts[conflictIndex];
  if (std::find(conflict.loans.begin(), conflict.loans.end(), id) ==
          conflict.loans.end() ||
      pathContainsUnsupportedControlFlow(conflict.path, conflict.switchPath)) {
    return std::nullopt;
  }

  const LoanFlowBreak *terminatingBreak = nullptr;
  for (const LoanFlowBreak &candidate : loanFlow.breaks) {
    if (candidate.target != loan.lastUse ||
        candidate.sequence <= conflict.sequence ||
        candidate.region != conflict.region ||
        candidate.order != conflict.order + 1 ||
        !sameStructuredPath(candidate.path, candidate.switchPath, conflict.path,
                            conflict.switchPath) ||
        pathContainsUnsupportedControlFlow(candidate.path,
                                           candidate.switchPath)) {
      continue;
    }
    if (terminatingBreak == nullptr ||
        candidate.sequence < terminatingBreak->sequence) {
      terminatingBreak = &candidate;
    }
  }
  if (terminatingBreak == nullptr) {
    return std::nullopt;
  }

  const LoanFlowUse *lastUse = nullptr;
  for (const LoanFlowUse &use : loanFlow.uses) {
    if (use.loan != id || use.sequence >= terminatingBreak->sequence ||
        !structuredPathsMayOverlap(use.path, use.switchPath, conflict.path,
                                   conflict.switchPath)) {
      continue;
    }
    if (!sameStructuredPath(use.path, use.switchPath, conflict.path,
                            conflict.switchPath)) {
      // A use on an ancestor or descendant path needs a general CFG proof.
      // Sibling paths do not overlap and were filtered above.
      return std::nullopt;
    }
    if (use.sequence >= conflict.sequence) {
      return std::nullopt;
    }
    if (lastUse == nullptr || use.sequence > lastUse->sequence) {
      lastUse = &use;
    }
  }

  LoanFlowArmEndpoint endpoint;
  if (lastUse != nullptr) {
    if (lastUse->statement == nullptr ||
        lastUse->statement == conflict.statement ||
        lastUse->region != conflict.region ||
        lastUse->order >= conflict.order ||
        !supportsConditionalArmLoanEnd(lastUse->statement)) {
      return std::nullopt;
    }
    endpoint = {.atEntry = false,
                .after = lastUse->statement,
                .sequence = lastUse->sequence,
                .path = lastUse->path,
                .switchPath = lastUse->switchPath};
  } else if (!conflict.path.empty()) {
    const ActiveLoanFlowConditionalArm active = conflict.path.back();
    if (active.conditional >= loanFlow.conditionals.size() ||
        loanFlow.conditionals[active.conditional].statement == nullptr) {
      return std::nullopt;
    }
    endpoint = {.entryConditional =
                    loanFlow.conditionals[active.conditional].statement,
                .thenBranch = active.thenBranch,
                .path = conflict.path,
                .switchPath = conflict.switchPath};
  } else if (!conflict.switchPath.empty()) {
    const ActiveLoanFlowSwitchArm active = conflict.switchPath.back();
    if (active.switchIndex >= loanFlow.switches.size() ||
        loanFlow.switches[active.switchIndex].statement == nullptr) {
      return std::nullopt;
    }
    endpoint = {.entrySwitch = loanFlow.switches[active.switchIndex].statement,
                .switchArm = active.armIndex,
                .path = conflict.path,
                .switchPath = conflict.switchPath};
  } else {
    return std::nullopt;
  }

  return LoanFlowBreakEndPlan{.conflict = conflictIndex,
                              .endpoint = std::move(endpoint)};
}

auto SemanticVisitor::Impl::conditionalHasConflict(std::size_t conditionalIndex,
                                                   SemanticLoanId loan) const
    -> bool {
  if (conditionalIndex >= loanFlow.conditionals.size()) {
    return false;
  }
  const LoanFlowConditional &conditional =
      loanFlow.conditionals[conditionalIndex];
  if (conditional.conflictEnd > loanFlow.conflicts.size()) {
    return false;
  }
  for (std::size_t index = conditional.conflictBegin;
       index < conditional.conflictEnd; ++index) {
    const std::vector<SemanticLoanId> &loans = loanFlow.conflicts[index].loans;
    if (std::find(loans.begin(), loans.end(), loan) != loans.end()) {
      return true;
    }
  }
  return false;
}

auto SemanticVisitor::Impl::armHasConflict(const LoanFlowConditionalArm &arm,
                                           SemanticLoanId loan) const -> bool {
  if (arm.conflictEnd > loanFlow.conflicts.size()) {
    return false;
  }
  for (std::size_t index = arm.conflictBegin; index < arm.conflictEnd;
       ++index) {
    const std::vector<SemanticLoanId> &loans = loanFlow.conflicts[index].loans;
    if (std::find(loans.begin(), loans.end(), loan) != loans.end()) {
      return true;
    }
  }
  return false;
}

auto SemanticVisitor::Impl::provenTerminatingPathEndpoints(
    std::size_t conditionalIndex, bool thenBranch, SemanticLoanId loan) const
    -> std::optional<std::vector<LoanFlowArmEndpoint>> {
  if (conditionalIndex >= loanFlow.conditionals.size()) {
    return std::nullopt;
  }
  const LoanFlowConditional &conditional =
      loanFlow.conditionals[conditionalIndex];
  const LoanFlowConditionalArm &arm =
      thenBranch ? conditional.thenArm : conditional.elseArm;
  if (conditional.statement == nullptr || arm.containsUnsupportedControlFlow) {
    return std::nullopt;
  }
  const Stmt *branch = thenBranch ? conditional.statement->thenBranch().get()
                                  : conditional.statement->elseBranch().get();
  if (branch != nullptr && !summarizeLoanFlow(branch).canFallThrough) {
    if (!armHasConflict(arm, loan)) {
      return std::vector<LoanFlowArmEndpoint>{};
    }
    return provenConditionalArmEndpoints(conditionalIndex, thenBranch, loan);
  }

  std::vector<LoanFlowArmEndpoint> endpoints;
  for (const std::size_t nestedIndex : arm.nestedConditionals) {
    std::optional<std::vector<LoanFlowArmEndpoint>> thenEndpoints =
        provenTerminatingPathEndpoints(nestedIndex, true, loan);
    std::optional<std::vector<LoanFlowArmEndpoint>> elseEndpoints =
        provenTerminatingPathEndpoints(nestedIndex, false, loan);
    if (!thenEndpoints || !elseEndpoints) {
      return std::nullopt;
    }
    endpoints.insert(endpoints.end(), thenEndpoints->begin(),
                     thenEndpoints->end());
    endpoints.insert(endpoints.end(), elseEndpoints->begin(),
                     elseEndpoints->end());
  }
  return endpoints;
}

auto SemanticVisitor::Impl::provenConditionalArmEndpoints(
    std::size_t conditionalIndex, bool thenBranch, SemanticLoanId loan) const
    -> std::optional<std::vector<LoanFlowArmEndpoint>> {
  if (conditionalIndex >= loanFlow.conditionals.size()) {
    return std::nullopt;
  }
  const LoanFlowConditional &conditional =
      loanFlow.conditionals[conditionalIndex];
  const LoanFlowConditionalArm &arm =
      thenBranch ? conditional.thenArm : conditional.elseArm;
  if (conditional.statement == nullptr || arm.containsUnsupportedControlFlow ||
      arm.useEnd > loanFlow.uses.size()) {
    return std::nullopt;
  }

  const Stmt *branch = thenBranch ? conditional.statement->thenBranch().get()
                                  : conditional.statement->elseBranch().get();
  if (branch != nullptr && !summarizeLoanFlow(branch).canFallThrough &&
      !armHasConflict(arm, loan)) {
    return std::vector<LoanFlowArmEndpoint>{};
  }

  std::vector<ActiveLoanFlowConditionalArm> armPath = conditional.parentPath;
  armPath.push_back(
      {.conditional = conditionalIndex, .thenBranch = thenBranch});

  const LoanFlowUse *lastUse = nullptr;
  std::size_t lastUseIndex = arm.useEnd;
  for (std::size_t index = arm.useBegin; index < arm.useEnd; ++index) {
    const LoanFlowUse &candidate = loanFlow.uses[index];
    if (candidate.loan == loan &&
        (lastUse == nullptr || candidate.sequence > lastUse->sequence)) {
      lastUse = &candidate;
      lastUseIndex = index;
    }
  }
  if (lastUse == nullptr) {
    return std::vector<LoanFlowArmEndpoint>{
        LoanFlowArmEndpoint{.entryConditional = conditional.statement,
                            .thenBranch = thenBranch,
                            .path = std::move(armPath),
                            .switchPath = conditional.parentSwitchPath}};
  }

  const LoanFlowConditional *lastUseConditional = nullptr;
  std::size_t lastUseConditionalIndex = 0;
  for (const std::size_t nestedIndex : arm.nestedConditionals) {
    if (nestedIndex >= loanFlow.conditionals.size()) {
      return std::nullopt;
    }
    const LoanFlowConditional &nested = loanFlow.conditionals[nestedIndex];
    if (lastUseIndex >= nested.useBegin && lastUseIndex < nested.useEnd) {
      lastUseConditional = &nested;
      lastUseConditionalIndex = nestedIndex;
      break;
    }
  }

  std::vector<LoanFlowArmEndpoint> endpoints;
  for (const std::size_t nestedIndex : arm.nestedConditionals) {
    if ((lastUseConditional != nullptr &&
         nestedIndex == lastUseConditionalIndex) ||
        (lastUseConditional == nullptr &&
         loanFlow.conditionals[nestedIndex].endSequence >= lastUse->sequence)) {
      break;
    }
    std::optional<std::vector<LoanFlowArmEndpoint>> thenEndpoints =
        provenTerminatingPathEndpoints(nestedIndex, true, loan);
    std::optional<std::vector<LoanFlowArmEndpoint>> elseEndpoints =
        provenTerminatingPathEndpoints(nestedIndex, false, loan);
    if (!thenEndpoints || !elseEndpoints) {
      return std::nullopt;
    }
    endpoints.insert(endpoints.end(), thenEndpoints->begin(),
                     thenEndpoints->end());
    endpoints.insert(endpoints.end(), elseEndpoints->begin(),
                     elseEndpoints->end());
  }

  if (lastUseConditional == nullptr) {
    if (lastUse->statement == nullptr ||
        !supportsConditionalArmLoanEnd(lastUse->statement)) {
      return std::nullopt;
    }
    endpoints.push_back({.atEntry = false,
                         .after = lastUse->statement,
                         .sequence = lastUse->sequence,
                         .path = lastUse->path,
                         .switchPath = lastUse->switchPath});
    return endpoints;
  }

  if (!conditionalHasConflict(lastUseConditionalIndex, loan)) {
    if (lastUseConditional->statement == nullptr) {
      return std::nullopt;
    }
    endpoints.push_back({.atEntry = false,
                         .after = lastUseConditional->statement,
                         .sequence = lastUseConditional->endSequence,
                         .path = lastUseConditional->parentPath,
                         .switchPath = lastUseConditional->parentSwitchPath});
    return endpoints;
  }

  std::optional<std::vector<LoanFlowArmEndpoint>> thenEndpoints =
      provenConditionalArmEndpoints(lastUseConditionalIndex, true, loan);
  std::optional<std::vector<LoanFlowArmEndpoint>> elseEndpoints =
      provenConditionalArmEndpoints(lastUseConditionalIndex, false, loan);
  if (!thenEndpoints || !elseEndpoints) {
    return std::nullopt;
  }
  endpoints.insert(endpoints.end(), thenEndpoints->begin(),
                   thenEndpoints->end());
  endpoints.insert(endpoints.end(), elseEndpoints->begin(),
                   elseEndpoints->end());
  endpoints.push_back({.atEntry = false,
                       .proofOnly = true,
                       .after = lastUseConditional->statement,
                       .sequence = lastUseConditional->endSequence,
                       .path = lastUseConditional->parentPath,
                       .switchPath = lastUseConditional->parentSwitchPath});
  return endpoints;
}

auto SemanticVisitor::Impl::endpointPrecedesConflict(
    const LoanFlowArmEndpoint &endpoint, const PendingBorrowConflict &conflict)
    -> bool {
  if (!pathStartsWith(conflict.path, endpoint.path) ||
      !pathStartsWith(conflict.switchPath, endpoint.switchPath)) {
    return false;
  }
  return endpoint.atEntry ||
         (endpoint.after != nullptr && endpoint.after != conflict.statement &&
          endpoint.sequence < conflict.sequence);
}

auto SemanticVisitor::Impl::planHasResolvedConflict(
    const LoanFlowConditionalEndPlan &plan, SemanticLoanId loan) const -> bool {
  if (plan.conditional >= loanFlow.conditionals.size()) {
    return false;
  }
  const LoanFlowConditional &conditional =
      loanFlow.conditionals[plan.conditional];
  for (std::size_t index = conditional.conflictBegin;
       index < conditional.conflictEnd && index < loanFlow.conflicts.size();
       ++index) {
    const PendingBorrowConflict &conflict = loanFlow.conflicts[index];
    if (std::find(conflict.loans.begin(), conflict.loans.end(), loan) ==
        conflict.loans.end()) {
      continue;
    }
    if (std::any_of(plan.endpoints.begin(), plan.endpoints.end(),
                    [&](const LoanFlowArmEndpoint &endpoint) {
                      return endpointPrecedesConflict(endpoint, conflict);
                    })) {
      return true;
    }
  }
  return false;
}

auto SemanticVisitor::Impl::conditionalEndPlan(SemanticLoanId id,
                                               const LoanFlowRecord &loan) const
    -> std::optional<LoanFlowConditionalEndPlan> {
  for (std::size_t index = 0; index < loanFlow.conditionals.size(); ++index) {
    const LoanFlowConditional &conditional = loanFlow.conditionals[index];
    if (conditional.statement == nullptr ||
        conditional.statement != loan.lastUse) {
      continue;
    }
    std::optional<std::vector<LoanFlowArmEndpoint>> endpoints =
        provenConditionalArmEndpoints(index, true, id);
    std::optional<std::vector<LoanFlowArmEndpoint>> elseEndpoints =
        provenConditionalArmEndpoints(index, false, id);
    if (!endpoints || !elseEndpoints) {
      return std::nullopt;
    }
    endpoints->insert(endpoints->end(), elseEndpoints->begin(),
                      elseEndpoints->end());
    LoanFlowConditionalEndPlan plan{.conditional = index,
                                    .statement = conditional.statement,
                                    .endpoints = std::move(*endpoints)};
    if (!planHasResolvedConflict(plan, id) &&
        summarizeLoanFlow(conditional.statement).canFallThrough) {
      return std::nullopt;
    }
    return plan;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::switchEndPlan(SemanticLoanId id,
                                          const LoanFlowRecord &loan) const
    -> std::optional<LoanFlowSwitchEndPlan> {
  for (std::size_t index = 0; index < loanFlow.switches.size(); ++index) {
    const LoanFlowSwitch &selection = loanFlow.switches[index];
    if (selection.statement == nullptr || selection.statement != loan.lastUse) {
      continue;
    }

    // An arm-entry endpoint is valid only when the subject is the final use
    // on every selected path. Arm-local uses need a more general per-arm
    // endpoint proof and remain conservative in this slice.
    for (const LoanFlowUse &use : loanFlow.uses) {
      if (use.loan != id ||
          std::none_of(use.switchPath.begin(), use.switchPath.end(),
                       [index](const ActiveLoanFlowSwitchArm &active) {
                         return active.switchIndex == index;
                       })) {
        continue;
      }
      return std::nullopt;
    }

    LoanFlowSwitchEndPlan plan{.switchIndex = index,
                               .statement = selection.statement};
    for (std::size_t arm = 0; arm < selection.arms.size(); ++arm) {
      std::vector<ActiveLoanFlowSwitchArm> path = selection.parentSwitchPath;
      path.push_back({.switchIndex = index, .armIndex = arm});
      plan.endpoints.push_back({.entrySwitch = selection.statement,
                                .switchArm = arm,
                                .path = selection.parentPath,
                                .switchPath = std::move(path)});
    }
    if (!selection.hasDefault) {
      plan.endpoints.push_back({.atEntry = false,
                                .after = selection.statement,
                                .sequence = selection.endSequence,
                                .path = selection.parentPath,
                                .switchPath = selection.parentSwitchPath});
    }
    return plan;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::conflictFollowsConditionalEndpoint(
    const LoanFlowConditionalEndPlan &plan, std::size_t conflictIndex,
    SemanticLoanId loan) const -> bool {
  if (plan.conditional >= loanFlow.conditionals.size()) {
    return false;
  }
  if (conflictIndex >= loanFlow.conflicts.size()) {
    return false;
  }
  const PendingBorrowConflict &conflict = loanFlow.conflicts[conflictIndex];
  return std::find(conflict.loans.begin(), conflict.loans.end(), loan) !=
             conflict.loans.end() &&
         std::any_of(plan.endpoints.begin(), plan.endpoints.end(),
                     [&](const LoanFlowArmEndpoint &endpoint) {
                       return endpointPrecedesConflict(endpoint, conflict);
                     });
}

auto SemanticVisitor::Impl::conflictIsInConditionalArm(
    const LoanFlowConditionalEndPlan &plan, std::size_t conflictIndex) const
    -> bool {
  if (plan.conditional >= loanFlow.conditionals.size()) {
    return false;
  }
  const LoanFlowConditional &conditional =
      loanFlow.conditionals[plan.conditional];
  return (conflictIndex >= conditional.thenArm.conflictBegin &&
          conflictIndex < conditional.thenArm.conflictEnd) ||
         (conflictIndex >= conditional.elseArm.conflictBegin &&
          conflictIndex < conditional.elseArm.conflictEnd);
}

auto SemanticVisitor::Impl::reportBorrowConflict(
    const Token &token, const std::vector<SemanticLoanId> &loans,
    std::string message, bool rawAudit) -> void {
  std::vector<SemanticLoanId> active;
  for (const SemanticLoanId loan : loans) {
    if (loanFlow.records.contains(loan)) {
      appendUniqueLoan(active, loan);
    }
  }
  if (active.empty()) {
    return;
  }
  Diagnostic diagnostic =
      rawAudit
          ? makeDiagnostic(DiagnosticId::RawAuditPotential, token, {message})
          : makeDiagnostic("GTI-S2017", DiagnosticPhase::Semantics, token,
                           std::move(message));
  if (loanFlow.currentStatement == nullptr || loanFlow.currentRegion == 0) {
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  auto &pending = rawAudit ? loanFlow.rawAuditConflicts : loanFlow.conflicts;
  pending.push_back(
      {.loans = std::move(active),
       .region = loanFlow.currentRegion,
       .order = loanFlow.currentOrder,
       .sequence = rawAudit ? loanFlow.nextSequence : loanFlow.nextSequence++,
       .statement = loanFlow.currentStatement,
       .path = loanFlow.activeConditionalArms,
       .switchPath = loanFlow.activeSwitchArms,
       .loops = loanFlow.activeLoops,
       .diagnostic = std::move(diagnostic)});
}

auto SemanticVisitor::Impl::reportRawLoanConflict(
    const Token &token, const SemanticLoanPlace &place, AccessMode access,
    SemanticLoanId throughLoan) -> void {
  if (!rawAuditEnabled() || !place.valid())
    return;
  reportBorrowConflict(
      token, conflictingLoans(place, access, throughLoan),
      "Raw access may overlap a live incompatible checked borrow.", true);
}

auto SemanticVisitor::Impl::recordProvenLoanEndpoint(
    SemanticLoanId id, const LoanFlowArmEndpoint &endpoint) -> void {
  if (endpoint.proofOnly) {
    return;
  }
  if (endpoint.atEntry && endpoint.entryConditional != nullptr) {
    semanticModel.recordLoanEndAtConditionalEntry(
        id, *endpoint.entryConditional, endpoint.thenBranch);
  } else if (endpoint.atEntry && endpoint.entrySwitch != nullptr) {
    semanticModel.recordLoanEndAtSwitchArmEntry(id, *endpoint.entrySwitch,
                                                endpoint.switchArm);
  } else if (!endpoint.atEntry && endpoint.after != nullptr) {
    semanticModel.recordLoanEndAfter(id, *endpoint.after);
  }
}

auto SemanticVisitor::Impl::finalizeLoanFlow() -> void {
  (void)finalizeCapturedText();
  recordProgramScopeBorrowCleanup();
  loanFlow.finalized = true;
  if (loanFlow.deferredGenericFlow) {
    // Unselected arms can add uses and conflicts. Publishing endpoints from
    // this incomplete primary would assert liveness that was never proved.
    return;
  }
  // Cleanup is a use of every retained dependency, including intermediate
  // carrier shells. A live cleanup-bound carrier cannot let an ancestor's
  // ordinary last-read endpoint retire that dependency underneath it. Carriers
  // already destroyed by an inner scope recorded their cleanup use at that
  // scope's exit and retain the existing structured endpoint restrictions.
  for (const auto &[id, flow] : loanFlow.records) {
    if (!flow.lifecycleBound || !flow.active)
      continue;
    const auto *loan = semanticModel.findLoan(id);
    if (loan == nullptr)
      continue;
    std::vector<SemanticLoanId> pending{loan->parent, loan->storedDependency};
    for (const auto &dependency : loan->additionalDependencies)
      pending.push_back(dependency.parent);
    std::unordered_set<SemanticLoanId> visited;
    while (!pending.empty()) {
      const auto ancestor = pending.back();
      pending.pop_back();
      if (ancestor == 0 || !visited.insert(ancestor).second)
        continue;
      if (auto found = loanFlow.records.find(ancestor);
          found != loanFlow.records.end())
        found->second.confinedToRegion = false;
      if (const auto *record = semanticModel.findLoan(ancestor)) {
        pending.push_back(record->parent);
        pending.push_back(record->storedDependency);
        for (const auto &dependency : record->additionalDependencies)
          pending.push_back(dependency.parent);
      }
    }
  }
  std::vector<SemanticLoanId> loanIds;
  loanIds.reserve(loanFlow.records.size());
  for (const auto &[id, _] : loanFlow.records) {
    loanIds.push_back(id);
  }
  std::sort(loanIds.begin(), loanIds.end());
  std::unordered_map<SemanticLoanId, LoanFlowConditionalEndPlan>
      conditionalEndPlans;
  std::unordered_map<SemanticLoanId, LoanFlowSwitchEndPlan> switchEndPlans;
  std::unordered_map<SemanticLoanId, std::vector<LoanFlowBreakEndPlan>>
      breakEndPlans;
  for (const SemanticLoanId id : loanIds) {
    const LoanFlowRecord &loan = loanFlow.records.at(id);
    if (canEndLoanEarly(loan)) {
      if (const std::optional<LoanFlowConditionalEndPlan> plan =
              conditionalEndPlan(id, loan)) {
        conditionalEndPlans.emplace(id, *plan);
        for (const LoanFlowArmEndpoint &endpoint : plan->endpoints) {
          recordProvenLoanEndpoint(id, endpoint);
        }
      } else if (const std::optional<LoanFlowSwitchEndPlan> plan =
                     switchEndPlan(id, loan)) {
        switchEndPlans.emplace(id, *plan);
        for (const LoanFlowArmEndpoint &endpoint : plan->endpoints) {
          recordProvenLoanEndpoint(id, endpoint);
        }
      } else {
        semanticModel.recordLoanEndAfter(id, *loan.lastUse);
      }

      std::vector<LoanFlowBreakEndPlan> plans;
      for (std::size_t conflictIndex = 0;
           conflictIndex < loanFlow.conflicts.size(); ++conflictIndex) {
        if (std::optional<LoanFlowBreakEndPlan> plan =
                pathLocalBreakEndPlan(id, loan, conflictIndex)) {
          recordProvenLoanEndpoint(id, plan->endpoint);
          plans.push_back(std::move(*plan));
        }
      }
      if (!plans.empty()) {
        breakEndPlans.emplace(id, std::move(plans));
      }
    }
  }
  // Scope cleanup can be recorded after statement traversal has restored its
  // enclosing source position. A loan carried entirely by that exiting scope
  // is nevertheless finished there if the same structured proof above found
  // an ordinary earlier endpoint. Cleanup-bound/outer-carried loans remain.
  for (const auto index : loanFlow.programScopeExitChecks) {
    auto &checks = requestSession.programStorageBorrowChecks;
    if (index >= checks.size())
      continue;
    std::erase_if(checks[index].protections, [&](const auto &protection) {
      if (!protection.scopeExit || protection.loan == 0)
        return false;
      const auto flow = loanFlow.records.find(protection.loan);
      return flow != loanFlow.records.end() && canEndLoanEarly(flow->second);
    });
  }
  // Endpoint decisions above see mandatory conflicts only. Append optional
  // observations afterward solely to reuse diagnostic liveness filtering.
  const auto mandatoryConflicts = loanFlow.conflicts.size();
  for (auto &conflict : loanFlow.rawAuditConflicts)
    loanFlow.conflicts.push_back(std::move(conflict));
  loanFlow.rawAuditConflicts.clear();
  for (std::size_t conflictIndex = 0; conflictIndex < loanFlow.conflicts.size();
       ++conflictIndex) {
    PendingBorrowConflict &conflict = loanFlow.conflicts[conflictIndex];
    std::vector<SemanticLoanId> liveLoans;
    for (const SemanticLoanId id : conflict.loans) {
      const auto found = loanFlow.records.find(id);
      // Entering a later loop does not revive an already-ended outer loan.
      // An actual use within this loop is projected to the loop statement by
      // recordSemanticLoanUseForLoan, so the strict order test cannot shorten
      // a dependency that must survive the backedge.
      if (found != loanFlow.records.end() && canEndLoanEarly(found->second) &&
          std::any_of(conflict.loops.begin(), conflict.loops.end(),
                      [&](std::size_t index) {
                        if (index >= loanFlow.loops.size())
                          return false;
                        const auto &loop = loanFlow.loops[index];
                        return id < loop.firstNestedLoan &&
                               loop.region == found->second.region &&
                               loop.order > found->second.lastUseOrder;
                      }))
        continue;
      const auto conditionalEnd = conditionalEndPlans.find(id);
      const auto switchEnd = switchEndPlans.find(id);
      const auto breakEnd = breakEndPlans.find(id);
      const bool pathLocalBreakConflict =
          breakEnd != breakEndPlans.end() &&
          std::any_of(breakEnd->second.begin(), breakEnd->second.end(),
                      [&](const LoanFlowBreakEndPlan &plan) {
                        return plan.conflict == conflictIndex;
                      });
      if (pathLocalBreakConflict) {
        continue;
      }
      const bool switchEndpointConflict =
          switchEnd != switchEndPlans.end() &&
          std::any_of(switchEnd->second.endpoints.begin(),
                      switchEnd->second.endpoints.end(),
                      [&](const LoanFlowArmEndpoint &endpoint) {
                        return endpointPrecedesConflict(endpoint, conflict);
                      });
      if (switchEndpointConflict) {
        continue;
      }
      const bool conditionalArmConflict =
          conditionalEnd != conditionalEndPlans.end() &&
          conflictIsInConditionalArm(conditionalEnd->second, conflictIndex);
      if (found == loanFlow.records.end() || !canEndLoanEarly(found->second) ||
          (conditionalArmConflict &&
           !conflictFollowsConditionalEndpoint(conditionalEnd->second,
                                               conflictIndex, id)) ||
          (!conditionalArmConflict &&
           (conflict.region != found->second.region ||
            conflict.order <= found->second.lastUseOrder))) {
        liveLoans.push_back(id);
      }
    }
    if (conflict.programStorageCheck) {
      // Hidden-call effects close after all source bodies are known, but their
      // loans must obey precisely the same structured endpoints as an ordinary
      // direct place conflict. Do not freeze provisional lexical liveness.
      auto &checks = requestSession.programStorageBorrowChecks;
      if (*conflict.programStorageCheck < checks.size())
        std::erase_if(checks[*conflict.programStorageCheck].protections,
                      [&](const auto &protection) {
                        return protection.loan != 0 &&
                               std::find(liveLoans.begin(), liveLoans.end(),
                                         protection.loan) == liveLoans.end();
                      });
      continue;
    }
    if (!liveLoans.empty()) {
      bool laterUse = false;
      bool lifecycleBound = false;
      for (const SemanticLoanId id : liveLoans) {
        const SemanticLoanInfo *loan = semanticModel.findLoan(id);
        if (loan != nullptr && loan->origin != nullptr) {
          RelatedDiagnostic related{tokenSpan(expressionToken(*loan->origin)),
                                    "Retained borrow originates here."};
          const bool duplicate = std::any_of(
              conflict.diagnostic.related.begin(),
              conflict.diagnostic.related.end(),
              [&](const RelatedDiagnostic &existing) {
                return existing.message == related.message &&
                       existing.span.source == related.span.source &&
                       existing.span.start == related.span.start &&
                       existing.span.end == related.span.end &&
                       existing.span.line == related.span.line;
              });
          if (!duplicate) {
            conflict.diagnostic.related.push_back(std::move(related));
          }
        }
        const auto flow = loanFlow.records.find(id);
        if (loan && loan->lifecycleBound)
          lifecycleBound = true;
        if (flow != loanFlow.records.end() &&
            flow->second.region == conflict.region &&
            flow->second.lastUseOrder > conflict.order &&
            flow->second.lastUse && flow->second.lastUse->extent()) {
          const auto &span = *flow->second.lastUse->extent();
          const bool duplicate = std::any_of(
              conflict.diagnostic.related.begin(),
              conflict.diagnostic.related.end(), [&](const auto &note) {
                return note.span.source == span.source &&
                       note.span.start == span.start &&
                       note.span.end == span.end;
              });
          if (!duplicate)
            conflict.diagnostic.related.push_back(
                {span, "The borrowed value is used later here."});
          laterUse = true;
        }
      }
      if (lifecycleBound) {
        conflict.diagnostic.hints.emplace_back(
            "This borrow protects a resource until its carrier is destroyed, "
            "not merely until its last read. End the carrier's scope before "
            "accessing the owner again.");
      } else if (laterUse) {
        conflict.diagnostic.hints.emplace_back(
            "Finish using the borrowed value before changing or moving its "
            "owner, or perform the owner operation before creating the borrow. "
            "Choose an order that preserves the intended effects.");
      } else {
        conflict.diagnostic.hints.emplace_back(
            "Place the borrowed value in a narrower block when an explicit "
            "lexical end is required across control flow.");
      }
      diagnostics.emplace_back(std::move(conflict.diagnostic));
    }
  }
  loanFlow.conflicts.resize(mandatoryConflicts);
}

auto SemanticVisitor::Impl::removeLoanCarrier(Symbol &carrier) -> void {
  const SemanticLoanId loan = carrier.retainedLoan;
  if (loan == 0) {
    return;
  }
  const SymbolId carrierId = toolingSymbolFor(carrier);
  loanFlow.carrierLoans.erase(carrierId);
  carrier.retainedLoan = 0;
  const auto found = loanFlow.records.find(loan);
  if (found == loanFlow.records.end()) {
    return;
  }
  found->second.carriers.erase(carrierId);
  if (!found->second.carriers.empty()) {
    return;
  }
  found->second.active = false;
  for (Scope &scope : scopes) {
    for (auto &[_, symbol] : scope) {
      std::erase(symbol.storageLoans, loan);
    }
  }
  std::erase(loanFlow.receiverStorageLoans, loan);
}

auto SemanticVisitor::Impl::isThreadHandleType(const SemanticType &type) const
    -> bool {
  return threadHandleResultType(type).has_value();
}

auto SemanticVisitor::Impl::threadHandleResultType(
    const SemanticType &type) const -> std::optional<SemanticType> {
  const SemanticType *candidate = &type;
  if (candidate->kind == SemanticType::Reference &&
      candidate->arguments.size() == 1) {
    candidate = &candidate->arguments.front();
  }
  const ClassInfo *owner = classInfo(*candidate);
  if (owner == nullptr ||
      owner->compilerCapability != CompilerCapabilityTypeKind::ThreadHandle) {
    return std::nullopt;
  }
  if (owner->genericParameters.empty() && candidate->arguments.empty()) {
    return SemanticType::Void;
  }
  if (owner->genericParameters.size() == 1 &&
      candidate->arguments.size() == 1) {
    return candidate->arguments.front();
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::isTransferableThreadResult(
    const SemanticType &type) const -> bool {
  if (type == SemanticType::Unknown ||
      type.kind == SemanticType::TypeParameter) {
    return true;
  }
  if (type == SemanticType::Void) {
    return true;
  }
  const SemanticTypeTraits traits = typeTraits(type);
  return traits.movable && traits.transferCapable &&
         !traits.containsBorrowedState &&
         !containsDisallowedCallableCaptureState(type);
}

auto SemanticVisitor::Impl::exactThreadTaskEntry(const SemanticType &type,
                                                 const SemanticType &resultType)
    -> std::optional<ResolvedThreadTaskEntry> {
  if (type.hasLambdaShape()) {
    const SemanticType *result = type.lambdaReturnType();
    if (type.lambdaId == 0 || result == nullptr || *result != resultType ||
        !type.lambdaParameterTypes().empty()) {
      return std::nullopt;
    }
    return ResolvedThreadTaskEntry{.lambda = type.lambdaId,
                                   .capability =
                                       CallableInvocationCapability::Read};
  }
  if (type.hasFunctionItemShape()) {
    const SemanticType *result = type.functionItemReturnType();
    if (result == nullptr || *result != resultType ||
        !type.functionItemParameterTypes().empty()) {
      return std::nullopt;
    }
    const FunctionInfo *function = semanticModel.findFunction(type.functionId);
    if (function == nullptr && instanceBaseModel != nullptr) {
      function = instanceBaseModel->findFunction(type.functionId);
    }
    if (function == nullptr || function->ownerClass != 0 ||
        function->entryPoint || function->linkage != LanguageLinkage::Gti ||
        function->declaration == nullptr ||
        function->bodyDeclaration()->body() == nullptr ||
        !function->genericParameters.empty()) {
      return std::nullopt;
    }
    return ResolvedThreadTaskEntry{.function = type.functionId,
                                   .capability =
                                       CallableInvocationCapability::Read};
  }
  if (type.kind != SemanticType::Class) {
    return std::nullopt;
  }
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr) {
    return std::nullopt;
  }
  const auto found = owner->members.find(
      std::string(operatorFunctionName(OverloadedOperator::Call)));
  if (found == owner->members.end() ||
      found->second.symbol.type != SemanticType::Function) {
    return std::nullopt;
  }

  const Symbol overloads = substituteSymbol(found->second.symbol, type);
  std::vector<const CallableCandidate *> viable;
  for (const CallableCandidate &candidate : overloads.overloads) {
    std::vector<SemanticType> parameters;
    for (const SemanticType &parameter : candidate.parameterTypes) {
      SemanticType::appendExpandedArgument(parameters, parameter);
    }
    if (candidate.id == 0 || candidate.declaration == nullptr ||
        candidate.access != AccessModifier::Public ||
        candidate.compilerPrivate || candidate.pureVirtual ||
        candidate.returnType != resultType || !parameters.empty() ||
        !candidate.record().genericParameters.empty() ||
        !candidate.requirements.empty()) {
      continue;
    }
    viable.push_back(&candidate);
  }
  if (viable.empty()) {
    return std::nullopt;
  }

  const auto capabilityRank = [](ReceiverMutability mutability) {
    switch (mutability) {
    case ReceiverMutability::ReadOnly:
      return 0;
    case ReceiverMutability::Mutable:
      return 1;
    case ReceiverMutability::Consuming:
      return 2;
    }
    return -1;
  };
  int selectedRank = -1;
  for (const CallableCandidate *candidate : viable) {
    selectedRank =
        std::max(selectedRank, capabilityRank(candidate->receiverMutability));
  }
  std::erase_if(viable, [&](const CallableCandidate *candidate) {
    return capabilityRank(candidate->receiverMutability) != selectedRank;
  });
  if (viable.size() != 1) {
    return std::nullopt;
  }
  const CallableCandidate &selected = *viable.front();
  return ResolvedThreadTaskEntry{
      .function = selected.id,
      .capability = callableInvocationCapability(selected.receiverMutability)};
}

auto SemanticVisitor::Impl::isExactThreadTaskType(
    const SemanticType &type, const SemanticType &resultType) -> bool {
  return type.kind == SemanticType::TypeParameter ||
         exactThreadTaskEntry(type, resultType).has_value();
}

auto SemanticVisitor::Impl::scopedThreadTaskShapeFor(const SemanticType &type)
    -> std::optional<ScopedThreadTaskShape> {
  const auto traits = typeTraits(type);
  const auto entry = exactThreadTaskEntry(type, SemanticType::Void);
  if (!traits.movable || !entry)
    return std::nullopt;
  if (entry->function != 0) {
    const auto *function = semanticModel.findFunction(entry->function);
    if (function == nullptr && instanceBaseModel != nullptr)
      function = instanceBaseModel->findFunction(entry->function);
    if (function == nullptr || function->requiresUnsafe)
      return std::nullopt;
  }
  std::vector<SemanticType> visiting;
  if (!typeCapabilities(type, visiting, ThreadTransferBoundary::Scoped)
           .transferCapable)
    return std::nullopt;

  ScopedThreadTaskShape shape{.taskType = type, .entry = *entry};
  if (!traits.containsBorrowedState) {
    if (containsDisallowedCallableCaptureState(type))
      return std::nullopt;
    return shape;
  }

  // The invocation layer will map these exact paths to its existing stored
  // loans. Missing paths are never permission to erase a hidden dependency.
  const auto paths = storedReferencePaths(type);
  if (paths.empty())
    return std::nullopt;
  for (const auto &path : paths) {
    visiting.clear();
    if (!typeCapabilities(path.reference, visiting,
                          ThreadTransferBoundary::Scoped)
             .transferCapable)
      return std::nullopt;
    auto fields = storedFieldSymbols(type, path.fields);
    if (fields.size() != path.fields.size() ||
        std::any_of(fields.begin(), fields.end(),
                    [](SymbolId field) { return field == 0; }))
      return std::nullopt;
    shape.dependencies.push_back(
        {.fields = std::move(fields), .reference = path.reference});
  }
  return shape;
}

auto SemanticVisitor::Impl::analyzeThreadScopeCall(const Call &expr) -> void {
  std::vector<SemanticType> types;
  for (const auto &argument : expr.arguments())
    types.push_back(analyze(argument));
  for (const auto &argument : expr.typeArguments())
    validateType(argument);

  ResolvedCallInfo resolved{.returnType = SemanticType::Int32,
                            .intrinsic = IntrinsicKind::ThreadScope};
  const auto inputs = concreteCallArguments(types, expr.arguments());
  if (!inputs) {
    // Generic primary bodies retain the symbolic pack. Concrete reanalysis
    // must replace it with one checked task record for every actual operand.
    resolved.parameterTypes = std::move(types);
    currentType = resolved.returnType;
    semanticModel.record(expr, std::move(resolved));
    return;
  }

  (void)validateConcurrentSurfaceAvailability(expr.paren(), "A parallel scope");

  if (std::any_of(inputs->begin(), inputs->end(), [](const auto &input) {
        return containsOpenTypeParameter(input.type);
      })) {
    for (const auto &input : *inputs)
      resolved.parameterTypes.push_back(input.type);
    currentType = resolved.returnType;
    semanticModel.record(expr, std::move(resolved));
    return; // The concrete instance selects entries and all borrowed origins.
  }

  for (const auto &input : *inputs) {
    const auto &argument = *input.expression;
    const auto *info = semanticModel.findExpression(*argument);
    const auto shape = scopedThreadTaskShapeFor(input.type);
    const bool consumed =
        input.forwardedPackElement ||
        (info != nullptr && info->category == ValueCategory::Value);
    resolved.parameterTypes.push_back(input.type);
    if (!shape || !consumed) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2084", DiagnosticPhase::Semantics, expressionToken(argument),
          "A parallel scope requires consumed exact void() tasks whose owned "
          "state may transfer and whose borrowed referents permit the "
          "selected access.");
      diagnostic.hints.emplace_back(
          "Pass a fresh task or std::move(task). Shared references require "
          "shareable referents; mutable references require transferable "
          "referents. Ordinary managed threads still require owned state.");
      diagnostics.emplace_back(std::move(diagnostic));
      continue;
    }
    ResolvedScopedThreadTask task{.shape = *shape};
    const auto dependencies = storedBorrowDependencies(input);
    bool complete = true;
    for (const auto &path : shape->dependencies) {
      auto found =
          std::find_if(dependencies.begin(), dependencies.end(),
                       [&](const auto &dependency) {
                         return dependency.designatedFields == path.fields;
                       });
      // The compact one-reference carrier form predates composed field paths.
      // Resolve that representation here, where the exact source type and
      // origin are both authoritative, rather than teaching a backend it.
      if (found == dependencies.end() && shape->dependencies.size() == 1 &&
          dependencies.size() == 1 &&
          dependencies.front().designatedFields.empty())
        found = dependencies.begin();
      if (found == dependencies.end() ||
          (found->place.root == 0 && !found->place.receiver) ||
          (path.reference.referenceAccess == AccessMode::Mutable &&
           found->access != AccessMode::Mutable)) {
        complete = false;
        break;
      }
      auto dependency = *found;
      dependency.designatedFields = path.fields;
      dependency.access = path.reference.referenceAccess;
      task.dependencies.push_back(std::move(dependency));
    }
    if (!complete) {
      report(expressionToken(argument),
             "A parallel task must retain an exact checked origin for every "
             "borrowed field through completion of the scope.",
             "GTI-S2084");
      continue;
    }
    // Argument evaluation is sequenced, but every child may execute at once.
    // Reuse ordinary place overlap, including disjoint field projections.
    for (const auto &previous : resolved.scopedThreadTasks)
      for (const auto &left : previous.dependencies)
        for (const auto &right : task.dependencies)
          if ((left.access == AccessMode::Mutable ||
               right.access == AccessMode::Mutable) &&
              loanPlacesOverlap(left.place, right.place)) {
            report(expressionToken(argument),
                   "Parallel tasks have overlapping borrowed access and at "
                   "least one task requires mutable access.",
                   "GTI-S2084");
          }
    resolved.scopedThreadTasks.push_back(std::move(task));
  }
  currentType = resolved.returnType;
  semanticModel.record(expr, std::move(resolved));
}

auto SemanticVisitor::Impl::analyzeThreadIntrinsicCall(
    const Call &expr, const CallableCandidate &declaration) -> void {
  const IntrinsicKind intrinsic = declaration.record().intrinsic;
  std::vector<SemanticType> explicitTypeArguments;
  explicitTypeArguments.reserve(expr.typeArguments().size());
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
    explicitTypeArguments.push_back(typeOf(argument));
  }

  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }

  if (intrinsic == IntrinsicKind::ThreadHandleEmpty) {
    if (!argumentTypes.empty()) {
      report(expr.paren(), "thread_handle_empty expects no arguments.",
             "GTI-S2084");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = declaration.returnType;
    if (currentType.kind == SemanticType::Class &&
        currentType.arguments.size() == 1 &&
        explicitTypeArguments.size() == 1) {
      currentType.arguments.front() = explicitTypeArguments.front();
    }
    if (!threadHandleResultType(currentType)) {
      report(expr.paren(),
             "The private empty-handle operation must return an exact "
             "managed-thread capability.",
             "GTI-S2084");
      currentType = SemanticType::Unknown;
    }
    semanticModel.record(expr, ResolvedCallInfo{.returnType = currentType,
                                                .typeArguments = std::move(
                                                    explicitTypeArguments),
                                                .intrinsic = intrinsic});
    return;
  }

  const std::size_t expected = intrinsic == IntrinsicKind::ThreadSpawn ? 2 : 1;
  if (argumentTypes.size() != expected) {
    report(expr.paren(),
           intrinsic == IntrinsicKind::ThreadSpawn
               ? "thread_spawn expects a mutable handle and one task."
               : "This managed-thread operation expects one handle.",
           "GTI-S2084");
    currentType = SemanticType::Unknown;
    return;
  }
  const std::optional<SemanticType> resultType =
      threadHandleResultType(argumentTypes.front());
  if (!resultType) {
    report(expressionToken(expr.arguments().front()),
           "Managed-thread operations require the compiler-private thread "
           "handle capability.",
           "GTI-S2084");
    currentType = SemanticType::Unknown;
    return;
  }

  const ExpressionInfo *handle =
      semanticModel.findExpression(*expr.arguments().front());
  if (handle == nullptr || handle->category != ValueCategory::Place ||
      ((intrinsic == IntrinsicKind::ThreadSpawn ||
        intrinsic == IntrinsicKind::ThreadJoin ||
        intrinsic == IntrinsicKind::ThreadResultTake ||
        intrinsic == IntrinsicKind::ThreadAutoJoin) &&
       handle->access != AccessMode::Mutable)) {
    report(expressionToken(expr.arguments().front()),
           "This managed-thread operation requires mutable stable handle "
           "storage.",
           "GTI-S2084");
  }

  std::vector<SemanticType> parameterTypes;
  parameterTypes.push_back(SemanticType::referenceTo(
      argumentTypes.front(), intrinsic == IntrinsicKind::ThreadJoinable
                                 ? AccessMode::ReadOnly
                                 : AccessMode::Mutable));
  std::vector<SemanticType> typeArguments;
  std::optional<ResolvedThreadTaskEntry> threadTaskEntry;
  if (intrinsic == IntrinsicKind::ThreadSpawn) {
    const SemanticType &task = argumentTypes[1];
    const ExpressionInfo *taskInfo =
        semanticModel.findExpression(*expr.arguments()[1]);
    const SemanticTypeTraits traits = typeTraits(task);
    const bool symbolicTask = task.kind == SemanticType::TypeParameter;
    if (!symbolicTask && task != SemanticType::Unknown) {
      threadTaskEntry = exactThreadTaskEntry(task, *resultType);
    }
    const bool validResult = isTransferableThreadResult(*resultType);
    const bool validTask =
        validResult &&
        (task == SemanticType::Unknown ||
         (taskInfo != nullptr && taskInfo->category == ValueCategory::Value &&
          (symbolicTask || threadTaskEntry.has_value()) &&
          (symbolicTask || (traits.movable && traits.transferCapable &&
                            !traits.containsBorrowedState &&
                            !containsDisallowedCallableCaptureState(task)))));
    if (!validTask || !validResult) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2084", DiagnosticPhase::Semantics,
          expressionToken(expr.arguments()[1]),
          std::string(*resultType == SemanticType::Void
                          ? "A managed thread requires one consumed, "
                            "transfer-safe exact void() task."
                          : "A result-bearing managed thread requires one "
                            "consumed, transfer-safe exact task whose return "
                            "type matches its owned transferable result."));
      if (const std::optional<TransferCapabilityDenial> denial =
              transferCapabilityDenial(task);
          denial && denial->source != nullptr) {
        diagnostic.related.push_back(
            {tokenSpan(*denial->source), denial->message});
      }
      diagnostic.hints.emplace_back(
          "Pass a fresh task value or std::move(task); captures must own "
          "their state and cannot contain references, tracked borrows, or "
          "unreviewed raw pointers. A result must satisfy the same "
          "cross-thread "
          "transfer boundary.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
    parameterTypes.push_back(task);
    typeArguments.push_back(task);
    if (const FunctionInfo *function = currentFunctionInfo()) {
      const std::optional<std::size_t> parameter =
          movedFunctionParameter(expr.arguments()[1], *function);
      if (parameter && *parameter < function->parameterTypes.size() &&
          function->parameterTypes[*parameter].kind ==
              SemanticType::TypeParameter &&
          currentFunctionDeclaration != nullptr) {
        const SemanticType callable = function->parameterTypes[*parameter];
        semanticModel.recordCallableRequirement(
            *currentFunctionDeclaration,
            CallableParameterContract{
                .parameterIndex = *parameter,
                .genericParameter = callable.genericParameterId,
                .callableType = callable,
                .access = AccessMode::ReadOnly,
                .boundary = CallableBoundary::Owned,
                .ownedTransport = CallableOwnedTransport{
                    .kind = CallableOwnedTransportKind::ThreadTask,
                    .destinationType = *resultType}});
      }
    }
  }

  // Both joins report a status. A typed join keeps a successful worker's
  // result in the handle until the take operation moves it out, so the
  // public wrapper builds its own result type in source.
  if (intrinsic == IntrinsicKind::ThreadResultTake &&
      *resultType == SemanticType::Void) {
    report(expr.paren(),
           "thread_result_take requires a result-bearing thread handle.",
           "GTI-S2084");
    currentType = SemanticType::Unknown;
    return;
  }
  currentType =
      intrinsic == IntrinsicKind::ThreadJoinable     ? SemanticType::Bool
      : intrinsic == IntrinsicKind::ThreadAutoJoin   ? SemanticType::Void
      : intrinsic == IntrinsicKind::ThreadResultTake ? *resultType
                                                     : SemanticType::Int32;
  semanticModel.record(
      expr, ResolvedCallInfo{.returnType = currentType,
                             .parameterTypes = std::move(parameterTypes),
                             .typeArguments = std::move(typeArguments),
                             .intrinsic = intrinsic,
                             .threadTaskEntry = std::move(threadTaskEntry)});
}

auto SemanticVisitor::Impl::analyzeAtomicIntrinsicCall(
    const Call &expr, const CallableCandidate &declaration) -> void {
  const IntrinsicKind intrinsic = declaration.record().intrinsic;
  const CallableDeclaration &record = declaration.record();
  const SynchronizationOperation synchronization =
      trustedSynchronizationOperation(record.sourceUnit, record.namespaceScope,
                                      record.syntax->name().lexeme);
  if (!validSynchronizationOperation(synchronization)) {
    report(expr.paren(),
           "This private atomic operation has no valid static memory-order "
           "contract.",
           "GTI-S2086");
    currentType = SemanticType::Unknown;
    return;
  }
  std::vector<SemanticType> explicitTypeArguments;
  explicitTypeArguments.reserve(expr.typeArguments().size());
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
    explicitTypeArguments.push_back(typeOf(argument));
  }

  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }

  const auto atomicElement =
      [&](const SemanticType &type) -> std::optional<SemanticType> {
    const SemanticType *candidate = &type;
    if (candidate->kind == SemanticType::Reference &&
        candidate->arguments.size() == 1) {
      candidate = &candidate->arguments.front();
    }
    const ClassInfo *owner = classInfo(*candidate);
    if (owner == nullptr ||
        owner->compilerCapability !=
            CompilerCapabilityTypeKind::AtomicStorage ||
        candidate->arguments.size() != 1) {
      return std::nullopt;
    }
    return candidate->arguments.front();
  };
  const auto validScalar = [&](const SemanticType &type) {
    return type == SemanticType::Unknown ||
           satisfiesConstraint(
               type, constraintBit(GenericConstraintKind::AtomicScalar));
  };

  const std::size_t expectedArguments =
      intrinsic == IntrinsicKind::AtomicLoad ||
              intrinsic == IntrinsicKind::AtomicNotifyOne ||
              intrinsic == IntrinsicKind::AtomicNotifyAll
          ? 1
      : intrinsic == IntrinsicKind::AtomicStore ||
              intrinsic == IntrinsicKind::AtomicWait ||
              intrinsic == IntrinsicKind::AtomicExchange
          ? 2
          : 4;
  if (argumentTypes.size() != expectedArguments || argumentTypes.empty()) {
    report(expr.paren(),
           "This private atomic operation has the wrong number of "
           "arguments.",
           "GTI-S2086");
    currentType = SemanticType::Unknown;
    return;
  }
  const std::optional<SemanticType> elementType =
      atomicElement(argumentTypes.front());
  if (!elementType || !validScalar(*elementType)) {
    report(expressionToken(expr.arguments().front()),
           "Atomic operations require the exact compiler-owned scalar "
           "atomic storage capability.",
           "GTI-S2086");
    currentType = SemanticType::Unknown;
    return;
  }
  const ExpressionInfo *storage =
      semanticModel.findExpression(*expr.arguments().front());
  if (storage == nullptr || storage->category != ValueCategory::Place) {
    report(expressionToken(expr.arguments().front()),
           "Atomic operations require stable atomic storage.", "GTI-S2086");
  }
  for (std::size_t index = 1; index < argumentTypes.size(); ++index) {
    if (argumentTypes[index] != SemanticType::Unknown &&
        *elementType != SemanticType::Unknown &&
        argumentTypes[index] != *elementType) {
      report(expressionToken(expr.arguments()[index]),
             "Atomic operands must exactly match the atomic scalar type '" +
                 typeSpelling(*elementType) + "'.",
             "GTI-S2086");
    }
  }
  if (intrinsic == IntrinsicKind::AtomicCompareExchange) {
    const ExpressionInfo *observed =
        semanticModel.findExpression(*expr.arguments().back());
    if (observed == nullptr || observed->category != ValueCategory::Place ||
        observed->access != AccessMode::Mutable) {
      report(expressionToken(expr.arguments().back()),
             "The private compare-exchange observed-value output requires "
             "mutable stable scalar storage.",
             "GTI-S2086");
    }
  }

  std::vector<SemanticType> recordedTypeArguments{*elementType};
  if (intrinsic == IntrinsicKind::AtomicCompareExchange) {
    if (!explicitTypeArguments.empty()) {
      report(expr.paren(),
             "atomic_compare_exchange infers its scalar type from storage.",
             "GTI-S2086");
    }
    currentType = SemanticType::Bool;
  } else {
    if (!explicitTypeArguments.empty()) {
      report(expr.paren(),
             "This atomic operation infers its scalar type from storage.",
             "GTI-S2086");
    }
    currentType = intrinsic == IntrinsicKind::AtomicStore ||
                          intrinsic == IntrinsicKind::AtomicWait ||
                          intrinsic == IntrinsicKind::AtomicNotifyOne ||
                          intrinsic == IntrinsicKind::AtomicNotifyAll
                      ? SemanticType::Void
                      : *elementType;
  }

  std::vector<SemanticType> parameterTypes = argumentTypes;
  parameterTypes.front() =
      SemanticType::referenceTo(argumentTypes.front(), AccessMode::ReadOnly);
  if (intrinsic == IntrinsicKind::AtomicCompareExchange) {
    parameterTypes.back() =
        SemanticType::referenceTo(argumentTypes.back(), AccessMode::Mutable);
  }
  semanticModel.record(
      expr, ResolvedCallInfo{.returnType = currentType,
                             .parameterTypes = std::move(parameterTypes),
                             .typeArguments = std::move(recordedTypeArguments),
                             .intrinsic = intrinsic,
                             .synchronization = synchronization});
}

auto SemanticVisitor::Impl::analyzeMutexIntrinsicCall(
    const Call &expr, const CallableCandidate &declaration) -> void {
  const IntrinsicKind intrinsic = declaration.record().intrinsic;
  std::vector<SemanticType> explicitTypeArguments;
  explicitTypeArguments.reserve(expr.typeArguments().size());
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
    explicitTypeArguments.push_back(typeOf(argument));
  }
  if (!explicitTypeArguments.empty()) {
    report(expr.paren(),
           "Private mutex operations infer their element type from their "
           "capability arguments.",
           "GTI-S2087");
  }

  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }

  const auto capabilityElement =
      [&](const SemanticType &type,
          CompilerCapabilityTypeKind kind) -> std::optional<SemanticType> {
    const SemanticType *candidate = &type;
    if (candidate->kind == SemanticType::Reference &&
        candidate->arguments.size() == 1) {
      candidate = &candidate->arguments.front();
    }
    const ClassInfo *owner = classInfo(*candidate);
    if (owner == nullptr || owner->compilerCapability != kind ||
        candidate->arguments.size() != 1) {
      return std::nullopt;
    }
    return candidate->arguments.front();
  };

  if (intrinsic == IntrinsicKind::MutexLockFailure) {
    if (!argumentTypes.empty()) {
      report(expr.paren(), "mutex_lock_failure expects no arguments.",
             "GTI-S2087");
    }
    currentType = SemanticType::Void;
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .intrinsic = IntrinsicKind::MutexLockFailure});
    return;
  }

  const bool storageAndToken = intrinsic == IntrinsicKind::MutexLock ||
                               intrinsic == IntrinsicKind::MutexUnlock ||
                               intrinsic == IntrinsicKind::MutexGuardRead ||
                               intrinsic == IntrinsicKind::MutexGuardReadMut;
  const std::size_t expectedArguments = storageAndToken ? 2 : 1;
  if (argumentTypes.size() != expectedArguments) {
    report(expr.paren(),
           storageAndToken
               ? "This private mutex operation expects one storage and one "
                 "lock token."
               : "mutex_guard_active expects one lock token.",
           "GTI-S2087");
    currentType = SemanticType::Unknown;
    return;
  }

  std::optional<SemanticType> elementType;
  std::size_t tokenIndex = 0;
  if (storageAndToken) {
    elementType = capabilityElement(argumentTypes.front(),
                                    CompilerCapabilityTypeKind::MutexStorage);
    tokenIndex = 1;
  }
  const std::optional<SemanticType> tokenElement = capabilityElement(
      argumentTypes[tokenIndex], CompilerCapabilityTypeKind::MutexLockToken);
  if ((storageAndToken && !elementType) || !tokenElement ||
      (elementType && *elementType != *tokenElement)) {
    report(expressionToken(expr.arguments().front()),
           "Mutex operations require the exact compiler-owned storage and "
           "lock-token capabilities with one matching element type.",
           "GTI-S2087");
    currentType = SemanticType::Unknown;
    return;
  }
  if (!elementType) {
    elementType = tokenElement;
  }

  for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
    const ExpressionInfo *argument =
        semanticModel.findExpression(*expr.arguments()[index]);
    if (argument == nullptr || argument->category != ValueCategory::Place) {
      report(expressionToken(expr.arguments()[index]),
             "Mutex operations require stable capability storage.",
             "GTI-S2087");
    }
  }
  const bool mutatesToken = intrinsic == IntrinsicKind::MutexLock ||
                            intrinsic == IntrinsicKind::MutexUnlock;
  if (mutatesToken) {
    const ExpressionInfo *token =
        semanticModel.findExpression(*expr.arguments()[tokenIndex]);
    if (token == nullptr || token->access != AccessMode::Mutable) {
      report(expressionToken(expr.arguments()[tokenIndex]),
             "Lock and unlock require mutable stable token storage.",
             "GTI-S2087");
    }
  }

  const bool read = intrinsic == IntrinsicKind::MutexGuardRead ||
                    intrinsic == IntrinsicKind::MutexGuardReadMut;
  const AccessMode readAccess = intrinsic == IntrinsicKind::MutexGuardReadMut
                                    ? AccessMode::Mutable
                                    : AccessMode::ReadOnly;
  currentType = intrinsic == IntrinsicKind::MutexLock ? SemanticType::Int32
                : intrinsic == IntrinsicKind::MutexGuardActive
                    ? SemanticType::Bool
                : read ? *elementType
                       : SemanticType::Void;

  std::vector<SemanticType> parameterTypes;
  if (storageAndToken) {
    parameterTypes.push_back(
        SemanticType::referenceTo(argumentTypes.front(), AccessMode::ReadOnly));
  }
  parameterTypes.push_back(SemanticType::referenceTo(
      argumentTypes[tokenIndex],
      mutatesToken ? AccessMode::Mutable : AccessMode::ReadOnly));
  const SynchronizationOperation synchronization =
      trustedSynchronizationOperation(
          declaration.record().sourceUnit, declaration.record().namespaceScope,
          declaration.record().syntax->name().lexeme);
  if ((intrinsic == IntrinsicKind::MutexLock ||
       intrinsic == IntrinsicKind::MutexUnlock) &&
      !validSynchronizationOperation(synchronization)) {
    report(expr.paren(),
           "This private mutex operation has no exact synchronization "
           "contract.",
           "GTI-S2087");
  }
  semanticModel.record(
      expr, ResolvedCallInfo{.returnType = read ? SemanticType::referenceTo(
                                                      *elementType, readAccess)
                                                : currentType,
                             .parameterTypes = std::move(parameterTypes),
                             .typeArguments = {*elementType},
                             .intrinsic = intrinsic,
                             .synchronization = synchronization,
                             .borrowOrigin = read ? BorrowOriginKind::Argument
                                                  : BorrowOriginKind::None,
                             .borrowArgument = 0,
                             .borrowAccess = readAccess});
}

auto SemanticVisitor::Impl::analyzeSourceLocationIntrinsicCall(
    const Call &expr, const CallableCandidate &declaration) -> void {
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  for (const ExprPtr &argument : expr.arguments()) {
    (void)analyze(argument);
  }
  const IntrinsicKind intrinsic = declaration.record().intrinsic;
  const bool text = intrinsic == IntrinsicKind::SourceFileName ||
                    intrinsic == IntrinsicKind::SourceFunctionName;
  const SemanticType result =
      text ? SemanticType::StringView : SemanticType::UInt32;
  if (!expr.typeArguments().empty() || !expr.arguments().empty() ||
      declaration.returnType != result || !declaration.parameterTypes.empty()) {
    report(expr.paren(),
           "A source-location capability takes no arguments and must retain "
           "its exact standard-library result type.",
           "GTI-S2019");
  }
  currentType = result;
  semanticModel.record(
      expr, ResolvedCallInfo{.returnType = result, .intrinsic = intrinsic});
}

// Reflection queries are constants of their generic arguments. With concrete
// arguments the answer is recorded as a program constant on the call, so the
// call is a literal at run time and in constant evaluation; with symbolic
// arguments the call stays a call until concrete reanalysis substitutes them.
namespace {

// The generic-argument shape of a folded reflection query: `types` leading
// type arguments (the subject, then an attribute type), then one `uint64_t`
// field index when `index` is set.
struct MetaQueryShape {
  std::size_t types = 1;
  std::size_t indices = 0;
  SemanticType result = SemanticType::UInt64;
  // The result is the subject type itself (an enumerator constant).
  bool subjectResult = false;
};

[[nodiscard]] std::optional<MetaQueryShape>
metaQueryShape(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::MetaFieldCount:
    return MetaQueryShape{};
  case IntrinsicKind::MetaFieldName:
    return MetaQueryShape{.indices = 1, .result = SemanticType::StringView};
  case IntrinsicKind::MetaFieldIsPublic:
  case IntrinsicKind::MetaFieldHasDefault:
    return MetaQueryShape{.indices = 1, .result = SemanticType::Bool};
  case IntrinsicKind::MetaTypeName:
    return MetaQueryShape{.result = SemanticType::StringView};
  case IntrinsicKind::MetaTypeKind:
    return MetaQueryShape{};
  case IntrinsicKind::MetaFieldAttributeCount:
    return MetaQueryShape{.indices = 1};
  case IntrinsicKind::MetaFieldHasAttribute:
    return MetaQueryShape{
        .types = 2, .indices = 1, .result = SemanticType::Bool};
  case IntrinsicKind::MetaTypeAttributeCount:
    return MetaQueryShape{};
  case IntrinsicKind::MetaTypeHasAttribute:
    return MetaQueryShape{.types = 2, .result = SemanticType::Bool};
  case IntrinsicKind::MetaTypeTrait:
    return MetaQueryShape{.indices = 1, .result = SemanticType::Bool};
  case IntrinsicKind::MetaTypeRelation:
    return MetaQueryShape{
        .types = 2, .indices = 1, .result = SemanticType::Bool};
  case IntrinsicKind::MetaEnumeratorCount:
    return MetaQueryShape{};
  case IntrinsicKind::MetaEnumeratorName:
    return MetaQueryShape{.indices = 1, .result = SemanticType::StringView};
  case IntrinsicKind::MetaEnumeratorValue:
    return MetaQueryShape{.indices = 1, .subjectResult = true};
  case IntrinsicKind::MetaPayloadCount:
    return MetaQueryShape{.indices = 1};
  case IntrinsicKind::MetaPayloadName:
    return MetaQueryShape{.indices = 2, .result = SemanticType::StringView};
  case IntrinsicKind::MetaBaseCount:
  case IntrinsicKind::MetaArgumentCount:
  case IntrinsicKind::MetaMethodCount:
  case IntrinsicKind::MetaConstructorCount:
    return MetaQueryShape{};
  case IntrinsicKind::MetaMethodName:
    return MetaQueryShape{.indices = 1, .result = SemanticType::StringView};
  case IntrinsicKind::MetaMethodTrait:
    return MetaQueryShape{.indices = 2, .result = SemanticType::Bool};
  case IntrinsicKind::MetaMethodParameterCount:
  case IntrinsicKind::MetaConstructorParameterCount:
    return MetaQueryShape{.indices = 1};
  case IntrinsicKind::MetaParameterName:
  case IntrinsicKind::MetaConstructorParameterName:
    return MetaQueryShape{.indices = 2, .result = SemanticType::StringView};
  case IntrinsicKind::MetaDeclarationLine:
    return MetaQueryShape{.indices = 2};
  case IntrinsicKind::MetaDeclarationFile:
  case IntrinsicKind::MetaTypePrimaryName:
    return MetaQueryShape{.result = SemanticType::StringView};
  default:
    return std::nullopt;
  }
}

} // namespace

auto SemanticVisitor::Impl::metaHasStableDeclarations(
    const SemanticType &subject) const -> bool {
  const auto stable = [&](const auto &self, const SemanticType &subject,
                          std::size_t depth) -> bool {
    // Invalid cyclic bases may still be present during diagnostic recovery.
    if (depth > classes.size())
      return false;
    if (!containsOpenTypeParameter(subject))
      return true;
    const auto *owner = classInfo(subject);
    if (subject.kind != SemanticType::Class || !owner ||
        owner->compilerType != CompilerTypeKind::None ||
        owner->definitionFamilyMembers.size() > 1 ||
        (owner->definitionFamilyPrimary != 0 &&
         owner->definitionFamilyPrimary != owner->id) ||
        std::any_of(exactClassSpecializations.begin(),
                    exactClassSpecializations.end(),
                    [&](const ExactClassSpecialization &specialization) {
                      return specialization.primary == owner->id;
                    }))
      return false;
    const auto substitution = classSubstitution(subject);
    for (const auto &base : owner->bases)
      if (!self(self, substituteType(base.type, substitution), depth + 1))
        return false;
    for (const auto *declaration : metaMethodDeclarations(subject)) {
      const auto *function = semanticModel.findFunction(*declaration);
      if (!function)
        return false;
      // Conversion names include their target spelling. An unexpanded pack
      // also prevents a fixed parameter inventory from being established.
      if (declaration->operatorName() &&
          declaration->operatorName()->kind == OverloadedOperator::Conversion &&
          containsOpenTypeParameter(
              substituteType(function->returnType, substitution)))
        return false;
      for (const auto &parameter : function->parameterTypes)
        if (substituteType(parameter, substitution).kind ==
            SemanticType::TypePack)
          return false;
    }
    // A known class declaration has a fixed member inventory even when the
    // types in its signatures remain symbolic. Alternate definitions and
    // dependent bases must first be selected by ordinary instantiation.
    return true;
  };
  return stable(stable, subject, 0);
}

auto SemanticVisitor::Impl::metaQueryAcceptsOpenSubject(
    IntrinsicKind intrinsic, const SemanticType &subject) const -> bool {
  switch (intrinsic) {
  case IntrinsicKind::MetaTypeRelation:
    // The relation kernel distinguishes a proof from an unknown symbolic
    // relationship. In particular, equal symbolic identities are still equal.
    return true;
  case IntrinsicKind::MetaMethodCount:
  case IntrinsicKind::MetaMethodName:
  case IntrinsicKind::MetaMethodTrait:
  case IntrinsicKind::MetaMethodParameterCount:
  case IntrinsicKind::MetaParameterName:
    return metaHasStableDeclarations(subject);
  default:
    break;
  }
  if (intrinsic != IntrinsicKind::MetaTypeKind ||
      subject.kind == SemanticType::Unknown ||
      subject.kind == SemanticType::TypeParameter ||
      subject.kind == SemanticType::TypePack)
    return false;
  if (subject.kind == SemanticType::Enum &&
      (subject.enumId == 0 || subject.enumId > enums.size()))
    return false;
  if (subject.kind == SemanticType::Class) {
    const auto *owner = classInfo(subject);
    if (owner == nullptr || owner->compilerType != CompilerTypeKind::None)
      return false;
  }
  // The outer category is independent of nested type/value arguments. Exact
  // class specializations must retain their primary's class/struct kind.
  // Unknown/computed projections still have no category proof.
  return metaTypeKind(subject) != 0;
}

// The constant a concrete reflection query denotes, or nullopt when the field
// index is out of range (the caller reports it).
auto SemanticVisitor::Impl::metaQueryConstant(
    IntrinsicKind intrinsic, const SemanticType &subject,
    const std::optional<SemanticType> &attribute,
    std::optional<std::uint64_t> index,
    std::optional<std::uint64_t> second) const -> std::optional<ConstantValue> {
  const auto unsignedConstant = [](std::uint64_t value) {
    return ConstantValue{ConstantInteger{
        .magnitude = value, .domain = CheckedIntegerDomain{.width = 64}}};
  };
  switch (intrinsic) {
  case IntrinsicKind::MetaTypeTrait: {
    if (!index) {
      return std::nullopt;
    }
    if (const auto constraint = metaTypeTraitConstraint(*index)) {
      return ConstantValue{satisfiesAtomicConstraint(subject, *constraint)};
    }
    switch (*index) {
    case 0:
      return ConstantValue{
          subject.kind == SemanticType::Enum && subject.enumId != 0 &&
          subject.enumId <= enums.size() && enums[subject.enumId - 1].payload};
    case 1:
      return ConstantValue{
          (subject.kind == SemanticType::Class ||
           subject.kind == SemanticType::Enum) &&
          (!subject.arguments.empty() || !subject.valueArguments.empty())};
    case 22:
      return ConstantValue{requiresActiveCleanupFor(subject)};
    case 23: {
      const ClassInfo *owner =
          subject.kind == SemanticType::Class ? classInfo(subject) : nullptr;
      return ConstantValue{owner && owner->polymorphic};
    }
    default:
      return std::nullopt;
    }
  }
  case IntrinsicKind::MetaTypeRelation:
    if (!attribute || !index) {
      return std::nullopt;
    }
    switch (*index) {
    case 0: {
      if (subject == *attribute)
        return ConstantValue{true};
      if (containsOpenTypeParameter(subject) ||
          containsOpenTypeParameter(*attribute)) {
        const auto knownOuterKind = [&](const SemanticType &type) {
          if (type.kind == SemanticType::Unknown ||
              type.kind == SemanticType::TypeParameter ||
              type.kind == SemanticType::TypePack)
            return false;
          const auto *owner = classInfo(type);
          return type.kind != SemanticType::Class ||
                 (owner && owner->compilerType == CompilerTypeKind::None);
        };
        // Do not confuse unequal symbolic identities (T and U) with a proof
        // of inequality: instantiation can make them equal. Different known
        // outer constructors, such as I and I&, cannot become the same type.
        if (subject.kind != attribute->kind && knownOuterKind(subject) &&
            knownOuterKind(*attribute))
          return ConstantValue{false};
        if (subject.kind == SemanticType::Class &&
            attribute->kind == SemanticType::Class &&
            subject.classId != attribute->classId &&
            metaHasStableDeclarations(subject) &&
            metaHasStableDeclarations(*attribute))
          return ConstantValue{false};
        if ((subject.kind == SemanticType::Reference ||
             subject.kind == SemanticType::RawPointer) &&
            subject.kind == attribute->kind && subject.arguments.size() == 1 &&
            attribute->arguments.size() == 1) {
          if ((subject.kind == SemanticType::Reference &&
               subject.referenceAccess != attribute->referenceAccess) ||
              (subject.kind == SemanticType::RawPointer &&
               subject.pointerAccess != attribute->pointerAccess))
            return ConstantValue{false};
          return metaQueryConstant(intrinsic, subject.arguments.front(),
                                   attribute->arguments.front(), index, second);
        }
        return std::nullopt;
      }
      return ConstantValue{subject == *attribute};
    }
    case 1: {
      if (containsOpenTypeParameter(subject) ||
          containsOpenTypeParameter(*attribute))
        return std::nullopt;
      const std::array<SemanticType, 2> arguments{subject, *attribute};
      return ConstantValue{structuralConstraintSatisfied(
          StructuralConstraintKind::EnumUnderlying, arguments)};
    }
    case 2:
      if (containsOpenTypeParameter(subject) ||
          containsOpenTypeParameter(*attribute))
        return std::nullopt;
      return ConstantValue{subject.kind == SemanticType::Class &&
                           attribute->kind == SemanticType::Class &&
                           isPublicBaseConversion(subject, *attribute)};
    default:
      return std::nullopt;
    }
  case IntrinsicKind::MetaEnumeratorCount:
    return unsignedConstant(metaEnumeratorCount(subject));
  case IntrinsicKind::MetaBaseCount: {
    const ClassInfo *owner =
        subject.kind == SemanticType::Class ? classInfo(subject) : nullptr;
    return unsignedConstant(owner == nullptr ? 0 : owner->bases.size());
  }
  case IntrinsicKind::MetaArgumentCount:
    return unsignedConstant(metaTypeArguments(subject).size());
  case IntrinsicKind::MetaMethodCount:
    return unsignedConstant(metaMethodDeclarations(subject).size());
  case IntrinsicKind::MetaConstructorCount: {
    const ClassInfo *owner =
        subject.kind == SemanticType::Class ? classInfo(subject) : nullptr;
    return unsignedConstant(owner == nullptr ? 0 : owner->constructors.size());
  }
  case IntrinsicKind::MetaMethodName:
  case IntrinsicKind::MetaMethodTrait:
  case IntrinsicKind::MetaMethodParameterCount:
  case IntrinsicKind::MetaParameterName: {
    const std::optional<MetaMethodFacts> facts =
        index ? metaMethodFacts(subject, *index) : std::nullopt;
    if (!facts) {
      return std::nullopt;
    }
    switch (intrinsic) {
    case IntrinsicKind::MetaMethodName:
      return ConstantValue{facts->name};
    case IntrinsicKind::MetaMethodParameterCount:
      return unsignedConstant(facts->parameterTypes.size());
    case IntrinsicKind::MetaMethodTrait:
      switch (second.value_or(99)) {
      case 0:
        return ConstantValue{facts->staticMember};
      case 1:
        return ConstantValue{facts->operatorMethod};
      case 2:
        return ConstantValue{facts->constexprFunction};
      case 3:
        return ConstantValue{facts->virtualMethod};
      case 4:
        return ConstantValue{facts->mutableReceiver};
      case 5:
        return ConstantValue{facts->publicAccess};
      case 6:
        return ConstantValue{facts->consumingReceiver};
      case 7:
        return ConstantValue{facts->requiresUnsafe};
      case 8:
        return ConstantValue{facts->generic};
      case 9:
        return ConstantValue{facts->constrained};
      default:
        return std::nullopt;
      }
    default:
      return second && *second < facts->parameterNames.size()
                 ? std::optional(ConstantValue{facts->parameterNames[*second]})
                 : std::nullopt;
    }
  }
  case IntrinsicKind::MetaConstructorParameterCount:
  case IntrinsicKind::MetaConstructorParameterName: {
    const std::optional<MetaConstructorFacts> facts =
        index ? metaConstructorFacts(subject, *index) : std::nullopt;
    if (!facts) {
      return std::nullopt;
    }
    if (intrinsic == IntrinsicKind::MetaConstructorParameterCount) {
      return unsignedConstant(facts->parameterTypes.size());
    }
    return second && *second < facts->parameterNames.size()
               ? std::optional(ConstantValue{facts->parameterNames[*second]})
               : std::nullopt;
  }
  case IntrinsicKind::MetaDeclarationLine: {
    const std::optional<std::uint64_t> line =
        metaDeclarationLine(subject, index.value_or(99), second.value_or(0));
    return line ? std::optional(unsignedConstant(*line)) : std::nullopt;
  }
  case IntrinsicKind::MetaDeclarationFile:
    return ConstantValue{metaDeclarationFile(subject)};
  case IntrinsicKind::MetaTypePrimaryName: {
    // The declaration's qualified name: a generic instance names its primary,
    // every other type its own spelling.
    if (subject.kind == SemanticType::Class) {
      const ClassInfo *owner = classInfo(subject);
      if (owner != nullptr) {
        return ConstantValue{
            qualifiedName(owner->namespaceScope, owner->name.lexeme)};
      }
    } else if (subject.kind == SemanticType::Enum && subject.enumId != 0 &&
               subject.enumId <= enums.size()) {
      const EnumInfo &info = enums[subject.enumId - 1];
      return ConstantValue{
          qualifiedName(info.namespaceScope, info.name.lexeme)};
    }
    return ConstantValue{typeSpelling(subject)};
  }
  case IntrinsicKind::MetaEnumeratorName:
  case IntrinsicKind::MetaEnumeratorValue:
  case IntrinsicKind::MetaPayloadCount:
  case IntrinsicKind::MetaPayloadName: {
    const std::optional<MetaEnumeratorFacts> facts =
        index ? metaEnumeratorFacts(subject, *index) : std::nullopt;
    if (!facts) {
      return std::nullopt;
    }
    switch (intrinsic) {
    case IntrinsicKind::MetaEnumeratorName:
      return ConstantValue{facts->name};
    case IntrinsicKind::MetaEnumeratorValue:
      return facts->payloadEnum ? std::nullopt : std::optional(facts->value);
    case IntrinsicKind::MetaPayloadCount:
      return unsignedConstant(facts->payloadTypes.size());
    default:
      return second && *second < facts->payloadNames.size()
                 ? std::optional(ConstantValue{facts->payloadNames[*second]})
                 : std::nullopt;
    }
  }
  case IntrinsicKind::MetaFieldCount:
    return unsignedConstant(metaFieldCount(subject));
  case IntrinsicKind::MetaTypeKind:
    return unsignedConstant(metaTypeKind(subject));
  case IntrinsicKind::MetaTypeName:
    return ConstantValue{typeSpelling(subject)};
  case IntrinsicKind::MetaFieldName:
  case IntrinsicKind::MetaFieldIsPublic:
  case IntrinsicKind::MetaFieldHasDefault: {
    const std::optional<MetaFieldFacts> field =
        index ? metaFieldFacts(subject, *index) : std::nullopt;
    if (!field) {
      return std::nullopt;
    }
    if (intrinsic == IntrinsicKind::MetaFieldHasDefault) {
      return ConstantValue{field->declaration != nullptr &&
                           field->declaration->initializer() != nullptr};
    }
    return intrinsic == IntrinsicKind::MetaFieldName
               ? ConstantValue{field->name}
               : ConstantValue{field->isPublic};
  }
  case IntrinsicKind::MetaFieldAttributeCount:
  case IntrinsicKind::MetaTypeAttributeCount: {
    const std::vector<ResolvedDeclarationAttribute> *attributes =
        declarationAttributesOf(subject, index);
    if (attributes == nullptr) {
      return intrinsic == IntrinsicKind::MetaFieldAttributeCount
                 ? std::nullopt
                 : std::optional(unsignedConstant(0));
    }
    return unsignedConstant(attributes->size());
  }
  case IntrinsicKind::MetaFieldHasAttribute:
  case IntrinsicKind::MetaTypeHasAttribute: {
    const std::vector<ResolvedDeclarationAttribute> *attributes =
        declarationAttributesOf(subject, index);
    if (attributes == nullptr) {
      return intrinsic == IntrinsicKind::MetaFieldHasAttribute
                 ? std::nullopt
                 : std::optional(ConstantValue{false});
    }
    const bool found =
        attribute && attribute->kind == SemanticType::Class &&
        std::any_of(attributes->begin(), attributes->end(),
                    [&](const ResolvedDeclarationAttribute &candidate) {
                      return candidate.type == attribute->classId;
                    });
    return ConstantValue{found};
  }
  case IntrinsicKind::MetaFieldAttribute:
  case IntrinsicKind::MetaTypeAttribute: {
    const std::vector<ResolvedDeclarationAttribute> *attributes =
        declarationAttributesOf(subject, index);
    if (attributes == nullptr || !attribute ||
        attribute->kind != SemanticType::Class) {
      return std::nullopt;
    }
    const auto found =
        std::find_if(attributes->begin(), attributes->end(),
                     [&](const ResolvedDeclarationAttribute &candidate) {
                       return candidate.type == attribute->classId;
                     });
    return found == attributes->end() ? std::nullopt
                                      : std::optional(found->value);
  }
  default:
    return std::nullopt;
  }
}

auto SemanticVisitor::Impl::metaQueryFailure(
    IntrinsicKind intrinsic, const SemanticType &subject,
    std::optional<std::uint64_t> index,
    std::optional<std::uint64_t> second) const
    -> std::pair<std::string, std::string> {
  switch (intrinsic) {
  case IntrinsicKind::MetaEnumeratorValue:
    if (metaEnumeratorFacts(subject, index.value_or(0))) {
      return {"GTI-S2117",
              "Enumerator " + std::to_string(index.value_or(0)) + " of '" +
                  typeSpelling(subject) +
                  "' has no integral value; a payload enum's alternatives "
                  "are not integer constants."};
    }
    [[fallthrough]];
  case IntrinsicKind::MetaEnumeratorName:
  case IntrinsicKind::MetaPayloadCount:
  case IntrinsicKind::MetaPayloadName: {
    const std::optional<MetaEnumeratorFacts> facts =
        metaEnumeratorFacts(subject, index.value_or(0));
    if (subject.kind != SemanticType::Enum) {
      return {"GTI-S2117", "'" + typeSpelling(subject) +
                               "' is not an enum; enumerator reflection "
                               "applies to enum types only."};
    }
    if (!facts) {
      return {"GTI-S2111", "Enumerator index " +
                               std::to_string(index.value_or(0)) +
                               " is out of range for '" +
                               typeSpelling(subject) + "', which has " +
                               std::to_string(metaEnumeratorCount(subject)) +
                               " enumerator(s)."};
    }
    return {"GTI-S2111", "Payload index " + std::to_string(second.value_or(0)) +
                             " is out of range for enumerator '" + facts->name +
                             "' of '" + typeSpelling(subject) +
                             "', which has " +
                             std::to_string(facts->payloadTypes.size()) +
                             " payload field(s)."};
  }
  case IntrinsicKind::MetaTypeTrait:
    return {"GTI-S2019", "Unknown reflection trait selector " +
                             std::to_string(index.value_or(0)) + "."};
  case IntrinsicKind::MetaTypeRelation:
    return {"GTI-S2019", "Unknown reflection type-relation selector " +
                             std::to_string(index.value_or(0)) + "."};
  case IntrinsicKind::MetaMethodName:
  case IntrinsicKind::MetaMethodTrait:
  case IntrinsicKind::MetaMethodParameterCount:
  case IntrinsicKind::MetaParameterName: {
    const std::optional<MetaMethodFacts> facts =
        metaMethodFacts(subject, index.value_or(0));
    if (!facts) {
      return {"GTI-S2111",
              "Method index " + std::to_string(index.value_or(0)) +
                  " is out of range for '" + typeSpelling(subject) +
                  "', which has " +
                  std::to_string(metaMethodDeclarations(subject).size()) +
                  " method(s)."};
    }
    if (intrinsic == IntrinsicKind::MetaMethodTrait) {
      return {"GTI-S2019", "Unknown method trait selector " +
                               std::to_string(second.value_or(0)) + "."};
    }
    return {"GTI-S2111",
            "Parameter index " + std::to_string(second.value_or(0)) +
                " is out of range for method '" + facts->name + "' of '" +
                typeSpelling(subject) + "', which has " +
                std::to_string(facts->parameterTypes.size()) +
                " parameter(s)."};
  }
  case IntrinsicKind::MetaConstructorParameterCount:
  case IntrinsicKind::MetaConstructorParameterName: {
    const std::optional<MetaConstructorFacts> facts =
        metaConstructorFacts(subject, index.value_or(0));
    const ClassInfo *owner =
        subject.kind == SemanticType::Class ? classInfo(subject) : nullptr;
    if (!facts) {
      return {"GTI-S2111",
              "Constructor index " + std::to_string(index.value_or(0)) +
                  " is out of range for '" + typeSpelling(subject) +
                  "', which has " +
                  std::to_string(
                      owner == nullptr ? 0 : owner->constructors.size()) +
                  " declared constructor(s)."};
    }
    return {"GTI-S2111", "Parameter index " +
                             std::to_string(second.value_or(0)) +
                             " is out of range for constructor " +
                             std::to_string(index.value_or(0)) + " of '" +
                             typeSpelling(subject) + "', which has " +
                             std::to_string(facts->parameterTypes.size()) +
                             " parameter(s)."};
  }
  case IntrinsicKind::MetaDeclarationLine:
    return {"GTI-S2111", "No reflected declaration of kind " +
                             std::to_string(index.value_or(0)) + " at index " +
                             std::to_string(second.value_or(0)) + " for '" +
                             typeSpelling(subject) + "'."};
  default:
    return {"GTI-S2111", "Field index " + std::to_string(index.value_or(0)) +
                             " is out of range for '" + typeSpelling(subject) +
                             "', which has " +
                             std::to_string(metaFieldCount(subject)) +
                             " instance field(s)."};
  }
}

auto SemanticVisitor::Impl::analyzeMetaIntrinsicCall(
    const Call &expr, const CallableCandidate &declaration) -> void {
  const IntrinsicKind intrinsic = declaration.record().intrinsic;
  for (const ExprPtr &argument : expr.arguments()) {
    (void)analyze(argument);
  }
  const std::optional<MetaQueryShape> shape = metaQueryShape(intrinsic);
  if (!shape) {
    currentType = SemanticType::Unknown;
    return;
  }
  const std::size_t expectedArguments = shape->types + shape->indices;
  std::optional<SemanticType> subject;
  std::optional<SemanticType> attribute;
  std::vector<CompileTimeValue> indices;
  bool indicesResolved = true;
  if (expr.typeArguments().size() == expectedArguments &&
      expr.arguments().empty()) {
    validateType(expr.typeArguments().front());
    subject = typeOf(expr.typeArguments().front());
    if (shape->types == 2) {
      validateType(expr.typeArguments()[1]);
      attribute = typeOf(expr.typeArguments()[1]);
    }
    for (std::size_t position = shape->types; position < expectedArguments;
         ++position) {
      const std::optional<CompileTimeValue> index =
          genericValueArgument(expr.typeArguments()[position]);
      if (!index) {
        indicesResolved = false;
        break;
      }
      indices.push_back(*index);
    }
  }
  const SemanticType result =
      shape->subjectResult && subject ? *subject : shape->result;
  if (!subject || *subject == SemanticType::Unknown ||
      (shape->types == 2 &&
       (!attribute || *attribute == SemanticType::Unknown)) ||
      !indicesResolved || indices.size() != shape->indices ||
      (!shape->subjectResult && declaration.returnType != shape->result)) {
    report(expr.paren(),
           "A reflection capability takes its subject as generic arguments, "
           "no call arguments, and must retain its exact standard-library "
           "result type.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  currentType = result;
  ResolvedCallInfo resolved{.returnType = result,
                            .typeArguments = {*subject},
                            .valueArguments = indices,
                            .intrinsic = intrinsic};
  if (attribute) {
    resolved.typeArguments.push_back(*attribute);
  }
  semanticModel.record(expr, resolved);
  if ((containsOpenTypeParameter(*subject) &&
       !metaQueryAcceptsOpenSubject(intrinsic, *subject)) ||
      (attribute && containsOpenTypeParameter(*attribute) &&
       intrinsic != IntrinsicKind::MetaTypeRelation) ||
      std::any_of(indices.begin(), indices.end(),
                  [](const CompileTimeValue &index) {
                    return index.kind != CompileTimeValue::UInt64;
                  })) {
    return;
  }
  const std::optional<std::uint64_t> index =
      indices.empty() ? std::nullopt : std::optional(indices[0].value);
  const std::optional<std::uint64_t> second =
      indices.size() < 2 ? std::nullopt : std::optional(indices[1].value);
  const std::optional<ConstantValue> constant =
      metaQueryConstant(intrinsic, *subject, attribute, index, second);
  if (!constant) {
    if (containsOpenTypeParameter(*subject) ||
        (attribute && containsOpenTypeParameter(*attribute)))
      return;
    const auto [code, message] =
        metaQueryFailure(intrinsic, *subject, index, second);
    report(expr.paren(), message, code);
    return;
  }
  semanticModel.recordConstant(expr, *constant);
  semanticModel.recordProgramConstantSubstitution(expr);
}

// The constant a reflection query denotes under the interpreter's current
// generic substitution, or nullopt when the call is not a reflection query or
// its arguments are still symbolic.
auto SemanticVisitor::Impl::metaIntrinsicConstant(
    const ResolvedCallInfo &call, const ConstexprInterpreter &context) const
    -> std::optional<ConstantValue> {
  std::size_t types = 1;
  std::size_t indices = 0;
  if (const std::optional<MetaQueryShape> shape =
          metaQueryShape(call.intrinsic)) {
    types = shape->types;
    indices = shape->indices;
  } else if (call.intrinsic == IntrinsicKind::MetaFieldAttribute ||
             call.intrinsic == IntrinsicKind::MetaTypeAttribute) {
    types = 2;
    indices = call.intrinsic == IntrinsicKind::MetaFieldAttribute ? 1 : 0;
  } else if (call.intrinsic == IntrinsicKind::MetaFieldDefault) {
    types = 1;
    indices = 1;
  } else {
    return std::nullopt;
  }
  if (call.typeArguments.empty()) {
    return std::nullopt;
  }
  if (call.typeArguments.size() != types ||
      call.valueArguments.size() != indices) {
    return std::nullopt;
  }
  const SemanticType subject =
      constexprType(call.typeArguments.front(), context);
  if (containsOpenTypeParameter(subject) &&
      !metaQueryAcceptsOpenSubject(call.intrinsic, subject)) {
    return std::nullopt;
  }
  std::optional<SemanticType> attribute;
  if (types == 2) {
    attribute = constexprType(call.typeArguments[1], context);
    if (containsOpenTypeParameter(*attribute) &&
        call.intrinsic != IntrinsicKind::MetaTypeRelation) {
      return std::nullopt;
    }
  }
  std::optional<std::uint64_t> index;
  std::optional<std::uint64_t> second;
  for (std::size_t position = 0; position < indices; ++position) {
    const CompileTimeValue value =
        constexprValue(call.valueArguments[position], context);
    if (value.kind != CompileTimeValue::UInt64) {
      return std::nullopt;
    }
    (position == 0 ? index : second) = value.value;
  }
  return metaQueryConstant(call.intrinsic, subject, attribute, index, second);
}

auto SemanticVisitor::Impl::analyzeRestoringIntrinsicCall(
    const Call &expr, IntrinsicKind intrinsic) -> void {
  std::vector<SemanticType> arguments;
  for (const ExprPtr &argument : expr.arguments()) {
    arguments.push_back(analyze(argument));
  }
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  const auto reject = [&](std::string message) {
    report(expr.paren(), std::move(message), "GTI-S2018");
    currentType = SemanticType::Unknown;
  };
  if (intrinsic == IntrinsicKind::RestoringIndexedSwap) {
    if (!expr.typeArguments().empty() || arguments.size() != 3 ||
        arguments.front().kind != SemanticType::Array ||
        arguments.front().arguments.size() != 1 ||
        arguments[1] != SemanticType::UInt64 ||
        arguments[2] != SemanticType::UInt64) {
      reject("Indexed restoring swap requires an initialized fixed array "
             "and two size_t indices.");
      return;
    }
    const SemanticType element = arguments.front().arguments.front();
    if (!satisfiesConstraint(
            element, constraintBit(GenericConstraintKind::Relocatable))) {
      reject(
          "Indexed restoring swap requires structurally relocatable elements.");
      return;
    }
    const auto *place = semanticModel.findExpression(*expr.arguments().front());
    if (!place || place->category != ValueCategory::Place ||
        place->access != AccessMode::Mutable) {
      reject("Indexed restoring swap requires initialized writable storage.");
      return;
    }
    arguments.front() =
        SemanticType::referenceTo(arguments.front(), AccessMode::Mutable);
    currentType = SemanticType::Void;
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = std::move(arguments),
                               .typeArguments = {element},
                               .intrinsic = intrinsic});
    return;
  }
  if (!expr.typeArguments().empty() || arguments.size() != 2) {
    reject("Restoring transfer requires two values and infers its exact type.");
    return;
  }
  const SemanticType type = arguments.front();
  if (type != arguments.back() ||
      !satisfiesConstraint(type,
                           constraintBit(GenericConstraintKind::Relocatable))) {
    reject("Restoring transfer requires matching structurally relocatable "
           "values; prepare an exact replacement before exchanging it.");
    return;
  }
  const bool swap = intrinsic == IntrinsicKind::RestoringSwap;
  for (const ExprPtr &argument : expr.arguments()) {
    if (isOwnerDependentTextExpression(argument)) {
      reportOwnerDependentTextStorage(argument,
                                      "exchanged through a mutable reference");
      currentType = SemanticType::Unknown;
      return;
    }
    noteLocalTextEscape(argument, LocalTextEscapeKind::Storage,
                        "exchanged through a mutable reference");
  }
  for (std::size_t index = 0; index < (swap ? 2u : 1u); ++index) {
    const ExpressionInfo *place =
        semanticModel.findExpression(*expr.arguments()[index]);
    if (place == nullptr || place->category != ValueCategory::Place ||
        place->access != AccessMode::Mutable) {
      report(expressionToken(expr.arguments()[index]),
             "Restoring transfer requires an initialized writable place.",
             "GTI-S2018");
    }
    arguments[index] = SemanticType::referenceTo(type, AccessMode::Mutable);
  }
  currentType = swap ? SemanticType::Void : type;
  semanticModel.record(expr,
                       ResolvedCallInfo{.returnType = currentType,
                                        .parameterTypes = std::move(arguments),
                                        .typeArguments = {type},
                                        .intrinsic = intrinsic});
}

auto SemanticVisitor::Impl::runtimeObjectLayoutType(
    const SemanticType &type) const -> bool {
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
  case SemanticType::RawPointer:
  case SemanticType::StringView:
  case SemanticType::CString:
    return true;
  case SemanticType::Enum:
    return type.enumId != 0 && type.enumId <= enums.size() &&
           enumTypeFor(type).has_value();
  case SemanticType::Array:
    return type.arguments.size() == 1 && type.arrayLength > 0 &&
           type.arrayLengthParameterId == 0 &&
           runtimeObjectLayoutType(type.arguments.front());
  case SemanticType::Class: {
    const ClassInfo *owner = classInfo(type);
    return owner != nullptr && owner->declaration != nullptr &&
           !owner->declaration->isForwardDeclaration() &&
           owner->kind != ClassKind::Interface && !owner->abstract &&
           !owner->cOpaqueHandle &&
           owner->compilerCapability == CompilerCapabilityTypeKind::None;
  }
  default:
    return false;
  }
}

auto SemanticVisitor::Impl::analyzeIntrinsicCall(
    const Call &expr, const CallableCandidate &declaration) -> void {
  const IntrinsicKind intrinsic = declaration.record().intrinsic;
  if (intrinsic == IntrinsicKind::SourceExpressionText) {
    for (const TypeRef &argument : expr.typeArguments())
      validateType(argument);
    const bool hasFallback = expr.arguments().size() == 2;
    if ((expr.arguments().size() != 1 && !hasFallback) ||
        declaration.parameterTypes.size() != expr.arguments().size() ||
        (hasFallback &&
         declaration.parameterTypes.back() != SemanticType::StringView) ||
        declaration.returnType != SemanticType::StringView ||
        expr.typeArguments().size() != 1 ||
        declaration.record().genericParameters.size() != 1 ||
        declaration.record().genericParameters.front().value ||
        declaration.record().genericParameters.front().pack ||
        declaration.parameterTypes.front() !=
            SemanticType::typeParameter(
                declaration.record().genericParameters.front().id)) {
      report(expr.paren(),
             "An expression-text capability requires one typed expression, "
             "an optional string-view fallback and a string-view result.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    auto probe = makeMacroProbe(expr);
    (void)probe->analyze(expr.arguments().front());
    diagnostics.insert(diagnostics.end(), probe->diagnostics.begin(),
                       probe->diagnostics.end());
    if (hasErrorDiagnostics(probe->diagnostics)) {
      currentType = SemanticType::Unknown;
      return;
    }
    SemanticType actual = probe->currentType;
    if (actual.kind == SemanticType::Reference &&
        actual.arguments.size() == 1) {
      SemanticType referent = actual.arguments.front();
      actual = std::move(referent);
    }
    const SemanticType expected = typeOf(expr.typeArguments().front());
    if (actual != expected && actual != SemanticType::Unknown &&
        expected != SemanticType::Unknown) {
      report(expr.paren(),
             "Expression text requires the exact expression type as its "
             "generic argument.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    std::optional<std::string> fallback;
    if (hasFallback) {
      auto fallbackProbe = makeMacroProbe(expr);
      const SemanticType fallbackType =
          fallbackProbe->analyze(expr.arguments().back());
      diagnostics.insert(diagnostics.end(), fallbackProbe->diagnostics.begin(),
                         fallbackProbe->diagnostics.end());
      if (hasErrorDiagnostics(fallbackProbe->diagnostics)) {
        currentType = SemanticType::Unknown;
        return;
      }
      ConstexprInterpreter context = fallbackProbe->makeConstexprInterpreter();
      const auto constant = fallbackProbe->evaluateConstexprExpression(
          expr.arguments().back(), context);
      const auto *text =
          constant ? std::get_if<std::string>(&*constant.value) : nullptr;
      if (fallbackType != SemanticType::StringView || !text) {
        report(expr.paren(),
               "An expression-text fallback must be compile-time "
               "literal-backed string-view text.",
               "GTI-S2019");
        currentType = SemanticType::Unknown;
        return;
      }
      fallback = *text;
    }
    const auto &origin = expr.arguments().front()->writtenOrigin();
    const std::string *source =
        sources && origin ? sources->find(origin->source) : nullptr;
    const bool written = origin && source && origin->start < origin->end &&
                         origin->end <= source->size();
    if (!written && !fallback) {
      report(expr.paren(),
             "Expression text requires one complete written source "
             "expression; a synthetic or composed expansion has no exact "
             "written spelling.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = SemanticType::StringView;
    semanticModel.record(expr, ResolvedCallInfo{.returnType = currentType,
                                                .intrinsic = intrinsic});
    semanticModel.recordConstant(
        expr, written
                  ? source->substr(origin->start, origin->end - origin->start)
                  : *fallback);
    semanticModel.recordProgramConstantSubstitution(expr);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::EraseReference ||
      intrinsic == IntrinsicKind::EraseReferenceMut ||
      intrinsic == IntrinsicKind::RestoreReference ||
      intrinsic == IntrinsicKind::RestoreReferenceMut) {
    analyzeReferenceErasureCall(expr, intrinsic);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::FunctionPointerFromItem ||
      intrinsic == IntrinsicKind::FunctionPointerInvoke) {
    analyzeFunctionPointerCall(expr, intrinsic);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::TextViewFromAddress ||
      intrinsic == IntrinsicKind::CStringFromAddress) {
    const bool counted = intrinsic == IntrinsicKind::TextViewFromAddress;
    const std::size_t witness = counted ? 2 : 1;
    std::vector<SemanticType> arguments;
    for (const ExprPtr &argument : expr.arguments())
      arguments.push_back(analyze(argument));
    for (const TypeRef &argument : expr.typeArguments())
      validateType(argument);
    requireUnsafe(expr, UnsafeOperationKind::RawDereference, expr.paren(),
                  "Asserting an owner-dependent raw text view");
    const bool addressShape =
        arguments.size() == witness + 1 &&
        arguments.front().kind == SemanticType::RawPointer &&
        arguments.front().arguments ==
            std::vector<SemanticType>{SemanticType::Char} &&
        (!counted || arguments[1] == SemanticType::UInt64);
    const ExpressionInfo *owner =
        arguments.size() == witness + 1
            ? semanticModel.findExpression(*expr.arguments()[witness])
            : nullptr;
    const bool exactOwner =
        expr.typeArguments().empty() ||
        (expr.typeArguments().size() == 1 && arguments.size() == witness + 1 &&
         typeOf(expr.typeArguments().front()) == arguments[witness]);
    if (!addressShape || !exactOwner || owner == nullptr ||
        owner->category != ValueCategory::Place) {
      report(expr.paren(),
             counted ? "A raw text-view assertion requires a character "
                       "address, uint64_t count and one checked owner place."
                     : "A raw C-string assertion requires a character "
                       "address and one checked owner place.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    const SemanticType ownerType = arguments[witness];
    arguments.front() =
        SemanticType::rawPointerTo(SemanticType::Char, AccessMode::ReadOnly);
    arguments[witness] =
        SemanticType::referenceTo(ownerType, AccessMode::ReadOnly);
    currentType = counted ? SemanticType::StringView : SemanticType::CString;
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = std::move(arguments),
                               .typeArguments = {ownerType},
                               .intrinsic = intrinsic,
                               .borrowOrigin = BorrowOriginKind::Argument,
                               .borrowArgument = witness,
                               .borrowAccess = AccessMode::ReadOnly});
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::ObjectRelocate ||
      intrinsic == IntrinsicKind::ObjectRelocateRange) {
    const bool range = intrinsic == IntrinsicKind::ObjectRelocateRange;
    std::vector<SemanticType> arguments;
    for (const ExprPtr &argument : expr.arguments())
      arguments.push_back(analyze(argument));
    for (const TypeRef &argument : expr.typeArguments())
      validateType(argument);
    requireUnsafe(expr, UnsafeOperationKind::ObjectLifetime, expr.paren(),
                  "Relocating a raw object");
    const bool pointerShape =
        arguments.size() == (range ? 3 : 2) && expr.typeArguments().empty() &&
        arguments[0] == arguments[1] &&
        arguments[0].kind == SemanticType::RawPointer &&
        arguments[0].pointerAccess == AccessMode::Mutable &&
        arguments[0].arguments.size() == 1 &&
        (!range || arguments[2] == SemanticType::UInt64);
    if (!pointerShape) {
      report(
          expr.paren(),
          range
              ? "Range relocation requires two writable pointers to the "
                "same complete object type and a size_t count."
              : "Object relocation requires two writable pointers to the same "
                "complete object type.",
          "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    const auto &element = arguments[0].arguments.front();
    if (!containsOpenTypeParameter(element) &&
        (!runtimeObjectLayoutType(element) ||
         typeTraits(element).containsBorrowedState ||
         typeTraits(element).relocation == RelocationKind::Unavailable ||
         (range &&
          typeTraits(element).relocation != RelocationKind::Bitwise))) {
      report(expr.paren(),
             range ? "Range relocation requires a complete bitwise-relocatable "
                     "type without checked borrowed state."
                   : "Object relocation requires a complete relocatable type "
                     "without "
                     "checked borrowed state.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = SemanticType::Void;
    semanticModel.record(expr, ResolvedCallInfo{.returnType = currentType,
                                                .parameterTypes = arguments,
                                                .typeArguments = {element},
                                                .intrinsic = intrinsic});
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::ObjectDestroy ||
      intrinsic == IntrinsicKind::ObjectConstruct) {
    const bool construction = intrinsic == IntrinsicKind::ObjectConstruct;
    std::vector<SemanticType> arguments;
    for (const ExprPtr &argument : expr.arguments())
      arguments.push_back(analyze(argument));
    for (const TypeRef &argument : expr.typeArguments())
      validateType(argument);
    requireUnsafe(expr, UnsafeOperationKind::ObjectLifetime, expr.paren(),
                  construction ? "Constructing a raw object"
                               : "Ending a raw object's lifetime");
    if (arguments.empty() || (!construction && arguments.size() != 1) ||
        !expr.typeArguments().empty() ||
        arguments.front().kind != SemanticType::RawPointer ||
        arguments.front().pointerAccess != AccessMode::Mutable ||
        arguments.front().arguments.size() != 1 ||
        (!containsOpenTypeParameter(arguments.front().arguments.front()) &&
         !runtimeObjectLayoutType(arguments.front().arguments.front()))) {
      report(
          expr.paren(),
          construction
              ? "Object construction requires a writable pointer to a "
                "complete object type followed by constructor arguments."
              : "Object destruction requires one writable pointer to a "
                "complete "
                "object type; the element type is inferred from the pointer.",
          "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    const SemanticType element = arguments.front().arguments.front();
    if (construction) {
      if (!containsOpenTypeParameter(element) &&
          typeTraits(element).containsBorrowedState) {
        report(expr.paren(),
               "Raw construction cannot store checked borrowed state without "
               "a tracked destination owner.",
               "GTI-S2019");
        currentType = SemanticType::Unknown;
        return;
      }
      if (element.kind == SemanticType::StringView ||
          element.kind == SemanticType::CString) {
        for (std::size_t index = 1; index < expr.arguments().size(); ++index) {
          if (isOwnerDependentTextExpression(expr.arguments()[index])) {
            reportOwnerDependentTextStorage(expr.arguments()[index],
                                            "nested in raw storage");
          } else {
            noteLocalTextEscape(expr.arguments()[index],
                                LocalTextEscapeKind::Storage,
                                "nested in raw storage");
          }
        }
      }
      std::vector<AnalyzedCallArgument> constructorArguments;
      bool concrete = true;
      for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (!appendConcreteCallArgument(arguments[index],
                                        expr.arguments()[index],
                                        constructorArguments)) {
          concrete = false;
          break;
        }
      }
      if (concrete) {
        if (element.kind == SemanticType::Class)
          analyzeConstructorCallArguments(
              expr, element.classId, element.arguments, element.valueArguments,
              constructorArguments, expr.paren());
        else if (!containsOpenTypeParameter(element))
          validateStorageValueConstruction(expr, element, constructorArguments);
      }
      preserveStorageConstructorParameters(semanticModel, expr, 1, arguments);
    }
    currentType = SemanticType::Void;
    semanticModel.record(expr, ResolvedCallInfo{.returnType = currentType,
                                                .parameterTypes = arguments,
                                                .typeArguments = {element},
                                                .intrinsic = intrinsic});
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::ObjectSize ||
      intrinsic == IntrinsicKind::ObjectAlignment) {
    const std::size_t diagnosticsBefore = diagnostics.size();
    for (const ExprPtr &argument : expr.arguments())
      (void)analyze(argument);
    std::vector<SemanticType> types;
    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
      types.push_back(typeOf(argument));
    }
    if (diagnostics.size() != diagnosticsBefore ||
        (types.size() == 1 && types.front() == SemanticType::Unknown)) {
      currentType = SemanticType::Unknown;
      return;
    }
    if (types.size() != 1 || !expr.arguments().empty() ||
        (!containsOpenTypeParameter(types.front()) &&
         !runtimeObjectLayoutType(types.front()))) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2063", DiagnosticPhase::Semantics,
          expr.typeArguments().size() == 1
              ? layoutDiagnosticSpan(expr.typeArguments().front(),
                                     LayoutEvaluationError::UnsupportedType)
              : tokenSpan(expr.paren()),
          "Runtime object layout requires one complete object type.");
      diagnostic.hints.emplace_back(
          "Use a scalar, raw pointer, integral enum, concrete positive array, "
          "or complete non-abstract class, struct or passive union. This query "
          "does not require copying or moving the type.");
      diagnostics.emplace_back(std::move(diagnostic));
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = SemanticType::UInt64;
    semanticModel.record(expr,
                         ResolvedCallInfo{.returnType = currentType,
                                          .typeArguments = std::move(types),
                                          .intrinsic = intrinsic});
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::EnumUnderlyingValue) {
    std::vector<SemanticType> arguments;
    for (const ExprPtr &argument : expr.arguments())
      arguments.push_back(analyze(argument));
    std::vector<SemanticType> types;
    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
      types.push_back(typeOf(argument));
    }
    const bool symbolic =
        types.size() == 2 && (containsOpenTypeParameter(types[0]) ||
                              containsOpenTypeParameter(types[1]));
    const bool exact =
        types.size() == 2 && arguments.size() == 1 &&
        arguments[0] == types[1] &&
        (symbolic || structuralConstraintSatisfied(
                         StructuralConstraintKind::EnumUnderlying,
                         std::array<SemanticType, 2>{types[1], types[0]}));
    if (!exact) {
      report(expr.paren(),
             "Enum extraction requires an integral enum and its exact "
             "underlying integer type.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = types[0];
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = std::move(arguments),
                               .typeArguments = std::move(types),
                               .intrinsic = intrinsic});
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::RestoringExchange ||
      intrinsic == IntrinsicKind::RestoringSwap ||
      intrinsic == IntrinsicKind::RestoringIndexedSwap) {
    analyzeRestoringIntrinsicCall(expr, intrinsic);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (simdIntrinsic(intrinsic)) {
    std::vector<SemanticType> arguments;
    for (const ExprPtr &argument : expr.arguments()) {
      arguments.push_back(analyze(argument));
    }
    for (const TypeRef &argument : expr.typeArguments())
      validateType(argument);
    const auto validArray = [](const SemanticType &type) {
      return type.kind == SemanticType::Array && type.arguments.size() == 1 &&
             (type.arrayLengthParameterId != 0 ||
              (type.arrayLength >= 1 && type.arrayLength <= 64));
    };
    const bool shape = arguments.size() == 3 && validArray(arguments[0]) &&
                       arguments[0] == arguments[1] && validArray(arguments[2]);
    const bool lane =
        shape &&
        (arguments[0].arguments[0] == SemanticType::Float ||
         arguments[0].arguments[0] == SemanticType::Double ||
         arguments[0].arguments[0].kind == SemanticType::TypeParameter);
    const bool output =
        shape && arguments[0].arrayLength == arguments[2].arrayLength &&
        arguments[0].arrayLengthParameterId ==
            arguments[2].arrayLengthParameterId &&
        arguments[2].arguments[0] == (simdComparisonIntrinsic(intrinsic)
                                          ? SemanticType::Bool
                                          : arguments[0].arguments[0]);
    if (!expr.typeArguments().empty() || !shape || !lane || !output) {
      report(expr.paren(),
             "SIMD operations require matching float or double arrays with "
             "1 to 64 lanes and an equally sized mutable output array "
             "(bool lanes for comparisons).",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    requireMutableStorage(expr.arguments()[2], "SIMD output");
    std::vector<SemanticType> parameters;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      parameters.push_back(SemanticType::referenceTo(
          arguments[index],
          index == 2 ? AccessMode::Mutable : AccessMode::ReadOnly));
    }
    currentType = SemanticType::Void;
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = std::move(parameters),
                               .intrinsic = intrinsic});
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::SourceFileName ||
      intrinsic == IntrinsicKind::SourceFunctionName ||
      intrinsic == IntrinsicKind::SourceLine ||
      intrinsic == IntrinsicKind::SourceColumn) {
    analyzeSourceLocationIntrinsicCall(expr, declaration);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::MetaConstruct) {
    analyzeMetaConstructCall(expr);
    return;
  }
  if (intrinsic >= IntrinsicKind::MetaFieldCount &&
      intrinsic <= IntrinsicKind::MetaTypePrimaryName) {
    analyzeMetaIntrinsicCall(expr, declaration);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic >= IntrinsicKind::SharedOwnerEmpty &&
      intrinsic <= IntrinsicKind::SharedOwnerUpcast) {
    analyzeSharedOwnerIntrinsicCall(expr, declaration);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsicTerminalFailureOutcome(intrinsic)) {
    const bool panic = intrinsic == IntrinsicKind::Panic;
    std::vector<SemanticType> argumentTypes;
    for (const auto &argument : expr.arguments()) {
      argumentTypes.push_back(analyze(argument));
    }
    for (const auto &argument : expr.typeArguments()) {
      validateType(argument);
    }
    if (!expr.typeArguments().empty() ||
        (panic ? argumentTypes !=
                     std::vector<SemanticType>{SemanticType::StringView}
               : !argumentTypes.empty())) {
      report(expr.paren(),
             panic ? "Panic requires one counted text-view message and no type "
                     "arguments."
                   : "Terminal owner failure requires no arguments or type "
                     "arguments.",
             "GTI-S2019");
    }
    currentType = SemanticType::Void;
    semanticModel.record(
        expr,
        ResolvedCallInfo{.returnType = SemanticType::Void,
                         .parameterTypes = std::move(argumentTypes),
                         .intrinsic = intrinsic,
                         .neverReturns = true,
                         .tracksCaller = intrinsicTracksCaller(intrinsic)});
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsicBooleanFailureOutcome(intrinsic)) {
    std::vector<SemanticType> argumentTypes;
    for (const auto &argument : expr.arguments()) {
      argumentTypes.push_back(analyze(argument));
    }
    for (const auto &argument : expr.typeArguments()) {
      validateType(argument);
    }
    if (!expr.typeArguments().empty() || argumentTypes.size() != 1 ||
        argumentTypes.front() != SemanticType::Bool) {
      report(expr.paren(),
             "A container contract check requires exactly one bool condition "
             "and no type arguments.",
             "GTI-S2019");
    }
    currentType = SemanticType::Void;
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = SemanticType::Void,
                               .parameterTypes = std::move(argumentTypes),
                               .intrinsic = intrinsic});
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (integerArithmeticIntrinsic(intrinsic)) {
    analyzeIntegerArithmeticIntrinsicCall(expr, declaration);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::AllocateUniqueOwner ||
      intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
      intrinsic == IntrinsicKind::UniqueOwnerBorrowMut ||
      intrinsic == IntrinsicKind::UniqueOwnerIsNull ||
      intrinsic == IntrinsicKind::UniqueOwnerSwap ||
      intrinsic == IntrinsicKind::UniqueOwnerUpcast) {
    analyzeUniqueOwnerIntrinsicCall(expr, intrinsic);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::StorageBoundsCheck) {
    analyzeStorageBoundsCheckCall(expr);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::ThreadScope) {
    analyzeThreadScopeCall(expr);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::ThreadHandleEmpty ||
      intrinsic == IntrinsicKind::ThreadSpawn ||
      intrinsic == IntrinsicKind::ThreadJoinable ||
      intrinsic == IntrinsicKind::ThreadJoin ||
      intrinsic == IntrinsicKind::ThreadResultTake ||
      intrinsic == IntrinsicKind::ThreadAutoJoin) {
    analyzeThreadIntrinsicCall(expr, declaration);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::AtomicLoad ||
      intrinsic == IntrinsicKind::AtomicWait ||
      intrinsic == IntrinsicKind::AtomicNotifyOne ||
      intrinsic == IntrinsicKind::AtomicNotifyAll ||
      intrinsic == IntrinsicKind::AtomicStore ||
      intrinsic == IntrinsicKind::AtomicExchange ||
      intrinsic == IntrinsicKind::AtomicCompareExchange) {
    analyzeAtomicIntrinsicCall(expr, declaration);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::MutexLock ||
      intrinsic == IntrinsicKind::MutexUnlock ||
      intrinsic == IntrinsicKind::MutexGuardActive ||
      intrinsic == IntrinsicKind::MutexGuardRead ||
      intrinsic == IntrinsicKind::MutexGuardReadMut ||
      intrinsic == IntrinsicKind::MutexLockFailure) {
    analyzeMutexIntrinsicCall(expr, declaration);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::OptionalStorageHasValue ||
      intrinsic == IntrinsicKind::OptionalStorageConstruct ||
      intrinsic == IntrinsicKind::OptionalStorageRead ||
      intrinsic == IntrinsicKind::OptionalStorageReadMut ||
      intrinsic == IntrinsicKind::OptionalStorageReset) {
    analyzeOptionalStorageIntrinsicCall(expr, intrinsic);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::PolymorphicStorageHasValue ||
      intrinsic == IntrinsicKind::PolymorphicStorageConstruct ||
      intrinsic == IntrinsicKind::PolymorphicStorageRead ||
      intrinsic == IntrinsicKind::PolymorphicStorageReadMut ||
      intrinsic == IntrinsicKind::PolymorphicStorageReset) {
    analyzePolymorphicStorageIntrinsicCall(expr, intrinsic);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::Move) {
    analyzeMoveCall(expr, &declaration);
    return;
  }
  throw std::logic_error("unhandled compiler intrinsic in semantic analysis");
}

auto SemanticVisitor::Impl::analyzeMoveCall(
    const Call &expr, const CallableCandidate *declaration,
    bool immediateReceiver) -> void {

  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (!expr.typeArguments().empty()) {
    report(expr.paren(), "std::move does not take type arguments.",
           "GTI-S2018");
  }
  if (expr.arguments().size() != 1) {
    for (const ExprPtr &argument : expr.arguments()) {
      analyze(argument);
    }
    report(expr.paren(), "std::move expects exactly one value.", "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }

  const ExprPtr &argument = expr.arguments().front();
  const SemanticType valueType = analyzeProjectionBase(argument);
  SemanticPlace place = semanticPlace(argument);
  if (destructorDepth != 0 && place.receiver && !place.projections.empty()) {
    // Destructor entry authority permits checked field access, not a second
    // ownership transfer. A retained consuming receiver may already have
    // consumed a field while keeping its physical shell for final cleanup.
    report(expressionToken(argument),
           "std::move cannot partially move a receiver field in a destructor; "
           "field cleanup remains owned by the enclosing object.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (place.receiver && place.root != nullptr && place.projections.empty()) {
    if (currentReceiverMutability != ReceiverMutability::Consuming ||
        !typeTraits(valueType).movable ||
        typeTraits(valueType).containsBorrowedState) {
      report(expressionToken(argument),
             "std::move(this) requires a consuming receiver with a movable "
             "owned type.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (const auto *state = unavailableProjectedState(place, true)) {
      reportUnavailablePlace(expressionToken(argument), *state, &place);
      currentType = SemanticType::Unknown;
      return;
    }
    reportLoanPlaceConflict(
        expressionToken(argument), place, AccessMode::Mutable,
        "Cannot move this while an overlapping borrow may still be live.");
    setProjectedValueState(place, ValueState::Moved, expressionToken(argument));
    semanticModel.recordOwnershipEvent(expr, {.kind = OwnershipEventKind::Move,
                                              .place = placeKey(place),
                                              .before = ValueState::Available,
                                              .after = ValueState::Moved});
    currentType = valueType;
    semanticModel.record(expr,
                         ResolvedCallInfo{.returnType = valueType,
                                          .parameterTypes = {valueType},
                                          .intrinsic = IntrinsicKind::Move});
    if (declaration != nullptr)
      bindIntrinsicCallDeclaration(expr, *declaration);
    return;
  }
  const Expr *retainedOwner = nullptr;
  if (immediateReceiver &&
      currentReceiverMutability == ReceiverMutability::Consuming &&
      (valueType.kind == SemanticType::Class ||
       valueType.kind == SemanticType::Lambda ||
       valueType.kind == SemanticType::TypeParameter) &&
      !typeTraits(valueType).containsBorrowedState) {
    const Expr *accessor = argument.get();
    while (const auto *grouping = exprAs<Grouping>(accessor))
      accessor = grouping->expression().get();
    const Expr *ownerExpression = nullptr;
    const FunctionInfo *function = nullptr;
    if (const auto *unary = exprAs<Unary>(accessor);
        unary != nullptr && unary->oper().kind == TokenKind::STAR) {
      const auto *resolved = semanticModel.findOperator(*unary);
      if (resolved != nullptr && resolved->dispatch == CallDispatch::Static &&
          resolved->receiverMode == CallReceiverMode::MutablePlace &&
          resolved->borrowOrigin == BorrowOriginKind::Receiver &&
          resolved->borrowAccess == AccessMode::Mutable) {
        function = semanticModel.findFunction(resolved->function);
        ownerExpression = unary->right().get();
      }
    } else if (const auto *call = exprAs<Call>(accessor)) {
      const auto *resolved = semanticModel.findCall(*call);
      const auto *member = exprAs<Get>(call->callee().get());
      if (resolved != nullptr && member != nullptr &&
          member->access().kind == TokenKind::DOT &&
          call->arguments().empty() &&
          resolved->dispatch == CallDispatch::Static &&
          resolved->receiverMode == CallReceiverMode::MutablePlace &&
          resolved->borrowOrigin == BorrowOriginKind::Receiver &&
          resolved->borrowAccess == AccessMode::Mutable) {
        function = semanticModel.findFunction(resolved->function);
        ownerExpression = member->object().get();
      }
    }
    if (function != nullptr && function->ownedPointeeProjection &&
        function->ownedPointeeProjection->borrowIntrinsic ==
            IntrinsicKind::UniqueOwnerBorrowMut &&
        ownerExpression != nullptr) {
      const ExpressionInfo *owner =
          semanticModel.findExpression(*ownerExpression);
      SemanticPlace anchor = semanticPlace(ownerExpression);
      if (owner != nullptr && owner->category == ValueCategory::Place &&
          owner->access == AccessMode::Mutable &&
          owner->type.kind == SemanticType::Class &&
          !owner->traits.containsBorrowedState && anchor.root != nullptr &&
          anchor.receiver && !anchor.projections.empty() &&
          std::all_of(anchor.projections.begin(), anchor.projections.end(),
                      [](const auto &projection) {
                        return projection.kind == PlaceProjectionKind::Field;
                      })) {
        retainedOwner = ownerExpression;
        place = std::move(anchor);
      }
    }
  }
  if (place.root != nullptr &&
      (place.root->bindingKind == SemanticBindingKind::GlobalVariable ||
       place.root->bindingKind == SemanticBindingKind::StaticField ||
       (place.root->variableDeclaration != nullptr &&
        place.root->variableDeclaration->isThreadLocal()))) {
    report(expressionToken(argument),
           place.root->variableDeclaration != nullptr &&
                   place.root->variableDeclaration->isThreadLocal()
               ? "std::move cannot consume thread-local storage because its "
                 "availability must remain valid across calls."
           : place.root->bindingKind == SemanticBindingKind::GlobalVariable
               ? "std::move cannot consume global binding '" +
                     place.root->declaration.lexeme +
                     "' because its move state is not locally provable."
               : "std::move cannot consume static class storage because "
                 "its move state is not locally provable.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (place.root != nullptr && !place.projections.empty()) {
    const ExpressionInfo *info = semanticModel.findExpression(*argument);
    if (place.root->type.kind == SemanticType::Reference) {
      report(expressionToken(argument),
             "std::move cannot consume a field through a borrowed "
             "reference in the current lifetime model.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (place.root->valueState != ValueState::Available) {
      currentType = SemanticType::Unknown;
      return;
    }
    const PlaceProjectionKind projectionKind = place.projections.back().kind;
    if (info == nullptr || info->category != ValueCategory::Place ||
        info->access != AccessMode::Mutable ||
        (projectionKind != PlaceProjectionKind::Field &&
         projectionKind != PlaceProjectionKind::ConstantIndex)) {
      report(expressionToken(argument),
             projectionKind == PlaceProjectionKind::DynamicIndex
                 ? "std::move requires an in-range constant fixed-array "
                   "index; a dynamic index may alias another element."
                 : "std::move requires a writable field or constant-index "
                   "fixed-array place; make its owning binding and receiver "
                   "mutable.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    // A projected transfer depends on the writable owning binding required
    // above, so it is a mutable use of that binding for GTI-S2096.
    recordMutableBindingUse(toolingSymbolFor(*place.root));
    if (!typeTraits(valueType).movable) {
      report(expressionToken(argument),
             "std::move requires a movable value, but type '" +
                 typeSpelling(valueType) + "' is not movable.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (const ProjectedValueState *state =
            unavailableProjectedState(place, true)) {
      reportUnavailablePlace(expressionToken(argument), *state, &place);
      currentType = SemanticType::Unknown;
      return;
    }
    reportLoanPlaceConflict(
        expressionToken(argument), place, AccessMode::Mutable,
        "Cannot move a place while an overlapping borrow may still be "
        "live.");
    const ValueState before =
        projectedValueState(*place.root, place.projections);
    setProjectedValueState(place, ValueState::Moved, expressionToken(argument));
    if (place.root->variableDeclaration != nullptr) {
      semanticModel.recordExplicitMove(*place.root->variableDeclaration);
    } else if (place.root->parameterDeclaration != nullptr) {
      semanticModel.recordExplicitMove(*place.root->parameterDeclaration);
    }
    const bool retainedReceiver =
        immediateReceiver && place.receiver &&
        currentReceiverMutability == ReceiverMutability::Consuming &&
        (retainedOwner != nullptr || valueType.kind == SemanticType::Class ||
         valueType.kind == SemanticType::Lambda) &&
        !typeTraits(valueType).containsBorrowedState &&
        std::all_of(place.projections.begin(), place.projections.end(),
                    [](const auto &projection) {
                      return projection.kind == PlaceProjectionKind::Field;
                    });
    semanticModel.recordOwnershipEvent(
        expr, {.kind = retainedReceiver ? OwnershipEventKind::ConsumeReceiver
                                        : OwnershipEventKind::Move,
               .place = placeKey(place),
               .before = before,
               .after = ValueState::Moved});
    currentType = valueType;
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = {valueType},
                               .intrinsic = IntrinsicKind::Move,
                               .borrowOrigin =
                                   typeTraits(currentType).containsBorrowedState
                                       ? BorrowOriginKind::Argument
                                       : BorrowOriginKind::None,
                               .borrowArgument = 0,
                               .borrowAccess = borrowAccess(currentType),
                               .retainedReceiverProjection = retainedReceiver,
                               .retainedReceiverOwner = retainedOwner});
    if (declaration != nullptr)
      bindIntrinsicCallDeclaration(expr, *declaration);
    return;
  }
  const Variable *variable = movedVariable(argument);
  if (variable == nullptr) {
    const std::optional<Symbol> symbol = resolveExpressionSymbol(argument);
    if (symbol && (symbol->bindingKind == SemanticBindingKind::GlobalVariable ||
                   (symbol->variableDeclaration != nullptr &&
                    symbol->variableDeclaration->isThreadLocal()))) {
      report(expressionToken(argument),
             "std::move cannot consume a global binding because its move "
             "state is not locally provable.",
             "GTI-S2018");
    } else if (const ExpressionInfo *info =
                   semanticModel.findExpression(*argument);
               info != nullptr && info->category == ValueCategory::Place) {
      report(expressionToken(argument),
             "std::move cannot partially move a field, indexed element, or "
             "borrowed place until place-aware initialization tracking is "
             "available.",
             "GTI-S2018");
    } else {
      report(expressionToken(argument),
             "std::move requires a named local value or by-value parameter; "
             "temporaries are already values.",
             "GTI-S2018");
    }
    currentType = SemanticType::Unknown;
    return;
  }
  const Symbol *symbol = resolve(variable->name());
  if (symbol == nullptr) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->variableDeclaration != nullptr &&
      structuredBindingElements.contains(symbol->variableDeclaration)) {
    report(variable->name(),
           "std::move cannot partially move structured binding '" +
               variable->name().lexeme +
               "' until place-aware partial initialization tracking is "
               "available.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->type.kind == SemanticType::Reference) {
    report(variable->name(),
           "std::move cannot consume a reference; move the owning value "
           "instead.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (!typeTraits(symbol->type).movable) {
    report(variable->name(),
           "std::move requires a movable value, but type '" +
               typeSpelling(symbol->type) + "' is not movable.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->bindingKind == SemanticBindingKind::Field) {
    report(variable->name(),
           "std::move cannot partially move field '" + variable->name().lexeme +
               "' until place-aware initialization tracking is available.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->bindingKind == SemanticBindingKind::GlobalVariable ||
      (symbol->variableDeclaration != nullptr &&
       symbol->variableDeclaration->isThreadLocal())) {
    report(variable->name(),
           std::string("std::move cannot consume ") +
               (symbol->variableDeclaration != nullptr &&
                        symbol->variableDeclaration->isThreadLocal()
                    ? "thread-local"
                    : "global") +
               " binding '" + variable->name().lexeme +
               "' because its move state is not locally provable.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->bindingKind == SemanticBindingKind::LambdaCapture) {
    report(variable->name(),
           "std::move cannot consume immutable lambda capture '" +
               variable->name().lexeme + "'.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->bindingKind != SemanticBindingKind::LocalVariable &&
      symbol->bindingKind != SemanticBindingKind::Parameter) {
    report(variable->name(),
           "std::move requires a named local value or by-value parameter.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->valueState != ValueState::Available) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (place.root != nullptr &&
      unavailableProjectedState(place, true) != nullptr) {
    report(expressionToken(argument),
           "std::move cannot consume a value while one of its owned "
           "places remains moved.",
           "GTI-S2018");
    diagnostics.back().hints.emplace_back(
        "Reinitialize the moved place before transferring its owner.");
    currentType = SemanticType::Unknown;
    return;
  }
  reportLoanPlaceConflict(
      variable->name(), place, AccessMode::Mutable,
      "Cannot move storage while an overlapping borrow may still be live.");
  Symbol *mutableSymbol = resolveMutable(variable->name());
  if (mutableSymbol == nullptr) {
    currentType = SemanticType::Unknown;
    return;
  }
  const ValueState before = mutableSymbol->valueState;
  if (mutableSymbol->completeBorrowSet)
    semanticModel.recordBorrowDependencySet(expr,
                                            *mutableSymbol->completeBorrowSet);
  mutableSymbol->completeBorrowSet.reset();
  invalidateRawPointerOrigins(placeForSymbol(*mutableSymbol));
  mutableSymbol->valueState = ValueState::Moved;
  mutableSymbol->moveOrigins = {tokenSpan(variable->name())};
  if (mutableSymbol->variableDeclaration != nullptr) {
    semanticModel.recordExplicitMove(*mutableSymbol->variableDeclaration);
  } else if (mutableSymbol->parameterDeclaration != nullptr) {
    semanticModel.recordExplicitMove(*mutableSymbol->parameterDeclaration);
  }
  SemanticPlace movedPlace = placeForSymbol(*mutableSymbol);
  semanticModel.recordOwnershipEvent(expr, {.kind = OwnershipEventKind::Move,
                                            .place = placeKey(movedPlace),
                                            .before = before,
                                            .after = ValueState::Moved});
  currentType = valueType;
  semanticModel.record(
      expr,
      ResolvedCallInfo{.returnType = currentType,
                       .parameterTypes = {valueType},
                       .intrinsic = IntrinsicKind::Move,
                       .borrowOrigin =
                           (typeTraits(currentType).containsBorrowedState ||
                            isOwnerDependentTextExpression(argument))
                               ? BorrowOriginKind::Argument
                               : BorrowOriginKind::None,
                       .borrowArgument = 0,
                       .borrowAccess = borrowAccess(currentType)});
  if (declaration != nullptr)
    bindIntrinsicCallDeclaration(expr, *declaration);
}

auto SemanticVisitor::Impl::analyzeImplicitReturnMove(
    const ExprPtr &expression, const SemanticType &returnType,
    const SemanticType &valueType) -> ImplicitReturnMoveStatus {
  if (!expression || returnType != valueType ||
      !isMoveOnlyOwnerType(valueType)) {
    return ImplicitReturnMoveStatus::NotEligible;
  }

  const Variable *variable = movedVariable(expression);
  const Symbol *symbol =
      variable == nullptr ? nullptr : resolve(variable->name());
  if (symbol == nullptr ||
      (symbol->bindingKind != SemanticBindingKind::LocalVariable &&
       symbol->bindingKind != SemanticBindingKind::Parameter) ||
      symbol->type.kind == SemanticType::Reference) {
    return ImplicitReturnMoveStatus::NotEligible;
  }
  if (symbol->variableDeclaration != nullptr &&
      symbol->variableDeclaration->isThreadLocal()) {
    report(variable->name(),
           "An implicit return move cannot consume thread-local storage "
           "because its availability must remain valid across calls.",
           "GTI-S2018");
    return ImplicitReturnMoveStatus::Invalid;
  }
  if (symbol->variableDeclaration != nullptr &&
      structuredBindingElements.contains(symbol->variableDeclaration)) {
    report(
        variable->name(),
        "An implicit return move cannot partially move structured binding '" +
            variable->name().lexeme +
            "' until place-aware partial initialization tracking is "
            "available.",
        "GTI-S2018");
    return ImplicitReturnMoveStatus::Invalid;
  }

  SemanticPlace place = semanticPlace(expression);
  if (place.root == nullptr || !place.projections.empty() || place.receiver) {
    return ImplicitReturnMoveStatus::NotEligible;
  }
  if (symbol->valueState != ValueState::Available) {
    return ImplicitReturnMoveStatus::Invalid;
  }
  if (unavailableProjectedState(place, true) != nullptr) {
    report(expressionToken(expression),
           "Cannot return a value while one of its owned places remains "
           "moved.",
           "GTI-S2018");
    diagnostics.back().hints.emplace_back(
        "Reinitialize the moved place before returning its owner.");
    return ImplicitReturnMoveStatus::Invalid;
  }

  reportLoanPlaceConflict(
      variable->name(), place, AccessMode::Mutable,
      "Cannot move returned storage while an overlapping borrow may still "
      "be live.");
  Symbol *mutableSymbol = resolveMutable(variable->name());
  if (mutableSymbol == nullptr) {
    return ImplicitReturnMoveStatus::Invalid;
  }

  const ValueState before = mutableSymbol->valueState;
  mutableSymbol->completeBorrowSet.reset();
  invalidateRawPointerOrigins(placeForSymbol(*mutableSymbol));
  mutableSymbol->valueState = ValueState::Moved;
  mutableSymbol->moveOrigins = {tokenSpan(variable->name())};
  if (mutableSymbol->variableDeclaration != nullptr) {
    semanticModel.recordExplicitMove(*mutableSymbol->variableDeclaration);
  } else if (mutableSymbol->parameterDeclaration != nullptr) {
    semanticModel.recordExplicitMove(*mutableSymbol->parameterDeclaration);
  }
  SemanticPlace movedPlace = placeForSymbol(*mutableSymbol);
  semanticModel.recordOwnershipEvent(*expression,
                                     {.kind = OwnershipEventKind::Move,
                                      .place = placeKey(movedPlace),
                                      .before = before,
                                      .after = ValueState::Moved});
  semanticModel.recordImplicitReturnMove(
      *expression, {.borrowOrigin = typeTraits(valueType).containsBorrowedState
                                        ? BorrowOriginKind::Argument
                                        : BorrowOriginKind::None,
                    .borrowAccess = borrowAccess(valueType)});
  return ImplicitReturnMoveStatus::Moved;
}

auto SemanticVisitor::Impl::analyzeIntegerArithmeticIntrinsicCall(
    const Call &expr, const CallableCandidate &declaration) -> void {
  const IntrinsicKind intrinsic = declaration.record().intrinsic;
  const std::optional<IntegerArithmeticIntrinsic> arithmetic =
      integerArithmeticIntrinsic(intrinsic);
  const bool overflowPredicate =
      arithmetic &&
      arithmetic->mode == IntegerArithmeticMode::OverflowPredicate;
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (!expr.typeArguments().empty()) {
    report(expr.paren(),
           "Integer arithmetic operations infer their operand type and do "
           "not take explicit type arguments.",
           "GTI-S2018");
  }
  if (argumentTypes.size() != 2) {
    report(expr.paren(),
           "Integer arithmetic operations expect exactly two operands.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }

  const SemanticType left = argumentTypes[0];
  const SemanticType right = argumentTypes[1];
  if (left == SemanticType::Unknown || right == SemanticType::Unknown) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (left != right) {
    report(expr.paren(),
           "Integer arithmetic operations require two operands of the "
           "same fixed-width integer type.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  if (!isIntegral(left)) {
    report(expr.paren(),
           "Integer arithmetic operations require a fixed-width integer "
           "type.",
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }

  currentType = overflowPredicate ? SemanticType::Bool : left;
  semanticModel.record(
      expr, ResolvedCallInfo{.returnType = currentType,
                             .parameterTypes = std::move(argumentTypes),
                             .intrinsic = intrinsic});
}

auto SemanticVisitor::Impl::analyzeUniqueOwnerIntrinsicCall(
    const Call &expr, IntrinsicKind intrinsic) -> void {
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }

  if (intrinsic == IntrinsicKind::AllocateUniqueOwner) {
    if (expr.typeArguments().size() != 1) {
      for (const TypeRef &argument : expr.typeArguments()) {
        validateType(argument);
      }
      report(expr.paren(),
             "gti_internal::allocate_unique_owner<T> requires exactly one "
             "pointee type.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }

    const TypeRef &pointeeRef = expr.typeArguments().front();
    validateType(pointeeRef);
    validateReferencePlacement(pointeeRef, false, "allocated type");
    const SemanticType pointeeType = typeOf(pointeeRef);
    const bool scalar = isAllocatedScalarType(pointeeType);
    if (scalar) {
      if (const auto arguments =
              concreteCallArguments(argumentTypes, expr.arguments())) {
        if (arguments->empty()) {
          if (!isDefaultInitializable(pointeeType, true)) {
            report(expr.paren(),
                   "Allocation of '" + typeSpelling(pointeeType) +
                       "' requires one value of the exact pointee type; this "
                       "type has no default initialization.",
                   "GTI-S2018");
          }
        } else if (arguments->size() != 1 ||
                   (arguments->front().type != SemanticType::Unknown &&
                    arguments->front().type != pointeeType)) {
          report(expr.paren(),
                 "Scalar allocation requires one value of the exact pointee "
                 "type '" +
                     typeSpelling(pointeeType) + "'.",
                 "GTI-S2018");
        }
      }
    } else if (pointeeType.kind != SemanticType::Class &&
               pointeeType.kind != SemanticType::TypeParameter) {
      report(pointeeRef.name.last(),
             "Compiler-private unique allocation currently requires a "
             "class, struct, scalar, or generic object type.",
             "GTI-S2018");
    } else if (pointeeType.kind == SemanticType::Class) {
      if (const std::optional<std::vector<AnalyzedCallArgument>> arguments =
              concreteCallArguments(argumentTypes, expr.arguments())) {
        analyzeConstructorCallArguments(
            expr, pointeeType.classId, pointeeType.arguments,
            pointeeType.valueArguments, *arguments, expr.paren());
      }
    }

    currentType = SemanticType::uniqueOwnerOf(pointeeType);
    semanticModel.record(
        expr,
        ResolvedCallInfo{.returnType = currentType,
                         .parameterTypes = argumentTypes,
                         .typeArguments = {pointeeType},
                         .intrinsic = IntrinsicKind::AllocateUniqueOwner});
    return;
  }

  if (intrinsic == IntrinsicKind::UniqueOwnerUpcast) {
    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
    }
    if (expr.typeArguments().size() != 1) {
      report(expr.paren(),
             "gti_internal::unique_owner_upcast<Base> requires exactly "
             "one target base type.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (argumentTypes.size() != 1 ||
        argumentTypes.front().kind != SemanticType::UniqueOwner ||
        argumentTypes.front().arguments.size() != 1) {
      report(expr.paren(),
             "unique_owner_upcast expects exactly one "
             "gti_internal::unique_owner<Derived> value.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType targetType = typeOf(expr.typeArguments().front());
    const SemanticType sourceType = argumentTypes.front().arguments.front();
    if (targetType.kind != SemanticType::Class &&
        targetType.kind != SemanticType::TypeParameter) {
      report(expr.typeArguments().front().name.last(),
             "A unique owner can only be upcast to a class or interface "
             "base type.",
             "GTI-S2018");
    } else if (sourceType.kind != SemanticType::TypeParameter &&
               targetType.kind != SemanticType::TypeParameter) {
      if (!isPublicBaseConversion(sourceType, targetType)) {
        report(expr.paren(),
               "Cannot upcast unique ownership from '" +
                   typeSpelling(sourceType) + "' to '" +
                   typeSpelling(targetType) +
                   "'; the target must be a public base.",
               "GTI-S2018");
      } else if (const ClassInfo *target = classInfo(targetType);
                 target == nullptr || !target->polymorphic) {
        report(expr.typeArguments().front().name.last(),
               "Unique-owner upcast target '" + typeSpelling(targetType) +
                   "' must have polymorphic destruction.",
               "GTI-S2018");
      }
    }

    currentType = SemanticType::uniqueOwnerOf(targetType);
    (void)reportOwnershipErasureCapabilityAmplification(
        expr.paren(), "unique", sourceType, targetType, argumentTypes.front(),
        currentType);
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = std::move(argumentTypes),
                               .typeArguments = {targetType, sourceType},
                               .intrinsic = IntrinsicKind::UniqueOwnerUpcast});
    return;
  }

  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (!expr.typeArguments().empty()) {
    report(expr.paren(),
           "Unique-owner operations infer their pointee type and do not "
           "take explicit type arguments.",
           "GTI-S2018");
  }
  const bool swap = intrinsic == IntrinsicKind::UniqueOwnerSwap;
  if (argumentTypes.size() != (swap ? 2u : 1u)) {
    report(expr.paren(),
           uniqueOwnerIntrinsicName(intrinsic) +
               (swap ? " expects exactly two unique owners."
                     : " expects exactly one unique owner."),
           "GTI-S2018");
    currentType = SemanticType::Unknown;
    return;
  }
  for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
    if (argumentTypes[index].kind != SemanticType::UniqueOwner ||
        argumentTypes[index].arguments.size() != 1) {
      if (argumentTypes[index] != SemanticType::Unknown) {
        report(expressionToken(expr.arguments()[index]),
               uniqueOwnerIntrinsicName(intrinsic) +
                   " requires gti_internal::unique_owner<T>.",
               "GTI-S2018");
      }
      currentType = SemanticType::Unknown;
      return;
    }
  }

  const SemanticType ownerType = argumentTypes.front();
  const SemanticType pointeeType = ownerType.arguments.front();
  if (swap) {
    if (argumentTypes[1] != ownerType) {
      report(expr.paren(),
             "unique_owner_swap requires two owners with the same pointee "
             "type.",
             "GTI-S2018");
    }
    for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
      const ExpressionInfo *owner =
          semanticModel.findExpression(*expr.arguments()[index]);
      if (owner == nullptr || owner->category != ValueCategory::Place) {
        report(expressionToken(expr.arguments()[index]),
               "unique_owner_swap requires stable unique-owner storage.",
               "GTI-S2018");
      } else if (owner->access != AccessMode::Mutable) {
        report(expressionToken(expr.arguments()[index]),
               "unique_owner_swap requires mutable unique-owner storage.",
               "GTI-S2018");
      }
    }
    currentType = SemanticType::Void;
    semanticModel.record(
        expr, ResolvedCallInfo{
                  .returnType = currentType,
                  .parameterTypes = {SemanticType::referenceTo(
                                         ownerType, AccessMode::Mutable),
                                     SemanticType::referenceTo(
                                         ownerType, AccessMode::Mutable)},
                  .typeArguments = {pointeeType},
                  .intrinsic = intrinsic});
    return;
  }
  const bool borrows = intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
                       intrinsic == IntrinsicKind::UniqueOwnerBorrowMut;
  if (borrows) {
    const ExpressionInfo *owner =
        semanticModel.findExpression(*expr.arguments().front());
    if (owner == nullptr || owner->category != ValueCategory::Place) {
      report(expressionToken(expr.arguments().front()),
             uniqueOwnerIntrinsicName(intrinsic) +
                 " requires stable unique-owner storage.",
             "GTI-S2018");
    } else if (intrinsic == IntrinsicKind::UniqueOwnerBorrowMut &&
               owner->access != AccessMode::Mutable) {
      report(expressionToken(expr.arguments().front()),
             "unique_owner_borrow_mut requires mutable unique-owner "
             "storage.",
             "GTI-S2018");
    }
  }

  currentType = intrinsic == IntrinsicKind::UniqueOwnerIsNull
                    ? SemanticType::Bool
                    : pointeeType;
  semanticModel.record(
      expr,
      ResolvedCallInfo{
          .returnType =
              borrows ? SemanticType::referenceTo(
                            pointeeType,
                            intrinsic == IntrinsicKind::UniqueOwnerBorrowMut
                                ? AccessMode::Mutable
                                : AccessMode::ReadOnly)
                      : currentType,
          .parameterTypes = std::move(argumentTypes),
          .typeArguments = {pointeeType},
          .intrinsic = intrinsic,
          .borrowOrigin =
              borrows ? BorrowOriginKind::Argument : BorrowOriginKind::None,
          .borrowArgument = 0,
          .borrowAccess = intrinsic == IntrinsicKind::UniqueOwnerBorrowMut
                              ? AccessMode::Mutable
                              : AccessMode::ReadOnly});
}

auto SemanticVisitor::Impl::analyzeSharedOwnerIntrinsicCall(
    const Call &expr, const CallableCandidate &declaration) -> void {
  const IntrinsicKind intrinsic = declaration.record().intrinsic;
  std::vector<SemanticType> arguments;
  for (const ExprPtr &argument : expr.arguments()) {
    arguments.push_back(analyze(argument));
  }
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (intrinsic == IntrinsicKind::SharedOwnerUpcast) {
    if (expr.typeArguments().size() != 1) {
      report(expr.paren(),
             "gti_internal::shared_owner_upcast<Base> requires exactly one "
             "target base type.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (arguments.size() != 1) {
      report(expr.paren(),
             "shared_owner_upcast expects exactly one consumed "
             "gti_internal::shared_owner<Derived> value.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    const ClassInfo *sourceOwner = classInfo(arguments.front());
    if (sourceOwner == nullptr ||
        sourceOwner->compilerCapability !=
            CompilerCapabilityTypeKind::SharedOwner ||
        arguments.front().arguments.size() != 1) {
      report(expr.paren(),
             "shared_owner_upcast requires the exact trusted "
             "shared_owner<Derived> capability.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    const auto *argumentInfo =
        semanticModel.findExpression(*expr.arguments().front());
    if (argumentInfo == nullptr ||
        argumentInfo->category == ValueCategory::Place) {
      report(expr.paren(),
             "Shared-owner upcast consumes its source handle; use std::move "
             "for a named handle.",
             "GTI-S2018");
    }

    const SemanticType targetType = typeOf(expr.typeArguments().front());
    const SemanticType sourceType = arguments.front().arguments.front();
    if (targetType.kind != SemanticType::Class &&
        targetType.kind != SemanticType::TypeParameter) {
      report(expr.typeArguments().front().name.last(),
             "A shared owner can only be upcast to a class or interface "
             "base type.",
             "GTI-S2018");
    } else if (sourceType.kind != SemanticType::TypeParameter &&
               targetType.kind != SemanticType::TypeParameter) {
      if (!isPublicBaseConversion(sourceType, targetType)) {
        report(expr.paren(),
               "Cannot upcast shared ownership from '" +
                   typeSpelling(sourceType) + "' to '" +
                   typeSpelling(targetType) +
                   "'; the target must be a public base.",
               "GTI-S2018");
      } else if (const ClassInfo *target = classInfo(targetType);
                 target == nullptr || !target->polymorphic) {
        report(expr.typeArguments().front().name.last(),
               "Shared-owner upcast target '" + typeSpelling(targetType) +
                   "' must have polymorphic destruction.",
               "GTI-S2018");
      }
    }

    currentType = declaration.record().returnType;
    const ClassInfo *targetOwner = classInfo(currentType);
    if (targetOwner == nullptr || targetOwner->compilerCapability !=
                                      CompilerCapabilityTypeKind::SharedOwner) {
      report(expr.paren(),
             "Shared-owner upcast requires a trusted shared-owner result.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType.arguments = {targetType};
    (void)reportOwnershipErasureCapabilityAmplification(
        expr.paren(), "shared", sourceType, targetType, arguments.front(),
        currentType);
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = std::move(arguments),
                               .typeArguments = {targetType, sourceType},
                               .intrinsic = intrinsic});
    return;
  }
  const bool empty = intrinsic == IntrinsicKind::SharedOwnerEmpty;
  const bool promote = intrinsic == IntrinsicKind::SharedOwnerFromUnique;
  const bool swap = intrinsic == IntrinsicKind::SharedOwnerSwap;
  const bool borrow = intrinsic == IntrinsicKind::SharedOwnerBorrow;
  const auto reject = [&](std::string message) {
    report(expr.paren(), std::move(message), "GTI-S2018");
    currentType = SemanticType::Unknown;
  };
  if (arguments.size() != (empty  ? 0
                           : swap ? 2
                                  : 1) ||
      expr.typeArguments().size() != (empty ? 1 : 0)) {
    reject(empty ? "shared_owner_empty<T> requires one pointee type and no "
                   "arguments."
                 : "Shared-owner operations infer their pointee type and "
                   "require exactly the declared capability arguments.");
    return;
  }

  SemanticType element =
      empty ? typeOf(expr.typeArguments().front()) : SemanticType::Unknown;
  SemanticType ownerType = SemanticType::Unknown;
  if (promote) {
    if (arguments.front().kind != SemanticType::UniqueOwner ||
        arguments.front().arguments.size() != 1) {
      reject(
          "Shared ownership promotion requires one exact unique-owner value.");
      return;
    }
    element = arguments.front().arguments.front();
    const auto *info = semanticModel.findExpression(*expr.arguments().front());
    if (info == nullptr || info->category == ValueCategory::Place) {
      reject("Shared ownership promotion consumes its unique owner; use "
             "std::move for a named owner.");
      return;
    }
  } else if (!empty) {
    ownerType = arguments.front();
    const ClassInfo *owner = classInfo(ownerType);
    if (owner == nullptr ||
        owner->compilerCapability != CompilerCapabilityTypeKind::SharedOwner ||
        ownerType.arguments.size() != 1) {
      reject("Shared-owner operations require the exact trusted "
             "shared_owner<T> capability.");
      return;
    }
    element = ownerType.arguments.front();
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto *info = semanticModel.findExpression(*expr.arguments()[index]);
      if (arguments[index] != ownerType || info == nullptr ||
          info->category != ValueCategory::Place) {
        reject("Shared-owner observers and cloning require stable handles; "
               "swap requires two matching stable handles.");
        return;
      }
      if (swap) {
        requireMutableStorage(expr.arguments()[index], "shared-owner swap");
      }
    }
  }
  if (element.kind != SemanticType::TypeParameter &&
      ((!isAllocatedScalarType(element) &&
        element.kind != SemanticType::Class) ||
       typeTraits(element).containsBorrowedState)) {
    reject("Shared ownership currently requires a class, struct, or scalar "
           "object type without borrowed state.");
    return;
  }
  if (empty || promote) {
    // The selected trusted declaration supplies the class identity. Never
    // recover ownership permission from a public wrapper's spelling.
    ownerType = declaration.record().returnType;
    const ClassInfo *owner = classInfo(ownerType);
    if (owner == nullptr ||
        owner->compilerCapability != CompilerCapabilityTypeKind::SharedOwner) {
      reject("Shared-owner creation requires a trusted shared-owner result.");
      return;
    }
    ownerType.arguments = {element};
  }
  std::vector<SemanticType> parameters = arguments;
  if (!empty && !promote) {
    for (auto &parameter : parameters) {
      parameter = SemanticType::referenceTo(
          parameter, swap ? AccessMode::Mutable : AccessMode::ReadOnly);
    }
  }
  currentType =
      borrow                                            ? element
      : intrinsic == IntrinsicKind::SharedOwnerIsNull   ? SemanticType::Bool
      : intrinsic == IntrinsicKind::SharedOwnerUseCount ? SemanticType::UInt64
      : swap                                            ? SemanticType::Void
                                                        : ownerType;
  semanticModel.record(
      expr,
      ResolvedCallInfo{.returnType = borrow ? SemanticType::referenceTo(element)
                                            : currentType,
                       .parameterTypes = std::move(parameters),
                       .typeArguments = {element},
                       .intrinsic = intrinsic,
                       .borrowOrigin = borrow ? BorrowOriginKind::Argument
                                              : BorrowOriginKind::None,
                       .borrowArgument = 0,
                       .borrowAccess = AccessMode::ReadOnly});
}

auto SemanticVisitor::Impl::analyzeOptionalStorageIntrinsicCall(
    const Call &expr, IntrinsicKind intrinsic) -> void {
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (!expr.typeArguments().empty()) {
    report(expr.paren(),
           "Optional storage operations infer their element type from the "
           "storage argument and do not take explicit type arguments.",
           "GTI-S2019");
  }

  const bool constructing =
      intrinsic == IntrinsicKind::OptionalStorageConstruct;
  if ((constructing && argumentTypes.size() != 2) ||
      (!constructing && argumentTypes.size() != 1)) {
    report(expr.paren(),
           constructing ? "optional_storage_construct expects mutable optional "
                          "storage and one completed payload value."
                        : "This optional storage operation expects exactly one "
                          "storage argument.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  if (argumentTypes.empty()) {
    currentType = SemanticType::Unknown;
    return;
  }

  const SemanticType storageType = argumentTypes.front();
  const ClassInfo *storageInfo = classInfo(storageType);
  if (storageType.kind != SemanticType::Class || storageInfo == nullptr ||
      storageInfo->compilerCapability !=
          CompilerCapabilityTypeKind::OptionalStorage ||
      storageType.arguments.size() != 1) {
    if (storageType != SemanticType::Unknown) {
      report(expressionToken(expr.arguments().front()),
             "Optional storage operations require the exact trusted "
             "gti_internal::optional_storage<T> capability.",
             "GTI-S2019");
    }
    currentType = SemanticType::Unknown;
    return;
  }

  const SemanticType elementType = storageType.arguments.front();
  const bool mutates = constructing ||
                       intrinsic == IntrinsicKind::OptionalStorageReadMut ||
                       intrinsic == IntrinsicKind::OptionalStorageReset;
  if (mutates) {
    requireMutableStorage(expr.arguments().front(),
                          "optional storage operation");
  }

  if (constructing) {
    if ((elementType.kind == SemanticType::StringView ||
         elementType.kind == SemanticType::CString) &&
        isOwnerDependentTextExpression(expr.arguments()[1])) {
      reportOwnerDependentTextStorage(expr.arguments()[1],
                                      "nested in optional storage");
    } else if (elementType.kind == SemanticType::StringView ||
               elementType.kind == SemanticType::CString) {
      noteLocalTextEscape(expr.arguments()[1], LocalTextEscapeKind::Storage,
                          "nested in optional storage");
    }
    if (elementType != SemanticType::Unknown &&
        elementType.kind != SemanticType::TypeParameter &&
        argumentTypes[1] != SemanticType::Unknown &&
        argumentTypes[1] != elementType) {
      report(expressionToken(expr.arguments()[1]),
             "Optional storage publication requires one completed payload "
             "value of the exact element type; construct the payload before "
             "publishing it.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    std::vector<AnalyzedCallArgument> constructorArguments;
    bool concreteArguments = true;
    for (std::size_t index = 1; index < argumentTypes.size(); ++index) {
      if (!appendConcreteCallArgument(argumentTypes[index],
                                      expr.arguments()[index],
                                      constructorArguments)) {
        concreteArguments = false;
        break;
      }
    }
    if (concreteArguments) {
      if (elementType.kind == SemanticType::Class) {
        analyzeConstructorCallArguments(
            expr, elementType.classId, elementType.arguments,
            elementType.valueArguments, constructorArguments, expr.paren());
      } else if (elementType != SemanticType::Unknown &&
                 elementType.kind != SemanticType::TypeParameter) {
        validateStorageValueConstruction(expr, elementType,
                                         constructorArguments);
      }
    }
    currentType = SemanticType::Void;
  } else if (intrinsic == IntrinsicKind::OptionalStorageHasValue) {
    currentType = SemanticType::Bool;
  } else if (intrinsic == IntrinsicKind::OptionalStorageRead ||
             intrinsic == IntrinsicKind::OptionalStorageReadMut) {
    currentType = elementType;
  } else {
    currentType = SemanticType::Void;
  }

  const bool borrows = intrinsic == IntrinsicKind::OptionalStorageRead ||
                       intrinsic == IntrinsicKind::OptionalStorageReadMut;
  const AccessMode borrowAccess =
      intrinsic == IntrinsicKind::OptionalStorageReadMut ? AccessMode::Mutable
                                                         : AccessMode::ReadOnly;
  semanticModel.record(
      expr,
      ResolvedCallInfo{.returnType = borrows ? SemanticType::referenceTo(
                                                   elementType, borrowAccess)
                                             : currentType,
                       .parameterTypes = std::move(argumentTypes),
                       .typeArguments = {elementType},
                       .intrinsic = intrinsic,
                       .borrowOrigin = borrows ? BorrowOriginKind::Argument
                                               : BorrowOriginKind::None,
                       .borrowArgument = 0,
                       .borrowAccess = borrowAccess});
}

// One inline polymorphic payload: a complete concrete class with a public
// polymorphic base Interface whose generated relocation is failure-free and
// whose cleanup is inert. Destroying, replacing, or relocating such a payload
// therefore runs no source code and cannot fail, so the owning slot needs no
// failure-aware cleanup schedule. It retains no checked borrow, and erasing it
// to Interface must not grant a concurrency capability the payload lacks.
auto SemanticVisitor::Impl::inlinePolymorphicPayload(
    const SemanticType &model, const SemanticType &interface) const -> bool {
  const ClassInfo *payload =
      model.kind == SemanticType::Class ? classInfo(model) : nullptr;
  const ClassInfo *base =
      interface.kind == SemanticType::Class ? classInfo(interface) : nullptr;
  if (payload == nullptr || base == nullptr || payload->abstract ||
      payload->kind == ClassKind::Interface ||
      payload->kind == ClassKind::Union || payload->cAbiRecord ||
      payload->cOpaqueHandle ||
      payload->compilerCapability != CompilerCapabilityTypeKind::None ||
      !base->polymorphic || !isPublicBaseConversion(model, interface))
    return false;
  const SemanticTypeTraits traits = typeTraits(model);
  const SemanticTypeTraits erased = typeTraits(interface);
  return traits.movable && traits.relocation != RelocationKind::Unavailable &&
         !traits.containsBorrowedState && !requiresActiveCleanupFor(model) &&
         (traits.transferCapable || !erased.transferCapable) &&
         (traits.shareCapable || !erased.shareCapable);
}

auto SemanticVisitor::Impl::analyzePolymorphicStorageIntrinsicCall(
    const Call &expr, IntrinsicKind intrinsic) -> void {
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (!expr.typeArguments().empty()) {
    report(expr.paren(),
           "Polymorphic storage operations infer their interface and payload "
           "types from their arguments and do not take explicit type "
           "arguments.",
           "GTI-S2019");
  }

  const bool constructing =
      intrinsic == IntrinsicKind::PolymorphicStorageConstruct;
  if ((constructing && argumentTypes.size() != 2) ||
      (!constructing && argumentTypes.size() != 1)) {
    report(expr.paren(),
           constructing
               ? "polymorphic_storage_construct expects mutable polymorphic "
                 "storage and one completed payload value."
               : "This polymorphic storage operation expects exactly one "
                 "storage argument.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }

  const SemanticType storageType = argumentTypes.front();
  const ClassInfo *storageInfo = classInfo(storageType);
  if (storageType.kind != SemanticType::Class || storageInfo == nullptr ||
      storageInfo->compilerCapability !=
          CompilerCapabilityTypeKind::PolymorphicStorage ||
      storageType.arguments.size() != 1 ||
      storageType.valueArguments.size() != 2) {
    if (storageType != SemanticType::Unknown) {
      report(expressionToken(expr.arguments().front()),
             "Polymorphic storage operations require the exact trusted "
             "gti_internal::polymorphic_storage<Interface, Bytes, Align> "
             "capability.",
             "GTI-S2019");
    }
    currentType = SemanticType::Unknown;
    return;
  }

  const SemanticType interfaceType = storageType.arguments.front();
  if (constructing || intrinsic == IntrinsicKind::PolymorphicStorageReadMut ||
      intrinsic == IntrinsicKind::PolymorphicStorageReset) {
    requireMutableStorage(expr.arguments().front(),
                          "polymorphic storage operation");
  }

  SemanticType modelType = SemanticType::Unknown;
  if (constructing) {
    modelType = argumentTypes[1];
    if (modelType != SemanticType::Unknown &&
        !containsOpenTypeParameter(modelType) &&
        !containsOpenTypeParameter(interfaceType) &&
        !inlinePolymorphicPayload(modelType, interfaceType)) {
      report(expressionToken(expr.arguments()[1]),
             "Inline polymorphic storage requires one completed payload of a "
             "concrete class publicly derived from polymorphic '" +
                 typeSpelling(interfaceType) +
                 "', with failure-free relocation, inert cleanup, no checked "
                 "borrowed state, and no concurrency capability beyond its "
                 "own; '" +
                 typeSpelling(modelType) + "' does not qualify.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = SemanticType::Void;
  } else if (intrinsic == IntrinsicKind::PolymorphicStorageHasValue) {
    currentType = SemanticType::Bool;
  } else if (intrinsic == IntrinsicKind::PolymorphicStorageRead ||
             intrinsic == IntrinsicKind::PolymorphicStorageReadMut) {
    currentType = interfaceType;
  } else {
    currentType = SemanticType::Void;
  }

  const bool borrows = intrinsic == IntrinsicKind::PolymorphicStorageRead ||
                       intrinsic == IntrinsicKind::PolymorphicStorageReadMut;
  const AccessMode borrowAccess =
      intrinsic == IntrinsicKind::PolymorphicStorageReadMut
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  std::vector<SemanticType> typeArguments{interfaceType};
  if (constructing) {
    typeArguments.push_back(modelType);
  }
  semanticModel.record(
      expr,
      ResolvedCallInfo{.returnType = borrows ? SemanticType::referenceTo(
                                                   interfaceType, borrowAccess)
                                             : currentType,
                       .parameterTypes = std::move(argumentTypes),
                       .typeArguments = std::move(typeArguments),
                       .intrinsic = intrinsic,
                       .borrowOrigin = borrows ? BorrowOriginKind::Argument
                                               : BorrowOriginKind::None,
                       .borrowArgument = 0,
                       .borrowAccess = borrowAccess});
}

auto SemanticVisitor::Impl::analyzeStorageBoundsCheckCall(const Call &expr)
    -> void {
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (!expr.typeArguments().empty() || argumentTypes.size() != 2 ||
      argumentTypes[0] != SemanticType::UInt64 ||
      argumentTypes[1] != SemanticType::UInt64) {
    report(expr.paren(),
           "index_bounds_check expects a uint64_t index and a uint64_t "
           "logical size.",
           "GTI-S2019");
  }
  currentType = SemanticType::Void;
  semanticModel.record(
      expr, ResolvedCallInfo{.returnType = SemanticType::Void,
                             .parameterTypes = std::move(argumentTypes),
                             .intrinsic = IntrinsicKind::StorageBoundsCheck});
}

auto SemanticVisitor::Impl::isAllocatedScalarType(
    const SemanticType &type) const -> bool {
  return isNumeric(type) || type == SemanticType::Bool ||
         type == SemanticType::Char || type.kind == SemanticType::RawPointer ||
         type.kind == SemanticType::NativeFunction ||
         type == SemanticType::NullPtr ||
         (type.kind == SemanticType::Enum && type.enumId != 0 &&
          type.enumId <= enums.size() && !enums[type.enumId - 1].payload);
}

auto SemanticVisitor::Impl::isStorageElementType(const SemanticType &type)
    -> bool {
  switch (type.kind) {
  case SemanticType::Unknown:
  case SemanticType::Void:
  case SemanticType::NullPtr:
  case SemanticType::Reference:
  case SemanticType::UniqueOwner:
  case SemanticType::SharedPointer:
  case SemanticType::TypeName:
  case SemanticType::Function:
  case SemanticType::FunctionSignature:
  case SemanticType::NativeFunction:
  case SemanticType::Lambda:
    return false;
  default:
    return true;
  }
}

auto SemanticVisitor::Impl::requireMutableStorage(const ExprPtr &argument,
                                                  std::string_view operation)
    -> void {
  const ExpressionInfo *info =
      argument ? semanticModel.findExpression(*argument) : nullptr;
  if (info == nullptr || info->category != ValueCategory::Place ||
      info->access != AccessMode::Mutable) {
    report(expressionToken(argument),
           std::string(operation) + " requires mutable storage.", "GTI-S2019");
  }
  reportLoanPlaceConflict(
      expressionToken(argument), semanticPlace(argument), AccessMode::Mutable,
      std::string(operation) +
          " cannot mutate storage while an overlapping borrow may still "
          "be live.");
}

} // namespace lang
