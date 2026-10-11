#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

lang::MirInstruction *instruction(lang::MirBody &body,
                                  lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &candidate : block.instructions)
      if (candidate.id == id)
        return &candidate;
  return nullptr;
}

void testMixedCarrierParameterPrefix() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_dual_range_direct.gti";
  std::ifstream input(path);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(!source.empty() && frontend.canGenerateCode(),
         "unrelated two-owner inline proxy fixture has verified MIR");
  if (!frontend.canGenerateCode())
    return;

  const lang::MirConstructorInstance *selected = nullptr;
  for (const auto &candidate : frontend.mir.constructorInstances()) {
    if (candidate.initializers.size() == 3 &&
        candidate.initializers[0].ownedParameter &&
        candidate.initializers[1].ownedParameter &&
        !candidate.initializers[2].ownedParameter &&
        candidate.initializers[2].targetType == lang::SemanticType::Int32) {
      expect(selected == nullptr, "the mixed carrier constructor is unique");
      selected = &candidate;
    }
  }
  expect(selected != nullptr,
         "fixture exercises two moved fields and scalar sibling");
  if (!selected)
    return;
  const auto bindings =
      lang::cppMirOwnedParameterFieldBindings(frontend.mir, *selected);
  expect(bindings && bindings->size() == 2,
         "complete mixed prologue preserves both exact native field moves");
  if (!bindings || bindings->size() != 2)
    return;

  lang::MirInstructionId scalarLoad = 0, scalarInitialize = 0, scalarEnd = 0;
  for (const auto &block : selected->body.blocks)
    for (const auto &candidate : block.instructions) {
      if (candidate.kind == lang::MirInstructionKind::Load &&
          candidate.hirValue == selected->initializers[2].arguments.front())
        scalarLoad = candidate.id;
      if (candidate.constructorInitializer == 3)
        scalarInitialize = candidate.id;
      if (candidate.fullExpressionEnd != 0)
        for (const auto &expression : selected->body.fullExpressions)
          if (expression.id == candidate.fullExpressionEnd &&
              expression.constructorInitializer == 3)
            scalarEnd = candidate.id;
    }
  expect(scalarLoad && scalarInitialize && scalarEnd,
         "scalar sibling retains its exact executable publication triple");
  if (!scalarLoad || !scalarInitialize || !scalarEnd)
    return;

  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                       .owner = selected->id};
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.supportsFailureBodyText(address),
           "mixed carrier constructor is admitted on both native standards");
    const auto emitted =
        emitter.emitFailureBodyText(address, "mixed-carrier", 1);
    expect(emitted.emitted(), "mixed carrier constructor has a MIR body plan");

    const auto reject = [&](auto mutate, std::string_view reason) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(selected->id));
      mutate(*changed);
      (void)lang::rebuildMirValueUses(changed->body);
      expect(!lang::cppMirOwnedParameterFieldBindings(malformed, *changed),
             reason);
      expect(!lang::verifyMirProgram(malformed).valid(),
             "independent MIR verification rejects the forged publication");
      const lang::CppMirBodyEmitter changedEmitter(malformed, map);
      expect(!changedEmitter.supportsFailureBodyText(address),
             "backend admission rejects the forged mixed prefix");
    };
    reject(
        [&](auto &changed) {
          instruction(changed.body, scalarInitialize)->constructorInitializer =
              0;
        },
        "scalar sibling cannot lose its exact initializer identity");
    reject(
        [&](auto &changed) {
          instruction(changed.body, scalarLoad)->operands.front().place =
              bindings->front().sourcePlace;
        },
        "scalar sibling cannot read a different typed parameter");
    reject(
        [&](auto &changed) {
          instruction(changed.body, scalarInitialize)->destination =
              instruction(changed.body, bindings->front().initializeInstruction)
                  ->destination;
        },
        "scalar sibling cannot publish into another field");
    reject(
        [&](auto &changed) {
          instruction(changed.body, scalarEnd)->fullExpressionEnd = 0;
        },
        "scalar sibling must retain its exact full-expression endpoint");
    reject(
        [&](auto &changed) {
          changed.initializers[1].ownedParameter =
              changed.initializers[0].ownedParameter;
        },
        "two native moved fields cannot claim the same source parameter");
  }
}

