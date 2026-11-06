#include "gti/lifetime_types.h"
#include "gti/borrow_conditions.h"
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

void testBorrowConditionTransport() {
  using namespace lang;
  const auto a = Lifetime::parameter(501);
  const auto x = Lifetime::inference(601);
  const auto y = Lifetime::inference(602);
  const auto enumeration = [](Lifetime region) {
    auto type = SemanticType::enumType(51);
    type.lifetimeArguments = {region};
    return type;
  };
  std::vector<BorrowAlternativePath> from;
  std::vector<BorrowAlternativePath> to;
  for (const SymbolId field : {SymbolId{10}, SymbolId{20}})
    for (std::size_t variant = 0; variant != 2; ++variant) {
      from.push_back({.conditions = {{.subobject = {field},
          .enumeration = enumeration(a), .variant = variant}}});
      to.push_back({.conditions = {{.subobject = {field},
          .enumeration = enumeration(field == 10 ? x : y), .variant = variant}}});
    }
  const auto transport = BorrowConditionTransport::between(from, to);
  expect(transport.has_value(), "guard transport derives exact typed paths");
  SemanticBorrowDependencySet source{
      .dependencies = {{.place = {.root = 7}, .designatedFields = {10, 30},
          .access = AccessMode::Mutable, .protectsStorage = true, .parent = 9,
          .conditions = from[0].conditions}},
      .possibleAlternatives = {from[0], from[2]}};
  const auto actual = transportBorrowDependencyConditions(source, *transport);
  expect(actual && actual->dependencies.front().place == source.dependencies.front().place &&
             actual->dependencies.front().parent == 9 &&
             actual->dependencies.front().access == AccessMode::Mutable &&
             actual->dependencies.front().protectsStorage &&
             actual->dependencies.front().designatedFields == std::vector<SymbolId>({10, 30}) &&
             actual->dependencies.front().conditions == to[0].conditions &&
             actual->possibleAlternatives == std::vector<BorrowAlternativePath>({to[0], to[2]}),
         "one source region maps by field path while every origin and active subset stays exact");
  auto reordered = to;
  std::reverse(reordered.begin(), reordered.end());
  expect(BorrowConditionTransport::between(from, reordered).has_value(),
         "typed inventory order does not change guard identity");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto broken = to;
    switch (mutation) {
    case 0: broken[0].conditions.front().enumeration.enumId = 52; break;
    case 1: broken[0].conditions.front().variant = 9; break;
    case 2: broken[0].conditions.front().subobject = {99}; break;
    case 3: broken.pop_back(); break;
    case 4: broken[1] = broken[0]; break;
    }
    expect(!BorrowConditionTransport::between(from, broken),
           "guard transport rejects changed declarations, variants, paths and incomplete inventories");
  }
  source.dependencies.front().conditions.front().enumeration = enumeration(x);
  expect(!transportBorrowDependencyConditions(source, *transport),
         "a stale or forged source guard cannot become target evidence");
  FunctionBorrowDependencySummary summary{
      .dependencies = {{.resultFields = {30}, .origin = BorrowOriginKind::Argument,
                        .parameter = 2, .sourceFields = {40}, .access = AccessMode::Mutable,
                        .resultConditions = from[0].conditions,
                        .sourceConditions = from[2].conditions}},
      .possibleAlternatives = {from[0]}};
  const auto instantiated = instantiateBorrowSummaryLifetimes(
      summary, std::array{LifetimeBinding{a, x}});
  expect(instantiated && instantiated->dependencies.front().parameter == 2 &&
             instantiated->dependencies.front().resultFields == std::vector<SymbolId>{30} &&
             instantiated->dependencies.front().sourceFields == std::vector<SymbolId>{40} &&
             instantiated->dependencies.front().sourceConditions.front().enumeration == enumeration(x) &&
             instantiated->dependencies.front().resultConditions.front().enumeration == enumeration(x) &&
             instantiated->possibleAlternatives.front().conditions.front().enumeration == enumeration(x),
         "canonical summary instantiation preserves suppliers and all typed guards");
}

