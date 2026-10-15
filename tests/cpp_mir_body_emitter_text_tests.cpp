#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

void testFieldInitializerExpressionVocabulary() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-field-initializer-expressions.gti", R"(
enum class payload_t { one(int32_t value), none };

int32_t conversion_source = 7;

class expression_owner_t {
public:
  mut int32_t grouped = (7);
  mut int32_t converted = int32_t(7);
  mut int32_t converted_expression = int32_t(3 + 4);
  mut int8_t checked_conversion = int8_t(conversion_source);
};

class payload_owner_t {
public:
  mut payload_t empty = payload_t::none;
  mut payload_t valued = payload_t::one(int32_t(9));
};

int main() {
  mut expression_owner_t expressions = expression_owner_t();
  mut payload_owner_t payloads = payload_owner_t();
  return expressions.grouped - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the field-expression fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirClassInstance *expressionOwner = nullptr;
  const lang::MirClassInstance *payloadOwner = nullptr;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.declaredFields.size() == 4) {
      expressionOwner = &instance;
    } else if (instance.declaredFields.size() == 2) {
      payloadOwner = &instance;
    }
  }
  expect(expressionOwner != nullptr && payloadOwner != nullptr,
         "the fixture should retain its expression and payload field owners");
  if (expressionOwner == nullptr || payloadOwner == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMapRows rows =
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
  const lang::CppMirBodyEmissionMap map(rows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress expressionAddress{
      .kind = lang::MirBodyKind::FieldInitializers,
      .owner = expressionOwner->id};
  const lang::MirBodyAddress payloadAddress{
      .kind = lang::MirBodyKind::FieldInitializers, .owner = payloadOwner->id};
  const lang::CppMirInitializerScheduleText expressions =
      emitter.initializerSchedule(expressionAddress);
  const lang::CppMirInitializerScheduleText payloads =
      emitter.initializerSchedule(payloadAddress);
  expect(
      expressions.supported && expressions.fields.size() == 4 &&
          std::all_of(expressions.fields.begin(), expressions.fields.end(),
                      [](const auto &field) { return field.spelling.empty(); }),
      "selected constructors suppress duplicate native expression defaults");
  expect(
      payloads.supported && payloads.fields.size() == 2 &&
          std::all_of(payloads.fields.begin(), payloads.fields.end(),
                      [](const auto &field) { return field.spelling.empty(); }),
      "selected constructors suppress duplicate native payload defaults");

  std::size_t constructorSchedules = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = constructor.owner == expressionOwner->id
                            ? expressionOwner
                        : constructor.owner == payloadOwner->id ? payloadOwner
                                                                : nullptr;
    if (owner == nullptr)
      continue;
    ++constructorSchedules;
    expect(
        constructor.generatedDefault &&
            constructor.initializers.size() == owner->declaredFields.size(),
        "generated defaults retain a complete concrete initializer schedule");
    std::vector<lang::SymbolId> published;
    for (const auto &block : constructor.body.blocks)
      for (std::size_t position = 0; position < block.instructions.size();
           ++position) {
        const auto &instruction = block.instructions[position];
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.destination) {
          const auto *place =
              constructor.body.findPlace(*instruction.destination);
          if (place != nullptr && place->root == lang::MirPlaceRootKind::This &&
              place->projections.size() == 1 &&
              place->projections.front().kind == lang::MirProjectionKind::Field)
            published.push_back(place->projections.front().field);
          continue;
        }
        // A passive payload default publishes through its selected initializer
        // result, not a separate Initialize/place. Pin the typed root and exact
        // adjacent full-expression endpoint that authorize that publication.
        if (instruction.operation != lang::MirOperation::PayloadConstruct ||
            !instruction.result)
          continue;
        const auto *value = constructor.body.findValue(*instruction.result);
        for (std::size_t ordinal = 0; ordinal < constructor.initializers.size();
             ++ordinal) {
          const auto &initializer = constructor.initializers[ordinal];
          if (initializer.kind !=
                  lang::ConstructorInitializerTargetKind::Field ||
              initializer.arguments !=
                  std::vector<lang::HirValueId>{instruction.hirValue} ||
              initializer.targetType != instruction.info.type)
            continue;
          const auto full = std::find_if(
              constructor.body.fullExpressions.begin(),
              constructor.body.fullExpressions.end(),
              [&](const auto &candidate) {
                return candidate.constructorInitializer == ordinal + 1 &&
                       candidate.roots == initializer.arguments;
              });
          const bool exactPublication =
              value != nullptr && value->definition == instruction.id &&
              value->info == instruction.info &&
              full != constructor.body.fullExpressions.end() &&
              position + 1 < block.instructions.size() &&
              block.instructions[position + 1].kind ==
                  lang::MirInstructionKind::Lifecycle &&
              block.instructions[position + 1].fullExpressionEnd == full->id;
          expect(exactPublication,
                 "passive payload publication retains its selected typed "
                 "initializer and full-expression endpoint");
          if (exactPublication)
            published.push_back(initializer.field);
        }
      }
    std::vector<lang::SymbolId> expectedFields;
    for (const auto &field : owner->declaredFields)
      expectedFields.push_back(field.symbol);
    expect(
        published == expectedFields &&
            (emitter.supportsBodyText({.kind = lang::MirBodyKind::Constructor,
                                       .owner = constructor.id}) ||
             (constructor.mayRaiseDefinedFailure &&
              emitter.supportsFailureBodyText(
                  {.kind = lang::MirBodyKind::Constructor,
                   .owner = constructor.id}))),
        "constructor MIR publishes each exact field once in declaration order");
  }
  expect(constructorSchedules == 2,
         "both owners retain their selected generated constructor bodies");

  std::size_t identities = 0;
  std::size_t conversions = 0;
  std::size_t payloadConstructions = 0;
  for (const lang::MirClassInstance *owner : {expressionOwner, payloadOwner}) {
    for (const lang::MirBlock &block : owner->fieldInitializers.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        identities += instruction.operation == lang::MirOperation::Identity;
        conversions += instruction.operation == lang::MirOperation::Convert;
        payloadConstructions +=
            instruction.operation == lang::MirOperation::PayloadConstruct;
      }
    }
  }
  expect(identities == 1 && conversions == 4 && payloadConstructions == 2,
         "the source fixture should exercise the three newly scheduled MIR "
         "operations directly");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("std::int32_t converted;") !=
                 std::string::npos &&
             artifact.contents.find("payload_t empty;") != std::string::npos &&
             artifact.contents.find("(*this).converted") != std::string::npos &&
             artifact.contents.find("mir_checked_convert_v1<std::int8_t>") !=
                 std::string::npos &&
             artifact.contents.find("(*this).empty = std::move(") !=
                 std::string::npos &&
             artifact.contents.find("(*this).valued = std::move(") !=
                 std::string::npos &&
             artifact.contents.find("__gti_variant_1{}") != std::string::npos,
         "production emission executes conversion and payload publication in "
         "constructor MIR without duplicate in-class defaults");

  const auto mutableOwner = [](lang::MirProgram &program,
                               lang::HirClassInstanceId owner) {
    auto &instances = const_cast<std::vector<lang::MirClassInstance> &>(
        program.classInstances());
    const auto found =
        std::find_if(instances.begin(), instances.end(),
                     [&](const auto &item) { return item.id == owner; });
    return found == instances.end() ? nullptr : &*found;
  };
  const auto mutableOperation = [](lang::MirBody &body,
                                   lang::MirOperation operation,
                                   bool requireOperand = false) {
    for (lang::MirBlock &block : body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == operation &&
            (!requireOperand || !instruction.operands.empty())) {
          return &instruction;
        }
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  const auto rejectedSchedule = [&](const lang::MirProgram &program,
                                    const lang::CppMirBodyEmissionMap &rows,
                                    lang::MirBodyAddress address) {
    return !lang::CppMirBodyEmitter(program, rows)
                .initializerSchedule(address)
                .supported;
  };

  lang::MirProgram duplicateUse = frontend.mir;
  lang::MirClassInstance *duplicateOwner =
      mutableOwner(duplicateUse, expressionOwner->id);
  lang::MirInstruction *identity =
      duplicateOwner == nullptr
          ? nullptr
          : mutableOperation(duplicateOwner->fieldInitializers,
                             lang::MirOperation::Identity);
  if (identity != nullptr && identity->result &&
      *identity->result <= duplicateOwner->fieldInitializers.valueUses.size()) {
    auto &uses =
        duplicateOwner->fieldInitializers.valueUses[*identity->result - 1];
    if (!uses.empty()) {
      uses.push_back(uses.front());
    }
  }
  expect(identity != nullptr &&
             rejectedSchedule(duplicateUse, map, expressionAddress),
         "duplicated Identity result-use metadata must fail closed");

  lang::MirProgram wrongType = frontend.mir;
  lang::MirClassInstance *wrongTypeOwner =
      mutableOwner(wrongType, payloadOwner->id);
  lang::MirInstruction *wrongPayload =
      wrongTypeOwner == nullptr
          ? nullptr
          : mutableOperation(wrongTypeOwner->fieldInitializers,
                             lang::MirOperation::PayloadConstruct, true);
  if (wrongPayload != nullptr) {
    wrongPayload->operands.front().type = lang::SemanticType::UInt32;
  }
  expect(wrongPayload != nullptr &&
             rejectedSchedule(wrongType, map, payloadAddress),
         "payload operand type drift must fail closed");

  lang::MirProgram wrongConstructorPayload = frontend.mir;
  lang::MirConstructorInstance *changedConstructor = nullptr;
  for (const auto &constructor : wrongConstructorPayload.constructorInstances())
    if (constructor.owner == payloadOwner->id)
      changedConstructor =
          const_cast<lang::MirConstructorInstance *>(&constructor);
  auto *changedPayload =
      changedConstructor == nullptr
          ? nullptr
          : mutableOperation(changedConstructor->body,
                             lang::MirOperation::PayloadConstruct, true);
  if (changedPayload != nullptr) {
    changedPayload->operands.front().type = lang::SemanticType::UInt32;
  }
  expect(
      changedPayload != nullptr &&
          !lang::verifyMirProgram(wrongConstructorPayload).valid() &&
          !lang::CppMirBodyEmitter(wrongConstructorPayload, map)
               .supportsFailureBodyText({.kind = lang::MirBodyKind::Constructor,
                                         .owner = changedConstructor->id}) &&
          !lang::CppMirBodyEmitter(wrongConstructorPayload, map)
               .supportsBodyText({.kind = lang::MirBodyKind::Constructor,
                                  .owner = changedConstructor->id}),
      "selected constructor payload operand drift must fail closed");

  lang::MirProgram missingPublication = frontend.mir;
  bool removedPublication = false;
  for (const auto &constructor : missingPublication.constructorInstances()) {
    if (constructor.owner != expressionOwner->id)
      continue;
    auto &changed = const_cast<lang::MirConstructorInstance &>(constructor);
    for (auto &block : changed.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.constructorInitializer != 0) {
          instruction.constructorInitializer = 0;
          removedPublication = true;
          break;
        }
  }
  expect(removedPublication &&
             !lang::verifyMirProgram(missingPublication).valid(),
         "selected constructor field publication must retain its initializer "
         "identity");

  lang::MirProgram missingPayloadEndpoint = frontend.mir;
  bool removedEndpoint = false;
  for (const auto &constructor :
       missingPayloadEndpoint.constructorInstances()) {
    if (constructor.owner != payloadOwner->id)
      continue;
    auto &changed = const_cast<lang::MirConstructorInstance &>(constructor);
    if (!changed.body.fullExpressions.empty()) {
      changed.body.fullExpressions.front().constructorInitializer = 0;
      removedEndpoint = true;
    }
  }
  expect(removedEndpoint &&
             !lang::verifyMirProgram(missingPayloadEndpoint).valid(),
         "passive payload publication must retain its initializer endpoint "
         "identity");

  lang::MirProgram wrongOrder = frontend.mir;
  lang::MirClassInstance *wrongOrderOwner =
      mutableOwner(wrongOrder, expressionOwner->id);
  bool reordered = false;
  if (wrongOrderOwner != nullptr) {
    for (lang::MirBlock &block : wrongOrderOwner->fieldInitializers.blocks) {
      const auto consumer = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const auto &instruction) {
            return instruction.operation == lang::MirOperation::Identity;
          });
      if (consumer == block.instructions.end() || consumer->operands.empty()) {
        continue;
      }
      const lang::MirValue *source =
          wrongOrderOwner->fieldInitializers.findValue(
              consumer->operands.front().value);
      const auto producer =
          source == nullptr
              ? block.instructions.end()
              : std::find_if(block.instructions.begin(),
                             block.instructions.end(),
                             [&](const auto &instruction) {
                               return instruction.id == source->definition;
                             });
      if (producer != block.instructions.end() && producer < consumer) {
        std::iter_swap(producer, consumer);
        reordered = true;
      }
      break;
    }
  }
  expect(reordered && rejectedSchedule(wrongOrder, map, expressionAddress),
         "a consumer reordered before its exact producer must fail closed");

  lang::CppMirBodyEmissionMapRows staleRows = rows;
  const auto payloadEnum =
      std::find_if(staleRows.enums.begin(), staleRows.enums.end(),
                   [](const auto &enumeration) {
                     return !enumeration.payloadVariants.empty();
                   });
  if (payloadEnum != staleRows.enums.end()) {
    payloadEnum->payloadVariants.front().spelling.clear();
  }
  expect(payloadEnum != staleRows.enums.end() &&
             rejectedSchedule(frontend.mir,
                              lang::CppMirBodyEmissionMap(std::move(staleRows)),
                              payloadAddress),
         "stale copied payload metadata must fail closed");

  const lang::FrontendResult logicalNot =
      analyze("cpp-mir-body-logical-not-field-expression.gti", R"(
class logical_not_t {
  bool flag = !false;
};

int main() {
  logical_not_t value = logical_not_t();
  return 0;
}
)");
  expect(logicalNot.canGenerateCode(),
         "the LogicalNot field-expression probe should pass the frontend");
  if (logicalNot.canGenerateCode() &&
      !logicalNot.mir.classInstances().empty()) {
    const lang::CppMirBodyEmissionMap logicalNotMap(
        buildRows(logicalNot, logicalNot.mir, lang::CppStandard::Cpp23));
    const lang::MirClassInstance &owner =
        logicalNot.mir.classInstances().back();
    const lang::CppMirBodyEmitter logicalNotEmitter(logicalNot.mir,
                                                    logicalNotMap);
    const auto schedule = logicalNotEmitter.initializerSchedule(
        {.kind = lang::MirBodyKind::FieldInitializers, .owner = owner.id});
    bool sawLogicalNot = false;
    for (const auto &constructor : logicalNot.mir.constructorInstances()) {
      if (constructor.owner != owner.id)
        continue;
      for (const auto &block : constructor.body.blocks)
        for (const auto &instruction : block.instructions)
          sawLogicalNot |=
              instruction.operation == lang::MirOperation::LogicalNot;
      expect(logicalNotEmitter.supportsBodyText(
                 {.kind = lang::MirBodyKind::Constructor,
                  .owner = constructor.id}) ||
                 (constructor.mayRaiseDefinedFailure &&
                  logicalNotEmitter.supportsFailureBodyText(
                      {.kind = lang::MirBodyKind::Constructor,
                       .owner = constructor.id})),
             "general constructor MIR supports LogicalNot field execution");
    }
    expect(schedule.supported && schedule.fields.size() == 1 &&
               schedule.fields.front().spelling.empty() && sawLogicalNot,
           "LogicalNot defaults execute only in selected constructor MIR");
  }
}

void testInlineClosureChainEmission() {
  const lang::FrontendResult frontend = analyze("cpp-mir-closure-chain.gti", R"(
int main() {
  int offset = 3;
  auto add_offset = [offset](int value) -> int {
    return offset + value;
  };
  auto copied = add_offset;
  int doubled = [](int value) -> int {
    return value * 2;
  }(4);
  return copied(doubled) - 11;
}
)");
  expect(frontend.canGenerateCode(),
         "the closure-chain fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  expect(entry != frontend.mir.functionInstances().end(),
         "the closure-chain fixture should retain its entry instance");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready(),
         "the closure-chain entry should be analysis-Ready under production "
         "rows");
  expect(emitter.supportsFailureBodyText(address),
         "the closure-chain entry should prove its failure-form text");
  for (const lang::MirLambdaInstance &lambda : frontend.mir.lambdaInstances()) {
    expect(emitter.supportsBodyText(
               {.kind = lang::MirBodyKind::Lambda, .owner = lambda.id}),
           "each lambda body should prove its plain-shape nested text");
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "closure-test-v0", 1);
  const auto contains = [&](std::string_view needle) {
    return text.text.find(needle) != std::string::npos;
  };
  expect(contains("// GTI verified-MIR body: closure-test-v0 "
                  "lambda-instance 1") &&
             contains("// GTI verified-MIR body: closure-test-v0 "
                      "lambda-instance 2"),
         "both lambda bodies should emit nested banners inside the entry");
  expect(contains("[offset = __gti_mir_p_"),
         "the capture should spell its Capture row name over the enclosing "
         "place expression");
  expect(contains("mir_checked_multiply_v1") &&
             contains("mir_checked_add_v1") &&
             contains("__gti_mir_call_success_"),
         "checked arithmetic inside transformed literals should forward "
         "status through the enclosing call edge");
  expect(contains("__gti_mir_out_result") &&
             contains("__gti_mir_failure_record"),
         "the generated literal signature should carry the transformed "
         "result and failure channels");
  expect(contains("rejoins the fused closure chain") &&
             contains("joins the fused chain") &&
             contains("spells at its consuming invocation") &&
             !contains(".construct("),
         "the fused chain instructions should spell as comments only and "
         "must not materialize an omitted lambda-typed slot");
}

// Fusing the literal to a later invocation is sound only while every
// captured place stays frozen after the Closure. When a capture is rewritten,
// the closure must instead materialize at the exact Closure step so it keeps
// value-capture snapshot semantics.

