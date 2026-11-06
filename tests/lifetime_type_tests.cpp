#include "gti/lifetime_types.h"
#include "gti/callable_lifetimes.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
lang::SemanticType reference(lang::Lifetime region,
                             lang::AccessMode access = lang::AccessMode::ReadOnly) {
  auto type = lang::SemanticType::referenceTo(lang::SemanticType::Int32, access);
  type.referenceLifetime = region;
  return type;
}

void testScopedCallableFormation() {
  using namespace lang;
  using Node = LifetimeRequirementNode;
  const auto invocation = Lifetime::parameter(201);
  const auto environment = Lifetime::parameter(202);
  const auto foreign = Lifetime::parameter(203);
  auto holder = SemanticType::classType(41, {SemanticType::typeParameter(301)});
  holder.lifetimeArguments = {invocation};
  const CallableNominalFormation formation = [](const SemanticType &type)
      -> std::optional<LifetimeRequirementFormula> {
    if (type.classId == 42)
      return LifetimeRequirementFormula{}; // A phantom type argument has no stored premise.
    if (type.classId != 41 || type.arguments.size() != 1 || type.lifetimeArguments.size() != 1)
      return std::nullopt;
    return LifetimeRequirementFormula{
        .nodes = {{.kind = Node::Kind::Type, .bound = {{}, type.lifetimeArguments.front()},
                   .type = type.arguments.front()}}, .root = 0};
  };
  auto signature = SemanticType::functionSignature(reference(environment), {holder});
  signature.quantifiedLifetimes = {invocation.id};
  const auto contract = callableLifetimeContract(signature, formation);
  expect(contract && contract->parameters == std::vector{invocation, environment} &&
             contract->requirements.nodes.size() == 1 &&
             contract->requirements.nodes.front().bound.shorter == invocation,
         "a callable owns exact nominal formation premises in its invocation scope");
  expect(!callableLifetimeContract(signature),
         "nominal callable formation requires authoritative declaration lookup");
  auto carrier = SemanticType::classType(42, {signature});
  const auto outer = SemanticType::functionSignature(SemanticType::Void, {carrier});
  const auto outerContract = callableLifetimeContract(outer, formation);
  expect(outerContract && outerContract->parameters == std::vector{environment} &&
             outerContract->typeBounds.empty() && outerContract->requirements.nodes.empty(),
         "nested invocation premises and quantifiers do not leak through a phantom carrier");
  expect(!callableLifetimeContract(outer, [](const SemanticType &type)
             -> std::optional<LifetimeRequirementFormula> {
               return type.classId == 42 ? std::optional{LifetimeRequirementFormula{}} : std::nullopt;
             }),
         "an unresolved nominal recipe inside a nested invocation is rejected");
  expect(!callableLifetimeContract(signature, [&](const SemanticType &)
             -> std::optional<LifetimeRequirementFormula> {
               return LifetimeRequirementFormula{
                   .nodes = {{.kind = Node::Kind::Type, .bound = {{}, invocation},
                              .type = reference(foreign)}}, .root = 0};
             }),
         "a nominal formation recipe cannot introduce a foreign region into a scoped contract");
  const LifetimeRequirementFormula either{
      .nodes = {{.kind = Node::Kind::Region, .bound = {invocation, environment}},
                 {.kind = Node::Kind::Region, .bound = {environment, invocation}},
                 {.kind = Node::Kind::Any, .left = 0, .right = 1}}, .root = 2};
  const auto disjunctive = callableLifetimeContract(signature,
      [&](const SemanticType &) -> std::optional<LifetimeRequirementFormula> { return either; });
  expect(disjunctive && disjunctive->bounds.empty() && disjunctive->requirements == either,
         "explicit nominal relations survive even when no stored field independently implies them");
  CallableLifetimeSignature proof{.signature = signature, .contract = *contract,
                                  .signatureBindings = {{environment, Lifetime::local(88)}}};
  const auto actual = substituteTypeLifetimes(signature, proof.signatureBindings);
  expect(actual && callableLifetimeCarrierMatches(proof, *actual, formation),
         "fixed local environment choices preserve the scoped nominal contract");
  proof.contract.requirements = {};
  expect(!callableLifetimeCarrierMatches(proof, *actual, formation),
         "removing a nominal formation premise invalidates callable metadata");
  GenericParameterId fresh = 1001;
  const auto canonical = makeCallableLifetimeSignature(*actual, formation,
      [&] { return Lifetime::parameter(fresh++); });
  expect(canonical && canonical->signature.quantifiedLifetimes == signature.quantifiedLifetimes &&
             canonical->signatureBindings.size() == 1 &&
             canonical->signatureBindings.front().second == Lifetime::local(88),
         "fixed carrier choices are universalized separately from invocation quantifiers");
  const auto staticSignature = SemanticType::functionSignature(
      reference(Lifetime::staticLifetime()), {reference(Lifetime::staticLifetime())});
  const auto staticProof = makeCallableLifetimeSignature(staticSignature, {},
      [&] { return Lifetime::parameter(fresh++); });
  expect(staticProof && staticProof->signature == staticSignature &&
             staticProof->signatureBindings.empty() && fresh == 1002 &&
             callableLifetimeCarrierMatches(*staticProof, staticSignature),
         "static is a fixed constant rather than a supplied carrier parameter");
  auto malformed = either;
  malformed.nodes.push_back({.kind = Node::Kind::Region, .bound = {invocation, environment}});
  malformed.root = malformed.nodes.size() - 1;
  LifetimeRequirementFormula destination = either;
  expect(!appendLifetimeRequirementFormula(destination, malformed) && destination == either,
         "formula composition rejects dangling nodes without changing checked facts");
}
} // namespace

