#include "semantic_analyzer_impl.h"
#include "gti/lifetime_types.h"

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace lang {
namespace {
TypeVariance joinVariance(TypeVariance left, TypeVariance right) {
  if (left == TypeVariance::Bivariant)
    return right;
  if (right == TypeVariance::Bivariant || left == right)
    return left;
  return TypeVariance::Invariant;
}

TypeVariance composeVariance(TypeVariance outer, TypeVariance inner) {
  if (outer == TypeVariance::Bivariant || inner == TypeVariance::Bivariant)
    return TypeVariance::Bivariant;
  if (outer == TypeVariance::Invariant || inner == TypeVariance::Invariant)
    return TypeVariance::Invariant;
  return outer == inner ? TypeVariance::Covariant : TypeVariance::Contravariant;
}
} // namespace

auto SemanticVisitor::Impl::normalizeNominalStoredType(
    const TypeRef &syntax, const Token &owner,
    std::vector<GenericParameterInfo> &parameters,
    std::size_t *inheritedAnonymous) -> void {
  resolveTypeAliasDependencies(syntax, currentNamespace);
  SemanticType type = typeOf(syntax);
  if (type == SemanticType::Unknown)
    return; // Ordinary type validation owns unresolved declaration diagnostics.
  if (!normalizeCallableLifetimeType(type, syntax.name.last())) {
    report(syntax.name.last(),
           "A stored callable result requires an explicit lifetime when its inputs are ambiguous.",
           "GTI-S2127");
    return;
  }
  std::vector<LifetimeBinding> bindings;
  for (const Lifetime region : freeTypeLifetimes(type)) {
    if (region.kind != Lifetime::Kind::Inference)
      continue;
    if (inheritedAnonymous) {
      std::size_t index = 0;
      const auto inherited = std::find_if(parameters.begin(), parameters.end(),
          [&](const auto &parameter) {
            return parameter.implicit && index++ == *inheritedAnonymous;
          });
      ++*inheritedAnonymous;
      if (inherited != parameters.end()) {
        bindings.emplace_back(region, Lifetime::parameter(inherited->id));
        continue;
      }
    }
    Token name = owner;
    name.lexeme = "$lifetime" + std::to_string(
        std::count_if(parameters.begin(), parameters.end(),
                      [](const auto &parameter) { return parameter.implicit; }) + 1);
    name.generated = true;
    GenericParameterInfo parameter{.id = nextGenericParameterId++,
                                   .name = std::move(name),
                                   .lifetime = true,
                                   .implicit = true};
    semanticModel.recordGenericParameter(parameter);
    bindings.emplace_back(region, Lifetime::parameter(parameter.id));
    parameters.push_back(std::move(parameter));
  }
  if (!bindings.empty())
    type = substituteTypeLifetimes(type, bindings).value_or(SemanticType::Unknown);
  catalog.normalizedNominalTypes.insert_or_assign(&syntax, std::move(type));
}

auto SemanticVisitor::Impl::normalizeClassLifetimes(ClassId id) -> void {
  if (id == 0 || id > classes.size())
    return;
  ClassInfo &owner = classInfo(id);
  if (owner.lifetimeState == NominalLifetimeState::Resolved ||
      owner.lifetimeState == NominalLifetimeState::Invalid)
    return;
  const auto key = std::pair{false, std::uint64_t(id)};
  auto &stack = requestSession.nominalLifetimeStack;
  if (owner.lifetimeState == NominalLifetimeState::Resolving) {
    const auto begin = std::find(stack.begin(), stack.end(), key);
    for (auto entry = begin; entry != stack.end(); ++entry)
      if (entry->first)
        enums[entry->second - 1].recursiveLifetimeDependency = true;
      else
        classInfo(entry->second).recursiveLifetimeDependency = true;
    return;
  }
  owner.lifetimeState = NominalLifetimeState::Resolving;
  stack.push_back(key);
  const auto previousNamespace = currentNamespace;
  const auto previousSource = currentSourceUnit;
  const auto previousClass = currentClass;
  auto previousTypes = std::move(typeParameterScopes);
  auto previousPacks = std::move(typePackScopes);
  auto previousValues = std::move(valueParameterScopes);
  auto previousLifetimes = std::move(lifetimeParameterScopes);
  auto previousRequirements = std::move(requirementScopes);
  typeParameterScopes.clear();
  typePackScopes.clear();
  valueParameterScopes.clear();
  lifetimeParameterScopes.clear();
  requirementScopes.clear();
  currentNamespace = owner.namespaceScope;
  currentNamespace.push_back(owner.name.lexeme);
  currentSourceUnit = owner.sourceUnit;
  currentClass = id;
  std::vector<ClassId> enclosingClasses;
  for (ClassId enclosing = owner.enclosingClass; enclosing;
       enclosing = classInfo(enclosing).enclosingClass)
    enclosingClasses.push_back(enclosing);
  for (auto enclosing = enclosingClasses.rbegin(); enclosing != enclosingClasses.rend(); ++enclosing)
    beginTypeParameterScope(classInfo(*enclosing).genericParameters);
  beginTypeParameterScope(owner.genericParameters);
  std::size_t inheritedAnonymous = 0;
  auto *inherited = owner.exactSpecializationPrimary ? &inheritedAnonymous : nullptr;
  if (owner.declaration) {
    for (const auto &base : owner.declaration->bases())
      normalizeNominalStoredType(base.type, owner.name, owner.genericParameters, inherited);
    const auto fields = [&](const auto &self, const StmtList &members) -> void {
      for (const auto &member : members)
        if (const auto *group = stmtAs<MutableFieldGroupDecl>(member.get()))
          self(self, group->members());
        else if (const auto *conditional = stmtAs<ConditionalStmt>(member.get())) {
          if (const auto *active = conditional->activeBranch(target))
            self(self, *active);
        } else if (const auto *field = stmtAs<VariableDecl>(member.get());
                   field && !field->isStatic())
          normalizeNominalStoredType(field->type(), owner.name,
                                     owner.genericParameters, inherited);
    };
    fields(fields, owner.declaration->members());
  }
  endTypeParameterScope();
  for (std::size_t index = 0; index < enclosingClasses.size(); ++index)
    endTypeParameterScope();
  typeParameterScopes = std::move(previousTypes);
  typePackScopes = std::move(previousPacks);
  valueParameterScopes = std::move(previousValues);
  lifetimeParameterScopes = std::move(previousLifetimes);
  requirementScopes = std::move(previousRequirements);
  currentNamespace = previousNamespace;
  currentSourceUnit = previousSource;
  currentClass = previousClass;
  stack.pop_back();
  const bool anonymous = std::any_of(owner.genericParameters.begin(),
                                     owner.genericParameters.end(),
                                     [](const auto &parameter) { return parameter.implicit; });
  owner.lifetimeState = owner.recursiveLifetimeDependency && anonymous
                           ? NominalLifetimeState::Invalid
                           : NominalLifetimeState::Resolved;
  if (owner.exactSpecializationPrimary &&
      genericLifetimeParameterCount(owner.genericParameters) !=
          genericLifetimeParameterCount(classInfo(owner.exactSpecializationPrimary).genericParameters)) {
    owner.lifetimeState = NominalLifetimeState::Invalid;
    report(owner.name,
           "An exact specialization cannot add anonymous lifetime slots; declare a compatible lifetime parameter shape on the primary.",
           "GTI-S2126");
  }
  if (owner.recursiveLifetimeDependency && anonymous)
    report(owner.name,
           "A recursive borrowed type requires explicit lifetime parameters and arguments; anonymous field regions cannot have a finite declaration arity.",
           "GTI-S2126");
}

auto SemanticVisitor::Impl::normalizeEnumLifetimes(EnumId id) -> void {
  if (id == 0 || id > enums.size())
    return;
  auto &owner = enums[id - 1];
  if (owner.lifetimeState == NominalLifetimeState::Resolved ||
      owner.lifetimeState == NominalLifetimeState::Invalid)
    return;
  const auto key = std::pair{true, std::uint64_t(id)};
  auto &stack = requestSession.nominalLifetimeStack;
  if (owner.lifetimeState == NominalLifetimeState::Resolving) {
    const auto begin = std::find(stack.begin(), stack.end(), key);
    for (auto entry = begin; entry != stack.end(); ++entry)
      if (entry->first)
        enums[entry->second - 1].recursiveLifetimeDependency = true;
      else
        classInfo(entry->second).recursiveLifetimeDependency = true;
    return;
  }
  owner.lifetimeState = NominalLifetimeState::Resolving;
  stack.push_back(key);
  const auto previousNamespace = currentNamespace;
  const auto previousSource = currentSourceUnit;
  auto previousTypes = std::move(typeParameterScopes);
  auto previousPacks = std::move(typePackScopes);
  auto previousValues = std::move(valueParameterScopes);
  auto previousLifetimes = std::move(lifetimeParameterScopes);
  auto previousRequirements = std::move(requirementScopes);
  typeParameterScopes.clear();
  typePackScopes.clear();
  valueParameterScopes.clear();
  lifetimeParameterScopes.clear();
  requirementScopes.clear();
  currentNamespace = owner.namespaceScope;
  currentSourceUnit = owner.sourceUnit;
  beginTypeParameterScope(owner.genericParameters);
  if (owner.declaration)
    for (const auto &variant : owner.declaration->enumerators())
      for (const auto &field : variant.payload)
        normalizeNominalStoredType(field.type, owner.name, owner.genericParameters);
  endTypeParameterScope();
  typeParameterScopes = std::move(previousTypes);
  typePackScopes = std::move(previousPacks);
  valueParameterScopes = std::move(previousValues);
  lifetimeParameterScopes = std::move(previousLifetimes);
  requirementScopes = std::move(previousRequirements);
  currentNamespace = previousNamespace;
  currentSourceUnit = previousSource;
  stack.pop_back();
  const bool anonymous = std::any_of(owner.genericParameters.begin(),
                                     owner.genericParameters.end(),
                                     [](const auto &parameter) { return parameter.implicit; });
  owner.lifetimeState = owner.recursiveLifetimeDependency && anonymous
                           ? NominalLifetimeState::Invalid
                           : NominalLifetimeState::Resolved;
  if (owner.lifetimeState == NominalLifetimeState::Invalid)
    report(owner.name,
           "A recursive borrowed enum requires explicit lifetime parameters and arguments; anonymous payload regions cannot have a finite declaration arity.",
           "GTI-S2126");
}

auto SemanticVisitor::Impl::normalizeNominalLifetimes() -> void {
  // Make class-scoped alias identities available before a field asks for one.
  for (auto &alias : typeAliases)
    if (alias.ownerDeclaration && alias.ownerClass == 0)
      if (const auto owner = classDeclIds.find(alias.ownerDeclaration);
          owner != classDeclIds.end())
        alias.ownerClass = owner->second;
  const bool previous = deferringTypeAliasConstraints;
  deferringTypeAliasConstraints = true;
  for (ClassId id = registrationFloor.classes + 1; id <= classes.size(); ++id)
    normalizeClassLifetimes(id);
  for (EnumId id = registrationFloor.enums + 1; id <= enums.size(); ++id)
    normalizeEnumLifetimes(id);
  deferringTypeAliasConstraints = previous;
  assert(requestSession.nominalLifetimeStack.empty());
}

auto SemanticVisitor::Impl::nominalVariance(const SemanticType &type) const
    -> NominalVariance {
  const NominalVariance *descriptor = nullptr;
  const std::vector<GenericParameterInfo> *parameters = nullptr;
  if (type.kind == SemanticType::Class && type.classId && type.classId <= classes.size()) {
    descriptor = &classInfo(type.classId).variance;
    parameters = &classInfo(type.classId).genericParameters;
  } else if (type.kind == SemanticType::Enum && type.enumId && type.enumId <= enums.size()) {
    descriptor = &enums[type.enumId - 1].variance;
    parameters = &enums[type.enumId - 1].genericParameters;
  }
  if (!descriptor || !descriptor->complete)
    return {};
  NominalVariance result = *descriptor;
  const bool pack = std::any_of(parameters->begin(), parameters->end(),
                                [](const auto &parameter) { return parameter.pack; });
  if (pack && !result.types.empty() && type.arguments.size() + 1 >= result.types.size()) {
    const auto tail = result.types.back();
    result.types.resize(type.arguments.size(), tail);
  }
  if (result.types.size() != type.arguments.size() ||
      result.lifetimes.size() != type.lifetimeArguments.size())
    return {};
  return result;
}

auto SemanticVisitor::Impl::nominalLifetimeContract(const SemanticType &type) const
    -> LifetimeSignatureContract {
  LifetimeSignatureContract result;
  GenericSubstitution substitution;
  if (type.kind == SemanticType::Class && type.classId && type.classId <= classes.size()) {
    result = classInfo(type.classId).lifetimeContract;
    substitution = classSubstitution(type);
  } else if (type.kind == SemanticType::Enum && type.enumId && type.enumId <= enums.size()) {
    result = enums[type.enumId - 1].lifetimeContract;
    substitution = enumSubstitution(type);
  } else {
    return result;
  }
  substitution.lifetimes.clear();
  for (auto &bound : result.typeBounds)
    bound.type = substituteType(bound.type, substitution);
  return result;
}

auto SemanticVisitor::Impl::inferNominalVariances() -> void {
  const auto initialize = [&](auto &owner) {
    owner.variance = {.types = std::vector<TypeVariance>(
                          genericTypeParameterCount(owner.genericParameters), TypeVariance::Bivariant),
                      .lifetimes = std::vector<TypeVariance>(
                          genericLifetimeParameterCount(owner.genericParameters), TypeVariance::Bivariant),
                      .complete = true};
  };
  for (auto &owner : classes)
    initialize(owner);
  for (auto &owner : enums)
    initialize(owner);

  // Each slot only ascends the four-element variance lattice. This fixed point
  // handles mutually recursive explicit nominal types without unfolding them.
  bool changed = true;
  while (changed) {
    changed = false;
    const auto infer = [&](auto &owner, const std::vector<SemanticType> &stored,
                           bool opaque, bool ownedCapability) {
      auto next = owner.variance;
      std::unordered_map<GenericParameterId, TypeVariance *> slots;
      std::size_t typeIndex = 0, lifetimeIndex = 0;
      for (const auto &parameter : owner.genericParameters)
        if (parameter.lifetime)
          slots.emplace(parameter.id, &next.lifetimes[lifetimeIndex++]);
        else if (!parameter.value)
          slots.emplace(parameter.id, &next.types[typeIndex++]);
      const auto observe = [&](GenericParameterId id, TypeVariance position) {
        if (const auto slot = slots.find(id); slot != slots.end())
          *slot->second = joinVariance(*slot->second, position);
      };
      if (opaque || ownedCapability)
        for (const auto &[id, unused] : slots) {
          (void)unused;
          observe(id, opaque ? TypeVariance::Invariant : TypeVariance::Covariant);
        }
      const auto visit = [&](const auto &self, const SemanticType &type,
                              TypeVariance position) -> void {
        if (position == TypeVariance::Bivariant)
          return;
        if (type.kind == SemanticType::TypeParameter || type.kind == SemanticType::TypePack)
          observe(type.genericParameterId, position);
        if (type.kind == SemanticType::Reference &&
            type.referenceLifetime.kind == Lifetime::Kind::Parameter)
          observe(type.referenceLifetime.id, position);
        const auto nominal = nominalVariance(type);
        for (std::size_t index = 0; index < type.lifetimeArguments.size(); ++index) {
          const auto region = type.lifetimeArguments[index];
          if (region.kind == Lifetime::Kind::Parameter)
            observe(region.id, composeVariance(position,
                nominal.complete ? nominal.lifetimes[index]
                : type.kind == SemanticType::StringView ? TypeVariance::Covariant
                                                       : TypeVariance::Invariant));
        }
        for (std::size_t index = 0; index < type.arguments.size(); ++index) {
          TypeVariance nested = TypeVariance::Covariant;
          if (nominal.complete)
            nested = nominal.types[index];
          else if (type.kind == SemanticType::Class || type.kind == SemanticType::Enum ||
                   type.kind == SemanticType::RawPointer ||
                   (type.kind == SemanticType::Reference &&
                    type.referenceAccess == AccessMode::Mutable))
            nested = TypeVariance::Invariant;
          else if (type.kind == SemanticType::FunctionSignature ||
                   type.kind == SemanticType::NativeFunction)
            nested = index == 0 ? TypeVariance::Covariant : TypeVariance::Contravariant;
          self(self, type.arguments[index], composeVariance(position, nested));
        }
      };
      for (const auto &type : stored)
        visit(visit, type, TypeVariance::Covariant);
      if (next != owner.variance) {
        owner.variance = std::move(next);
        changed = true;
      }
    };
    for (auto &owner : classes) {
      std::vector<SemanticType> fields;
      for (const auto &base : owner.bases)
        fields.push_back(base.type);
      for (const auto &field : owner.fields)
        if (const auto member = owner.members.find(field.declaration->name().lexeme);
            member != owner.members.end())
          fields.push_back(member->second.symbol.type);
      const auto capability = owner.compilerCapability;
      const bool ownedCapability = capability == CompilerCapabilityTypeKind::UniqueOwner ||
          capability == CompilerCapabilityTypeKind::SharedOwner ||
          capability == CompilerCapabilityTypeKind::OptionalStorage ||
          capability == CompilerCapabilityTypeKind::FunctionPointer ||
          capability == CompilerCapabilityTypeKind::TextView;
      const bool opaque = owner.cOpaqueHandle || owner.polymorphic ||
          owner.kind == ClassKind::Interface || !owner.declaration ||
          owner.declaration->isForwardDeclaration() ||
          owner.lifetimeState == NominalLifetimeState::Invalid ||
          (capability != CompilerCapabilityTypeKind::None && !ownedCapability);
      infer(owner, fields, opaque, ownedCapability);
    }
    for (auto &owner : enums) {
      std::vector<SemanticType> fields;
      for (const auto &[name, variant] : owner.enumerators) {
        (void)name;
        fields.insert(fields.end(), variant.payloadTypes.begin(), variant.payloadTypes.end());
      }
      infer(owner, fields, owner.lifetimeState == NominalLifetimeState::Invalid, false);
    }
    // An open primary must not promise covariance that one exact definition
    // violates. Selection changes storage, not the family's region contract.
    for (auto &owner : classes) {
      if (!owner.exactSpecializationPrimary)
        continue;
      auto &primary = classInfo(owner.exactSpecializationPrimary);
      if (owner.variance.lifetimes.size() != primary.variance.lifetimes.size())
        continue;
      for (std::size_t index = 0; index < owner.variance.lifetimes.size(); ++index) {
        const auto combined = joinVariance(owner.variance.lifetimes[index],
                                            primary.variance.lifetimes[index]);
        changed = changed || combined != owner.variance.lifetimes[index] ||
                              combined != primary.variance.lifetimes[index];
        owner.variance.lifetimes[index] = combined;
        primary.variance.lifetimes[index] = combined;
      }
    }
  }
  const auto contractFor = [&](auto &owner, std::vector<SemanticType> fields) {
    LifetimeSignatureContract seed;
    for (const auto &parameter : owner.genericParameters)
      if (parameter.lifetime) {
        seed.parameters.push_back(Lifetime::parameter(parameter.id));
        if (!parameter.implicit)
          seed.origin = LifetimeContractOrigin::Declared;
      }
    SemanticType result = SemanticType::Void;
    // Nominal declarations have already established their complete arity.
    // Reuse callable WF collection, but never invent another region here.
    auto normalized = normalizeLifetimeSignature(result, fields, std::move(seed),
                                                   {}, [] { return Lifetime{}; });
    owner.lifetimeContract = std::move(normalized.contract);
    if (owner.lifetimeState == NominalLifetimeState::Invalid)
      owner.lifetimeContract.complete = false;
    else if (!owner.lifetimeContract.valid())
      report(owner.name,
             "Stored borrowed types require a finite complete lifetime contract; name unresolved lifetime slots explicitly.",
             "GTI-S2126");
  };
  for (auto &owner : classes) {
    std::vector<SemanticType> fields;
    for (const auto &base : owner.bases)
      fields.push_back(base.type);
    for (const auto &field : owner.fields)
      if (const auto member = owner.members.find(field.declaration->name().lexeme);
          member != owner.members.end())
        fields.push_back(member->second.symbol.type);
    contractFor(owner, std::move(fields));
  }
  for (auto &owner : enums) {
    std::vector<SemanticType> fields;
    for (const auto &[name, variant] : owner.enumerators) {
      (void)name;
      fields.insert(fields.end(), variant.payloadTypes.begin(), variant.payloadTypes.end());
    }
    contractFor(owner, std::move(fields));
  }
  for (auto &owner : classes) {
    if (!owner.exactSpecializationPrimary || !owner.lifetimeContract.valid())
      continue;
    const auto &primary = classInfo(owner.exactSpecializationPrimary);
    GenericSubstitution substitution = nominalSubstitution(
        primary.genericParameters,
        {.types = owner.exactTypeArguments, .values = owner.exactValueArguments});
    auto inherited = primary.lifetimeContract;
    for (auto &bound : inherited.typeBounds)
      bound.type = substituteType(bound.type, substitution);
    std::vector<AppliedConceptRequirement> assumptions;
    for (const auto bound : inherited.bounds)
      assumptions.push_back({.lifetimeBound = bound});
    for (const auto &bound : inherited.typeBounds)
      assumptions.push_back({.lifetimeBound = LifetimeBound{{}, bound.region},
                             .outlivesType = bound.type});
    requirementScopes.push_back(std::move(assumptions));
    bool compatible = inherited.valid();
    for (const auto bound : owner.lifetimeContract.bounds)
      compatible = !lifetimeRequirementFailure({.lifetimeBound = bound}) && compatible;
    for (const auto &bound : owner.lifetimeContract.typeBounds)
      compatible = !lifetimeRequirementFailure(
          {.lifetimeBound = LifetimeBound{{}, bound.region}, .outlivesType = bound.type}) && compatible;
    requirementScopes.pop_back();
    if (!compatible) {
      owner.lifetimeContract.complete = false;
      report(owner.name,
             "An exact specialization cannot strengthen the primary's lifetime formation obligations.",
             "GTI-S2127");
      continue;
    }
    for (const auto bound : inherited.bounds)
      if (std::find(owner.lifetimeContract.bounds.begin(), owner.lifetimeContract.bounds.end(), bound) ==
          owner.lifetimeContract.bounds.end())
        owner.lifetimeContract.bounds.push_back(bound);
    for (const auto &bound : inherited.typeBounds)
      if (std::find(owner.lifetimeContract.typeBounds.begin(), owner.lifetimeContract.typeBounds.end(), bound) ==
          owner.lifetimeContract.typeBounds.end())
        owner.lifetimeContract.typeBounds.push_back(bound);
  }
  for (const auto &owner : enums)
    if (const auto *record = semanticModel.findEnumType(owner.id)) {
      auto updated = *record;
      updated.variance = owner.variance;
      updated.lifetimeContract = owner.lifetimeContract;
      semanticModel.recordEnumType(*owner.declaration, std::move(updated));
    }
}
} // namespace lang