void testExclusiveGeneratedShell() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_mutable_inline_proxy_endpoint.gti";
  std::ifstream input(path);
  std::string source((std::istreambuf_iterator<char>(input)), {});
  const auto main = source.find("\nint main(");
  expect(main != std::string::npos,
         "exclusive shell fixture has an entry point");
  if (main == std::string::npos)
    return;
  // The exact same source classes exercise shell retirement independently of
  // the structural range protocol and its separate endpoint marker proof.
  source.replace(main, std::string::npos, R"(
int main() {
  mut MutableOwnerRange range{0};
  { auto cursor = range.begin_mut(); }
  return range.at(std::size_t(0)) == 101 ? 0 : 1;
}
)");
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "exclusive generated shell has verified MIR without range lowering");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirConstructorInstance *cleanupConstructor = nullptr;
  lang::HirClassInstanceId exclusiveShell = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    if (!owner || !owner->destructor || owner->declaredFields.size() != 1)
      continue;
    const lang::MirClassInstance *child = nullptr;
    for (const auto &candidate : frontend.mir.classInstances())
      if (candidate.type == owner->declaredFields.front().type)
        child = &candidate;
    if (!child || child->requiresActiveCleanup || child->destructor ||
        child->traits.copyable ||
        std::none_of(child->declaredFields.begin(), child->declaredFields.end(),
                     [](const auto &field) {
                       return field.type.kind ==
                                  lang::SemanticType::Reference &&
                              field.type.referenceAccess ==
                                  lang::AccessMode::Mutable;
                     }))
      continue;
    expect(cleanupConstructor == nullptr,
           "exclusive shell constructor fixture is unique");
    cleanupConstructor = &constructor;
    exclusiveShell = child->id;
  }
  expect(cleanupConstructor && exclusiveShell,
         "cleanup-bearing cursor uses an inline exclusive generated shell");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    if (cleanupConstructor && exclusiveShell) {
      const lang::MirBodyAddress constructorAddress{
          .kind = lang::MirBodyKind::Constructor,
          .owner = cleanupConstructor->id};
      expect(emitter.supportsFailureBodyText(constructorAddress),
             "exclusive generated shell has exact constructor rollback "
             "representation");
      const auto rejectShell = [&](auto mutate, std::string_view reason) {
        auto malformed = frontend.mir;
        auto *child = const_cast<lang::MirClassInstance *>(
            malformed.findClassInstance(exclusiveShell));
        auto *constructor = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(cleanupConstructor->id));
        mutate(*child, *constructor);
        const lang::CppMirBodyEmitter changedEmitter(malformed, map);
        expect(!changedEmitter.supportsFailureBodyText(constructorAddress),
               reason);
        expect(!changedEmitter.analyzeProgram().ready(),
               "detached representation planning rejects the forged exclusive "
               "shell");
      };
      rejectShell([](auto &child, auto &) { child.traits.copyable = true; },
                  "exclusive shell cannot claim copyable reference state");
      rejectShell(
          [](auto &child, auto &) {
            child.destructorStatus = lang::SpecialMemberStatus::Deleted;
          },
          "exclusive shell requires generated observation-free destruction");
      rejectShell(
          [](auto &, auto &constructor) {
            constructor.initializers.front().field = 0;
          },
          "exclusive shell cannot bypass exact native field publication");
    }
  }
}

