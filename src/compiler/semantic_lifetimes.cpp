#include "semantic_analyzer_impl.h"
#include "gti/lifetime_types.h"
#include "gti/lifetime_requirements.h"
#include "gti/callable_lifetimes.h"
#include <array>

namespace lang {
namespace {
template <typename T> void appendUnique(std::vector<T> &values, const T &value) {
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}

struct LifetimeAssumptions {
  std::vector<LifetimeBound> regions;
  std::vector<LifetimeTypeBound> types;
  bool impossible = false;
};

LifetimeAssumptions guaranteedLifetimeAssumptions(
    const LifetimeRequirementFormula &formula) {
  using Kind = LifetimeRequirementNode::Kind;
  std::vector<LifetimeAssumptions> facts;
  for (const auto &node : formula.nodes) {
    LifetimeAssumptions result;
    if (node.kind == Kind::Region) {
      result.regions.push_back(node.bound);
    } else if (node.kind == Kind::Type) {
      result.types.push_back({node.type, node.bound.shorter});
    } else if (node.kind == Kind::SemanticPredicate) {
      result.impossible = node.semanticTruth && !*node.semanticTruth;
    } else if (node.left < facts.size() && node.right < facts.size()) {
      const auto &left = facts[node.left];
      const auto &right = facts[node.right];
      if (node.kind == Kind::All) {
        result = left;
        result.impossible = left.impossible || right.impossible;
        for (const auto edge : right.regions)
          appendUnique(result.regions, edge);
        for (const auto &bound : right.types)
          appendUnique(result.types, bound);
      } else if (left.impossible || right.impossible) {
        result = left.impossible ? right : left;
      } else {
        LifetimeBounds leftBounds, rightBounds;
        for (const auto edge : left.regions)
          (void)leftBounds.assume(edge.longer, edge.shorter);
        for (const auto edge : right.regions)
          (void)rightBounds.assume(edge.longer, edge.shorter);
        auto candidates = left.regions;
        for (const auto edge : right.regions)
          appendUnique(candidates, edge);
        for (const auto edge : candidates)
          if (leftBounds.proves(edge.longer, edge.shorter) &&
              rightBounds.proves(edge.longer, edge.shorter))
            appendUnique(result.regions, edge);
        for (const auto &bound : left.types)
          if (std::any_of(right.types.begin(), right.types.end(), [&](const auto &other) {
                return equivalentLifetimeTypes(bound.type, other.type) &&
                       rightBounds.proves(other.region, bound.region);
              }))
            appendUnique(result.types, bound);
      }
    } else {
      return {};
    }
    facts.push_back(std::move(result));
  }
  return formula.root < facts.size() ? facts[formula.root] : LifetimeAssumptions{};
}

// A conjunction guarantees both sides. A disjunction guarantees only facts
// shared by both sides; treating every written bound as an assumption would
// make `outlives<a,b> || Other<T>` unsound in the function body.
LifetimeAssumptions guaranteedLifetimeAssumptions(
    std::span<const AppliedConceptRequirement> requirements) {
  LifetimeAssumptions result;
  std::vector<const ConstraintExpression *> expressions;
  const auto facts = [](const AppliedConceptRequirement &requirement) {
    LifetimeAssumptions result;
    if (requirement.lifetimeBound) {
      if (requirement.outlivesType)
        result.types.push_back({*requirement.outlivesType,
                                requirement.lifetimeBound->shorter});
      else
        result.regions.push_back(*requirement.lifetimeBound);
    }
    return result;
  };
  const auto merge = [](LifetimeAssumptions &target,
                        const LifetimeAssumptions &source) {
    for (const auto bound : source.regions)
      appendUnique(target.regions, bound);
    for (const auto &bound : source.types)
      appendUnique(target.types, bound);
  };
  for (const auto &requirement : requirements) {
    if (!requirement.expression)
      merge(result, facts(requirement));
    else
      appendUnique(expressions, requirement.expression);
  }
  for (const auto *expression : expressions) {
    if (expression->root >= expression->nodes.size())
      continue;
    std::vector<LifetimeAssumptions> nodes;
    nodes.reserve(expression->nodes.size());
    bool valid = true;
    for (const auto &node : expression->nodes) {
      LifetimeAssumptions current;
      if (node.kind == ConstraintExpressionKind::Application) {
        for (const auto &requirement : requirements)
          if (requirement.expression == expression &&
              requirement.expressionApplication == node.application)
            merge(current, facts(requirement));
      } else if (node.left < nodes.size() && node.right < nodes.size()) {
        current = nodes[node.left];
        const auto &right = nodes[node.right];
        if (node.kind == ConstraintExpressionKind::Conjunction) {
          merge(current, right);
        } else if (node.kind == ConstraintExpressionKind::Disjunction) {
          std::erase_if(current.regions, [&](const auto bound) {
            return std::find(right.regions.begin(), right.regions.end(), bound) ==
                   right.regions.end();
          });
          std::erase_if(current.types, [&](const auto &bound) {
            return std::find(right.types.begin(), right.types.end(), bound) ==
                   right.types.end();
          });
        } else {
          valid = false;
          break;
        }
      } else {
        valid = false;
        break;
      }
      nodes.push_back(std::move(current));
    }
    if (valid)
      merge(result, nodes[expression->root]);
  }
  return result;
}
} // namespace

auto SemanticVisitor::Impl::implicitFunctionLifetime(
    const FunctionDecl &function, std::size_t slot, std::string_view name)
    -> Lifetime {
  auto *declaration = catalog.mutableCallable(function);
  // Concept operations and callable contracts share the normalizer without
  // becoming executable catalog declarations. Their syntax-keyed identities
  // follow the same request/instance rollback discipline as HRTB binders.
  auto &parameters = declaration
                         ? declaration->implicitLifetimeParameters
                         : lifetimeBinders[lifetimeSyntaxKey(
                               function.name(), std::numeric_limits<std::size_t>::max())];
  if (parameters.size() <= slot)
    parameters.resize(slot + 1);
  auto &parameter = parameters[slot];
  if (parameter.id == 0) {
    Token token = function.name();
    token.kind = TokenKind::IDENTIFIER;
    token.lexeme = std::string(name);
    token.generated = true;
    parameter = {.id = nextGenericParameterId++,
                 .name = std::move(token),
                 .lifetime = true};
  }
  semanticModel.recordGenericParameter(parameter);
  return Lifetime::parameter(parameter.id);
}

auto SemanticVisitor::Impl::prepareFunctionLifetimeContract(
    const FunctionDecl &function, ClassId ownerClass) -> void {
  currentReceiverLifetime = {};
  if (auto *declaration = catalog.mutableCallable(function)) {
    declaration->ownerClass = ownerClass;
    if (ownerClass != 0 && !function.isStatic()) {
      currentReceiverLifetime = implicitFunctionLifetime(function, 0, "this");
      declaration->lifetimeContract.receiver = currentReceiverLifetime;
    }
  }
}

auto SemanticVisitor::Impl::normalizeFunctionLifetimeContract(
    CallableCandidate &function) -> void {
  if (!function.declaration || !function.declarationRecord)
    return;
  const auto &syntax = *function.declaration;
  LifetimeSignatureContract contract;
  contract.receiver = function.record().lifetimeContract.receiver;
  std::size_t nextSlot = 1; // slot zero is the implicit receiver, when present.
  const auto fresh = [&] {
    const auto slot = nextSlot++;
    return implicitFunctionLifetime(syntax, slot,
                                    "_input" + std::to_string(slot));
  };
  const auto addParameter = [&](Lifetime region) {
    if (region.kind == Lifetime::Kind::Parameter && region.specified())
      appendUnique(contract.parameters, region);
  };
  addParameter(contract.receiver);
  if (function.ownerClass != 0) {
    const auto &owner = classInfo(function.ownerClass);
    contract.bounds = owner.lifetimeContract.bounds;
    contract.typeBounds = owner.lifetimeContract.typeBounds;
    for (const auto &parameter : owner.genericParameters)
      if (parameter.lifetime)
        addParameter(Lifetime::parameter(parameter.id));
    for (const auto &parameter : owner.genericArgumentPatternLifetimes)
      addParameter(Lifetime::parameter(parameter.id));
  }
  for (const auto &parameter : function.record().genericParameters)
    if (parameter.lifetime) {
      addParameter(Lifetime::parameter(parameter.id));
      contract.origin = LifetimeContractOrigin::Declared;
    }
  std::optional<std::size_t> borrowedParameter;
  bool borrowedReceiver = false;
  if (const auto *attribute = syntax.findFunctionAttribute("borrowed_from");
      attribute && attribute->argument) {
    contract.origin = LifetimeContractOrigin::Declared;
    borrowedReceiver = attribute->argument->kind == TokenKind::THIS;
    for (std::size_t index = 0; index < syntax.parameters().size(); ++index)
      if (syntax.parameters()[index].name.lexeme == attribute->argument->lexeme)
        borrowedParameter = index;
  }
  if (privateLifetimeInferenceEligible(function)) {
    SemanticType noResult = SemanticType::Void;
    auto inputs = normalizeLifetimeSignature(noResult, function.parameterTypes,
                                              std::move(contract), function.requirements, fresh);
    contract = std::move(inputs.contract);
    prepareInferredLifetimeResult(function.returnType, fresh);
    contract.origin = LifetimeContractOrigin::BodyInferred;
  }
  auto normalized = normalizeLifetimeSignature(
      function.returnType, function.parameterTypes, std::move(contract),
      function.requirements, fresh, borrowedParameter, borrowedReceiver);
  if (normalized.contract.receiver.specified() && function.ownerClass)
    appendUnique(normalized.contract.typeBounds,
                 LifetimeTypeBound{openClassType(function.ownerClass),
                                   normalized.contract.receiver});
  if (normalized.ambiguousResult) {
    auto diagnostic = makeDiagnostic(
        "GTI-S2127", DiagnosticPhase::Semantics, syntax.returnType().name.last(),
        "The returned borrow has no unambiguous input lifetime; name its "
        "relationship with a lifetime parameter.");
    for (std::size_t index = 0; index < syntax.parameters().size() &&
                                index < function.parameterTypes.size(); ++index)
      if (!freeTypeLifetimes(function.parameterTypes[index]).empty())
        diagnostic.related.push_back({tokenSpan(syntax.parameters()[index].sourceAnchor()),
                                      "This input contributes a lifetime."});
    diagnostic.hints.emplace_back(
        "Declare 'lifetime a' and use '&<a>' or '<a>' on the input and result "
        "that share the borrow. Use 'static' only for storage proved to be static.");
    diagnostics.push_back(std::move(diagnostic));
  }
  function.lifetimeContract = std::move(normalized.contract);
}

auto SemanticVisitor::Impl::implicitSyntaxLifetime(
    const Token &site, std::size_t slot, std::string_view name,
    std::size_t identity) const -> Lifetime {
  auto &parameters = lifetimeBinders[lifetimeSyntaxKey(site, identity)];
  if (parameters.size() <= slot)
    parameters.resize(slot + 1);
  auto &parameter = parameters[slot];
  if (parameter.id == 0) {
    Token token = site;
    token.kind = TokenKind::IDENTIFIER;
    token.lexeme = std::string(name);
    token.generated = true;
    parameter = {.id = nextGenericParameterId++, .name = std::move(token),
                 .lifetime = true};
  }
  semanticModel.recordGenericParameter(parameter);
  return Lifetime::parameter(parameter.id);
}

auto SemanticVisitor::Impl::normalizeConstructorLifetimeContract(
    ConstructorInfo &constructor) -> void {
  const auto &owner = classInfo(constructor.owner);
  const Token site = constructor.declaration ? constructor.declaration->name()
                                           : owner.name;
  LifetimeSignatureContract contract = owner.lifetimeContract;
  for (const auto &parameter : owner.genericArgumentPatternLifetimes)
    appendUnique(contract.parameters, Lifetime::parameter(parameter.id));
  for (const auto *parameters : std::array<const std::vector<GenericParameterInfo> *, 2>{
                                   &owner.genericParameters,
                                   &constructor.genericParameters})
    for (const auto &parameter : *parameters)
      if (parameter.lifetime) {
        appendUnique(contract.parameters, Lifetime::parameter(parameter.id));
        contract.origin = LifetimeContractOrigin::Declared;
      }
  std::size_t slot = 0;
  const auto fresh = [&] {
    ++slot;
    return implicitSyntaxLifetime(site, slot, "_input" + std::to_string(slot));
  };
  SemanticType result = openClassType(constructor.owner);
  auto normalized = normalizeLifetimeSignature(
      result, constructor.parameterTypes, std::move(contract),
      constructor.requirements, fresh);
  constructor.lifetimeContract = std::move(normalized.contract);
  if (normalized.ambiguousResult)
    report(site, "A constructed borrowed type needs a declared relationship "
                 "between its stored lifetimes and constructor inputs.",
           "GTI-S2127");
}

auto SemanticVisitor::Impl::addConstructorInitializerLifetimeBounds(
    ConstructorInfo &constructor) -> void {
  const auto &owner = classInfo(constructor.owner);
  const auto result = openClassType(constructor.owner);
  // Anonymous stored regions are declaration-derived output slots. Their
  // initializer list can connect them to inputs without making callers name
  // those hidden slots. This is a serialized constructor contract, not an
  // inference from arbitrary statements in the constructor body. Explicit
  // owner regions keep their written input/bound requirements.
  const auto anonymousOwnerRegion = [&](Lifetime region) {
    return region.kind == Lifetime::Kind::Parameter &&
           std::any_of(owner.genericParameters.begin(), owner.genericParameters.end(),
               [&](const auto &parameter) {
                 return parameter.lifetime && parameter.implicit &&
                        parameter.id == region.id;
               });
  };
  if (constructor.declaration &&
      std::any_of(owner.genericParameters.begin(), owner.genericParameters.end(),
                  [](const auto &parameter) {
                    return parameter.lifetime && parameter.implicit;
                  })) {
    const auto bindings = storedCarrierConstructorBindings(
        result, constructor.declaration, constructor.parameterTypes);
    for (const auto &binding : bindings) {
      if (binding.parameter >= constructor.parameterTypes.size())
        continue;
      SemanticType source = constructor.parameterTypes[binding.parameter];
      if (!binding.sourceFields.empty()) {
        const auto sources = storedReferencePaths(source);
        const auto selected = std::find_if(sources.begin(), sources.end(),
            [&](const auto &path) { return path.fields == binding.sourceFields; });
        if (selected == sources.end())
          continue;
        source = selected->reference;
      }
      std::vector<LifetimeBound> edges;
      const auto adjusted = constrainLifetimeType(
          source, binding.destination.reference, 1, edges);
      if (!adjusted ||
          eraseTypeLifetimes(*adjusted) !=
              eraseTypeLifetimes(binding.destination.reference))
        continue;
      for (const auto edge : edges)
        if (anonymousOwnerRegion(edge.longer) || anonymousOwnerRegion(edge.shorter))
          appendUnique(constructor.lifetimeContract.bounds, edge);
    }
  }
}

auto SemanticVisitor::Impl::finalizeConstructorInitializerLifetimes() -> void {
  struct RestoreContext {
    Impl &analyzer;
    std::optional<ClassId> owner;
    SourceUnitId source;
    std::vector<std::string> nameSpace;
    ~RestoreContext() {
      analyzer.currentClass = owner;
      analyzer.currentSourceUnit = source;
      analyzer.currentNamespace = std::move(nameSpace);
    }
  } restore{*this, currentClass, currentSourceUnit, currentNamespace};
  // Signature normalization has already allocated every declaration binder.
  // Add only initializer-derived bounds after all exact field identities have
  // been reserved; rebuilding signatures here could mint or lose input slots.
  for (std::size_t index = registrationFloor.classes; index < classes.size(); ++index) {
    auto &owner = classInfo(index + 1);
    currentClass = owner.id;
    currentSourceUnit = sourceUnitFor(owner.name);
    currentNamespace = owner.namespaceScope;
    beginClassParameterScope(owner);
    const auto finish = [&](ConstructorInfo &constructor) {
      beginTypeParameterScope(constructor.genericParameters);
      addConstructorInitializerLifetimeBounds(constructor);
      endTypeParameterScope();
    };
    for (auto &constructor : owner.constructors)
      finish(constructor);
    for (auto *constructor : {&owner.copyConstructor, &owner.moveConstructor,
                              &owner.designatedConstructor, &owner.reversedDesignatedConstructor})
      if (*constructor)
        finish(**constructor);
    endTypeParameterScope();
  }
}

auto SemanticVisitor::Impl::normalizeDestructorLifetimeContract(
    DestructorInfo &destructor) -> void {
  const auto &owner = classInfo(destructor.owner);
  const Token site = destructor.declaration ? destructor.declaration->tilde()
                                          : owner.name;
  LifetimeSignatureContract contract = owner.lifetimeContract;
  for (const auto &parameter : owner.genericArgumentPatternLifetimes)
    appendUnique(contract.parameters, Lifetime::parameter(parameter.id));
  contract.receiver = implicitSyntaxLifetime(site, 0, "this");
  contract.parameters.push_back(contract.receiver);
  for (const auto &parameter : owner.genericParameters)
    if (parameter.lifetime)
      appendUnique(contract.parameters, Lifetime::parameter(parameter.id));
  contract.typeBounds.push_back({openClassType(owner.id), contract.receiver});
  contract.complete = true;
  destructor.lifetimeContract = std::move(contract);
}

auto SemanticVisitor::Impl::dependentCallablePattern(const SemanticType &type) const -> bool {
  if (!isLifetimeInvocationSignature(type) || !currentClass)
    return false;
  const auto &owner = classInfo(*currentClass);
  if (!owner.declaration || owner.declaration->genericArgumentPatterns().empty())
    return false;
  const auto depends = [&](auto &&self, const SemanticType &part) -> bool {
    if ((part.kind == SemanticType::TypeParameter ||
         (part.kind == SemanticType::TypePack && !part.concretePack)) &&
        std::any_of(owner.genericParameters.begin(), owner.genericParameters.end(),
                    [&](const auto &parameter) { return parameter.id == part.genericParameterId; }))
      return true;
    bool found = false;
    lifetime_detail::children(part, [&](const auto &child) { found |= self(self, child); });
    return found;
  };
  return depends(depends, type);
}

auto SemanticVisitor::Impl::normalizeCallableLifetimeType(
    SemanticType &type, const Token &site) const -> bool {
  std::size_t slot = 0;
  const auto fresh = [&] {
    ++slot;
    return implicitSyntaxLifetime(site, slot, "_input" + std::to_string(slot));
  };
  const auto normalize = [&](auto &&self, SemanticType &current) -> bool {
    if (isLifetimeInvocationSignature(current)) {
      if (dependentCallablePattern(current))
        return true; // Its complete invocation is derived from actual pattern arguments.
      SemanticType unused = SemanticType::Void;
      std::vector<SemanticType> parameters{current};
      auto result = normalizeLifetimeSignature(unused, parameters, {}, {}, fresh);
      current = std::move(parameters.front());
      return result.contract.valid();
    }
    bool valid = true;
    lifetime_detail::children(current, [&](SemanticType &child) {
      valid = self(self, child) && valid;
    });
    return valid;
  };
  return normalize(normalize, type);
}

auto SemanticVisitor::Impl::declarationTypeLifetimes(const SemanticType &type) const
    -> std::vector<Lifetime> {
  std::vector<Lifetime> result;
  const auto visit = [&](auto &&self, const SemanticType &part,
                         std::vector<GenericParameterId> bound) -> void {
    if (dependentCallablePattern(part))
      return;
    bound.insert(bound.end(), part.quantifiedLifetimes.begin(), part.quantifiedLifetimes.end());
    const auto add = [&](Lifetime region) {
      if (region.specified() && (region.kind != Lifetime::Kind::Parameter ||
          std::find(bound.begin(), bound.end(), region.id) == bound.end()))
        appendUnique(result, region);
    };
    add(part.referenceLifetime);
    for (const auto region : part.lifetimeArguments)
      add(region);
    lifetime_detail::children(part, [&](const auto &child) { self(self, child, bound); });
  };
  visit(visit, type, {});
  return result;
}

auto SemanticVisitor::Impl::normalizeLifetimeSignature(
    SemanticType &returnType, std::vector<SemanticType> &parameterTypes,
    LifetimeSignatureContract contract,
    std::span<const AppliedConceptRequirement> requirements,
    const std::function<Lifetime()> &fresh,
    std::optional<std::size_t> borrowedParameter, bool borrowedReceiver) const
    -> LifetimeSignatureNormalization {
  bool valid = true;
  bool ambiguousResult = false;
  const auto isHole = [](Lifetime region) {
    return region.kind == Lifetime::Kind::Unspecified ||
           region.kind == Lifetime::Kind::Inference;
  };
  const auto addParameter = [&](Lifetime region) {
    if (region.kind == Lifetime::Kind::Parameter && region.specified())
      appendUnique(contract.parameters, region);
  };
  addParameter(contract.receiver);

  // A callable nested in a signature has its own elision scope. Its inferred
  // input regions become higher-ranked binders, never outer call inputs.
  const auto normalize = [&](auto &&self, SemanticType &type, bool input,
                             Lifetime resultRegion,
                             std::vector<GenericParameterId> *quantified) -> void {
    if (isLifetimeInvocationSignature(type) ||
        type.kind == SemanticType::FunctionItem) {
      if (dependentCallablePattern(type))
        return;
      if (type.arguments.empty()) {
        valid = false;
        return;
      }
      std::vector<Lifetime> inputs;
      for (std::size_t index = 1; index < type.arguments.size(); ++index) {
        self(self, type.arguments[index], true, {}, &type.quantifiedLifetimes);
        for (const auto region : declarationTypeLifetimes(type.arguments[index]))
          appendUnique(inputs, region);
      }
      self(self, type.arguments.front(), false,
           inputs.size() == 1 ? inputs.front() : Lifetime{},
           &type.quantifiedLifetimes);
      return;
    }
    const auto resolve = [&](Lifetime &region) {
      if (!isHole(region)) {
        if (!region.declarationRegion())
          valid = false;
        return;
      }
      if (input) {
        region = fresh();
        if (quantified)
          quantified->push_back(region.id);
      } else if (resultRegion.specified()) {
        region = resultRegion;
      } else {
        valid = false;
        ambiguousResult = true;
      }
    };
    if (type.kind == SemanticType::Reference)
      resolve(type.referenceLifetime);
    if ((type.kind == SemanticType::StringView || type.kind == SemanticType::CString) &&
        type.lifetimeArguments.empty())
      type.lifetimeArguments.emplace_back();
    for (auto &region : type.lifetimeArguments)
      resolve(region);
    lifetime_detail::children(type, [&](SemanticType &child) {
      self(self, child, input, resultRegion, quantified);
    });
  };
  const auto freeRegions = [&](const SemanticType &type) {
    return declarationTypeLifetimes(type);
  };
  std::vector<Lifetime> inputRegions;
  for (auto &parameter : parameterTypes) {
    normalize(normalize, parameter, true, {}, nullptr);
    for (const auto region : freeRegions(parameter)) {
      appendUnique(inputRegions, region);
      addParameter(region);
    }
  }
  Lifetime resultRegion = contract.receiver;
  if (!resultRegion.specified() && inputRegions.size() == 1)
    resultRegion = inputRegions.front();
  // Translate the existing one-origin source spelling into the same region
  // interface; it is never a second solver or permission to ignore a bound.
  if (borrowedReceiver) {
    resultRegion = contract.receiver;
  } else if (borrowedParameter && *borrowedParameter < parameterTypes.size()) {
    const auto regions = freeRegions(parameterTypes[*borrowedParameter]);
    if (regions.size() == 1) {
      resultRegion = regions.front();
    } else if (!regions.empty()) {
      resultRegion = fresh();
      addParameter(resultRegion);
      for (const auto region : regions)
        appendUnique(contract.bounds, LifetimeBound{region, resultRegion});
    }
  }
  normalize(normalize, returnType, false, resultRegion, nullptr);
  for (const auto region : freeRegions(returnType))
    addParameter(region);
  const auto assumptions = guaranteedLifetimeAssumptions(requirements);
  for (const auto bound : assumptions.regions)
    appendUnique(contract.bounds, bound);
  for (const auto &bound : assumptions.types)
    appendUnique(contract.typeBounds, bound);

  // Receiving a checked reference implies validity of the exact referent for
  // that region. Preserve opaque types as type bounds, rather than guessing
  // their borrowed fields from generic spelling or backend layout.
  const auto implied = [&](auto &&self, const SemanticType &type) -> void {
    if (!type.quantifiedLifetimes.empty() || isLifetimeInvocationSignature(type))
      return; // Quantified assumptions are scoped to that callable's body.
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
        type.referenceLifetime.declarationRegion())
      appendUnique(contract.typeBounds,
                   LifetimeTypeBound{type.arguments.front(), type.referenceLifetime});
    lifetime_detail::children(type, [&](const SemanticType &child) {
      self(self, child);
    });
  };
  for (const auto &parameter : parameterTypes)
    implied(implied, parameter);
  if (const auto requirementsFormula = buildLifetimeRequirements(requirements)) {
    // Re-normalizing a concrete instance refreshes predicate facts without
    // duplicating the same declaration formula or changing its leaf ordinals.
    if (!requirements.empty())
      contract.requirements = *requirementsFormula;
    const auto guaranteed = guaranteedLifetimeAssumptions(contract.requirements);
    for (const auto edge : guaranteed.regions)
      appendUnique(contract.bounds, edge);
    for (const auto &bound : guaranteed.types)
      appendUnique(contract.typeBounds, bound);
  } else {
    valid = false;
  }
  for (const auto &parameter : parameterTypes)
    valid = collectNominalFormationAssumptions(parameter, contract) && valid;
  valid = collectNominalFormationAssumptions(returnType, contract) && valid;
  contract.complete = valid;
  return {.contract = std::move(contract), .ambiguousResult = ambiguousResult};
}