void testClosureCaptureFreezeMaterializes() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-closure-freeze.gti", R"(
int main() {
  mut int x = 1;
  auto f = [x](int value) -> int {
    return x + value;
  };
  x = 2;
  return f(0) - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the capture-freeze fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  expect(entry != frontend.mir.functionInstances().end(),
         "the capture-freeze fixture should retain its entry instance");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(!emitter.supportsBodyText(address) &&
             emitter.supportsFailureBodyText(address),
         "a rewritten capture should select materialized failure-form "
         "closure emission");
  const std::string text =
      emitter.emitFailureBodyText(address, "closure-freeze-test-v0", 1).text;
  const std::size_t slotType =
      text.find("mir_lifetime_slot<__gti_mir_closure_type_");
  const std::size_t slotNameAt = slotType == std::string::npos
                                     ? std::string::npos
                                     : text.find("__gti_mir_p_", slotType);
  const std::size_t slotNameEnd = slotNameAt == std::string::npos
                                      ? std::string::npos
                                      : text.find(';', slotNameAt);
  const std::string slot =
      slotNameEnd == std::string::npos
          ? std::string{}
          : text.substr(slotNameAt, slotNameEnd - slotNameAt);
  const std::size_t construction =
      slot.empty() ? std::string::npos
                   : text.find(slot + ".construct_from", slotNameEnd);
  const std::size_t rewrite = text.find("static_cast<std::int32_t>(2)");
  const std::size_t invocation = slot.empty() || rewrite == std::string::npos
                                     ? std::string::npos
                                     : text.find(slot + ".get()(", rewrite);
  expect(text.find("__gti_mir_closure_factory_") != std::string::npos &&
             text.find("using __gti_mir_closure_type_") != std::string::npos &&
             text.find("was constructed at its Closure step") !=
                 std::string::npos &&
             // The invocation's prepared receiver borrows that slot.
             text.find("aliases the caller-owned receiver lifetime") !=
                 std::string::npos &&
             text.find("joins the fused chain") == std::string::npos &&
             text.find("rejoins the fused closure chain") ==
                 std::string::npos &&
             construction != std::string::npos &&
             rewrite != std::string::npos && invocation != std::string::npos &&
             construction < rewrite && rewrite < invocation,
         "a rewritten capture should snapshot into a materialized slot before "
         "the source place changes and invoke that slot afterward");
}

// A class capture has lexical cleanup even when it is copied. MIR publishes
// that closure lifetime at the destination Initialize step; it does not
// duplicate an Initialize event on the Closure instruction. Passing the
// stored closure through a deduced-callable template must preserve that exact
// schedule through production failure-form emission.

void testLexicalCopyCaptureThroughCallableTemplate() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-lexical-copy-capture.gti", R"(
struct Scalar {
  int value = 0;
};

int inspect_transfer<std::transferable T>(T value) {
  return value();
}

int main() {
  Scalar scalar = Scalar();
  auto callable = [scalar]() -> int { return 1; };
  int result = inspect_transfer(callable);
  return result - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the lexical copy-capture fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *callableTemplate = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (!function.callableParameters.empty()) {
      callableTemplate = &function;
    }
  }
  expect(entry != nullptr && callableTemplate != nullptr,
         "the lexical copy-capture fixture should retain its entry and "
         "callable-template instances");
  if (entry == nullptr || callableTemplate == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "a lexical class copy-capture should materialize from its "
         "destination Reparent schedule");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto productionMarker = [&](lang::HirFunctionInstanceId id) {
    return artifact.contents.find("function-instance " + std::to_string(id) +
                                  "\n") != std::string::npos;
  };
  expect(productionMarker(entry->id) &&
             productionMarker(callableTemplate->id) &&
             artifact.contents.find("__gti_mir_closure_factory_") !=
                 std::string::npos,
         "production emission should publish both the materialized caller and "
         "its callable-template body from verified MIR");
}

// A closure local initialized by a copy, a closure-returning call or a move is
// constructed in place by that producer, and the same proof declares the
// local as a lifetime slot. A deduced-callable body therefore declares and
// constructs one slot per local even though it creates no literal of the
// closure type. A passive closure's slot is representation-only, so each
// producer first retires the previous representation, as a loop rebuilding a
// copy of a materialized closure requires.

void testProducedClosureLocalSlots() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-produced-closure-locals.gti", R"(
#include <std/utility>

F pass<F>(F f) { return f; }

int call<F>(F f) { return f(); }

int relay<F>(F f) {
  F copied = f;
  F returned = pass(copied);
  F moved = std::move(returned);
  return call(std::move(moved));
}

int main() {
  int k = 3;
  mut int x = 1;
  auto local = [x]() -> int { return x; };
  x = 2;
  mut int total = relay([k]() -> int { return k; });
  for (mut int index = 0; index < 2; index++) {
    auto copy = local;
    total += call(std::move(copy));
  }
  return total - 5;
}
)");
  expect(frontend.canGenerateCode(),
         "the produced closure-local fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  // The definition (not a declaration) whose signature contains `marker`,
  // through its closing brace at the definition's own indentation.
  const auto definition = [&](std::string_view marker) {
    for (std::size_t at = text.find(marker); at != std::string::npos;
         at = text.find(marker, at + marker.size())) {
      const std::size_t lineStart = text.rfind('\n', at) + 1;
      const std::size_t lineEnd = text.find('\n', at);
      if (lineEnd == std::string::npos || text[lineEnd - 1] != '{') {
        continue;
      }
      const std::size_t indent =
          text.find_first_not_of(' ', lineStart) - lineStart;
      const std::string closing = "\n" + std::string(indent, ' ') + "}\n";
      const std::size_t end = text.find(closing, lineEnd);
      return end == std::string::npos
                 ? std::string{}
                 : text.substr(lineStart, end + closing.size() - lineStart);
    }
    return std::string{};
  };
  const auto count = [](const std::string &body, std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = body.find(needle); at != std::string::npos;
         at = body.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  const auto startsAt = [](const std::string &body, std::size_t at,
                           std::string_view needle) {
    return at != std::string::npos &&
           body.compare(at, needle.size(), needle) == 0;
  };
  // The number of representation-only slots declared for `type`, provided
  // each is rearmed immediately before its one construction.
  const auto rearmedSlots =
      [&](const std::string &body,
          std::string_view type) -> std::optional<std::size_t> {
    const std::string declaration =
        "::gti_internal::backend::mir_lifetime_slot<" + std::string(type) +
        ", true> ";
    std::size_t slots = 0;
    for (std::size_t at = body.find(declaration); at != std::string::npos;
         at = body.find(declaration, at + declaration.size())) {
      const std::size_t nameAt = at + declaration.size();
      const std::string name =
          body.substr(nameAt, body.find(';', nameAt) - nameAt);
      const std::string rearm = name + ".destroy_if_engaged();\n";
      const std::string construction = name + ".construct";
      const std::size_t rearmAt = body.find(rearm);
      if (rearmAt == std::string::npos ||
          !startsAt(body, body.find_first_not_of(' ', rearmAt + rearm.size()),
                    construction) ||
          count(body, construction) != 1) {
        return std::nullopt;
      }
      ++slots;
    }
    return slots;
  };
  const std::string relay = definition("_relay__gti_mir_failure(F ");
  const std::size_t plainLocal = relay.find(" F __gti_mir_p_");
  expect(!relay.empty() && rearmedSlots(relay, "F") == 3 &&
             count(relay, "was constructed at its producing step") == 3 &&
             count(relay, "joins the fused chain") == 0,
         "the copied, call-returned and moved closure locals should each be "
         "a representation-only slot constructed by its producer");
  expect(count(relay, " F __gti_mir_p_") == 1 &&
             startsAt(relay,
                      plainLocal == std::string::npos
                          ? std::string::npos
                          : relay.find(" = ", plainLocal),
                      " = __gti_mir_arg_0;"),
         "only the closure parameter should declare a plain closure value");
  const std::string entry = definition("bool __gti_entry__gti_mir_failure(");
  std::size_t entrySlots = 0;
  bool entryRearmed = true;
  constexpr std::string_view alias = "using ";
  constexpr std::string_view closureType = "__gti_mir_closure_type_";
  const std::string closureAlias =
      std::string(alias) + std::string(closureType);
  for (std::size_t at = entry.find(closureAlias); at != std::string::npos;
       at = entry.find(closureAlias, at + closureAlias.size())) {
    const std::size_t nameAt = at + alias.size();
    const std::optional<std::size_t> slots = rearmedSlots(
        entry, entry.substr(nameAt, entry.find(" = ", nameAt) - nameAt));
    entryRearmed = entryRearmed && slots.has_value();
    entrySlots += slots.value_or(0);
  }
  expect(entryRearmed && entrySlots == 2 &&
             count(entry, "was constructed at its producing step") == 1,
         "the materialized closure and its loop copy should each rebuild "
         "their representation-only slot after retiring the previous one");
}

// A constructor that copies its closure parameter into a field can use that
// parameter again in a body that may fail. A struct capture gives the field a
// lexical drop, but the record-published copy still constructs it in the
// native member-initializer list, and ending it on rollback runs no source
// code. Both closure instances therefore keep failure-form constructors. Every
// bare MIR place of a copied formal, closure or class, spells the one alias of
// the native parameter, never a lifetime slot that nothing constructed.

void testCopiedFormalReusedInConstructorBody() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-copied-formal-reuse.gti", R"(
#include <std/utility>

int call<F>(F f) { return f(); }

class Keeper<F> {
public:
  F callback;
  mut int seen;
  Keeper(F f) : callback(f), seen(0) {
    F spare = f;
    this.seen = call(std::move(spare)) + call(f);
  }
};

int keep<F>(F f) {
  Keeper<F> value = Keeper<F>(f);
  return value.seen;
}

struct Offset {
  int amount;
  Offset(int amount) : amount(amount) {}
};

int amount(Offset offset) { return offset.amount; }

class Stamp {
public:
  Offset origin;
  int total;
  Stamp(Offset offset) : origin(offset), total(amount(offset)) {}
};

int main() {
  int k = 3;
  Offset offset{4};
  Stamp stamp{offset};
  return keep([offset]() -> int { return offset.amount; }) +
         keep([k]() -> int { return k; }) + stamp.total - 18;
}
)");
  expect(frontend.canGenerateCode(),
         "the copied-formal reuse fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  std::string text;
  try {
    text = gti_test::emitCpp(frontend).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("a copied formal reused in a constructor body "
                              "should emit: ") +
                      error.what());
    return;
  }
  // Every definition whose first line contains `marker`, through its closing
  // brace at the definition's own indentation.
  const auto definitions = [&](std::string_view marker) {
    std::vector<std::string> found;
    for (std::size_t at = text.find(marker); at != std::string::npos;
         at = text.find(marker, at + marker.size())) {
      const std::size_t lineStart = text.rfind('\n', at) + 1;
      const std::size_t lineEnd = text.find('\n', at);
      if (lineEnd == std::string::npos || text[lineEnd - 1] != '{') {
        continue;
      }
      const std::size_t indent =
          text.find_first_not_of(' ', lineStart) - lineStart;
      const std::string closing = "\n" + std::string(indent, ' ') + "}\n";
      const std::size_t end = text.find(closing, lineEnd);
      if (end != std::string::npos) {
        found.push_back(
            text.substr(lineStart, end + closing.size() - lineStart));
      }
    }
    return found;
  };
  const auto count = [](const std::string &body, std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = body.find(needle); at != std::string::npos;
         at = body.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  // The one alias of the copied parameter, provided no other carrier of the
  // formal is declared and nothing reads the alias as a lifetime slot.
  const auto parameterAlias =
      [&](const std::string &body) -> std::optional<std::string> {
    constexpr std::string_view declaration = "auto &";
    constexpr std::string_view source = " = __gti_mir_arg_0;\n";
    const std::size_t sourceAt = body.find(source);
    const std::size_t nameAt = body.rfind(declaration, sourceAt);
    if (sourceAt == std::string::npos || nameAt == std::string::npos ||
        count(body, source) != 1) {
      return std::nullopt;
    }
    std::string alias = body.substr(nameAt + declaration.size(),
                                    sourceAt - nameAt - declaration.size());
    if (count(body, alias + ".get()") != 0 ||
        count(body, "std::move(__gti_mir_arg_0)") != 0) {
      return std::nullopt;
    }
    return alias;
  };
  // The slot that the first `.construct(alias)` constructs.
  const auto copiedSlot = [&](const std::string &body,
                              const std::string &alias) {
    const std::string construction = ".construct(" + alias + ");";
    const std::size_t at = body.find(construction);
    if (at == std::string::npos) {
      return std::string{};
    }
    const std::size_t nameAt = body.rfind(' ', at) + 1;
    return body.substr(nameAt, at - nameAt);
  };

  const std::vector<std::string> keepers = definitions(
      "Keeper<F>::Keeper(::gti_internal::backend::mir_failure_constructor_"
      "tag_v1<");
  std::size_t lexical = 0;
  std::size_t passive = 0;
  for (const std::string &body : keepers) {
    const std::optional<std::string> alias = parameterAlias(body);
    const std::string spare = alias ? copiedSlot(body, *alias) : "";
    expect(alias.has_value() &&
               count(body, ") : callback(__gti_mir_arg_0), ") == 1 &&
               count(body, ".construct(" + *alias + ");") == 2,
           "each closure instance should copy the field from the parameter "
           "in its member-initializer list, then copy the local and the "
           "call argument from the one parameter alias");
    if (spare.empty()) {
      continue;
    }
    // A struct capture makes the local's drop lexical: a real slot that the
    // success and every failure edge destroy. A number capture keeps a
    // representation-only slot with no MIR drop.
    if (count(body, "mir_lifetime_slot<F> " + spare + ";") == 1 &&
        count(body, spare + ".destroy();\n") == 1 &&
        count(body, spare + ".destroy(); // failure cleanup") >= 1) {
      ++lexical;
    }
    if (count(body, "mir_lifetime_slot<F, true> " + spare + ";") == 1 &&
        count(body, spare + ".destroy()") == 0) {
      ++passive;
    }
  }
  expect(keepers.size() == 2 && lexical == 1 && passive == 1,
         "the struct- and number-capturing instances should each keep a "
         "failure-form constructor with their own local drop shape");

  const std::vector<std::string> stamps = definitions("Stamp::Stamp(");
  expect(!stamps.empty() &&
             std::all_of(
                 stamps.begin(), stamps.end(),
                 [&](const std::string &body) {
                   const std::optional<std::string> alias =
                       parameterAlias(body);
                   return alias.has_value() &&
                          count(body, ") : origin(__gti_mir_arg_0), ") == 1 &&
                          count(body, ".construct(" + *alias + ");") == 1;
                 }),
         "a class parameter copied into a field and passed on should read "
         "its one alias rather than an unconstructed slot");
}

// A constructor that moves its closure parameter into a field can still fail
// afterwards, in its body or in a later sibling initializer. MIR publishes
// the move through the constructor record. Only effect-free initializer
// records may precede it, so the native member-initializer list constructs
// the field exactly where MIR moves the formal, in the failure form too; a
// struct capture's lexical field drop ends with the caller's object like a
// trivial one. The proof never hoists the move across earlier body work.

void testMovedClosureFieldInFallibleConstructor() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-moved-closure-field.gti", R"(
#include <std/utility>

int call<F>(F f) { return f(); }

class Keeper<F> {
public:
  F callback;
  mut int seen;
  Keeper(F f) : callback(std::move(f)), seen(0) {
    this.seen = call(this.callback);
  }
};

class Sibling<F> {
public:
  F callback;
  int seen;
  Sibling(F f) : callback(std::move(f)), seen(call([]() -> int { return 5; })) {}
};

int keep<F>(F f) {
  Keeper<F> value = Keeper<F>(f);
  Sibling<F> sibling = Sibling<F>(f);
  return value.seen + sibling.seen;
}

struct Offset {
  int amount;
  Offset(int amount) : amount(amount) {}
};

int main() {
  int k = 3;
  Offset offset{4};
  return keep([offset]() -> int { return offset.amount; }) +
         keep([k]() -> int { return k; }) - 17;
}
)");
  expect(frontend.canGenerateCode(),
         "the moved-closure-field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto movesClosure = [](const lang::MirConstructorInstance &candidate) {
    return std::any_of(
        candidate.initializers.begin(), candidate.initializers.end(),
        [](const lang::MirConstructorInitializer &initializer) {
          return initializer.ownedParameter.has_value() &&
                 initializer.targetType.kind == lang::SemanticType::Lambda;
        });
  };
  std::size_t provenMoves = 0;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (!movesClosure(constructor)) {
      continue;
    }
    const auto bindings =
        lang::cppMirOwnedParameterFieldBindings(frontend.mir, constructor);
    if (constructor.mayRaiseDefinedFailure &&
        !constructor.body.failureRecords.empty() && bindings &&
        bindings->size() == 1 && bindings->front().initializeInstruction == 0 &&
        bindings->front().valueDrop == 0 && bindings->front().fieldDrop == 0) {
      ++provenMoves;
    }
  }
  expect(provenMoves == 4,
         "both closure instances of both fallible constructors should prove "
         "their record-published move as a native owned-field publication");

  std::string text;
  try {
    text = gti_test::emitCpp(frontend).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("a closure field moved in a fallible "
                              "constructor should emit: ") +
                      error.what());
    return;
  }
  // Every definition whose first line contains `marker`, through its closing
  // brace at the definition's own indentation.
  const auto definitions = [&](std::string_view marker) {
    std::vector<std::string> found;
    for (std::size_t at = text.find(marker); at != std::string::npos;
         at = text.find(marker, at + marker.size())) {
      const std::size_t lineStart = text.rfind('\n', at) + 1;
      const std::size_t lineEnd = text.find('\n', at);
      if (lineEnd == std::string::npos || text[lineEnd - 1] != '{') {
        continue;
      }
      const std::size_t indent =
          text.find_first_not_of(' ', lineStart) - lineStart;
      const std::string closing = "\n" + std::string(indent, ' ') + "}\n";
      const std::size_t end = text.find(closing, lineEnd);
      if (end != std::string::npos) {
        found.push_back(
            text.substr(lineStart, end + closing.size() - lineStart));
      }
    }
    return found;
  };
  const auto count = [](const std::string &body, std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = body.find(needle); at != std::string::npos;
         at = body.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  // The member-initializer list moves the parameter once, and the body only
  // records that publication: it neither aliases nor moves the formal again.
  const auto nativeMove = [&](const std::string &body) {
    return count(body, ") : callback(std::move(__gti_mir_arg_0)), seen(") ==
               1 &&
           count(body, "std::move(__gti_mir_arg_0)") == 1 &&
           count(body, " = __gti_mir_arg_0;") == 0 &&
           count(body, " publishes in the member initializer list\n") == 1;
  };
  for (std::string_view owner : {"Keeper", "Sibling"}) {
    const std::vector<std::string> bodies = definitions(
        std::string(owner) + "<F>::" + std::string(owner) +
        "(::gti_internal::backend::mir_failure_constructor_tag_v1<");
    // The struct-capturing formal keeps its lexical drop on both exits; it
    // retires only the moved-from native parameter.
    const auto lexical = std::count_if(
        bodies.begin(), bodies.end(), [&](const std::string &body) {
          return count(body,
                       "// GTI MIR owned-field parameter drop of place") == 2;
        });
    expect(bodies.size() == 2 &&
               std::all_of(bodies.begin(), bodies.end(), nativeMove) &&
               lexical == 1,
           std::string(owner) +
               " should keep one failure-form constructor per closure "
               "instance, each publishing the moved field natively");
  }

  // Native publication stays exact only while no body work precedes the
  // move. Replaying the later scalar sibling first must fail closed.
  {
    lang::MirProgram mutated = frontend.mir;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            mutated.constructorInstances());
    bool reordered = false;
    for (lang::MirConstructorInstance &constructor : constructors) {
      const auto bindings =
          movesClosure(constructor)
              ? lang::cppMirOwnedParameterFieldBindings(mutated, constructor)
              : std::nullopt;
      lang::MirBlock *entry =
          constructor.body.findBlock(constructor.body.entry);
      if (reordered || !bindings || bindings->size() != 1 || entry == nullptr) {
        continue;
      }
      auto &instructions = entry->instructions;
      const auto move = std::find_if(
          instructions.begin(), instructions.end(), [&](const auto &item) {
            return item.id == bindings->front().moveInstruction;
          });
      // Keeper's `seen(0)` sibling publishes in the three records after the
      // move and its boundary: a Literal, its Initialize and a boundary.
      if (move != instructions.begin() ||
          std::distance(move, instructions.end()) < 5 ||
          std::next(move, 2)->kind != lang::MirInstructionKind::Compute ||
          std::next(move, 2)->operation != lang::MirOperation::Literal) {
        continue;
      }
      std::rotate(move, std::next(move, 2), std::next(move, 5));
      reordered = true;
      expect(!lang::cppMirOwnedParameterFieldBindings(mutated, constructor),
             "a move recorded after a later sibling's publication must not "
             "be hoisted into the member-initializer list");
    }
    expect(reordered, "the mutation should find a Keeper constructor");
  }

  // A fallible sibling initializer declared before the closure field runs
  // before the move. The owned proof never reorders the move across it.
  const lang::FrontendResult late =
      analyzeWithStandardLibrary("cpp-mir-moved-closure-late-field.gti", R"(
#include <std/utility>

int call<F>(F f) { return f(); }

class Late<F> {
public:
  int seen;
  F callback;
  Late(F f) : seen(call([]() -> int { return 4; })), callback(std::move(f)) {}
};

int late<F>(F f) {
  Late<F> value = Late<F>(f);
  return value.seen;
}

struct Offset {
  int amount;
  Offset(int amount) : amount(amount) {}
};

int main() {
  int k = 3;
  Offset offset{4};
  return late([offset]() -> int { return offset.amount; }) +
         late([k]() -> int { return k; }) - 8;
}
)");
  expect(late.canGenerateCode(),
         "the late moved-closure-field fixture should pass the frontend");
  if (!late.canGenerateCode()) {
    return;
  }
  std::size_t lateConstructors = 0;
  for (const lang::MirConstructorInstance &constructor :
       late.mir.constructorInstances()) {
    if (!movesClosure(constructor)) {
      continue;
    }
    ++lateConstructors;
    expect(!constructor.body.failureRecords.empty() &&
               !lang::cppMirOwnedParameterFieldBindings(late.mir, constructor),
           "a move after a fallible sibling must not publish natively");
  }
  expect(lateConstructors == 2,
         "both late closure instances should keep their own constructor");
}

void testInlineCapturingLiteralAsAlgorithmArgument() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-inline-capture-argument.gti", R"(
#include <std/vector>
#include <std/algorithm>

int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(3);
  values.push_back(8);
  values.push_back(5);
  int32_t limit = 4;
  std::ptrdiff_t above = std::count_if(
      values.begin(), values.end(),
      [limit](int32_t& value) -> bool { return value > limit; });
  return above == 2 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the inline capture-argument fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  expect(entry != frontend.mir.functionInstances().end(),
         "the inline capture fixture should retain its entry instance");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::MirInstruction *closure = nullptr;
  lang::MirBlockId closureBlock = 0;
  lang::MirBlockId writerBlock = 0;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Compute &&
          instruction.operation == lang::MirOperation::Closure) {
        closure = &instruction;
        closureBlock = block.id;
      }
    }
  }
  if (closure != nullptr && closure->operands.size() == 1) {
    for (const lang::MirBlock &block : entry->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.destination &&
            *instruction.destination == closure->operands.front().place) {
          writerBlock = block.id;
        }
      }
    }
  }
  const std::optional<lang::MirDominanceInfo> dominance =
      lang::computeMirDominance(entry->body);
  expect(closure != nullptr && writerBlock != 0 &&
             writerBlock != closureBlock && writerBlock != entry->body.entry &&
             dominance && dominance->dominates(writerBlock, closureBlock),
         "the capture initializer should be a distinct dominating, "
         "non-entry block");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a single-shot dominating capture initializer should admit fused "
         "inline-literal emission");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("__gti_mir_closure_factory_") !=
             std::string::npos,
         "the now-scheduled borrowed iterator inputs should stage the closure "
         "through its factory over the frozen capture");
}

void testInlineCaptureControlFlowProofFailsClosed() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-inline-capture-cfg.gti", R"(
#include <std/vector>
#include <std/algorithm>

int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(3);
  int32_t limit = 2;
  std::ptrdiff_t above = std::count_if(
      values.begin(), values.end(),
      [limit](int32_t& value) -> bool { return value > limit; });
  return above == 1 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the inline capture CFG fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  if (entry == frontend.mir.functionInstances().end()) {
    expect(false, "the CFG fixture should retain its entry instance");
    return;
  }

  lang::MirBlockId closureBlock = 0;
  lang::MirBlockId writerBlock = 0;
  lang::MirPlaceId capturedPlace = 0;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Compute &&
          instruction.operation == lang::MirOperation::Closure &&
          instruction.operands.size() == 1) {
        closureBlock = block.id;
        capturedPlace = instruction.operands.front().place;
      }
    }
  }
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.destination &&
          *instruction.destination == capturedPlace) {
        writerBlock = block.id;
      }
    }
  }
  expect(writerBlock != 0 && closureBlock != 0 && writerBlock != closureBlock,
         "the CFG near misses require distinct writer and closure blocks");
  if (writerBlock == 0 || closureBlock == 0 || writerBlock == closureBlock) {
    return;
  }

  lang::MirProgram bypass = frontend.mir;
  auto &bypassFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      bypass.functionInstances());
  lang::MirBody &bypassBody = bypassFunctions.at(entry->id - 1).body;
  lang::MirBlock *bypassEntry = bypassBody.findBlock(bypassBody.entry);
  expect(bypassEntry != nullptr && bypassEntry->terminator.target != 0,
         "the CFG fixture entry should expose one success edge");
  if (bypassEntry != nullptr && bypassEntry->terminator.target != 0) {
    bypassEntry->terminator.target = closureBlock;
    const std::optional<lang::MirDominanceInfo> bypassDominance =
        lang::computeMirDominance(bypassBody);
    expect(bypassDominance &&
               !bypassDominance->dominates(writerBlock, closureBlock) &&
               !lang::verifyMirProgram(bypass).valid(),
           "an edge bypassing capture initialization must fail verification "
           "before materialized closure emission");
  }

  lang::MirProgram repeated = frontend.mir;
  auto &repeatedFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          repeated.functionInstances());
  lang::MirBody &repeatedBody = repeatedFunctions.at(entry->id - 1).body;
  lang::MirBlock *repeatedClosure = repeatedBody.findBlock(closureBlock);
  expect(repeatedClosure != nullptr && repeatedClosure->terminator.target != 0,
         "the closure block should expose its successful call edge");
  if (repeatedClosure != nullptr && repeatedClosure->terminator.target != 0) {
    const lang::MirBlockId latch =
        static_cast<lang::MirBlockId>(repeatedBody.blocks.size() + 1);
    repeatedClosure->terminator.target = latch;
    repeatedBody.blocks.push_back(
        {.id = latch,
         .terminator = {.kind = lang::MirTerminatorKind::Goto,
                        .target = writerBlock},
         .reachable = true});
    const std::optional<lang::MirDominanceInfo> repeatedDominance =
        lang::computeMirDominance(repeatedBody);
    const lang::MirLoopAnalysis loops =
        repeatedDominance
            ? lang::analyzeMirLoops(repeatedBody, *repeatedDominance)
            : lang::MirLoopAnalysis{};
    const bool writerRepeats =
        std::any_of(loops.loops.begin(), loops.loops.end(),
                    [&](const lang::MirNaturalLoop &loop) {
                      return loop.contains(writerBlock);
                    });
    expect(repeatedDominance && writerRepeats &&
               !lang::verifyMirProgram(repeated).valid(),
           "capture initialization on a re-entered loop path must fail "
           "verification before materialized closure emission");
  }
}

// A generated default construction can publish without first becoming a
// source binding. A by-value generic call uses its prepared-parameter slot;
// Expected<T, E> constructs its destination directly from T{}. Each route is
// admitted only by its recorded Initialize/TransferOut schedule.

void testGenericMemberFailureOverloadOnOrdinaryOwner() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-generic-member-ordinary-owner.gti", R"(
class Total {
public:
  int add<uint64_t N>(int values[N]) {
    return values[0];
  }
};

int main() {
  Total total = Total();
  return total.add({7}) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic-member ordinary-owner fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *member = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (function.owner && !function.parameterTypes.empty() &&
        function.parameterTypes.front().kind == lang::SemanticType::Array) {
      member = &function;
    }
  }
  expect(entry != nullptr && member != nullptr,
         "the generic-member fixture should retain its entry and concrete "
         "member instance");
  if (entry == nullptr || member == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const lang::MirFunctionInstance *function : {entry, member}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "the generic member and its caller should support transformed MIR "
           "emission");
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string transformedPrefix =
      "bool __gti_program::Total::__gti_member_";
  const std::size_t transformed = text.find(transformedPrefix);
  const std::size_t transformedName =
      transformed == std::string::npos
          ? std::string::npos
          : text.find("_add__gti_mir_failure(", transformed);
  const bool concreteName =
      transformedName != std::string::npos &&
      transformedName == transformed + transformedPrefix.size() + 64;
  expect(text.find("function-instance " + std::to_string(entry->id) + "\n") !=
                 std::string::npos &&
             text.find("function-instance " + std::to_string(member->id) +
                       "\n") != std::string::npos &&
             concreteName &&
             text.find("template <> " + transformedPrefix) == std::string::npos,
         "production emission should define the concrete transformed member "
         "overload with ordinary-owner C++ syntax");
}

// The deduced-callable template vocabulary (task: CallableDispatch): a
// Function body with callable parameters uses a transformed template shape:
// its callable parameter place declares only under an overlay type row, and
// invoking it returns a success flag while forwarding the current failure
// record. The caller passes a transformed fused lambda with the same private
// convention. Without the overlay row the body stays outside the vocabulary.

