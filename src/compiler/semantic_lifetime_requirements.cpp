#include "semantic_analyzer_impl.h"
#include "gti/lifetime_types.h"
#include "gti/lifetime_requirements.h"
#include <array>

namespace lang {

auto SemanticVisitor::Impl::forEveryLifetimeAssumption(
    const std::function<bool(const LifetimeSignatureContract &)> &prove) const
    -> bool {
  LifetimeSignatureContract contract{.complete = true};
  if (loanFlow.lifetimeContract.valid())
    contract = loanFlow.lifetimeContract;
  else if (const auto *function = currentFunctionInfo();
           function && function->lifetimeContract.valid())
    contract = function->lifetimeContract;
  // Enclosing class/concept scopes may add premises to a local declaration.
  // Keep their Boolean structure; collecting each leaf would assume both
  // sides of an OR. A duplicate declaration formula needs no second expansion.
  std::vector<LifetimeRequirementFormula> seen{contract.requirements};
  for (const auto &scope : requirementScopes) {
    const auto formula = buildLifetimeRequirements(scope, false);
    if (!formula)
      return false;
    if (formula->nodes.empty() ||
        std::find(seen.begin(), seen.end(), *formula) != seen.end())
      continue;
    seen.push_back(*formula);
    const auto offset = contract.requirements.nodes.size();
    if (offset + formula->nodes.size() + 1 > 4096)
      return false;
    for (auto node : formula->nodes) {
      if (node.kind == LifetimeRequirementNode::Kind::All ||
          node.kind == LifetimeRequirementNode::Kind::Any) {
        node.left += offset;
        node.right += offset;
      }
      contract.requirements.nodes.push_back(std::move(node));
    }
    const auto root = offset + formula->root;
    if (contract.requirements.root == LifetimeRequirementNode::invalidIndex) {
      contract.requirements.root = root;
    } else {
      contract.requirements.nodes.push_back(
          {.kind = LifetimeRequirementNode::Kind::All,
           .left = contract.requirements.root, .right = root});
      contract.requirements.root = contract.requirements.nodes.size() - 1;
    }
  }
  return visitLifetimeAssumptionCases(contract, prove).proof ==
         LifetimeFormulaProof::Proven;
}

auto SemanticVisitor::Impl::validateLifetimeConstraints(
    std::span<const LifetimeBound> additional,
    std::span<const LifetimeBound> prove) const -> bool {
  return forEveryLifetimeAssumption([&](const auto &assumptions) {
    LifetimeInferenceConstraints graph(currentLifetimeBounds(&assumptions));
    for (const auto edges : {std::span<const LifetimeBound>(loanFlow.lifetimeObligations),
                             additional})
      for (const auto edge : edges)
        if (!graph.require(edge.longer, edge.shorter))
          return false;
    const auto provesAll = [&](const auto &checked) {
      return !checked.failure() &&
             std::all_of(prove.begin(), prove.end(), [&](const auto edge) {
               return checked.proves(edge.longer, edge.shorter);
             });
    };
    if (provesAll(graph))
      return true;
    if (!loanFlow.collectingLifetimeContract)
      return false;
    auto proposed = currentLifetimeBounds(&assumptions);
    for (const auto edge : graph.fixedRequirements()) {
      if (!representableInferredLifetimeBound(loanFlow.lifetimeContract, edge) ||
          !proposed.assume(edge.longer, edge.shorter))
        return false;
    }
    LifetimeInferenceConstraints provisional(std::move(proposed));
    for (const auto edge : graph.obligations())
      if (!provisional.require(edge.longer, edge.shorter))
        return false;
    // This is representability only. Selected applications and non-call
    // writes retain obligations; speculative candidates publish nothing.
    return provesAll(provisional);
  });
}

auto SemanticVisitor::Impl::buildLifetimeRequirements(
    std::span<const AppliedConceptRequirement> requirements,
    bool evaluatePredicates) const
    -> std::optional<LifetimeRequirementFormula> {
  using Node = LifetimeRequirementNode;
  using Kind = Node::Kind;
  struct Part {
    std::size_t root = Node::invalidIndex;
    bool lifetime = false;
  };
  LifetimeRequirementFormula result;
  bool valid = true;
  std::size_t predicate = 0;
  std::vector<ConceptId> expanding;
  const auto append = [&](Node node) {
    if (result.nodes.size() >= 4096) {
      valid = false;
      return Node::invalidIndex;
    }
    result.nodes.push_back(std::move(node));
    return result.nodes.size() - 1;
  };
  const auto combine = [&](Part left, Part right, Kind kind) {
    if (left.root == Node::invalidIndex)
      return right;
    if (right.root == Node::invalidIndex)
      return left;
    return Part{append({.kind = kind, .left = left.root, .right = right.root}),
                left.lifetime || right.lifetime};
  };
  std::function<Part(std::span<const AppliedConceptRequirement>, bool)> build;
  std::function<Part(const AppliedConceptRequirement &)> leaf;
  leaf = [&](const AppliedConceptRequirement &requirement) -> Part {
    if (requirement.lifetimeBound)
      return {append({.kind = requirement.outlivesType ? Kind::Type : Kind::Region,
                      .bound = *requirement.lifetimeBound,
                      .type = requirement.outlivesType.value_or(SemanticType::Unknown)}),
              true};
    if (requirement.conceptId && requirement.conceptId <= concepts.size()) {
      const auto &definition = concepts[requirement.conceptId - 1];
      if (!definition.protocol && definition.definitionExpression &&
          !definition.definitionRequirements.empty()) {
        if (expanding.size() >= 64 ||
            std::find(expanding.begin(), expanding.end(), requirement.conceptId) !=
                expanding.end()) {
          valid = false;
          return {};
        }
        expanding.push_back(requirement.conceptId);
        const auto substitution = conceptSubstitution(
            definition, requirement.arguments, requirement.lifetimeArguments);
        std::vector<AppliedConceptRequirement> expanded;
        for (const auto &pattern : definition.definitionRequirements) {
          AppliedConceptRequirement item{
              .conceptId = pattern.conceptId,
              .syntax = pattern.syntax,
              .lifetimeArguments = pattern.lifetimeArguments,
              .expression = definition.definitionExpression,
              .expressionApplication = pattern.expressionApplication,
              .lookupScope = definition.namespaceScope,
              .lifetimeBound = pattern.lifetimeBound,
              .outlivesType = pattern.outlivesType};
          item = substituteRequirement(item, substitution);
          for (const auto &argument : pattern.arguments) {
            if (!argument.parameter)
              item.arguments.push_back(substituteType(argument.concrete, substitution));
            else if (*argument.parameter < requirement.arguments.size())
              item.arguments.push_back(requirement.arguments[*argument.parameter]);
            else
              valid = false;
          }
          expanded.push_back(std::move(item));
        }
        const auto part = build(expanded, false);
        expanding.pop_back();
        return part;
      }
    }
    std::optional<bool> truth;
    if (evaluatePredicates &&
        std::none_of(requirement.arguments.begin(), requirement.arguments.end(),
                     containsOpenTypeParameter))
      truth = !firstConceptApplicationFailure(requirement).has_value();
    return {append({.kind = Kind::SemanticPredicate, .predicate = predicate++,
                    .semanticTruth = truth}), false};
  };
  build = [&](std::span<const AppliedConceptRequirement> items,
               bool relevantOnly) -> Part {
    Part whole;
    std::vector<const ConstraintExpression *> seen;
    for (const auto &requirement : items) {
      const auto first = result.nodes.size();
      Part part;
      if (!requirement.expression) {
        part = leaf(requirement);
      } else {
        const auto *expression = requirement.expression;
        if (std::find(seen.begin(), seen.end(), expression) != seen.end())
          continue;
        seen.push_back(expression);
        std::vector<Part> translated;
        for (const auto &node : expression->nodes) {
          if (node.kind == ConstraintExpressionKind::Application) {
            const auto found = std::find_if(items.begin(), items.end(), [&](const auto &item) {
              return item.expression == expression &&
                     item.expressionApplication == node.application;
            });
            if (found == items.end()) {
              valid = false;
              return {};
            }
            translated.push_back(leaf(*found));
          } else if (node.left < translated.size() && node.right < translated.size()) {
            translated.push_back(combine(
                translated[node.left], translated[node.right],
                node.kind == ConstraintExpressionKind::Conjunction ? Kind::All : Kind::Any));
          } else {
            valid = false;
            return {};
          }
        }
        if (expression->root >= translated.size()) {
          valid = false;
          return {};
        }
        part = translated[expression->root];
      }
      if (relevantOnly && !part.lifetime) {
        result.nodes.resize(first);
        continue;
      }
      whole = combine(whole, part, Kind::All);
    }
    return whole;
  };
  const auto whole = build(requirements, true);
  if (!valid)
    return std::nullopt;
  result.root = whole.root;
  return result;
}

auto SemanticVisitor::Impl::applyLifetimeRequirements(
    const LifetimeRequirementFormula &requirements,
    SemanticLifetimeApplication &application, const Token &site, bool diagnose)
    -> bool {
  if (requirements.nodes.empty())
    return requirements.root == LifetimeRequirementNode::invalidIndex;
  using Node = LifetimeRequirementNode;
  using Kind = Node::Kind;
  // A declaration can already entail a disjunctive target without entailing
  // any one target leaf unconditionally. Prove the formula per caller case
  // and publish no branch-specific edge as a global inference obligation.
  const bool entailed = forEveryLifetimeAssumption([&](const auto &assumptions) {
    LifetimeInferenceConstraints graph(currentLifetimeBounds(&assumptions));
    for (const auto edges : {
             std::span<const LifetimeBound>(loanFlow.lifetimeObligations),
             std::span<const LifetimeBound>(application.obligations)})
      for (const auto edge : edges)
        if (!graph.require(edge.longer, edge.shorter))
          return false;
    if (graph.failure())
      return false;
    const auto size = loanFlow.lifetimeObligations.size();
    loanFlow.lifetimeObligations.insert(loanFlow.lifetimeObligations.end(),
                                        application.obligations.begin(), application.obligations.end());
    const auto value = evaluateLifetimeRequirements(requirements, [&](const auto &node) {
      if (node.kind == Kind::Region)
        return graph.proves(node.bound.longer, node.bound.shorter);
      return !lifetimeRequirementFailure(
          {.lifetimeBound = node.bound, .outlivesType = node.type}, &assumptions);
    });
    loanFlow.lifetimeObligations.resize(size);
    return value.value_or(false);
  });
  if (entailed)
    return true;
  struct State {
    std::vector<LifetimeBound> edges;
    std::vector<LifetimeTypeBound> types;
  };
  std::size_t steps = 0;
  bool exhausted = false;
  const auto append = [](auto &values, const auto &value) {
    if (std::find(values.begin(), values.end(), value) == values.end())
      values.push_back(value);
  };
  const auto consistent = [&](const State &state) {
    auto edges = application.obligations;
    edges.insert(edges.end(), state.edges.begin(), state.edges.end());
    return validateLifetimeConstraints(edges);
  };
  const auto typeSatisfied = [&](const LifetimeTypeBound &bound, const State &state) {
    const auto size = loanFlow.lifetimeObligations.size();
    loanFlow.lifetimeObligations.insert(loanFlow.lifetimeObligations.end(),
                                        application.obligations.begin(), application.obligations.end());
    loanFlow.lifetimeObligations.insert(loanFlow.lifetimeObligations.end(),
                                        state.edges.begin(), state.edges.end());
    const bool satisfied = !lifetimeRequirementFailure(
        {.lifetimeBound = LifetimeBound{{}, bound.region}, .outlivesType = bound.type});
    bool representable = false;
    if (!satisfied && loanFlow.collectingLifetimeContract) {
      SemanticLifetimeBody proposal{.obligations = loanFlow.lifetimeObligations,
                                     .typeObligations = {bound}};
      representable = projectInferredLifetimeContract(loanFlow.lifetimeContract,
                                                       proposal).has_value();
    }
    loanFlow.lifetimeObligations.resize(size);
    return satisfied || representable;
  };
  using Continuation = std::function<bool(State)>;
  std::function<bool(std::size_t, State, const Continuation &)> solve;
  solve = [&](std::size_t index, State state, const Continuation &next) -> bool {
    if (++steps > 4096) {
      exhausted = true;
      return false;
    }
    if (index >= requirements.nodes.size())
      return false;
    const auto &node = requirements.nodes[index];
    if (node.kind == Kind::All) {
      if (node.left >= index || node.right >= index)
        return false;
      return solve(node.left, std::move(state), [&](State left) {
        return solve(node.right, std::move(left), next);
      });
    }
    if (node.kind == Kind::Any) {
      if (node.left >= index || node.right >= index)
        return false;
      return solve(node.left, state, next) || solve(node.right, std::move(state), next);
    }
    if (node.kind == Kind::SemanticPredicate)
      return node.semanticTruth.value_or(false) && next(std::move(state));
    if (node.kind == Kind::Region) {
      append(state.edges, node.bound);
      return consistent(state) && next(std::move(state));
    }
    if (node.kind != Kind::Type)
      return false;
    const LifetimeTypeBound bound{node.type, node.bound.shorter};
    append(state.types, bound);
    if (typeSatisfied(bound, state))
      return next(std::move(state));
    // An exact opaque-type premise may constrain an inferred destination;
    // it never lets T:a prove an unrelated associated projection outlives a.
    std::vector<LifetimeTypeBound> known = loanFlow.lifetimeContract.typeBounds;
    if (const auto *function = currentFunctionInfo())
      for (const auto &item : function->lifetimeContract.typeBounds)
        append(known, item);
    for (const auto &item : known)
      if (equivalentLifetimeTypes(item.type, bound.type)) {
        auto alternative = state;
        append(alternative.edges, LifetimeBound{item.region, bound.region});
        if (consistent(alternative) && typeSatisfied(bound, alternative) && next(std::move(alternative)))
          return true;
      }
    auto structural = state;
    for (const auto region : freeTypeLifetimes(bound.type))
      append(structural.edges, LifetimeBound{region, bound.region});
    return consistent(structural) && typeSatisfied(bound, structural) && next(std::move(structural));
  };
  const bool accepted = solve(requirements.root, {}, [&](State state) {
    if (!consistent(state))
      return false;
    for (const auto &bound : state.types)
      if (!typeSatisfied(bound, state))
        return false;
    for (const auto edge : state.edges)
      append(application.obligations, edge);
    for (const auto &bound : state.types)
      append(application.typeObligations, bound);
    return true;
  });
  if (!accepted && diagnose)
    report(site, exhausted
        ? "Lifetime requirement inference exceeded its bounded branch budget; simplify or name the required relationship."
        : "No branch of the declared lifetime requirements can be proved for this call.", "GTI-S2127");
  return accepted;
}

} // namespace lang