auto SemanticVisitor::Impl::lifetimeBoundsForContract(
    const LifetimeSignatureContract &contract) const -> LifetimeBounds {
  LifetimeBounds result;
  for (const auto bound : contract.bounds)
    (void)result.assume(bound.longer, bound.shorter);
  std::vector<LifetimeTypeBound> pending = contract.typeBounds;
  std::vector<LifetimeTypeBound> visited;
  for (std::size_t index = 0; index < pending.size() && visited.size() < 256; ++index) {
    const auto bound = pending[index];
    if (std::find(visited.begin(), visited.end(), bound) != visited.end())
      continue;
    visited.push_back(bound);
    const auto &type = bound.type;
    const auto add = [&](const SemanticType &child) {
      appendUnique(pending, LifetimeTypeBound{child, bound.region});
    };
    if (type.kind == SemanticType::TypeParameter ||
        type.kind == SemanticType::RawPointer ||
        isLifetimeInvocationSignature(type))
      continue; // Opaque types and raw pointees imply no checked storage facts.
    if (type.kind == SemanticType::Reference) {
      (void)result.assume(type.referenceLifetime, bound.region);
      // A well-formed incoming reference also establishes its referent's
      // validity for the borrow. Do not infer this from an unchecked pointer.
      if (type.arguments.size() == 1)
        add(type.arguments.front());
    } else if (type.kind == SemanticType::StringView || type.kind == SemanticType::CString) {
      for (const auto region : type.lifetimeArguments)
        (void)result.assume(region, bound.region);
    } else if (type.kind == SemanticType::Class) {
      if (const auto *owner = classInfo(type)) {
        const auto substitution = classSubstitution(type);
        for (const auto &field : owner->fields) {
          const auto member = owner->members.find(field.declaration->name().lexeme);
          if (member != owner->members.end())
            add(substituteType(member->second.symbol.type, substitution));
        }
        for (const auto &base : owner->bases)
          if (!base.interface)
            add(substituteType(base.type, substitution));
      }
    } else if (type.kind == SemanticType::Enum) {
      if (const auto enumeration = enumTypeFor(type))
        for (const auto &variant : enumeration->enumerators)
          for (const auto &field : variant.payloadTypes)
            add(field);
    } else {
      lifetime_detail::children(type, add);
    }
  }
  // Expansion is only an implication aid. Exhaustion contributes no guessed
  // facts; exact opaque type bounds remain available to the outlives checker.
  return result;
}