void testCallableTemplateBodyVocabulary() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-callable-template.gti", R"(
void invoke<Operation>(Operation operation) {
  operation(42);
}

int main() {
  auto report = [](int value) -> void {
    if (value == 42) {
      return;
    }
  };
  invoke(report);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the callable-template fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *invoke = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (!function.callableParameters.empty()) {
      invoke = &function;
    }
  }
  expect(invoke != nullptr && !invoke->parameterTypes.empty(),
         "the fixture should lower the callable-parameter instance");
  if (invoke == nullptr || invoke->parameterTypes.empty()) {
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = invoke->id};
  lang::CppMirBodyEmissionMapRows production =
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
  {
    lang::CppMirBodyEmissionMapRows copy = production;
    const lang::CppMirBodyEmissionMap withoutOverlay{std::move(copy)};
    const lang::CppMirBodyEmitter emitter(frontend.mir, withoutOverlay);
    expect(!emitter.supportsBodyText(address) &&
               !emitter.supportsFailureBodyText(address),
           "without the overlay row the callable-template body must stay "
           "outside the vocabulary");
  }
  const std::optional<lang::CppMirTypeRepresentationKind> callableKind =
      lang::cppMirExpectedTypeRepresentation(invoke->parameterTypes.front());
  expect(callableKind.has_value(),
         "the callable type should classify for representation");
  if (!callableKind) {
    return;
  }
  production.types.push_back({.type = invoke->parameterTypes.front(),
                              .kind = *callableKind,
                              .spelling = "Operation"});
  const lang::CppMirBodyEmissionMap map(std::move(production));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(!emitter.supportsBodyText(address) &&
             emitter.supportsFailureBodyText(address),
         "under the overlay row the callable-template body should prove "
         "its transformed text");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "callable-template-test-v0", 1);
  const auto contains = [&](std::string_view needle) {
    return text.text.find(needle) != std::string::npos;
  };
  expect(contains("Operation __gti_mir_p_1 = __gti_mir_arg_0;"),
         "the callable parameter local should declare with the template "
         "parameter spelling");
  expect(contains("__gti_mir_call_success_") &&
             contains("__gti_mir_p_1(__gti_mir_v_") &&
             contains("__gti_mir_failure_record"),
         "the invocation should stage the callable parameter place "
         "directly through the transformed channel");
  expect(contains("GTI MIR propagate failure-record") &&
             contains("return false;"),
         "the callable failure continuation should propagate its exact "
         "record through the failure-form ABI");
}

// An explicitly moved callable receiver may resolve to a mutable operator()
// when no consuming overload has the requested signature. MIR records both
// facts independently: MoveValue transports the receiver, while the selected
// callable capability names the exact overload. The C++ body emitter must not
// collapse those facts into an equality check.

void testMovedCallableReceiverMutableFallback() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-moved-callable-fallback.gti", R"(
class fallback_counter {
public:
  int operator()(int value) mut {
    return value + 1;
  }

  int operator()() && {
    return 0;
  }
};

int invoke_once<Operation>(Operation operation) {
  return std::move(operation)(7);
}

int main() {
  fallback_counter operation{};
  return invoke_once(operation) - 8;
}
)");
  expect(frontend.canGenerateCode(),
         "the moved callable fallback fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *fallback = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (std::any_of(function.callableParameters.begin(),
                    function.callableParameters.end(),
                    [](const lang::MirCallableParameter &parameter) {
                      return std::any_of(
                          parameter.signatures.begin(),
                          parameter.signatures.end(),
                          [](const lang::MirCallableSignature &signature) {
                            return signature.selectedCapability ==
                                   lang::CallableInvocationCapability::Mutable;
                          });
                    })) {
      fallback = &function;
    }
  }
  expect(fallback != nullptr && entry != nullptr,
         "the fixture should retain the mutable fallback and entry bodies");
  if (fallback == nullptr || entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = fallback->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "MoveValue transport should admit the resolved mutable callable "
         "fallback");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const std::string body =
      emitter.emitFailureBodyText(address, "moved-fallback-test-v0", 1).text;
  expect(body.find("std::move(") != std::string::npos &&
             body.find("__gti_mir_call_success_") != std::string::npos &&
             body.find("std::move(__gti_mir_p_") == std::string::npos &&
             body.find(".get().") != std::string::npos,
         "the fallback body should transfer its parameter into the MIR slot "
         "but invoke the selected mutable overload on an lvalue receiver");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto productionMarker = [&](lang::HirFunctionInstanceId id) {
    return artifact.contents.find("// GTI verified-MIR body: ") !=
               std::string::npos &&
           artifact.contents.find("function-instance " + std::to_string(id) +
                                  "\n") != std::string::npos;
  };
  expect(productionMarker(fallback->id) && productionMarker(entry->id),
         "production emission should select both the fallback body and its "
         "entry caller from MIR");
}

// Free-function instances keep their own verified body and selected call
// identity. Native templates carry physical closure types, not GTI dispatch.
void expectDistinctCallableInstances(const lang::FrontendResult &frontend,
                                     const std::string &text) {
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.owner || function.parameterTypes.size() != 1 ||
        function.parameterTypes.front().kind != lang::SemanticType::Lambda)
      continue;
    const std::string symbol = "__gti_callable_inst_" +
                               std::to_string(function.id) +
                               "_apply__gti_mir_failure";
    const std::string signature =
        symbol + "(Operation __gti_mir_arg_0, ::gti_failure_record_v2 "
                 "*__gti_mir_failure_record)";
    const std::string definition =
        signature +
        " {\n    // GTI verified-MIR body: "
        "deduced-callable-failure-v1 function-instance " +
        std::to_string(function.id) + "\n";
    const auto at = text.find(definition);
    expect(at != std::string::npos &&
               text.find(definition, at + definition.size()) ==
                   std::string::npos,
           "each exact callable instance must have one definition carrying "
           "its own MIR body identity");
    expect(text.find(signature + ";") != std::string::npos &&
               text.find("::__gti_program::" + symbol + "(") !=
                   std::string::npos,
           "the declaration and actual caller must select the same exact "
           "instance symbol");
    ++checked;
  }
  expect(checked == 2, "both concrete callable instances must be checked");
}

void testDeducedCallableTemplateEmission() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-deduced-template.gti", R"(
void apply<Operation>(Operation operation) {
  operation(1);
}

int main() {
  auto first = [](int value) -> void {
    if (value == 1) {
      return;
    }
  };
  auto second = [](int value) -> void {
    if (value == 2) {
      return;
    }
  };
  apply(first);
  apply(second);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the deduced-template fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto count = [&](std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  expect(count("(Operation __gti_mir_arg_0, ::gti_failure_record_v2 "
               "*__gti_mir_failure_record) {") == 2,
         "each free-function instance must retain its own MIR body");
  expect(count("deduced-callable-failure-v1 function-instance") == 2,
         "each instance body must carry exactly one provenance banner");
  expectDistinctCallableInstances(frontend, text);
  expect(count("// GTI verified-MIR body: scalar-cfg-failure-v1 "
               "lambda-instance") == 2,
         "both fused literals should embed their verified lambda bodies at "
         "the call sites");
}

// Formal pack bindings remain grouped for source identity, while the call ABI
// and parameter types are flattened, including when a fixed prefix is present.
void testForwardedConcretePackLayout() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-forwarded-concrete-pack.gti", R"(
void consume<Args...>(Args... values) {}

void forward<Args...>(Args... values) {
  consume(values...);
  consume(0, values...);
}

T keep_first<T, Rest...>(T first, Rest... rest) {
  forward(rest...);
  return first;
}

int32_t main() {
  forward();
  forward(1, true, "gti");
  int32_t inferred = keep_first(7, false, "tail");
  int32_t explicit_types =
      keep_first<int32_t, std::string_view>(9, "tail");
  return inferred + explicit_types - 16;
}
)");
  expect(frontend.canGenerateCode(),
         "the forwarded concrete-pack fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *forwarded = nullptr;
  bool checkedPrefix = false;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const bool projectsPackElement = std::any_of(
        function.body.places.begin(), function.body.places.end(),
        [](const lang::MirPlace &candidate) {
          return std::any_of(candidate.projections.begin(),
                             candidate.projections.end(), [](const auto &item) {
                               return item.kind ==
                                      lang::MirProjectionKind::PackElement;
                             });
        });
    if (!projectsPackElement) {
      continue;
    }
    for (const lang::MirPlace &place : function.body.places) {
      if (place.root != lang::MirPlaceRootKind::Binding ||
          !place.projections.empty() ||
          place.type.kind != lang::SemanticType::TypePack ||
          !place.type.concretePack) {
        continue;
      }
      const auto binding =
          std::find(function.parameterBindings.begin(),
                    function.parameterBindings.end(), place.binding);
      if (binding == function.parameterBindings.end()) {
        continue;
      }
      const std::size_t index = static_cast<std::size_t>(
          std::distance(function.parameterBindings.begin(), binding));
      if (place.type.arguments.size() < 2)
        continue;
      expect(index + place.type.arguments.size() ==
                     function.parameterTypes.size() &&
                 std::equal(place.type.arguments.begin(),
                            place.type.arguments.end(),
                            function.parameterTypes.begin() + index,
                            function.parameterTypes.end()),
             "each grouped source pack must match the exact flattened ABI "
             "suffix after its fixed parameters");
      for (const auto &element : function.body.places) {
        if (element.binding != place.binding ||
            element.projections.size() != 1 ||
            element.projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        const auto ordinal = element.projections.front().constantIndex;
        expect(ordinal && *ordinal < place.type.arguments.size() &&
                   element.type == place.type.arguments[*ordinal],
               "pack projection identity must select its exact source element");
      }
      checkedPrefix = checkedPrefix || index != 0;
      forwarded = &function;
    }
  }
  expect(forwarded != nullptr,
         "forwarding must retain grouped source packs with projected elements");
  expect(checkedPrefix, "the fixed-prefix pack mapping must also be checked");
  if (forwarded == nullptr) {
    return;
  }

  const auto projected =
      std::find_if(forwarded->body.places.begin(), forwarded->body.places.end(),
                   [](const auto &place) {
                     return place.projections.size() == 1 &&
                            place.projections.front().kind ==
                                lang::MirProjectionKind::PackElement;
                   });
  expect(projected != forwarded->body.places.end(),
         "the selected body must contain a real pack projection");
  if (projected != forwarded->body.places.end()) {
    auto forged = frontend.mir;
    auto *body =
        lang::findMirBody(forged, {lang::MirBodyKind::Function, forwarded->id});
    auto &place =
        body->places[std::distance(forwarded->body.places.begin(), projected)];
    place.projections.front().constantIndex =
        forwarded->parameterTypes.size() + 1;
    expect(!lang::verifyMirProgram(forged).valid(),
           "an out-of-layout pack projection must fail independent MIR "
           "verification before C++ admission");
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = forwarded->id};
  // A forwarder with no failure source is proved failure-free and emits only
  // its plain body; the element layout is the same in either form.
  const bool failureForm = forwarded->mayRaiseDefinedFailure;
  const bool supported = failureForm ? emitter.supportsFailureBodyText(address)
                                     : emitter.supportsBodyText(address);
  expect(emitter.analyze(address).ready() && supported,
         "a forwarded concrete pack should enter the general body path from "
         "its exact element layout");
  if (!supported) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      failureForm
          ? emitter.emitFailureBodyText(address,
                                        "forwarded-concrete-pack-test-v0", 1)
          : emitter.emitBodyText(address, "forwarded-concrete-pack-test-v0", 1);
  expect(text.text.find("__gti_mir_arg_0") != std::string::npos &&
             text.text.find("__gti_mir_arg_1") != std::string::npos,
         "forwarded pack elements should spell as their flattened native "
         "arguments");
}

void testRetainedReferenceAddressAndConcreteDefaultReturn() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-retained-reference-address.gti", R"(
struct Sentinel {};

class BorrowingIterator<T> {
  T& value;

public:
  BorrowingIterator(T& source) : value(source) {}

  T& operator*() { return this.value; }
};

class SingleRange<T> {
  mut T value;

public:
  SingleRange(T initial) : value(initial) {}

  BorrowingIterator<T> begin() {
    return BorrowingIterator<T>(this.value);
  }

  Sentinel end() { return Sentinel(); }
};

int32_t main() {
  SingleRange<int32_t> range = SingleRange<int32_t>(7);
  mut BorrowingIterator<int32_t> iterator = range.begin();
  int32_t& value = *iterator;
  Sentinel sentinel = range.end();
  return value - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the retained-reference fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *defaultReturn = nullptr;
  lang::MirPlaceId referencePlace = 0;
  lang::MirPlaceId addressPlace = 0;
  lang::MirInstructionId referenceInitializer = 0;
  lang::MirLoanId retainedLoan = 0;
  lang::MirLoanId addressLoan = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
      for (const lang::MirBlock &block : function.body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Initialize ||
              !instruction.destination || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != lang::MirOperandKind::Loan) {
            continue;
          }
          const lang::MirPlace *destination =
              function.body.findPlace(*instruction.destination);
          const lang::MirPlace *address =
              function.body.findPlace(instruction.operands.front().place);
          if (destination == nullptr ||
              destination->type.kind != lang::SemanticType::Reference ||
              address == nullptr ||
              address->root != lang::MirPlaceRootKind::Loan) {
            continue;
          }
          const auto *child = function.body.findLoan(address->loan);
          const auto *parent = child == nullptr
                                   ? nullptr
                                   : function.body.findLoan(child->parent);
          if (child != nullptr &&
              child->kind == lang::MirLoanKind::CallResult &&
              child->id == instruction.operands.front().loan &&
              parent != nullptr && parent->kind == lang::MirLoanKind::Stored &&
              std::find(child->carriers.begin(), child->carriers.end(),
                        destination->binding) != child->carriers.end()) {
            referencePlace = destination->id;
            addressPlace = address->id;
            referenceInitializer = instruction.id;
            retainedLoan = parent->id;
            addressLoan = address->loan;
          }
        }
      }
    }

    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.result && !instruction.functionTarget &&
            !instruction.constructorTarget && !instruction.receiver &&
            instruction.intrinsic == lang::IntrinsicKind::None &&
            instruction.operands.empty() &&
            instruction.info.type.kind == lang::SemanticType::Class) {
          defaultReturn = &function;
        }
      }
    }
  }
  expect(entry != nullptr && defaultReturn != nullptr && referencePlace != 0 &&
             retainedLoan != 0 && addressLoan != 0 &&
             retainedLoan != addressLoan,
         "the reference must retain its exact address loan and distinct "
         "stored-owner parent");
  if (entry == nullptr || defaultReturn == nullptr || referencePlace == 0 ||
      retainedLoan == 0 || addressLoan == 0 || retainedLoan == addressLoan) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);

  // A default construction has no failure source, so a body returning one is
  // proved failure-free: its plain form returns the value-initialized class,
  // and a failure form constructs it in the uninitialized result storage.
  const lang::MirBodyAddress defaultAddress{.kind = lang::MirBodyKind::Function,
                                            .owner = defaultReturn->id};
  const bool defaultFailure = defaultReturn->mayRaiseDefinedFailure;
  const bool defaultSupported =
      defaultFailure ? emitter.supportsFailureBodyText(defaultAddress)
                     : emitter.supportsBodyText(defaultAddress);
  expect(emitter.analyze(defaultAddress).ready() && defaultSupported,
         "a concrete default construction should publish at its class-return "
         "boundary");
  if (defaultSupported) {
    const lang::CppMirBodyEmissionText text =
        defaultFailure
            ? emitter.emitFailureBodyText(defaultAddress,
                                          "concrete-default-return-test-v0", 1)
            : emitter.emitBodyText(defaultAddress,
                                   "concrete-default-return-test-v0", 1);
    expect(defaultFailure
               ? text.text.find(
                     "construct_representation_at(__gti_mir_out_result);") !=
                     std::string::npos
               : text.text.find("return ::__gti_program::Sentinel{};") !=
                     std::string::npos,
           "the concrete default return should publish exactly once, directly "
           "as its result");
  }

  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "the retained-reference caller should enter the transformed path");
  if (emitter.supportsFailureBodyText(entryAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        entryAddress, "retained-reference-address-test-v0", 1);
    const std::string destination =
        "__gti_mir_p_" + std::to_string(referencePlace) + " = ";
    expect(text.text.find(destination + "&(*__gti_mir_loan_" +
                          std::to_string(addressLoan) + ");") !=
                   std::string::npos &&
               text.text.find(destination + "&(*__gti_mir_loan_" +
                              std::to_string(retainedLoan) + ");") ==
                   std::string::npos,
           "a retained reference should copy the child call-result address, "
           "not its provenance-only parent loan");
  }
  for (int mutation = 0; mutation != 2; ++mutation) {
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, entryAddress);
    if (mutation == 0) {
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == referenceInitializer)
            instruction.operands.front().loan = retainedLoan;
    } else {
      body->places[addressPlace - 1].loan = retainedLoan;
    }
    const lang::CppMirBodyEmitter rejectedEmitter(malformed, map);
    const std::string changed =
        mutation == 0 ? "retained loan" : "runtime address";
    expect(!lang::verifyMirProgram(malformed).valid(),
           "MIR rejects stored-owner provenance substituted for reference " +
               changed);
    expect(
        !rejectedEmitter.analyze(entryAddress).ready() &&
            !rejectedEmitter.supportsFailureBodyText(entryAddress),
        "backend rejects stored-owner provenance substituted for reference " +
            changed);
  }
}

void testStoredBorrowReferenceAliasAddresses() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-stored-borrow-reference-aliases.gti", R"(
#include <std/string>

int32_t main() {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  std::detail::storage_iterator<char>& alias = iterator;
  std::detail::storage_iterator<char>& second_alias = alias;
  char first = *iterator;
  char second = *alias;
  char third = *second_alias;
  value.push_back('!');
  return first == second and second == third ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the stored-borrow reference-alias fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  std::vector<lang::MirPlaceId> aliases;
  lang::MirLoanId provenance = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    entry = &function;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !instruction.destination || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Loan ||
            instruction.operands.front().place == 0) {
          continue;
        }
        const lang::MirPlace *destination =
            function.body.findPlace(*instruction.destination);
        const lang::MirLoan *loan =
            function.body.findLoan(instruction.operands.front().loan);
        const lang::MirLoan *dependency =
            loan == nullptr ? nullptr
                            : function.body.findLoan(loan->storedDependency);
        if (destination != nullptr && loan != nullptr &&
            destination->type.kind == lang::SemanticType::Reference &&
            loan->kind == lang::MirLoanKind::Local && dependency != nullptr &&
            dependency->kind == lang::MirLoanKind::Stored &&
            dependency->storedField == 0) {
          const auto *runtimeSource =
              function.body.findPlace(instruction.operands.front().place);
          const bool previousAlias =
              runtimeSource != nullptr &&
              runtimeSource->root == lang::MirPlaceRootKind::Binding &&
              runtimeSource->projections.size() == 1 &&
              runtimeSource->projections.front().kind ==
                  lang::MirProjectionKind::Dereference &&
              std::any_of(aliases.begin(), aliases.end(), [&](auto alias) {
                return function.body.findPlace(alias)->binding ==
                       runtimeSource->binding;
              });
          expect((loan->source == instruction.operands.front().place ||
                  previousAlias) &&
                     std::find(loan->carriers.begin(), loan->carriers.end(),
                               destination->binding) != loan->carriers.end() &&
                     loan->source != dependency->source &&
                     (provenance == 0 || provenance == dependency->id),
                 "each alias retains the physical loan and binds from its "
                 "source shell or an earlier checked reference alias");
          aliases.push_back(destination->id);
          provenance = dependency->id;
        }
      }
    }
  }
  expect(entry != nullptr && aliases.size() == 2 && provenance != 0,
         "both aliases should retain physical-borrow authority separate from "
         "the stored provenance dependency");
  if (entry == nullptr || aliases.size() != 2 || provenance == 0) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyzeProgram().ready() &&
             emitter.supportsFailureBodyText(address),
         "stored-borrow aliases should remain inside the complete C++ MIR "
         "text vocabulary");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter
          .emitFailureBodyText(address, "stored-borrow-reference-alias-test-v0",
                               1)
          .text;
  bool exactAddresses = true;
  for (const lang::MirPlaceId alias : aliases) {
    exactAddresses =
        exactAddresses && text.find("__gti_mir_p_" + std::to_string(alias) +
                                    " = &") != std::string::npos;
  }
  expect(exactAddresses &&
             text.find(" = __gti_mir_loan_" + std::to_string(provenance) +
                       ";") == std::string::npos,
         "reference aliases should bind from MIR's physical address places, "
         "not from the stored provenance loan");
}

void testExpectedValueOrBorrowedReceiver() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-value-or.gti", R"(
expected<void, std::string_view> complete() {
  return;
}

int32_t main() {
  expected<int32_t, std::string_view> result = int32_t(1);
  expected<void, std::string_view> completion = complete();
  completion.value();
  return result.value_or(int32_t(0)) - int32_t(1);
}
)");
  expect(frontend.canGenerateCode(),
         "the expected value_or fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirInstruction *valueOr = nullptr;
  const lang::MirInstruction *voidExtraction = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    entry = &function;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.intrinsic == lang::IntrinsicKind::ExpectedValueOr) {
          valueOr = &instruction;
        }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.intrinsic == lang::IntrinsicKind::ExpectedValue &&
            instruction.info.type == lang::SemanticType::Void) {
          voidExtraction = &instruction;
        }
      }
    }
  }
  expect(entry != nullptr && valueOr != nullptr && valueOr->receiver &&
             voidExtraction != nullptr && voidExtraction->receiver &&
             !voidExtraction->result &&
             valueOr->receiver->kind == lang::MirOperandKind::BorrowRead &&
             valueOr->receiver->place != 0,
         "Expected observers should retain their exact read-borrowed receiver "
         "and void value() should remain resultless");
  if (entry == nullptr || valueOr == nullptr || !valueOr->receiver ||
      voidExtraction == nullptr || !voidExtraction->receiver) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the borrowed value_or receiver should support transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "expected-value-or-test-v0", 1);
  const std::string receiver =
      "__gti_mir_p_" + std::to_string(valueOr->receiver->place) + ".value_or(";
  const std::string voidReceiver =
      "::gti_internal::backend::mir_expected_void_value_v1(__gti_mir_p_" +
      std::to_string(voidExtraction->receiver->place) + ");";
  expect(text.text.find(receiver) != std::string::npos &&
             text.text.find(voidReceiver) != std::string::npos,
         "Expected observers should read their live places without an SSA "
         "copy or a fabricated void result");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("function-instance " +
                                std::to_string(entry->id) + "\n") !=
             std::string::npos,
         "production emission should retain the borrowed value_or body on "
         "the verified MIR route");
}