void testRangeEndpointMarkerAuthority() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_mutable_inline_proxy_endpoint.gti";
  std::ifstream input(path);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "exclusive range endpoint has verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirFunctionInstance *selected = nullptr;
  std::size_t rangeIndex = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (std::size_t index = 0; index < function.body.rangeIterations.size();
         ++index)
      if (function.body.rangeIterations[index].endpointParking) {
        expect(selected == nullptr,
               "exclusive endpoint parking fixture is unique");
        selected = &function;
        rangeIndex = index;
      }
  expect(selected != nullptr, "exclusive range records exact endpoint parking");
  if (!selected)
    return;
  const auto &parking =
      *selected->body.rangeIterations[rangeIndex].endpointParking;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = selected->id};
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(supportsSelectedBodyText(frontend.mir, map, selected->id),
           "verified range endpoint markers have a backend representation");
    const auto text =
        selected->mayRaiseDefinedFailure
            ? emitter.emitFailureBodyText(address, "range-marker", 1)
            : emitter.emitBodyText(address, "range-marker", 1);
    expect(text.emitted() &&
               text.text.find("// GTI MIR park range cursor loan ") !=
                   std::string::npos &&
               text.text.find("// GTI MIR restore range cursor loan ") !=
                   std::string::npos,
           "endpoint parking erases to comments, not runtime state changes");
    const auto reject = [&](auto mutate, std::string_view reason) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(selected->id));
      mutate(changed->body);
      (void)lang::rebuildMirValueUses(changed->body);
      expect(!lang::verifyMirProgram(malformed).valid(), reason);
      expect(!supportsSelectedBodyText(malformed, map, selected->id),
             "backend refuses forged or unclaimed endpoint markers");
    };
    reject(
        [&](auto &body) {
          body.rangeIterations[rangeIndex].endpointParking.reset();
        },
        "endpoint marker cannot lose its exact range inventory");
    reject(
        [&](auto &body) { instruction(body, parking.park)->hirStatement = 0; },
        "endpoint marker retains source range identity");
    reject(
        [&](auto &body) { instruction(body, parking.restore)->hirValue = 0; },
        "restore retains the selected endpoint identity");
    reject(
        [&](auto &body) {
          instruction(body, parking.park)
              ->operands.push_back({.kind = lang::MirOperandKind::Constant,
                                    .type = lang::SemanticType::Int32});
        },
        "endpoint marker cannot hide an executable operand");
    reject(
        [&](auto &body) {
          body.rangeIterations[rangeIndex]
              .endpointParking->failureRestores.push_back(parking.restore);
        },
        "restore has one exact normal or failure inventory membership");
  }
}

void testScalarSiblingPositions() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_dual_range_scalar_siblings.gti";
  std::ifstream input(path);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "all scalar sibling positions have verified MIR");
  if (!frontend.canGenerateCode())
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    std::size_t checked = 0;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      if (constructor.initializers.size() != 3 ||
          std::count_if(constructor.initializers.begin(),
                        constructor.initializers.end(),
                        [](const auto &initializer) {
                          return initializer.ownedParameter.has_value();
                        }) != 2)
        continue;
      const auto bindings =
          lang::cppMirOwnedParameterFieldBindings(frontend.mir, constructor);
      expect(bindings && bindings->size() == 2,
             "leading, middle and trailing scalar siblings preserve exact move "
             "proof");
      expect(emitter.supportsFailureBodyText(
                 {.kind = lang::MirBodyKind::Constructor,
                  .owner = constructor.id}),
             "scalar sibling declaration positions do not alter constructor "
             "admission");
      ++checked;
    }
    expect(checked == 3,
           "all three scalar sibling positions exercise the backend gate");
    expect(
        !gti_test::emitCpp(frontend, standard).contents.empty(),
        "mixed source view carriers generate an artifact through verified MIR");
  }
}