auto SemanticVisitor::Impl::currentLifetimeBounds(
    const LifetimeSignatureContract *assumptionCase) const -> LifetimeBounds {
  const auto *function = currentFunctionInfo();
  const auto *contract = assumptionCase ? assumptionCase
                             : loanFlow.lifetimeContract.valid()
                             ? &loanFlow.lifetimeContract
                             : function ? &function->lifetimeContract : nullptr;
  LifetimeSignatureContract assumptions = contract ? *contract
                                                  : LifetimeSignatureContract{};
  for (const auto &scope : requirementScopes) {
    const auto known = guaranteedLifetimeAssumptions(scope);
    for (const auto bound : known.regions)
      appendUnique(assumptions.bounds, bound);
    for (const auto &bound : known.types)
      appendUnique(assumptions.typeBounds, bound);
  }
  return lifetimeBoundsForContract(assumptions);
}

auto SemanticVisitor::Impl::lifetimeOfOriginPlace(
    const SemanticLoanPlace &place) const -> Lifetime {
  if (!place.valid())
    return {};
  SemanticType type;
  Lifetime region;
  if (place.receiver) {
    region = loanFlow.lifetimeContract.receiver;
    if (const auto *function = currentFunctionInfo();
        function && function->ownerClass && !function->staticMember) {
      type = openClassType(function->ownerClass);
      if (!region.specified())
        region = function->lifetimeContract.receiver;
    } else if (currentClass) {
      type = openClassType(*currentClass);
      if (!region.specified())
        region = currentReceiverLifetime;
    }
  } else if (const auto *symbol = semanticModel.database().findSymbol(place.root)) {
    type = symbol->type;
    if (!symbol->threadLocal &&
        (symbol->kind == SymbolKind::GlobalVariable || symbol->staticMember))
      region = Lifetime::staticLifetime();
  }
  if (!place.receiver && currentLambdaReturn)
    for (const auto &capture : currentLambdaReturn->captures)
      if (capture.bindingSymbol && capture.bindingSymbol == place.root) {
        type = capture.type;
        region = currentLambdaReturn->lifetimeContract.receiver;
        break;
      }
  bool inputType = false;
  if (!place.receiver)
    for (const auto &[symbol, declared] : loanFlow.lifetimeInputTypes)
      if (symbol == place.root) {
        type = declared;
        inputType = true;
        break;
      }
  if (!place.receiver && !inputType)
    if (const auto *function = currentFunctionInfo();
        function && function->bodyDeclaration())
      for (const auto &parameter : function->bodyDeclaration()->parameters())
        if (const auto *binding = semanticModel.findBinding(parameter);
            binding && binding->symbol == place.root) {
          type = binding->type;
          break;
        }
  const auto reference = [&] {
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1) {
      region = type.referenceLifetime;
      type = SemanticType(type.arguments.front());
    }
  };
  reference();
  for (const auto &projection : place.projections) {
    if (projection.kind == PlaceProjectionKind::Field ||
        projection.kind == PlaceProjectionKind::Base ||
        projection.kind == PlaceProjectionKind::ConstantIndex) {
      const auto selected = borrowedPathElement(type, projection);
      if (!selected)
        return {};
      type = *selected;
      reference();
    } else if (projection.kind == PlaceProjectionKind::DynamicIndex &&
               type.kind == SemanticType::Array && type.arguments.size() == 1) {
      type = SemanticType(type.arguments.front());
      reference();
    } else if (projection.kind == PlaceProjectionKind::Dereference &&
               type.kind == SemanticType::RawPointer) {
      // A raw pointer spelling supplies no checked provenance or region.
      return {};
    } else if (projection.kind != PlaceProjectionKind::Dereference ||
               type.kind != SemanticType::Class) {
      return {};
    }
  }
  return region;
}

