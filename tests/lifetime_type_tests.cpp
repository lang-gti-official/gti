#include "gti/lifetime_types.h"
#include "gti/borrow_conditions.h"
#include "gti/callable_lifetimes.h"
#include "gti/generic_type_patterns.h"

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

void testInlineBorrowPathDisjointness() {
  using namespace lang;
  const auto root = SemanticType::classType(13, {SemanticType::Int32});
  const auto base = SemanticType::classType(11);
  const auto project = [&](const SemanticType &type, const PlaceProjection &step)
      -> std::optional<SemanticType> {
    if (type == root) {
      if (step == baseProjection(13, 1) || step == fieldProjection(114))
        return base;
      if (step == fieldProjection(113))
        return SemanticType::Int32;
    }
    if (type == base) {
      if (step == fieldProjection(111))
        return SemanticType::Int32;
      if (step == fieldProjection(112))
        return reference(Lifetime::parameter(1));
    }
    return std::nullopt;
  };
  const BorrowPath inherited{baseProjection(13, 1), fieldProjection(111)};
  const BorrowPath direct{fieldProjection(113)};
  const auto disjoint = [&](const SemanticType &type, const BorrowPath &left,
                             const BorrowPath &right) {
    return inlineBorrowPathsDisjoint(type, left, right, project);
  };
  expect(disjoint(root, inherited, direct) && disjoint(root, direct, inherited),
         "exact inherited and direct inline fields are disjoint in either order");
  expect(disjoint(SemanticType::referenceTo(root, AccessMode::Mutable), inherited, direct),
         "a reference root denotes its checked inline referent storage");
  expect(disjoint(root, inherited, {fieldProjection(114), fieldProjection(111)}),
         "equal leaf declarations in separate inline subobjects stay distinct");
  expect(!disjoint(root, inherited, inherited) &&
             !disjoint(root, inherited, {baseProjection(13, 1)}) &&
             !disjoint(root, {}, direct),
         "equal places and ancestor storage remain overlapping");
  expect(!disjoint(root, {baseProjection(13, 1), fieldProjection(112)}, direct) &&
             !disjoint(root, inherited, {fieldProjection(114), fieldProjection(112)}),
         "separate physical fields never prove external reference referents disjoint");
  for (auto malformed : {BorrowPath{baseProjection(12, 1), fieldProjection(111)},
                         BorrowPath{baseProjection(13, 0), fieldProjection(111)},
                         BorrowPath{baseProjection(13, 1), fieldProjection(999)}})
    expect(!disjoint(root, malformed, direct),
           "wrong base owner, interface ordinal or undeclared field cannot prove separation");
  expect(!disjoint(SemanticType::classType(13, {SemanticType::Bool}), inherited, direct),
         "an unavailable concrete nominal view cannot reuse a different instance");
  auto indirect = inherited;
  indirect.push_back({.kind = PlaceProjectionKind::Dereference});
  expect(!disjoint(root, indirect, direct),
         "dereferencing an indirect sibling never grants inline disjointness");
  const auto unavailable = [](const SemanticType &, const PlaceProjection &)
      -> std::optional<SemanticType> { return std::nullopt; };
  expect(!inlineBorrowPathsDisjoint(root, inherited, direct, unavailable),
         "missing inline-layout authority remains conservative");
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
      from.push_back({.conditions = {{.subobject = {fieldProjection(field)},
          .enumeration = enumeration(a), .variant = variant}}});
      to.push_back({.conditions = {{.subobject = {fieldProjection(field)},
          .enumeration = enumeration(field == 10 ? x : y), .variant = variant}}});
    }
  const auto transport = BorrowConditionTransport::between(from, to);
  expect(transport.has_value(), "guard transport derives exact typed paths");
  SemanticBorrowDependencySet source{
      .dependencies = {{.place = {.root = 7}, .designatedFields = {fieldProjection(10), fieldProjection(30)},
          .access = AccessMode::Mutable, .protectsStorage = true, .parent = 9,
          .conditions = from[0].conditions}},
      .possibleAlternatives = {from[0], from[2]}};
  const auto actual = transportBorrowDependencyConditions(source, *transport);
  expect(actual && actual->dependencies.front().place == source.dependencies.front().place &&
             actual->dependencies.front().parent == 9 &&
             actual->dependencies.front().access == AccessMode::Mutable &&
             actual->dependencies.front().protectsStorage &&
             actual->dependencies.front().designatedFields == BorrowPath({fieldProjection(10), fieldProjection(30)}) &&
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
    case 2: broken[0].conditions.front().subobject = {fieldProjection(99)}; break;
    case 3: broken.pop_back(); break;
    case 4: broken[1] = broken[0]; break;
    }
    expect(!BorrowConditionTransport::between(from, broken),
           "guard transport rejects changed declarations, variants, paths and incomplete inventories");
  }
  auto projectedFrom = from;
  auto projectedTo = to;
  for (auto *inventory : {&projectedFrom, &projectedTo})
    for (auto &alternative : *inventory)
      for (auto &condition : alternative.conditions)
        condition.subobject.insert(condition.subobject.begin(),
            {baseProjection(80, 1), constantIndexProjection(2)});
  expect(BorrowConditionTransport::between(projectedFrom, projectedTo).has_value(),
         "guard transport preserves base owner, full ordinal and constant element identity");
  projectedTo.front().conditions.front().subobject.front().index = 0;
  expect(!BorrowConditionTransport::between(projectedFrom, projectedTo),
         "a changed base ordinal cannot reuse another subobject's guard transport");
  projectedTo = projectedFrom;
  projectedFrom.front().conditions.front().subobject.front().baseOwner = 0;
  projectedTo.front().conditions.front().subobject.front().baseOwner = 0;
  expect(!BorrowConditionTransport::between(projectedFrom, projectedTo),
         "matching malformed Base steps cannot establish typed guard transport");
  source.dependencies.front().conditions.front().enumeration = enumeration(x);
  expect(!transportBorrowDependencyConditions(source, *transport),
         "a stale or forged source guard cannot become target evidence");
  FunctionBorrowDependencySummary summary{
      .dependencies = {{.resultFields = {fieldProjection(30)}, .origin = BorrowOriginKind::Argument,
                        .parameter = 2, .sourceFields = {fieldProjection(40)}, .access = AccessMode::Mutable,
                        .resultConditions = from[0].conditions,
                        .sourceConditions = from[2].conditions}},
      .possibleAlternatives = {from[0]}};
  const auto instantiated = instantiateBorrowSummaryLifetimes(
      summary, std::array{LifetimeBinding{a, x}});
  expect(instantiated && instantiated->dependencies.front().parameter == 2 &&
             instantiated->dependencies.front().resultFields == BorrowPath{fieldProjection(30)} &&
             instantiated->dependencies.front().sourceFields == BorrowPath{fieldProjection(40)} &&
             instantiated->dependencies.front().sourceConditions.front().enumeration == enumeration(x) &&
             instantiated->dependencies.front().resultConditions.front().enumeration == enumeration(x) &&
             instantiated->possibleAlternatives.front().conditions.front().enumeration == enumeration(x),
         "canonical summary instantiation preserves suppliers and all typed guards");
}

