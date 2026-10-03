#include "semantic_analyzer_impl.h"

namespace lang {

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
    if (cursor->kind != SemanticType::Class)
      continue;
    if (cursor->classId == type.classId)
      return semanticTypeComplexity(type) >= semanticTypeComplexity(*cursor);
    if (const ClassInfo *owner = classInfo(*cursor);
        owner != nullptr && owner->genericParameters.empty())
      return false;
  }
  return false;
}

auto SemanticVisitor::Impl::lambdaTraitsForCaptures(
    std::span<const SemanticType> captureTypes) const -> SemanticTypeTraits {
  SemanticTypeTraits result;
  result.drop = DropKind::Trivial;
  result.relocation = RelocationKind::Structural;
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
    if (capture.relocation == RelocationKind::Unavailable ||
        capture.containsBorrowedState)
      result.relocation = RelocationKind::Unavailable;
    result.transferCapable = result.transferCapable && capture.transferCapable;
    result.shareCapable = result.shareCapable && capture.shareCapable;
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
    if (owner != nullptr && (owner->compilerCapability ==
                                 CompilerCapabilityTypeKind::MutexStorage ||
                             owner->compilerCapability ==
                                 CompilerCapabilityTypeKind::OptionalStorage)) {
      return type.arguments.size() == 1 &&
             requiresActiveCleanup(type.arguments.front(), query);
    }
    if (owner != nullptr && owner->compilerCapability ==
                                CompilerCapabilityTypeKind::MutexLockToken) {
      return false;
    }
  }
  switch (type.kind) {
  case SemanticType::UniqueOwner:
  case SemanticType::SharedPointer:
  case SemanticType::Storage:
  case SemanticType::PrefixStorage:
    return true;
  case SemanticType::Array:
  case SemanticType::Unexpected:
  case SemanticType::Expected:
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

  if (type.kind == SemanticType::Class) {
    if (type.classId == 0 || type.classId > classes.size()) {
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
  case SemanticType::Unexpected:
    result = type.arguments.size() == 1 &&
             requiresActiveCleanup(type.arguments.front(), query);
    break;
  case SemanticType::Expected:
    result = std::any_of(type.arguments.begin(), type.arguments.end(),
                         [&](const SemanticType &argument) {
                           return argument != SemanticType::Void &&
                                  requiresActiveCleanup(argument, query);
                         });
    break;
  case SemanticType::Enum: {
    if (type.enumId == 0 || type.enumId > enums.size() ||
        !enums[type.enumId - 1].payload) {
      break;
    }
    for (const auto &[_, enumerator] : enums[type.enumId - 1].enumerators) {
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
    const SemanticType &type, std::vector<SemanticType> &visiting) const
    -> ConcurrencyCapabilities {
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
    for (const auto &[_, enumerator] : enums[type.enumId - 1].enumerators) {
      for (const SemanticType &payload : enumerator.payloadTypes) {
        const ConcurrencyCapabilities component =
            typeCapabilities(payload, visiting);
        result.transferCapable =
            result.transferCapable && component.transferCapable;
        result.shareCapable = result.shareCapable && component.shareCapable;
      }
    }
    return result;
  }
  case SemanticType::Array:
  case SemanticType::UniqueOwner:
  case SemanticType::Storage:
  case SemanticType::PrefixStorage:
    if (type.arguments.size() == 1) {
      return typeCapabilities(type.arguments.front(), visiting);
    }
    return {};
  case SemanticType::Expected:
  case SemanticType::Unexpected: {
    ConcurrencyCapabilities result{.transferCapable = true,
                                   .shareCapable = true};
    if (type.arguments.empty()) {
      return {};
    }
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (type.kind == SemanticType::Expected && index == 0 &&
          type.arguments[index] == SemanticType::Void) {
        continue;
      }
      const ConcurrencyCapabilities component =
          typeCapabilities(type.arguments[index], visiting);
      result.transferCapable =
          result.transferCapable && component.transferCapable;
      result.shareCapable = result.shareCapable && component.shareCapable;
    }
    return result;
  }
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
        owner->compilerCapability ==
            CompilerCapabilityTypeKind::OptionalStorage &&
        type.arguments.size() == 1) {
      return typeCapabilities(type.arguments.front(), visiting);
    }
    break;
  case SemanticType::Unknown:
  case SemanticType::Void:
  case SemanticType::StringView:
  case SemanticType::CString:
  case SemanticType::RawPointer:
  case SemanticType::Reference:
  case SemanticType::SharedPointer:
  case SemanticType::TypePack:
  case SemanticType::TypeName:
  case SemanticType::Function:
  case SemanticType::FunctionSignature:
  case SemanticType::NativeFunction:
    return {};
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
    visiting.pop_back();
    return {.transferCapable = false, .shareCapable = true};
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
        typeCapabilities(componentType, visiting);
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
  if (hasSourceDestructorBody(owner) || owner.storedReference) {
    result = {};
  }
  if (owner.transferPolicy == ConcurrencyCapabilityPolicy::Denied) {
    result.transferCapable = false;
  } else if (owner.transferPolicy ==
             ConcurrencyCapabilityPolicy::UnsafeAsserted) {
    result.transferCapable = true;
  }
  if (owner.sharePolicy == ConcurrencyCapabilityPolicy::Denied) {
    result.shareCapable = false;
  } else if (owner.sharePolicy == ConcurrencyCapabilityPolicy::UnsafeAsserted) {
    result.shareCapable = true;
  }
  visiting.pop_back();
  return result;
}

auto SemanticVisitor::Impl::typeTraits(
    const SemanticType &type, std::vector<SemanticType> &visiting) const
    -> SemanticTypeTraits {
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

  if (type.kind == SemanticType::Expected ||
      type.kind == SemanticType::Unexpected) {
    SemanticTypeTraits traits = semanticTraits(type);
    traits.relocation = RelocationKind::Structural;
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      const SemanticType &argument = type.arguments[index];
      if (type.kind == SemanticType::Expected && index == 0 &&
          argument == SemanticType::Void) {
        continue;
      }
      const SemanticTypeTraits argumentTraits = typeTraits(argument, visiting);
      if (argumentTraits.ownership == OwnershipKind::Unique ||
          (traits.ownership == OwnershipKind::Value &&
           argumentTraits.ownership == OwnershipKind::Shared)) {
        traits.ownership = argumentTraits.ownership;
      }
      traits.copyable = traits.copyable && argumentTraits.copyable;
      traits.movable = traits.movable && argumentTraits.movable;
      traits.copyAssignable =
          traits.copyAssignable && argumentTraits.copyAssignable;
      traits.moveAssignable =
          traits.moveAssignable && argumentTraits.moveAssignable;
      traits.containsBorrowedState =
          traits.containsBorrowedState || argumentTraits.containsBorrowedState;
      if (argumentTraits.relocation == RelocationKind::Unavailable)
        traits.relocation = RelocationKind::Unavailable;
    }
    return traits;
  }

  if (type.kind == SemanticType::Enum && type.enumId != 0 &&
      type.enumId <= enums.size() && enums[type.enumId - 1].payload) {
    SemanticTypeTraits traits = semanticTraits(type);
    traits.relocation = RelocationKind::Structural;
    ActiveCleanupQuery cleanupQuery;
    for (const auto &[_, enumerator] : enums[type.enumId - 1].enumerators) {
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
                              .drop = element.drop,
                              .copyable = false,
                              .movable = true,
                              .copyAssignable = false,
                              .moveAssignable = false,
                              .containsBorrowedState = false,
                              .transferCapable = false,
                              .shareCapable = true};
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

auto SemanticVisitor::Impl::reportConcurrentGlobalPolicyFailure(
    const VariableDecl &declaration, const SemanticType &type,
    bool globalStorage) -> bool {
  if (!globalStorage ||
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

auto SemanticVisitor::Impl::isOwnerDependentTextExpression(
    const ExprPtr &expression) const -> bool {
  const ExpressionInfo *info =
      expression ? semanticModel.findExpression(*expression) : nullptr;
  return info != nullptr &&
         (info->type.kind == SemanticType::StringView ||
          info->type.kind == SemanticType::CString) &&
         info->traits.containsBorrowedState;
}

auto SemanticVisitor::Impl::reportOwnerDependentTextStorage(
    const ExprPtr &expression, std::string_view destination) -> void {
  const ExpressionInfo *info =
      expression ? semanticModel.findExpression(*expression) : nullptr;
  const bool cString =
      info != nullptr && info->type.kind == SemanticType::CString;
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2045", DiagnosticPhase::Semantics, expressionToken(expression),
      (cString ? "An owner-dependent c_string" : "A dynamic std::string_view") +
          std::string(" cannot be ") + std::string(destination) +
          (cString ? " in the single-owner text-borrow model."
                   : " in the single-owner view model."));
  diagnostic.hints.emplace_back(cString
                                    ? "Initialize a new local C-string binding "
                                      "in a narrow lexical scope; stored and "
                                      "reassignable dependency graphs are not "
                                      "supported yet."
                                    : "Initialize a new local view binding in "
                                      "a narrow lexical scope; stored and "
                                      "reassignable dependency graphs are not "
                                      "supported yet.");
  diagnostics.emplace_back(std::move(diagnostic));
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

auto SemanticVisitor::Impl::storedReferencePaths(const SemanticType &type) const
    -> std::vector<StoredReferencePath> {
  std::vector<SemanticType> visiting;
  const auto visit =
      [&](const auto &self,
          const SemanticType &current) -> std::vector<StoredReferencePath> {
    const ClassInfo *owner = classInfo(current);
    if (owner == nullptr || owner->kind == ClassKind::Union ||
        owner->cAbiRecord || owner->cOpaqueHandle ||
        (hasSourceDestructorBody(*owner) &&
         (!visiting.empty() || !isDefaultLibraryMutexGuardCarrier(*owner))) ||
        std::find(visiting.begin(), visiting.end(), current) !=
            visiting.end()) {
      return {};
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
        continue;
      }
      const SemanticType fieldType = substituteType(
          substituteType(member->second.symbol.type, substitution),
          GenericSubstitution{.types = instanceTypeSubstitution,
                              .values = instanceValueSubstitution});
      std::vector<StoredReferencePath> children;
      if (fieldType.kind == SemanticType::Reference) {
        if (fieldType.arguments.size() != 1) {
          invalid = true;
          continue;
        }
        children.push_back(StoredReferencePath{.reference = fieldType});
      } else if (typeTraits(fieldType).containsBorrowedState) {
        children = self(self, fieldType);
        if (children.empty()) {
          invalid = true;
          continue;
        }
      }
      for (StoredReferencePath &child : children) {
        child.fields.insert(child.fields.begin(), field.declaration);
        paths.push_back(std::move(child));
      }
    }
    visiting.pop_back();
    return invalid ? std::vector<StoredReferencePath>{} : paths;
  };
  return visit(visit, type);
}

auto SemanticVisitor::Impl::storedReferencePath(const SemanticType &type) const
    -> std::optional<StoredReferencePath> {
  auto paths = storedReferencePaths(type);
  if (paths.size() != 1) {
    return std::nullopt;
  }
  return std::move(paths.front());
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
  const auto paths = storedReferencePaths(type);
  const ClassInfo *owner = classInfo(type);
  if (paths.empty() || owner == nullptr || constructor.declaration == nullptr) {
    return {};
  }
  std::vector<std::size_t> result;
  std::unordered_set<const VariableDecl *> visitedFields;
  for (const StoredReferencePath &path : paths) {
    if (path.fields.empty()) {
      return {};
    }
    const VariableDecl *field = path.fields.front();
    if (!visitedFields.insert(field).second) {
      continue;
    }
    const auto member = owner->members.find(field->name().lexeme);
    if (member == owner->members.end()) {
      return {};
    }
    const SemanticType fieldType =
        substituteType(member->second.symbol.type, classSubstitution(type));
    std::optional<std::size_t> selected;
    for (const ConstructorInitializer &initializer :
         constructor.declaration->initializers()) {
      if (initializer.target.name.segments.size() != 1 ||
          initializer.target.name.last().lexeme != field->name().lexeme ||
          initializer.arguments.size() != 1) {
        continue;
      }
      const ExprPtr *argument = &initializer.arguments.front();
      while (const auto *group = exprAs<Grouping>(argument->get())) {
        argument = &group->expression();
      }
      if (const Call *call = directCall(*argument)) {
        const auto intrinsic = intrinsicCandidate(call->callee());
        if (!intrinsic ||
            intrinsic->record().intrinsic != IntrinsicKind::Move ||
            call->arguments().size() != 1 ||
            fieldType.kind == SemanticType::Reference) {
          return {};
        }
        argument = &call->arguments().front();
      }
      const Variable *source = movedVariable(*argument);
      if (source == nullptr) {
        return {};
      }
      const auto &parameters = constructor.declaration->parameters();
      for (std::size_t index = 0;
           index < parameters.size() && index < parameterTypes.size();
           ++index) {
        if (parameters[index].name.lexeme == source->name().lexeme &&
            parameterTypes[index] == fieldType && !parameters[index].pack) {
          selected = index;
          break;
        }
      }
    }
    if (!selected) {
      return {};
    }
    if (std::find(result.begin(), result.end(), *selected) == result.end()) {
      result.push_back(*selected);
    }
  }
  return result;
}

auto SemanticVisitor::Impl::isDirectStoredReferenceType(
    const SemanticType &type) const -> bool {
  return !storedReferencePaths(type).empty();
}

auto SemanticVisitor::Impl::isFallibleBorrowType(const SemanticType &type) const
    -> bool {
  if (type.kind != SemanticType::Expected || type.arguments.size() != 2) {
    return false;
  }
  const SemanticType &success = type.arguments.front();
  return success.kind == SemanticType::Reference &&
         success.referenceAccess == AccessMode::ReadOnly &&
         success.arguments.size() == 1 &&
         !typeTraits(success.arguments.front()).containsBorrowedState &&
         !typeTraits(type.arguments.back()).containsBorrowedState;
}

auto SemanticVisitor::Impl::fallibleBorrowReferent(
    const SemanticType &type) const -> std::optional<SemanticType> {
  if (!isFallibleBorrowType(type)) {
    return std::nullopt;
  }
  return type.arguments.front().arguments.front();
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
         isDirectStoredReferenceType(type) || isFallibleBorrowType(type);
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

  const auto readOnlyDependency = [&](const SemanticType &parameter) {
    const bool readOnlyReference =
        parameter.kind == SemanticType::Reference &&
        parameter.referenceAccess == AccessMode::ReadOnly &&
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
  const FunctionReturnBorrowSummary summary = functionReturnBorrowSummary(
      function.returnType, function.parameterTypes, function.ownerClass,
      function.staticMember, function.record().intrinsic);
  function.returnBorrowOrigin = summary.origin;
  function.returnBorrowParameter = summary.parameter;
  function.returnBorrowAccess = summary.access;
  function.returnBorrowParameters = summary.parameters;
  function.returnBorrowPlace = summary.place;
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
            {tokenSpan(function.parameters()[index].name),
             "Eligible owner dependency source is here."});
      }
    }
    diagnostic.hints.emplace_back(
        summary.access == AccessMode::Mutable
            ? "Return the borrowed value from a mutable instance method or "
              "reduce the function to one mutable dependency source."
            : "Return the borrowed value from an instance method or reduce "
              "the function to one read-only dependency source.");
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
  if (isFallibleBorrowType(type)) {
    return AccessMode::ReadOnly;
  }
  const ClassInfo *owner = classInfo(type);
  if (owner != nullptr && isDefaultLibraryMutexGuardCarrier(*owner)) {
    return AccessMode::Mutable;
  }
  const auto path = storedReferencePath(type);
  return path ? path->reference.referenceAccess : AccessMode::ReadOnly;
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

auto SemanticVisitor::Impl::isBorrowedCarrierReference(
    const SemanticType &type) const -> bool {
  if (type.kind != SemanticType::Reference || type.arguments.size() != 1 ||
      !isDirectStoredReferenceType(type.arguments.front())) {
    return false;
  }
  return true;
}

auto SemanticVisitor::Impl::isDirectOwnerType(const SemanticType &type)
    -> bool {
  return type.kind == SemanticType::UniqueOwner ||
         type.kind == SemanticType::Storage ||
         type.kind == SemanticType::PrefixStorage;
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
    if (name == "thread_auto_join" || name == "thread_result_auto_join") {
      return IntrinsicKind::ThreadAutoJoin;
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
    if (name == "allocate_storage") {
      return IntrinsicKind::AllocateStorage;
    }
    if (name == "storage_try_allocate") {
      return IntrinsicKind::StorageTryAllocate;
    }
    if (name == "storage_capacity") {
      return IntrinsicKind::StorageCapacity;
    }
    if (name == "storage_initialized") {
      return IntrinsicKind::StorageInitialized;
    }
    if (name == "storage_construct") {
      return IntrinsicKind::StorageConstruct;
    }
    if (name == "storage_read") {
      return IntrinsicKind::StorageRead;
    }
    if (name == "storage_read_mut") {
      return IntrinsicKind::StorageReadMut;
    }
    if (name == "storage_destroy") {
      return IntrinsicKind::StorageDestroy;
    }
    if (name == "storage_relocate") {
      return IntrinsicKind::StorageRelocate;
    }
    if (name == "storage_shift_right") {
      return IntrinsicKind::StorageShiftRight;
    }
    if (name == "storage_shift_left") {
      return IntrinsicKind::StorageShiftLeft;
    }
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
    if (name == "integer_checked_add") {
      return IntrinsicKind::IntegerCheckedAdd;
    }
    if (name == "integer_checked_sub") {
      return IntrinsicKind::IntegerCheckedSubtract;
    }
    if (name == "integer_checked_mul") {
      return IntrinsicKind::IntegerCheckedMultiply;
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
  if (name == "require_key") {
    return IntrinsicKind::RequireKey;
  }
  if (name == "require_container_argument") {
    return IntrinsicKind::RequireContainerArgument;
  }
  if (name == "allocate_prefix_storage") {
    return IntrinsicKind::AllocatePrefixStorage;
  }
  if (name == "prefix_storage_append") {
    return IntrinsicKind::PrefixStorageAppend;
  }
  if (name == "prefix_storage_pop") {
    return IntrinsicKind::PrefixStoragePop;
  }
  if (name == "prefix_storage_read") {
    return IntrinsicKind::PrefixStorageRead;
  }
  if (name == "prefix_storage_read_mut") {
    return IntrinsicKind::PrefixStorageReadMut;
  }
  if (name == "prefix_storage_length") {
    return IntrinsicKind::PrefixStorageLength;
  }
  if (name == "prefix_storage_relocate") {
    return IntrinsicKind::PrefixStorageRelocate;
  }
  if (name == "prefix_storage_insert") {
    return IntrinsicKind::PrefixStorageInsert;
  }
  if (name == "prefix_storage_erase") {
    return IntrinsicKind::PrefixStorageErase;
  }
  if (name == "prefix_storage_text_view") {
    return IntrinsicKind::PrefixStorageTextView;
  }
  if (name == "prefix_storage_c_string") {
    return IntrinsicKind::PrefixStorageCString;
  }
  if (name == "prefix_storage_try_c_string") {
    return IntrinsicKind::PrefixStorageTryCString;
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

auto SemanticVisitor::Impl::storedBorrowDependencies(const ExprPtr &expression)
    -> std::vector<SemanticLoanDependency> {
  if (!expression) {
    return {};
  }
  if (const auto *group = exprAs<Grouping>(expression.get())) {
    return storedBorrowDependencies(group->expression());
  }
  if (const ResolvedConstructionInfo *construction =
          semanticModel.findConstruction(*expression);
      construction != nullptr && !construction->borrowArguments.empty()) {
    const auto *call = exprAs<Call>(expression.get());
    const auto *initializer = exprAs<DirectInitializer>(expression.get());
    const auto *arguments = call != nullptr          ? &call->arguments()
                            : initializer != nullptr ? &initializer->arguments()
                                                     : nullptr;
    if (arguments == nullptr) {
      return {};
    }
    std::vector<SemanticLoanDependency> dependencies;
    for (const std::size_t parameter : construction->borrowArguments) {
      if (parameter >= arguments->size()) {
        return {};
      }
      auto sources = storedBorrowDependencies((*arguments)[parameter]);
      if (sources.empty()) {
        return {};
      }
      dependencies.insert(dependencies.end(), sources.begin(), sources.end());
    }
    return dependencies;
  }
  if (const auto *call = exprAs<Call>(expression.get())) {
    if (const auto *resolved = semanticModel.findCall(*call);
        resolved != nullptr && !resolved->borrowArguments.empty()) {
      std::vector<SemanticLoanDependency> dependencies;
      for (const auto parameter : resolved->borrowArguments) {
        if (parameter >= call->arguments().size()) {
          return {};
        }
        const auto sources =
            storedBorrowDependencies(call->arguments()[parameter]);
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
    if (loan != nullptr && loan->storedDependency != 0) {
      loan = semanticModel.findLoan(loan->storedDependency);
    }
    if (loan != nullptr) {
      std::vector<SemanticLoanDependency> dependencies{
          {.place = loan->place,
           .designatedFields = loan->designatedFields,
           .access = loan->access,
           .protectsStorage = loan->protectsStorage}};
      dependencies.insert(dependencies.end(),
                          loan->additionalDependencies.begin(),
                          loan->additionalDependencies.end());
      return dependencies;
    }
  }
  SemanticLoanPlace place = retainedLoanPlace(expression);
  if (place.root == 0 && !place.receiver) {
    return {};
  }
  return {{.place = std::move(place),
           .access = AccessMode::ReadOnly,
           .protectsStorage = hasStableBorrowStorage(expression)}};
}

auto SemanticVisitor::Impl::storedBorrowSource(const ExprPtr &expression) const
    -> const ExprPtr * {
  if (!expression) {
    return nullptr;
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    const Symbol *symbol = resolve(variable->name());
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
  const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
  if (resolved == nullptr) {
    return nullptr;
  }
  if (resolved->borrowOrigin == BorrowOriginKind::Argument) {
    return resolved->borrowArgument < call->arguments().size()
               ? &call->arguments()[resolved->borrowArgument]
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

auto SemanticVisitor::Impl::retainedLoanSource(const ExprPtr &expression) const
    -> std::optional<RetainedLoanSource> {
  if (!expression) {
    return std::nullopt;
  }
  if (exprAs<This>(expression.get()) != nullptr) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      const auto found = scope->find(std::string(receiverStateName));
      if (found != scope->end() && found->second.retainedLoan != 0)
        return RetainedLoanSource{.loan = found->second.retainedLoan};
    }
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    const Symbol *symbol = resolve(variable->name());
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
          return RetainedLoanSource{.loan = found->second.retainedLoan};
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
    return retainedLoanSource(member->object());
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
  if (resolved->borrowOrigin == BorrowOriginKind::Argument &&
      resolved->borrowArgument < call->arguments().size()) {
    return retainedLoanSource(call->arguments()[resolved->borrowArgument]);
  }
  if (resolved->borrowOrigin == BorrowOriginKind::Receiver) {
    const auto *member = exprAs<Get>(call->callee().get());
    if (member != nullptr) {
      return retainedLoanSource(member->object());
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

auto SemanticVisitor::Impl::loanPlace(SemanticPlace place)
    -> SemanticLoanPlace {
  if (place.root == nullptr) {
    return {};
  }

  SemanticLoanPlace result;
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
  return {.domain = {.snapshot = semanticModel.placeSnapshot()},
          .root = place.root,
          .projections = place.projections};
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
                 resolved->borrowArgument < call->arguments().size()) {
        return loanPlace(call->arguments()[resolved->borrowArgument]);
      }
    }
    if (resolved != nullptr &&
        (resolved->intrinsic == IntrinsicKind::ExpectedValue ||
         resolved->intrinsic == IntrinsicKind::ExpectedError)) {
      const auto *member = exprAs<Get>(call->callee().get());
      if (member != nullptr) {
        // Expected alternatives share one active state and storage root.
        // Conflict checking therefore conservatively treats access through
        // either projection as access to the complete expected object. The
        // range-only retention permission remains gated separately by the
        // exact ResolvedCallInfo borrow origin above.
        return loanPlace(member->object());
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
  SemanticLoanId current = descendant;
  std::unordered_set<SemanticLoanId> visited;
  while (current != 0 && visited.insert(current).second) {
    if (current == ancestor) {
      return true;
    }
    const SemanticLoanInfo *loan = semanticModel.findLoan(current);
    current = loan == nullptr ? 0 : loan->parent;
  }
  return false;
}

auto SemanticVisitor::Impl::accessLoan(const SemanticPlace &place) const
    -> SemanticLoanId {
  if (place.root == nullptr || place.root->retainedLoan == 0) {
    return 0;
  }
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
  for (const auto &[id, flow] : loanFlow.records) {
    if (!flow.active) {
      continue;
    }
    const SemanticLoanInfo *loan = semanticModel.findLoan(id);
    const bool overlaps =
        loan != nullptr &&
        ((loan->protectsStorage && loanPlacesOverlap(place, loan->place)) ||
         std::any_of(loan->additionalDependencies.begin(),
                     loan->additionalDependencies.end(),
                     [&](const SemanticLoanDependency &dependency) {
                       return dependency.protectsStorage &&
                              loanPlacesOverlap(place, dependency.place);
                     }));
    if (loan == nullptr || !overlaps || loanIsAncestor(id, throughLoan)) {
      continue;
    }
    const bool suspendedParentUse =
        throughLoan != 0 && loanIsAncestor(throughLoan, id);
    if (suspendedParentUse || requested == AccessMode::Mutable ||
        loan->access == AccessMode::Mutable) {
      result.push_back(id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

auto SemanticVisitor::Impl::reportLoanPlaceConflict(
    const Token &token, const SemanticLoanPlace &stable, AccessMode requested,
    std::string message, SemanticLoanId throughLoan) -> void {
  if (stable.root == 0 && !stable.receiver) {
    return;
  }
  std::vector<SemanticLoanId> conflicts =
      conflictingLoans(stable, requested, throughLoan);
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
  reportLoanPlaceConflict(token, loanPlace(place), requested,
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
    const SemanticLoanPlace stable = transientLoanPlace(arguments[index]);
    recordMutableBindingUse(stable.root);
    if (stable.root == 0 && !stable.receiver) {
      continue;
    }
    const SemanticLoanId throughLoan = accessLoan(arguments[index]);
    if (suppressLoanPlaceAccessDepth == 0 &&
        !conflictingLoans(stable, AccessMode::ReadOnly, throughLoan).empty()) {
      // Ordinary argument analysis already diagnosed this suspended-parent
      // or mutable-loan read. Retain the mutable boundary check only when it
      // adds a conflict that a read-only expression access cannot see.
      continue;
    }
    reportLoanPlaceConflict(
        location, stable, AccessMode::Mutable,
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
          sourcePlace.root->bindingKind == SemanticBindingKind::StaticField));
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
          resolved->borrowArgument < call->arguments().size()) {
        const bool mutableProducer =
            resolved->borrowArgument < resolved->parameterTypes.size() &&
            resolved->parameterTypes[resolved->borrowArgument].kind ==
                SemanticType::Reference &&
            resolved->parameterTypes[resolved->borrowArgument]
                    .referenceAccess == AccessMode::Mutable;
        return fromSource(call->arguments()[resolved->borrowArgument],
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

auto SemanticVisitor::Impl::appendTransientMutationPlaces(
    const ExprPtr &expression, std::vector<SemanticLoanPlace> &places) -> void {
  if (!expression) {
    return;
  }

  if (const auto *assign = exprAs<Assign>(expression.get());
      assign != nullptr && assign->path().segments.size() == 1) {
    if (Symbol *target = resolveMutable(assign->name())) {
      appendUniquePlace(places, loanPlace(placeForSymbol(*target)));
    }
  } else if (const auto *set = exprAs<Set>(expression.get())) {
    appendUniquePlace(places, loanPlace(semanticPlace(*set)));
  } else if (const auto *indexSet = exprAs<IndexSet>(expression.get())) {
    appendUniquePlace(places, loanPlace(semanticPlace(*indexSet)));
  } else if (const auto *dereferenceSet =
                 exprAs<DereferenceSet>(expression.get())) {
    SemanticPlace target = semanticPlace(dereferenceSet->object());
    if (target.root != nullptr) {
      target.projections.push_back({.kind = PlaceProjectionKind::Dereference});
      appendUniquePlace(places, loanPlace(std::move(target)));
    }
  } else if (const auto *postfix = exprAs<Postfix>(expression.get())) {
    if (postfix->oper().kind == TokenKind::PLUS_PLUS ||
        postfix->oper().kind == TokenKind::MINUS_MINUS) {
      appendUniquePlace(places, transientLoanPlace(postfix->expression()));
    }
  } else if (const auto *unary = exprAs<Unary>(expression.get())) {
    if (unary->oper().kind == TokenKind::PLUS_PLUS ||
        unary->oper().kind == TokenKind::MINUS_MINUS) {
      appendUniquePlace(places, transientLoanPlace(unary->right()));
    }
  }

  if (const ResolvedOperatorInfo *resolved =
          semanticModel.findOperator(*expression)) {
    if (resolved->declaration != nullptr &&
        receiverAllowsMutation(resolved->declaration->receiverMutability())) {
      if (const ExprPtr *receiver = operatorReceiverExpression(expression)) {
        appendUniquePlace(places, transientLoanPlace(*receiver));
      }
    }
    if (const auto *call = exprAs<Call>(expression.get())) {
      const std::size_t count =
          std::min(resolved->parameterTypes.size(), call->arguments().size());
      for (std::size_t index = 0; index < count; ++index) {
        if (resolved->parameterTypes[index].kind == SemanticType::Reference &&
            resolved->parameterTypes[index].referenceAccess ==
                AccessMode::Mutable) {
          appendUniquePlace(places,
                            transientLoanPlace(call->arguments()[index]));
        }
      }
    } else if (const auto *binary = exprAs<Binary>(expression.get())) {
      if (!resolved->parameterTypes.empty() &&
          resolved->parameterTypes.front().kind == SemanticType::Reference &&
          resolved->parameterTypes.front().referenceAccess ==
              AccessMode::Mutable) {
        appendUniquePlace(places, transientLoanPlace(binary->right()));
      }
    } else if (const auto *index = exprAs<Index>(expression.get())) {
      if (!resolved->parameterTypes.empty() &&
          resolved->parameterTypes.front().kind == SemanticType::Reference &&
          resolved->parameterTypes.front().referenceAccess ==
              AccessMode::Mutable) {
        appendUniquePlace(places, transientLoanPlace(index->index()));
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
          appendUniquePlace(places, transientLoanPlace((*arguments)[index]));
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
    appendUniquePlace(places, transientLoanPlace(call->arguments().front()));
  }
  if (call != nullptr && resolved != nullptr) {
    if (resolved->declaration != nullptr &&
        resolved->declaration->receiverMutability() ==
            ReceiverMutability::Mutable) {
      if (const auto *member = exprAs<Get>(call->callee().get())) {
        appendUniquePlace(places, transientLoanPlace(member->object()));
      } else if (currentClass) {
        appendUniquePlace(places,
                          loanPlace(SemanticPlace{.root = receiverStateSymbol(),
                                                  .receiver = true}));
      }
    }
    const std::size_t count =
        std::min(resolved->parameterTypes.size(), call->arguments().size());
    for (std::size_t index = 0; index < count; ++index) {
      if (resolved->parameterTypes[index].kind == SemanticType::Reference &&
          resolved->parameterTypes[index].referenceAccess ==
              AccessMode::Mutable) {
        appendUniquePlace(places, transientLoanPlace(call->arguments()[index]));
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
  } else if (const auto *unexpected = exprAs<Unexpected>(expression.get())) {
    appendTransientMutationPlaces(unexpected->error(), places);
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
    appendUniquePlace(places, loanPlace(expression));
    return;
  }
  if (kind == ExprKind::Get || kind == ExprKind::Index) {
    SemanticLoanPlace place = loanPlace(expression);
    const bool stablePlace = place.root != 0 || place.receiver;
    if (stablePlace) {
      appendUniquePlace(places, std::move(place));
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
  } else if (const auto *unexpected = exprAs<Unexpected>(expression.get())) {
    appendTransientReadPlaces(unexpected->error(), places, productions);
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

auto SemanticVisitor::Impl::validateCallPlaceExclusivityImpl(
    std::span<const SemanticType> parameterTypes,
    std::span<const ExprPtr *> arguments, const Token &location,
    std::optional<SemanticLoanPlace> receiver, AccessMode receiverAccess,
    bool transientReceiver) -> void {
  struct CallPlace {
    SemanticLoanPlace place;
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
        storedReferencePaths(parameter).size() > 1) {
      for (const auto &dependency :
           storedBorrowDependencies(*arguments[index])) {
        places.push_back({.place = dependency.place,
                          .access = dependency.access,
                          .argument = index});
      }
      continue;
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
          {.place = std::move(place),
           .access = production && production->access == AccessMode::Mutable
                         ? AccessMode::Mutable
                     : referenceParameter ? parameter.referenceAccess
                                          : AccessMode::ReadOnly,
           .argument = index});
    }
  }
  if (receiver && (receiver->root != 0 || receiver->receiver)) {
    places.push_back({.place = std::move(*receiver),
                      .access = receiverAccess,
                      .argument = arguments.size(),
                      .receiver = true,
                      .transient = transientReceiver});
  }
  for (std::size_t left = 0; left < places.size(); ++left) {
    for (std::size_t right = left + 1; right < places.size(); ++right) {
      if ((places[left].access == AccessMode::Mutable ||
           places[right].access == AccessMode::Mutable) &&
          loanPlacesOverlap(places[left].place, places[right].place)) {
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
      if (argument != retained.argument &&
          loanPlacesOverlap(retained.place, mutation)) {
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
      productions.push_back({.sequence = 0,
                             .argument = std::nullopt,
                             .place = retained.place,
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
    bool transientReceiver) -> void {
  std::vector<const ExprPtr *> argumentReferences;
  argumentReferences.reserve(arguments.size());
  for (const ExprPtr &argument : arguments) {
    argumentReferences.push_back(&argument);
  }
  validateCallPlaceExclusivityImpl(parameterTypes, argumentReferences, location,
                                   std::move(receiver), receiverAccess,
                                   transientReceiver);
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
  const std::size_t index = loanFlow.loops.size();
  loanFlow.loops.push_back({.region = loanFlow.currentRegion,
                            .order = loanFlow.currentOrder,
                            .statement = &statement,
                            .firstNestedLoan = nextSemanticLoanId});
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
    SemanticLoanId parent, bool entry, std::vector<SymbolId> designatedFields)
    -> SemanticLoanId {
  const SemanticLoanId id = nextSemanticLoanId++;
  const ClassInfo *carrierOwner = classInfo(carrier.type);
  const bool lifecycleBound = carrierOwner != nullptr &&
                              isDefaultLibraryMutexGuardCarrier(*carrierOwner);
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
    bool receiver) -> SemanticLoanId {
  const SemanticLoanPlace place{.root = carrierSymbol, .receiver = receiver};
  const auto paths = storedReferencePaths(type);
  const bool abstractEntry =
      receiver || carrier.type.kind == SemanticType::Reference;
  std::vector<SymbolId> designatedFields;
  std::vector<SemanticLoanDependency> additional;
  for (const auto &path : paths) {
    SemanticType ownerType = type;
    std::vector<SymbolId> fields;
    for (const VariableDecl *field : path.fields) {
      const ClassInfo *owner = classInfo(ownerType);
      if (owner == nullptr)
        return 0;
      const auto member = owner->members.find(field->name().lexeme);
      if (member == owner->members.end())
        return 0;
      const SymbolId symbol = toolingSymbolFor(member->second.symbol);
      if (symbol == 0)
        return 0;
      fields.push_back(symbol);
      ownerType = substituteType(member->second.symbol.type,
                                 classSubstitution(ownerType));
    }
    if (designatedFields.empty())
      designatedFields = std::move(fields);
    else
      additional.push_back({.place = place,
                            .designatedFields = std::move(fields),
                            .access = AccessMode::ReadOnly});
  }
  if (designatedFields.empty())
    return 0;
  const SemanticLoanId id =
      createRetainedLoan(carrier, carrierSymbol, SemanticLoanKind::StoredValue,
                         nullptr, place, carrierSymbol, receiver,
                         abstractEntry ? paths.front().reference.referenceAccess
                                       : borrowAccess(type),
                         false, 0, true, std::move(designatedFields));
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
    loan.entryFunction = receiver ? currentFunctionDeclaration : nullptr;
  }
  semanticModel.recordLoan(std::move(loan));
  return id;
}

auto SemanticVisitor::Impl::recordParameterEntryLoan(const Parameter &parameter)
    -> void {
  Symbol *carrier = resolveMutable(parameter.name);
  if (carrier == nullptr) {
    return;
  }
  if (parameter.pack && carrier->type.kind == SemanticType::TypePack &&
      carrier->type.concretePack) {
    // Elements cannot be named independently in the source language. Their
    // entry capabilities remain live for the whole call and whole-pack
    // forwarding reborrows each exact element, never the aggregate binding.
    const SymbolId symbol = toolingSymbolFor(*carrier);
    for (std::size_t index = 0; index < carrier->type.arguments.size();
         ++index) {
      const auto &element = carrier->type.arguments[index];
      if (element.kind != SemanticType::Reference ||
          element.referenceAccess != AccessMode::Mutable) {
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
           .access = AccessMode::Mutable,
           .protectsStorage = true,
           .carriers = {symbol}});
    }
    return;
  }
  const bool referenceCarrier = carrier->type.kind == SemanticType::Reference &&
                                carrier->type.arguments.size() == 1;
  const FunctionInfo *function = currentFunctionInfo();
  const bool summarizedParameterSource =
      function != nullptr && function->declaration != nullptr &&
      function->returnBorrowOrigin == BorrowOriginKind::Argument &&
      function->returnBorrowParameter <
          function->declaration->parameters().size() &&
      (&function->declaration->parameters()[function->returnBorrowParameter] ==
           &parameter ||
       std::any_of(
           function->returnBorrowParameters.begin(),
           function->returnBorrowParameters.end(),
           [&](std::size_t index) {
             return index < function->declaration->parameters().size() &&
                    &function->declaration->parameters()[index] == &parameter;
           }));
  const bool dynamicTextCarrier =
      carrier->type.kind == SemanticType::StringView ||
      carrier->type.kind == SemanticType::CString;
  const bool storedValueCarrier =
      isDirectStoredReferenceType(carrier->type) || dynamicTextCarrier;
  if (!referenceCarrier && !storedValueCarrier) {
    return;
  }
  const bool summarizedReadOnlySource =
      referenceCarrier &&
      carrier->type.referenceAccess == AccessMode::ReadOnly &&
      summarizedParameterSource;
  const bool borrowedShell =
      referenceCarrier &&
      isDirectStoredReferenceType(carrier->type.arguments.front());
  if (referenceCarrier &&
      carrier->type.referenceAccess != AccessMode::Mutable &&
      !summarizedReadOnlySource && !borrowedShell) {
    return;
  }
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

auto SemanticVisitor::Impl::recordRetainedBorrow(const VariableDecl &binding,
                                                 const ExprPtr &expression)
    -> void {
  Symbol *carrier = resolveMutable(binding.name());
  if (carrier == nullptr ||
      carrier->bindingKind != SemanticBindingKind::LocalVariable) {
    return;
  }

  const SymbolId carrierSymbol = toolingSymbolFor(*carrier);
  const ExpressionInfo *info =
      expression ? semanticModel.findExpression(*expression) : nullptr;
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
  if (storedReferencePaths(carrier->type).size() > 1 &&
      ((construction != nullptr && !construction->borrowArguments.empty()) ||
       (resolved != nullptr && !resolved->borrowArguments.empty()))) {
    auto dependencies = storedBorrowDependencies(expression);
    if (dependencies.empty()) {
      report(expressionToken(expression),
             "Every dependency of a composed borrowed value must have a "
             "checked stable owner.",
             "GTI-S2045");
      return;
    }
    for (const auto &dependency : dependencies) {
      if (dependency.access != AccessMode::ReadOnly) {
        report(expressionToken(expression),
               "A composed borrowed value cannot retain an exclusive "
               "dependency.",
               "GTI-S2045");
        return;
      }
      reportBorrowConflict(
          expressionToken(expression),
          conflictingLoans(dependency.place, AccessMode::ReadOnly),
          "Cannot compose borrowed values while an overlapping mutable "
          "borrow may still be live.");
    }
    const auto primary = dependencies.front();
    const SemanticLoanId id = createRetainedLoan(
        *carrier, carrierSymbol, SemanticLoanKind::StoredValue,
        expression.get(), primary.place, primary.place.root,
        primary.place.receiver, AccessMode::ReadOnly, primary.protectsStorage);
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
  if (const auto source = retainedLoanSource(expression)) {
    const SemanticLoanInfo *sourceLoan = semanticModel.findLoan(source->loan);
    if (sourceLoan != nullptr && retainedStoredBorrow &&
        sourceLoan->storedDependency != 0) {
      // Copying a view out of a reference copies its external dependency,
      // not a dependency on the source view object's physical storage.
      sourceLoan = semanticModel.findLoan(sourceLoan->storedDependency);
    }
    if (sourceLoan == nullptr) {
      return;
    }
    if (carrier->type.kind == SemanticType::Reference &&
        carrier->type.arguments.size() == 1 &&
        typeTraits(carrier->type.arguments.front()).containsBorrowedState &&
        sourceLoan->kind == SemanticLoanKind::StoredValue &&
        !source->transfer) {
      // Referencing the carrier is not a copy of its stored dependency.
      // Protect the addressable shell and retain its existing external-owner
      // authority through a separate edge. Neither permission upgrades the
      // other; ordinary reference binding already checks shell access.
      SemanticLoanPlace shell = loanPlace(expression);
      if (shell.root == 0 && !shell.receiver) {
        report(expressionToken(expression),
               "A reference to borrowed-state storage requires a stable "
               "carrier object.",
               "GTI-S2017");
        return;
      }
      const SemanticLoanId dependency = sourceLoan->id;
      reportBorrowConflict(
          expressionToken(expression), conflictingLoans(shell, carrierAccess),
          "Cannot borrow this carrier while an overlapping borrow of its "
          "storage may still be live.");
      const SymbolId owner = shell.root;
      const bool receiver = shell.receiver;
      const SemanticLoanId reference = createRetainedLoan(
          *carrier, carrierSymbol, SemanticLoanKind::Reference,
          expression.get(), std::move(shell), owner, receiver, carrierAccess,
          true);
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
              sourceLoan->access == AccessMode::Mutable) {
            semanticModel.removeLoanCarrier(source->loan, sourceSymbol);
            if (sourceCarrier->variableDeclaration != nullptr) {
              semanticModel.recordBindingLoan(
                  *sourceCarrier->variableDeclaration, 0);
            }
            if (sourceCarrier->parameterDeclaration != nullptr) {
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
    const bool exactMutableStoredReborrow =
        retainedStoredBorrow && isDirectStoredReferenceType(carrier->type) &&
        carrierAccess == AccessMode::Mutable &&
        sourceLoan->access == AccessMode::Mutable;
    if (retainedStoredBorrow && !exactMutableStoredReborrow &&
        (sourceLoan->access == AccessMode::Mutable ||
         sourceLoan->parent != 0)) {
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
      // validateReferenceBinding owns the single actionable upgrade
      // diagnostic. Avoid emitting a second S2017 while constructing the
      // loan graph for the same declaration.
      return;
    }
    if (sourceLoan->access == AccessMode::ReadOnly) {
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
        expressionToken(expression),
        conflictingLoans(childPlace, carrierAccess, sourceLoan->id),
        "Cannot create a child reborrow while another overlapping child "
        "loan may still be live.");
    const SemanticLoanKind kind = retainedStoredBorrow
                                      ? SemanticLoanKind::StoredValue
                                      : SemanticLoanKind::Reference;
    const SemanticLoanId dependency = sourceLoan->storedDependency;
    const SemanticLoanId child = createRetainedLoan(
        *carrier, carrierSymbol, kind, expression.get(), std::move(childPlace),
        sourceLoan->owner, sourceLoan->receiverOrigin, carrierAccess,
        sourceLoan->protectsStorage, sourceLoan->id);
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

  const auto conditionalBoundary =
      std::find_if(loanFlow.conditionalBoundaries.begin(),
                   loanFlow.conditionalBoundaries.end(),
                   [&](const LoanFlowConditionalBoundary &candidate) {
                     return candidate.region == flow.region;
                   });
  if (conditionalBoundary != loanFlow.conditionalBoundaries.end() &&
      conditionalBoundary->statement != nullptr) {
    if (conditionalBoundary->order >= flow.lastUseOrder) {
      flow.lastUseOrder = conditionalBoundary->order;
      flow.lastUse = conditionalBoundary->statement;
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
  std::vector<SemanticLoanId> pending{retained->second};
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
    }
  }
}

auto SemanticVisitor::Impl::supportsProvenLoanEnd(
    const LoanFlowRecord &loan) const -> bool {
  if (stmtAs<VariableDecl>(loan.lastUse) != nullptr ||
      stmtAs<ExpressionStmt>(loan.lastUse) != nullptr ||
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
    std::string message) -> void {
  std::vector<SemanticLoanId> active;
  for (const SemanticLoanId loan : loans) {
    if (loanFlow.records.contains(loan)) {
      appendUniqueLoan(active, loan);
    }
  }
  if (active.empty()) {
    return;
  }
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2017", DiagnosticPhase::Semantics, token, std::move(message));
  if (loanFlow.currentStatement == nullptr || loanFlow.currentRegion == 0) {
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  loanFlow.conflicts.push_back({.loans = std::move(active),
                                .region = loanFlow.currentRegion,
                                .order = loanFlow.currentOrder,
                                .sequence = loanFlow.nextSequence++,
                                .statement = loanFlow.currentStatement,
                                .path = loanFlow.activeConditionalArms,
                                .switchPath = loanFlow.activeSwitchArms,
                                .diagnostic = std::move(diagnostic)});
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
  if (loanFlow.deferredConstexprSelection) {
    // Unselected arms can add uses and conflicts. Publishing endpoints from
    // this incomplete primary would assert liveness that was never proved.
    return;
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
  for (std::size_t conflictIndex = 0; conflictIndex < loanFlow.conflicts.size();
       ++conflictIndex) {
    PendingBorrowConflict &conflict = loanFlow.conflicts[conflictIndex];
    std::vector<SemanticLoanId> liveLoans;
    for (const SemanticLoanId id : conflict.loans) {
      const auto found = loanFlow.records.find(id);
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
    if (!liveLoans.empty()) {
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
      }
      conflict.diagnostic.hints.emplace_back(
          "Place the borrowed value in a narrower block when an explicit "
          "lexical end is required across control flow.");
      diagnostics.emplace_back(std::move(conflict.diagnostic));
    }
  }
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
        function->declaration->body() == nullptr ||
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
    if (candidate.id == 0 || candidate.declaration == nullptr ||
        candidate.access != AccessModifier::Public ||
        candidate.compilerPrivate || candidate.pureVirtual ||
        candidate.returnType != resultType ||
        !candidate.parameterTypes.empty() ||
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
          "raw pointers. A result must satisfy the same cross-thread "
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

  currentType =
      intrinsic == IntrinsicKind::ThreadJoinable   ? SemanticType::Bool
      : intrinsic == IntrinsicKind::ThreadAutoJoin ? SemanticType::Void
      : intrinsic == IntrinsicKind::ThreadJoin     ? declaration.returnType
                                                   : SemanticType::Int32;
  if (intrinsic == IntrinsicKind::ThreadJoin &&
      *resultType != SemanticType::Void &&
      currentType.kind == SemanticType::Expected &&
      currentType.arguments.size() == 2) {
    currentType.arguments.front() = *resultType;
  }
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
      intrinsic == IntrinsicKind::AtomicLoad ? 1
      : intrinsic == IntrinsicKind::AtomicStore ||
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
    currentType = intrinsic == IntrinsicKind::AtomicStore ? SemanticType::Void
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

auto SemanticVisitor::Impl::analyzeIntrinsicCall(
    const Call &expr, const CallableCandidate &declaration) -> void {
  const IntrinsicKind intrinsic = declaration.record().intrinsic;
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
  if (intrinsic >= IntrinsicKind::SharedOwnerEmpty &&
      intrinsic <= IntrinsicKind::SharedOwnerUpcast) {
    analyzeSharedOwnerIntrinsicCall(expr, declaration);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::RequireKey ||
      intrinsic == IntrinsicKind::RequireContainerArgument) {
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
  if (intrinsic == IntrinsicKind::ThreadHandleEmpty ||
      intrinsic == IntrinsicKind::ThreadSpawn ||
      intrinsic == IntrinsicKind::ThreadJoinable ||
      intrinsic == IntrinsicKind::ThreadJoin ||
      intrinsic == IntrinsicKind::ThreadAutoJoin) {
    analyzeThreadIntrinsicCall(expr, declaration);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }
  if (intrinsic == IntrinsicKind::AtomicLoad ||
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
  if (intrinsic == IntrinsicKind::AllocatePrefixStorage ||
      intrinsic == IntrinsicKind::PrefixStorageAppend ||
      intrinsic == IntrinsicKind::PrefixStoragePop ||
      intrinsic == IntrinsicKind::PrefixStorageRead ||
      intrinsic == IntrinsicKind::PrefixStorageReadMut ||
      intrinsic == IntrinsicKind::PrefixStorageLength ||
      intrinsic == IntrinsicKind::PrefixStorageRelocate ||
      intrinsic == IntrinsicKind::PrefixStorageInsert ||
      intrinsic == IntrinsicKind::PrefixStorageErase ||
      intrinsic == IntrinsicKind::PrefixStorageTextView ||
      intrinsic == IntrinsicKind::PrefixStorageCString ||
      intrinsic == IntrinsicKind::PrefixStorageTryCString) {
    analyzePrefixStorageIntrinsicCall(expr, intrinsic);
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
  if (intrinsic != IntrinsicKind::Move) {
    analyzeStorageIntrinsicCall(expr, intrinsic);
    bindIntrinsicCallDeclaration(expr, declaration);
    return;
  }

  analyzeMoveCall(expr, &declaration);
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
    setProjectedValueState(place, ValueState::Moved);
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
       place.root->bindingKind == SemanticBindingKind::StaticField)) {
    report(expressionToken(argument),
           place.root->bindingKind == SemanticBindingKind::GlobalVariable
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
    setProjectedValueState(place, ValueState::Moved);
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
    if (symbol && symbol->bindingKind == SemanticBindingKind::GlobalVariable) {
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
  if (symbol->bindingKind == SemanticBindingKind::GlobalVariable) {
    report(variable->name(),
           "std::move cannot consume global binding '" +
               variable->name().lexeme +
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
  mutableSymbol->valueState = ValueState::Moved;
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
  mutableSymbol->valueState = ValueState::Moved;
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
  const bool checkedResult =
      arithmetic && arithmetic->mode == IntegerArithmeticMode::CheckedResult;
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (!checkedResult && !expr.typeArguments().empty()) {
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

  if (checkedResult) {
    const SemanticType resultType = expr.typeArguments().empty()
                                        ? SemanticType::Unknown
                                        : typeOf(expr.typeArguments().front());
    const SemanticType explicitValueType =
        expr.typeArguments().size() < 2 ? SemanticType::Unknown
                                        : typeOf(expr.typeArguments()[1]);
    if (expr.typeArguments().size() != 2 ||
        resultType.kind != SemanticType::Expected ||
        resultType.arguments.size() != 2 || resultType.arguments[0] != left ||
        resultType.arguments[1].kind != SemanticType::Enum ||
        explicitValueType != left) {
      report(expr.paren(),
             "Checked integer arithmetic requires an expected<T, E> "
             "result whose value type exactly matches its operands.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = resultType;
  } else {
    currentType = left;
  }
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

auto SemanticVisitor::Impl::analyzePrefixStorageIntrinsicCall(
    const Call &expr, IntrinsicKind intrinsic) -> void {
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }
  if (intrinsic == IntrinsicKind::AllocatePrefixStorage) {
    if (expr.typeArguments().size() != 1 || argumentTypes.size() != 1 ||
        argumentTypes.front() != SemanticType::UInt64) {
      for (const TypeRef &argument : expr.typeArguments()) {
        validateType(argument);
      }
      report(expr.paren(),
             "gti_internal::allocate_prefix_storage<T> requires exactly "
             "one element type and a uint64_t capacity.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }
    const TypeRef &elementRef = expr.typeArguments().front();
    validateType(elementRef);
    const SemanticType elementType = typeOf(elementRef);
    currentType = SemanticType::prefixStorageOf(elementType);
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = std::move(argumentTypes),
                               .typeArguments = {elementType},
                               .intrinsic = intrinsic});
    return;
  }
  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (!expr.typeArguments().empty()) {
    report(expr.paren(),
           "Prefix storage operations infer their element type from the "
           "storage argument and do not take explicit type arguments.",
           "GTI-S2019");
  }
  if (argumentTypes.empty() ||
      argumentTypes.front().kind != SemanticType::PrefixStorage ||
      argumentTypes.front().arguments.size() != 1) {
    report(expr.paren(),
           "Prefix storage operations require "
           "gti_internal::prefix_storage<T> as their first argument.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  const SemanticType storageType = argumentTypes.front();
  const SemanticType elementType = storageType.arguments.front();
  const auto analyzeElementConstruction = [&](std::size_t argumentOffset) {
    if (argumentTypes.size() < argumentOffset) {
      return;
    }
    if (elementType.kind == SemanticType::StringView ||
        elementType.kind == SemanticType::CString) {
      for (std::size_t index = argumentOffset; index < argumentTypes.size();
           ++index) {
        if (isOwnerDependentTextExpression(expr.arguments()[index])) {
          reportOwnerDependentTextStorage(expr.arguments()[index],
                                          "nested in dynamic storage");
        }
      }
    }
    std::vector<AnalyzedCallArgument> constructorArguments;
    bool concreteArguments = true;
    for (std::size_t index = argumentOffset; index < argumentTypes.size();
         ++index) {
      if (!appendConcreteCallArgument(argumentTypes[index],
                                      expr.arguments()[index],
                                      constructorArguments)) {
        concreteArguments = false;
        break;
      }
    }
    if (!concreteArguments) {
      return;
    }
    if (elementType.kind == SemanticType::Class) {
      analyzeConstructorCallArguments(
          expr, elementType.classId, elementType.arguments,
          elementType.valueArguments, std::move(constructorArguments),
          expr.paren());
    } else if (elementType != SemanticType::Unknown &&
               elementType.kind != SemanticType::TypeParameter) {
      validateStorageValueConstruction(expr, elementType, constructorArguments);
    }
  };
  const auto requireUInt64 = [&](std::size_t index) {
    if (argumentTypes.size() > index &&
        argumentTypes[index] != SemanticType::UInt64 &&
        argumentTypes[index] != SemanticType::Unknown) {
      report(expressionToken(expr.arguments()[index]),
             "Prefix storage indices are uint64_t values.", "GTI-S2019");
    }
  };
  const auto requireMoveConstructibleElement = [&](std::string_view name) {
    if (!satisfiesConstraint(
            elementType,
            constraintBit(GenericConstraintKind::MoveConstructible))) {
      report(expr.paren(),
             std::string(name) +
                 " requires a move-constructible element type, but '" +
                 typeSpelling(elementType) +
                 "' does not guarantee move construction.",
             "GTI-S2019");
    }
  };
  switch (intrinsic) {
  case IntrinsicKind::PrefixStorageAppend:
    // Appends exactly at the live length, constructing in place from the
    // analyzed arguments; movability is a shift requirement only.
    analyzeElementConstruction(1);
    currentType = SemanticType::Void;
    break;
  case IntrinsicKind::PrefixStorageInsert:
    // Inserts inside the live prefix: the element shift and the
    // construction happen inside one sealed operation, so the prefix
    // invariant holds before and after with no observable gap.
    requireMoveConstructibleElement("prefix_storage_insert");
    if (argumentTypes.size() < 2) {
      report(expr.paren(),
             "prefix_storage_insert expects mutable prefix storage, a "
             "uint64_t index, and the element constructor arguments.",
             "GTI-S2019");
    }
    requireUInt64(1);
    analyzeElementConstruction(2);
    currentType = SemanticType::Void;
    break;
  case IntrinsicKind::PrefixStorageErase:
    requireMoveConstructibleElement("prefix_storage_erase");
    if (argumentTypes.size() != 2) {
      report(expr.paren(),
             "prefix_storage_erase expects mutable prefix storage and a "
             "uint64_t index.",
             "GTI-S2019");
    }
    requireUInt64(1);
    currentType = SemanticType::Void;
    break;
  case IntrinsicKind::PrefixStoragePop:
    if (argumentTypes.size() != 1) {
      report(expr.paren(),
             "prefix_storage_pop expects mutable prefix storage only.",
             "GTI-S2019");
    }
    currentType = SemanticType::Void;
    break;
  case IntrinsicKind::PrefixStorageRead:
  case IntrinsicKind::PrefixStorageReadMut:
    if (argumentTypes.size() != 2) {
      report(expr.paren(),
             "Prefix storage reads expect the storage and a uint64_t "
             "index.",
             "GTI-S2019");
    }
    requireUInt64(1);
    // Borrows present as place-category expressions of the element
    // value type; the reference lives in the resolved-call record.
    currentType = elementType;
    break;
  case IntrinsicKind::PrefixStorageLength:
    if (argumentTypes.size() != 1) {
      report(expr.paren(), "prefix_storage_length expects the storage only.",
             "GTI-S2019");
    }
    currentType = SemanticType::UInt64;
    break;
  case IntrinsicKind::PrefixStorageRelocate:
    requireMoveConstructibleElement("prefix_storage_relocate");
    if (argumentTypes.size() != 2 ||
        (argumentTypes[1] != SemanticType::Unknown &&
         argumentTypes[1] != storageType)) {
      report(expr.paren(),
             "prefix_storage_relocate requires source and destination "
             "prefix storage with the same element type.",
             "GTI-S2019");
    }
    currentType = SemanticType::Void;
    break;
  case IntrinsicKind::PrefixStorageTextView:
    if (argumentTypes.size() != 1 || elementType != SemanticType::Char) {
      report(expr.paren(),
             "prefix_storage_text_view requires exactly one read-only "
             "gti_internal::prefix_storage<char> argument.",
             "GTI-S2019");
    }
    currentType = SemanticType::StringView;
    break;
  case IntrinsicKind::PrefixStorageCString:
  case IntrinsicKind::PrefixStorageTryCString:
    if (argumentTypes.size() != 1 || elementType != SemanticType::Char) {
      report(expr.paren(),
             std::string(intrinsic == IntrinsicKind::PrefixStorageCString
                             ? "prefix_storage_c_string"
                             : "prefix_storage_try_c_string") +
                 " requires exactly one read-only "
                 "gti_internal::prefix_storage<char> argument.",
             "GTI-S2019");
    }
    currentType = SemanticType::CString;
    break;
  default:
    currentType = SemanticType::Unknown;
    break;
  }
  const bool borrows = intrinsic == IntrinsicKind::PrefixStorageRead ||
                       intrinsic == IntrinsicKind::PrefixStorageReadMut ||
                       intrinsic == IntrinsicKind::PrefixStorageTextView ||
                       intrinsic == IntrinsicKind::PrefixStorageCString ||
                       intrinsic == IntrinsicKind::PrefixStorageTryCString;
  const bool returnsTextView =
      intrinsic == IntrinsicKind::PrefixStorageTextView;
  const bool returnsCString =
      intrinsic == IntrinsicKind::PrefixStorageCString ||
      intrinsic == IntrinsicKind::PrefixStorageTryCString;
  if (intrinsic == IntrinsicKind::PrefixStorageAppend) {
    if (const auto *construction = semanticModel.findConstruction(expr);
        construction != nullptr &&
        construction->kind == ConstructorKind::Ordinary &&
        construction->constructor != 0 && !construction->generatedDefault &&
        !argumentTypes.empty()) {
      // Preserve the selected constructor's reference/value roles rather than
      // replacing them with the expressions' (possibly pack-grouped) types.
      argumentTypes.resize(1);
      argumentTypes.insert(argumentTypes.end(),
                           construction->parameterTypes.begin(),
                           construction->parameterTypes.end());
    }
  }
  semanticModel.record(
      expr,
      ResolvedCallInfo{
          .returnType =
              returnsTextView
                  ? SemanticType::StringView
                  : (returnsCString
                         ? SemanticType::CString
                         : (borrows ? SemanticType::referenceTo(
                                          elementType,
                                          intrinsic == IntrinsicKind::
                                                           PrefixStorageReadMut
                                              ? AccessMode::Mutable
                                              : AccessMode::ReadOnly)
                                    : currentType)),
          .parameterTypes = std::move(argumentTypes),
          .typeArguments = {elementType},
          .intrinsic = intrinsic,
          .borrowOrigin =
              borrows ? BorrowOriginKind::Argument : BorrowOriginKind::None,
          .borrowArgument = 0,
          .borrowAccess = intrinsic == IntrinsicKind::PrefixStorageReadMut
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

auto SemanticVisitor::Impl::analyzeStorageIntrinsicCall(const Call &expr,
                                                        IntrinsicKind intrinsic)
    -> void {
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.emplace_back(analyze(argument));
  }

  if (intrinsic == IntrinsicKind::AllocateStorage) {
    analyzeStorageAllocation(expr, argumentTypes);
    return;
  }

  for (const TypeRef &argument : expr.typeArguments()) {
    validateType(argument);
  }
  if (!expr.typeArguments().empty()) {
    report(expr.paren(),
           "Storage operations infer their element type from the storage "
           "argument and do not take explicit type arguments.",
           "GTI-S2019");
  }

  if (intrinsic == IntrinsicKind::StorageConstruct) {
    if (expr.arguments().size() < 2) {
      report(expr.paren(),
             "storage_construct expects mutable storage, a uint64_t "
             "index, and zero or more constructor arguments.",
             "GTI-S2019");
    }
  } else {
    const std::size_t expectedArguments = [&] {
      switch (intrinsic) {
      case IntrinsicKind::StorageCapacity:
        return std::size_t{1};
      case IntrinsicKind::StorageTryAllocate:
      case IntrinsicKind::StorageRelocate:
      case IntrinsicKind::StorageShiftRight:
      case IntrinsicKind::StorageShiftLeft:
        return std::size_t{3};
      default:
        return std::size_t{2};
      }
    }();
    if (expr.arguments().size() != expectedArguments) {
      reportStorageArity(expr, intrinsic, expectedArguments);
    }
  }

  if (argumentTypes.empty() ||
      argumentTypes.front().kind != SemanticType::Storage ||
      argumentTypes.front().arguments.size() != 1) {
    if (!argumentTypes.empty() &&
        argumentTypes.front() != SemanticType::Unknown) {
      report(expressionToken(expr.arguments().front()),
             storageIntrinsicName(intrinsic) +
                 " requires gti_internal::storage<T> as its first argument.",
             "GTI-S2019");
    }
    currentType = SemanticType::Unknown;
    return;
  }

  const SemanticType storageType = argumentTypes.front();
  const SemanticType elementType = storageType.arguments.front();
  const bool mutatesFirst = intrinsic == IntrinsicKind::StorageTryAllocate ||
                            intrinsic == IntrinsicKind::StorageConstruct ||
                            intrinsic == IntrinsicKind::StorageReadMut ||
                            intrinsic == IntrinsicKind::StorageDestroy ||
                            intrinsic == IntrinsicKind::StorageRelocate ||
                            intrinsic == IntrinsicKind::StorageShiftRight ||
                            intrinsic == IntrinsicKind::StorageShiftLeft;
  if (mutatesFirst) {
    requireMutableStorage(expr.arguments().front(),
                          storageIntrinsicName(intrinsic));
  }

  if (intrinsic == IntrinsicKind::StorageTryAllocate) {
    if (argumentTypes.size() >= 2) {
      requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
    }
    if (argumentTypes.size() >= 3) {
      requireStorageIndex(expr.arguments()[2], argumentTypes[2], intrinsic);
    }
    currentType = SemanticType::Int32;
  } else if (intrinsic == IntrinsicKind::StorageCapacity) {
    currentType = SemanticType::UInt64;
  } else if (intrinsic == IntrinsicKind::StorageInitialized) {
    if (argumentTypes.size() >= 2) {
      requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
    }
    currentType = SemanticType::Bool;
  } else if (intrinsic == IntrinsicKind::StorageConstruct) {
    if (argumentTypes.size() >= 2) {
      requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
    }
    if (elementType.kind == SemanticType::StringView ||
        elementType.kind == SemanticType::CString) {
      for (std::size_t index = 2; index < argumentTypes.size(); ++index) {
        if (isOwnerDependentTextExpression(expr.arguments()[index])) {
          reportOwnerDependentTextStorage(expr.arguments()[index],
                                          "nested in dynamic storage");
        }
      }
    }
    if (argumentTypes.size() >= 2) {
      std::vector<AnalyzedCallArgument> constructorArguments;
      bool concreteArguments = true;
      for (std::size_t index = 2; index < argumentTypes.size(); ++index) {
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
    }
    currentType = SemanticType::Void;
  } else if (intrinsic == IntrinsicKind::StorageRead ||
             intrinsic == IntrinsicKind::StorageReadMut) {
    if (argumentTypes.size() >= 2) {
      requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
    }
    currentType = elementType;
  } else if (intrinsic == IntrinsicKind::StorageDestroy) {
    if (argumentTypes.size() >= 2) {
      requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
    }
    currentType = SemanticType::Void;
  } else if (intrinsic == IntrinsicKind::StorageRelocate) {
    if (!satisfiesConstraint(elementType,
                             constraintBit(GenericConstraintKind::Movable))) {
      report(expr.paren(),
             "storage_relocate requires a movable element type, but '" +
                 typeSpelling(elementType) +
                 "' does not guarantee move construction.",
             "GTI-S2019");
    }
    if (argumentTypes.size() >= 2 &&
        argumentTypes[1] != SemanticType::Unknown) {
      if (argumentTypes[1] != storageType) {
        report(expressionToken(expr.arguments()[1]),
               "storage_relocate requires source and destination storage to "
               "have the same element type.",
               "GTI-S2019");
      } else {
        requireMutableStorage(expr.arguments()[1], "storage_relocate");
      }
    }
    if (argumentTypes.size() >= 3) {
      requireStorageIndex(expr.arguments()[2], argumentTypes[2], intrinsic);
    }
    currentType = SemanticType::Void;
  } else {
    if (!satisfiesConstraint(elementType,
                             constraintBit(GenericConstraintKind::Movable))) {
      report(expr.paren(),
             storageIntrinsicName(intrinsic) +
                 " requires a movable element type, but '" +
                 typeSpelling(elementType) +
                 "' does not guarantee move construction.",
             "GTI-S2019");
    }
    if (argumentTypes.size() >= 2) {
      requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
    }
    if (argumentTypes.size() >= 3) {
      requireStorageIndex(expr.arguments()[2], argumentTypes[2], intrinsic);
    }
    currentType = SemanticType::Void;
  }

  const bool borrowsStorage = intrinsic == IntrinsicKind::StorageRead ||
                              intrinsic == IntrinsicKind::StorageReadMut;
  semanticModel.record(
      expr,
      ResolvedCallInfo{
          .returnType =
              borrowsStorage
                  ? SemanticType::referenceTo(
                        elementType, intrinsic == IntrinsicKind::StorageReadMut
                                         ? AccessMode::Mutable
                                         : AccessMode::ReadOnly)
                  : currentType,
          .parameterTypes = std::move(argumentTypes),
          .typeArguments = {elementType},
          .intrinsic = intrinsic,
          .borrowOrigin = borrowsStorage ? BorrowOriginKind::Argument
                                         : BorrowOriginKind::None,
          .borrowArgument = 0,
          .borrowAccess = intrinsic == IntrinsicKind::StorageReadMut
                              ? AccessMode::Mutable
                              : AccessMode::ReadOnly});
}

auto SemanticVisitor::Impl::analyzeStorageAllocation(
    const Call &expr, const std::vector<SemanticType> &argumentTypes) -> void {
  if (expr.typeArguments().size() != 1) {
    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
    }
    report(expr.paren(),
           "gti_internal::allocate_storage<T> requires exactly one element "
           "type.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }

  const TypeRef &elementRef = expr.typeArguments().front();
  validateType(elementRef);
  validateReferencePlacement(elementRef, false, "storage element type");
  const SemanticType elementType = typeOf(elementRef);
  if (!isStorageElementType(elementType)) {
    report(elementRef.name.last(),
           "Compiler-private storage requires a concrete value element type.",
           "GTI-S2019");
  } else if (elementType.kind != SemanticType::TypeParameter &&
             typeTraits(elementType).containsBorrowedState) {
    report(elementRef.name.last(),
           "Compiler-private storage cannot contain a value that retains "
           "borrowed state until general owner dependencies are available.",
           "GTI-S2019");
  }
  if (argumentTypes.size() != 1) {
    reportStorageArity(expr, IntrinsicKind::AllocateStorage, 1);
  } else {
    requireStorageIndex(expr.arguments().front(), argumentTypes.front(),
                        IntrinsicKind::AllocateStorage);
  }

  currentType = SemanticType::storageOf(elementType);
  semanticModel.record(
      expr, ResolvedCallInfo{.returnType = currentType,
                             .parameterTypes = argumentTypes,
                             .typeArguments = {elementType},
                             .intrinsic = IntrinsicKind::AllocateStorage});
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
  case SemanticType::Storage:
  case SemanticType::PrefixStorage:
  case SemanticType::TypeName:
  case SemanticType::Function:
  case SemanticType::FunctionSignature:
  case SemanticType::NativeFunction:
  case SemanticType::Lambda:
  case SemanticType::Unexpected:
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

auto SemanticVisitor::Impl::requireStorageIndex(const ExprPtr &argument,
                                                const SemanticType &type,
                                                IntrinsicKind intrinsic)
    -> void {
  if (type != SemanticType::Unknown && type != SemanticType::UInt64) {
    report(expressionToken(argument),
           storageIntrinsicName(intrinsic) +
               " requires a uint64_t index or count.",
           "GTI-S2019");
  }
}

} // namespace lang