auto SemanticVisitor::Impl::requirementsAwaitLifetimeApplication(
    std::span<const AppliedConceptRequirement> requirements) const -> bool {
  return std::any_of(requirements.begin(), requirements.end(), [](const auto &item) {
    return item.lifetimeBound.has_value() || !item.lifetimeArguments.empty() ||
           std::any_of(item.arguments.begin(), item.arguments.end(),
                       [](const auto &type) { return !freeTypeLifetimes(type).empty(); });
  });
}

auto SemanticVisitor::Impl::borrowedExpressionLifetime(const Expr *expression) const
    -> Lifetime {
  std::unordered_set<const Expr *> visited;
  while (expression && visited.insert(expression).second) {
    if (const auto *alias = semanticModel.findExpressionAlias(*expression)) {
      expression = alias;
      continue;
    }
    if (const auto *group = exprAs<Grouping>(expression)) {
      expression = group->expression().get();
      continue;
    }
    if (const auto *binary = exprAs<Binary>(expression);
        binary && binary->oper().kind == TokenKind::COMMA) {
      expression = binary->right().get();
      continue;
    }
    const SemanticType *type = nullptr;
    if (const auto *call = exprAs<Call>(expression)) {
      if (const auto *resolved = semanticModel.findCall(*call))
        type = &resolved->returnType;
      else if (const auto *resolved = semanticModel.findLambdaCall(*call))
        type = &resolved->returnType;
    }
    if (!type)
      if (const auto *resolved = semanticModel.findOperator(*expression))
        type = &resolved->returnType;
    if (type && type->kind == SemanticType::Reference)
      return type->referenceLifetime;
    if (const auto *variable = exprAs<Variable>(expression))
      if (const auto *symbol = resolve(variable->name());
          symbol && symbol->type.kind == SemanticType::Reference &&
          symbol->type.referenceLifetime.specified()) {
        const auto region = symbol->type.referenceLifetime;
        if (symbol->parameterDeclaration ||
            std::any_of(loanFlow.lifetimeObligations.begin(),
                        loanFlow.lifetimeObligations.end(),
                        [&](const auto bound) { return bound.shorter == region; }))
          return region;
      }
    break;
  }
  return {};
}