void testLifetimeConversionShapes() {
  using namespace lang;
  const auto a = Lifetime::parameter(301);
  const auto b = Lifetime::parameter(302);
  const auto x = Lifetime::parameter(401);
  const auto y = Lifetime::parameter(402);
  expect(sameLifetimeConversionShape(reference(a), reference(Lifetime::local(9))) &&
             !equivalentLifetimeTypes(reference(a), reference(Lifetime::local(9))),
         "free region shape admission is not a semantic conversion proof");
  expect(!sameLifetimeConversionShape(reference(a), reference(a, AccessMode::Mutable)) &&
             !sameLifetimeConversionShape(reference(a), reference({})) &&
             !sameLifetimeConversionShape(SemanticType::Unknown, SemanticType::Unknown),
         "conversion shape preserves access, region-slot presence, and known types");
  auto first = SemanticType::functionSignature(reference(a), {reference(a), reference(b)});
  first.quantifiedLifetimes = {a.id};
  auto second = SemanticType::functionSignature(reference(x), {reference(x), reference(y)});
  second.quantifiedLifetimes = {x.id};
  expect(sameLifetimeConversionShape(first, second),
         "bound callable scopes compare by alpha identity while free regions may differ");
  auto captured = second;
  captured.arguments.back() = reference(x);
  expect(!sameLifetimeConversionShape(first, captured),
         "a free environment region cannot become an invocation binder");
  auto unrelated = second;
  unrelated.quantifiedLifetimes.push_back(y.id);
  expect(!sameLifetimeConversionShape(first, unrelated),
         "conversion shape preserves callable quantifier count");
  auto altered = SemanticType::functionSignature(reference(y), {reference(x), reference(b)});
  altered.quantifiedLifetimes = {x.id, y.id};
  auto correlated = SemanticType::functionSignature(reference(a), {reference(a), reference(b)});
  correlated.quantifiedLifetimes = {a.id, b.id};
  expect(!sameLifetimeConversionShape(correlated, altered),
         "conversion shape cannot weaken a quantified input/result correlation");
  auto nominal = SemanticType::classType(71, {reference(a)});
  auto changed = SemanticType::classType(72, {reference(x)});
  expect(!sameLifetimeConversionShape(nominal, changed),
         "region shape admission never substitutes a different nominal declaration");
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

void testNativeInvocationScopes() {
  using namespace lang;
  const auto input = Lifetime::parameter(4001);
  auto text = SemanticType(SemanticType::StringView);
  text.lifetimeArguments = {input};
  auto native = SemanticType::nativeFunctionType(SemanticType::Int32, std::array{text});
  native.quantifiedLifetimes = {input.id};
  const auto contract = callableLifetimeContract(native);
  expect(contract && contract->parameters == std::vector{input} &&
             freeTypeLifetimes(native).empty(),
         "a native pointer scopes its invocation lifetimes separately from pointer storage");
  auto returned = SemanticType::functionSignature(native, {});
  const auto outer = callableLifetimeContract(returned);
  expect(outer && outer->parameters.empty() && outer->typeBounds.empty(),
         "returning a native invocation pointer does not expose its input universals");
  GenericParameterId fresh = 4100;
  const auto proof = makeCallableLifetimeSignature(native, {},
      [&] { return Lifetime::parameter(fresh++); });
  expect(proof && proof->signature.kind == SemanticType::NativeFunction &&
             proof->signatureBindings.empty() &&
             callableLifetimeCarrierMatches(*proof, native),
         "native callable proof preserves ABI kind and exact quantifier scope");
  if (!contract)
    return;
  auto staticText = text;
  staticText.lifetimeArguments = {Lifetime::staticLifetime()};
  LifetimeSignatureContract staticTarget;
  staticTarget.complete = true;
  expect(!callableLifetimeTargetBindings(SemanticType::Int32, std::array{staticText},
                                         staticTarget, native),
         "a fixed-static callback cannot implement a universally callable text input");
  auto renamed = native;
  renamed.quantifiedLifetimes = {4200};
  renamed.arguments[1].lifetimeArguments = {Lifetime::parameter(4200)};
  expect(equivalentLifetimeTypes(native, renamed),
         "native invocation binder spelling is alpha-equivalent");
  renamed.arguments[1].lifetimeArguments = {input};
  expect(!equivalentLifetimeTypes(native, renamed),
         "native invocation types reject an escaped binder occurrence");
}

int main() {
  testBorrowConditionTransport();
  testLifetimeConversionShapes();
  using namespace lang;
  testScopedCallableFormation();
  testContextualLifetimeInference();
  testNativeInvocationScopes();
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
