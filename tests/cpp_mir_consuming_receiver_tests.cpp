#include "../src/compiler/cpp_mir_body_emitter.h"
#include "compiler_test_fixture.h"
#include "cpp_backend_test_support.h"

// Frozen C++ backend: the C++ emitter's owned-parameter field-binding proof
// for constructors whose owned field move follows a scalar prefix. The
// backend-neutral constructor contract lives in consuming_receiver_tests.

void checkCppScalarPrefixBeforeOwnedFieldMove() {
  const auto frontend =
      analyzeTrustedPreludeFixture("scalar-prefix-owned-field.gti", R"(
class Payload { public: Payload() {} ~Payload() {} };
class Box {
  int identity;
  mut Payload payload;
public:
  Box(int identity, Payload payload)
      : identity(identity), payload(std::move(payload)) {}
};
struct Other { int foreign = 0;  public: Other() = default; };
int main() {
  Payload payload{};
  Box box{8, std::move(payload)};
  Other other{};
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "scalar parameter followed by an owning field is valid source");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirConstructorInstance *constructor = nullptr;
  for (const auto &candidate : frontend.mir.constructorInstances())
    if (candidate.parameterTypes.size() == 2 &&
        candidate.parameterTypes.front() == lang::SemanticType::Int32 &&
        candidate.parameterTypes.back().kind == lang::SemanticType::Class)
      constructor = &candidate;
  expect(constructor != nullptr,
         "scalar-prefix fixture selects one mixed constructor");
  if (!constructor)
    return;
  const auto canonical =
      lang::cppMirOwnedParameterFieldBindings(frontend.mir, *constructor);
  expect(canonical && canonical->size() == 1 &&
             canonical->front().initializer == 1,
         "the exact scalar prefix admits the following owned field move");
  if (!canonical || canonical->size() != 1)
    return;
  lang::SymbolId foreignField = 0;
  for (const auto &owner : frontend.mir.classInstances())
    if (owner.id != constructor->owner)
      for (const auto &field : owner.declaredFields)
        if (field.type == lang::SemanticType::Int32)
          foreignField = field.symbol;
  expect(foreignField != 0, "fixture has an unrelated scalar field identity");
  if (!foreignField)
    return;

  for (int mutation = 0; mutation != 9; ++mutation) {
    auto forged = frontend.mir;
    auto *changed = const_cast<lang::MirConstructorInstance *>(
        forged.findConstructorInstance(constructor->id));
    auto entry = std::find_if(
        changed->body.blocks.begin(), changed->body.blocks.end(),
        [&](const auto &block) { return block.id == changed->body.entry; });
    expect(
        entry != changed->body.blocks.end() && entry->instructions.size() >= 4,
        "prefix mutations require Load, Initialize, boundary, and owned Move");
    if (entry == changed->body.blocks.end() || entry->instructions.size() < 4)
      continue;
    auto &load = entry->instructions[0];
    auto &store = entry->instructions[1];
    auto &boundary = entry->instructions[2];
    expect(load.kind == lang::MirInstructionKind::Load &&
               store.kind == lang::MirInstructionKind::Initialize &&
               boundary.kind == lang::MirInstructionKind::Lifecycle &&
               entry->instructions[3].id == canonical->front().moveInstruction,
           "each direct-helper mutation starts from the newly admitted prefix");
    switch (mutation) {
    case 0:
      load.definedFailure.propagation =
          lang::FailurePropagationKind::DirectCall;
      break;
    case 1:
      load.kind = lang::MirInstructionKind::Compute;
      load.operation = lang::MirOperation::Add;
      break;
    case 2:
      load.info.type = lang::SemanticType::RawPointer;
      load.info.type.arguments = {lang::SemanticType::Int32};
      break;
    case 3:
      load.info.traits.containsBorrowedState = true;
      break;
    case 4:
      load.operands.front().kind = lang::MirOperandKind::BorrowRead;
      break;
    case 5: {
      auto field = std::find_if(
          changed->body.places.begin(), changed->body.places.end(),
          [&](const auto &place) { return store.destination == place.id; });
      expect(field != changed->body.places.end() && !field->projections.empty(),
             "scalar store retains one destination field projection");
      if (field != changed->body.places.end() && !field->projections.empty())
        field->projections.front().field = foreignField;
      break;
    }
    case 6:
      boundary.fullExpressionEnd = 0;
      for (const auto &expression : changed->body.fullExpressions)
        if (expression.constructorInitializer == 2)
          boundary.fullExpressionEnd = expression.id;
      expect(
          boundary.fullExpressionEnd != 0,
          "mutation retargets the boundary to an existing other initializer");
      break;
    case 7:
      changed->initializers.front().arguments =
          changed->initializers.back().arguments;
      break;
    case 8:
      store.definedFailure.propagation =
          lang::FailurePropagationKind::DirectCall;
      break;
    }
    // Deliberately call the local proof directly: rejection must not be an
    // incidental effect of whole-program verifier failure elsewhere.
    expect(!lang::cppMirOwnedParameterFieldBindings(forged, *changed),
           "owned-field scalar-prefix proof rejects mutation " +
               std::to_string(mutation));
  }
}

int main() {
  checkCppScalarPrefixBeforeOwnedFieldMove();
  if (failures != 0) {
    std::cerr << failures << " C++ consuming receiver test(s) failed\n";
    return 1;
  }
  std::cout << "C++ consuming receiver tests passed\n";
  return 0;
}