auto SemanticVisitor::Impl::requireLifetimeRelationship(
    Lifetime source, Lifetime target, const Token &site, bool diagnose) -> bool {
  const std::array required{LifetimeBound{source, target}};
  if (!validateLifetimeConstraints(required)) {
    if (diagnose)
      report(site, "The borrowed value does not satisfy the destination's "
                   "lifetime contract.", "GTI-S2127");
    return false;
  }
  appendUnique(loanFlow.lifetimeObligations, LifetimeBound{source, target});
  if (target.kind == Lifetime::Kind::Inference)
    appendUnique(loanFlow.lifetimeVariables, target);
  return true;
}

auto SemanticVisitor::Impl::recordReferenceLifetimeBinding(
    const SemanticType &reference, const ExprPtr &initializer) -> void {
  if (!initializer || reference.kind != SemanticType::Reference ||
      expressionNeverReturns(initializer.get()))
    return;
  auto source = borrowedExpressionLifetime(initializer.get());
  if (!source.specified()) {
    const auto place = retainedLoanPlace(initializer);
    source = lifetimeOfOriginPlace(place);
    if (!source.specified() && place.valid()) {
      source = Lifetime::local(inferredLifetime(
          expressionToken(initializer), std::numeric_limits<std::size_t>::max()).id);
      appendUnique(loanFlow.lifetimeOrigins,
                   SemanticLifetimeOrigin{.region = source, .place = place,
                                          .source = initializer.get()});
    }
  }
  if (reference.referenceLifetime.specified())
    (void)requireLifetimeRelationship(source, reference.referenceLifetime,
                                      expressionToken(initializer));
}