void testContextualSignedMinimumInFailureCaller() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-contextual-signed-minimum.gti", R"(
int32_t checked_modulo(int32_t value, int32_t divisor) {
  return value % divisor;
}

bool matches_parenthesized_minimum(int64_t value) {
  return value == -(9223372036854775808);
}

int32_t main() {
  int32_t minimum = -2147483648;
  int32_t remainder = checked_modulo(4, 2);
  if (minimum == -2147483648) {
    return remainder;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the contextual signed-minimum fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *parenthesized = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    const lang::HirFunctionInstance *hir =
        frontend.hir.findFunctionInstance(function.id);
    if (hir != nullptr && hir->source != nullptr &&
        hir->source->name().lexeme == "matches_parenthesized_minimum") {
      parenthesized = &function;
    }
  }
  expect(entry != nullptr && parenthesized != nullptr,
         "the contextual signed-minimum fixture should lower its entry and "
         "parenthesized comparison");
  if (entry == nullptr || parenthesized == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a failure caller should retain a contextually narrowed signed "
         "minimum in transformed MIR emission");
  if (emitter.supportsFailureBodyText(address)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        address, "contextual-signed-minimum-test-v0", 1);
    expect(text.text.find("static_cast<std::int32_t>(-2147483647 - 1)") !=
               std::string::npos,
           "the wider literal carrier should fuse into the safe int32 minimum "
           "spelling at negation");
  }

  // The fused comparison has no failure source, so MIR proves this body
  // failure-free and it emits only its plain form; both forms fuse alike.
  const lang::MirBodyAddress parenthesizedAddress{
      .kind = lang::MirBodyKind::Function, .owner = parenthesized->id};
  const bool parenthesizedFailure = parenthesized->mayRaiseDefinedFailure;
  const bool parenthesizedSupported =
      parenthesizedFailure
          ? emitter.supportsFailureBodyText(parenthesizedAddress)
          : emitter.supportsBodyText(parenthesizedAddress);
  expect(emitter.analyze(parenthesizedAddress).ready() &&
             parenthesizedSupported,
         "an exact identity inserted by parentheses should remain inside the "
         "signed-minimum MIR fusion proof");
  if (parenthesizedSupported) {
    const lang::CppMirBodyEmissionText text =
        parenthesizedFailure
            ? emitter.emitFailureBodyText(
                  parenthesizedAddress, "parenthesized-signed-minimum-test-v0",
                  1)
            : emitter.emitBodyText(parenthesizedAddress,
                                   "parenthesized-signed-minimum-test-v0", 1);
    expect(
        text.text.find("static_cast<std::int64_t>(-9223372036854775807 - 1)") !=
            std::string::npos,
        "the parenthesized uint64 carrier should fuse into the safe int64 "
        "minimum spelling");
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id)) !=
                 std::string::npos &&
             artifact.contents.find("checked_modulo__gti_mir_failure(") !=
                 std::string::npos,
         "production emission should keep the caller and checked callee on "
         "the transformed failure ABI");
}

void testPlainCopyReturnUsesExactSourcePlace() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-plain-copy-return.gti", R"(
[[c_abi]] struct Pair {
  mut float x;
  mut float y;
};

Pair forward(Pair value) {
  return value;
}

int main() {
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive-record copy-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto hir =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == "forward";
                   });
  const lang::MirFunctionInstance *forward =
      hir == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(hir->id);
  expect(forward != nullptr && !forward->mayRaiseDefinedFailure,
         "the passive-record copy return should be a plain MIR body");
  if (forward == nullptr) {
    return;
  }

  const lang::MirInstruction *copy = nullptr;
  lang::MirValueId copiedValue = 0;
  for (const lang::MirBlock &block : forward->body.blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Return ||
        !block.terminator.value ||
        block.terminator.value->kind != lang::MirOperandKind::Value) {
      continue;
    }
    copiedValue = block.terminator.value->value;
    const lang::MirValue *value = forward->body.findValue(copiedValue);
    if (value == nullptr) {
      continue;
    }
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.id == value->definition &&
          instruction.kind == lang::MirInstructionKind::Load) {
        copy = &instruction;
        break;
      }
    }
  }
  expect(copy != nullptr && copiedValue != 0 && copy->operands.size() == 1 &&
             copy->operands.front().kind == lang::MirOperandKind::Copy,
         "the passive-record return should retain one exact copy Load");
  if (copy == nullptr || copiedValue == 0) {
    return;
  }
  const lang::MirPlaceId sourcePlace = copy->operands.front().place;
  const std::string sourceSpelling =
      "__gti_mir_p_" + std::to_string(sourcePlace) + ".get()";

  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = forward->id};
  const lang::CppMirBodyEmissionMapRows rows =
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
  const auto recordRow =
      std::find_if(rows.types.begin(), rows.types.end(), [&](const auto &row) {
        return row.type == forward->returnType;
      });
  expect(recordRow != rows.types.end() && recordRow->copyConstructible,
         "the passive-record representation should prove copy construction");
  if (recordRow == rows.types.end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(rows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyze(address).ready() && emitter.supportsBodyText(address),
         "an exact copy Load -> Return should enter plain body emission");
  if (emitter.supportsBodyText(address)) {
    const std::string text =
        emitter.emitBodyText(address, "plain-copy-return-test-v0", 1).text;
    const std::string local =
        "__gti_mir_v_" + std::to_string(copiedValue) + "{}";
    expect(text.find("return " + sourceSpelling + ";") != std::string::npos &&
               text.find(local) == std::string::npos &&
               text.find("construct_representation_at(__gti_mir_out_result") ==
                   std::string::npos,
           "plain copy return should copy directly from the exact source "
           "place without inventing an SSA object or failure ABI");
  }

  lang::MirProgram conservative = frontend.mir;
  auto &conservativeFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          conservative.functionInstances());
  const auto conservativeForward =
      std::find_if(conservativeFunctions.begin(), conservativeFunctions.end(),
                   [&](const lang::MirFunctionInstance &function) {
                     return function.id == forward->id;
                   });
  if (conservativeForward != conservativeFunctions.end()) {
    conservativeForward->mayRaiseDefinedFailure = true;
  }
  expect(lang::verifyMirProgram(conservative).valid(),
         "a conservative true failure summary should remain valid MIR");
  const lang::CppMirBodyEmitter failureEmitter(conservative, map);
  expect(failureEmitter.supportsFailureBodyText(address),
         "the same exact copy return should remain admitted in failure form");
  if (failureEmitter.supportsFailureBodyText(address)) {
    const std::string text =
        failureEmitter
            .emitFailureBodyText(address, "failure-copy-return-test-v0", 1)
            .text;
    expect(text.find("construct_representation_at(__gti_mir_out_result, " +
                     sourceSpelling + ");") != std::string::npos,
           "failure-form copy return should retain placement construction "
           "into the exact out result");
  }

  lang::CppMirBodyEmissionMapRows nonCopyRows = rows;
  const auto nonCopy = std::find_if(
      nonCopyRows.types.begin(), nonCopyRows.types.end(),
      [&](const auto &row) { return row.type == forward->returnType; });
  if (nonCopy != nonCopyRows.types.end()) {
    nonCopy->copyConstructible = false;
    nonCopy->copyable = false;
  }
  expect(!lang::CppMirBodyEmitter(
              frontend.mir, lang::CppMirBodyEmissionMap(std::move(nonCopyRows)))
              .supportsBodyText(address),
         "copy return must fail closed without a copy-construction row");

  lang::MirProgram multipleUse = frontend.mir;
  auto &multipleUseFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          multipleUse.functionInstances());
  const auto multipleUseForward =
      std::find_if(multipleUseFunctions.begin(), multipleUseFunctions.end(),
                   [&](const lang::MirFunctionInstance &function) {
                     return function.id == forward->id;
                   });
  if (multipleUseForward != multipleUseFunctions.end() &&
      copiedValue <= multipleUseForward->body.valueUses.size() &&
      !multipleUseForward->body.valueUses[copiedValue - 1].empty()) {
    multipleUseForward->body.valueUses[copiedValue - 1].push_back(
        multipleUseForward->body.valueUses[copiedValue - 1].front());
  }
  expect(
      !lang::verifyMirProgram(multipleUse).valid() &&
          !lang::CppMirBodyEmitter(multipleUse, map).supportsBodyText(address),
      "duplicated copy-return use metadata must fail closed");

  lang::MirProgram malformed = frontend.mir;
  auto &malformedFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          malformed.functionInstances());
  const auto malformedForward =
      std::find_if(malformedFunctions.begin(), malformedFunctions.end(),
                   [&](const lang::MirFunctionInstance &function) {
                     return function.id == forward->id;
                   });
  if (malformedForward != malformedFunctions.end()) {
    for (lang::MirBlock &block : malformedForward->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == copy->id && !instruction.operands.empty()) {
          instruction.operands.front().type = lang::SemanticType::Float;
        }
      }
    }
  }
  // MIR verification owns Load operand/source type agreement; the backend
  // boundary still fails closed independently.
  expect(!lang::verifyMirProgram(malformed).valid() &&
             !lang::CppMirBodyEmitter(malformed, map).supportsBodyText(address),
         "a type-incoherent copy-return operand must fail MIR verification "
         "and the backend boundary");
}

void testReferenceParameterBorrowUsesReferentCarrier() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-reference-parameter-range.gti", R"(
#include <std/vector>

int32_t sum(std::vector<int32_t>& values) {
  mut int32_t total = 0;
  for (int32_t value : values) {
    total += value;
  }
  return total;
}

int32_t main() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(3);
  return sum(values) - 3;
}
)");
  expect(frontend.canGenerateCode(),
         "a range over a reference parameter should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  const lang::MirLoan *borrow = nullptr;
  const lang::MirPlace *source = nullptr;
  const lang::MirPlace *carrier = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.body.rangeIterations.empty() ||
        function.parameterBindings.empty()) {
      continue;
    }
    const lang::MirRangeIteration &range =
        function.body.rangeIterations.front();
    const lang::MirLoan *candidateBorrow =
        function.body.findLoan(range.rangeLoan);
    const lang::MirPlace *candidateSource =
        candidateBorrow == nullptr
            ? nullptr
            : function.body.findPlace(candidateBorrow->source);
    const auto candidateCarrier = std::find_if(
        function.body.places.begin(), function.body.places.end(),
        [&](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.binding == function.parameterBindings.front() &&
                 place.projections.empty() &&
                 place.type.kind == lang::SemanticType::Reference;
        });
    if (candidateSource != nullptr &&
        candidateSource->root == lang::MirPlaceRootKind::Binding &&
        candidateSource->binding == function.parameterBindings.front() &&
        candidateSource->projections.size() == 1 &&
        candidateSource->projections.front().kind ==
            lang::MirProjectionKind::Dereference &&
        candidateCarrier != function.body.places.end()) {
      selected = &function;
      borrow = candidateBorrow;
      source = candidateSource;
      carrier = &*candidateCarrier;
      break;
    }
  }
  expect(selected != nullptr && borrow != nullptr && source != nullptr &&
             carrier != nullptr,
         "the range fixture should retain a borrow sourced by the explicit "
         "referent of its reference parameter carrier");
  if (selected == nullptr || borrow == nullptr || source == nullptr ||
      carrier == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.supportsFailureBodyText(address),
         "the reference-parameter range should enter transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "reference-parameter-range-v0", 1)
          .text;
  const std::string assignment =
      "__gti_mir_loan_" + std::to_string(borrow->id) + " = &(*__gti_mir_p_" +
      std::to_string(carrier->id) + ");";
  expect(text.find(assignment) != std::string::npos &&
             text.find("__gti_mir_loan_" + std::to_string(borrow->id) +
                       " = &__gti_mir_p_" + std::to_string(carrier->id)) ==
                 std::string::npos,
         "borrowing a reference parameter should address its explicit "
         "referent rather than take the pointer carrier's address");
}

void testAdjacentClassCopyAssignmentFusion() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-class-copy-assignment.gti", R"(
struct CopyValue {
  int32_t value = 0;
  CopyValue(int32_t initial) : value(initial) {}
  int32_t read() { return this.value; }
};

int main() {
  mut CopyValue target = CopyValue();
  CopyValue source = CopyValue(7);
  target = source;
  return target.read() - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the adjacent class-copy fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  const lang::MirInstruction *load = nullptr;
  const lang::MirInstruction *assignment = nullptr;
  const lang::MirPlace *source = nullptr;
  const lang::MirPlace *destination = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index) {
        const lang::MirInstruction &candidate = block.instructions[index];
        const lang::MirInstruction &consumer = block.instructions[index + 1];
        if (candidate.kind != lang::MirInstructionKind::Load ||
            !candidate.result || candidate.operands.size() != 1 ||
            candidate.operands.front().kind != lang::MirOperandKind::Copy ||
            candidate.info.type.kind != lang::SemanticType::Class ||
            consumer.kind != lang::MirInstructionKind::Assign ||
            !consumer.destination || consumer.operands.size() != 1 ||
            consumer.operands.front().kind != lang::MirOperandKind::Value ||
            consumer.operands.front().value != *candidate.result) {
          continue;
        }
        selected = &function;
        load = &candidate;
        assignment = &consumer;
        source = function.body.findPlace(candidate.operands.front().place);
        destination = function.body.findPlace(*consumer.destination);
      }
    }
  }
  expect(selected != nullptr && load != nullptr && assignment != nullptr &&
             source != nullptr && destination != nullptr,
         "the fixture should lower one adjacent class Load/Assign pair");
  if (selected == nullptr || load == nullptr || assignment == nullptr ||
      source == nullptr || destination == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the adjacent class copy should support transformed MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-copy-assignment-test-v0", 1)
          .text;
  const std::string copyValue = "__gti_mir_v_" + std::to_string(*load->result);
  const std::string assignmentValue =
      "__gti_mir_v_" + std::to_string(*assignment->result);
  const std::string directCopy =
      "__gti_mir_p_" + std::to_string(destination->id) +
      ".get() = __gti_mir_p_" + std::to_string(source->id) + ".get();";
  expect(text.find("class copy load " + std::to_string(*load->result) +
                   " fuses into assignment " +
                   std::to_string(assignment->id)) != std::string::npos &&
             text.find(directCopy) != std::string::npos &&
             text.find(copyValue) == std::string::npos &&
             text.find(assignmentValue) == std::string::npos,
         "the class copy should spell directly between MIR-owned slots without "
         "inventing either discarded SSA lifetime");
}

void testNestedReferenceReturnForwardsCallResultPointer() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-nested-reference-return.gti", R"(
class inner {
  mut int elements[2] = {2, 3};

public:
  mut int& operator[](uint64_t index) mut { return this.elements[index]; }
};

class outer {
  mut inner data = inner();

public:
  mut int& operator[](uint64_t index) mut { return this.data[index]; }
};

int main() {
  mut outer o = outer();
  o[uint64_t(1)] = 11;
  return o[uint64_t(1)] - 11;
}
)");
  expect(frontend.canGenerateCode(),
         "the nested reference-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  lang::MirLoanId published = 0;
  lang::MirLoanId forwarded = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Borrow ||
            !instruction.loan || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Loan) {
          continue;
        }
        const lang::MirLoan *produced =
            function.body.findLoan(*instruction.loan);
        const lang::MirLoan *retained =
            function.body.findLoan(instruction.operands.front().loan);
        if (produced != nullptr && retained != nullptr &&
            produced->kind == lang::MirLoanKind::Return &&
            retained->kind == lang::MirLoanKind::CallResult) {
          selected = &function;
          published = produced->id;
          forwarded = retained->id;
        }
      }
    }
  }
  expect(selected != nullptr && published != 0 && forwarded != 0,
         "the outer subscript should forward its call-result loan into the "
         "published return loan");
  if (selected == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the forwarding subscript should support transformed MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "nested-reference-return-test-v0", 1)
          .text;
  const std::string publishedLocal =
      "__gti_mir_loan_" + std::to_string(published);
  const std::string forwardedLocal =
      "__gti_mir_loan_" + std::to_string(forwarded);
  expect(text.find("std::int32_t *" + publishedLocal + "{}") !=
             std::string::npos,
         "the forwarded return loan should declare the element-typed pointer "
         "local, not the provenance class type");
  expect(text.find(publishedLocal + " = " + forwardedLocal + ";") !=
             std::string::npos,
         "the publishing borrow should copy the callee-published element "
         "pointer");
  expect(text.find(publishedLocal + " = &") == std::string::npos,
         "the publishing borrow must not re-borrow the provenance place");
  expect(text.find("*__gti_mir_out_result = " + publishedLocal + ";") !=
             std::string::npos,
         "the forwarded pointer should publish through the out-parameter");
}

void testOpaquePointerFailureResults() {
  const std::filesystem::path fixture =
      std::filesystem::path(__FILE__).parent_path() /
      "fixtures/native_opaque_pointer_results.gti";
  std::ifstream stream(fixture);
  const std::string source{std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "opaque pointer results should pass semantic and MIR verification");
  if (!frontend.canGenerateCode())
    return;
  const auto rows = buildRows(frontend, frontend.mir);
  const lang::CppMirBodyEmissionMap map(rows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t returning = 0;
  bool wrongResultTested = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.definitionKind != lang::MirDefinitionKind::Source ||
        !function.mayRaiseDefinedFailure ||
        function.returnType.kind != lang::SemanticType::RawPointer)
      continue;
    ++returning;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    expect(emitter.supportsFailureBodyText(address),
           "opaque pointer returns use the typed failure result channel");
    auto missingRows = rows;
    std::erase_if(missingRows.types, [&](const auto &row) {
      return row.type == function.returnType;
    });
    const lang::CppMirBodyEmissionMap missingMap(std::move(missingRows));
    expect(!lang::CppMirBodyEmitter(frontend.mir, missingMap)
                .supportsFailureBodyText(address),
           "opaque pointer results require their exact representation row");
    for (const auto &place : function.body.places) {
      if (place.type.kind != lang::SemanticType::RawPointer ||
          place.type == function.returnType)
        continue;
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(malformed, address);
      for (auto &block : body->blocks) {
        if (block.terminator.kind == lang::MirTerminatorKind::Return &&
            block.terminator.value) {
          block.terminator.value->type = place.type;
          wrongResultTested = true;
        }
      }
      expect(!lang::verifyMirProgram(malformed).valid(),
             "a returned pointer cannot substitute a distinct opaque identity");
      break;
    }
  }
  expect(returning >= 2 && wrongResultTested,
         "exercise member and forwarding results with distinct opaque types");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "the complete production backend emits opaque pointer results");
}

void testNativeCallbackFactoryBody() {
  lang::FrontendResult frontend =
      analyzeWithStandardLibrary("native_callback_factory.gti", R"(
[[c_opaque]] struct NativeHandle;

using callback = (int32_t) -> int32_t;
using notification = (NativeHandle*) -> void;
using callback_factory = () -> callback;

[[c_abi]] struct NativeCallbacks {
  callback unary;
  notification notify;
  void* user;
};

extern "C" {
  callback set_callback(callback value);
  callback_factory set_factory(callback_factory value);
  void install_callbacks(const NativeCallbacks* callbacks);
}

int32_t add_one(int32_t value) {
  return value + 1;
}

callback make_callback() {
  return add_one;
}
)");
  expect(frontend.canGenerateCode(),
         "the native callback factory should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O0,
                                        .compatibility = &compatibility});
  expect(optimized.valid(),
         "the callback factory should preserve a valid optimized MIR program");
  if (!optimized.valid()) {
    return;
  }
  const lang::MirProgram &mir = optimized.mir;

  const auto selected = std::find_if(
      mir.functionInstances().begin(), mir.functionInstances().end(),
      [](const lang::MirFunctionInstance &function) {
        return function.definitionKind == lang::MirDefinitionKind::Source &&
               function.returnType.kind == lang::SemanticType::NativeFunction;
      });
  expect(selected != mir.functionInstances().end(),
         "the fixture should lower a callback-valued source function");
  if (selected == mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(mir, map);
  const lang::CppMirProgramEmissionAnalysis programAnalysis =
      emitter.analyzeProgram();
  for (const lang::CppMirBodyEmissionIssue &issue : programAnalysis.issues) {
    std::cerr << "native callback program issue: " << issue.detail << '\n';
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(address);
  for (const lang::CppMirBodyEmissionIssue &issue : analysis.issues) {
    std::cerr << "native callback body issue: " << issue.detail << '\n';
  }
  expect(!selected->mayRaiseDefinedFailure,
         "forming and returning a native callback should not raise a defined "
         "failure");
  expect(analysis.ready(),
         "the callback factory should have complete MIR representation rows");
  expect(programAnalysis.issues.empty(),
         "nested callback aliases should preserve a coherent representation "
         "map");
  expect(emitter.supportsBodyText(address),
         "the callback factory should support plain verified-MIR emission");
  if (!analysis.ready() || !emitter.supportsBodyText(address)) {
    return;
  }
  const std::string text =
      emitter.emitBodyText(address, "native-callback-factory-test-v0", 1).text;
  expect(text.find("__gti_native_callback_") != std::string::npos,
         "the callback factory should return its exact adapter thunk");
}

void testInvokePublishesScalarEdgeArguments() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-invoke-arguments.gti", R"(
int main() {
  int offset = 3;
  auto add_offset = [offset](int value) -> int {
    return offset + value;
  };
  auto copied = add_offset;
  int doubled = [](int value) -> int {
    return value * 2;
  }(4);
  return copied(doubled) - 11;
}
)");
  expect(frontend.canGenerateCode(),
         "the invoke-edge fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMapRows rows =
      buildRows(frontend, frontend.mir);
  const lang::CppMirBodyEmissionMap canonicalMap(rows);
  const lang::CppMirBodyEmitter canonicalEmitter(frontend.mir, canonicalMap);
  lang::MirProgram program = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      program.functionInstances());
  lang::MirBody *selectedBody = nullptr;
  lang::MirBodyAddress address;
  lang::MirBlock *invoke = nullptr;
  for (lang::MirFunctionInstance &function : functions) {
    if (function.definitionKind != lang::MirDefinitionKind::Source) {
      continue;
    }
    const lang::MirBodyAddress candidate{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
    if (!canonicalEmitter.analyze(candidate).ready() ||
        (!canonicalEmitter.supportsFailureBodyText(candidate) &&
         !canonicalEmitter.supportsBodyText(candidate))) {
      continue;
    }
    for (lang::MirBlock &block : function.body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Invoke &&
          block.terminator.target != 0) {
        selectedBody = &function.body;
        address = candidate;
        invoke = &block;
        break;
      }
    }
    if (invoke != nullptr) {
      break;
    }
  }
  if (invoke == nullptr) {
    auto &lambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
        program.lambdaInstances());
    for (lang::MirLambdaInstance &lambda : lambdas) {
      const lang::MirBodyAddress candidate{.kind = lang::MirBodyKind::Lambda,
                                           .owner = lambda.id};
      if (!canonicalEmitter.analyze(candidate).ready() ||
          (!canonicalEmitter.supportsFailureBodyText(candidate) &&
           !canonicalEmitter.supportsBodyText(candidate))) {
        continue;
      }
      for (lang::MirBlock &block : lambda.body.blocks) {
        if (block.terminator.kind == lang::MirTerminatorKind::Invoke &&
            block.terminator.target != 0) {
          selectedBody = &lambda.body;
          address = candidate;
          invoke = &block;
          break;
        }
      }
      if (invoke != nullptr) {
        break;
      }
    }
  }
  expect(selectedBody != nullptr && invoke != nullptr,
         "the checked callable should lower one admitted source invoke edge");
  if (selectedBody == nullptr || invoke == nullptr) {
    return;
  }

  lang::MirBody &body = *selectedBody;
  lang::MirBlock *destination = body.findBlock(invoke->terminator.target);
  expect(destination != nullptr && destination->failureParameter == 0 &&
             destination->parameters.empty(),
         "the invoke success block should accept a scalar test parameter");
  if (destination == nullptr || destination->failureParameter != 0 ||
      !destination->parameters.empty()) {
    return;
  }

  lang::HirValueId sourceValue = 1;
  for (const lang::MirValue &value : body.values) {
    sourceValue = std::max(sourceValue, value.sourceValue + 1);
  }
  const lang::MirValueId parameterValue = body.values.size() + 1;
  destination->parameters.push_back({.value = parameterValue,
                                     .type = lang::SemanticType::Int32,
                                     .sourceValue = sourceValue});
  body.values.push_back({.id = parameterValue,
                         .sourceValue = sourceValue,
                         .info = {.type = lang::SemanticType::Int32},
                         .definitionBlock = destination->id});

  const lang::MirOperand argument{.kind = lang::MirOperandKind::Constant,
                                  .literal = lang::Literal{std::uint64_t{7}},
                                  .type = lang::SemanticType::Int32};
  for (lang::MirBlock &predecessor : body.blocks) {
    lang::MirTerminator &terminator = predecessor.terminator;
    if ((terminator.kind == lang::MirTerminatorKind::Goto ||
         terminator.kind == lang::MirTerminatorKind::Branch ||
         terminator.kind == lang::MirTerminatorKind::Switch ||
         terminator.kind == lang::MirTerminatorKind::Invoke) &&
        terminator.target == destination->id) {
      terminator.targetArguments.push_back(argument);
    }
    if ((terminator.kind == lang::MirTerminatorKind::Branch ||
         terminator.kind == lang::MirTerminatorKind::Invoke) &&
        terminator.elseTarget == destination->id) {
      terminator.elseArguments.push_back(argument);
    }
    if (terminator.kind == lang::MirTerminatorKind::Switch) {
      for (lang::MirSwitchTarget &target : terminator.switchTargets) {
        if (target.target == destination->id) {
          target.arguments.push_back(argument);
        }
      }
    }
  }
  (void)lang::rebuildMirValueUses(body);
  const lang::MirVerificationResult verification =
      lang::verifyMirProgram(program);
  for (const lang::MirVerificationError &error : verification.errors) {
    std::cerr << "invoke argument MIR issue: " << error.message << '\n';
  }
  expect(verification.valid(),
         "an invoke edge carrying a scalar block argument should remain valid "
         "MIR");
  if (!verification.valid()) {
    return;
  }

  const lang::CppMirBodyEmissionMap emissionMap(rows);
  const lang::CppMirBodyEmitter emitter(program, emissionMap);
  const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(address);
  for (const lang::CppMirBodyEmissionIssue &issue : analysis.issues) {
    std::cerr << "invoke argument emission issue: " << issue.detail << '\n';
  }
  const bool failureBody = emitter.supportsFailureBodyText(address);
  const bool plainBody = emitter.supportsBodyText(address);
  expect(analysis.ready() && (failureBody || plainBody),
         "the invoke edge with a scalar argument should remain inside the C++ "
         "failure-body vocabulary");
  if (!failureBody && !plainBody) {
    return;
  }

  const std::string text =
      failureBody
          ? emitter.emitFailureBodyText(address, "invoke-edge-arguments-v0", 1)
                .text
          : emitter.emitBodyText(address, "invoke-edge-arguments-v0", 1).text;
  const std::string edgeTemporary =
      "__gti_mir_edge_" + std::to_string(invoke->id) + "_0_0";
  const std::string parameter = "__gti_mir_v_" + std::to_string(parameterValue);
  expect(text.find(edgeTemporary + " = static_cast<") != std::string::npos &&
             text.find(parameter + " = " + edgeTemporary) != std::string::npos,
         "C++ emission should evaluate and publish invoke edge arguments "
         "before entering the success block");
}