void testSourceLiteralScalarSiblings() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_scalar_default_move.gti";
  std::ifstream input(path);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "unrelated generic owners and view defaults have verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirConstructorInstance *mixed = nullptr;
  for (const auto &constructor : frontend.mir.constructorInstances())
    if (constructor.initializers.size() == 3 &&
        constructor.initializers[0].ownedParameter &&
        !constructor.initializers[1].ownedParameter &&
        constructor.initializers[1].targetType == lang::SemanticType::UInt64 &&
        constructor.initializers[2].ownedParameter) {
      expect(mixed == nullptr, "literal-between-view fixture is unique");
      mixed = &constructor;
    }
  expect(mixed != nullptr,
         "literal scalar sibling separates two native field moves");
  if (!mixed)
    return;
  lang::MirInstructionId literal = 0, initialize = 0, boundary = 0;
  for (const auto &block : mixed->body.blocks)
    for (const auto &member : block.instructions) {
      if (member.hirValue == mixed->initializers[1].arguments.front() &&
          member.kind == lang::MirInstructionKind::Compute &&
          member.operation == lang::MirOperation::Literal)
        literal = member.id;
      if (member.constructorInitializer == 2)
        initialize = member.id;
      if (member.fullExpressionEnd)
        for (const auto &expression : mixed->body.fullExpressions)
          if (expression.id == member.fullExpressionEnd &&
              expression.constructorInitializer == 2)
            boundary = member.id;
    }
  expect(literal && initialize && boundary,
         "source literal has an exact executable publication triple");
  if (!literal || !initialize || !boundary)
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    std::size_t checked = 0;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      const auto *owner = frontend.mir.findClassInstance(constructor.owner);
      if (!owner)
        continue;
      const bool genericNode =
          constructor.initializers.size() == 3 &&
          owner->declaredFields.size() == 3 &&
          constructor.initializers[0].ownedParameter &&
          owner->declaredFields[1].type == lang::SemanticType::UInt64 &&
          owner->declaredFields[2].type == lang::SemanticType::UInt64;
      const bool leadingLiteral = constructor.initializers.size() == 2 &&
                                  !constructor.initializers[0].ownedParameter &&
                                  constructor.initializers[0].targetType ==
                                      lang::SemanticType::UInt64 &&
                                  constructor.initializers[1].ownedParameter;
      if (!genericNode && !leadingLiteral && constructor.id != mixed->id)
        continue;
      const auto bindings =
          lang::cppMirOwnedParameterFieldBindings(frontend.mir, constructor);
      expect(bindings &&
                 bindings->size() == (constructor.id == mixed->id ? 2u : 1u),
             "passive defaults preserve exact native move binding");
      expect(emitter.supportsFailureBodyText(
                 {.kind = lang::MirBodyKind::Constructor,
                  .owner = constructor.id}),
             "scalar and owning generic payloads preserve constructor "
             "representation beside source defaults");
      ++checked;
    }
    expect(checked == 4,
           "scalar, owning, leading and interleaved defaults are covered");
    const auto reject = [&](auto mutate, std::string_view reason) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(mixed->id));
      mutate(changed->body);
      (void)lang::rebuildMirValueUses(changed->body);
      expect(!lang::cppMirOwnedParameterFieldBindings(malformed, *changed),
             reason);
      expect(!lang::verifyMirProgram(malformed).valid(),
             "independent MIR verification rejects forged scalar literal "
             "publication");
      const lang::CppMirBodyEmitter changedEmitter(malformed, map);
      expect(!changedEmitter.supportsFailureBodyText(
                 {.kind = lang::MirBodyKind::Constructor, .owner = mixed->id}),
             "backend scalar prefix gate rejects malformed literal siblings");
    };
    reject([&](auto &body) { instruction(body, literal)->literal.reset(); },
           "literal sibling requires its exact typed value");
    reject(
        [&](auto &body) {
          instruction(body, literal)->operation = lang::MirOperation::Add;
        },
        "scalar prefix cannot hide runtime computation");
    reject(
        [&](auto &body) {
          instruction(body, literal)->literalProvenance.kind =
              lang::MirLiteralProvenanceKind::None;
        },
        "literal sibling requires canonical source provenance");
    reject(
        [&](auto &body) {
          instruction(body, initialize)->constructorInitializer = 0;
        },
        "literal publication retains its selected field identity");
    reject(
        [&](auto &body) { instruction(body, boundary)->fullExpressionEnd = 0; },
        "literal sibling retains its exact expression endpoint");
  }
}

} // namespace

int main() {
  testMixedCarrierParameterPrefix();
  testScalarSiblingPositions();
  testSourceLiteralScalarSiblings();
  testExclusiveGeneratedShell();
  testRangeEndpointMarkerAuthority();
  if (failures)
    return 1;
  std::cout << "Unrelated two-owner backend proof mutations passed\n";
  return 0;
}
