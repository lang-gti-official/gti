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

} // namespace

int main() {
  testMixedCarrierParameterPrefix();
  testScalarSiblingPositions();
  testRangeEndpointMarkerAuthority();
  if (failures)
    return 1;
  std::cout << "Unrelated two-owner backend proof mutations passed\n";
  return 0;
}
