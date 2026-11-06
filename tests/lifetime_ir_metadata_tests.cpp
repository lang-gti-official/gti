#include "../src/compiler/mir_lifetimes.h"
#include "gti/mir_printer.h"

#include <cstdlib>
#include <iostream>

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
} // namespace
int main() {
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
