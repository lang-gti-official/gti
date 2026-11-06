#include "semantic_analyzer_impl.h"
#include "gti/lifetime_types.h"

namespace lang {
namespace {
template <typename T> void appendUnique(std::vector<T> &values, const T &value) {
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}

struct LifetimeAssumptions {
  std::vector<LifetimeBound> regions;
  std::vector<LifetimeTypeBound> types;
};

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
  bool valid = true;
  bool ambiguousResult = false;
  std::size_t nextSlot = 1; // slot zero is the implicit receiver, when present.
  const auto fresh = [&] {
    const auto slot = nextSlot++;
    return implicitFunctionLifetime(syntax, slot,
                                    "_input" + std::to_string(slot));
  };
  const auto isHole = [](Lifetime region) {
    return region.kind == Lifetime::Kind::Unspecified ||
           region.kind == Lifetime::Kind::Inference;
  };
  const auto addParameter = [&](Lifetime region) {
    if (region.kind == Lifetime::Kind::Parameter && region.specified())
      appendUnique(contract.parameters, region);
  };
  addParameter(contract.receiver);
  if (function.ownerClass != 0) {
    for (const auto &parameter : classInfo(function.ownerClass).genericParameters)
      if (parameter.lifetime)
        addParameter(Lifetime::parameter(parameter.id));
  }
  for (const auto &parameter : function.record().genericParameters)
    if (parameter.lifetime) {
      addParameter(Lifetime::parameter(parameter.id));
      contract.origin = LifetimeContractOrigin::Declared;
    }

