#pragma once

#include "gti/semantic_core.h"

namespace lang {

namespace lifetime_requirement_detail {
inline bool validFormulaShape(const LifetimeRequirementFormula &formula) {
  using Node = LifetimeRequirementNode;
  using Kind = Node::Kind;
  if (formula.nodes.empty())
    return formula.root == Node::invalidIndex;
  if (formula.nodes.size() > 4096 || formula.root != formula.nodes.size() - 1)
    return false;
  for (std::size_t index = 0; index < formula.nodes.size(); ++index) {
    const auto &node = formula.nodes[index];
    if (node.kind == Kind::All || node.kind == Kind::Any) {
      if (node.left >= index || node.right >= index ||
          node.type != SemanticType::Unknown || node.bound != LifetimeBound{} ||
          node.predicate != Node::invalidIndex || node.semanticTruth)
        return false;
    } else {
      if (node.left != Node::invalidIndex || node.right != Node::invalidIndex)
        return false;
      if (node.kind == Kind::SemanticPredicate) {
        if (node.predicate == Node::invalidIndex ||
            node.type != SemanticType::Unknown || node.bound != LifetimeBound{})
          return false;
      } else if (node.kind == Kind::Region || node.kind == Kind::Type) {
        if (!node.bound.shorter.specified() || node.semanticTruth ||
            node.predicate != Node::invalidIndex ||
            (node.kind == Kind::Region
                 ? !node.bound.longer.specified() || node.type != SemanticType::Unknown
                 : node.bound.longer != Lifetime{} || node.type == SemanticType::Unknown))
          return false;
      } else {
        return false;
      }
    }
  }
  std::vector<bool> reached(formula.nodes.size());
  std::vector<std::size_t> pending{formula.root};
  reached[formula.root] = true;
  while (!pending.empty()) {
    const auto &node = formula.nodes[pending.back()];
    pending.pop_back();
    if (node.kind != Kind::All && node.kind != Kind::Any)
      continue;
    for (const auto child : {node.left, node.right})
      if (!reached[child]) {
        reached[child] = true;
        pending.push_back(child);
      }
  }
  return std::all_of(reached.begin(), reached.end(), [](bool value) { return value; });
}
} // namespace lifetime_requirement_detail

// Composition preserves each complete Boolean subexpression. Region scope
// and predicate authority are checked by the declaration/use-site consumer;
// this operation also supports instantiated local and inference regions.
inline bool appendLifetimeRequirementFormula(
    LifetimeRequirementFormula &destination, const LifetimeRequirementFormula &part) {
  using Node = LifetimeRequirementNode;
  if (!lifetime_requirement_detail::validFormulaShape(destination) ||
      !lifetime_requirement_detail::validFormulaShape(part))
    return false;
  if (part.nodes.empty())
    return true;
  if (destination.nodes.empty()) {
    destination = part;
    return true;
  }
  const auto offset = destination.nodes.size();
  if (offset + part.nodes.size() + 1 > 4096)
    return false;
  for (auto node : part.nodes) {
    if (node.kind == Node::Kind::All || node.kind == Node::Kind::Any) {
      node.left += offset;
      node.right += offset;
    }
    destination.nodes.push_back(std::move(node));
  }
  destination.nodes.push_back({.kind = Node::Kind::All,
                               .left = destination.root, .right = offset + part.root});
  destination.root = destination.nodes.size() - 1;
  return true;
}

// Keep this order identical in semantic formation and independent IR replay.
// The caller owns validation and exact instantiation of the source contract.
inline std::optional<LifetimeRequirementFormula>
lifetimeContractRequirements(const LifetimeSignatureContract &contract) {
  LifetimeRequirementFormula result;
  if (!appendLifetimeRequirementFormula(result, contract.requirements))
    return std::nullopt;
  for (const auto bound : contract.bounds)
    if (!appendLifetimeRequirementFormula(
            result, {.nodes = {{.kind = LifetimeRequirementNode::Kind::Region,
                                 .bound = bound}}, .root = 0}))
      return std::nullopt;
  for (const auto &bound : contract.typeBounds)
    if (!appendLifetimeRequirementFormula(
            result, {.nodes = {{.kind = LifetimeRequirementNode::Kind::Type,
                                 .bound = {{}, bound.region}, .type = bound.type}}, .root = 0}))
      return std::nullopt;
  return result;
}

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