// A cleanup-free Expected<Class, E> call result consumed by one by-value
// parameter has neither a MIR drop nor a value-rooted place. It shares the
// cleanup-free class result's representation-only slot. A non-placement
// Expected keeps its ordinary SSA local, and a cleanup-owning payload keeps
// MIR's value-rooted transfer slot. (Regression: the passive shape raised
// GTI-B0001 unless its payload was an indirectly transported C ABI record.)
void testCleanupFreeExpectedClassArgumentSlot() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-expected-class-argument.gti", R"(
struct pair_value {
  int32_t first = 0;
  pair_value(int32_t first) : first(first) {}
};

struct code_value {
  int32_t code = 0;
  code_value(int32_t code) : code(code) {}
};

class tracked {
public:
  int32_t value = 0;
  tracked(int32_t value) : value(value) {}
  ~tracked() {}
};

expected<pair_value, int32_t> make_pair(bool ok) {
  if (ok) { return pair_value(5); }
  return unexpected(4);
}

expected<int32_t, code_value> make_scalar(bool ok) {
  if (ok) { return 5; }
  return unexpected(code_value(4));
}

expected<tracked, int32_t> make_tracked(bool ok) {
  if (ok) { return tracked(5); }
  return unexpected(4);
}

int32_t read_pair(expected<pair_value, int32_t> result) {
  return result ? result.value().first : 0;
}

int32_t read_scalar(expected<int32_t, code_value> result) {
  return result ? result.value() : 0;
}

int32_t read_tracked(expected<tracked, int32_t> result) {
  return result ? result.value().value : 0;
}

int main() {
  return read_pair(make_pair(true)) + read_scalar(make_scalar(true)) +
         read_tracked(make_tracked(true)) - 15;
}
)");
  expect(frontend.canGenerateCode(),
         "the Expected class argument fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string passiveSlot =
      "::gti_internal::backend::mir_lifetime_slot<::gti_internal::backend::"
      "expected<::__gti_"
      "program::pair_value, std::int32_t>, true> __gti_mir_v_";
  const std::size_t declared = text.find(passiveSlot);
  expect(declared != std::string::npos,
         "a cleanup-free Expected<Class, E> argument should declare one "
         "representation-only slot");
  if (declared == std::string::npos) {
    return;
  }
  const std::size_t idStart = declared + passiveSlot.size();
  const std::string slot =
      "__gti_mir_v_" + text.substr(idStart, text.find(';', idStart) - idStart);
  // A checked producer publishes through the slot's construction address; a
  // failure-free one constructs the slot from its returned wrapper.
  expect((text.find(slot + ".construction_address()") != std::string::npos ||
          text.find(slot + ".construct_from(") != std::string::npos) &&
             text.find("read_pair__gti_mir_failure(std::move(" + slot +
                       ".get())") != std::string::npos &&
             text.find(slot + ".destroy();") != std::string::npos,
         "the producer should publish into the slot on success, the call "
         "should move its wrapper, and the slot should retire afterwards");
  expect(
      text.find(
          "mir_lifetime_slot<::gti_internal::backend::expected<std::int32_t, "
          "::__gti_program::code_value>") == std::string::npos &&
          text.find("::gti_internal::backend::expected<std::int32_t, "
                    "::__gti_program::code_"
                    "value> __gti_mir_v_") != std::string::npos,
      "a non-placement Expected argument should keep its ordinary SSA "
      "local");
  expect(text.find("mir_lifetime_slot<::gti_internal::backend::expected<::__"
                   "gti_program::tracked, "
                   "std::int32_t>, true> __gti_mir_v_") == std::string::npos &&
             text.find("read_tracked__gti_mir_failure(__gti_mir_p_") !=
                 std::string::npos,
         "a cleanup-owning Expected argument should keep MIR's value-rooted "
         "transfer slot");
}

// The same cleanup-free Expected<Class, E> result may initialize one by-value
// parameter of an exact ordinary constructor. Both the failure-form
// construction and a constructor body's plain form move the staged wrapper,
// and the moved-from representation is retired right after the construction.
// A cleanup-owning payload keeps MIR's value-rooted transfer slot.
// (Regression: a constructor consumer raised GTI-B0001.)
void testCleanupFreeExpectedClassConstructorArgumentSlot() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-expected-class-constructor-argument.gti", R"(
struct pair_value {
  int32_t first = 0;
  pair_value(int32_t first) : first(first) {}
};

class tracked {
public:
  int32_t value = 0;
  tracked(int32_t value) : value(value) {}
  ~tracked() {}
};

expected<pair_value, int32_t> make_pair(bool ok) {
  if (ok) { return pair_value(5); }
  return unexpected(4);
}

expected<tracked, int32_t> make_tracked(bool ok) {
  if (ok) { return tracked(5); }
  return unexpected(4);
}

class pair_holder {
public:
  int32_t first;
  pair_holder(expected<pair_value, int32_t> result)
      : first(result ? result.value().first : 0) {}
};

class tracked_holder {
public:
  int32_t value;
  tracked_holder(expected<tracked, int32_t> result)
      : value(result ? result.value().value : 0) {}
};

class outer {
public:
  mut int32_t first = 0;
  outer(bool ok) {
    pair_holder held{make_pair(ok)};
    this.first = held.first;
  }
};

int main() {
  pair_holder held{make_pair(true)};
  tracked_holder owned{make_tracked(true)};
  outer nested{true};
  return held.first + owned.value + nested.first - 15;
}
)");
  expect(frontend.canGenerateCode(),
         "the Expected class constructor argument fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string passiveSlot = "::gti_internal::backend::mir_lifetime_slot<:"
                                  ":gti_internal::backend::expected<::__gti_"
                                  "program::pair_value, std::int32_t>, true> ";
  // Counts constructions that spell `marker` on a line naming `construction`.
  // Each must move one declared slot and retire it on the next line.
  const auto retiredConstructions = [&](std::string_view construction,
                                        std::string_view marker) {
    std::size_t count = 0;
    for (std::size_t at = text.find(marker); at != std::string::npos;
         at = text.find(marker, at + marker.size())) {
      const std::size_t lineStart = text.rfind('\n', at) + 1;
      const std::size_t lineEnd = text.find('\n', at);
      const std::size_t idStart = at + marker.size();
      const std::size_t idEnd = text.find(".get())", idStart);
      if (lineEnd == std::string::npos || idEnd == std::string::npos ||
          idEnd > lineEnd)
        return std::size_t{0};
      if (std::string_view(text)
              .substr(lineStart, lineEnd - lineStart)
              .find(construction) == std::string_view::npos)
        continue;
      const std::string slot =
          "__gti_mir_v_" + text.substr(idStart, idEnd - idStart);
      const std::size_t next = text.find_first_not_of(' ', lineEnd + 1);
      if (text.find(passiveSlot + slot + ";") == std::string::npos ||
          text.compare(next, slot.size() + 11, slot + ".destroy();") != 0)
        return std::size_t{0};
      ++count;
    }
    return count;
  };
  expect(retiredConstructions("::__gti_program::pair_holder(",
                              ">{}, std::move(__gti_mir_v_") == 2,
         "the entry and the constructor body's failure form should each move "
         "a staged wrapper into the raising constructor and retire it "
         "afterwards");
  expect(retiredConstructions("__gti_mir_p_",
                              ".construct(std::move(__gti_mir_v_") == 1,
         "the constructor body's plain form should construct its binding from "
         "the staged wrapper and retire it afterwards");
  const std::size_t owned =
      text.find("::__gti_program::tracked_holder(::gti_internal::backend::"
                "mir_failure_constructor_tag_v1<");
  expect(text.find("mir_lifetime_slot<::gti_internal::backend::expected<::__"
                   "gti_program::tracked, "
                   "std::int32_t>, true> __gti_mir_v_") == std::string::npos &&
             owned != std::string::npos &&
             text.find(">{}, __gti_mir_p_", owned) == text.find(">{}, ", owned),
         "a cleanup-owning Expected constructor argument should keep MIR's "
         "value-rooted transfer slot");
}

// A named cleanup-free Expected<Class, E> binding copied into one by-value
// function or constructor parameter constructs MIR's Load copy in its own
// representation-only slot. The consumer moves the copy's wrapper, never the
// binding, and the moved-from representation is retired after the call or
// construction. A non-placement Expected keeps its ordinary SSA local.
// (Regression: the copied binding raised GTI-B0001 in the caller.)
void testCleanupFreeExpectedClassCopiedArgumentSlot() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-expected-class-copied-argument.gti", R"(
struct pair_value {
  int32_t first = 0;
  pair_value(int32_t first) : first(first) {}
};

struct code_value {
  int32_t code = 0;
  code_value(int32_t code) : code(code) {}
};

expected<pair_value, int32_t> make_pair(bool ok) {
  if (ok) { return pair_value(5); }
  return unexpected(4);
}

expected<int32_t, code_value> make_scalar(bool ok) {
  if (ok) { return 5; }
  return unexpected(code_value(4));
}

int32_t read_pair(expected<pair_value, int32_t> result) {
  return result ? result.value().first : 0;
}

int32_t read_scalar(expected<int32_t, code_value> result) {
  return result ? result.value() : 0;
}

class pair_holder {
public:
  int32_t first;
  pair_holder(expected<pair_value, int32_t> result)
      : first(result ? result.value().first : 0) {}
};

int main() {
  expected<pair_value, int32_t> local = make_pair(true);
  expected<int32_t, code_value> scalar = make_scalar(true);
  pair_holder held{local};
  return read_pair(local) + read_scalar(scalar) + held.first - 15;
}
)");
  expect(frontend.canGenerateCode(),
         "the Expected class copied-argument fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string passiveSlot = "::gti_internal::backend::mir_lifetime_slot<:"
                                  ":gti_internal::backend::expected<::__gti_"
                                  "program::pair_value, std::int32_t>, true> ";
  // Counts consumers that spell `marker` on a line naming `consumer`. Each
  // must move one declared slot that its copy constructed from a binding,
  // and retire that slot after the consumer.
  const auto copiedSlots = [&](std::string_view consumer,
                               std::string_view marker) {
    std::size_t count = 0;
    for (std::size_t at = text.find(marker); at != std::string::npos;
         at = text.find(marker, at + marker.size())) {
      const std::size_t lineStart = text.rfind('\n', at) + 1;
      const std::size_t lineEnd = text.find('\n', at);
      const std::size_t idStart = at + marker.size();
      const std::size_t idEnd = text.find(".get())", idStart);
      if (lineEnd == std::string::npos || idEnd == std::string::npos ||
          idEnd > lineEnd)
        return std::size_t{0};
      if (std::string_view(text)
              .substr(lineStart, lineEnd - lineStart)
              .find(consumer) == std::string_view::npos)
        continue;
      const std::string slot =
          "__gti_mir_v_" + text.substr(idStart, idEnd - idStart);
      const std::size_t copied = text.rfind(slot + ".construct(", at);
      if (text.find(passiveSlot + slot + ";") == std::string::npos ||
          copied == std::string::npos ||
          text.compare(copied + slot.size() + 11, 12, "__gti_mir_p_") != 0 ||
          text.find(slot + ".destroy();", lineEnd) == std::string::npos)
        return std::size_t{0};
      ++count;
    }
    return count;
  };
  expect(copiedSlots("read_pair__gti_mir_failure(",
                     "read_pair__gti_mir_failure(std::move(__gti_mir_v_") == 1,
         "the call should move the copy staged from the named binding and "
         "retire its slot afterwards");
  expect(copiedSlots("::__gti_program::pair_holder(",
                     ">{}, std::move(__gti_mir_v_") == 1,
         "the construction should move the copy staged from the named "
         "binding and retire its slot afterwards");
  expect(text.find("read_pair__gti_mir_failure(std::move(__gti_mir_p_") ==
             std::string::npos,
         "a copied binding should never be moved into the parameter");
  expect(
      text.find(
          "mir_lifetime_slot<::gti_internal::backend::expected<std::int32_t, "
          "::__gti_program::code_value>") == std::string::npos &&
          text.find("read_scalar__gti_mir_failure(__gti_mir_v_") !=
              std::string::npos,
      "a non-placement Expected copied argument should keep its ordinary "
      "SSA local");
}

// A cleanup-free Expected<Class, E> call result that replaces a whole mutable
// binding is staged in the same representation-only slot. After MIR's
// inactive replacement cleanup, the Assign move-assigns the staged wrapper
// into the binding and retires the moved-from representation. A
// non-placement Expected keeps its ordinary SSA local. (Regression for issue
// #224: the replacing call result raised GTI-B0001 in the caller.)
void testCleanupFreeExpectedClassAssignmentSlot() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-expected-class-assignment.gti", R"(
struct pair_value {
  int32_t first = 0;
  pair_value(int32_t first) : first(first) {}
};

struct code_value {
  int32_t code = 0;
  code_value(int32_t code) : code(code) {}
};

expected<pair_value, int32_t> make_pair(bool ok) {
  if (ok) { return pair_value(5); }
  return unexpected(4);
}

expected<int32_t, code_value> make_scalar(bool ok) {
  if (ok) { return 5; }
  return unexpected(code_value(4));
}

int main() {
  mut expected<pair_value, int32_t> changing = make_pair(true);
  changing = make_pair(false);
  mut expected<int32_t, code_value> scalar = make_scalar(true);
  scalar = make_scalar(false);
  return changing ? 1 : changing.error() + (scalar ? 1 : 0) - 4;
}
)");
  expect(frontend.canGenerateCode(),
         "the Expected class assignment fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string marker = ", std::move(__gti_mir_v_";
  const std::size_t assigned =
      text.find("::gti_internal::backend::assign_representation(__gti_mir_p_");
  const std::size_t moved =
      assigned == std::string::npos ? assigned : text.find(marker, assigned);
  const std::size_t lineEnd =
      assigned == std::string::npos ? assigned : text.find('\n', assigned);
  expect(moved != std::string::npos && moved < lineEnd,
         "the Assign should move a staged wrapper into the binding");
  if (moved == std::string::npos || moved >= lineEnd) {
    return;
  }
  const std::size_t idStart = moved + marker.size();
  const std::string slot =
      "__gti_mir_v_" +
      text.substr(idStart, text.find(".get()", idStart) - idStart);
  const std::size_t next = text.find_first_not_of(' ', lineEnd + 1);
  // A checked producer publishes through the slot's construction address; a
  // failure-free one constructs the slot from its returned wrapper.
  const std::size_t addressed = text.find(slot + ".construction_address()");
  const std::size_t published = addressed != std::string::npos
                                    ? addressed
                                    : text.find(slot + ".construct_from(");
  const std::size_t cleanup =
      text.rfind("__gti_mir_replacement_success_", assigned);
  expect(text.find("::gti_internal::backend::mir_lifetime_slot<::gti_internal::"
                   "backend::expected<"
                   "::__gti_program::pair_value, std::int32_t>, true> " +
                   slot + ";") != std::string::npos &&
             published != std::string::npos && published < assigned &&
             cleanup != std::string::npos && cleanup > published &&
             text.compare(next, slot.size() + 11, slot + ".destroy();") == 0,
         "the producer should publish into the slot, the inactive replacement "
         "cleanup should precede the Assign, and the slot should retire right "
         "after it");
  expect(
      text.find(
          "mir_lifetime_slot<::gti_internal::backend::expected<std::int32_t, "
          "::__gti_program::code_value>") == std::string::npos,
      "a non-placement Expected replacing a binding should keep its "
      "ordinary SSA local");
}

// Moving a cleanup-free Expected<Class, E> binding into a new binding
// constructs the new binding's lifetime slot from the moved source at the
// Move; the adjacent Initialize only activates the binding's drop. The
// success payload has no default constructor, so no value-initialized
// Expected local may exist. (Regression: the move raised GTI-B0001 "Expected
// value has no exact uninitialized publication".)
void testCleanupFreeExpectedClassMoveSlot() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-class-move.gti", R"(
#include <std/utility>

struct point {
  mut int32_t x;
  mut int32_t y;
};

expected<point, int32_t> make_point(int32_t x) {
  point made = {.x = x, .y = x + 1};
  return made;
}

int main() {
  mut auto first = make_point(3);
  auto moved = std::move(first);
  return moved.value().y - 4;
}
)");
  expect(frontend.canGenerateCode(),
         "the cleanup-free Expected move fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string marker =
      "// GTI MIR moved cleanup-free Expected initializes p";
  const std::size_t initialized = text.find(marker);
  expect(initialized != std::string::npos,
         "the moved binding's Initialize should only activate its drop");
  if (initialized == std::string::npos) {
    return;
  }
  const std::size_t idStart = initialized + marker.size();
  const std::string destination =
      "__gti_mir_p_" + text.substr(idStart, text.find('\n', idStart) - idStart);
  const std::size_t declared =
      text.find("::gti_internal::backend::mir_lifetime_slot<::gti_internal::"
                "backend::expected<"
                "::__gti_program::point, std::int32_t>> " +
                destination + ";");
  const std::size_t constructed = text.rfind(
      destination + ".construct(std::move(__gti_mir_p_", initialized);
  expect(declared != std::string::npos && constructed != std::string::npos &&
             declared < constructed &&
             text.find('\n', constructed) == text.rfind('\n', initialized),
         "the Move should construct the new binding's lifetime slot from the "
         "moved source immediately before its Initialize");
  expect(text.find("::gti_internal::backend::expected<::__gti_program::point, "
                   "std::int32_t> "
                   "__gti_mir_v_") == std::string::npos,
         "no Expected should value-initialize an ordinary local");
}

// Copying a cleanup-free Expected<Class, E> binding into a new binding
// copy-constructs the new binding's lifetime slot from the source at the
// Load; the adjacent Initialize only activates the binding's drop. The success
// payload has no default constructor, so no value-initialized Expected local
// may exist. (Regression: the copy was outside the C++ text vocabulary and
// reported GTI-B0001.)
void testCleanupFreeExpectedClassCopySlot() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-expected-class-copy.gti", R"(
struct point {
  mut int32_t x;
  mut int32_t y;
};

expected<point, int32_t> make_point(int32_t x) {
  point made = {.x = x, .y = x + 1};
  return made;
}

int main() {
  auto original = make_point(3);
  auto copy = original;
  return copy.value().y + original.value().x - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the cleanup-free Expected copy fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string marker = "// GTI MIR copied Expected initializes p";
  const std::size_t initialized = text.find(marker);
  expect(initialized != std::string::npos,
         "the copied binding's Initialize should only activate its drop");
  if (initialized == std::string::npos) {
    return;
  }
  const std::size_t idStart = initialized + marker.size();
  const std::string destination =
      "__gti_mir_p_" + text.substr(idStart, text.find('\n', idStart) - idStart);
  const std::size_t declared =
      text.find("::gti_internal::backend::mir_lifetime_slot<::gti_internal::"
                "backend::expected<"
                "::__gti_program::point, std::int32_t>> " +
                destination + ";");
  const std::size_t constructed =
      text.rfind(destination + ".construct(__gti_mir_p_", initialized);
  expect(declared != std::string::npos && constructed != std::string::npos &&
             declared < constructed &&
             text.find('\n', constructed) == text.rfind('\n', initialized),
         "the Load should copy-construct the new binding's lifetime slot "
         "from the source immediately before its Initialize");
  expect(text.find("::gti_internal::backend::expected<::__gti_program::point, "
                   "std::int32_t> "
                   "__gti_mir_v_") == std::string::npos,
         "no Expected should value-initialize an ordinary local");
}

// Every other source replacing a whole Expected<Class, E> binding spells the
// native wrapper's own assignment after MIR's replacement cleanup, with no
// value-initialized Expected local. A cleanup-free copy, move, Unexpected and
// payload construction follow a passive cleanup; an owning call result moves
// out of its value-rooted slot after the active cleanup, and that slot's
// Drop retires only the moved-from representation. (Regression for issue
// #225: copies, Unexpected values and payloads raised GTI-B0001, a move
// assigned to the lifetime slot itself, and owning results raised GTI-B0001.)
void testExpectedClassReplacementSources() {
  for (const std::string_view name :
       {"expected_pointer_boundary", "expected_owned_payload_replacement",
        "getter_owned_field_replacement"}) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         ("fixtures/" + std::string(name) + ".gti");
    std::ifstream input(fixture);
    expect(input.good(), "Expected boundary regression fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(),
           "pointer boundaries and owning payload replacement must verify");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
           "Expected operations must compose with existing field storage");
    std::size_t checked = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &call : block.instructions) {
          const bool extraction =
              name != "getter_owned_field_replacement" &&
              call.kind == lang::MirInstructionKind::Call &&
              (call.intrinsic == lang::IntrinsicKind::ExpectedValue ||
               call.intrinsic == lang::IntrinsicKind::ExpectedError);
          const bool construction =
              name == "getter_owned_field_replacement" &&
              call.kind == lang::MirInstructionKind::Construct;
          if ((!extraction && !construction) || !call.result ||
              call.info.type.kind != lang::SemanticType::Class ||
              !call.successResultDrop)
            continue;
          const lang::MirInstruction *assignment = nullptr;
          for (const auto &consumerBlock : function.body.blocks)
            for (const auto &consumer : consumerBlock.instructions)
              if (consumer.kind == lang::MirInstructionKind::Assign &&
                  consumer.operands.size() == 1 &&
                  consumer.operands.front().kind ==
                      lang::MirOperandKind::Value &&
                  consumer.operands.front().value == *call.result)
                assignment = &consumer;
          if (!assignment)
            continue;
          if (construction) {
            const auto *destination =
                assignment->destination
                    ? function.body.findPlace(*assignment->destination)
                    : nullptr;
            if (!destination ||
                destination->root != lang::MirPlaceRootKind::Loan ||
                destination->projections.empty())
              continue;
          }
          ++checked;
          for (int mutation = 0; mutation != 4; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &instruction : changedBlock.instructions) {
                if (instruction.id == call.id && mutation == 0)
                  instruction.successResultDrop.reset();
                if (instruction.id == assignment->id && mutation == 1)
                  instruction.lifecycle.clear();
                if (instruction.id == assignment->id && mutation == 2)
                  instruction.lifecycle.front().conditional = true;
                if (instruction.id == assignment->replacementPair &&
                    mutation == 3)
                  instruction.replacementCleanup.reset();
              }
            const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                               function.id};
            expect(
                !lang::verifyMirProgram(malformed).valid(),
                "activation, transfer and old-value cleanup are MIR authority");
            expect(!lang::CppMirBodyEmitter(malformed, map)
                        .supportsFailureBodyText(address),
                   "replacement rejects missing or conditional lifetime "
                   "witnesses");
          }
        }
      }
    }
    if (name == "expected_owned_payload_replacement")
      expect(checked == 3, "value, error and source-defined string payloads "
                           "exercise replacement");
    if (name == "getter_owned_field_replacement")
      expect(checked == 2, "fresh owning values replace fields through nested "
                           "non-failing getters: " +
                               std::to_string(checked));
  }
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-expected-class-replacement-sources.gti", R"gti(
#include <std/utility>

struct pair_value {
  int32_t first = 0;
  pair_value(int32_t first) : first(first) {}
};

class tracked {
public:
  int32_t value = 0;
  tracked(int32_t value) : value(value) {}
  ~tracked() {}
};

expected<pair_value, int32_t> make_pair(bool ok) {
  if (ok) { return pair_value(5); }
  return unexpected(4);
}

expected<tracked, int32_t> make_tracked(bool ok) {
  if (ok) { return tracked(5); }
  return unexpected(4);
}

int main() {
  mut expected<pair_value, int32_t> target = make_pair(true);
  expected<pair_value, int32_t> source = make_pair(false);
  expected<pair_value, int32_t> movable = make_pair(true);
  target = source;
  target = std::move(movable);
  target = unexpected(8);
  target = pair_value(3);
  mut expected<tracked, int32_t> owned = make_tracked(true);
  owned = make_tracked(false);
  return (target ? target.value().first : 0) + (owned ? 1 : owned.error()) - 7;
}
)gti");
  expect(frontend.canGenerateCode(),
         "the Expected class replacement fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  std::vector<std::string> lines;
  for (std::size_t start = 0; start < text.size();) {
    const std::size_t end = std::min(text.find('\n', start), text.size());
    const std::size_t first = text.find_first_not_of(' ', start);
    lines.push_back(first < end ? text.substr(first, end - first) : "");
    start = end + 1;
  }
  const auto starts = [](const std::string &line, std::string_view prefix) {
    return line.compare(0, prefix.size(), prefix) == 0;
  };
  const auto ends = [](const std::string &line, std::string_view suffix) {
    return line.size() >= suffix.size() &&
           line.compare(line.size() - suffix.size(), suffix.size(), suffix) ==
               0;
  };
  // Each assignment must directly follow its replacement cleanup.
  const auto afterCleanup = [&](std::size_t index) {
    return index > 0 &&
           starts(lines[index - 1], "__gti_mir_replacement_success_");
  };
  std::size_t copies = 0;
  std::size_t moves = 0;
  std::size_t owningMoves = 0;
  std::size_t unexpecteds = 0;
  std::size_t payloads = 0;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string &line = lines[index];
    if (!starts(line, "__gti_mir_p_") && !starts(line, "::gti_internal"))
      continue;
    if (line.find(".get() = __gti_mir_p_") != std::string::npos &&
        ends(line, ".get();") && afterCleanup(index))
      ++copies;
    const std::size_t moved = line.find(".get() = std::move(__gti_mir_p_");
    if (moved != std::string::npos && ends(line, ".get());") &&
        afterCleanup(index)) {
      const std::size_t idStart = moved + 19;
      const std::string slot =
          line.substr(idStart, line.find(".get())", idStart) - idStart);
      if (index + 1 < lines.size() && lines[index + 1] == slot + ".destroy();")
        ++owningMoves;
      else
        ++moves;
    }
    if (starts(line, "::gti_internal::backend::assign_representation(__gti_"
                     "mir_p_") &&
        line.find(".get(), ::gti_internal::backend::unexpected(") !=
            std::string::npos &&
        afterCleanup(index))
      ++unexpecteds;
    if (line.find(".get() = ::__gti_program::pair_value(") !=
            std::string::npos &&
        afterCleanup(index))
      ++payloads;
  }
  expect(copies == 1 && moves == 1 && unexpecteds == 1 && payloads == 1,
         "a cleanup-free copy, move, Unexpected and payload should each "
         "assign the binding's wrapper directly after its replacement "
         "cleanup");
  expect(owningMoves == 1 &&
             text.find("__gti_mir_unique_cleanup_failure") != std::string::npos,
         "an owning call result should move out of its slot after the active "
         "replacement cleanup and retire the moved-from slot");
  expect(text.find("::gti_internal::backend::expected<::__gti_program::pair_"
                   "value, std::int32_t> "
                   "__gti_mir_v_") == std::string::npos,
         "no replacement source should declare a value-initialized Expected "
         "local");
}

