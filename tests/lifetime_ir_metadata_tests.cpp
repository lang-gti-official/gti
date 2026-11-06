#include "../src/compiler/mir_lifetimes.h"
#include "gti/mir_printer.h"
#include "gti/mir_effects.h"
#include "gti/callable_lifetimes.h"
#include "gti/nominal_variance.h"

#include <cstdlib>
#include <iostream>

namespace lang {
struct MirProgramTestAccess {
  static auto &classes(MirProgram &program) { return program.classes; }
  static auto &payloads(MirProgram &program) { return program.payloadEnums_; }
  static auto &integrals(MirProgram &program) { return program.integralEnums_; }
  static auto &lambdas(MirProgram &program) { return program.lambdas; }
};
} // namespace lang
using namespace lang;
namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
SemanticType reference(GenericParameterId id) {
  auto type = SemanticType::referenceTo(SemanticType(SemanticType::Int32));
  type.referenceLifetime = Lifetime::parameter(id);
  return type;
}
void cleanupLifetimeRecipes() {
  const auto region = Lifetime::parameter(801);
  const auto actualRegion = Lifetime::local(901);
  const auto instantiate = [&](const SemanticType &type) {
    return *substituteTypeLifetimes(type, std::array{LifetimeBinding{region, actualRegion}});
  };
  MirProgram program;
  MirClassInstance leaf;
  leaf.id = 1;
  leaf.declaration = 11;
  leaf.type = SemanticType::classType(11);
  leaf.type.lifetimeArguments = {region};
  leaf.lifetimeContract = {.parameters = {region}, .complete = true};
  leaf.destructor = 1;
  leaf.destructorStatus = SpecialMemberStatus::Declared;
  leaf.requiresActiveDropState = true;
  leaf.requiresActiveCleanup = true;
  leaf.declaredFields = {{.symbol = 101, .type = reference(801)}};
  MirClassInstance outer;
  outer.id = 2;
  outer.declaration = 12;
  outer.type = SemanticType::classType(12);
  outer.type.lifetimeArguments = {region};
  outer.lifetimeContract = leaf.lifetimeContract;
  outer.requiresActiveCleanup = true;
  outer.declaredFields = {{.symbol = 102,
      .type = SemanticType::uniqueOwnerOf(leaf.type), .requiresActiveCleanup = true}};
  outer.bases = {{.instance = leaf.id, .type = leaf.type}};
  MirProgramTestAccess::classes(program) = {leaf, outer};
  const auto actualLeaf = instantiate(leaf.type);
  const auto actualOuter = instantiate(outer.type);
  const auto record = mirUniqueCleanupType(program, actualOuter);
  expect(record && record->type == actualOuter && record->classInstance == 2 &&
             record->aggregate.size() == 2 &&
             record->aggregate[0].type == SemanticType::uniqueOwnerOf(actualLeaf) &&
             record->aggregate[0].uniqueOwnerField == 102 &&
             record->aggregate[0].uniquePointeeClass == 1 &&
             record->aggregate[1].type == actualLeaf &&
             record->aggregate[1].uniqueOwnerBase == 1,
         "cleanup substitutes exact field and base regions while retaining physical declaration IDs");
  const auto arrayType = SemanticType::arrayOf(actualOuter, std::size_t{2});
  const auto array = mirUniqueCleanupType(program, arrayType);
  expect(array && array->fixedArray && array->fixedArray->leafType == actualOuter &&
             array->aggregate.size() == 1 && array->aggregate.front() == *record,
         "array cleanup retains its actual leaf recipe and exact child lifetime types");
  MirPayloadEnum payload;
  payload.declaration = 21;
  payload.type = SemanticType::enumType(21);
  payload.type.lifetimeArguments = {region};
  payload.lifetimeContract = leaf.lifetimeContract;
  payload.requiresActiveCleanup = true;
  payload.variants = {{.variantIndex = 0, .payloadTypes = {outer.type},
                       .payloadSymbols = {103}, .payloadRequiresActiveCleanup = {true}}};
  MirProgramTestAccess::payloads(program) = {payload};
  const auto actualPayload = instantiate(payload.type);
  const auto enumeration = mirUniqueCleanupType(program, actualPayload);
  expect(enumeration && enumeration->type == actualPayload &&
             enumeration->aggregate.size() == 1 &&
             enumeration->aggregate.front().type == actualOuter &&
             enumeration->aggregate.front().payloadField == 0,
         "enum cleanup instantiates the actual payload recipe");
  expect(!mirUniqueCleanupType(program, SemanticType::uniqueOwnerOf(actualPayload)) &&
             !mirUniqueCleanupType(program, SemanticType::uniqueOwnerOf(SemanticType::enumType(99))),
         "a failed payload view must never be treated as an integral enum cleanup");
  MirIntegralEnum integral;
  integral.declaration = 22;
  integral.type = SemanticType::enumType(22);
  integral.underlyingType = SemanticType::Int32;
  MirProgramTestAccess::integrals(program) = {integral};
  expect(mirUniqueCleanupType(program, SemanticType::uniqueOwnerOf(integral.type)).has_value(),
         "a known integral enum retains its supported owner cleanup");
  MirLambdaInstance closure;
  closure.id = 1;
  closure.declaration = 31;
  closure.environmentLifetimes = {region};
  closure.type = SemanticType::lambdaType(31, SemanticType::Int32, {}, std::array{outer.type});
  closure.type.lifetimeArguments = {region};
  closure.returnType = SemanticType::Int32;
  closure.lifetimeContract = leaf.lifetimeContract;
  closure.captureTypes = {outer.type};
  closure.captureRequiresActiveCleanup = {true};
  MirProgramTestAccess::lambdas(program) = {closure};
  const auto actualClosure = instantiate(closure.type);
  const auto capture = mirUniqueCleanupType(program, actualClosure);
  expect(capture && capture->type == actualClosure && capture->lambdaInstance == 1 &&
             capture->aggregate.size() == 1 &&
             capture->aggregate.front().type == actualOuter &&
             capture->aggregate.front().captureIndex == 0,
         "closure cleanup substitutes its fixed environment without changing invocation binders");
  MirProgramTestAccess::classes(program).push_back(leaf);
  expect(!mirUniqueCleanupType(program, actualLeaf),
         "ambiguous physical instances cannot select a cleanup target by erased type equality");
}
} // namespace
int main() {
  cleanupLifetimeRecipes();
  const auto fixedEnvironment = Lifetime::parameter(701);
  const std::array closureParameters{reference(702)};
  const std::array closureCaptures{reference(701)};
  MirLambdaInstance closure{
      .id = 1, .declaration = 70, .environmentLifetimes = {fixedEnvironment},
      .type = SemanticType::lambdaType(70, reference(702), closureParameters, closureCaptures),
      .returnType = reference(702), .parameterTypes = {reference(702)},
      .lifetimeContract = {.parameters = {fixedEnvironment, Lifetime::parameter(702)}, .complete = true},
      .captureTypes = {reference(701)}};
  closure.type.lifetimeArguments = {fixedEnvironment};
  closure.type.quantifiedLifetimes = {702};
  const auto actualClosure = substituteTypeLifetimes(
      closure.type, std::array{LifetimeBinding{fixedEnvironment, Lifetime::staticLifetime()}});
  expect(actualClosure && mirLambdaLifetimeView(closure, *actualClosure),
         "closure environment choices instantiate every occurrence while invocation stays quantified");
  auto changedCapture = *actualClosure;
  changedCapture.arguments.back().referenceLifetime = Lifetime::local(71);
  expect(!mirLambdaLifetimeView(closure, changedCapture),
         "closure matching rejects a capture inconsistent with its fixed environment slot");
  auto changedInvocation = *actualClosure;
  changedInvocation.arguments.front().referenceLifetime = Lifetime::staticLifetime();
  expect(!mirLambdaLifetimeView(closure, changedInvocation),
         "a fixed environment choice cannot specialize a quantified invocation result");
  auto quantified = SemanticType::functionSignature(reference(101), {reference(101)});
  quantified.quantifiedLifetimes = {101};
  const auto quantifiedContract = callableLifetimeContract(quantified);
  expect(quantifiedContract && quantifiedContract->parameters ==
                                  std::vector<Lifetime>{Lifetime::parameter(101)} &&
             quantifiedContract->typeBounds.size() == 1,
         "callable contracts derive scoped universal inputs and exact referent bounds");
  CallableLifetimeSignature quantifiedProof{.signature = quantified,
                                             .contract = *quantifiedContract};
  auto alpha = SemanticType::functionSignature(reference(202), {reference(202)});
  alpha.quantifiedLifetimes = {202};
  expect(callableLifetimeCarrierMatches(quantifiedProof, alpha),
         "higher-ranked carrier identities compare by scoped alpha equivalence");
  quantifiedProof.signatureBindings = {{Lifetime::parameter(101), Lifetime::staticLifetime()}};
  expect(!callableLifetimeCarrierMatches(quantifiedProof, alpha),
         "fixed carrier substitutions cannot instantiate a quantified binder");
  auto fixedSignature = SemanticType::functionSignature(reference(303), {reference(303)});
  CallableLifetimeSignature fixedProof{
      .signature = fixedSignature,
      .contract = *callableLifetimeContract(fixedSignature),
      .signatureBindings = {{Lifetime::parameter(303), Lifetime::local(404)}}};
  const auto fixedCarrier = substituteTypeLifetimes(fixedSignature, fixedProof.signatureBindings);
  expect(fixedCarrier && callableLifetimeCarrierMatches(fixedProof, *fixedCarrier),
         "a universal carrier slot reconstructs its exact local region without publishing it");
  fixedProof.signatureBindings[0].second = Lifetime::local(405);
  expect(!callableLifetimeCarrierMatches(fixedProof, *fixedCarrier),
         "changing a fixed carrier region fails exact signature identity");
  fixedProof.signatureBindings[0].second = Lifetime::local(404);
  fixedProof.contract.typeBounds.clear();
  expect(!callableLifetimeCarrierMatches(fixedProof, *fixedCarrier),
         "a complete flag cannot replace the derived callable type-outlives obligations");
  const auto targetBindings = callableLifetimeTargetBindings(
      reference(303), std::array{reference(303)},
      *callableLifetimeContract(fixedSignature), quantified);
  expect(targetBindings && *targetBindings ==
              std::vector<LifetimeBinding>{{Lifetime::parameter(303), Lifetime::parameter(101)}},
         "a universal target may implement an alpha-renamed higher-ranked signature");
  auto requested = SemanticType::functionSignature(reference(101), {reference(101), reference(102)});
  requested.quantifiedLifetimes = {101, 102};
  CallableLifetimeSignature requestedProof{
      .signature = requested, .contract = *callableLifetimeContract(requested)};
  auto constrainedSignature = SemanticType::functionSignature(reference(303), {reference(303), reference(304)});
  auto constrained = *callableLifetimeContract(constrainedSignature);
  using RequirementKind = LifetimeRequirementNode::Kind;
  constrained.requirements.nodes = {
      {.kind = RequirementKind::Region,
       .bound = {Lifetime::parameter(303), Lifetime::parameter(304)}},
      {.kind = RequirementKind::Region,
       .bound = {Lifetime::parameter(304), Lifetime::parameter(303)}},
      {.kind = RequirementKind::Any, .left = 0, .right = 1}};
  constrained.requirements.root = 2;
  MirProgram callableProgram;
  expect(!mirCallableLifetimeTargetMatches(callableProgram, requestedProof, reference(303),
                                           std::array{reference(303), reference(304)}, constrained),
         "a target disjunction cannot disappear behind empty guaranteed bounds");
  requested.arguments[2] = reference(101);
  requestedProof = {.signature = requested, .contract = *callableLifetimeContract(requested)};
  expect(mirCallableLifetimeTargetMatches(callableProgram, requestedProof, reference(303),
                                          std::array{reference(303), reference(304)}, constrained),
         "a proved disjunct suffices without assuming every leaf");
  constrained.requirements.nodes[0] = {.kind = RequirementKind::SemanticPredicate, .predicate = 0};
  expect(!mirCallableLifetimeTargetMatches(callableProgram, requestedProof, reference(303),
                                           std::array{reference(303), reference(304)}, constrained),
         "unresolved target predicate truth cannot reach executable proof");
  NominalVarianceRecipe shared{
      .declaration = 1, .parameters = {{.id = 501}, {.id = 502, .lifetime = true}},
      .stored = {{.symbol = 601, .type = SemanticType::referenceTo(
          SemanticType::typeParameter(501), AccessMode::ReadOnly, Lifetime::parameter(502))}},
      .complete = true};
  auto exclusive = shared;
  exclusive.declaration = 2;
  exclusive.stored[0].type.referenceAccess = AccessMode::Mutable;
  const auto inferred = inferNominalVarianceRecipes(std::array{shared, exclusive});
  expect(inferred && (*inferred)[0].types == std::vector{TypeVariance::Covariant} &&
             (*inferred)[1].types == std::vector{TypeVariance::Invariant} &&
             (*inferred)[1].lifetimes == std::vector{TypeVariance::Covariant},
         "shared symbolic recipes distinguish mutable referents from covariant borrow regions");
  auto specialization = exclusive;
  specialization.exactSpecializationPrimary = 1;
  specialization.parameters.erase(specialization.parameters.begin());
  specialization.exactTypeArguments = {SemanticType::Int32};
  specialization.stored[0].type = SemanticType::rawPointerTo(reference(502));
  const auto family = inferNominalVarianceRecipes(std::array{shared, specialization});
  expect(family && (*family)[0].lifetimes == std::vector{TypeVariance::Invariant} &&
             (*family)[1].types == std::vector{TypeVariance::Invariant},
         "an exact specialization retains public type slots and constrains family lifetime variance");
  auto specializedType = SemanticType::classType(2, {reference(701)});
  specializedType.lifetimeArguments = {Lifetime::parameter(702)};
  const auto expandedSpec = expandNominalVarianceRecipe(specializedType, specialization,
      (*family)[1], std::array{shared, specialization});
  expect(expandedSpec.complete && expandedSpec.types.size() == 1 &&
             expandedSpec.lifetimes.size() == 1,
         "specialized public arguments remain visible to nested-region variance proof");
  specialization.parameters.push_back(specialization.parameters.front());
  expect(!inferNominalVarianceRecipes(std::array{shared, specialization}),
         "duplicate symbolic parameter identities cannot enter variance proof");
  NominalVarianceRecipe projected{
      .declaration = 3,
      .argumentPatterns = {SemanticType::referenceTo(
          SemanticType::typeParameter(801), AccessMode::ReadOnly,
          Lifetime::parameter(802))},
      .argumentPatternLifetimes = {{.id = 802, .lifetime = true, .implicit = true}},
      .parameters = {{.id = 801, .typeArgumentPath = {0, 0}}},
      .stored = {{.symbol = 803, .type = SemanticType::rawPointerTo(reference(802))}},
      .complete = true};
  const auto projectedVariance = inferNominalVarianceRecipes(std::array{projected});
  expect(projectedVariance && projectedVariance->front().types ==
             std::vector{TypeVariance::Invariant} &&
             projectedVariance->front().lifetimes.empty(),
         "projected pattern regions do not add top-level arity or grant unchecked variance");
  auto namedProjected = projected;
  namedProjected.argumentPatternLifetimes.front().implicit = false;
  const auto namedVariance = inferNominalVarianceRecipes(std::array{namedProjected});
  expect(namedVariance && *namedVariance == *projectedVariance,
         "named pattern binders retain the same exact scope and public arity as anonymous slots");
  auto unboundNamedProjection = namedProjected;
  unboundNamedProjection.argumentPatternLifetimes.front().id = 804;
  expect(!inferNominalVarianceRecipes(std::array{unboundNamedProjection}),
         "a named pattern binder must occur exactly in its declared recipe scope");
  const std::array projectedPatterns{reference(802), reference(802)};
  const std::array projectedActual{reference(901), reference(901)};
  const std::array projectedRegions{Lifetime::parameter(802)};
  const auto projectedBindings = matchUniversalTypeArgumentLifetimes(
      projectedPatterns, projectedActual, projectedRegions);
  expect(projectedBindings && *projectedBindings == std::vector<LifetimeBinding>{
             {Lifetime::parameter(802), Lifetime::parameter(901)}},
         "a projected region binding must agree across all public type arguments");
  expect(!matchUniversalTypeArgumentLifetimes(
             projectedPatterns, std::array{reference(901), reference(902)}, projectedRegions),
         "a repeated projected binder cannot receive inconsistent actual regions");
  auto duplicateProjected = projected;
  duplicateProjected.argumentPatternLifetimes.push_back(
      duplicateProjected.argumentPatternLifetimes.front());
  expect(!inferNominalVarianceRecipes(std::array{duplicateProjected}),
         "duplicate projected lifetime binder identity is invalid");
  auto foreignProjected = projected;
  foreignProjected.argumentPatterns.front().referenceLifetime = Lifetime::parameter(804);
  expect(!inferNominalVarianceRecipes(std::array{foreignProjected}),
         "an argument pattern cannot reference an undeclared region binder");
  auto unusedProjected = projected;
  unusedProjected.argumentPatterns.front().referenceLifetime = Lifetime::staticLifetime();
  expect(!inferNominalVarianceRecipes(std::array{unusedProjected}),
         "a projected lifetime binder requires an exact pattern occurrence");
  MirBody body;
  body.returnType = reference(1);
  body.entry = 1;
  MirBlock block;
  block.id = 1;
  MirInstruction call;
  call.id = 1;
  call.kind = MirInstructionKind::Call;
  call.info.type = reference(1);
  call.parameterTypes = {reference(1)};
  call.lifetimeArguments = {Lifetime::parameter(1)};
  call.lifetimeApplication.bindings = {
      {Lifetime::parameter(2), Lifetime::parameter(1)}};
  call.lifetimeApplication.variables = {Lifetime::inference(3)};
  call.lifetimeApplication.obligations = {
      {Lifetime::parameter(1), Lifetime::inference(3)}};
  call.lifetimeApplication.typeObligations = {
      {SemanticType::typeParameter(4), Lifetime::inference(3)}};
  call.lifetimeApplication.origins = {{.region = Lifetime::local(5),
                                       .place = 1,
                                       .key = {.domain = {1, 2, 3}, .root = 6},
                                       .source = 7}};
  call.lifetimeApplication.complete = true;
  block.instructions.push_back(call);
  body.blocks.push_back(block);
  const MirPrinter printer;
  const auto original = printer.print(body);
  expect(
      original.starts_with("mir-body-v94\n"),
      "lifetime-bearing structural metadata requires its own format version");
  auto changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.callableSignature = fixedProof;
  const auto callableBytes = printer.print(changed);
  changed.blocks[0].instructions[0].lifetimeApplication.callableSignature->signatureBindings[0].second =
      Lifetime::local(406);
  expect(printer.print(changed) != callableBytes,
         "fixed callable regions participate in structural MIR identity");
  changed = body;
  changed.returnType.referenceLifetime = Lifetime::staticLifetime();
  expect(
      printer.print(changed) != original,
      "changing a return region must invalidate a structural body fingerprint");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeArguments[0] =
      Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "generic lifetime arguments must participate in structural identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.bindings[0].second =
      Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "changing an actual contract substitution must invalidate the proof "
         "fingerprint");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.bindings[0].first =
      Lifetime::parameter(3);
  expect(printer.print(changed) != original,
         "a contract binding's declaration identity is part of its proof");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.variables[0] =
      Lifetime::inference(8);
  expect(printer.print(changed) != original,
         "inference variable allocation must participate in proof identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.obligations[0].longer =
      Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "required outlives edges must participate in proof identity");
  changed = body;
  changed.blocks[0]
      .instructions[0]
      .lifetimeApplication.typeObligations[0]
      .type = SemanticType::typeParameter(9);
  expect(printer.print(changed) != original,
         "type outlives obligations retain the exact bounded type");
  changed = body;
  changed.blocks[0]
      .instructions[0]
      .lifetimeApplication.typeObligations[0]
      .region = Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "type outlives obligations retain the required region");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.origins[0].region =
      Lifetime::local(10);
  expect(printer.print(changed) != original,
         "local region witnesses retain their checked region identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.origins[0].place = 2;
  expect(printer.print(changed) != original,
         "origin storage identity must participate in proof identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.origins[0].key.root =
      11;
  expect(printer.print(changed) != original,
         "origin semantic place keys must participate in proof identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.origins[0].source = 12;
  expect(printer.print(changed) != original,
         "temporary origin source identity must participate in proof identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeApplication.complete = false;
  expect(printer.print(changed) != original,
         "incomplete lifetime applications cannot reuse checked fingerprints");
  changed = body;
  changed.lifetimeBody.complete = true;
  expect(printer.print(changed) != original,
         "body lifetime proof completeness must participate in fingerprints");
  changed.lifetimeBody.variables = call.lifetimeApplication.variables;
  const auto inventoryOnly = printer.print(changed);
  changed.lifetimeBody.obligations = call.lifetimeApplication.obligations;
  expect(printer.print(changed) != inventoryOnly,
         "body assignment and return constraints must participate in "
         "fingerprints");
  auto callable = SemanticType::functionSignature(reference(4), {reference(4)});
  callable.quantifiedLifetimes = {4};
  body.returnType = callable;
  changed = body;
  changed.returnType.quantifiedLifetimes.clear();
  expect(printer.print(changed) != printer.print(body),
         "a quantified callable and a free-region callable cannot share a "
         "fingerprint");
  SemanticType view(SemanticType::StringView);
  view.lifetimeArguments = {Lifetime::parameter(1)};
  body.returnType = view;
  changed = body;
  changed.returnType.lifetimeArguments = {Lifetime::staticLifetime()};
  expect(
      printer.print(changed) != printer.print(body),
      "borrowing value regions must participate in structural type identity");
  auto projection = SemanticType::typeParameter(0);
  projection.projectionConcept = 77;
  projection.projectionMember = 0;
  projection.arguments = {SemanticType::typeParameter(5)};
  body.returnType = projection;
  changed = body;
  changed.returnType.projectionMember = 1;
  expect(printer.print(changed) != printer.print(body),
         "distinct associated members in exact type bounds need distinct "
         "fingerprints");
  changed = body;
  changed.returnType.projectionConcept = 78;
  expect(printer.print(changed) != printer.print(body),
         "associated concept identity cannot be replaced by its argument "
         "spelling");
  HirLifetimeApplication source;
  source.bindings = call.lifetimeApplication.bindings;
  source.variables = call.lifetimeApplication.variables;
  source.obligations = call.lifetimeApplication.obligations;
  source.typeObligations = call.lifetimeApplication.typeObligations;
  source.complete = true;
  source.origins = {{.region = Lifetime::local(5),
                     .place = {.domain = {1, 2, 3}, .root = 6},
                     .source = 7}};
  auto application = lowerMirLifetimeApplication(source);
  MirBody originBody;
  MirPlace named;
  named.id = 1;
  named.root = MirPlaceRootKind::Binding;
  named.binding = 1;
  named.sourceValue = 7;
  named.key = source.origins[0].place;
  originBody.places.push_back(named);
  expect(resolveMirLifetimeOrigins(application, originBody) &&
             application.origins[0].place == named.id &&
             preservesHirLifetimeApplication(source, application),
         "an exact named origin must survive HIR to MIR mapping");
  HirLifetimeBody bodyFacts{.variables = source.variables,
                            .obligations = source.obligations,
                            .typeObligations = source.typeObligations,
                            .origins = source.origins,
                            .complete = true};
  auto loweredBodyFacts = lowerMirLifetimeBody(bodyFacts);
  expect(resolveMirLifetimeOrigins(loweredBodyFacts, originBody) &&
             preservesHirLifetimeBody(bodyFacts, loweredBodyFacts),
         "whole-body lifetime facts retain their exact mapped origins");
  loweredBodyFacts.typeObligations.clear();
  expect(!preservesHirLifetimeBody(bodyFacts, loweredBodyFacts),
         "whole-body lowering cannot drop exact type obligations");
  auto damaged = application;
  damaged.obligations.clear();
  expect(!preservesHirLifetimeApplication(source, damaged),
         "lowering must not drop a required outlives edge");
  damaged = application;
  damaged.origins[0].source = 8;
  expect(!preservesHirLifetimeApplication(source, damaged),
         "lowering must preserve the origin source exactly");
  auto wrongBody = originBody;
  wrongBody.places[0].key->domain.revision += 1;
  expect(
      !resolveMirLifetimeOrigins(application, wrongBody),
      "equal symbol IDs in a different place domain are not origin evidence");
  source.origins[0].place = {};
  application = lowerMirLifetimeApplication(source);
  expect(!resolveMirLifetimeOrigins(application, originBody),
         "a temporary source cannot be inferred from a named binding");
  originBody.places[0].root = MirPlaceRootKind::Temporary;
  originBody.places[0].key.reset();
  originBody.places[0].temporary = 1;
  expect(resolveMirLifetimeOrigins(application, originBody) &&
             preservesHirLifetimeApplication(source, application),
         "a unique temporary materialization retains its exact source");
  auto secondTemporary = originBody.places[0];
  secondTemporary.id = 2;
  secondTemporary.temporary = 2;
  originBody.places.push_back(secondTemporary);
  expect(!resolveMirLifetimeOrigins(application, originBody),
         "ambiguous temporary materializations must not guess an origin");
  LifetimeSignatureContract contract{
      .parameters = {Lifetime::parameter(1), Lifetime::parameter(2)},
      .bounds = {{Lifetime::parameter(1), Lifetime::parameter(2)}},
      .typeBounds = {{SemanticType::Int32, Lifetime::parameter(2)}},
      .complete = true};
  MirLifetimeApplication applied{
      .bindings = {{Lifetime::parameter(1), Lifetime::parameter(11)},
                   {Lifetime::parameter(2), Lifetime::inference(12)}},
      .variables = {Lifetime::inference(12)},
      .obligations = {{Lifetime::parameter(11), Lifetime::inference(12)}},
      .typeObligations = {{SemanticType::Int32, Lifetime::inference(12)}},
      .complete = true};
  const std::vector<SemanticType> declaredParameters{reference(1)};
  const auto instantiated = instantiateMirLifetimeSignature(
      contract, reference(2), declaredParameters, applied);
  expect(instantiated &&
             instantiated->returnType.referenceLifetime ==
                 Lifetime::inference(12) &&
             instantiated->parameterTypes[0].referenceLifetime ==
                 Lifetime::parameter(11),
         "MIR must instantiate every declared region without changing its "
         "signature shape");
  auto invalidApplication = applied;
  invalidApplication.bindings.pop_back();
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "a missing universal binding is not an instantiated contract");
  invalidApplication = applied;
  invalidApplication.bindings[1].first = Lifetime::parameter(1);
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "duplicate binding keys cannot cover a different universal");
  invalidApplication = applied;
  invalidApplication.bindings[1].first = Lifetime::parameter(3);
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "unknown binding keys cannot belong to a selected declaration");
  invalidApplication = applied;
  invalidApplication.variables.clear();
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "inference IDs need an explicit allocation inventory");
  invalidApplication = applied;
  invalidApplication.obligations.clear();
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "callee bounds remain required obligations after instantiation");
  invalidApplication = applied;
  invalidApplication.typeObligations[0].type = SemanticType::Int64;
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "an exact type bound cannot be replaced by another type's bound");
  invalidApplication = applied;
  invalidApplication.complete = false;
  expect(!instantiateMirLifetimeSignature(
             contract, reference(2), declaredParameters, invalidApplication),
         "incomplete applications cannot authorize lifetime instantiation");
  auto badDeclaration = contract;
  badDeclaration.parameters.pop_back();
  expect(!instantiateMirLifetimeSignature(badDeclaration, reference(2),
                                          declaredParameters, applied),
         "free signature regions require declaration-owned binders");
  auto quantifiedResult =
      SemanticType::functionSignature(reference(1), {reference(13)});
  quantifiedResult.quantifiedLifetimes = {13};
  invalidApplication = applied;
  invalidApplication.bindings[0].second = Lifetime::parameter(13);
  invalidApplication.obligations[0].longer = Lifetime::parameter(13);
  expect(!instantiateMirLifetimeSignature(contract, quantifiedResult,
                                          declaredParameters,
                                          invalidApplication),
         "call substitution cannot capture a higher-ranked binder");
  expect(instantiateMirLifetimeSignature({}, SemanticType::Int32, {}, {})
             .has_value(),
         "region-free legacy signatures retain their exact behavior");
  MirProgram emptyProgram;
  LifetimeSignatureContract caller{
      .parameters = {Lifetime::parameter(21), Lifetime::parameter(22)},
      .complete = true};
  MirBody graphBody;
  graphBody.lifetimeBody = {
      .variables = {Lifetime::inference(23)},
      .obligations = {{Lifetime::parameter(21), Lifetime::inference(23)},
                      {Lifetime::parameter(22), Lifetime::inference(23)}},
      .complete = true};
  expect(verifyMirLifetimeBodyFacts(emptyProgram, graphBody, caller).valid(),
         "two independent inputs may constrain one existential meet");
  LifetimeSignatureContract cases{
      .parameters = {Lifetime::parameter(21), Lifetime::parameter(22),
                     Lifetime::parameter(24), Lifetime::parameter(25)},
      .complete = true};
  using CaseKind = LifetimeRequirementNode::Kind;
  cases.requirements.nodes = {
      {.kind = CaseKind::Region, .bound = {Lifetime::parameter(21), Lifetime::parameter(22)}},
      {.kind = CaseKind::Region, .bound = {Lifetime::parameter(22), Lifetime::parameter(25)}},
      {.kind = CaseKind::All, .left = 0, .right = 1},
      {.kind = CaseKind::Region, .bound = {Lifetime::parameter(21), Lifetime::parameter(24)}},
      {.kind = CaseKind::Region, .bound = {Lifetime::parameter(24), Lifetime::parameter(25)}},
      {.kind = CaseKind::All, .left = 3, .right = 4},
      {.kind = CaseKind::Any, .left = 2, .right = 5}};
  cases.requirements.root = 6;
  MirBody caseBody;
  caseBody.lifetimeBody = {
      .obligations = {{Lifetime::parameter(21), Lifetime::parameter(25)}},
      .complete = true};
  expect(verifyMirLifetimeBodyFacts(emptyProgram, caseBody, cases).valid(),
         "each caller disjunct may prove the same transitive required relation");
  caseBody.lifetimeBody.obligations.push_back({Lifetime::parameter(22), Lifetime::parameter(25)});
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, caseBody, cases).valid(),
         "a relation true in one disjunct cannot become an unconditional body edge");
  auto unboundTypeContract = caller;
  unboundTypeContract.typeBounds = {{reference(999), Lifetime::parameter(21)}};
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, graphBody, unboundTypeContract).valid(),
         "type-outlives assumptions cannot introduce an unbound nested region");
  unboundTypeContract.typeBounds.clear();
  unboundTypeContract.requirements.nodes = {{.kind = CaseKind::Type,
      .bound = {{}, Lifetime::parameter(21)}, .type = reference(999)}};
  unboundTypeContract.requirements.root = 0;
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, graphBody, unboundTypeContract).valid(),
         "a requirement type leaf must be scoped before it becomes a branch assumption");
  auto forgedGraph = graphBody;
  forgedGraph.lifetimeBody.obligations.push_back(
      {Lifetime::inference(23), Lifetime::parameter(22)});
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, forgedGraph, caller).valid(),
         "an inference meet must not smuggle an unrelated universal outlives "
         "proof");
  auto relatedCaller = caller;
  relatedCaller.bounds = {{Lifetime::parameter(21), Lifetime::parameter(22)}};
  expect(verifyMirLifetimeBodyFacts(emptyProgram, forgedGraph, relatedCaller)
             .valid(),
         "a declared universal relationship may discharge the required path");
  forgedGraph = graphBody;
  forgedGraph.lifetimeBody.variables.clear();
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, forgedGraph, caller).valid(),
         "body graph edges cannot invent inference identities");
  forgedGraph = graphBody;
  forgedGraph.lifetimeBody.obligations[0].longer = Lifetime::local(24);
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, forgedGraph, caller).valid(),
         "a local region requires an exact origin witness");
  graphBody.lifetimeBody.typeObligations = {
      {SemanticType::typeParameter(25), Lifetime::inference(23)}};
  caller.typeBounds = {
      {SemanticType::typeParameter(25), Lifetime::parameter(21)}};
  expect(verifyMirLifetimeBodyFacts(emptyProgram, graphBody, caller).valid(),
         "an exact caller type bound may flow into an existential region");
  graphBody.lifetimeBody.typeObligations[0].type.projectionConcept = 26;
  expect(!verifyMirLifetimeBodyFacts(emptyProgram, graphBody, caller).valid(),
         "a bound on T does not establish a bound on an associated projection "
         "of T");
  MirBody initializing;
  initializing.placeDomain = {1, 2, 0};
  MirPlace owned;
  owned.id = 1;
  owned.root = MirPlaceRootKind::Binding;
  owned.binding = 1;
  owned.type = SemanticType::Int32;
  owned.sourceValue = 7;
  owned.key = PlaceKey{.domain = initializing.placeDomain, .root = 1};
  MirPlace alias = owned;
  alias.id = 2;
  alias.binding = 2;
  alias.type = SemanticType::referenceTo(SemanticType::Int32);
  alias.type.referenceLifetime = Lifetime::inference(8);
  alias.sourceValue = 9;
  alias.key->root = 2;
  initializing.places = {owned, alias};
  initializing.lifetimeBody = {
      .variables = {Lifetime::inference(8)},
      .obligations = {{Lifetime::local(11), Lifetime::inference(8)}},
      .origins = {{.region = Lifetime::local(11),
                   .place = 1,
                   .key = *owned.key,
                   .source = 7}},
      .complete = true};
  auto staticReference = alias.type;
  staticReference.referenceLifetime = Lifetime::staticLifetime();
  expect(mirLifetimeReferenceConversion(emptyProgram, initializing, 0,
                                         staticReference, alias.type),
         "reference conversion may shorten a proven static region");
  auto wrongReferent = alias.type;
  wrongReferent.arguments.front() = SemanticType::Int64;
  expect(!mirLifetimeReferenceConversion(emptyProgram, initializing, 0,
                                          staticReference, wrongReferent),
         "lifetime shortening never substitutes a different referent type");
  auto mutableReference = alias.type;
  mutableReference.referenceAccess = AccessMode::Mutable;
  expect(!mirLifetimeReferenceConversion(emptyProgram, initializing, 0,
                                          staticReference, mutableReference),
         "lifetime shortening cannot invent mutable permission");
  auto unknownReference = alias.type;
  unknownReference.referenceLifetime = Lifetime::parameter(999);
  expect(!mirLifetimeReferenceConversion(emptyProgram, initializing, 0,
                                          staticReference, unknownReference),
         "reference shortening cannot invent an undeclared region");
  MirInstruction initialize;
  initialize.id = 1;
  initialize.kind = MirInstructionKind::Initialize;
  initialize.hirValue = 7;
  initialize.destination = 2;
  initialize.operands = {
      {.kind = MirOperandKind::BorrowRead, .place = 1, .type = alias.type}};
  MirBlock initializationBlock;
  initializationBlock.id = 1;
  initializationBlock.instructions = {initialize};
  initializing.blocks = {initializationBlock};
  expect(verifyMirLifetimeApplications(emptyProgram, initializing).valid(),
         "reference initialization must replay its exact "
         "source-to-declared-region edge");
  auto missingInitializationEdge = initializing;
  missingInitializationEdge.lifetimeBody.obligations.clear();
  expect(!verifyMirLifetimeApplications(emptyProgram, missingInitializationEdge)
              .valid(),
         "dropping a reference initializer edge must invalidate MIR proof");
  auto escapedLocal = initializing;
  escapedLocal.returnType = SemanticType::referenceTo(SemanticType::Int32);
  escapedLocal.returnType.referenceLifetime = Lifetime::staticLifetime();
  escapedLocal.blocks[0].terminator.kind = MirTerminatorKind::Return;
  escapedLocal.blocks[0].terminator.hirValue = 7;
  escapedLocal.blocks[0].terminator.value =
      MirOperand{.kind = MirOperandKind::BorrowRead,
                 .place = 1,
                 .type = escapedLocal.returnType};
  expect(!verifyMirLifetimeApplications(emptyProgram, escapedLocal).valid(),
         "MIR return reconstruction cannot promote a local origin to static");
  std::cout << "lifetime IR structural metadata passed\n";
}