void testContextualLifetimeInference() {
  using namespace lang;
  const auto hole = Lifetime::inference(1001);
  const auto fixed = Lifetime::parameter(1002);
  const auto actual = Lifetime::parameter(1003);
  const std::array holes{hole};
  auto pattern = SemanticType::enumType(31, {reference(hole), reference(fixed)}, {}, {hole});
  auto target = SemanticType::enumType(31, {reference(actual), reference(fixed)}, {}, {actual});
  pattern.valueArguments = target.valueArguments = {CompileTimeValue::uint64(3)};
  expect(matchInferredTypeLifetimes(pattern, target, holes) ==
             std::optional{std::vector<LifetimeBinding>{{hole, actual}}},
         "contextual inference matches repeated syntax holes without changing fixed regions");
  auto mismatch = target;
  mismatch.arguments.back().referenceLifetime = actual;
  expect(!matchInferredTypeLifetimes(pattern, mismatch, holes),
         "contextual inference preserves explicitly named regions");
  mismatch = target;
  mismatch.arguments.front().referenceAccess = AccessMode::Mutable;
  expect(!matchInferredTypeLifetimes(pattern, mismatch, holes),
         "contextual inference preserves reference access");
  mismatch = target;
  mismatch.arguments.front().arguments.front() = SemanticType::Bool;
  expect(!matchInferredTypeLifetimes(pattern, mismatch, holes),
         "contextual inference preserves ordinary type arguments");
  mismatch = target;
  mismatch.valueArguments.front() = CompileTimeValue::uint64(4);
  expect(!matchInferredTypeLifetimes(pattern, mismatch, holes),
         "contextual inference preserves ordinary value arguments");
  mismatch = target;
  mismatch.lifetimeArguments.front() = fixed;
  expect(!matchInferredTypeLifetimes(pattern, mismatch, holes),
         "a repeated syntax hole must have one exact contextual region");
  expect(!matchUniversalTypeLifetimes(pattern, target, holes) &&
             !matchInferredTypeLifetimes(pattern, target, std::array{fixed}),
         "universal and inference slot inventories remain distinct");
  auto leftSignature = SemanticType::functionSignature(reference(Lifetime::parameter(1010)),
      {reference(Lifetime::parameter(1010)), reference(hole)});
  leftSignature.quantifiedLifetimes = {1010};
  auto rightSignature = SemanticType::functionSignature(reference(Lifetime::parameter(1020)),
      {reference(Lifetime::parameter(1020)), reference(actual)});
  rightSignature.quantifiedLifetimes = {1020};
  expect(matchInferredTypeLifetimes(leftSignature, rightSignature, holes).has_value(),
         "contextual inference retains nested quantified alpha scope");
  rightSignature.arguments.back().referenceLifetime = Lifetime::parameter(1020);
  expect(!matchInferredTypeLifetimes(leftSignature, rightSignature, holes),
         "a contextual hole cannot capture a nested quantified lifetime");
}