// An owning Expected<Class, E> binding replaced by a move, an Unexpected or a
// payload keeps MIR's active replacement cleanup and retires each staged
// shell exactly once: a moved binding and a converted payload each stage in
// their value-rooted slot, and the converted payload, which MIR transfers out
// at the Assign, has its shell ended there. An Unexpected wrapping a fresh
// cleanup-free class error constructs inline when it initializes or replaces
// a binding behind a passive pair. Behind an owning binding's active cleanup,
// the error is constructed in its slot at its own position and moved in at
// the Assign. (Regression for issue #227: these raised GTI-B0001, the payload
// conversion while constructing MIR.)
void testShortCircuitExpectedPayloadFieldView() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-short-circuit-expected-payload.gti", R"(
class counted {
public:
  int32_t number;
  counted(int32_t number) : number(number) {}
  ~counted() {}
};

expected<counted, int32_t> make_counted(bool ok) {
  if (ok) { return counted(5); }
  return unexpected(int32_t(1));
}

bool check(bool skip) { return skip || make_counted(true).value().number == 5; }

int main() { return check(false) ? 0 : 1; }
)");
  expect(frontend.canGenerateCode(),
         "the short-circuit expected payload fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  // The checked extraction moves the payload out of the expected result's
  // conditional temporary into its own, and the field is read there.
  const std::string extraction =
      "::gti_internal::backend::mir_expected_value_move_v1(__gti_mir_p_";
  const std::size_t extracted = text.find(extraction);
  const std::size_t target =
      extracted == std::string::npos
          ? std::string::npos
          : text.find(".get(), &__gti_mir_p_", extracted);
  const std::size_t nameStart = target == std::string::npos
                                    ? std::string::npos
                                    : target + std::string(".get(), &").size();
  const std::string payload =
      nameStart == std::string::npos
          ? std::string()
          : text.substr(nameStart, text.find(')', nameStart) - nameStart);
  expect(!payload.empty() && text.find(" = " + payload + ".get().number;",
                                       extracted) != std::string::npos,
         "the payload should move into its conditional temporary and its "
         "field should be read from that temporary");
  expect(text.find("::__gti_program::counted __gti_mir_v_") ==
             std::string::npos,
         "the viewed payload should not declare an ordinary local");
}

void testOwningExpectedReplacementSources() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-owning-expected-replacement-sources.gti", R"gti(
#include <std/utility>

struct pair_value {
  int32_t first = 0;
  pair_value(int32_t first) : first(first) {}
};

struct code_value {
  int32_t code = 0;
  code_value(int32_t code) : code(code) {}
};

class tracked {
public:
  int32_t value = 0;
  tracked(int32_t value) : value(value) {}
  ~tracked() {}
};

expected<tracked, int32_t> make_tracked(bool ok) {
  if (ok) { return tracked(5); }
  return unexpected(4);
}

expected<tracked, code_value> make_coded_tracked(bool ok) {
  if (ok) { return tracked(5); }
  return unexpected(code_value(4));
}

int main() {
  mut expected<tracked, int32_t> owned = make_tracked(true);
  expected<tracked, int32_t> moved = make_tracked(true);
  owned = std::move(moved);
  owned = unexpected(3);
  owned = tracked(9);
  mut expected<pair_value, code_value> coded = unexpected(code_value(6));
  coded = unexpected(code_value(7));
  mut expected<tracked, code_value> owned_coded = make_coded_tracked(true);
  owned_coded = unexpected(code_value(8));
  return (owned ? owned.value().value : 0) + (coded ? 0 : coded.error().code) +
         (owned_coded ? 0 : owned_coded.error().code) - 24;
}
)gti");
  expect(frontend.canGenerateCode(),
         "the owning Expected replacement fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  std::vector<std::string> lines;
  for (std::size_t start = 0; start < text.size();) {
    const std::size_t end = std::min(text.find('\n', start), text.size());
    const std::size_t first = text.find_first_not_of(' ', start);
    lines.push_back(first < end ? text.substr(first, end - first) : "");
    start = end + 1;
  }
  const auto contains = [](const std::string &line, std::string_view part) {
    return line.find(part) != std::string::npos;
  };
  std::size_t movedShells = 0;
  std::size_t transferredShells = 0;
  for (std::size_t index = 0; index + 1 < lines.size(); ++index) {
    const std::string &line = lines[index];
    const std::size_t moved = line.find(".get() = std::move(__gti_mir_p_");
    if (moved == std::string::npos || !contains(line, ".get());"))
      continue;
    const std::size_t idStart = moved + 19;
    const std::string slot =
        line.substr(idStart, line.find(".get())", idStart) - idStart);
    if (lines[index + 1] ==
        slot + ".destroy(); // end transferred-out payload shell")
      ++transferredShells;
    else if (lines[index + 1] == slot + ".destroy();" &&
             text.find(slot + ".construct(std::move(__gti_mir_p_") !=
                 std::string::npos)
      ++movedShells;
  }
  expect(movedShells == 1,
         "an owning move should stage in its slot, move into the binding and "
         "retire the moved-from shell");
  expect(transferredShells == 1,
         "an owning payload should move into the binding and end its "
         "transferred-out shell at the Assign");
  expect(
      text.find(".get(), ::gti_internal::backend::unexpected(__gti_mir_v_") !=
          std::string::npos,
      "an owning binding should accept an inline Unexpected replacement");
  expect(text.find(".construct(::gti_internal::backend::unexpected(::__gti_"
                   "program::code_value(") != std::string::npos &&
             text.find(".get(), "
                       "::gti_internal::backend::unexpected(::__gti_program::"
                       "code_value(") != std::string::npos,
         "an Unexpected wrapping a fresh class error should construct inline "
         "at a binding initializer and replacement");
  std::size_t stagedErrors = 0;
  for (std::size_t index = 0; index + 1 < lines.size(); ++index) {
    const std::string marker =
        ".get(), ::gti_internal::backend::unexpected(std::move(__gti_mir_v_";
    const std::size_t staged = lines[index].find(marker);
    if (staged == std::string::npos ||
        !contains(lines[index], "assign_representation("))
      continue;
    const std::size_t idStart = staged + marker.size() - 12;
    const std::string slot = lines[index].substr(
        idStart, lines[index].find(".get())", idStart) - idStart);
    if (lines[index + 1] == slot + ".destroy();" &&
        text.find(slot + ".construct(") != std::string::npos)
      ++stagedErrors;
  }
  expect(stagedErrors == 1,
         "a fresh class error replacing an owning binding should be "
         "constructed in its slot at its MIR position, moved into the "
         "Unexpected at the Assign and retired there");
  expect(text.find("::gti_internal::backend::expected<::__gti_program::tracked,"
                   " std::int32_t> "
                   "__gti_mir_v_") == std::string::npos,
         "no owning replacement should declare a value-initialized Expected "
         "local");
}

void testExpectedConsumingMemberReturn() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-expected-consuming-member-return.gti", R"(
#include <std/utility>
class Item {
  int divisor = 1;
public:
  Item() = default;
  Item(Item& other) = delete;
  Item(Item&& other) = default;
  ~Item() { int checked = 1 / this.divisor; }
  Item Take(int divisor) && {
    int checked = 2 / divisor;
    return std::move(this);
  }
};
expected<Item, int> Make(int divisor) {
  Item item{};
  return std::move(item).Take(divisor);
}
int main() { auto result = Make(1); return result ? 0 : 1; }
)");
  expect(frontend.canGenerateCode(),
         "a consuming member may supply an owning expected payload");
  if (!frontend.canGenerateCode())
    return;
  lang::MirBodyAddress address{};
  lang::MirInstructionId producer = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.returnType.kind != lang::SemanticType::Expected)
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.callableInvocation ==
                lang::CallableInvocationCapability::Once &&
            instruction.successResultDrop) {
          address = {.kind = lang::MirBodyKind::Function, .owner = function.id};
          producer = instruction.id;
        }
  }
  expect(producer != 0,
         "the payload producer retains its consuming capability");
  if (producer == 0)
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a consuming member uses the exact ordinary pending-payload proof");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == producer) {
          if (mutation == 0)
            instruction.callableInvocation =
                lang::CallableInvocationCapability::Read;
          else if (mutation == 1)
            instruction.callableInvocation.reset();
          else if (mutation == 2)
            instruction.receiver.reset();
          else if (mutation == 3)
            instruction.successResultDrop.reset();
          else
            instruction.successResultDrop = 0;
        }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "a changed consuming receiver or payload activation fails MIR "
           "verification " +
               std::to_string(mutation));
    const lang::CppMirBodyEmitter rejected(malformed, map);
    expect(!rejected.analyze(address).ready() ||
               !rejected.supportsFailureBodyText(address),
           "a malformed consuming return cannot enter backend emission " +
               std::to_string(mutation));
  }
}

// A cleanup-free receiver moved out of an immutable parameter is a prepared
// parameter that the consuming call transfers to its callee. The returned
// result keeps its own slot across the moved-from parameter's drop. Forged
// handoff facts must not reach emission.
void testPreparedConsumingReceiverReturn() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-prepared-consuming-receiver-return.gti", R"(
class Tracked {
  mut int id;
public:
  Tracked(int value) : id(value) {}
  ~Tracked() { this.id = 0; }
  int read() { return this.id; }
};
class Maker {
  int base;
public:
  Maker(int base) : base(base) {}
  Tracked make() && { return Tracked(this.base); }
};
Tracked take(Maker maker) { return std::move(maker).make(); }
int main() { Tracked kept = take(Maker(1)); return kept.read() - 1; }
)");
  expect(frontend.canGenerateCode(),
         "a consuming member may consume an immutable cleanup-free parameter");
  if (!frontend.canGenerateCode())
    return;
  lang::MirBodyAddress address{};
  lang::MirInstructionId call = 0;
  lang::MirInstructionId stage = 0;
  lang::MirDropObligationId prepared = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputRole != lang::MirCallInputRole::Receiver ||
            !instruction.preparedParameterDrop)
          continue;
        for (const auto &consumer : block.instructions)
          if (consumer.kind == lang::MirInstructionKind::Call &&
              consumer.receiver && instruction.result &&
              consumer.receiver->value == *instruction.result &&
              consumer.callableInvocation ==
                  lang::CallableInvocationCapability::Once) {
            address = {.kind = lang::MirBodyKind::Function,
                       .owner = function.id};
            call = consumer.id;
            stage = instruction.id;
            prepared = *instruction.preparedParameterDrop;
          }
      }
  expect(call != 0 && stage != 0,
         "the moved receiver is staged as the call's prepared parameter");
  if (call == 0 || stage == 0)
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyze(address).ready() && emitter.supportsBodyText(address),
         "the returned result accounts for the prepared receiver handoff");
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions) {
        if (instruction.id == call && mutation == 0)
          std::erase_if(instruction.lifecycle, [&](const auto &event) {
            return event.source == prepared;
          });
        else if (instruction.id == call && mutation == 1)
          instruction.lifecycle.push_back(
              {.kind = lang::MirLifecycleEventKind::TransferOut,
               .source = prepared});
        else if (instruction.id == stage && mutation == 2)
          instruction.lifecycle.clear();
        else if (instruction.id == call && mutation == 3)
          instruction.callableInvocation =
              lang::CallableInvocationCapability::Mutable;
      }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "a forged prepared receiver handoff fails MIR verification " +
               std::to_string(mutation));
    const lang::CppMirBodyEmitter rejected(malformed, map);
    expect(!rejected.analyze(address).ready() ||
               !rejected.supportsBodyText(address),
           "a forged prepared receiver handoff cannot enter backend "
           "emission " +
               std::to_string(mutation));
  }
}

// A failure-free call in a conditional arm that consumes a prepared input
// (a moved cleanup-free receiver or by-value argument) transfers it out beside
// its own result Initialize. The result is still published into the
// conditional temporary that a binding, a Return or a field read consumes.
// Only exactly one unconditional transfer per prepared input and one result
// activation are admitted.
void testConditionalPreparedInputResult() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-conditional-prepared-input-result.gti", R"(
class Tracked {
  mut int id;
public:
  Tracked(int value) : id(value) {}
  ~Tracked() { this.id = 0; }
  int read() { return this.id; }
};
class Viewed {
public:
  mut int id;
  Viewed(int value) : id(value) {}
  ~Viewed() { this.id = 0; }
};
class Maker {
  int base;
public:
  Maker(int base) : base(base) {}
  int get() { return this.base; }
  Tracked make() && { return Tracked(this.base); }
  Viewed view() && { return Viewed(this.base); }
};
Tracked build(Maker maker) { return Tracked(maker.get()); }
Viewed show(Maker maker) { return Viewed(maker.get()); }
int bound(bool left, Maker a, Maker b) {
  Tracked kept = left ? std::move(a).make() : build(std::move(b));
  return kept.read();
}
Tracked returned(bool left, Maker a, Maker b) {
  return left ? build(std::move(a)) : std::move(b).make();
}
int viewed(bool left, Maker a, Maker b) {
  return left ? std::move(a).view().id : show(std::move(b)).id;
}
int main() {
  Tracked kept = returned(true, Maker(2), Maker(3));
  return bound(false, Maker(1), Maker(4)) + kept.read() +
         viewed(true, Maker(5), Maker(6)) - 11;
}
)");
  expect(frontend.canGenerateCode(),
         "conditional results of calls consuming moved inputs verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  std::size_t bodies = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    // Each arm's call transfers its prepared input and activates its result.
    std::vector<lang::MirInstructionId> calls;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.lifecycle.size() == 2 &&
            instruction.lifecycle[0].kind ==
                lang::MirLifecycleEventKind::TransferOut &&
            instruction.lifecycle[1].kind ==
                lang::MirLifecycleEventKind::Initialize)
          calls.push_back(instruction.id);
    if (calls.size() != 2)
      continue;
    ++bodies;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyze(address).ready() &&
               emitter.supportsBodyText(address),
           "a conditional result accounts for its call's prepared input");
    for (const lang::MirInstructionId call : calls) {
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        for (auto &block : body->blocks)
          for (auto &instruction : block.instructions) {
            if (instruction.id != call)
              continue;
            const auto transfer = instruction.lifecycle[0];
            const auto activation = instruction.lifecycle[1];
            if (mutation == 0)
              instruction.lifecycle = {activation};
            else if (mutation == 1)
              instruction.lifecycle.push_back(transfer);
            else if (mutation == 2)
              instruction.lifecycle[0].conditional = true;
            else
              instruction.lifecycle.push_back(activation);
          }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "a forged conditional prepared input transfer fails MIR "
               "verification " +
                   std::to_string(mutation));
        const lang::CppMirBodyEmitter rejected(malformed, map);
        expect(!rejected.analyze(address).ready() ||
                   !rejected.supportsBodyText(address),
               "a forged conditional prepared input transfer cannot enter "
               "backend emission " +
                   std::to_string(mutation));
      }
    }
  }
  expect(bodies == 3, "the bound, returned and field-read conditionals were "
                      "exercised: " +
                          std::to_string(bodies));
}

// A generic owner's representation row takes its special members from the
// generic declaration, so maybe<Tracked> declares value-initialization even
// though the instance over a cleanup-bearing payload is move-only. Moving it
// out of a binding into a consuming call stages a MoveValue receiver that
// aliases the Move's full-expression slot. That stage owns no storage and is
// not a copy stage, so local declaration must not demand any for it.
void testMovedGenericOwnerReceiverStage() {
  const auto frontend =
      analyzeWithStandardLibrary("cpp-mir-moved-generic-owner-receiver.gti", R"(
class Tracked {
  int id;
public:
  Tracked(int value) : id(value) {}
  ~Tracked() {}
};
enum class state<T> { empty, engaged(T value) };
class maybe<T> {
  mut state<T> slot;
public:
  maybe() : slot(state<T>::empty) {}
  maybe(T value) : slot(state<T>::engaged(std::move(value))) {}
  int probe() && { return 1; }
};
int main() {
  maybe<Tracked> owner = maybe<Tracked>(Tracked(7));
  int probed = std::move(owner).probe();
  return probed - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "a consuming member may consume a moved generic owner");
  if (!frontend.canGenerateCode())
    return;
  lang::MirBodyAddress address{};
  bool failureForm = false;
  lang::MirInstructionId move = 0;
  lang::MirInstructionId stage = 0;
  lang::MirInstructionId call = 0;
  lang::MirValueId staged = 0;
  lang::MirDropObligationId lifetime = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputRole != lang::MirCallInputRole::Receiver ||
            instruction.callInputKind != lang::HirCallInputKind::MoveValue ||
            !instruction.result || instruction.operands.size() != 1)
          continue;
        const auto *moved =
            function.body.findValue(instruction.operands.front().value);
        const lang::MirInstruction *producer = nullptr;
        for (const auto &candidate : block.instructions)
          if (moved != nullptr && candidate.id == moved->definition)
            producer = &candidate;
        for (const auto &consumer : block.instructions)
          if (producer != nullptr &&
              producer->kind == lang::MirInstructionKind::Move &&
              producer->lifecycle.size() == 1 &&
              consumer.kind == lang::MirInstructionKind::Call &&
              consumer.receiver &&
              consumer.receiver->value == *instruction.result &&
              consumer.callableInvocation ==
                  lang::CallableInvocationCapability::Once) {
            address = {.kind = lang::MirBodyKind::Function,
                       .owner = function.id};
            failureForm = function.mayRaiseDefinedFailure;
            move = producer->id;
            stage = instruction.id;
            call = consumer.id;
            staged = *instruction.result;
            lifetime = producer->lifecycle.front().target;
          }
      }
  expect(call != 0 && stage != 0 && lifetime != 0,
         "the moved owner is staged as a receiver aliasing its full-expression "
         "slot");
  if (call == 0 || stage == 0 || lifetime == 0)
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const auto plan = emitter.buildPlan(address);
  expect(plan.admitted(),
         "the moved generic owner's receiver stage needs no local of its own");
  if (!plan.admitted())
    return;
  const auto text = failureForm
                        ? emitter.emitFailureBodyText(plan, "generic-owner", 1)
                        : emitter.emitBodyText(plan, "generic-owner", 1);
  expect(text.emitted() &&
             text.text.find("call input " + std::to_string(staged) +
                            " aliases the caller-owned receiver lifetime") !=
                 std::string::npos &&
             text.text.find("__gti_mir_v_" + std::to_string(staged) + ';') ==
                 std::string::npos &&
             text.text.find("__gti_mir_v_" + std::to_string(staged) + '{') ==
                 std::string::npos,
         "the call reads the moved owner through its receiver lifetime slot");
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    if (mutation == 0)
      for (auto &drop : body->dropObligations)
        if (drop.id == lifetime)
          drop.fullExpression = 0;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions) {
        if (instruction.id == move && mutation == 1)
          instruction.lifecycle.clear();
        else if (instruction.id == call && mutation == 2)
          instruction.lifecycle.push_back(
              {.kind = lang::MirLifecycleEventKind::TransferOut,
               .source = lifetime});
        else if (instruction.id == stage && mutation == 3)
          instruction.preparedParameterDrop = lifetime;
      }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "a forged moved owner receiver handoff fails MIR verification " +
               std::to_string(mutation));
    expect(
        !lang::CppMirBodyEmitter(malformed, map).buildPlan(address).admitted(),
        "a forged moved owner receiver handoff cannot enter backend "
        "emission " +
            std::to_string(mutation));
  }
}

