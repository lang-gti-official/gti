#include "gti/lifetime_requirements.h"

#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  using namespace lang;
  using Kind = LifetimeRequirementNode::Kind;
  const auto a = Lifetime::parameter(1);
  const auto b = Lifetime::parameter(2);
  const auto c = Lifetime::parameter(3);
  const auto d = Lifetime::parameter(4);
  LifetimeSignatureContract contract{.parameters = {a, b, c, d},
                                     .complete = true};
  contract.requirements = {
      .nodes = {{.kind = Kind::Region, .bound = {a, b}},
                {.kind = Kind::Region, .bound = {b, c}},
                {.kind = Kind::All, .left = 0, .right = 1},
                {.kind = Kind::Region, .bound = {a, d}},
                {.kind = Kind::Region, .bound = {d, c}},
                {.kind = Kind::All, .left = 3, .right = 4},
                {.kind = Kind::Any, .left = 2, .right = 5}},
      .root = 6};
  const auto relation = [&](Lifetime longer, Lifetime shorter) {
    return visitLifetimeAssumptionCases(contract, [&](const auto &assumptions) {
      LifetimeBounds bounds;
      for (const auto edge : assumptions.bounds)
        if (!bounds.assume(edge.longer, edge.shorter))
          return false;
      return bounds.proves(longer, shorter);
    });
  };
  auto proof = relation(a, c);
  expect(proof.proof == LifetimeFormulaProof::Proven && proof.cases == 2,
         "independent transitive routes prove their common conclusion");
  expect(relation(a, b).proof == LifetimeFormulaProof::Disproven,
         "one disjunction branch cannot become an unconditional assumption");
  proof = visitLifetimeAssumptionCases(contract, [&](const auto &assumptions) {
    LifetimeBounds bounds;
    for (const auto edge : assumptions.bounds)
      if (!bounds.assume(edge.longer, edge.shorter))
        return false;
    return evaluateLifetimeRequirements(
               contract.requirements,
               [&](const auto &node) {
                 return node.kind == Kind::Region &&
                        bounds.proves(node.bound.longer, node.bound.shorter);
               })
        .value_or(false);
  });
  expect(
      proof.proof == LifetimeFormulaProof::Proven,
      "a disjunctive contract entails itself without granting both branches");
  expect(visitLifetimeAssumptionCases(
             contract, [](const auto &) { return true; }, 1)
                 .proof == LifetimeFormulaProof::Exhausted,
         "exhausted proof budget is never successful evidence");

  auto malformed = contract;
  malformed.requirements.nodes.back().right = 6;
  expect(visitLifetimeAssumptionCases(
             malformed,
             [](const auto &) {
               return true;
             }).proof == LifetimeFormulaProof::Invalid,
         "a cyclic requirement graph is rejected before traversal");
  expect(!evaluateLifetimeRequirements(malformed.requirements,
                                       [](const auto &) { return true; })
              .has_value(),
         "formula evaluation also rejects cyclic or forward edges");

  contract.requirements = {
      .nodes = {{.kind = Kind::SemanticPredicate,
                 .predicate = 0,
                 .semanticTruth = false},
                {.kind = Kind::Region, .bound = {a, b}},
                {.kind = Kind::Any, .left = 0, .right = 1}},
      .root = 2};
  proof = relation(a, b);
  expect(proof.proof == LifetimeFormulaProof::Proven && proof.cases == 1,
         "an impossible concrete predicate branch grants no spurious case");
  contract.requirements.nodes.front().semanticTruth.reset();
  expect(relation(a, b).proof == LifetimeFormulaProof::Disproven,
         "unknown ordinary predicates cannot be assumed false to force a "
         "lifetime branch");
  const auto type = SemanticType::typeParameter(9);
  contract.requirements = {
      .nodes = {{.kind = Kind::Type, .bound = {{}, a}, .type = type},
                {.kind = Kind::Type, .bound = {{}, b}, .type = type},
                {.kind = Kind::Any, .left = 0, .right = 1}},
      .root = 2};
  proof = visitLifetimeAssumptionCases(contract, [&](const auto &assumptions) {
    return assumptions.typeBounds.size() == 1 &&
           assumptions.typeBounds.front().type == type;
  });
  expect(proof.proof == LifetimeFormulaProof::Proven && proof.cases == 2,
         "exact opaque-type premises remain separated by assumption branch");
  return 0;
}