int main() {
  using namespace lang;
  testScopedCallableFormation();
  testContextualLifetimeInference();
  const auto a = Lifetime::parameter(1);
  const auto b = Lifetime::parameter(2);
  const auto x = Lifetime::parameter(31);
  const auto y = Lifetime::parameter(32);
  const std::array renamed{LifetimeBinding{a, x}, LifetimeBinding{b, y}};
  const auto first = SemanticType::functionSignature(reference(a),
                                                     {reference(a), reference(b)});
  const auto second = SemanticType::functionSignature(reference(x),
                                                      {reference(x), reference(y)});
  expect(equivalentLifetimeTypes(first, second, renamed),
         "alpha-renaming preserves the complete signature relationship");
  auto different = second;
  different.arguments.front().referenceLifetime = y;
  expect(!equivalentLifetimeTypes(first, different, renamed),
         "renaming cannot change which input supplies the result");
  expect(!equivalentLifetimeTypes(reference(a), reference(x)),
         "unrelated free universals cannot be silently unified");
  expect(!equivalentLifetimeTypes(reference(a), reference(a, AccessMode::Mutable)),
         "equal regions never erase access permissions");
  expect(!equivalentLifetimeTypes(reference(a), reference(Lifetime::inference(1))),
         "inference variables cannot masquerade as universal binders");
  const std::array collapsed{LifetimeBinding{a, x}, LifetimeBinding{b, x}};
  expect(!equivalentLifetimeTypes(first, second, collapsed),
         "alpha-equivalence requires a bijection");

  auto higherRanked = SemanticType::functionSignature(reference(a),
                                                      {reference(a), reference(b)});
  higherRanked.quantifiedLifetimes = {a.id};
  auto renamedHigher = SemanticType::functionSignature(reference(x),
                                                       {reference(x), reference(b)});
  renamedHigher.quantifiedLifetimes = {x.id};
  expect(equivalentLifetimeTypes(higherRanked, renamedHigher),
         "higher-ranked binders are scoped and alpha-equivalent");
  expect(freeTypeLifetimes(higherRanked) == std::vector<Lifetime>{b},
         "quantified callback inputs are not free caller regions");
  const std::array instantiate{LifetimeBinding{a, Lifetime::staticLifetime()},
                               LifetimeBinding{b, y}};
  const auto substituted = substituteTypeLifetimes(higherRanked, instantiate);
  expect(substituted && substituted->arguments.front().referenceLifetime == a &&
             substituted->arguments.back().referenceLifetime == y,
         "substitution preserves a callback's universality and replaces free regions");
  const std::array capture{LifetimeBinding{b, a}};
  expect(!substituteTypeLifetimes(higherRanked, capture),
         "a substitution cannot capture an outer region in a callback binder");

  auto nominal = SemanticType::classType(5, {reference(a)});
  nominal.lifetimeArguments = {b};
  nominal.lambdaEnclosingClassTypes = {reference(b)};
  nominal.lambdaEnclosingFunctionTypes = {higherRanked};
  const auto transformed = substituteTypeLifetimes(nominal, renamed);
  expect(transformed && transformed->lifetimeArguments == std::vector<Lifetime>{y} &&
             transformed->arguments.front().referenceLifetime == x &&
             transformed->lambdaEnclosingClassTypes.front().referenceLifetime == y &&
             transformed->lambdaEnclosingFunctionTypes.front()
                     .arguments.front().referenceLifetime == a,
         "nested nominal and closure substitutions retain independent regions");
  expect(eraseTypeLifetimes(first) == eraseTypeLifetimes(second),
         "region renaming cannot multiply executable signatures");
  expect(eraseTypeLifetimes(reference(a)) !=
             eraseTypeLifetimes(reference(a, AccessMode::Mutable)),
         "representation erasure changes only region metadata");
  expect(freeTypeLifetimes(eraseTypeLifetimes(nominal)).empty(),
         "representation erasure includes all nested type vectors");

  LifetimeSignatureContract contract{.parameters = {a, b},
                                      .bounds = {{a, b}},
                                      .receiver = a,
                                      .origin = LifetimeContractOrigin::Declared,
                                      .complete = true};
  expect(contract.valid(), "a complete declared region interface is valid");
  contract.bounds.push_back({b, Lifetime::inference(7)});
  expect(!contract.valid(), "public contracts cannot publish local inference holes");
  contract.bounds.pop_back();
  contract.typeBounds.push_back({reference(a), b});
  contract.requirements.nodes = {
      {.kind = LifetimeRequirementNode::Kind::Region, .bound = {a, b}},
      {.kind = LifetimeRequirementNode::Kind::Type, .bound = {{}, b}, .type = reference(a)},
      {.kind = LifetimeRequirementNode::Kind::Any, .left = 0, .right = 1}};
  contract.requirements.root = 2;
  const auto renamedContract = substituteLifetimeContract(contract, renamed);
  expect(renamedContract && renamedContract->valid() &&
             renamedContract->parameters == std::vector<Lifetime>{x, y} &&
             renamedContract->receiver == x &&
             renamedContract->bounds == std::vector<LifetimeBound>{{x, y}} &&
             renamedContract->typeBounds.front().type == reference(x) &&
             renamedContract->typeBounds.front().region == y &&
             renamedContract->requirements.nodes[0].bound == LifetimeBound{x, y} &&
             renamedContract->requirements.nodes[1].type == reference(x) &&
             renamedContract->requirements.nodes[1].bound.shorter == y,
         "projected instance renaming preserves all contract binders and Boolean requirements");
  const std::array patternArguments{reference(a), reference(a), reference(b)};
  const std::array actualArguments{reference(x), reference(x), reference(y)};
  const std::array patternRegions{a, b};
  const auto projected = matchUniversalTypeArgumentLifetimes(
      patternArguments, actualArguments, patternRegions);
  expect(projected && *projected == std::vector<LifetimeBinding>(renamed.begin(), renamed.end()),
         "argument-pattern matching preserves repeated region identities across arguments");
  auto mismatchedArguments = actualArguments;
  mismatchedArguments[1] = reference(y);
  expect(!matchUniversalTypeArgumentLifetimes(patternArguments, mismatchedArguments, patternRegions),
         "a repeated projected binder cannot match unrelated actual regions");
  const std::array fixedPattern{reference(Lifetime::staticLifetime())};
  const std::array nonstaticArgument{reference(x)};
  expect(!matchUniversalTypeArgumentLifetimes(fixedPattern, nonstaticArgument, {}),
         "fixed pattern lifetimes are obligations after structural selection");
  contract.parameters.push_back(a);
  expect(!contract.valid(), "duplicate region identities are malformed metadata");
  contract.parameters.pop_back();
  contract.receiver = x;
  expect(!contract.valid(), "receiver identity must be declared by its contract");
  return 0;
}