// Once a later argument can fail, the same prepared receiver occupies its
// slot from the stage: the call consumes that slot, and the failure edge
// destroys it once. Removed, duplicated or ordinary failure-edge Drops must not
// reach emission.
void testPreparedConsumingReceiverLaterFailure() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-prepared-consuming-receiver-later-failure.gti", R"(
class Tracked {
  mut int id;
public:
  Tracked(int value) : id(value) {}
  ~Tracked() { this.id = 0; }
  int read() { return this.id; }
};
class Maker {
  int base;
public:
  Maker(int base) : base(base) {}
  Tracked add(int value) && { return Tracked(this.base + value); }
};
int take(Maker maker, int divisor) {
  Tracked made = std::move(maker).add(1 / divisor);
  return made.read();
}
int main() { return take(Maker(1), 1) - 2; }
)");
  expect(frontend.canGenerateCode(),
         "a consuming member may take a later fallible argument");
  if (!frontend.canGenerateCode())
    return;
  lang::MirBodyAddress address{};
  lang::MirInstructionId stage = 0;
  lang::MirInstructionId cleanup = 0;
  lang::MirPlaceId slot = 0;
  lang::MirDropObligationId prepared = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputRole == lang::MirCallInputRole::Receiver &&
            instruction.preparedParameterDrop && instruction.destination) {
          address = {.kind = lang::MirBodyKind::Function, .owner = function.id};
          stage = instruction.id;
          slot = *instruction.destination;
          prepared = *instruction.preparedParameterDrop;
        }
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().failureCleanup &&
            instruction.lifecycle.front().source == prepared && prepared != 0)
          cleanup = instruction.id;
      }
  expect(stage != 0 && cleanup != 0,
         "the later argument's failure edge drops the prepared receiver");
  if (stage == 0 || cleanup == 0)
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the prepared receiver is materialized ahead of the later argument");
  const std::string text =
      emitter.emitFailureBodyText(address, "prepared-receiver-test-v0", 1).text;
  const std::string name = "__gti_mir_p_" + std::to_string(slot);
  std::size_t destroys = 0;
  for (std::size_t at = text.find(name + ".destroy();");
       at != std::string::npos; at = text.find(name + ".destroy();", at + 1))
    ++destroys;
  expect(text.find(name + ".construct(std::move(") != std::string::npos &&
             text.find("std::move(" + name + ".get()).") != std::string::npos &&
             destroys == 2,
         "the call consumes the receiver slot; it is retired after the call "
         "and destroyed once on the failure edge:\n" +
             text);
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const auto &instruction) { return instruction.id == cleanup; });
      if (found == block.instructions.end())
        continue;
      if (mutation == 0) {
        block.instructions.erase(found);
      } else if (mutation == 1) {
        auto duplicate = *found;
        duplicate.id = 0;
        for (const auto &other : body->blocks)
          for (const auto &instruction : other.instructions)
            duplicate.id = std::max(duplicate.id, instruction.id + 1);
        block.instructions.insert(std::next(found), duplicate);
      } else {
        found->lifecycle.front().failureCleanup = false;
      }
      break;
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "a forged prepared receiver cleanup fails MIR verification " +
               std::to_string(mutation));
    const lang::CppMirBodyEmitter rejected(malformed, map);
    expect(!rejected.analyze(address).ready() ||
               !rejected.supportsFailureBodyText(address),
           "a forged prepared receiver cleanup cannot enter backend "
           "emission " +
               std::to_string(mutation));
  }
}

// A construction consuming a cleanup-free Expected<Class, E> argument may
// publish its own result through its consuming Return or into a constructor
// field. The success class has no default constructor: a failure-free
// producer constructs the staged slot, the construction moves its wrapper and
// the slot retires right after. A cleanup-owning payload keeps MIR's
// value-rooted transfer slot. (Regression: the former gti-lang-official issue
// #6 emitted a default-constructed ::gti_internal::backend::expected<Item, E>
// local.)
void testExpectedClassArgumentReturnedConstruction() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-expected-class-returned-construction.gti", R"(
struct item_value {
  int32_t number;
  item_value(int32_t number) : number(number) {}
};

class tracked {
public:
  int32_t value = 0;
  tracked(int32_t value) : value(value) {}
  ~tracked() {}
};

expected<item_value, int32_t> make_item() { return item_value(7); }

expected<tracked, int32_t> make_tracked() { return tracked(7); }

struct item_holder {
  int32_t number;
  item_holder(expected<item_value, int32_t> item)
      : number(item ? item.value().number : 0) {}
};

struct tracked_holder {
  int32_t value;
  tracked_holder(expected<tracked, int32_t> item)
      : value(item ? item.value().value : 0) {}
};

item_holder hold_item() { return item_holder(make_item()); }

tracked_holder hold_tracked() { return tracked_holder(make_tracked()); }

struct item_owner {
  item_holder held;
  item_owner() : held(item_holder(make_item())) {}
};

int main() {
  item_holder held = hold_item();
  tracked_holder owned = hold_tracked();
  item_owner owner{};
  return held.number + owned.value + owner.held.number - 21;
}
)");
  expect(frontend.canGenerateCode(),
         "the returned Expected construction fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string slotType = "::gti_internal::backend::mir_lifetime_slot<::"
                               "gti_internal::backend::expected<::__gti_"
                               "program::item_value, std::int32_t>, true> ";
  // Counts item_holder constructions publishing at `destination` that move a
  // slot the producer constructed and retire it on the next line.
  const auto publishedConstructions = [&](std::string_view destination) {
    const std::string_view marker = ">{}, std::move(__gti_mir_v_";
    std::size_t count = 0;
    for (std::size_t at = text.find(marker); at != std::string::npos;
         at = text.find(marker, at + marker.size())) {
      const std::size_t lineStart = text.rfind('\n', at) + 1;
      const std::size_t lineEnd = text.find('\n', at);
      const std::size_t idStart = at + marker.size();
      const std::size_t idEnd = text.find(".get())", idStart);
      if (lineEnd == std::string::npos || idEnd == std::string::npos ||
          idEnd > lineEnd)
        return std::size_t{0};
      const std::string_view line =
          std::string_view(text).substr(lineStart, lineEnd - lineStart);
      if (line.find("::__gti_program::item_holder(") ==
              std::string_view::npos ||
          line.find(destination) == std::string_view::npos)
        continue;
      const std::string slot =
          "__gti_mir_v_" + text.substr(idStart, idEnd - idStart);
      const std::size_t next = text.find_first_not_of(' ', lineEnd + 1);
      if (text.find(slotType + slot + ";") == std::string::npos ||
          text.find(slot + ".construct_from([&]() { return ::__gti_program::"
                           "__gti_fn_") == std::string::npos ||
          text.compare(next, slot.size() + 11, slot + ".destroy();") != 0)
        return std::size_t{0};
      ++count;
    }
    return count;
  };
  expect(publishedConstructions("(__gti_mir_out_result)) ") == 1,
         "a returned construction should publish into the transformed result "
         "from the staged wrapper and retire it afterwards");
  expect(publishedConstructions("std::addressof(") == 1,
         "a member-initializer construction should publish into its field "
         "from the staged wrapper and retire it afterwards");
  expect(text.find("::gti_internal::backend::expected<::__gti_program::item_"
                   "value, std::int32_t> "
                   "__gti_mir_v_") == std::string::npos,
         "no Expected without a default-constructible payload should "
         "value-initialize an ordinary local");
  const std::size_t owned =
      text.find("::__gti_program::tracked_holder(::gti_internal::backend::"
                "mir_failure_constructor_tag_v1<");
  expect(owned != std::string::npos &&
             text.find(">{}, __gti_mir_p_", owned) ==
                 text.find(">{}, ", owned) &&
             text.find(".take()", owned) < text.find('\n', owned),
         "a returned construction consuming a cleanup-owning Expected should "
         "take it from MIR's value-rooted transfer slot");
}

// A cleanup-free Expected<Class, E> call temporary read once by its state test
// or a checked value() or error() lives in representation-only storage: the
// producer constructs the slot, the observer reads the wrapper there, and the
// representation retires right after that read. A class payload moves into
// its own field-read slot. An owning temporary is read in place from MIR's
// value-rooted slot, which MIR's own Drop retires. (Regression: these
// value-initialized ::gti_internal::backend::expected<Item, E> or reported
// GTI-B0001.)
void testExpectedTemporaryObservationSlot() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-expected-temporary-observation.gti", R"(
struct item_value {
  int32_t number;
  item_value(int32_t number) : number(number) {}
};

class tracked {
public:
  int32_t value = 0;
  tracked(int32_t value) : value(value) {}
  ~tracked() {}
};

expected<item_value, int32_t> make_item(bool ok) {
  if (ok) { return item_value(7); }
  return unexpected(int32_t(3));
}

expected<tracked, int32_t> make_tracked(bool ok) {
  if (ok) { return tracked(7); }
  return unexpected(int32_t(3));
}

int32_t observe(bool ok) {
  if (make_item(ok)) { return make_item(ok).value().number; }
  return make_item(ok).error() + make_tracked(ok).error();
}

int main() { return observe(true) + observe(false) - 13; }
)");
  expect(frontend.canGenerateCode(),
         "the Expected temporary observation fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string slotType =
      "::gti_internal::backend::mir_lifetime_slot<::gti_internal::backend::"
      "expected<::__gti_"
      "program::item_value, std::int32_t>, true> __gti_mir_v_";
  // Each observed slot is constructed by its producer, read once through
  // get() on the next line, and retired on the line after that read.
  std::size_t observed = 0;
  std::string payloadSlot;
  for (std::size_t at = text.find(slotType); at != std::string::npos;
       at = text.find(slotType, at + slotType.size())) {
    const std::size_t idStart = at + slotType.size();
    const std::string slot =
        "__gti_mir_v_" +
        text.substr(idStart, text.find(';', idStart) - idStart);
    const std::size_t produced = text.find(
        slot + ".construct_from([&]() { return ::__gti_program::__gti_fn_");
    const std::size_t read = produced == std::string::npos
                                 ? std::string::npos
                                 : text.find(slot + ".get()", produced);
    const std::size_t lineEnd =
        read == std::string::npos ? std::string::npos : text.find('\n', read);
    const std::size_t next = lineEnd == std::string::npos
                                 ? std::string::npos
                                 : text.find_first_not_of(' ', lineEnd + 1);
    if (next == std::string::npos ||
        text.compare(next, slot.size() + 11, slot + ".destroy();") != 0)
      continue;
    ++observed;
    const std::string move = "mir_expected_value_move_v1(" + slot + ".get(), &";
    const std::size_t moved = text.rfind(move, lineEnd);
    if (moved != std::string::npos && moved > produced)
      payloadSlot =
          text.substr(moved + move.size(), text.find(')', moved + move.size()) -
                                               moved - move.size());
  }
  expect(observed == 3,
         "the state test, the value() extraction and the error() extraction "
         "should each read one constructed slot and retire it: " +
             std::to_string(observed) + " observed");
  expect(!payloadSlot.empty() &&
             text.find("mir_lifetime_slot<::__gti_program::item_value, true> " +
                       payloadSlot + ";") != std::string::npos &&
             text.find(payloadSlot + ".get().number") != std::string::npos,
         "value() should move the payload into its own field-read slot");
  expect(text.find("::gti_internal::backend::expected<::__gti_program::item_"
                   "value, std::int32_t> "
                   "__gti_mir_v_") == std::string::npos,
         "no observed Expected should value-initialize an ordinary local");
  expect(text.find("mir_expected_error_v1(__gti_mir_p_") != std::string::npos &&
             text.find("mir_lifetime_slot<::gti_internal::backend::expected<::_"
                       "_gti_program::"
                       "tracked, std::int32_t>, true>") == std::string::npos,
         "an owning temporary should be read in place from MIR's value-rooted "
         "slot");
}

// A field of an owning class temporary is a view of the result's own
// value-rooted lifetime slot, read in place before MIR's full-expression Drop
// destroys the temporary. An owning Expected temporary's payload moves into
// its own result slot and is read the same way. The view declares no local:
// a field class without a default constructor could not be declared.
// (Regression: these reported GTI-B0001.)
void testOwningTemporaryFieldView() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-owning-temporary-field-view.gti", R"(
struct inner_value {
  int32_t code;
  inner_value(int32_t code) : code(code) {}
};

class tracked {
public:
  inner_value inner;
  tracked(int32_t code) : inner(inner_value(code)) {}
  ~tracked() {}
};

tracked make_tracked(int32_t code) { return tracked(code); }

expected<tracked, int32_t> make_result(bool ok) {
  if (ok) { return tracked(7); }
  return unexpected(int32_t(3));
}

int32_t read() {
  return make_tracked(7).inner.code + make_result(true).value().inner.code;
}

int main() { return read() - 14; }
)");
  expect(frontend.canGenerateCode(),
         "the owning temporary field view fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  // Each view reads one declared owning slot that a normal-path Drop retires.
  const std::string read = ".get().inner.code;";
  std::size_t views = 0;
  std::string moved;
  for (std::size_t at = text.find(read); at != std::string::npos;
       at = text.find(read, at + read.size())) {
    const std::size_t slotStart = text.rfind("__gti_mir_p_", at);
    const std::string slot = text.substr(slotStart, at - slotStart);
    if (text.find("mir_lifetime_slot<::__gti_program::tracked> " + slot +
                  ";") == std::string::npos ||
        text.find(slot + ".destroy();\n") == std::string::npos)
      continue;
    ++views;
    if (text.find("mir_expected_value_move_v1(__gti_mir_p_") <
            text.find(slot + read) &&
        text.find(", &" + slot + ");") != std::string::npos)
      moved = slot;
  }
  expect(views == 2,
         "both field reads should view a declared owning slot that MIR's "
         "Drop retires: " +
             std::to_string(views) + " views");
  expect(!moved.empty(),
         "the owning Expected payload should move into its own result slot "
         "before its field is read");
  expect(text.find("::__gti_program::inner_value __gti_mir_p_") ==
             std::string::npos,
         "a field view should declare no local of its own");
}

// A construction whose constructor can fail, assigned to an existing
// variable, publishes into representation-only storage on its success edge;
// the Assign then moves it in and the slot retires. This holds when the
// construction consumes a staged Expected argument too. A failure-free
// construction keeps the fused assignment. (Regression: GTI-B0001.)
void testRaisingConstructionAssignmentSlot() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-raising-construction-assignment.gti", R"(
struct item_value {
  int32_t number;
  item_value(int32_t number) : number(number) {}
};

expected<item_value, int32_t> make_item(bool ok) {
  if (ok) { return item_value(7); }
  return unexpected(int32_t(3));
}

struct sum_holder {
  int32_t number;
  sum_holder(int32_t number) : number(number + 1) {}
};

struct item_holder {
  int32_t number;
  item_holder(expected<item_value, int32_t> item)
      : number(item ? item.value().number : 0) {}
};

struct plain_holder {
  int32_t number;
  plain_holder(int32_t number) : number(number) {}
};

int main() {
  mut sum_holder sum{1};
  sum = sum_holder(6);
  mut item_holder item{make_item(false)};
  item = item_holder(make_item(true));
  mut plain_holder plain{1};
  plain = plain_holder(7);
  return sum.number + item.number + plain.number - 21;
}
)");
  expect(frontend.canGenerateCode(),
         "the raising construction assignment fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  // The assignment slot is constructed by the failure-tag constructor, moved
  // into the binding, and retired on the next line.
  const auto assignedThroughSlot = [&](std::string_view holder) {
    const std::string declaration =
        "::gti_internal::backend::mir_lifetime_slot<::__gti_program::" +
        std::string(holder) + ", true> __gti_mir_v_";
    const std::size_t declared = text.find(declaration);
    if (declared == std::string::npos)
      return false;
    const std::size_t idStart = declared + declaration.size();
    const std::string slot =
        "__gti_mir_v_" +
        text.substr(idStart, text.find(';', idStart) - idStart);
    const std::string move = ", std::move(" + slot + ".get()));\n";
    const std::size_t moved = text.find(move);
    if (moved == std::string::npos)
      return false;
    const std::size_t next = text.find_first_not_of(' ', moved + move.size());
    return text.find(slot + ".construction_address())) ::__gti_program::" +
                     std::string(holder) +
                     "(::gti_internal::backend::mir_failure_constructor_tag_"
                     "v1<") != std::string::npos &&
           text.compare(next, slot.size() + 11, slot + ".destroy();") == 0;
  };
  expect(assignedThroughSlot("sum_holder"),
         "a raising construction should publish into its assignment slot "
         "before the binding moves it in");
  expect(assignedThroughSlot("item_holder") &&
             text.find("item_holder(::gti_internal::backend::mir_failure_"
                       "constructor_tag_v1<") != std::string::npos &&
             text.find("end moved-from argument representation") !=
                 std::string::npos,
         "a raising construction consuming a staged Expected argument should "
         "use the same assignment slot");
  expect(text.find("mir_lifetime_slot<::__gti_program::plain_holder, true>") ==
                 std::string::npos &&
             text.find("= ::__gti_program::plain_holder(") != std::string::npos,
         "a failure-free construction should keep the fused assignment");
}

// A cleanup-free Expected<Class, E> call result initializing a constructor
// field is staged and moved into the field's storage after its empty
// representation ends. Two such temporaries joined by a conditional publish
// into the join's own storage, which each publication first retires, and the
// grouped join reads that storage directly. Neither value-initializes the
// Expected. (Regression: both reported GTI-B0001.)
void testExpectedFieldAndJoinPublication() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-expected-field-and-join.gti", R"(
struct item_value {
  int32_t number;
  item_value(int32_t number) : number(number) {}
};

expected<item_value, int32_t> make_item(bool ok) {
  if (ok) { return item_value(7); }
  return unexpected(int32_t(3));
}

struct box {
  expected<item_value, int32_t> item;
  box(bool ok) : item(make_item(ok)) {}
};

bool observe(bool ok) {
  return (ok ? make_item(true) : make_item(false)).has_value();
}

int main() {
  box held{true};
  return held.item.value().number + (observe(false) ? 1 : 0) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the Expected field and join fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string rearm =
      ".destroy_if_engaged(); // retire the previous join representation\n";
  std::size_t publications = 0;
  std::string join;
  for (std::size_t at = text.find(rearm); at != std::string::npos;
       at = text.find(rearm, at + rearm.size())) {
    const std::size_t slotStart = text.rfind("__gti_mir_p_", at);
    const std::string slot = text.substr(slotStart, at - slotStart);
    const std::size_t next = text.find_first_not_of(' ', at + rearm.size());
    const std::string publish =
        slot + ".construct_from([&]() { return ::__gti_program::__gti_fn_";
    if (text.compare(next, publish.size(), publish) != 0)
      continue;
    ++publications;
    join = slot;
  }
  expect(publications == 2 && !join.empty() &&
             text.find("mir_lifetime_slot<::gti_internal::backend::expected<::_"
                       "_gti_program::item_"
                       "value, std::int32_t>, true> " +
                       join + ";") != std::string::npos &&
             text.find(join + ".get().has_value();") != std::string::npos,
         "both join arms should publish into the join's storage after "
         "retiring it, and the grouped join should read that storage");
  const std::string field =
      "::gti_internal::backend::construct_representation_at(std::addressof("
      "__gti_mir_p_";
  const std::size_t constructed = text.find(field);
  const std::size_t moved =
      constructed == std::string::npos
          ? std::string::npos
          : text.find(", std::move(__gti_mir_v_", constructed);
  const std::size_t slotStart =
      moved == std::string::npos ? std::string::npos : moved + 12;
  const std::string slot =
      slotStart == std::string::npos
          ? std::string()
          : text.substr(slotStart, text.find(".get()", slotStart) - slotStart);
  expect(!slot.empty() &&
             text.find(slot + ".construct_from([&]() { return ::__gti_program::"
                              "__gti_fn_") != std::string::npos &&
             text.find(slot + ".destroy();", moved) != std::string::npos,
         "the field initializer should move a staged result into the field");
  expect(text.find("::gti_internal::backend::expected<::__gti_program::item_"
                   "value, std::int32_t> "
                   "__gti_mir_v_") == std::string::npos,
         "no Expected should value-initialize an ordinary local");
}

void testOwnedArgumentMemberInitializer() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-owned-argument-member-initializer.gti", R"(
class counted {
public:
  int32_t number;
  counted(int32_t number) : number(number) {}
  ~counted() {}
};

expected<counted, int32_t> make_counted(bool ok) {
  if (ok) { return counted(5); }
  return unexpected(int32_t(2));
}

struct holder {
  int32_t number;
  holder(expected<counted, int32_t> item)
      : number(item ? item.value().number : 100 + item.error()) {}
};

struct owner {
  holder held;
  owner(bool ok) : held(holder(make_counted(ok))) {}
};

int main() {
  owner success{true};
  return success.held.number - 5;
}
)");
  expect(frontend.canGenerateCode(),
         "the owned-argument member initializer fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  // The member's constructor takes the owning result's own value obligation
  // directly; that handoff is not a transfer into `this`.
  bool routed = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const lang::MirBody &body = constructor.body;
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct)
          continue;
        for (const lang::MirLifecycleEvent &event : instruction.lifecycle) {
          const lang::MirDropObligation *source =
              body.findDropObligation(event.source);
          routed = routed ||
                   (event.kind == lang::MirLifecycleEventKind::TransferOut &&
                    source != nullptr &&
                    source->kind == lang::MirDropObligationKind::Value &&
                    lang::mirBodyRoutesFailureEdges(body) &&
                    !body.failureRecords.empty());
        }
      }
    }
  }
  expect(routed, "a constructor handing an owned result to a member's "
                 "constructor should keep routing its failure edges");
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::size_t constructed =
      text.find(") ::__gti_program::holder(::gti_internal::backend::mir_"
                "failure_constructor_tag_v1<");
  const std::size_t lineEnd = constructed == std::string::npos
                                  ? std::string::npos
                                  : text.find('\n', constructed);
  const std::size_t slotStart =
      constructed == std::string::npos
          ? std::string::npos
          : text.find("{}, __gti_mir_p_", constructed);
  const std::size_t slotEnd = slotStart >= lineEnd
                                  ? std::string::npos
                                  : text.find(".take(), &__gti_mir_construct_"
                                              "success_",
                                              slotStart);
  const std::string slot =
      slotEnd >= lineEnd ? std::string()
                         : text.substr(slotStart + 4, slotEnd - slotStart - 4);
  const std::size_t declared =
      slot.empty()
          ? std::string::npos
          : text.rfind(
                "::gti_internal::backend::mir_lifetime_slot<"
                "::gti_internal::backend::expected<::__gti_program::counted, "
                "std::int32_t>> " +
                    slot + ";",
                constructed);
  expect(declared != std::string::npos &&
             text.find(slot + ".construct_from([&]() { return ::__gti_"
                              "program::__gti_fn_",
                       declared) < constructed,
         "the member should be constructed in place from the owned result's "
         "slot, taken once after its producer published it");
  expect(text.find("::gti_internal::backend::expected<::__gti_program::counted,"
                   " std::int32_t> "
                   "__gti_mir_v_") == std::string::npos,
         "no Expected should value-initialize an ordinary local");
}

