#include "../src/compiler/mir_verification_queries.h"
#include "gti/lifetime_types.h"
#include "gti/mir_printer.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace lang {
struct MirProgramTestAccess {
  static auto &classes(MirProgram &program) { return program.classes; }
  static auto &payloads(MirProgram &program) { return program.payloadEnums_; }
  static auto &lambdas(MirProgram &program) { return program.lambdas; }
};
} // namespace lang
using namespace lang;
using namespace lang::mir_detail;
namespace {
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
SemanticType nominal(ClassId declaration, Lifetime region) {
  auto type = SemanticType::classType(declaration);
  type.lifetimeArguments = {region};
  return type;
}
SemanticType enumeration(Lifetime region) {
  auto type = SemanticType::enumType(21);
  type.lifetimeArguments = {region};
  return type;
}
struct Fixture {
  MirProgram program;
  Lifetime actualRegion = Lifetime::local(900);
  SemanticType actual = nominal(13, actualRegion);
  BorrowPath path{{.kind = PlaceProjectionKind::Base, .index = 1, .baseOwner = 13},
                  {.kind = PlaceProjectionKind::Field, .field = 111},
                  {.kind = PlaceProjectionKind::ConstantIndex, .index = 1},
                  {.kind = PlaceProjectionKind::Field, .field = 311}};
  std::vector<MirBorrowPathGuard> guards{
      {.subobject = {path[0], path[1], path[2]},
       .enumeration = enumeration(actualRegion), .variant = 1}};
  Fixture() {
    const auto baseRegion = Lifetime::parameter(101);
    const auto derivedRegion = Lifetime::parameter(103);
    const auto enumRegion = Lifetime::parameter(201);
    MirClassInstance base;
    base.id = 1;
    base.declaration = 11;
    base.type = nominal(11, baseRegion);
    base.lifetimeContract = {.parameters = {baseRegion}, .complete = true};
    base.declaredFields = {{.symbol = 111,
        .type = SemanticType::arrayOf(enumeration(baseRegion), std::uint64_t{2})}};
    MirClassInstance interface;
    interface.id = 2;
    interface.declaration = 12;
    interface.type = SemanticType::classType(12);
    interface.kind = ClassKind::Interface;
    interface.lifetimeContract.complete = true;
    MirClassInstance derived;
    derived.id = 3;
    derived.declaration = 13;
    derived.type = nominal(13, derivedRegion);
    derived.lifetimeContract = {.parameters = {derivedRegion}, .complete = true};
    derived.structuralBases = {{.instance = 2, .type = interface.type, .interface = true},
                              {.instance = 1, .type = nominal(11, derivedRegion)}};
    derived.bases = derived.structuralBases;
    MirProgramTestAccess::classes(program) = {base, interface, derived};
    MirPayloadEnum payload;
    payload.declaration = 21;
    payload.type = enumeration(enumRegion);
    payload.lifetimeContract = {.parameters = {enumRegion}, .complete = true};
    payload.variants = {{.variantIndex = 0},
        {.variantIndex = 1,
         .payloadTypes = {SemanticType::referenceTo(SemanticType::Int32,
                                                   AccessMode::ReadOnly, enumRegion)},
         .payloadSymbols = {311}}};
    MirProgramTestAccess::payloads(program) = {payload};
  }
  std::optional<MirBorrowPathView> walk() const {
    return walkBorrowPath(program, actual, AccessMode::Mutable, path, guards,
                          MirBorrowPathRoot::Value);
  }
};
void exactBaseAndPayloadPaths() {
  const Fixture fixture;
  const auto value = fixture.walk();
  expect(value && value->type == SemanticType::Int32 &&
             value->referenceAccessLimit == AccessMode::ReadOnly,
         "a nonzero state-base ordinal reaches the exact actual guarded referent");
  expect(value->projections.size() == 5 &&
             value->projections.front() == MirPlaceProjection{
                 .kind = MirProjectionKind::Base, .constantIndex = 1, .selection = 1} &&
             value->projections.back().kind == MirProjectionKind::Dereference &&
             value->referenceCrossings.size() == 1 &&
             value->referenceCrossings.front().reference.referenceLifetime == fixture.actualRegion &&
             value->referenceCrossings.front().pathOffset == 4 &&
             value->referenceCrossings.front().projectionOffset == 4 &&
             !value->referenceCrossings.front().physicalRoot,
         "base identity, bounded element and external-reference crossing stay exact");
  expect(value->payloadSelections == std::vector<MirBorrowPathPayloadSelection>{
             {.pathOffset = 3, .projectionOffset = 3,
              .enumeration = enumeration(fixture.actualRegion), .variant = 1, .payloadIndex = 0}},
         "payload selection offsets include every preceding structural projection");
  const auto restored = borrowPathForProjections(
      fixture.program, fixture.actual, AccessMode::Mutable, value->projections,
      fixture.guards, value->payloadSelections, MirBorrowPathRoot::Value);
  expect(restored && *restored == fixture.path,
         "reverse mapping requires exact forward replay of the complete typed path");
  const auto reference = SemanticType::referenceTo(fixture.actual, AccessMode::Mutable,
                                                  Lifetime::local(901));
  const auto physical = walkBorrowPath(fixture.program, reference, AccessMode::Mutable,
      fixture.path, fixture.guards, MirBorrowPathRoot::PhysicalReference);
  expect(physical && physical->referenceCrossings.size() == 2 &&
             physical->referenceCrossings[0].physicalRoot &&
             !physical->referenceCrossings[1].physicalRoot &&
             physical->referenceCrossings[1].projectionOffset == 5 &&
             physical->payloadSelections.front().projectionOffset == 4,
         "physical root borrow is distinct from the later stored external reference");
  expect(borrowPathForProjections(fixture.program, reference, AccessMode::Mutable,
             physical->projections, fixture.guards, physical->payloadSelections,
             MirBorrowPathRoot::PhysicalReference) == std::optional{fixture.path},
         "reverse replay preserves the explicit physical root role");
  expect(!walkBorrowPath(fixture.program, reference, AccessMode::Mutable,
             fixture.path, fixture.guards, MirBorrowPathRoot::Value) &&
         !walkBorrowPath(fixture.program, fixture.actual, AccessMode::Mutable,
             fixture.path, fixture.guards, MirBorrowPathRoot::PhysicalReference),
         "a root role cannot silently insert or remove a physical reference");
  const BorrowPath domain(fixture.path.begin(), fixture.path.end() - 1);
  const auto guardedDomain = walkBorrowPath(fixture.program, fixture.actual,
      AccessMode::Mutable, domain, fixture.guards, MirBorrowPathRoot::Value);
  expect(guardedDomain && guardedDomain->type == enumeration(fixture.actualRegion) &&
             guardedDomain->payloadSelections.empty(),
         "an endpoint enum guard validates its exact domain without inventing a payload address");
}
void malformedPaths() {
  for (int mutation = 0; mutation != 18; ++mutation) {
    Fixture fixture;
    switch (mutation) {
    case 0: fixture.path[0].baseOwner = 11; fixture.guards[0].subobject[0] = fixture.path[0]; break;
    case 1: fixture.path[0].index = 0; fixture.guards[0].subobject[0] = fixture.path[0]; break;
    case 2: fixture.path[0].index = 2; fixture.guards[0].subobject[0] = fixture.path[0]; break;
    case 3: fixture.path[1].field = 999; fixture.guards[0].subobject[1] = fixture.path[1]; break;
    case 4: fixture.path[2].index = 2; fixture.guards[0].subobject[2] = fixture.path[2]; break;
    case 5: fixture.guards[0].enumeration = enumeration(Lifetime::staticLifetime()); break;
    case 6: fixture.guards[0].variant = 0; break;
    case 7: fixture.guards.clear(); break;
    case 8: fixture.guards[0].subobject.push_back(fixture.path.back()); break;
    case 9: fixture.guards.push_back(fixture.guards.front()); break;
    case 10: fixture.path[0].field = 111; fixture.guards[0].subobject[0] = fixture.path[0]; break;
    case 11:
      fixture.path.erase(fixture.path.begin());
      fixture.guards[0].subobject.erase(fixture.guards[0].subobject.begin());
      break;
    case 12: fixture.actual.classId = 999; break;
    case 13: {
      auto duplicate = MirProgramTestAccess::classes(fixture.program).back();
      duplicate.id = 4;
      MirProgramTestAccess::classes(fixture.program).push_back(duplicate);
      break;
    }
    case 14: MirProgramTestAccess::classes(fixture.program)[2].structuralBases[1].instance = 2; break;
    case 15: MirProgramTestAccess::classes(fixture.program)[0].declaredFields[0].type.arrayLengthParameterId = 999; break;
    case 16: {
      auto &fields = MirProgramTestAccess::classes(fixture.program)[0].declaredFields;
      fields.push_back(fields.front());
      break;
    }
    case 17: {
      auto &variant = MirProgramTestAccess::payloads(fixture.program)[0].variants[1];
      variant.payloadSymbols.push_back(variant.payloadSymbols.front());
      variant.payloadTypes.push_back(variant.payloadTypes.front());
      break;
    }
    }
    if (fixture.walk()) {
      std::cerr << "accepted malformed typed path mutation=" << mutation << '\n';
      expect(false, "typed paths reject invalid owners, ordinals, guards, bounds and ambiguous inventories");
    }
  }
}
void reverseForgeryControls() {
  const Fixture fixture;
  const auto valid = *fixture.walk();
  for (int mutation = 0; mutation != 9; ++mutation) {
    auto projections = valid.projections;
    auto selections = valid.payloadSelections;
    switch (mutation) {
    case 0: projections[0].constantIndex.reset(); break;
    case 1: projections[0].constantIndex = 0; break;
    case 2: projections[0].selection = 2; break;
    case 3: projections.erase(projections.begin()); break;
    case 4: ++selections[0].projectionOffset; break;
    case 5: projections.push_back({.kind = MirProjectionKind::Dereference}); break;
    case 6: projections[2].index = 1; break;
    case 7: ++selections[0].payloadIndex; break;
    case 8: projections.pop_back(); break;
    }
    expect(!borrowPathForProjections(fixture.program, fixture.actual, AccessMode::Mutable,
               projections, fixture.guards, selections, MirBorrowPathRoot::Value),
           "reverse replay rejects missing base identity, hidden dereferences and forged selections");
  }
}
void packAndCapturePaths() {
  MirProgram program;
  const auto region = Lifetime::local(700);
  const auto reference = SemanticType::referenceTo(SemanticType::Int32, AccessMode::Mutable, region);
  const auto pack = SemanticType::concreteTypePack(701, {SemanticType::Int32, reference});
  const BorrowPath element{{.kind = PlaceProjectionKind::ConstantIndex, .index = 1}};
  const auto selected = walkBorrowPath(program, pack, AccessMode::Mutable, element, {}, MirBorrowPathRoot::Value);
  expect(selected && selected->type == SemanticType::Int32 &&
             selected->referenceAccessLimit == AccessMode::Mutable &&
             selected->projections.front().kind == MirProjectionKind::PackElement,
         "a concrete pack element retains its exact external reference permission");
  expect(!walkBorrowPath(program, pack, AccessMode::Mutable,
             BorrowPath{{.kind = PlaceProjectionKind::ConstantIndex, .index = 2}}, {}, MirBorrowPathRoot::Value) &&
         !walkBorrowPath(program, SemanticType::rawPointerTo(SemanticType::Int32), AccessMode::Mutable,
             element, {}, MirBorrowPathRoot::Value),
         "out-of-range packs and raw-pointer operations are not exported borrow paths");
  const auto universal = Lifetime::parameter(702);
  const auto capture = SemanticType::referenceTo(SemanticType::Int32, AccessMode::ReadOnly, universal);
  MirLambdaInstance lambda;
  lambda.id = 1;
  lambda.declaration = 31;
  lambda.environmentLifetimes = {universal};
  lambda.type = SemanticType::lambdaType(31, SemanticType::Int32, {}, std::array{capture});
  lambda.type.lifetimeArguments = {universal};
  lambda.returnType = SemanticType::Int32;
  lambda.captureTypes = {capture};
  lambda.captureSymbols = {600};
  lambda.lifetimeContract = {.parameters = {universal}, .complete = true};
  MirProgramTestAccess::lambdas(program) = {lambda};
  const auto actual = *substituteTypeLifetimes(lambda.type, std::array{LifetimeBinding{universal, region}});
  const BorrowPath field{{.kind = PlaceProjectionKind::Field, .field = 600}};
  const auto captured = walkBorrowPath(program, actual, AccessMode::Mutable, field, {}, MirBorrowPathRoot::Value);
  expect(captured && captured->referenceCrossings.front().reference.referenceLifetime == region,
         "lambda capture paths use the exact fixed environment substitution");
  lambda.id = 2;
  lambda.captureSymbols = {601};
  MirProgramTestAccess::lambdas(program).push_back(lambda);
  expect(!walkBorrowPath(program, actual, AccessMode::Mutable, field, {}, MirBorrowPathRoot::Value),
         "compatible closure types cannot hide an ambiguous capture inventory");
}
void serializedOwnerIdentity() {
  MirBody body;
  body.places.push_back({.id = 1, .root = MirPlaceRootKind::Symbol, .symbol = 1,
      .key = PlaceKey{.root = 1,
          .projections = {{.kind = PlaceProjectionKind::Base, .index = 1, .baseOwner = 13}}}});
  const auto first = MirPrinter{}.print(body);
  body.places[0].key->projections[0].baseOwner = 14;
  expect(first != MirPrinter{}.print(body) && first.find("base-owner=13") != std::string::npos,
         "deterministic MIR identity includes the semantic Base declaration owner");
}
} // namespace
int main() {
  exactBaseAndPayloadPaths();
  malformedPaths();
  reverseForgeryControls();
  packAndCapturePaths();
  serializedOwnerIdentity();
}
