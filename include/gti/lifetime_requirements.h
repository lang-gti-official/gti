#pragma once

#include "gti/semantic_core.h"

namespace lang {

enum class LifetimeFormulaProof { Proven, Disproven, Invalid, Exhausted };

struct LifetimeFormulaProofResult {
  LifetimeFormulaProof proof = LifetimeFormulaProof::Invalid;
  std::size_t steps = 0;
  std::size_t cases = 0;
};

// Prove a property under every feasible alternative of a declaration's
// assumptions. Choosing one alternative would be unsound: A or B permits a
// call when either is true, while its implementation must work in both cases.
// Unknown ordinary predicates remain feasible but grant no region/type facts.
// Semantic predicate authority stays with the concrete semantic instance.
template <class Prove>
[[nodiscard]] LifetimeFormulaProofResult visitLifetimeAssumptionCases(
    const LifetimeSignatureContract &declaration, Prove prove,
    std::size_t budget = 4096) {
  LifetimeFormulaProofResult result;
  if (!declaration.valid())
    return result;
  using Node = LifetimeRequirementNode;
  using Kind = Node::Kind;
  using Contract = LifetimeSignatureContract;
  result.proof = LifetimeFormulaProof::Proven;
  const auto step = [&] {
    if (result.steps == budget) {
      result.proof = LifetimeFormulaProof::Exhausted;
      return false;
    }
    ++result.steps;
    return true;
  };
  const auto append = [](auto &values, const auto &value) {
    if (std::find(values.begin(), values.end(), value) == values.end())
      values.push_back(value);
  };
  using Continuation = std::function<bool(Contract)>;
  std::function<bool(std::size_t, Contract, std::size_t, const Continuation &)> visit;
  visit = [&](std::size_t index, Contract assumptions, std::size_t depth,
              const Continuation &next) -> bool {
    if (!step())
      return false;
    if (depth > 256) {
      result.proof = LifetimeFormulaProof::Exhausted;
      return false;
    }
    const auto &node = declaration.requirements.nodes[index];
    if (node.kind == Kind::All)
      return visit(node.left, std::move(assumptions), depth + 1,
          [&](Contract left) {
            return visit(node.right, std::move(left), depth + 1, next);
          });
    if (node.kind == Kind::Any)
      return visit(node.left, assumptions, depth + 1, next) &&
             visit(node.right, std::move(assumptions), depth + 1, next);
    if (node.kind == Kind::SemanticPredicate && node.semanticTruth == false)
      return true; // No admitted caller can enter this alternative.
    if (node.kind == Kind::Region)
      append(assumptions.bounds, node.bound);
    else if (node.kind == Kind::Type)
      append(assumptions.typeBounds, LifetimeTypeBound{node.type, node.bound.shorter});
    return next(std::move(assumptions));
  };
  Contract assumptions = declaration;
  assumptions.requirements = {};
  const Continuation finish = [&](Contract current) {
    if (!step())
      return false;
    ++result.cases;
    if (!prove(current)) {
      result.proof = LifetimeFormulaProof::Disproven;
      return false;
    }
    return true;
  };
  if (declaration.requirements.nodes.empty())
    static_cast<void>(finish(std::move(assumptions)));
  else
    static_cast<void>(visit(declaration.requirements.root,
                           std::move(assumptions), 0, finish));
  return result;
}

// Evaluate a required formula against independently established facts. Only
// region/type leaf proof is supplied by the consumer; ordinary predicate truth
// is the exact semantic target's stored fact, never supplied by a call site.
template <class ProveLeaf>
[[nodiscard]] std::optional<bool> evaluateLifetimeRequirements(
    const LifetimeRequirementFormula &formula, ProveLeaf proveLeaf) {
  using Node = LifetimeRequirementNode;
  using Kind = Node::Kind;
  if (formula.nodes.empty())
    return formula.root == Node::invalidIndex ? std::optional<bool>{true}
                                              : std::nullopt;
  if (formula.nodes.size() > 4096 || formula.root != formula.nodes.size() - 1)
    return std::nullopt;
  std::vector<bool> values;
  for (std::size_t index = 0; index < formula.nodes.size(); ++index) {
    const auto &node = formula.nodes[index];
    bool value = false;
    if (node.kind == Kind::All || node.kind == Kind::Any) {
      if (node.left >= index || node.right >= index || node.bound != LifetimeBound{} ||
          node.type != SemanticType::Unknown || node.predicate != Node::invalidIndex ||
          node.semanticTruth)
        return std::nullopt;
      value = node.kind == Kind::All ? values[node.left] && values[node.right]
                                    : values[node.left] || values[node.right];
    } else {
      if (node.left != Node::invalidIndex || node.right != Node::invalidIndex)
        return std::nullopt;
      if (node.kind == Kind::SemanticPredicate) {
        if (node.predicate == Node::invalidIndex || node.bound != LifetimeBound{} ||
            node.type != SemanticType::Unknown)
          return std::nullopt;
        value = node.semanticTruth.value_or(false);
      } else if (node.kind == Kind::Region || node.kind == Kind::Type) {
        if (node.predicate != Node::invalidIndex || node.semanticTruth ||
            !node.bound.shorter.specified() ||
            (node.kind == Kind::Region
                 ? !node.bound.longer.specified() || node.type != SemanticType::Unknown
                 : node.bound.longer != Lifetime{} || node.type == SemanticType::Unknown))
          return std::nullopt;
        value = proveLeaf(node);
      } else {
        return std::nullopt;
      }
    }
    values.push_back(value);
  }
  return values[formula.root];
}

} // namespace lang