void testOwningExpectedFieldPublication() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-owning-expected-field.gti", R"(
class counted {
public:
  int32_t number;
  counted(int32_t number) : number(number) {}
  ~counted() {}
};

expected<counted, int32_t> make_counted(bool ok) {
  if (ok) { return counted(5); }
  return unexpected(int32_t(2));
}

expected<counted, int32_t> make_checked(int32_t value) {
  return counted(value + 1);
}

struct box {
  expected<counted, int32_t> plain;
  expected<counted, int32_t> checked;
  box(bool ok) : plain(make_counted(ok)), checked(make_checked(6)) {}
};

int main() {
  box held{true};
  return held.plain.value().number + held.checked.value().number - 12;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning Expected field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const std::string retire =
      ".destroy(); // end transferred Expected field representation\n";
  std::size_t publications = 0;
  bool checkedProducer = false;
  for (std::size_t at = text.find(retire); at != std::string::npos;
       at = text.find(retire, at + retire.size())) {
    const std::size_t slotStart = text.rfind("__gti_mir_p_", at);
    const std::string slot = text.substr(slotStart, at - slotStart);
    const std::size_t declared =
        text.rfind("::gti_internal::backend::mir_lifetime_slot<::gti_internal::"
                   "backend::expected<"
                   "::__gti_program::counted, std::int32_t>> " +
                       slot + ";",
                   at);
    const std::size_t moved =
        text.rfind("::gti_internal::backend::construct_representation_at(std::"
                   "addressof(",
                   at);
    if (declared == std::string::npos || moved == std::string::npos ||
        text.find(", std::move(" + slot + ".get()));", moved) > at ||
        text.rfind("std::destroy_at(std::addressof(", moved) < declared)
      continue;
    ++publications;
    checkedProducer = checkedProducer ||
                      text.find(slot + ".mark_constructed();", declared) < at;
  }
  expect(publications >= 2 && checkedProducer,
         "each owning Expected field should be constructed in place from its "
         "result's lifetime slot, including after a checked producer, and "
         "the slot should retire without a semantic drop");
  expect(text.find("::gti_internal::backend::expected<::__gti_program::counted,"
                   " std::int32_t> "
                   "__gti_mir_v_") == std::string::npos,
         "no Expected should value-initialize an ordinary local");
}

void testCommaDenotesRightOperandObject() {
  const lang::FrontendResult frontend = analyze("cpp-mir-comma-object.gti", R"(
class counted {
public:
  int32_t number;
  counted(int32_t number) : number(number) {}
  ~counted() {}
};

struct plain_value {
  int32_t number;
  plain_value(int32_t number) : number(number) {}
};

int32_t tick(int32_t value) { return value; }
counted make_counted(int32_t number) { return counted(number); }
plain_value make_plain(int32_t number) { return plain_value(number); }

int32_t use_owned() {
  counted value = (tick(1), make_counted(2));
  return value.number;
}

int32_t use_plain() {
  plain_value value = (make_plain(1), make_plain(3));
  return value.number;
}

int main() { return use_owned() + use_plain() - 5; }
)");
  expect(frontend.canGenerateCode(),
         "the class-valued comma fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  // No comma computes a class value. The right operand's call result
  // initializes the binding itself, reparenting its own obligation; the left
  // operand's class result is discarded.
  std::size_t classCommas = 0;
  std::size_t directPublications = 0;
  std::size_t discarded = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const lang::MirBody &body = function.body;
    for (const lang::MirBlock &block : body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const lang::MirInstruction &instruction = block.instructions[index];
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::Comma &&
            instruction.info.type.kind == lang::SemanticType::Class)
          ++classCommas;
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.result ||
            instruction.info.type.kind != lang::SemanticType::Class)
          continue;
        const auto uses = body.usesOf(*instruction.result);
        const auto executable = std::count_if(
            uses.begin(), uses.end(), [](const lang::MirValueUse &use) {
              return use.kind != lang::MirValueUseKind::PlaceRoot;
            });
        if (executable == 0) {
          ++discarded;
          continue;
        }
        const lang::MirInstruction *next = index + 1 < block.instructions.size()
                                               ? &block.instructions[index + 1]
                                               : nullptr;
        if (executable == 1 && next != nullptr &&
            next->kind == lang::MirInstructionKind::Initialize &&
            next->operands.size() == 1 &&
            next->operands.front().value == *instruction.result) {
          const lang::MirPlace *destination =
              next->destination ? body.findPlace(*next->destination) : nullptr;
          if (destination != nullptr &&
              destination->root == lang::MirPlaceRootKind::Binding)
            ++directPublications;
        }
      }
    }
  }
  expect(classCommas == 0 && directPublications == 2 && discarded >= 1,
         "a class-valued comma should denote its right operand's result, "
         "which initializes the binding directly, and discard its left "
         "operand's result");
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  expect(text.find(".destroy(); // end discarded result representation") !=
                 std::string::npos &&
             text.find("::__gti_program::plain_value __gti_mir_v_") ==
                 std::string::npos &&
             text.find("::__gti_program::counted __gti_mir_v_") ==
                 std::string::npos,
         "the discarded left result should retire its representation, and "
         "no class result should value-initialize an ordinary local");
}

void testPlainCallMemberInitializer() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-plain-call-member-initializer.gti", R"(
struct plain_value {
  int32_t number;
  plain_value(int32_t number) : number(number) {}
};

class counted {
public:
  int32_t number;
  counted(int32_t number) : number(number) {}
  ~counted() {}
};

plain_value make_plain(int32_t number) { return plain_value(number); }
counted make_counted(int32_t number) { return counted(number); }

struct owner {
  plain_value plain;
  counted owned;
  owner(int32_t number)
      : plain(make_plain(number)), owned(make_counted(number)) {}
};

int main() {
  owner both{2};
  return both.plain.number + both.owned.number - 4;
}
)");
  expect(frontend.canGenerateCode(),
         "the plain-call member initializer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto slotNamed = [&](const std::string &declaration) {
    const std::size_t at = text.find(declaration);
    if (at == std::string::npos)
      return std::string();
    const std::size_t start = at + declaration.size();
    return text.substr(start, text.find(';', start) - start);
  };
  // The cleanup-free result constructs in representation-only storage, then
  // replaces the field's empty representation.
  const std::string plain = slotNamed(
      "::gti_internal::backend::mir_lifetime_slot<::__gti_program::plain_"
      "value, true> ");
  const std::size_t plainCall =
      plain.empty() ? std::string::npos
                    : text.find(plain + ".construct_from([&]() { return "
                                        "::__gti_program::__gti_fn_");
  expect(plainCall != std::string::npos &&
             text.find("construct_representation_at(std::addressof(__gti_mir_"
                       "p_",
                       plainCall) <
                 text.find(", std::move(" + plain + ".get()));", plainCall),
         "a failure-free call's cleanup-free result should construct the "
         "field in place from its representation-only slot");
  // The owning result constructs in its lifetime slot and moves into the
  // field, whose rollback MIR then owns.
  const std::string owned = slotNamed(
      "::gti_internal::backend::mir_lifetime_slot<::__gti_program::counted> ");
  const std::size_t ownedCall =
      owned.empty() ? std::string::npos
                    : text.find(owned + ".construct_from([&]() { return "
                                        "::__gti_program::__gti_fn_");
  expect(ownedCall != std::string::npos &&
             text.find(" = std::move(" + owned + ".get());", ownedCall) !=
                 std::string::npos,
         "a failure-free call's owning result should move from its slot into "
         "the field");
  expect(text.find("::__gti_program::plain_value __gti_mir_v_") ==
                 std::string::npos &&
             text.find("::__gti_program::counted __gti_mir_v_") ==
                 std::string::npos,
         "no class result should value-initialize an ordinary local");
}

void testClassConditionalJoinConsumers() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-class-conditional-join.gti", R"(
struct plain_value {
  int32_t number;
  plain_value(int32_t number) : number(number) {}
};

class counted {
public:
  int32_t number;
  counted(int32_t number) : number(number) {}
  ~counted() {}
};

plain_value make_plain(int32_t number) { return plain_value(number); }
counted make_counted(int32_t number) { return counted(number); }

plain_value pick(bool ok) { return ok ? make_plain(1) : make_plain(2); }

counted pick_counted(bool ok) {
  return ok ? make_counted(3) : make_counted(4);
}

int main() { return pick(true).number + pick_counted(true).number - 4; }
)");
  expect(frontend.canGenerateCode(),
         "the class conditional join fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  // Both call arms publish into the cleanup-free join's storage; the joined
  // value moves into its own staged slot and the join's representation ends.
  const std::string retire = ".destroy(); // end moved join representation\n";
  const std::size_t retired = text.find(retire);
  const std::size_t joinStart = retired == std::string::npos
                                    ? std::string::npos
                                    : text.rfind("__gti_mir_p_", retired);
  const std::string join = joinStart == std::string::npos
                               ? std::string()
                               : text.substr(joinStart, retired - joinStart);
  std::size_t armPublications = 0;
  for (std::size_t at = join.empty() ? std::string::npos
                                     : text.find(join + ".construct_from(");
       at != std::string::npos && at < retired;
       at = text.find(join + ".construct_from(", at + 1))
    ++armPublications;
  expect(armPublications == 2 &&
             text.rfind(".construct(std::move(" + join + ".get()));",
                        retired) != std::string::npos,
         "both call arms should publish into the join's storage, and the "
         "returned value should move from it before the join retires");
  // An owning returned join ends its moved-from storage at the transfer.
  expect(text.find(".destroy(); // GTI MIR returned temporary transfer-out") !=
             std::string::npos,
         "an owning conditional returned from a plain body should retire its "
         "moved-from join storage at the transfer");
  expect(text.find("::__gti_program::plain_value __gti_mir_v_") ==
                 std::string::npos &&
             text.find("::__gti_program::plain_value __gti_mir_p_") ==
                 std::string::npos,
         "no join value or storage should value-initialize an ordinary local");
}

void testShortCircuitOwnedFieldView() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-short-circuit-field-view.gti", R"(
class counted {
public:
  int32_t number;
  counted(int32_t number) : number(number) {}
  ~counted() {}
};

counted make_counted(int32_t number) { return counted(number); }

bool check(bool skip) { return skip || make_counted(5).number == 5; }

int main() { return check(false) ? 0 : 1; }
)");
  expect(frontend.canGenerateCode(),
         "the short-circuit field view fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  // The call constructs its owning result in the conditional temporary, and
  // the field is read from that temporary in place.
  const std::string declaration =
      "::gti_internal::backend::mir_lifetime_slot<::__gti_program::counted> "
      "__gti_mir_p_";
  const std::size_t declared = text.find(declaration);
  const std::size_t nameStart = declared == std::string::npos
                                    ? std::string::npos
                                    : declared + declaration.size() - 12;
  const std::string temporary =
      nameStart == std::string::npos
          ? std::string()
          : text.substr(nameStart, text.find(';', nameStart) - nameStart);
  const std::size_t constructed =
      temporary.empty()
          ? std::string::npos
          : text.find(temporary + ".construct_from([&]() { return "
                                  "::__gti_program::__gti_fn_");
  expect(constructed != std::string::npos &&
             text.find(" = " + temporary + ".get().number;", constructed) !=
                 std::string::npos,
         "the owning result should construct in its conditional temporary "
         "and its field should be read from that temporary");
  expect(text.find("::__gti_program::counted __gti_mir_v_") ==
             std::string::npos,
         "the viewed result should not declare an ordinary local");
}

void testOwningConditionalJoinConsumers() {
  const auto frontend =
      analyzeWithStandardLibrary("cpp-mir-owning-conditional-join.gti", R"gti(
#include <std/utility>

mut int32_t destroyed = 0;

class counted {
public:
  int32_t number;
  counted(int32_t number) : number(number) {}
  ~counted() { destroyed += 1; }
};

counted make_counted(int32_t number) { return counted(number); }

int32_t read(bool ok) { return (ok ? make_counted(1) : make_counted(2)).number; }

int32_t assign(bool ok) {
  mut counted target = make_counted(0);
  target = ok ? make_counted(3) : make_counted(4);
  return target.number;
}

counted pick(bool ok) {
  counted local = make_counted(5);
  return ok ? std::move(local) : make_counted(6);
}

int main() { return read(true) + assign(true) + pick(true).number - 9; }
)gti");
  expect(frontend.canGenerateCode(),
         "the owning conditional join fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  // A value moved out of the owning join is its temporary: a field is read
  // there in place, and an assignment move-assigns from it.
  const std::string alias = " denotes its owning conditional temporary\n";
  bool readInPlace = false;
  for (std::size_t at = text.find(alias); at != std::string::npos;
       at = text.find(alias, at + 1)) {
    const std::size_t line = at + alias.size();
    const std::size_t end = text.find('\n', line);
    readInPlace |= end != std::string::npos &&
                   text.substr(line, end - line).find(".get().number;") !=
                       std::string::npos;
  }
  expect(readInPlace,
         "a field of an owning conditional should be read from its temporary");
  expect(text.find(".get() = std::move(__gti_mir_p_") != std::string::npos,
         "assigning an owning conditional should move-assign from its "
         "temporary");
  // A returned join keeps the result in its temporary until the moved
  // local's fallible cleanup has run, then publishes it.
  expect(text.find("construct_representation_at(__gti_mir_out_result, "
                   "__gti_mir_p_") != std::string::npos &&
             text.find(".take());\n      return true;") != std::string::npos,
         "a returned owning conditional should publish from its temporary at "
         "Return");
}

void testBranchTemporaryCallResultIsNotDiscarded() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-branch-temporary-result.gti", R"(
class reading {
public:
  reading(uint32_t value) : value(value) {}
  bool even() { return (this.value + uint32_t(1)) % uint32_t(2) == uint32_t(1); }
private:
  uint32_t value;
};

reading make_reading(uint32_t value) { return reading(value); }

int main() {
  mut uint32_t one = uint32_t(1);
  one = one + uint32_t(0);
  if (one != uint32_t(1) || !make_reading(uint32_t(4)).even()) { return 1; }
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the branch-temporary receiver fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  // The receiver temporary exists only on the right of `||`, so MIR rehomes
  // its owning value into a hidden temporary place and the plain call
  // publishes there. The call's value has no uses, yet it is not discarded.
  std::size_t rehomed = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const lang::MirBody &body = function.body;
    for (const lang::MirBlock &block : body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.result || !instruction.destination ||
            instruction.info.type.kind != lang::SemanticType::Class)
          continue;
        const lang::MirPlace *destination =
            body.findPlace(*instruction.destination);
        if (destination != nullptr &&
            destination->root == lang::MirPlaceRootKind::Temporary &&
            body.usesOf(*instruction.result).empty())
          ++rehomed;
      }
  }
  expect(rehomed == 1,
         "the branch-only receiver result should publish into its hidden "
         "temporary place with no remaining value uses");
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  expect(text.find("end discarded result representation") ==
                 std::string::npos &&
             text.find("mir_lifetime_slot<::__gti_program::reading, true> "
                       "__gti_mir_v_") == std::string::npos &&
             text.find(".construct_from([&]() { return "
                       "::__gti_program::") != std::string::npos,
         "a call publishing into its MIR destination should construct there "
         "and never retire an unconstructed discard slot");
}

void testCopyableLambdaReturnRetainsSsa() {
  const auto frontend =
      analyzeWithStandardLibrary("copyable-lambda-return.gti", R"gti(
#include <std/utility>
T relay<T>(T value) { return value; }
int main() {
  int offset = 1;
  auto operation = [offset](int value) -> int { return value + offset; };
  auto result = relay(std::move(operation));
  return result(41) - 42;
}
)gti");
  expect(frontend.canGenerateCode(), "copyable lambda return remains a copy");
  if (!frontend.canGenerateCode())
    return;
  const auto optimization = lang::OptimizationPipeline().run(
      frontend.hir, lang::OptimizationLevel::O0);
  const auto artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimization);
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.returnType.kind != lang::SemanticType::Lambda)
      continue;
    // The relay has no failure source of its own, so MIR proves it
    // failure-free and it emits only its plain form.
    const auto marker = std::string(function.mayRaiseDefinedFailure
                                        ? "deduced-callable-failure-v1"
                                        : "deduced-callable-v1") +
                        " function-instance " + std::to_string(function.id);
    expect(artifact.contents.find(marker) != std::string::npos,
           "verified copyable lambda relay has complete production text");
    expect(artifact.contents.find("auto __gti_mir_v_1 = __gti_mir_p_1;") !=
               std::string::npos,
           "copy Load materializes the SSA value consumed by return");
    checked = true;
  }
  expect(checked, "the relay retains its concrete lambda result identity");
  auto malformed = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      malformed.functionInstances());
  for (auto &function : functions) {
    if (function.returnType.kind != lang::SemanticType::Lambda)
      continue;
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Load)
          instruction.operands.clear();
    (void)lang::rebuildMirValueUses(function.body);
  }
  expect(!lang::verifyMirProgram(malformed).valid(),
         "lambda result publication still rejects a missing source operand");
}

// Free-function instances with different cleanup shapes retain separate MIR
// bodies. C++ destructibility must not select another instance's GTI execution.
// (Regression: the combination ICEd with "lowered callable instances do not
// share one source template representation" while each family alone passed.)
void testMixedCaptureDeducedCallableTemplateEmission() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-mixed-capture-template.gti", R"(
#include <std/memory>

mut int cleanup_count = 0;

class payload {
public:
  payload() {}
  payload(payload& other) = delete;
  payload(payload&& other) = default;
  ~payload() { cleanup_count++; }

  int tag() { return 1; }
};

void apply<Operation>(Operation operation) {
  operation(1);
}

int main() {
  int offset = 1;
  auto scalar = [offset](int value) -> void {
    if (value == offset) {
      return;
    }
  };
  payload owned = payload();
  auto classy = [kept = std::move(owned)](int value) -> void {
    if (value == kept.tag()) {
      return;
    }
  };
  apply(std::move(scalar));
  apply(std::move(classy));
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the mixed-capture template fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto count = [&](std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  expect(count("(Operation __gti_mir_arg_0, ::gti_failure_record_v2 "
               "*__gti_mir_failure_record) {") == 2,
         "each cleanup-shape instance must retain its own definition");
  expect(count("deduced-callable-failure-v1 function-instance") == 2,
         "each concrete cleanup body must retain its exact MIR identity");
  expectDistinctCallableInstances(frontend, text);
  expect(count("if constexpr (std::is_trivially_destructible_v<Operation>)") ==
             0,
         "native destructibility must not select free-function MIR bodies");
  expect(text.find("::gti_internal::backend::mir_lifetime_slot<Operation>") !=
             std::string::npos,
         "the non-trivial cleanup variant should keep its lifetime-slot "
         "parameter storage");
}

constexpr NamedTest kTests[] = {
    {"testFieldInitializerExpressionVocabulary",
     testFieldInitializerExpressionVocabulary},
    {"testInlineClosureChainEmission", testInlineClosureChainEmission},
    {"testClosureCaptureFreezeMaterializes",
     testClosureCaptureFreezeMaterializes},
    {"testLexicalCopyCaptureThroughCallableTemplate",
     testLexicalCopyCaptureThroughCallableTemplate},
    {"testProducedClosureLocalSlots", testProducedClosureLocalSlots},
    {"testCopiedFormalReusedInConstructorBody",
     testCopiedFormalReusedInConstructorBody},
    {"testMovedClosureFieldInFallibleConstructor",
     testMovedClosureFieldInFallibleConstructor},
    {"testInlineCapturingLiteralAsAlgorithmArgument",
     testInlineCapturingLiteralAsAlgorithmArgument},
    {"testInlineCaptureControlFlowProofFailsClosed",
     testInlineCaptureControlFlowProofFailsClosed},
    {"testGenericMemberFailureOverloadOnOrdinaryOwner",
     testGenericMemberFailureOverloadOnOrdinaryOwner},
    {"testCallableTemplateBodyVocabulary", testCallableTemplateBodyVocabulary},
    {"testMovedCallableReceiverMutableFallback",
     testMovedCallableReceiverMutableFallback},
    {"testDeducedCallableTemplateEmission",
     testDeducedCallableTemplateEmission},
    {"testMixedCaptureDeducedCallableTemplateEmission",
     testMixedCaptureDeducedCallableTemplateEmission},
    {"testCopyableLambdaReturnRetainsSsa", testCopyableLambdaReturnRetainsSsa},
    {"testForwardedConcretePackLayout", testForwardedConcretePackLayout},
    {"testRetainedReferenceAddressAndConcreteDefaultReturn",
     testRetainedReferenceAddressAndConcreteDefaultReturn},
    {"testStoredBorrowReferenceAliasAddresses",
     testStoredBorrowReferenceAliasAddresses},
    {"testExpectedValueOrBorrowedReceiver",
     testExpectedValueOrBorrowedReceiver},
    {"testContextualSignedMinimumInFailureCaller",
     testContextualSignedMinimumInFailureCaller},
    {"testPlainCopyReturnUsesExactSourcePlace",
     testPlainCopyReturnUsesExactSourcePlace},
    {"testReferenceParameterBorrowUsesReferentCarrier",
     testReferenceParameterBorrowUsesReferentCarrier},
    {"testAdjacentClassCopyAssignmentFusion",
     testAdjacentClassCopyAssignmentFusion},
    {"testNestedReferenceReturnForwardsCallResultPointer",
     testNestedReferenceReturnForwardsCallResultPointer},
    {"testNativeCallbackFactoryBody", testNativeCallbackFactoryBody},
    {"testOpaquePointerFailureResults", testOpaquePointerFailureResults},
    {"testInvokePublishesScalarEdgeArguments",
     testInvokePublishesScalarEdgeArguments},
    {"testCleanupFreeExpectedClassArgumentSlot",
     testCleanupFreeExpectedClassArgumentSlot},
    {"testCleanupFreeExpectedClassConstructorArgumentSlot",
     testCleanupFreeExpectedClassConstructorArgumentSlot},
    {"testExpectedClassArgumentReturnedConstruction",
     testExpectedClassArgumentReturnedConstruction},
    {"testExpectedConsumingMemberReturn", testExpectedConsumingMemberReturn},
    {"testPreparedConsumingReceiverReturn",
     testPreparedConsumingReceiverReturn},
    {"testPreparedConsumingReceiverLaterFailure",
     testPreparedConsumingReceiverLaterFailure},
    {"testConditionalPreparedInputResult", testConditionalPreparedInputResult},
    {"testMovedGenericOwnerReceiverStage", testMovedGenericOwnerReceiverStage},
    {"testExpectedTemporaryObservationSlot",
     testExpectedTemporaryObservationSlot},
    {"testOwningTemporaryFieldView", testOwningTemporaryFieldView},
    {"testRaisingConstructionAssignmentSlot",
     testRaisingConstructionAssignmentSlot},
    {"testExpectedFieldAndJoinPublication",
     testExpectedFieldAndJoinPublication},
    {"testOwnedArgumentMemberInitializer", testOwnedArgumentMemberInitializer},
    {"testOwningExpectedFieldPublication", testOwningExpectedFieldPublication},
    {"testCommaDenotesRightOperandObject", testCommaDenotesRightOperandObject},
    {"testBranchTemporaryCallResultIsNotDiscarded",
     testBranchTemporaryCallResultIsNotDiscarded},
    {"testPlainCallMemberInitializer", testPlainCallMemberInitializer},
    {"testClassConditionalJoinConsumers", testClassConditionalJoinConsumers},
    {"testShortCircuitOwnedFieldView", testShortCircuitOwnedFieldView},
    {"testOwningConditionalJoinConsumers", testOwningConditionalJoinConsumers},
    {"testCleanupFreeExpectedClassCopiedArgumentSlot",
     testCleanupFreeExpectedClassCopiedArgumentSlot},
    {"testCleanupFreeExpectedClassAssignmentSlot",
     testCleanupFreeExpectedClassAssignmentSlot},
    {"testCleanupFreeExpectedClassMoveSlot",
     testCleanupFreeExpectedClassMoveSlot},
    {"testCleanupFreeExpectedClassCopySlot",
     testCleanupFreeExpectedClassCopySlot},
    {"testExpectedClassReplacementSources",
     testExpectedClassReplacementSources},
    {"testOwningExpectedReplacementSources",
     testOwningExpectedReplacementSources},
    {"testShortCircuitExpectedPayloadFieldView",
     testShortCircuitExpectedPayloadFieldView},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "text");
}