auto SemanticVisitor::Impl::firstRequirementFailureBeforeLifetimeApplication(
    std::span<const AppliedConceptRequirement> requirements) const
    -> std::optional<ConstraintFailure> {
  std::vector<const ConstraintExpression *> visited;
  for (const auto &requirement : requirements) {
    std::vector<AppliedConceptRequirement> group;
    if (requirement.expression) {
      if (std::find(visited.begin(), visited.end(), requirement.expression) != visited.end())
        continue;
      visited.push_back(requirement.expression);
      for (const auto &other : requirements)
        if (other.expression == requirement.expression)
          group.push_back(other);
    } else {
      group.push_back(requirement);
    }
    // Defer the whole Boolean formula, never one arm of a disjunction.
    if (!requirementsAwaitLifetimeApplication(group))
      if (auto failure = firstRequirementFailure(group))
        return failure;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::commitLifetimeApplication(
    const SemanticLifetimeApplication &application) -> void {
  assert(application.complete);
  for (const auto variable : application.variables)
    appendUnique(loanFlow.lifetimeVariables, variable);
  for (const auto bound : application.obligations)
    appendUnique(loanFlow.lifetimeObligations, bound);
  for (const auto &bound : application.typeObligations)
    appendUnique(loanFlow.lifetimeTypeObligations, bound);
  for (const auto &origin : application.origins)
    appendUnique(loanFlow.lifetimeOrigins, origin);
}

auto SemanticVisitor::Impl::constrainLifetimeType(
    const SemanticType &actual, const SemanticType &expected, int variance,
    std::vector<LifetimeBound> &obligations, bool allowBase) const
    -> std::optional<SemanticType> {
  std::vector<std::pair<SemanticType, SemanticType>> active;
  const auto relate = [&](auto &&self, const SemanticType &source,
                          const SemanticType &target, int mode, bool allowProjection)
      -> std::optional<SemanticType> {
    if (source == target)
      return source;
    if (source.kind == SemanticType::StringView &&
        target.kind == SemanticType::CString) {
      // Text conversion preserves the same storage. Adapt only the region
      // spelling here: ordinary conversion rules still prove NUL termination
      // and decide whether this particular value can become a c_string.
      auto viewTarget = target;
      viewTarget.kind = SemanticType::StringView;
      return self(self, source, viewTarget, mode, false);
    }
    if (source.kind != target.kind)
      return source; // The ordinary conversion rules still own type shape.
    if (source.hasLambdaShape() && target.hasLambdaShape() &&
        source.lambdaId == target.lambdaId) {
      const auto *lambda = semanticModel.findLambda(source.lambdaId);
      if (!lambda && instanceBaseModel)
        lambda = instanceBaseModel->findLambda(source.lambdaId);
      if (!lambda || !lambda->type.hasLambdaShape())
        return std::nullopt;
      const auto sourceEnvironment = matchUniversalTypeLifetimes(
          lambda->type, source, lambda->environmentLifetimes);
      const auto targetEnvironment = matchUniversalTypeLifetimes(
          lambda->type, target, lambda->environmentLifetimes);
      if (!sourceEnvironment || !targetEnvironment ||
          sourceEnvironment->size() != lambda->environmentLifetimes.size() ||
          targetEnvironment->size() != lambda->environmentLifetimes.size())
        return std::nullopt;
      for (const auto slot : lambda->environmentLifetimes) {
        const auto from = std::find_if(sourceEnvironment->begin(), sourceEnvironment->end(),
            [&](const auto binding) { return binding.first == slot; });
        const auto to = std::find_if(targetEnvironment->begin(), targetEnvironment->end(),
            [&](const auto binding) { return binding.first == slot; });
        if (from == sourceEnvironment->end() || to == targetEnvironment->end())
          return std::nullopt;
        appendUnique(obligations, LifetimeBound{from->second, to->second});
        appendUnique(obligations, LifetimeBound{to->second, from->second});
      }
      return target;
    }
    if (!source.quantifiedLifetimes.empty() ||
        !target.quantifiedLifetimes.empty())
      return equivalentLifetimeTypes(source, target)
                 ? std::optional<SemanticType>{target} : std::nullopt;
    const auto key = std::pair{source, target};
    if (active.size() >= 256 ||
        std::find(active.begin(), active.end(), key) != active.end())
      return std::nullopt;
    active.push_back(key);
    struct Pop {
      decltype(active) &values;
      ~Pop() { values.pop_back(); }
    } pop{active};
    if (source.kind == SemanticType::Class && source.classId != target.classId) {
      if (!allowProjection)
        return source;
      std::vector<SemanticType> matches;
      std::vector<SemanticType> visited;
      const auto project = [&](auto &&walk, const SemanticType &type) -> void {
        if (visited.size() >= 256 ||
            std::find(visited.begin(), visited.end(), type) != visited.end())
          return;
        visited.push_back(type);
        if (type.classId == target.classId) {
          appendUnique(matches, type);
          return;
        }
        if (const auto *owner = classInfo(type)) {
          const auto substitution = classSubstitution(type);
          for (const auto &base : owner->bases)
            if (!base.interface)
              walk(walk, substituteType(base.type, substitution));
        }
      };
      project(project, source);
      // Unrelated nominal types must never relate their slots by position.
      if (matches.empty())
        return source;
      if (matches.size() != 1)
        return std::nullopt;
      return self(self, matches.front(), target, mode, false);
    }
    if (source.kind == SemanticType::Enum && source.enumId != target.enumId)
      return source;
    SemanticType adjusted = source;
    const auto region = [&](Lifetime from, Lifetime to, int direction) {
      if (!from.specified() && !to.specified())
        return true;
      if (!from.specified() || !to.specified())
        return false;
      appendUnique(obligations, direction < 0 ? LifetimeBound{to, from}
                                             : LifetimeBound{from, to});
      if (direction == 0)
        appendUnique(obligations, LifetimeBound{to, from});
      return true;
    };
    if (source.kind == SemanticType::Reference) {
      if (!region(source.referenceLifetime, target.referenceLifetime, mode))
        return std::nullopt;
      adjusted.referenceLifetime = target.referenceLifetime;
    }
    if (source.arguments.size() != target.arguments.size() ||
        source.lifetimeArguments.size() != target.lifetimeArguments.size())
      return source;
    const bool nominal = source.kind == SemanticType::Class ||
                         source.kind == SemanticType::Enum;
    const auto descriptor = nominal ? nominalVariance(target) : NominalVariance{};
    if (nominal &&
        (!descriptor.complete || descriptor.types.size() != target.arguments.size() ||
         descriptor.lifetimes.size() != target.lifetimeArguments.size()))
      return std::nullopt;
    const auto compose = [&](TypeVariance nested) {
      return nested == TypeVariance::Invariant ? 0
           : nested == TypeVariance::Contravariant ? -mode : mode;
    };
    for (std::size_t index = 0; index < source.lifetimeArguments.size(); ++index) {
      const auto direction = nominal ? descriptor.lifetimes[index]
          : (source.kind == SemanticType::StringView || source.kind == SemanticType::CString)
                ? TypeVariance::Covariant
                                                   : TypeVariance::Invariant;
      if (direction != TypeVariance::Bivariant &&
          !region(source.lifetimeArguments[index], target.lifetimeArguments[index],
                  compose(direction)))
        return std::nullopt;
      adjusted.lifetimeArguments[index] = target.lifetimeArguments[index];
    }
    for (std::size_t index = 0; index < source.arguments.size(); ++index) {
      auto direction = nominal ? descriptor.types[index] : TypeVariance::Covariant;
      if (!nominal &&
          ((target.kind == SemanticType::Reference &&
            target.referenceAccess == AccessMode::Mutable) ||
           target.kind == SemanticType::RawPointer))
        direction = TypeVariance::Invariant;
      else if (isLifetimeInvocationSignature(target) && index != 0)
        direction = TypeVariance::Contravariant;
      if (direction == TypeVariance::Bivariant) {
        // Ignored parameters may vary in their regions, never in their
        // unrelated runtime type identity. Recurse invariantly for shape,
        // then discard only this position's lifetime obligations.
        std::vector<LifetimeBound> saved = obligations;
        auto child = self(self, source.arguments[index], target.arguments[index], 0, false);
        obligations = std::move(saved);
        if (!child)
          return std::nullopt;
        adjusted.arguments[index] = std::move(*child);
      } else {
        auto child = self(self, source.arguments[index], target.arguments[index],
                          compose(direction), target.kind == SemanticType::Reference);
        if (!child)
          return std::nullopt;
        adjusted.arguments[index] = std::move(*child);
      }
    }
    return adjusted;
  };
  return relate(relate, actual, expected, variance, allowBase);
}

auto SemanticVisitor::Impl::applyLifetimeValueConversion(
    const SemanticType &actual, const SemanticType &expected, const Token &site,
    int variance, bool allowBase, bool diagnose) -> std::optional<SemanticType> {
  std::vector<LifetimeBound> obligations;
  auto adjusted = constrainLifetimeType(actual, expected, variance, obligations, allowBase);
  if (!adjusted)
    return std::nullopt;
  if (!validateLifetimeConstraints(obligations)) {
    if (diagnose)
      report(site, "This value conversion requires a lifetime relationship "
                   "that the current borrowing contract does not prove.",
             "GTI-S2127");
    return std::nullopt;
  }
  for (const auto edge : obligations) {
    appendUnique(loanFlow.lifetimeObligations, edge);
    for (const auto region : {edge.longer, edge.shorter})
      if (region.kind == Lifetime::Kind::Inference)
        appendUnique(loanFlow.lifetimeVariables, region);
  }
  return adjusted;
}

auto SemanticVisitor::Impl::callLifetimeArgumentType(
    const CallableCandidate &function, const SemanticType &actual,
    const SemanticType &expected) const -> std::optional<SemanticType> {
  if (!function.lifetimeApplication.complete)
    return std::nullopt;
  std::vector<LifetimeBound> obligations;
  const auto &target = actual.kind != SemanticType::Reference &&
                               expected.kind == SemanticType::Reference &&
                               expected.arguments.size() == 1
                           ? expected.arguments.front() : expected;
  // An existing reference may be reborrowed for a shorter call region. Its
  // outer lifetime is covariant even for mutable access; constrainLifetimeType
  // keeps the mutable referent invariant. A value argument has no reference
  // shell here, so its whole type is the invariant mutable referent instead.
  const int variance = actual.kind != SemanticType::Reference &&
                               expected.kind == SemanticType::Reference &&
                               expected.referenceAccess == AccessMode::Mutable
                           ? 0 : 1;
  auto adjusted = constrainLifetimeType(actual, target, variance, obligations,
                                        expected.kind == SemanticType::Reference);
  if (!adjusted)
    return std::nullopt;
  // Selection may use only relationships proved by the complete application;
  // it must not silently create an unrecorded inference obligation.
  if (!validateLifetimeConstraints(function.lifetimeApplication.obligations, obligations))
    return std::nullopt;
  return adjusted;
}

auto SemanticVisitor::Impl::applyCallLifetimeContract(
    CallableCandidate &function, const std::vector<SemanticType> &argumentTypes,
    const ExprList &arguments, const Token &site, const Expr *receiver,
    bool diagnose) -> bool {
  std::vector<AnalyzedCallArgument> analyzed;
  for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
    const auto *expression = index < arguments.size() ? &arguments[index] : nullptr;
    if (expression && argumentTypes[index].kind == SemanticType::TypePack &&
        argumentTypes[index].concretePack) {
      if (!appendConcreteCallArgument(argumentTypes[index], *expression, analyzed))
        return false;
    } else {
      analyzed.push_back({.type = argumentTypes[index], .expression = expression});
    }
  }
  return applyCallLifetimeContract(function, analyzed, site, receiver, diagnose);
}

auto SemanticVisitor::Impl::applyCallLifetimeContract(
    CallableCandidate &function, std::span<const AnalyzedCallArgument> arguments,
    const Token &site, const Expr *receiver, bool diagnose,
    const SemanticLoanPlace *receiverPlace) -> bool {
  notePrivateLifetimeDependency(function.id);
  const auto reject = [&](std::string_view message) {
    if (diagnose)
      report(site, std::string(message), "GTI-S2127");
    return false;
  };
  if (!function.lifetimeContract.valid())
    return reject("The selected callable has no complete lifetime contract.");
  SemanticLifetimeApplication application;
  application.bindings = function.lifetimeApplication.bindings;
  const auto bind = [&](Lifetime parameter, Lifetime value) {
    if (!value.specified() || parameter.kind != Lifetime::Kind::Parameter)
      return false;
    for (const auto existing : application.bindings)
      if (existing.first == parameter)
        return existing.second == value;
    application.bindings.emplace_back(parameter, value);
    return true;
  };
  // A method's enclosing nominal arguments are already selected by its
  // receiver. They must not be freshly inferred as operation-local binders.
  if (function.ownerClass && function.dispatchOwner.kind == SemanticType::Class) {
    std::size_t index = 0;
    for (const auto &parameter : classInfo(function.ownerClass).genericParameters)
      if (parameter.lifetime) {
        if (index < function.dispatchOwner.lifetimeArguments.size() &&
            !bind(Lifetime::parameter(parameter.id),
                  function.dispatchOwner.lifetimeArguments[index]))
          return reject("Conflicting lifetime arguments for the method's owner.");
        ++index;
      }
  }
  for (const auto parameter : function.lifetimeContract.parameters)
    if (std::none_of(application.bindings.begin(), application.bindings.end(),
                     [&](const auto existing) { return existing.first == parameter; }))
      (void)bind(parameter, inferredLifetime(site, parameter.id));
  for (const auto binding : application.bindings) {
    if (std::find(function.lifetimeContract.parameters.begin(),
                  function.lifetimeContract.parameters.end(), binding.first) ==
            function.lifetimeContract.parameters.end() ||
        !binding.second.specified())
      return reject("A call lifetime argument does not belong to this declaration.");
    if (binding.second.kind == Lifetime::Kind::Inference)
      appendUnique(application.variables, binding.second);
  }
  const auto substitute = [&](Lifetime region) {
    for (const auto binding : application.bindings)
      if (binding.first == region)
        return binding.second;
    return region;
  };
  const auto require = [&](Lifetime longer, Lifetime shorter) {
    if (!longer.specified() || !shorter.specified())
      return false;
    appendUnique(application.obligations, LifetimeBound{longer, shorter});
    return true;
  };
  for (const auto bound : function.lifetimeApplication.obligations)
    if (!require(substitute(bound.longer), substitute(bound.shorter)))
      return reject("The selected interface lifetime obligation is unresolved.");
  for (const auto bound : function.lifetimeContract.bounds)
    if (!require(substitute(bound.longer), substitute(bound.shorter)))
      return reject("The selected lifetime bound contains an unresolved region.");

  std::size_t originSlot = 0;
  const auto actualRegion = [&](const SemanticLoanPlace &place,
                                const Expr *source) {
    if (const auto known = lifetimeOfOriginPlace(place); known.specified())
      return known;
    if (!place.valid() && !source)
      return Lifetime{};
    for (const auto &origin : application.origins)
      if (origin.place == place && origin.source == source)
        return origin.region;
    const Token token = source && place.valid() ? expressionToken(*source) : site;
    const auto region = Lifetime::local(
        inferredLifetime(token, std::numeric_limits<std::size_t>::max() - originSlot++).id);
    appendUnique(application.origins,
                 SemanticLifetimeOrigin{.region = region, .place = place,
                                        .source = source});
    return region;
  };
  std::vector<SemanticType> parameters;
  for (const auto &parameter : function.parameterTypes) {
    auto instantiated = substituteTypeLifetimes(parameter, application.bindings);
    if (!instantiated)
      return reject("A call lifetime substitution would capture a quantified lifetime.");
    parameters.push_back(std::move(*instantiated));
  }
  const auto result = substituteTypeLifetimes(function.returnType, application.bindings);
  if (!result)
    return reject("A result lifetime substitution would capture a quantified lifetime.");
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto &parameter = parameters[index];
    const AnalyzedCallArgument *argument = index < arguments.size() ? &arguments[index]
                                                                 : nullptr;
    const Expr *source = argument && argument->expression
                             ? argument->expression->get() : nullptr;
    if (!source && function.declaration &&
        index < function.declaration->parameters().size())
      if (const auto &argument = function.declaration->parameters()[index].defaultArgument)
        source = argument->expression.get();
    const auto actual = argument
                            ? argument->type
                            : source ? semanticModel.typeOf(*source)
                                     : SemanticType::Unknown;
    if (parameter.kind == SemanticType::Reference && parameter.arguments.size() == 1) {
      const auto declaredRegion = actual.kind == SemanticType::Reference
                                      ? actual.referenceLifetime
                                      : borrowedExpressionLifetime(source);
      std::vector<SemanticLoanDependency> dependencies;
      // A reference to a carrier borrows its physical shell. Its contents'
      // external owners are distinct obligations in the nested type relation.
      if (source && !typeTraits(actual).containsBorrowedState)
        dependencies = argument ? storedBorrowDependencies(*argument)
                                : storedBorrowDependencies(source);
      if (dependencies.empty()) {
        auto place = source ? retainedLoanPlace(source) : SemanticLoanPlace{};
        if (argument && argument->forwardedPackElement) {
          const auto *carrier = parameterPackArgument(*argument);
          place = carrier ? SemanticLoanPlace{.root = toolingSymbolFor(*carrier)}
                          : SemanticLoanPlace{};
          for (const auto element : argument->packPath)
            place.projections.push_back(
                {.kind = PlaceProjectionKind::ConstantIndex, .index = element});
        }
        dependencies.push_back({.place = std::move(place)});
      }
      if (declaredRegion.specified()) {
        if (!require(declaredRegion, parameter.referenceLifetime))
          return reject("A borrowed argument has no complete lifetime contract.");
      } else {
        for (std::size_t dependency = 0; dependency < dependencies.size(); ++dependency)
          if (!require(actualRegion(dependencies[dependency].place, source),
                       parameter.referenceLifetime))
            return reject("A borrowed argument has no checked lifetime origin.");
      }
      const auto &referent = actual.kind == SemanticType::Reference &&
                                     actual.arguments.size() == 1
                                 ? actual.arguments.front() : actual;
      if (!constrainLifetimeType(referent, parameter.arguments.front(),
                                 parameter.referenceAccess == AccessMode::Mutable ? 0 : 1,
                                 application.obligations, true))
        return reject("The argument's nested lifetimes do not match the parameter contract.");
    } else if (!constrainLifetimeType(actual, parameter, 1, application.obligations)) {
      return reject("The argument's lifetime contract does not match the parameter.");
    }
  }
  if (function.lifetimeContract.receiver.specified()) {
    const auto place = receiverPlace ? *receiverPlace
                     : receiver ? loanPlace(receiver)
                                : SemanticLoanPlace{.receiver = true};
    if (!require(actualRegion(place, receiver),
                 substitute(function.lifetimeContract.receiver)))
      return reject("The method receiver has no checked lifetime origin.");
  }
  for (const auto &bound : function.lifetimeContract.typeBounds) {
    auto type = substituteTypeLifetimes(bound.type, application.bindings);
    if (!type)
      return reject("A type-outlives obligation captures a quantified lifetime.");
    application.typeObligations.push_back({std::move(*type), substitute(bound.region)});
  }
  for (const auto &bound : function.lifetimeApplication.typeObligations) {
    auto type = substituteTypeLifetimes(bound.type, application.bindings);
    if (!type)
      return reject("An interface type-outlives obligation captures a quantified lifetime.");
    appendUnique(application.typeObligations,
                 LifetimeTypeBound{std::move(*type), substitute(bound.region)});
  }
  if (!validateLifetimeConstraints(application.obligations))
    return reject("The call requires a lifetime relationship the caller cannot prove.");
  // Check obligations in the same body graph without publishing a speculative
  // overload candidate. The selected caller commits them after overload choice.
  const auto previous = loanFlow.lifetimeObligations.size();
  loanFlow.lifetimeObligations.insert(loanFlow.lifetimeObligations.end(),
                                      application.obligations.begin(),
                                      application.obligations.end());
  struct Restore {
    std::vector<LifetimeBound> &values; std::size_t size;
    ~Restore() { values.resize(size); }
  } restore{loanFlow.lifetimeObligations, previous};
  const auto previousOrigins = loanFlow.lifetimeOrigins.size();
  loanFlow.lifetimeOrigins.insert(loanFlow.lifetimeOrigins.end(),
                                  application.origins.begin(), application.origins.end());
  struct RestoreOrigins {
    std::vector<SemanticLifetimeOrigin> &values; std::size_t size;
    ~RestoreOrigins() { values.resize(size); }
  } restoreOrigins{loanFlow.lifetimeOrigins, previousOrigins};
  auto requirements = function.requirements;
  for (auto &requirement : requirements) {
    for (auto &type : requirement.arguments)
      if (auto replacement = substituteTypeLifetimes(type, application.bindings))
        type = std::move(*replacement);
    for (auto &region : requirement.lifetimeArguments)
      region = substitute(region);
    if (requirement.lifetimeBound)
      requirement.lifetimeBound = LifetimeBound{
          substitute(requirement.lifetimeBound->longer),
          substitute(requirement.lifetimeBound->shorter)};
    if (requirement.outlivesType)
      requirement.outlivesType = substituteTypeLifetimes(
          *requirement.outlivesType, application.bindings);
  }
  auto formula = buildLifetimeRequirements(requirements);
  if (!formula)
    return reject("The lifetime requirement graph is recursive or exceeds its bounded size.");
  for (const auto &bound : application.typeObligations) {
    const auto root = formula->nodes.size();
    formula->nodes.push_back({.kind = LifetimeRequirementNode::Kind::Type,
                              .bound = {{}, bound.region}, .type = bound.type});
    if (formula->root == LifetimeRequirementNode::invalidIndex) {
      formula->root = root;
    } else {
      formula->nodes.push_back({.kind = LifetimeRequirementNode::Kind::All,
                                .left = formula->root, .right = root});
      formula->root = formula->nodes.size() - 1;
    }
  }
  const auto previousApplicationEdges = application.obligations.size();
  if (!applyLifetimeRequirements(*formula, application, site, diagnose))
    return false;
  loanFlow.lifetimeObligations.insert(
      loanFlow.lifetimeObligations.end(),
      application.obligations.begin() + previousApplicationEdges,
      application.obligations.end());
  for (const auto edge : application.obligations)
    for (const auto region : {edge.longer, edge.shorter})
      if (region.kind == Lifetime::Kind::Inference)
        appendUnique(application.variables, region);
  if (const auto failure = firstRequirementFailureBeforeLifetimeApplication(requirements)) {
    if (diagnose)
      reportConstraintFailure(site, *failure);
    return false;
  }
  application.complete = true;
  function.lifetimeApplication = std::move(application);
  function.returnType = *result;
  function.parameterTypes = std::move(parameters);
  function.requirements = std::move(requirements);
  return true;
}

