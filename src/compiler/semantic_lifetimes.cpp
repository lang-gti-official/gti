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
  const auto requested = *requirement.lifetimeBound;
  bool satisfied = wellFormed &&
                   bounds.proves(requested.longer, requested.shorter);
  if (requirement.outlivesType && wellFormed) {
    std::vector<SemanticType> pending;
    const auto typeOutlives = [&](auto &&self, const SemanticType &type) -> bool {
      for (const auto &known : assumptions.types)
        if (equivalentLifetimeTypes(type, known.type) &&
            bounds.proves(known.region, requested.shorter))
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
          !bounds.proves(type.referenceLifetime, requested.shorter))
        return false;
      for (const auto region : type.lifetimeArguments)
        if (!bounds.proves(region, requested.shorter))
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
          if (!bounds.proves(region, requested.shorter))
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