void testBorrowAlternativeCoverage() {
  using namespace lang;
  const auto outer = SemanticType::enumType(70);
  const auto inner = SemanticType::enumType(71);
  const BorrowDependencyCondition first{.subobject = {fieldProjection(10)}, .enumeration = outer, .variant = 0};
  const BorrowDependencyCondition empty{.subobject = {fieldProjection(10)}, .enumeration = outer, .variant = 1};
  const BorrowDependencyCondition nested{.subobject = {fieldProjection(10), fieldProjection(11)}, .enumeration = inner, .variant = 0};
  const BorrowDependencyCondition nestedEmpty{.subobject = {fieldProjection(10), fieldProjection(11)}, .enumeration = inner, .variant = 1};
  const BorrowDependencyCondition sibling{.subobject = {fieldProjection(20)}, .enumeration = outer, .variant = 0};
  const BorrowDependencyCondition siblingEmpty{.subobject = {fieldProjection(20)}, .enumeration = outer, .variant = 1};
  const std::vector<BorrowAlternativePath> inventory{
      {{first}}, {{empty}}, {{first, nested}}, {{first, nestedEmpty}},
      {{sibling}}, {{siblingEmpty}}};
  const auto covered = [&](std::vector<BorrowAlternativePath> selected) {
    return hasCompleteBorrowAlternativeCoverage(inventory, selected);
  };
  expect(covered({{{empty}}, {{siblingEmpty}}}),
         "explicit empty siblings require no invented borrowed origin");
  expect(covered({{{first}}, {{first, nestedEmpty}}, {{sibling}}}),
         "each active nested domain and independent sibling is represented");
  expect(!covered({{{empty}}}),
         "one empty field cannot conceal missing evidence for its sibling");
  expect(!covered({{{first}}, {{sibling}}}),
         "an active outer alternative requires its nested enum domain");
  expect(!covered({}), "missing alternatives do not certify an empty value");
  auto forged = sibling;
  forged.enumeration = inner;
  expect(!covered({{{empty}}, {{forged}}}),
         "the same field path with another enum identity cannot supply a domain");
  expect(hasCompleteBorrowAlternativeCoverage({}, {}),
         "an authoritative unconditional inventory has an empty alternative set");
}