auto SemanticVisitor::Impl::finalizeLifetimeBody() -> void {
  if (!loanFlow.lifetimeBlock && !loanFlow.lifetimeExpression)
    return;
  SemanticLifetimeBody body{.variables = loanFlow.lifetimeVariables,
                            .obligations = loanFlow.lifetimeObligations,
                            .typeObligations = loanFlow.lifetimeTypeObligations,
                            .origins = loanFlow.lifetimeOrigins};
  const bool valid = validateLifetimeConstraints();
  body.complete = valid && !loanFlow.deferredGenericFlow;
  if (!valid && !loanFlow.deferredGenericFlow) {
    Token site = currentFunctionDeclaration ? currentFunctionDeclaration->name()
                                           : Token{};
    if (loanFlow.lifetimeExpression)
      site = expressionToken(*loanFlow.lifetimeExpression);
    else if (!body.origins.empty() && body.origins.front().source)
      site = expressionToken(*body.origins.front().source);
    report(site, "The body's inferred lifetime requirements conflict with its "
                 "declared borrowing contract.", "GTI-S2127");
  }
  if (loanFlow.lifetimeBlock)
    semanticModel.recordLifetimeBody(*loanFlow.lifetimeBlock, std::move(body));
  else
    semanticModel.recordLifetimeBody(*loanFlow.lifetimeExpression, std::move(body));
}