  // A callable nested in a signature has its own elision scope. Its inferred
  // input regions become higher-ranked binders, never outer call inputs.
  const auto normalize = [&](auto &&self, SemanticType &type, bool input,
                             Lifetime resultRegion,
                             std::vector<GenericParameterId> *quantified) -> void {
    if (type.kind == SemanticType::FunctionSignature) {
      if (type.arguments.empty()) {
        valid = false;
        return;
      }
      std::vector<Lifetime> inputs;
      for (std::size_t index = 1; index < type.arguments.size(); ++index) {
        self(self, type.arguments[index], true, {}, &type.quantifiedLifetimes);
        for (const auto region : freeTypeLifetimes(type.arguments[index]))
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
    if (type.kind == SemanticType::StringView && type.lifetimeArguments.empty())
      type.lifetimeArguments.emplace_back();
    for (auto &region : type.lifetimeArguments)
      resolve(region);
    lifetime_detail::children(type, [&](SemanticType &child) {
      self(self, child, input, resultRegion, quantified);
    });
  };
  std::vector<Lifetime> inputRegions;
  for (auto &parameter : function.parameterTypes) {
    normalize(normalize, parameter, true, {}, nullptr);
    for (const auto region : freeTypeLifetimes(parameter)) {
      appendUnique(inputRegions, region);
      addParameter(region);
    }
  }
  Lifetime resultRegion = contract.receiver;
  if (!resultRegion.specified() && inputRegions.size() == 1)
    resultRegion = inputRegions.front();
  // Translate the existing one-origin source spelling into the same region
  // interface; it is never a second solver or permission to ignore a bound.
  if (const auto *attribute = syntax.findFunctionAttribute("borrowed_from");
      attribute && attribute->argument) {
    contract.origin = LifetimeContractOrigin::Declared;
    if (attribute->argument->kind == TokenKind::THIS) {
      resultRegion = contract.receiver;
    } else {
      for (std::size_t index = 0; index < syntax.parameters().size() &&
                                  index < function.parameterTypes.size(); ++index) {
        if (syntax.parameters()[index].name.lexeme != attribute->argument->lexeme)
          continue;
        const auto regions = freeTypeLifetimes(function.parameterTypes[index]);
        if (regions.size() == 1) {
          resultRegion = regions.front();
        } else if (!regions.empty()) {
          resultRegion = fresh();
          addParameter(resultRegion);
          for (const auto region : regions)
            appendUnique(contract.bounds, LifetimeBound{region, resultRegion});
        }
      }
    }
  }
  normalize(normalize, function.returnType, false, resultRegion, nullptr);
  for (const auto region : freeTypeLifetimes(function.returnType))
    addParameter(region);
  const auto assumptions = guaranteedLifetimeAssumptions(function.requirements);
  for (const auto bound : assumptions.regions)
    appendUnique(contract.bounds, bound);
  for (const auto &bound : assumptions.types)
    appendUnique(contract.typeBounds, bound);

  // Receiving a checked reference implies validity of the exact referent for
  // that region. Preserve opaque types as type bounds, rather than guessing
  // their borrowed fields from generic spelling or backend layout.
  const auto implied = [&](auto &&self, const SemanticType &type) -> void {
    if (!type.quantifiedLifetimes.empty())
      return; // Quantified assumptions are scoped to that callable's body.
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
        type.referenceLifetime.declarationRegion())
      appendUnique(contract.typeBounds,
                   LifetimeTypeBound{type.arguments.front(), type.referenceLifetime});
    lifetime_detail::children(type, [&](const SemanticType &child) {
      self(self, child);
    });
  };
  for (const auto &parameter : function.parameterTypes)
    implied(implied, parameter);
  contract.complete = valid;
  if (ambiguousResult) {
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
  function.lifetimeContract = std::move(contract);
}

auto SemanticVisitor::Impl::currentLifetimeBounds() const -> LifetimeBounds {
  LifetimeBounds result;
  if (const auto *function = currentFunctionInfo())
    for (const auto bound : function->lifetimeContract.bounds)
      (void)result.assume(bound.longer, bound.shorter);
  for (const auto &scope : requirementScopes)
    for (const auto bound : guaranteedLifetimeAssumptions(scope).regions)
      (void)result.assume(bound.longer, bound.shorter);
  return result;
}

auto SemanticVisitor::Impl::lifetimeOfOriginPlace(
    const SemanticLoanPlace &place) const -> Lifetime {
  if (!place.valid())
    return {};
  SemanticType type;
  Lifetime region;
  if (place.receiver) {
    if (const auto *function = currentFunctionInfo();
        function && function->ownerClass && !function->staticMember) {
      type = openClassType(function->ownerClass);
      region = function->lifetimeContract.receiver;
    } else if (currentClass) {
      type = openClassType(*currentClass);
      region = currentReceiverLifetime;
    }
  } else if (const auto *symbol = semanticModel.database().findSymbol(place.root)) {
    type = symbol->type;
    if (!symbol->threadLocal &&
        (symbol->kind == SymbolKind::GlobalVariable || symbol->staticMember))
      region = Lifetime::staticLifetime();
  }
  if (!place.receiver)
    if (const auto *function = currentFunctionInfo();
        function && function->bodyDeclaration())
      for (std::size_t index = 0;
           index < function->parameterTypes.size() &&
           index < function->bodyDeclaration()->parameters().size(); ++index)
        if (const auto *binding = semanticModel.findBinding(
                function->bodyDeclaration()->parameters()[index]);
            binding && binding->symbol == place.root) {
          type = function->parameterTypes[index];
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
    if (projection.kind == PlaceProjectionKind::Field) {
      const auto *field = semanticModel.database().findSymbol(projection.field);
      if (!field)
        return {};
      type = substituteType(field->type, classSubstitution(type));
      reference();
    } else if ((projection.kind == PlaceProjectionKind::ConstantIndex ||
                projection.kind == PlaceProjectionKind::DynamicIndex) &&
               type.kind == SemanticType::Array && type.arguments.size() == 1) {
      type = SemanticType(type.arguments.front());
      reference();
    } else if (projection.kind == PlaceProjectionKind::Dereference &&
               type.kind == SemanticType::RawPointer) {
      // A raw pointer spelling supplies no checked provenance or region.
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

auto SemanticVisitor::Impl::applyCallLifetimeContract(
    CallableCandidate &function, const std::vector<SemanticType> &argumentTypes,
    const ExprList &arguments, const Token &site, const Expr *receiver,
    bool diagnose) -> bool {
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

  const auto actualRegion = [&](const SemanticLoanPlace &place,
                                const Expr *source, std::size_t slot) {
    if (const auto known = lifetimeOfOriginPlace(place); known.specified())
      return known;
    if (!place.valid() && !source)
      return Lifetime{};
    const Token token = source && place.valid() ? expressionToken(*source) : site;
    const auto region = Lifetime::local(
        inferredLifetime(token, std::numeric_limits<std::size_t>::max() - slot).id);
    appendUnique(application.origins,
                 SemanticLifetimeOrigin{.region = region, .place = place,
                                        .source = source});
    return region;
  };
  const auto relate = [&](auto &&self, const SemanticType &actual,
                          const SemanticType &expected, int variance) -> bool {
    if (actual.kind != expected.kind)
      return true; // Ordinary conversion selection owns non-region shape.
    if (!actual.quantifiedLifetimes.empty() || !expected.quantifiedLifetimes.empty())
      return equivalentLifetimeTypes(actual, expected);
    const auto regions = [&](Lifetime source, Lifetime target, int mode) {
      if (!source.specified() && !target.specified())
        return true;
      return (mode < 0 ? require(target, source) : require(source, target)) &&
             (mode != 0 || require(target, source));
    };
    if (actual.kind == SemanticType::Reference &&
        !regions(actual.referenceLifetime, expected.referenceLifetime, variance))
      return false;
    if (actual.lifetimeArguments.size() != expected.lifetimeArguments.size())
      return false;
    for (std::size_t index = 0; index < actual.lifetimeArguments.size(); ++index)
      if (!regions(actual.lifetimeArguments[index], expected.lifetimeArguments[index],
                   actual.kind == SemanticType::StringView ? variance : 0))
        return false;
    if (actual.arguments.size() != expected.arguments.size())
      return true;
    for (std::size_t index = 0; index < actual.arguments.size(); ++index) {
      int nested = variance;
      if ((expected.kind == SemanticType::Reference &&
           expected.referenceAccess == AccessMode::Mutable) ||
          expected.kind == SemanticType::RawPointer ||
          expected.kind == SemanticType::Class || expected.kind == SemanticType::Enum)
        nested = 0;
      else if (expected.kind == SemanticType::FunctionSignature && index != 0)
        nested = -variance;
      if (!self(self, actual.arguments[index], expected.arguments[index], nested))
        return false;
    }
    return true;
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
    const Expr *source = index < arguments.size() ? arguments[index].get() : nullptr;
    if (!source && function.declaration &&
        index < function.declaration->parameters().size())
      if (const auto &argument = function.declaration->parameters()[index].defaultArgument)
        source = argument->expression.get();
    const auto actual = index < argumentTypes.size()
                            ? argumentTypes[index]
                            : source ? semanticModel.typeOf(*source)
                                     : SemanticType::Unknown;
    if (parameter.kind == SemanticType::Reference && parameter.arguments.size() == 1) {
      std::vector<SemanticLoanDependency> dependencies;
      // A reference to a carrier borrows its physical shell. Its contents'
      // external owners are distinct obligations in the nested type relation.
      if (source && !typeTraits(actual).containsBorrowedState)
        dependencies = storedBorrowDependencies(source);
      if (dependencies.empty())
        dependencies.push_back({.place = source ? retainedLoanPlace(source)
                                               : SemanticLoanPlace{}});
      for (std::size_t dependency = 0; dependency < dependencies.size(); ++dependency)
        if (!require(actualRegion(dependencies[dependency].place, source, dependency),
                     parameter.referenceLifetime))
          return reject("A borrowed argument has no checked lifetime origin.");
      const auto &referent = actual.kind == SemanticType::Reference &&
                                     actual.arguments.size() == 1
                                 ? actual.arguments.front() : actual;
      if (!relate(relate, referent, parameter.arguments.front(),
                  parameter.referenceAccess == AccessMode::Mutable ? 0 : 1))
        return reject("The argument's nested lifetimes do not match the parameter contract.");
    } else if (!relate(relate, actual, parameter, 1)) {
      return reject("The argument's lifetime contract does not match the parameter.");
    }
  }
  if (function.lifetimeContract.receiver.specified()) {
    const auto place = receiver ? loanPlace(receiver)
                               : SemanticLoanPlace{.receiver = true};
    if (!require(actualRegion(place, receiver, 0),
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
  LifetimeInferenceConstraints graph(currentLifetimeBounds());
  for (const auto bound : loanFlow.lifetimeObligations)
    if (!graph.require(bound.longer, bound.shorter))
      return reject("The enclosing body's lifetime constraints are incomplete.");
  for (const auto bound : application.obligations)
    if (!graph.require(bound.longer, bound.shorter))
      return reject("The call's lifetime constraints are incomplete.");
  if (graph.failure())
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
  for (const auto &bound : application.typeObligations) {
    AppliedConceptRequirement requirement{.lifetimeBound = LifetimeBound{{}, bound.region},
                                           .outlivesType = bound.type};
    if (const auto failure = lifetimeRequirementFailure(requirement)) {
      if (diagnose)
        reportConstraintFailure(site, *failure);
      return false;
    }
  }
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
  if (const auto failure = firstRequirementFailure(requirements)) {
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
  LifetimeInferenceConstraints constraints(currentLifetimeBounds());
  bool valid = true;
  for (const auto bound : body.obligations)
    valid = constraints.require(bound.longer, bound.shorter) && valid;
  valid = !constraints.failure() && valid;
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

auto SemanticVisitor::Impl::lifetimeRequirementFailure(
    const AppliedConceptRequirement &requirement) const
    -> std::optional<ConstraintFailure> {
  if (!requirement.lifetimeBound)
    return std::nullopt;
  LifetimeAssumptions assumptions;
  if (const auto *function = currentFunctionInfo()) {
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
  LifetimeBounds bounds;
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
    std::vector<SemanticType> pending;
    const auto typeOutlives = [&](auto &&self, const SemanticType &type) -> bool {
      for (const auto &known : assumptions.types)
        if (equivalentLifetimeTypes(type, known.type) &&
            constraints.proves(known.region, requested.shorter))
          return true;
      if (type.kind == SemanticType::Unknown ||
          type.kind == SemanticType::TypeParameter ||
          (type.kind == SemanticType::TypePack && !type.concretePack))
        return false;
      if (std::find(pending.begin(), pending.end(), type) != pending.end())
        return true; // A nominal recursion adds no new observed component.
      if (pending.size() >= 256)
        return false; // Expanding generic recursion is not a validity proof.
      pending.push_back(type);
      struct Pop { std::vector<SemanticType> &types; ~Pop() { types.pop_back(); } } pop{pending};
      if (type.kind == SemanticType::Reference &&
          !constraints.proves(type.referenceLifetime, requested.shorter))
        return false;
      for (const auto region : type.lifetimeArguments)
        if (!constraints.proves(region, requested.shorter))
          return false;
      if (type.kind == SemanticType::StringView && type.lifetimeArguments.size() != 1)
        return false;
      if (type.kind == SemanticType::Class) {
        const auto *owner = classInfo(type);
        if (!owner)
          return false;
        const auto substitution = classSubstitution(type);
        for (const auto &field : owner->fields) {
          const auto member = owner->members.find(field.declaration->name().lexeme);
          if (member == owner->members.end() ||
              !self(self, substituteType(member->second.symbol.type, substitution)))
            return false;
        }
        for (const auto &base : owner->bases)
          if (!base.interface && !self(self, substituteType(base.type, substitution)))
            return false;
        // Base subobjects and exact borrowed enum projections are included in
        // the semantic inventory; a raw layout walk is never their authority.
        if (const auto inventory = storedReferenceInventory(type))
          for (const auto &path : *inventory)
            if (!self(self, path.reference))
              return false;
        return true;
      }
      if (type.kind == SemanticType::FunctionSignature &&
          !type.quantifiedLifetimes.empty()) {
        // Quantified call validity is independent of the lifetime of a plain
        // function signature; any free captured region remains an obligation.
        for (const auto region : freeTypeLifetimes(type))
          if (!constraints.proves(region, requested.shorter))
            return false;
        return true;
      }
      bool valid = true;
      lifetime_detail::children(type, [&](const SemanticType &child) {
        valid = self(self, child) && valid;
      });
      return valid;
    };
    satisfied = requested.shorter.specified() &&
                typeOutlives(typeOutlives, *requirement.outlivesType);
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