void testCallablePatternCaptures() {
  using namespace lang;
  const auto a = Lifetime::parameter(801);
  const auto b = Lifetime::parameter(802);
  auto actual = SemanticType::functionSignature(reference(a), {reference(a), reference(b)});
  actual.quantifiedLifetimes = {a.id, b.id, 803};
  const auto r = SemanticType::typeParameter(11);
  const auto args = SemanticType::typePack(12);
  const std::array parameters{GenericParameterIdentity{.id = 11},
                            GenericParameterIdentity{.id = 12, .pack = true}};
  const std::array primary{SemanticType::functionSignature(r, {args})};
  const std::array values{actual};
  const auto captured = deriveGenericArgumentPatternSubstitution(primary, values, parameters, {});
  expect(captured && captured->types.empty() && captured->callableScopes.size() == 1,
         "invocation-bound projections stay out of the ordinary generic type map");
  const auto &scope = captured->callableScopes.front();
  expect(scope.argumentPath == std::vector<std::size_t>{0} &&
             scope.signature == actual && scope.signature.quantifiedLifetimes.size() == 3 &&
             scope.capturedTypes.at(11) == reference(a) &&
             scope.capturedTypes.at(12).arguments ==
                 std::vector<SemanticType>{reference(a), reference(b)},
         "scoped projection retains repeated, independent and unused invocation binders");
  GenericSubstitution raw;
  raw.types.emplace(11, reference(a));
  raw.types.emplace(12, SemanticType::concreteTypePack(12, {reference(a), reference(b)}));
  expect(deriveGenericArgumentPatternSubstitution(primary, values, parameters, {}, raw) == captured,
         "independent raw projections are rederived under the complete callable scope");
  const auto rebuild = [](const SemanticType &type, const GenericSubstitution &substitution) {
    return substituteScopedGenericType(type, substitution,
        [](const auto &value) { return value; },
        [](SemanticType value) { return value; });
  };
  expect(rebuild(primary.front(), *captured) == actual,
         "reconstructed callable preserves its full invocation scope and argument correlation");
  expect(!rebuild(r, *captured) && !rebuild(SemanticType::classType(1, {r}), *captured),
         "a projected invocation result cannot become free field or alias storage");
  auto imported = *captured;
  imported.callableScopes.front().imports = {
      {a.id, Lifetime::parameter(901)}, {b.id, Lifetime::parameter(902)},
      {803, Lifetime::parameter(903)}};
  expect(rebuild(r, imported) == reference(Lifetime::parameter(901)) &&
             rebuild(primary.front(), imported) == actual,
         "member imports open fresh universals without changing the stored callable scope");
  auto nested = SemanticType::functionSignature(SemanticType::Void, {primary.front()});
  const auto nestedRebuilt = rebuild(nested, *captured);
  expect(nestedRebuilt && nestedRebuilt->quantifiedLifetimes.empty() &&
             nestedRebuilt->arguments.back() == actual,
         "nested invocation binders stay inside their original signature boundary");
  auto renamed = actual;
  renamed.quantifiedLifetimes = {811, 812, 813};
  renamed.arguments[0].referenceLifetime = Lifetime::parameter(811);
  renamed.arguments[1].referenceLifetime = Lifetime::parameter(811);
  renamed.arguments[2].referenceLifetime = Lifetime::parameter(812);
  const auto alpha = deriveGenericArgumentPatternSubstitution(primary, std::array{renamed}, parameters, {});
  expect(alpha && *alpha != *captured && equivalentGenericSubstitutions(*alpha, *captured),
         "proof-local alpha identities retain exact captured scope equality without aliasing raw cache keys");
  const auto repeatedCapture = deriveGenericArgumentPatternSubstitution(
      std::array{primary.front(), primary.front()}, std::array{actual, renamed}, parameters, {});
  expect(repeatedCapture && rebuild(primary.front(), *repeatedCapture) == actual,
         "repeated pattern participants agree under independent alpha scopes");
  auto invalid = *captured;
  invalid.valid = false;
  expect(!rebuild(primary.front(), invalid) &&
             !deriveGenericArgumentPatternSubstitution(primary, values, parameters, {}, invalid),
         "invalid selected-family evidence cannot become an identity substitution");

  const std::array projected{GenericParameterIdentity{.id = 13, .lifetime = true}};
  auto shell = SemanticType::referenceTo(r, AccessMode::ReadOnly);
  shell.referenceLifetime = Lifetime::parameter(13);
  const std::array referencePattern{SemanticType::functionSignature(shell, {args})};
  const auto refCapture = deriveGenericArgumentPatternSubstitution(
      referencePattern, values, parameters, projected);
  expect(refCapture && refCapture->types.at(11) == SemanticType::Int32 &&
             refCapture->lifetimes.empty() &&
             refCapture->callableScopes.front().regionBindings ==
                 std::vector<LifetimeBinding>{{Lifetime::parameter(13), a}},
         "a named result-shell projection remains inside the matched invocation scope");
  const auto repeatedReference = deriveGenericArgumentPatternSubstitution(
      std::array{referencePattern.front(), referencePattern.front()},
      std::array{actual, renamed}, parameters, projected);
  expect(repeatedReference && rebuild(referencePattern.front(), *repeatedReference) == actual,
         "named projected regions choose the active alpha scope when a recipe is repeated");
  const std::array repeated{SemanticType::functionSignature(shell, {shell})};
  auto unequal = SemanticType::functionSignature(reference(a), {reference(b)});
  unequal.quantifiedLifetimes = {a.id, b.id};
  expect(!deriveGenericArgumentPatternSubstitution(repeated, std::array{unequal}, parameters, projected),
         "a repeated projected region cannot match independent invocation regions");
  expect(deriveGenericArgumentPatternSubstitution(repeated, std::array{unequal}, parameters,
              projected, {}, GenericPatternMatch::Structure).has_value(),
         "failed region formation cannot change structural family selection");
  auto fixed = shell;
  fixed.referenceLifetime = Lifetime::staticLifetime();
  expect(!deriveGenericArgumentPatternSubstitution(
             std::array{SemanticType::functionSignature(fixed, {args})}, values, parameters, {}),
         "fixed static requirements are not inferred invocation slots");
  auto malformed = actual;
  malformed.quantifiedLifetimes.push_back(a.id);
  expect(!deriveGenericArgumentPatternSubstitution(primary, std::array{malformed}, parameters, {}),
         "duplicate invocation binder inventory is invalid capture evidence");
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
  for (const auto receiver : {ReceiverMutability::Mutable, ReceiverMutability::Consuming}) {
    auto qualified = signature;
    qualified.signatureReceiver = receiver;
    expect(callableLifetimeContract(qualified, formation) == contract,
           "general callable formation preserves lifetime premises for mutable and consuming interfaces");
    GenericParameterId local = 1200;
    const auto proof = makeCallableLifetimeSignature(qualified, formation,
        [&] { return Lifetime::parameter(local++); });
    expect(proof && proof->signature.signatureReceiver == receiver &&
               callableLifetimeCarrierMatches(*proof, qualified, formation),
           "qualified callable formation retains the exact receiver qualifier");
    auto unqualified = qualified;
    unqualified.signatureReceiver = ReceiverMutability::ReadOnly;
    expect(proof && !callableLifetimeCarrierMatches(*proof, unqualified, formation),
           "a callable carrier cannot change its checked receiver qualifier");
    qualified.kind = SemanticType::NativeFunction;
    expect(!callableLifetimeContract(qualified, formation),
           "native ABI signatures cannot forge a receiver qualifier");
  }
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
  testInlineBorrowPathDisjointness();
  testBorrowConditionTransport();
  testBorrowAlternativeCoverage();
  testCallablePatternCaptures();
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