auto SemanticVisitor::Impl::declaredLifetimeRelationship(
    const LifetimeSignatureContract &contract, Lifetime source, Lifetime target) const
    -> bool {
  return visitLifetimeAssumptionCases(contract, [&](const auto &assumptions) {
    return lifetimeBoundsForContract(assumptions).proves(source, target);
  }).proof == LifetimeFormulaProof::Proven;
}

auto SemanticVisitor::Impl::proveLifetimeTypeOutlives(
    const SemanticType &requestedType, Lifetime region,
    const LifetimeSignatureContract &assumptionCase,
    const LifetimeInferenceConstraints &constraints) const -> bool {
  if (!region.specified() || constraints.failure())
    return false;
  std::vector<SemanticType> pending;
  const auto check = [&](auto &&self, const SemanticType &type) -> bool {
    for (const auto &known : assumptionCase.typeBounds)
      if (equivalentLifetimeTypes(type, known.type) &&
          constraints.proves(known.region, region))
        return true;
    if (type.kind == SemanticType::Unknown || type.kind == SemanticType::TypeParameter ||
        (type.kind == SemanticType::TypePack && !type.concretePack))
      return false;
    if (std::find(pending.begin(), pending.end(), type) != pending.end())
      return true;
    if (pending.size() >= 256)
      return false;
    pending.push_back(type);
    struct Pop { std::vector<SemanticType> &values; ~Pop() { values.pop_back(); } } pop{pending};
    if (type.kind == SemanticType::Reference &&
        !constraints.proves(type.referenceLifetime, region))
      return false;
    if (type.kind == SemanticType::Lambda) {
      const auto *lambda = semanticModel.findLambda(type.lambdaId);
      if (!type.hasLambdaShape() || !lambda ||
          lambda->captures.size() != type.lambdaCaptureTypes().size())
        return false;
      // A closure stores its captures. Per-invocation signature binders and
      // ambient slots used only in that signature are not stored borrows.
      for (const auto &capture : type.lambdaCaptureTypes())
        if (!self(self, capture))
          return false;
      return true;
    }
    if (type.kind != SemanticType::Class && type.kind != SemanticType::Enum)
      for (const auto lifetime : type.lifetimeArguments)
        if (!constraints.proves(lifetime, region))
          return false;
    if ((type.kind == SemanticType::StringView || type.kind == SemanticType::CString) &&
        type.lifetimeArguments.size() != 1)
      return false;
    if (type.kind == SemanticType::Class) {
      const auto *owner = classInfo(type);
      if (!owner)
        return false;
      const auto substitution = classSubstitution(type);
      for (const auto &field : owner->fields) {
        if (!field.declaration)
          return false;
        const auto member = owner->members.find(field.declaration->name().lexeme);
        if (member == owner->members.end() ||
            !self(self, substituteType(member->second.symbol.type, substitution)))
          return false;
      }
      for (const auto &base : owner->bases)
        if (!base.interface && !self(self, substituteType(base.type, substitution)))
          return false;
      return true;
    }
    if (type.kind == SemanticType::Enum) {
      const auto enumeration = enumTypeFor(type);
      if (!enumeration)
        return false;
      for (const auto &variant : enumeration->enumerators)
        for (const auto &payload : variant.payloadTypes)
          if (!self(self, payload))
            return false;
      return true;
    }
    if ((isLifetimeInvocationSignature(type) || type.kind == SemanticType::FunctionItem) &&
        !type.quantifiedLifetimes.empty()) {
      for (const auto lifetime : freeTypeLifetimes(type))
        if (!constraints.proves(lifetime, region))
          return false;
      return true;
    }
    bool valid = true;
    lifetime_detail::children(type, [&](const auto &child) {
      valid = self(self, child) && valid;
    });
    return valid;
  };
  return check(check, requestedType);
}

auto SemanticVisitor::Impl::lifetimeRequirementFailure(
    const AppliedConceptRequirement &requirement,
    const LifetimeSignatureContract *assumptionCase) const
    -> std::optional<ConstraintFailure> {
  if (!requirement.lifetimeBound)
    return std::nullopt;
  if (!assumptionCase) {
    std::optional<ConstraintFailure> failure;
    if (forEveryLifetimeAssumption([&](const auto &branch) {
          failure = lifetimeRequirementFailure(requirement, &branch);
          return !failure;
        }))
      return std::nullopt;
    if (failure)
      return failure;
    return ConstraintFailure{
        .parameter = requirement.syntax ? requirement.syntax->name.last() : Token{},
        .argument = requirement.outlivesType.value_or(SemanticType::Unknown),
        .application = requirement.syntax, .leafApplication = requirement.syntax,
        .expressionFailure = "The lifetime requirement cannot be proved within the bounded declaration-contract analysis."};
  }
  LifetimeAssumptions assumptions;
  if (assumptionCase) {
    assumptions.regions = assumptionCase->bounds;
    assumptions.types = assumptionCase->typeBounds;
  } else if (loanFlow.lifetimeContract.valid()) {
    assumptions.regions = loanFlow.lifetimeContract.bounds;
    assumptions.types = loanFlow.lifetimeContract.typeBounds;
  } else if (const auto *function = currentFunctionInfo()) {
    assumptions.regions = function->lifetimeContract.bounds;
    assumptions.types = function->lifetimeContract.typeBounds;
  }
  for (const auto &scope : requirementScopes) {
    const auto known = guaranteedLifetimeAssumptions(scope);
    for (const auto bound : known.regions)
      appendUnique(assumptions.regions, bound);
    for (const auto &bound : known.types)
      appendUnique(assumptions.types, bound);
  }
  LifetimeBounds bounds = currentLifetimeBounds(assumptionCase);
  bool wellFormed = true;
  for (const auto bound : assumptions.regions)
    wellFormed = bounds.assume(bound.longer, bound.shorter) && wellFormed;
  LifetimeInferenceConstraints constraints(bounds);
  for (const auto bound : loanFlow.lifetimeObligations)
    wellFormed = constraints.require(bound.longer, bound.shorter) && wellFormed;
  const auto requested = *requirement.lifetimeBound;
  bool satisfied = wellFormed &&
                   constraints.proves(requested.longer, requested.shorter);
  if (requirement.outlivesType && wellFormed) {
    auto explicitContract = *assumptionCase;
    explicitContract.bounds = assumptions.regions;
    explicitContract.typeBounds = assumptions.types;
    // A checked local storage witness establishes its exact value type for
    // that local borrow. In particular, borrowing an owned opaque T does not
    // require the public caller to promise T:static. These facts remain body
    // local: the local anchor cannot outlive a universal result region, and
    // ordinary availability, loan endpoints and cleanup still prove the use.
    for (const auto &origin : loanFlow.lifetimeOrigins) {
      if (origin.region.kind != Lifetime::Kind::Local || !origin.source)
        continue;
      auto type = semanticModel.typeOf(*origin.source);
      if (type.kind == SemanticType::Unknown)
        continue;
      appendUnique(explicitContract.typeBounds,
                   LifetimeTypeBound{type, origin.region});
      if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
        appendUnique(explicitContract.typeBounds,
                     LifetimeTypeBound{type.arguments.front(), origin.region});
    }
    satisfied = proveLifetimeTypeOutlives(*requirement.outlivesType,
        requested.shorter, explicitContract, constraints);
  }
  if (satisfied)
    return std::nullopt;
  return ConstraintFailure{
      .parameter = requirement.syntax ? requirement.syntax->name.last() : Token{},
      .argument = requirement.outlivesType.value_or(SemanticType::Unknown),
      .application = requirement.syntax,
      .leafApplication = requirement.syntax,
      .expressionFailure =
          "The required outlives relationship is not established by the caller's "
          "borrow contract; name or constrain the relationship at its declaration."};
}

} // namespace lang
