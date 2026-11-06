#include "gti/persistent_declaration.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace lang;
namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
void rejects(const std::function<void()> &operation, const char *message) {
  try {
    operation();
  } catch (const std::invalid_argument &) {
    return;
  }
  expect(false, message);
}
SemanticType reference(Lifetime lifetime) {
  auto result = SemanticType::referenceTo(SemanticType(SemanticType::Int32));
  result.referenceLifetime = lifetime;
  return result;
}
SemanticType callable(GenericParameterId external, GenericParameterId local) {
  auto result = SemanticType::functionSignature(
      reference(Lifetime::parameter(local)),
      {reference(Lifetime::parameter(local)),
       reference(Lifetime::parameter(external))},
      ReceiverMutability::Mutable);
  result.quantifiedLifetimes = {local};
  return result;
}
LifetimeSignatureContract contract(GenericParameterId first,
                                   GenericParameterId second) {
  LifetimeSignatureContract result;
  result.parameters = {Lifetime::parameter(first), Lifetime::parameter(second)};
  result.bounds = {{result.parameters[0], result.parameters[1]},
                   {Lifetime::staticLifetime(), result.parameters[0]}};
  result.receiver = result.parameters[0];
  result.origin = LifetimeContractOrigin::Declared;
  result.complete = true;
  return result;
}
void typeRecipes() {
  PersistentTypeRegistry registry;
  const PersistentLifetimeBindings source{{11}, {12}};
  const PersistentLifetimeBindings target{{101}, {102}};
  auto type = callable(11, 12);
  auto bytes = writePersistentType(type, registry, source);
  expect(bytes.starts_with("gti.type.3:"), "region recipes must use version 3");
  expect(writePersistentType(callable(101, 102), registry, target) == bytes,
         "alpha-renaming must not change persisted type bytes");
  expect(readPersistentType(bytes, registry, target) == callable(101, 102),
         "type decoding must remap every external and quantified region");
  expect(writePersistentType(type, registry, {{11}, {}}) == bytes,
         "explicit binding context may discover quantified declaration IDs");
  auto changed = type;
  changed.arguments[0].referenceLifetime = Lifetime::parameter(11);
  expect(writePersistentType(changed, registry, source) != bytes,
         "changing a result's lifetime relation must change the recipe");
  const auto staticReference = reference(Lifetime::staticLifetime());
  expect(readPersistentType(writePersistentType(staticReference, registry),
                            registry) == staticReference,
         "static needs no imported declaration binder");
  const auto plainReference = reference({});
  expect(readPersistentType(writePersistentType(plainReference, registry),
                            registry) == plainReference,
         "legacy absent lifetime annotations must retain their shape");
  rejects([&] { (void)writePersistentType(type, registry); },
          "no-context writer must reject quantified regions");
  auto onlyQuantified = SemanticType::functionSignature(
      reference(Lifetime::parameter(12)), {reference(Lifetime::parameter(12))});
  onlyQuantified.quantifiedLifetimes = {12};
  rejects([&] { (void)writePersistentType(onlyQuantified, registry); },
          "self-contained HRTB still requires an explicit binding context");
  rejects(
      [&] {
        (void)writePersistentType(reference(Lifetime::parameter(11)), registry);
      },
      "no-context writer must reject declaration regions");
  rejects([&] { (void)readPersistentType(bytes, registry); },
          "reader must never invent quantified region identities");
  rejects([&] { (void)readPersistentType(bytes, registry, {{101}, {}}); },
          "missing quantified IDs must reject");
  rejects(
      [&] { (void)readPersistentType(bytes, registry, {{101}, {102, 103}}); },
      "unused quantified IDs must reject");
  rejects([&] { (void)readPersistentType(bytes, registry, {{101}, {101}}); },
          "overlapping declaration and quantified IDs must reject");
  rejects([&] { (void)writePersistentType(type, registry, {{11, 11}, {12}}); },
          "duplicate declaration IDs must reject");
  rejects([&] { (void)writePersistentType(type, registry, {{11}, {13}}); },
          "writer's quantified bindings must match the type");
  rejects(
      [&] {
        (void)writePersistentType(reference(Lifetime::parameter(99)), registry,
                                  source);
      },
      "unbound declaration IDs must reject");
  for (auto lifetime : {Lifetime::local(1), Lifetime::inference(1)})
    rejects(
        [&] { (void)writePersistentType(reference(lifetime), registry, {}); },
        "local and unresolved inference regions cannot cross interfaces");
  rejects([&] { (void)readPersistentType(bytes + "1:x", registry, target); },
          "trailing recipe data must reject");
  rejects(
      [&] {
        (void)readPersistentType(bytes.substr(0, bytes.size() - 1), registry,
                                 target);
      },
      "truncated recipe must reject");
  auto oldVersion = bytes;
  oldVersion.replace(0, 11, "gti.type.2:");
  rejects([&] { (void)readPersistentType(oldVersion, registry, target); },
          "old wire version must not acquire new meaning");
  auto invalid = type;
  invalid.quantifiedLifetimes = {12, 12};
  rejects([&] { (void)writePersistentType(invalid, registry, {{11}, {}}); },
          "duplicate live quantified IDs must reject");

  auto nested = SemanticType::functionSignature(type, {onlyQuantified});
  auto nestedBytes = writePersistentType(nested, registry, {{11}, {}});
  auto nestedResult =
      readPersistentType(nestedBytes, registry, {{101}, {102, 103}});
  expect(nestedResult.arguments[0] == callable(101, 102),
         "nested result must use the first preorder quantified binding");
  expect(nestedResult.arguments[1].quantifiedLifetimes ==
                 std::vector<GenericParameterId>{103} &&
             nestedResult.arguments[1].arguments[0].referenceLifetime ==
                 Lifetime::parameter(103),
         "sibling quantifier scopes must use independent imported identities");
}
void nominalRecipes() {
  ClassTypeInfo original;
  original.id = 17;
  original.qualifiedName = "example::Borrowed";
  GenericParameterInfo element;
  element.id = 21;
  element.name.lexeme = "T";
  GenericParameterInfo lifetime;
  lifetime.id = 22;
  lifetime.name.lexeme = "owner";
  lifetime.lifetime = true;
  original.genericParameters = {element, lifetime};
  auto imported = original;
  imported.id = 117;
  imported.genericParameters[0].id = 121;
  imported.genericParameters[1].id = 122;
  PersistentTypeRegistry from, to;
  const PersistentDeclarationOwner owner{"example", "core", "borrows", ""};
  from.bind(original, owner);
  to.bind(imported, owner);
  auto type =
      SemanticType::classType(original.id, {SemanticType(SemanticType::Int32)});
  type.lifetimeArguments = {Lifetime::parameter(22)};
  auto expected =
      SemanticType::classType(imported.id, {SemanticType(SemanticType::Int32)});
  expected.lifetimeArguments = {Lifetime::parameter(122)};
  auto bytes = writePersistentType(type, from, {{22}, {}});
  expect(readPersistentType(bytes, to, {{122}, {}}) == expected,
         "nominal IDs and compact lifetime arguments must remap together");
  expect(writePersistentType(expected, to, {{122}, {}}) == bytes,
         "nominal lifetime parameter identities cannot enter wire bytes");
  auto wrongArity = type;
  wrongArity.lifetimeArguments.clear();
  rejects([&] { (void)writePersistentType(wrongArity, from, {{22}, {}}); },
          "nominal declaration lifetime arity must be checked");
  auto absentRegion = type;
  absentRegion.lifetimeArguments = {{}};
  rejects([&] { (void)writePersistentType(absentRegion, from, {{22}, {}}); },
          "a required generic lifetime cannot persist as an absent annotation");
  auto reversed = original;
  reversed.genericParameters = {lifetime, element};
  expect(persistentClassKey(reversed, owner) !=
             persistentClassKey(original, owner),
         "declaration kind order must participate in nominal identity");
  SemanticType textView(SemanticType::StringView);
  textView.lifetimeArguments = {Lifetime::parameter(22)};
  auto viewBytes = writePersistentType(textView, from, {{22}, {}});
  textView.lifetimeArguments = {Lifetime::parameter(122)};
  expect(readPersistentType(viewBytes, to, {{122}, {}}) == textView,
         "builtin borrowing view regions must survive persistence");
}
void contracts() {
  const PersistentLifetimeBindings source{{31, 32}, {}};
  const PersistentLifetimeBindings target{{301, 302}, {}};
  auto value = contract(31, 32);
  auto bytes = writePersistentLifetimeContract(value, source);
  expect(writePersistentLifetimeContract(contract(301, 302), target) == bytes,
         "contract wire bytes must be alpha-renaming invariant");
  auto decoded = readPersistentLifetimeContract(bytes, target);
  expect(
      decoded.valid() && decoded.parameters == contract(301, 302).parameters &&
          decoded.receiver == Lifetime::parameter(301) &&
          decoded.bounds.size() == 2,
      "contract round trip must preserve receiver, parameters and all bounds");
  expect(writePersistentLifetimeContract(decoded, target) == bytes,
         "canonical imported contract must re-encode identically");
  std::reverse(value.bounds.begin(), value.bounds.end());
  value.bounds.push_back(value.bounds.front());
  expect(writePersistentLifetimeContract(value, source) == bytes,
         "bound ordering and duplicate premises must normalize");
  value = contract(31, 32);
  value.bounds[0] = {Lifetime::parameter(32), Lifetime::parameter(31)};
  expect(writePersistentLifetimeContract(value, source) != bytes,
         "changing an outlives relation must change contract bytes");
  value = contract(31, 32);
  value.complete = false;
  rejects([&] { (void)writePersistentLifetimeContract(value, source); },
          "incomplete contracts must never become interface artifacts");
  for (auto invalid :
       {Lifetime::local(7), Lifetime::inference(7), Lifetime::parameter(99)}) {
    value = contract(31, 32);
    value.bounds[0].longer = invalid;
    rejects([&] { (void)writePersistentLifetimeContract(value, source); },
            "unbound and body-local contract regions must reject");
  }
  value = contract(31, 32);
  rejects([&] { (void)writePersistentLifetimeContract(value, {{31, 31}, {}}); },
          "duplicate binding context must reject");
  rejects(
      [&] { (void)writePersistentLifetimeContract(value, {{31, 32}, {33}}); },
      "signature contract cannot absorb an unused type quantifier");
  rejects([&] { (void)readPersistentLifetimeContract(bytes, {{301}, {}}); },
          "wrong contract binding count must reject");
  rejects([&] { (void)readPersistentLifetimeContract(bytes + "0:", target); },
          "trailing contract data must reject");
  auto corrupt = bytes;
  corrupt.replace(corrupt.find("8:complete"), 10, "7:pending");
  rejects([&] { (void)readPersistentLifetimeContract(corrupt, target); },
          "serialized incomplete marker must reject");
}
void exactTypeBounds() {
  auto original = contract(31, 32);
  original.typeBounds = {
      {SemanticType::typeParameter(41), Lifetime::parameter(31)}};
  auto imported = contract(301, 302);
  imported.typeBounds = {
      {SemanticType::typeParameter(401), Lifetime::parameter(301)}};
  const PersistentLifetimeBindings from{{31, 32}, {}, {41}};
  const PersistentLifetimeBindings to{{301, 302}, {}, {401}};
  auto bytes = writePersistentLifetimeContract(original, from);
  expect(writePersistentLifetimeContract(imported, to) == bytes,
         "type-outlives bounds must normalize type and region parameter "
         "identities");
  auto decoded = readPersistentLifetimeContract(bytes, to);
  expect(
      decoded.typeBounds == imported.typeBounds,
      "exact generic type-outlives obligations must remap without reduction");
  imported.typeBounds[0].type =
      SemanticType::referenceTo(SemanticType::typeParameter(401));
  expect(writePersistentLifetimeContract(imported, to) != bytes,
         "a bound on a reference is not a bound on its referent");
  rejects(
      [&] {
        (void)writePersistentLifetimeContract(original, {{31, 32}, {}, {}});
      },
      "unbound generic types cannot enter lifetime contracts");
  rejects(
      [&] {
        (void)readPersistentLifetimeContract(bytes, {{301, 302}, {}, {}});
      },
      "type declaration binding counts must match imported contracts");
  rejects(
      [&] {
        (void)writePersistentLifetimeContract(original, {{31, 32}, {}, {31}});
      },
      "one identity cannot be both a type and a lifetime binder");
  auto projection = SemanticType::typeParameter(0);
  projection.projectionConcept = 77;
  projection.projectionMember = 0;
  projection.arguments = {SemanticType::typeParameter(41)};
  original.typeBounds[0].type = projection;
  rejects([&] { (void)writePersistentLifetimeContract(original, from); },
          "unregistered associated projections must fail closed without "
          "reducing to their arguments");

  original = contract(31, 32);
  original.typeBounds = {{callable(31, 51), Lifetime::parameter(32)}};
  auto quantifiedBytes =
      writePersistentLifetimeContract(original, {{31, 32}, {51}, {}});
  auto quantifiedResult =
      readPersistentLifetimeContract(quantifiedBytes, {{301, 302}, {501}, {}});
  expect(
      quantifiedResult.typeBounds[0].type == callable(301, 501) &&
          quantifiedResult.typeBounds[0].region == Lifetime::parameter(302),
      "higher-ranked types inside exact bounds retain their own binder scope");
}
void associatedTypeBounds() {
  GenericParameterInfo conceptType;
  conceptType.id = 701;
  conceptType.name.lexeme = "T";
  GenericParameterInfo conceptRegion;
  conceptRegion.id = 702;
  conceptRegion.name.lexeme = "source";
  conceptRegion.lifetime = true;
  GenericParameterInfo familyType;
  familyType.id = 703;
  familyType.name.lexeme = "U";
  GenericParameterInfo familyRegion;
  familyRegion.id = 704;
  familyRegion.name.lexeme = "item";
  familyRegion.lifetime = true;
  ProtocolConceptSpelling original;
  original.qualifiedName = "example::Collection";
  original.genericParameters = {conceptType, conceptRegion};
  original.parameterCount = 1;
  original.associatedTypes = {"Item", "Other"};
  original.associatedTypeParameters = {{familyType, familyRegion}, {}};
  auto imported = original;
  imported.genericParameters[0].id = 1701;
  imported.genericParameters[1].id = 1702;
  imported.associatedTypes = {"Other", "Item"};
  imported.associatedTypeParameters = {{}, {familyType, familyRegion}};
  imported.associatedTypeParameters[1][0].id = 1703;
  imported.associatedTypeParameters[1][1].id = 1704;
  const PersistentDeclarationOwner owner{"example", "core", "collections", ""};
  PersistentTypeRegistry from, to;
  from.bind(77, original, owner);
  to.bind(177, imported, owner);
  auto projection = SemanticType::typeParameter(0);
  projection.projectionConcept = 77;
  projection.projectionMember = 0;
  projection.arguments = {SemanticType::typeParameter(41),
                          SemanticType::typeParameter(42)};
  projection.lifetimeArguments = {Lifetime::parameter(31),
                                  Lifetime::parameter(32)};
  auto target = SemanticType::typeParameter(0);
  target.projectionConcept = 177;
  target.projectionMember = 1;
  target.arguments = {SemanticType::typeParameter(401),
                      SemanticType::typeParameter(402)};
  target.lifetimeArguments = {Lifetime::parameter(301),
                              Lifetime::parameter(302)};
  const PersistentLifetimeBindings sourceBindings{{31, 32}, {}, {41, 42}};
  const PersistentLifetimeBindings targetBindings{{301, 302}, {}, {401, 402}};
  auto bytes = writePersistentType(projection, from, sourceBindings);
  expect(writePersistentType(target, to, targetBindings) == bytes,
         "associated-family keys must not depend on snapshot concept ID or "
         "member index");
  expect(readPersistentType(bytes, to, targetBindings) == target,
         "associated projection import must remap exact member and all "
         "argument kinds");
  expect(persistentAssociatedTypeKey(original, 0, owner) ==
             persistentAssociatedTypeKey(imported, 1, owner),
         "stable associated identity is its declaration name and generic kind "
         "shape");
  auto originalContract = contract(31, 32);
  originalContract.typeBounds = {{projection, Lifetime::parameter(31)}};
  auto targetContract = contract(301, 302);
  targetContract.typeBounds = {{target, Lifetime::parameter(301)}};
  auto contractBytes =
      writePersistentLifetimeContract(originalContract, from, sourceBindings);
  expect(readPersistentLifetimeContract(contractBytes, to, targetBindings)
                 .typeBounds == targetContract.typeBounds,
         "opaque associated outlives obligation must persist without reducing "
         "its type arguments");
  expect(writePersistentLifetimeContract(targetContract, to, targetBindings) ==
             contractBytes,
         "associated-type contract bytes must survive semantic snapshot "
         "remapping");
  auto missingArguments = projection;
  missingArguments.lifetimeArguments.pop_back();
  rejects(
      [&] {
        (void)writePersistentType(missingArguments, from, sourceBindings);
      },
      "associated-family lifetime arity must match both declarations");
  auto malformed = projection;
  malformed.genericParameterId = 99;
  rejects([&] { (void)writePersistentType(malformed, from, sourceBindings); },
          "an opaque projection cannot masquerade as a type binder");
  auto incomplete = original;
  incomplete.associatedTypeParameters.clear();
  rejects([&] { from.bind(77, incomplete, owner); },
          "incomplete associated-family descriptors must reject");
  auto duplicate = original;
  duplicate.associatedTypes[1] = "Item";
  rejects([&] { from.bind(77, duplicate, owner); },
          "duplicate associated names must reject before registry mutation");
  auto conflicting = original;
  conflicting.qualifiedName = "example::Different";
  rejects([&] { from.bind(77, conflicting, owner); },
          "one semantic concept cannot acquire a second persistent identity");
  rejects([&] { from.bind(78, original, owner); },
          "one persistent concept cannot bind two semantic declarations");
  expect(
      writePersistentType(projection, from, sourceBindings) == bytes,
      "failed concept registration must preserve earlier projection bindings");
  PersistentTypeRegistry absent;
  rejects([&] { (void)readPersistentType(bytes, absent, targetBindings); },
          "projection decoder must never invent an unregistered concept");
  auto wrongKind = imported;
  wrongKind.associatedTypeParameters[1][1].lifetime = false;
  expect(persistentAssociatedTypeKey(original, 0, owner) !=
             persistentAssociatedTypeKey(wrongKind, 1, owner),
         "associated generic kind changes must change persistent identity");
  rejects(
      [&] {
        (void)readPersistentType(bytes.substr(0, bytes.size() - 1), to,
                                 targetBindings);
      },
      "truncated associated projection recipes must reject");
}
} // namespace
int main() {
  typeRecipes();
  nominalRecipes();
  contracts();
  exactTypeBounds();
  associatedTypeBounds();
  std::cout << "persistent lifetime recipes and contracts passed\n";
}
