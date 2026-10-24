#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

// expected<T, E> is the prelude's ordinary source class.
bool isExpectedClass(const lang::FrontendResult &frontend,
                     const lang::SemanticType &type) {
  const auto *info = type.kind == lang::SemanticType::Class
                         ? frontend.semantics.findClassType(type.classId)
                         : nullptr;
  return info != nullptr && info->declaration != nullptr &&
         info->declaration->name().lexeme == "expected";
}

const lang::MirFunctionInstance *
namedFunction(const lang::FrontendResult &frontend, std::string_view name) {
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const auto *source = frontend.hir.findFunctionInstance(function.id);
    if (source != nullptr && source->source != nullptr &&
        source->sourceUnit == frontend.sourceGraph.entryUnit() &&
        source->source->name().lexeme == name)
      return &function;
  }
  return nullptr;
}

// The selected native form of one function body.
std::string selectedBodyText(const lang::FrontendResult &frontend,
                             const lang::MirFunctionInstance &function) {
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{lang::MirBodyKind::Function, function.id};
  return function.mayRaiseDefinedFailure
             ? emitter.emitFailureBodyText(address, "selected", 1).text
             : emitter.emitBodyText(address, "selected", 1).text;
}

// Each prepared expected argument of a body is published into its slot,
// moved into its consumer, and retired after that consumer.
std::size_t stagedExpectedArguments(const lang::FrontendResult &frontend,
                                    const lang::MirBody &body,
                                    const std::string &text,
                                    std::string_view message) {
  std::size_t staged = 0;
  for (const lang::MirBlock &block : body.blocks)
    for (const lang::MirInstruction &input : block.instructions) {
      if (input.kind != lang::MirInstructionKind::CallInput ||
          !input.destination || !input.preparedParameterDrop ||
          !isExpectedClass(frontend, input.info.type))
        continue;
      ++staged;
      const std::string slot =
          "__gti_mir_p_" + std::to_string(*input.destination);
      const auto published = text.find(slot + ".construction_address()");
      const auto moved =
          published == std::string::npos
              ? std::string::npos
              : text.find("std::move(" + slot + ".get())", published);
      expect(moved != std::string::npos &&
                 text.find(slot + ".destroy();", moved) != std::string::npos,
             message);
    }
  return staged;
}

// unexpected() staged its result in `slot`. A failure-capable unexpected()
// writes through the slot's construction address; one whose error moves
// failure-free (a scalar error) returns the value the slot is built from.
bool stagedByUnexpected(const std::string &text, const std::string &slot) {
  const std::size_t produced = text.find(slot + ".construction_address()");
  const std::size_t producer =
      produced == std::string::npos ? produced : text.rfind('\n', produced);
  if (producer != std::string::npos &&
      text.substr(producer, produced - producer)
              .find("_unexpected__gti_mir_failure(") != std::string::npos)
    return true;
  const std::size_t built = text.find(slot + ".construct_from([&]() { return ");
  return built != std::string::npos &&
         text.substr(built, text.find('\n', built) - built)
                 .find("_unexpected(") != std::string::npos;
}

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
        // A passive payload default is published by the field Initialize that
        // immediately consumes its selected initializer result. Pin the typed
        // root, that exact Initialize and the adjacent full-expression
        // endpoint that authorize the publication.
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
          const auto *publication = position + 2 < block.instructions.size()
                                        ? &block.instructions[position + 1]
                                        : nullptr;
          const auto *field =
              publication != nullptr && publication->destination
                  ? constructor.body.findPlace(*publication->destination)
                  : nullptr;
          const bool exactPublication =
              value != nullptr && value->definition == instruction.id &&
              value->info == instruction.info &&
              full != constructor.body.fullExpressions.end() &&
              publication != nullptr &&
              publication->kind == lang::MirInstructionKind::Initialize &&
              publication->operands.size() == 1 &&
              publication->operands.front().value == *instruction.result &&
              field != nullptr && field->projections.size() == 1 &&
              field->projections.front().field == initializer.field &&
              block.instructions[position + 2].kind ==
                  lang::MirInstructionKind::Lifecycle &&
              block.instructions[position + 2].fullExpressionEnd == full->id;
          expect(exactPublication,
                 "passive payload publication retains its selected typed "
                 "initializer and full-expression endpoint");
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
  expect(
      artifact.contents.find("std::int32_t converted;") != std::string::npos &&
          artifact.contents.find("payload_t empty;") != std::string::npos &&
          artifact.contents.find("(*this).converted") != std::string::npos &&
          artifact.contents.find("mir_checked_convert_v1<std::int8_t>") !=
              std::string::npos &&
          // A trivially move-assignable passive payload field is published
          // by assignment into its already-constructed storage.
          artifact.contents.find(
              "(*this).empty = ::__gti_program::payload_t{"
              "std::in_place_index<2>}; // direct passive payload field "
              "publication") != std::string::npos &&
          artifact.contents.find("(*this).valued = ::__gti_program::payload_t{"
                                 "std::in_place_index<1>, __gti_mir_v_") !=
              std::string::npos,
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

void testMovedPassiveFieldAfterFallibleSibling() {
  // A scalar and a plain enum moved after a fallible field and a fallible
  // computed sibling have no drop record at all. They cannot be hoisted into
  // the member-initializer list, so the failure form keeps each exact move in
  // its verified body and starts the field from its empty state.
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-moved-passive-field.gti", R"(
#include <std/utility>

class Ledger {
public:
  int32_t id;
  Ledger(int32_t id, int32_t divisor) : id(id + 60 / divisor) {}
  ~Ledger() {}
};

enum class Shade { light, dark };

class Gauge {
public:
  Ledger ledger;
  bool odd;
  int32_t tag;
  Shade shade;
  Gauge(int32_t tag, Shade shade, int32_t divisor)
      : ledger(Ledger(tag, divisor)), odd(tag % 2 == 1), tag(std::move(tag)),
        shade(std::move(shade)) {}
};

int main() {
  Gauge gauge = Gauge(7, Shade::dark, 1);
  return gauge.shade == Shade::dark && gauge.odd
             ? gauge.tag + gauge.ledger.id - 74
             : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the moved-passive-field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto ownedInitializers =
      [](const lang::MirConstructorInstance &constructor) {
        return std::count_if(
            constructor.initializers.begin(), constructor.initializers.end(),
            [](const lang::MirConstructorInitializer &initializer) {
              return initializer.ownedParameter.has_value();
            });
      };
  const lang::MirConstructorInstance *gauge = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (ownedInitializers(constructor) == 2) {
      gauge = &constructor;
    }
  }
  expect(gauge != nullptr && gauge->mayRaiseDefinedFailure &&
             !gauge->body.failureRecords.empty() &&
             !lang::cppMirOwnedParameterFieldBindings(frontend.mir, *gauge),
         "moves recorded after fallible work are not native publications");
  if (gauge == nullptr) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = gauge->id};
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
             .supportsFailureBodyText(address),
         "the failure form keeps drop-free moves in its verified body");

  std::string text;
  try {
    text = gti_test::emitCpp(frontend).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("drop-free moves after a fallible sibling "
                              "should emit: ") +
                      error.what());
    return;
  }
  const std::size_t start = text.find(
      "Gauge::Gauge(::gti_internal::backend::mir_failure_constructor_tag_v1<");
  const std::size_t end =
      start == std::string::npos ? start : text.find("\n  }\n", start);
  const std::string body = end == std::string::npos
                               ? std::string()
                               : text.substr(start, end - start);
  const auto count = [&](std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = body.find(needle); at != std::string::npos;
         at = body.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  // Each moved field begins empty and MIR moves its formal exactly once,
  // after the ledger and the checked remainder; a failure in between rolls
  // back only the ledger.
  const std::size_t remainder = body.find("mir_checked_remainder_v1");
  const std::size_t firstMove = body.find(" = std::move(__gti_mir_p_");
  expect(count("tag(::gti_internal::backend::mir_empty_state_v1<std::remove_"
               "cv_t<decltype(tag)>>())") == 1 &&
             count("shade(::gti_internal::backend::mir_empty_state_v1<std::"
                   "remove_cv_t<decltype(shade)>>())") == 1 &&
             count("std::move(__gti_mir_arg_") == 0 &&
             count(" = std::move(__gti_mir_p_") == 2 &&
             count("mir_rollback_field_v1(") == 1 &&
             remainder != std::string::npos && firstMove != std::string::npos &&
             body.find("::__gti_program::Ledger(") < remainder &&
             remainder < firstMove,
         "the failure-form constructor publishes each moved field in MIR "
         "order from its empty state");

  // Borrowed state is outside the drop-free route.
  {
    lang::MirProgram mutated = frontend.mir;
    bool forged = false;
    for (const auto &constructor : mutated.constructorInstances()) {
      if (constructor.id != gauge->id) {
        continue;
      }
      auto &changed = const_cast<lang::MirConstructorInstance &>(constructor);
      for (lang::MirValue &value : changed.body.values) {
        const lang::MirInstruction *definition = nullptr;
        for (const lang::MirBlock &block : changed.body.blocks) {
          for (const lang::MirInstruction &instruction : block.instructions) {
            if (instruction.id == value.definition) {
              definition = &instruction;
            }
          }
        }
        if (!forged && definition != nullptr &&
            definition->kind == lang::MirInstructionKind::Move &&
            value.info.type == lang::SemanticType::Int32) {
          value.info.traits.containsBorrowedState = true;
          forged = true;
        }
      }
    }
    expect(forged && !lang::CppMirBodyEmitter(mutated, map)
                          .supportsFailureBodyText(address),
           "a moved value carrying borrowed state must not be body-staged as "
           "drop-free");
  }

  // A moved text view or C string carries only its caller's entry loan. Its
  // by-value field keeps ownerless text, so the move publishes no dependency
  // and MIR stages no Initialize: the constructor record publishes the exact
  // Move. The failure form keeps each Move in its verified body after the
  // ledger and the checked remainder.
  const lang::FrontendResult view =
      analyzeWithStandardLibrary("cpp-mir-moved-view-field.gti", R"(
#include <std/string>
#include <std/utility>

class Ledger {
public:
  int32_t id;
  Ledger(int32_t id, int32_t divisor) : id(id + 60 / divisor) {}
  ~Ledger() {}
};

class Label {
public:
  Ledger ledger;
  bool odd;
  std::string_view name;
  c_string tag;
  Label(std::string_view name, c_string tag, int32_t divisor)
      : ledger(Ledger(1, divisor)), odd(divisor % 2 == 1),
        name(std::move(name)), tag(std::move(tag)) {}
};

int main() {
  Label label = Label("abc", "t", 1);
  return label.name.size() == 3 && label.odd ? 0 : 1;
}
)");
  expect(view.canGenerateCode(),
         "the moved-view-field fixture should pass the frontend");
  if (!view.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *label = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       view.mir.constructorInstances()) {
    if (ownedInitializers(constructor) == 2 &&
        !constructor.parameterTypes.empty() &&
        constructor.parameterTypes.front().kind ==
            lang::SemanticType::StringView) {
      label = &constructor;
    }
  }
  expect(label != nullptr && !label->body.failureRecords.empty() &&
             !lang::cppMirOwnedParameterFieldBindings(view.mir, *label),
         "text moves recorded after fallible work are not native publications");
  if (label == nullptr) {
    return;
  }
  lang::SymbolId nameField = 0;
  for (const lang::MirConstructorInitializer &initializer :
       label->initializers) {
    if (initializer.targetType.kind == lang::SemanticType::StringView) {
      nameField = initializer.field;
    }
  }
  std::size_t recordMoves = 0;
  for (const lang::MirBlock &block : label->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Move &&
          lang::mirRecordPublishedConstructorInitializer(
              view.mir, label->body, label->id, instruction) != nullptr) {
        ++recordMoves;
      }
    }
  }
  expect(nameField != 0 && recordMoves == 2 &&
             std::none_of(label->body.loans.begin(), label->body.loans.end(),
                          [](const lang::MirLoan &loan) {
                            return loan.storedField != 0;
                          }),
         "each text field publishes from its constructor record, with no "
         "Stored loan for either field");
  const lang::CppMirBodyEmissionMap viewMap(buildRows(view, view.mir));
  const lang::MirBodyAddress labelAddress{
      .kind = lang::MirBodyKind::Constructor, .owner = label->id};
  expect(lang::CppMirBodyEmitter(view.mir, viewMap)
             .supportsFailureBodyText(labelAddress),
         "the failure form keeps moved text views in its verified body");

  std::string viewText;
  try {
    viewText = gti_test::emitCpp(view).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("moved text views after a fallible sibling "
                              "should emit: ") +
                      error.what());
    return;
  }
  const std::size_t labelStart = viewText.find(
      "Label::Label(::gti_internal::backend::mir_failure_constructor_tag_v1<");
  const std::size_t labelEnd = labelStart == std::string::npos
                                   ? labelStart
                                   : viewText.find("\n  }\n", labelStart);
  const std::string labelBody =
      labelEnd == std::string::npos
          ? std::string()
          : viewText.substr(labelStart, labelEnd - labelStart);
  const auto labelCount = [&](std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = labelBody.find(needle); at != std::string::npos;
         at = labelBody.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  const std::size_t labelRemainder = labelBody.find("mir_checked_remainder_v1");
  const std::size_t nameMove =
      labelBody.find("(*this).name = std::move(__gti_mir_v_");
  const std::size_t tagMove =
      labelBody.find("(*this).tag = std::move(__gti_mir_v_");
  expect(labelCount("std::move(__gti_mir_arg_") == 0 &&
             labelCount(" = std::move(__gti_mir_p_") == 2 &&
             labelCount("mir_rollback_field_v1(") == 1 &&
             labelRemainder != std::string::npos &&
             nameMove != std::string::npos && tagMove != std::string::npos &&
             labelBody.find("::__gti_program::Ledger(") < labelRemainder &&
             labelRemainder < nameMove && nameMove < tagMove,
         "the failure-form constructor publishes each moved text field in "
         "MIR order after the ledger");

  // Borrowed state must be the formal's own entry loan, which only ends: it
  // may neither feed a derived loan nor publish into the field.
  const auto refusesForged = [&](const auto &forge) {
    lang::MirProgram mutated = view.mir;
    bool forged = false;
    for (const auto &constructor : mutated.constructorInstances()) {
      if (constructor.id == label->id) {
        forged =
            forge(const_cast<lang::MirConstructorInstance &>(constructor).body);
      }
    }
    return forged && !lang::CppMirBodyEmitter(mutated, viewMap)
                          .supportsFailureBodyText(labelAddress);
  };
  expect(refusesForged([](lang::MirBody &body) {
           for (lang::MirPlace &place : body.places) {
             if (place.root == lang::MirPlaceRootKind::Binding &&
                 place.type.kind == lang::SemanticType::StringView) {
               place.traits.containsBorrowedState = false;
               return true;
             }
           }
           return false;
         }),
         "a moved view whose formal does not carry its borrowed state is not "
         "body-staged");
  expect(refusesForged([](lang::MirBody &body) {
           lang::MirLoanId entry = 0;
           for (const lang::MirLoan &loan : body.loans) {
             const lang::MirPlace *source = body.findPlace(loan.source);
             if (loan.entry && source != nullptr &&
                 source->type.kind == lang::SemanticType::StringView) {
               entry = loan.id;
             }
           }
           if (entry == 0) {
             return false;
           }
           lang::MirLoan derived = *body.findLoan(entry);
           derived.id = body.loans.size() + 1;
           derived.entry = false;
           derived.parent = entry;
           body.loans.push_back(derived);
           return true;
         }),
         "a loan derived from the moved view's entry loan is not body-staged");
  expect(refusesForged([&](lang::MirBody &body) {
           for (lang::MirLoan &loan : body.loans) {
             const lang::MirPlace *source = body.findPlace(loan.source);
             if (loan.entry && source != nullptr &&
                 source->type.kind == lang::SemanticType::StringView) {
               loan.storedField = nameField;
               return true;
             }
           }
           return false;
         }),
         "an entry loan that publishes into the moved view's field is not "
         "body-staged");

  // A user-defined carrier with a reference field retains its dependency in
  // the field: the move publishes a Stored loan, and the native class has no
  // empty representation, because its reference must be bound when the
  // object is constructed. Its owner declares the field in deferred storage
  // instead, and the failure form moves the formal into that storage in MIR
  // order (testDeferredBoundShellFields covers the family).
  const lang::FrontendResult carrier =
      analyzeWithStandardLibrary("cpp-mir-moved-carrier-field.gti", R"(
#include <std/utility>

class Ledger {
public:
  int32_t id;
  Ledger(int32_t id, int32_t divisor) : id(id + 60 / divisor) {}
  ~Ledger() {}
};

class Cursor {
public:
  int32_t& current;
  Cursor(int32_t& value) : current(value) {}
};

class Holder {
public:
  Ledger ledger;
  Cursor cursor;
  Holder(Cursor cursor, int32_t divisor)
      : ledger(Ledger(1, divisor)), cursor(std::move(cursor)) {}
};

int main() {
  int32_t value = 3;
  Holder holder = Holder(Cursor(value), 1);
  return holder.cursor.current == 3 ? 0 : 1;
}
)");
  expect(carrier.canGenerateCode(),
         "the moved-carrier-field fixture should pass the frontend");
  if (!carrier.canGenerateCode()) {
    return;
  }
  const lang::CppMirBodyEmissionMap carrierMap(buildRows(carrier, carrier.mir));
  std::size_t carrierConstructors = 0;
  for (const lang::MirConstructorInstance &constructor :
       carrier.mir.constructorInstances()) {
    if (ownedInitializers(constructor) != 1 ||
        constructor.parameterTypes.size() != 2) {
      continue;
    }
    ++carrierConstructors;
    const auto stored = std::find_if(
        constructor.body.loans.begin(), constructor.body.loans.end(),
        [](const lang::MirLoan &loan) {
          return loan.kind == lang::MirLoanKind::Stored &&
                 loan.storedField != 0;
        });
    expect(stored != constructor.body.loans.end() &&
               lang::cppMirDeferredFieldStorage(carrier.mir, constructor.owner,
                                                stored->storedField) &&
               !lang::cppMirOwnedParameterFieldBindings(carrier.mir,
                                                        constructor) &&
               lang::CppMirBodyEmitter(carrier.mir, carrierMap)
                   .supportsFailureBodyText(
                       {.kind = lang::MirBodyKind::Constructor,
                        .owner = constructor.id}),
           "a moved carrier that retains its dependency after fallible work "
           "publishes from deferred storage in its verified body");
  }
  expect(carrierConstructors == 1, "the carrier fixture keeps its constructor");
}

void testDeferredBoundShellFields() {
  // A borrowed carrier (a class whose references must be bound when it is
  // constructed) has no empty native value. A field of that type that can
  // follow other initialization work is declared in deferred storage: an
  // anonymous union whose inert byte is active until a constructor publishes
  // the field at its MIR point. That point may follow a fallible ledger, and
  // the carrier's own constructor may check its input. A class whose only
  // carrier is its first field keeps a native member.
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-deferred-bound-fields.gti", R"(
#include <std/utility>

class Ledger {
public:
  int32_t id;
  Ledger(int32_t id, int32_t divisor) : id(id + 60 / divisor) {}
  ~Ledger() {}
};

class Cursor {
public:
  int32_t& current;
  Cursor(int32_t& value) : current(value) {}
};

class Checked {
public:
  int32_t& current;
  int32_t step;
  Checked(int32_t& value, int32_t divisor) : current(value), step(60 / divisor) {}
};

class Moved {
public:
  Ledger ledger;
  Cursor cursor;
  Moved(Cursor cursor, int32_t divisor)
      : ledger(Ledger(1, divisor)), cursor(std::move(cursor)) {}
};

class Built {
public:
  Ledger ledger;
  Checked checked;
  Built(int32_t& value, int32_t divisor)
      : ledger(Ledger(2, divisor)), checked(Checked(value, divisor)) {}
};

class Leading {
public:
  Cursor cursor;
  Ledger ledger;
  Leading(Cursor cursor, int32_t divisor)
      : cursor(std::move(cursor)), ledger(Ledger(3, divisor)) {}
};

int main() {
  int32_t value = 3;
  Moved moved = Moved(Cursor(value), 1);
  Built built = Built(value, 1);
  Leading leading = Leading(Cursor(value), 1);
  return moved.cursor.current + built.checked.step + leading.cursor.current -
         66;
}
)");
  expect(frontend.canGenerateCode(),
         "the deferred-bound-fields fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto className = [](const lang::FrontendResult &result,
                            lang::HirClassInstanceId id) -> std::string {
    const lang::MirClassInstance *owner = result.mir.findClassInstance(id);
    const auto *info =
        owner == nullptr ? nullptr
                         : result.semantics.findClassType(owner->type.classId);
    return info == nullptr || info->declaration == nullptr
               ? std::string()
               : std::string(info->declaration->name().lexeme);
  };
  const auto constructorOf =
      [&](const lang::FrontendResult &result,
          std::string_view name) -> const lang::MirConstructorInstance * {
    for (const lang::MirConstructorInstance &constructor :
         result.mir.constructorInstances())
      if (constructor.definitionKind == lang::MirDefinitionKind::Source &&
          className(result, constructor.owner) == name)
        return &constructor;
    return nullptr;
  };
  // The field each constructor initializes with a borrowed carrier.
  const auto carrierField =
      [](const lang::MirConstructorInstance &constructor) {
        for (const lang::MirLoan &loan : constructor.body.loans)
          if (loan.kind == lang::MirLoanKind::Stored && loan.storedField != 0)
            return loan.storedField;
        return lang::SymbolId{0};
      };
  const auto *moved = constructorOf(frontend, "Moved");
  const auto *built = constructorOf(frontend, "Built");
  const auto *leading = constructorOf(frontend, "Leading");
  expect(moved != nullptr && built != nullptr && leading != nullptr &&
             carrierField(*moved) != 0 && carrierField(*built) != 0 &&
             carrierField(*leading) != 0,
         "each fixture constructor publishes one carrier field");
  if (moved == nullptr || built == nullptr || leading == nullptr) {
    return;
  }
  expect(lang::cppMirDeferredFieldStorage(frontend.mir, moved->owner,
                                          carrierField(*moved)) &&
             lang::cppMirDeferredFieldStorage(frontend.mir, built->owner,
                                              carrierField(*built)) &&
             !lang::cppMirDeferredFieldStorage(frontend.mir, leading->owner,
                                               carrierField(*leading)),
         "carriers after other fields use deferred storage; a first-field "
         "carrier keeps its native member");
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const auto failureAddress = [](const lang::MirConstructorInstance &item) {
    return lang::MirBodyAddress{.kind = lang::MirBodyKind::Constructor,
                                .owner = item.id};
  };
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
                 .supportsFailureBodyText(failureAddress(*moved)) &&
             lang::CppMirBodyEmitter(frontend.mir, map)
                 .supportsFailureBodyText(failureAddress(*built)),
         "the failure forms publish their carriers from deferred storage");

  std::string text;
  try {
    text = gti_test::emitCpp(frontend).contents;
  } catch (const std::exception &error) {
    expect(false,
           std::string("deferred bound fields should emit: ") + error.what());
    return;
  }
  expect(text.find("union { unsigned char __gti_deferred_cursor{}; "
                   "::__gti_program::Cursor cursor; };") != std::string::npos &&
             text.find("union { unsigned char __gti_deferred_checked{}; "
                       "::__gti_program::Checked checked; };") !=
                 std::string::npos &&
             text.find("  class Leading {\n  public:\n    "
                       "::__gti_program::Cursor cursor;\n") !=
                 std::string::npos,
         "deferred storage keeps each carrier's name and type in an "
         "anonymous union");
  const auto failureBody = [&](std::string_view owner) {
    const std::size_t start =
        text.find(std::string(owner) + "::" + std::string(owner) +
                  "(::gti_internal::backend::mir_failure_constructor_tag_v1<");
    const std::size_t end =
        start == std::string::npos ? start : text.find("\n  }\n", start);
    return end == std::string::npos ? std::string()
                                    : text.substr(start, end - start);
  };
  const auto count = [](const std::string &body, std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = body.find(needle); at != std::string::npos;
         at = body.find(needle, at + needle.size()))
      ++occurrences;
    return occurrences;
  };
  // The moved carrier starts inert and is constructed in its storage after
  // the ledger, from the formal's exact Move. Nothing rolls it back.
  const std::string movedBody = failureBody("Moved");
  const std::size_t movedList = movedBody.find(") : ");
  const std::size_t movedOpen = movedBody.find(" {\n", movedList);
  const std::size_t ledger = movedBody.find("::__gti_program::Ledger(");
  const std::size_t publication =
      movedBody.find("std::construct_at(std::addressof(");
  expect(
      movedList != std::string::npos && movedOpen != std::string::npos &&
          movedBody.substr(movedList, movedOpen - movedList).find("cursor(") ==
              std::string::npos &&
          ledger != std::string::npos && publication != std::string::npos &&
          ledger < publication &&
          count(movedBody, "std::construct_at(std::addressof(") == 1 &&
          count(movedBody, "mir_rollback_field_v1(") == 0 &&
          movedBody.find("publishes the dependency of its deferred field") !=
              std::string::npos,
      "the moved carrier publishes into deferred storage after the ledger");
  // The fallible carrier constructor builds in place through its status
  // overload; only the ledger's empty value is retired first.
  const std::string builtBody = failureBody("Built");
  const std::size_t ledgerBuilt = builtBody.find("::__gti_program::Ledger(");
  const std::size_t checkedBuilt =
      builtBody.find("::__gti_program::Checked(::gti_internal::backend::"
                     "mir_failure_constructor_tag_v1<");
  expect(ledgerBuilt != std::string::npos &&
             checkedBuilt != std::string::npos && ledgerBuilt < checkedBuilt &&
             count(builtBody, "std::destroy_at(std::addressof(") == 1,
         "the fallible carrier constructor builds in its deferred storage "
         "after the ledger");

  // The proof keeps the exact Stored publication: a forged dependency source
  // or a carrier with a source destructor is not deferred.
  const auto refuses = [&](const auto &forge) {
    lang::MirProgram mutated = frontend.mir;
    const bool forged = forge(mutated);
    return forged && !lang::CppMirBodyEmitter(mutated, map)
                          .supportsFailureBodyText(failureAddress(*moved));
  };
  expect(refuses([&](lang::MirProgram &program) {
           for (const auto &constructor : program.constructorInstances()) {
             if (constructor.id != moved->id)
               continue;
             auto &body = const_cast<lang::MirBody &>(constructor.body);
             for (lang::MirLoan &loan : body.loans)
               if (loan.kind == lang::MirLoanKind::Stored &&
                   loan.storedField != 0) {
                 for (const lang::MirPlace &place : body.places)
                   if (place.id != loan.source &&
                       place.root == lang::MirPlaceRootKind::This) {
                     loan.source = place.id;
                     return true;
                   }
               }
           }
           return false;
         }),
         "a stored dependency that does not name the moved formal is not "
         "body-staged");
  expect(refuses([&](lang::MirProgram &program) {
           for (const auto &instance : program.classInstances())
             if (className(frontend, instance.id) == "Cursor") {
               const_cast<lang::MirClassInstance &>(instance).destructorStatus =
                   lang::SpecialMemberStatus::Declared;
               return true;
             }
           return false;
         }),
         "a carrier whose destruction runs source code is not deferred");

  // A payload enum inside the carrier has a backend variant representation,
  // so the carrier stays outside deferred storage and outside the route.
  const lang::FrontendResult tagged =
      analyzeWithStandardLibrary("cpp-mir-deferred-payload-leaf.gti", R"(
#include <std/utility>

class Ledger {
public:
  int32_t id;
  Ledger(int32_t id, int32_t divisor) : id(id + 60 / divisor) {}
  ~Ledger() {}
};

enum class Reading { value(int32_t amount), missing };

class Tagged {
public:
  int32_t& current;
  Reading reading;
  Tagged(int32_t& value, Reading reading) : current(value), reading(reading) {}
};

class Labeled {
public:
  Ledger ledger;
  Tagged tagged;
  Labeled(Tagged tagged, int32_t divisor)
      : ledger(Ledger(4, divisor)), tagged(std::move(tagged)) {}
};

int main() {
  int32_t value = 3;
  Labeled labeled = Labeled(Tagged(value, Reading::missing), 1);
  return labeled.tagged.current - 3;
}
)");
  expect(tagged.canGenerateCode(),
         "the payload-leaf fixture should pass the frontend");
  if (!tagged.canGenerateCode()) {
    return;
  }
  const auto *labeled = constructorOf(tagged, "Labeled");
  const lang::CppMirBodyEmissionMap taggedMap(buildRows(tagged, tagged.mir));
  expect(labeled != nullptr && carrierField(*labeled) != 0 &&
             !lang::cppMirDeferredFieldStorage(tagged.mir, labeled->owner,
                                               carrierField(*labeled)) &&
             !lang::CppMirBodyEmitter(tagged.mir, taggedMap)
                  .supportsFailureBodyText(failureAddress(*labeled)),
         "a carrier with a payload enum leaf is not deferred");
}

void testMovedBorrowedPayloadAfterFallibleSibling() {
  // A borrowed payload enum moved from its formal keeps the caller's owner in
  // the field: MIR hands the formal's entry group to the field's Stored loan
  // in one proof-only transfer. Unlike a class with a reference field, the
  // enum has an empty state, so the failure form moves the formal in MIR
  // order once the fallible ledger exists.
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-moved-borrowed-payload.gti", R"(
#include <std/utility>

enum Slot { ready(int32_t& value), empty };

class Ledger {
public:
  int32_t id;
  Ledger(int32_t id, int32_t divisor) : id(id + 60 / divisor) {}
  ~Ledger() {}
};

class Gauge {
public:
  Ledger ledger;
  Slot slot;
  Gauge(Slot slot, int32_t divisor)
      : ledger(Ledger(1, divisor)), slot(std::move(slot)) {}
};

int main() {
  int32_t owner = 3;
  Gauge gauge = Gauge(Slot::ready(owner), 1);
  return gauge.ledger.id - 61;
}
)");
  expect(frontend.canGenerateCode(),
         "the moved-borrowed-payload fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *gauge = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.completeBorrow && constructor.mayRaiseDefinedFailure) {
      gauge = &constructor;
    }
  }
  expect(gauge != nullptr, "the gauge constructor may raise and publishes a "
                           "complete borrowed field");
  if (gauge == nullptr) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = gauge->id};
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
             .supportsFailureBodyText(address),
         "the failure form keeps the borrowed payload move in its body");

  std::string text;
  try {
    text = gti_test::emitCpp(frontend).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("a borrowed payload moved after a fallible "
                              "sibling should emit: ") +
                      error.what());
    return;
  }
  const std::size_t start = text.find(
      "Gauge::Gauge(::gti_internal::backend::mir_failure_constructor_tag_v1<");
  const std::size_t end =
      start == std::string::npos ? start : text.find("\n  }\n", start);
  const std::string body = end == std::string::npos
                               ? std::string()
                               : text.substr(start, end - start);
  const std::size_t ledger = body.find("::__gti_program::Ledger(");
  const std::size_t move = body.find(" = std::move(__gti_mir_p_");
  expect(body.find("slot(::gti_internal::backend::mir_empty_state_v1<std::"
                   "remove_cv_t<decltype(slot)>>())") != std::string::npos &&
             body.find("std::move(__gti_mir_arg_") == std::string::npos &&
             ledger != std::string::npos && move != std::string::npos &&
             ledger < move,
         "the failure-form constructor moves the borrowed payload in MIR "
         "order from its empty state");

  const auto refusesForged = [&](const auto &forge) {
    lang::MirProgram mutated = frontend.mir;
    bool forged = false;
    for (const auto &constructor : mutated.constructorInstances()) {
      if (constructor.id == gauge->id) {
        forged =
            forge(const_cast<lang::MirConstructorInstance &>(constructor).body);
      }
    }
    return forged && !lang::CppMirBodyEmitter(mutated, map)
                          .supportsFailureBodyText(address);
  };
  expect(refusesForged([](lang::MirBody &body) {
           for (lang::MirLoan &loan : body.loans) {
             if (loan.escapes && loan.storedField != 0) {
               loan.producedBy = loan.producedBy + 1;
               return true;
             }
           }
           return false;
         }),
         "a field publication not produced by the moved value is not "
         "body-staged");
  expect(refusesForged([](lang::MirBody &body) {
           lang::MirLoanId entry = 0;
           for (const lang::MirLoan &loan : body.loans) {
             if (loan.entry && loan.kind == lang::MirLoanKind::Stored) {
               entry = loan.id;
             }
           }
           for (lang::MirBlock &block : body.blocks) {
             for (std::size_t index = 0; index < block.instructions.size();
                  ++index) {
               lang::MirInstruction &instruction = block.instructions[index];
               // The failure route's retirement, not the handoff after the
               // transfer, gains metadata no comment-only record carries.
               if (entry != 0 &&
                   instruction.kind == lang::MirInstructionKind::EndBorrow &&
                   instruction.loan == entry &&
                   (index == 0 || block.instructions[index - 1].kind !=
                                      lang::MirInstructionKind::Borrow)) {
                 instruction.fullExpressionEnd = 1;
                 return true;
               }
             }
           }
           return false;
         }),
         "an entry-group record that is not a comment-only retirement is not "
         "body-staged");
}

void testMovedViewAfterFormalRead() {
  // Earlier initializers read the view formal, as a receiver and as a call
  // argument, and no record can fail. The native member-initializer list
  // would move the formal before those reads run in the body, so the owned
  // proof refuses it; the failure form keeps the reads and the move in MIR
  // order. A sibling that does not name the formal keeps the native move.
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-moved-view-after-read.gti", R"(
#include <std/string>
#include <std/utility>

uint64_t measure(std::string_view text) { return text.size(); }

class Sized {
public:
  uint64_t length;
  uint64_t measured;
  std::string_view name;
  Sized(std::string_view name)
      : length(name.size()), measured(measure(name)), name(std::move(name)) {}
};

class Flagged {
public:
  bool small;
  std::string_view name;
  Flagged(std::string_view name, int32_t tag)
      : small(tag < 10), name(std::move(name)) {}
};

int main() {
  Sized sized = Sized("abc");
  Flagged flagged = Flagged("de", 4);
  return sized.length == 3 && sized.measured == 3 && flagged.small &&
                 flagged.name.size() == 2
             ? 0
             : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the moved-view-after-read fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *sized = nullptr;
  const lang::MirConstructorInstance *flagged = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.parameterTypes.empty() ||
        constructor.parameterTypes.front().kind !=
            lang::SemanticType::StringView ||
        constructor.initializers.empty() ||
        !constructor.initializers.back().ownedParameter) {
      continue;
    }
    if (constructor.parameterTypes.size() == 1 &&
        constructor.initializers.size() == 3) {
      sized = &constructor;
    } else if (constructor.parameterTypes.size() == 2 &&
               constructor.parameterTypes.back() == lang::SemanticType::Int32 &&
               constructor.initializers.size() == 2) {
      flagged = &constructor;
    }
  }
  const auto flaggedBindings =
      flagged == nullptr
          ? std::nullopt
          : lang::cppMirOwnedParameterFieldBindings(frontend.mir, *flagged);
  expect(sized != nullptr && sized->body.failureRecords.empty() &&
             sized->mayRaiseDefinedFailure &&
             !lang::cppMirOwnedParameterFieldBindings(frontend.mir, *sized),
         "a move recorded after reads of its formal is not a native "
         "publication");
  expect(flaggedBindings && flaggedBindings->size() == 1,
         "a move after a sibling that does not name the formal stays native");
  if (sized == nullptr) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
             .supportsFailureBodyText(
                 {.kind = lang::MirBodyKind::Constructor, .owner = sized->id}),
         "the failure form keeps the reads and the move in its verified body");

  std::string text;
  try {
    text = gti_test::emitCpp(frontend).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("a view moved after reads of it should emit: ") +
                      error.what());
    return;
  }
  const std::size_t start = text.find(
      "Sized::Sized(::gti_internal::backend::mir_failure_constructor_tag_v1<");
  const std::size_t end =
      start == std::string::npos ? start : text.find("\n  }\n", start);
  const std::string body = end == std::string::npos
                               ? std::string()
                               : text.substr(start, end - start);
  const std::size_t read = body.find(".size();");
  const std::size_t move = body.find(" = std::move(__gti_mir_p_");
  const std::size_t publish = body.find("(*this).name = std::move(");
  expect(body.find("std::move(__gti_mir_arg_") == std::string::npos &&
             body.find("std::string_view __gti_mir_p_") != std::string::npos &&
             read != std::string::npos && move != std::string::npos &&
             publish != std::string::npos && read < move && move < publish,
         "the formal is read through its body alias before MIR moves it into "
         "the field");
  expect(text.find("Sized::Sized(std::string_view") == std::string::npos,
         "only the failure form is defined for the reading constructor");
}

void testPassiveTextViewClassAfterFallibleSibling() {
  // A user-defined class whose only field is a text view is a passive
  // generated class: the view copies bitwise and destroying it runs no
  // source cleanup. A by-value text field holds ownerless text, so MIR
  // records no loan for the class formal and, as for a class of scalar
  // fields, neither a value nor a rollback obligation for its move. Moved
  // after a fallible field, it publishes from the field's empty state in the
  // verified failure-form body; copied, through the native parameter copy.
  const auto program = [](std::string_view initializer) {
    return std::string(R"(
#include <std/string>
#include <std/utility>
#include <std/vector>

class Name {
public:
  std::string_view text;
  Name(std::string_view text) : text(text) {}
};

class shelf {
  std::vector<int32_t> v;
  Name name;
public:
  shelf(Name name) : v(std::vector<int32_t>()), )") +
           std::string(initializer) + R"( {}
  int32_t get() { return name.text.size() == 3 ? 7 : 1; }
};

int main() {
  shelf s = shelf(Name("abc"));
  return s.get();
}
)";
  };
  const auto named = [](const lang::FrontendResult &frontend,
                        const lang::SemanticType &type, std::string_view name) {
    const auto *info = type.kind == lang::SemanticType::Class
                           ? frontend.semantics.findClassType(type.classId)
                           : nullptr;
    return info != nullptr && info->declaration != nullptr &&
           info->declaration->name().lexeme == name;
  };
  const auto shelfConstructor = [&](const lang::FrontendResult &frontend) {
    const lang::MirConstructorInstance *found = nullptr;
    for (const lang::MirConstructorInstance &constructor :
         frontend.mir.constructorInstances()) {
      const auto *owner = frontend.mir.findClassInstance(constructor.owner);
      if (owner != nullptr && named(frontend, owner->type, "shelf") &&
          constructor.parameterTypes.size() == 1) {
        found = &constructor;
      }
    }
    return found;
  };
  // A copy of the program whose Name instance is forged.
  const auto forgedName = [&](const lang::FrontendResult &frontend,
                              const auto &forge) {
    lang::MirProgram mutated = frontend.mir;
    auto &instances = const_cast<std::vector<lang::MirClassInstance> &>(
        mutated.classInstances());
    bool forged = false;
    for (lang::MirClassInstance &instance : instances) {
      if (named(frontend, instance.type, "Name") &&
          instance.declaredFields.size() == 1) {
        forge(instance);
        forged = true;
      }
    }
    return std::pair{std::move(mutated), forged};
  };
  const auto failureBody = [](const std::string &text) {
    const std::size_t start = text.find("shelf::shelf(::gti_internal::backend::"
                                        "mir_failure_constructor_tag_v1<");
    const std::size_t end =
        start == std::string::npos ? start : text.find("\n  }\n", start);
    return end == std::string::npos ? std::string()
                                    : text.substr(start, end - start);
  };
  const auto count = [](const std::string &body, std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = body.find(needle); at != std::string::npos;
         at = body.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };

  const lang::FrontendResult moved =
      analyzeWithStandardLibrary("cpp-mir-passive-text-view-class-move.gti",
                                 program("name(std::move(name))"));
  expect(moved.canGenerateCode(),
         "the moved text-view class fixture should pass the frontend");
  if (!moved.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *move = shelfConstructor(moved);
  expect(move != nullptr && move->mayRaiseDefinedFailure &&
             !move->body.failureRecords.empty() &&
             !lang::cppMirOwnedParameterFieldBindings(moved.mir, *move),
         "a class move recorded after fallible work is not a native "
         "publication");
  if (move == nullptr) {
    return;
  }
  const lang::SemanticType &nameType = move->parameterTypes.front();
  const bool passiveRecords =
      move->body.loans.empty() &&
      std::none_of(
          move->body.dropObligations.begin(), move->body.dropObligations.end(),
          [&](const lang::MirDropObligation &drop) {
            return drop.dropType.type == nameType &&
                   (drop.kind != lang::MirDropObligationKind::Binding ||
                    drop.dropType.requiresActiveCleanup);
          });
  bool lifecycleFree = true;
  for (const lang::MirBlock &block : move->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if ((instruction.kind == lang::MirInstructionKind::Move ||
           instruction.kind == lang::MirInstructionKind::Initialize) &&
          instruction.info.type == nameType) {
        lifecycleFree = lifecycleFree && instruction.lifecycle.empty();
      }
    }
  }
  expect(passiveRecords && lifecycleFree,
         "MIR records no loan, value or rollback obligation and no lifecycle "
         "event for the moved text-view class, only its inert formal drop");

  const lang::CppMirBodyEmissionMap movedMap(buildRows(moved, moved.mir));
  const lang::MirBodyAddress moveAddress{.kind = lang::MirBodyKind::Constructor,
                                         .owner = move->id};
  expect(lang::CppMirBodyEmitter(moved.mir, movedMap)
             .supportsFailureBodyText(moveAddress),
         "the failure form keeps the passive class move in its verified body");
  std::string movedText;
  try {
    movedText = gti_test::emitCpp(moved).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("a text-view class moved after a fallible "
                              "sibling should emit: ") +
                      error.what());
    return;
  }
  const std::string moveBody = failureBody(movedText);
  const std::size_t vector = moveBody.find(
      "vector<std::int32_t>(::gti_internal::backend::mir_failure_constructor_"
      "tag_v1<");
  const std::size_t publication = moveBody.find(" = std::move(__gti_mir_v_");
  expect(count(moveBody, "name(::gti_internal::backend::mir_empty_state_v1<"
                         "std::remove_cv_t<decltype(name)>>())") == 1 &&
             count(moveBody, " = std::move(__gti_mir_v_") == 1 &&
             vector != std::string::npos && publication != std::string::npos &&
             vector < publication,
         "the moved class field starts empty and is published once, after "
         "the fallible vector");

  // Borrowed state and active cleanup each leave the passive route.
  const auto refuses = [&](const auto &forge) {
    auto [mutated, forged] = forgedName(moved, forge);
    return forged && !lang::CppMirBodyEmitter(mutated, movedMap)
                          .supportsFailureBodyText(moveAddress);
  };
  expect(refuses([](lang::MirClassInstance &instance) {
           instance.traits.containsBorrowedState = true;
         }),
         "a class carrying borrowed state is not moved by the passive route");
  expect(refuses([](lang::MirClassInstance &instance) {
           instance.declaredFields.front().requiresActiveCleanup = true;
         }),
         "a field with active cleanup makes the class non-passive");

  const lang::FrontendResult copied = analyzeWithStandardLibrary(
      "cpp-mir-passive-text-view-class-copy.gti", program("name(name)"));
  expect(copied.canGenerateCode(),
         "the copied text-view class fixture should pass the frontend");
  if (!copied.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *copy = shelfConstructor(copied);
  const auto copiesName = [&](const lang::MirProgram &mir) {
    const auto bindings = lang::cppMirCopyParameterFieldBindings(mir, *copy);
    return bindings &&
           std::any_of(
               bindings->begin(), bindings->end(),
               [](const lang::CppMirCopyParameterFieldBinding &binding) {
                 return binding.initializer == 1;
               });
  };
  expect(copy != nullptr && copy->mayRaiseDefinedFailure &&
             copiesName(copied.mir),
         "the passive text-view class copy is an exact parameter copy");
  if (copy == nullptr) {
    return;
  }
  std::string copiedText;
  try {
    copiedText = gti_test::emitCpp(copied).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("a text-view class copied after a fallible "
                              "sibling should emit: ") +
                      error.what());
    return;
  }
  const std::string copyBody = failureBody(copiedText);
  expect(count(copyBody, "name(__gti_mir_arg_0)") == 1 &&
             copyBody.find("mir_lifetime_slot<::__gti_program::Name>") ==
                 std::string::npos,
         "the copied class field publishes through the native parameter copy");
  auto [uncopyable, forged] =
      forgedName(copied, [](lang::MirClassInstance &instance) {
        instance.declaredFields.front().requiresActiveCleanup = true;
      });
  expect(forged && !copiesName(uncopyable),
         "a class with an actively cleaned field is not a passive copy");
}

void testPassiveFunctionItemClassAfterFallibleSibling() {
  // A generic class instantiated with a function name holds a function item:
  // a stateless identity tag that copies bitwise and runs no cleanup, called
  // directly by MIR rather than through the stored object. Like a class of
  // scalar fields, it moves after a fallible field in the verified
  // failure-form body and copies through the native parameter copy.
  const auto program = [](std::string_view initializer) {
    return std::string(R"(
#include <std/utility>
#include <std/vector>

int32_t seven() { return 7; }

class Op<F> {
public:
  F action;
  Op(F action) : action(action) {}
};

class Holder<F> {
public:
  std::vector<int32_t> v;
  Op<F> op;
  Holder(Op<F> op) : v(std::vector<int32_t>()), )") +
           std::string(initializer) + R"( {}
};

int32_t run<F>(F action) {
  Holder<F> holder = Holder<F>(Op<F>(action));
  int32_t called = holder.op.action();
  return called;
}

int main() { return run(seven); }
)";
  };
  const auto named = [](const lang::FrontendResult &frontend,
                        const lang::SemanticType &type, std::string_view name) {
    const auto *info = type.kind == lang::SemanticType::Class
                           ? frontend.semantics.findClassType(type.classId)
                           : nullptr;
    return info != nullptr && info->declaration != nullptr &&
           info->declaration->name().lexeme == name;
  };
  const auto holderConstructor = [&](const lang::FrontendResult &frontend) {
    const lang::MirConstructorInstance *found = nullptr;
    for (const lang::MirConstructorInstance &constructor :
         frontend.mir.constructorInstances()) {
      const auto *owner = frontend.mir.findClassInstance(constructor.owner);
      if (owner != nullptr && named(frontend, owner->type, "Holder") &&
          constructor.parameterTypes.size() == 1) {
        found = &constructor;
      }
    }
    return found;
  };
  // A copy of the program whose Op instance is forged.
  const auto forgedOp = [&](const lang::FrontendResult &frontend,
                            const auto &forge) {
    lang::MirProgram mutated = frontend.mir;
    auto &instances = const_cast<std::vector<lang::MirClassInstance> &>(
        mutated.classInstances());
    bool forged = false;
    for (lang::MirClassInstance &instance : instances) {
      if (named(frontend, instance.type, "Op") &&
          instance.declaredFields.size() == 1 &&
          instance.declaredFields.front().type.kind ==
              lang::SemanticType::FunctionItem) {
        forge(instance);
        forged = true;
      }
    }
    return std::pair{std::move(mutated), forged};
  };
  // The out-of-class definition of the failure form, not its in-class
  // declaration or the ordinary overload.
  const auto failureBody = [](const std::string &text) {
    for (std::size_t start = text.find("_Holder::__gti_type_");
         start != std::string::npos;
         start = text.find("_Holder::__gti_type_", start + 1)) {
      const std::size_t line = text.find('\n', start);
      if (text.substr(start, line - start)
              .find("(::gti_internal::backend::mir_failure_constructor_tag_"
                    "v1<") == std::string::npos) {
        continue;
      }
      const std::size_t end = text.find("\n  }\n", start);
      return end == std::string::npos ? std::string()
                                      : text.substr(start, end - start);
    }
    return std::string();
  };
  const auto count = [](const std::string &body, std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = body.find(needle); at != std::string::npos;
         at = body.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };

  const lang::FrontendResult moved =
      analyzeWithStandardLibrary("cpp-mir-passive-function-item-class-move.gti",
                                 program("op(std::move(op))"));
  expect(moved.canGenerateCode(),
         "the moved function-item class fixture should pass the frontend");
  if (!moved.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *move = holderConstructor(moved);
  expect(move != nullptr && move->mayRaiseDefinedFailure &&
             !move->body.failureRecords.empty() &&
             !lang::cppMirOwnedParameterFieldBindings(moved.mir, *move),
         "a class move recorded after fallible work is not a native "
         "publication");
  if (move == nullptr) {
    return;
  }
  const lang::SemanticType &opType = move->parameterTypes.front();
  bool lifecycleFree = true;
  for (const lang::MirBlock &block : move->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if ((instruction.kind == lang::MirInstructionKind::Move ||
           instruction.kind == lang::MirInstructionKind::Initialize) &&
          instruction.info.type == opType) {
        lifecycleFree = lifecycleFree && instruction.lifecycle.empty();
      }
    }
  }
  expect(move->body.loans.empty() && lifecycleFree &&
             std::none_of(move->body.dropObligations.begin(),
                          move->body.dropObligations.end(),
                          [&](const lang::MirDropObligation &drop) {
                            return drop.dropType.type == opType &&
                                   (drop.kind !=
                                        lang::MirDropObligationKind::Binding ||
                                    drop.dropType.requiresActiveCleanup);
                          }),
         "MIR records no loan, value or rollback obligation and no lifecycle "
         "event for the moved function-item class");

  const lang::CppMirBodyEmissionMap movedMap(buildRows(moved, moved.mir));
  const lang::MirBodyAddress moveAddress{.kind = lang::MirBodyKind::Constructor,
                                         .owner = move->id};
  expect(lang::CppMirBodyEmitter(moved.mir, movedMap)
             .supportsFailureBodyText(moveAddress),
         "the failure form keeps the function-item class move in its body");
  std::string movedText;
  try {
    movedText = gti_test::emitCpp(moved).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("a function-item class moved after a fallible "
                              "sibling should emit: ") +
                      error.what());
    return;
  }
  const std::string moveBody = failureBody(movedText);
  const std::size_t vector = moveBody.find(
      "vector<std::int32_t>(::gti_internal::backend::mir_failure_constructor_"
      "tag_v1<");
  const std::size_t publication = moveBody.find(" = std::move(__gti_mir_v_");
  expect(count(moveBody, "op(::gti_internal::backend::mir_empty_state_v1<"
                         "std::remove_cv_t<decltype(op)>>())") == 1 &&
             count(moveBody, " = std::move(__gti_mir_v_") == 1 &&
             vector != std::string::npos && publication != std::string::npos &&
             vector < publication,
         "the moved function-item class field starts empty and is published "
         "once, after the fallible vector");

  // Only a shaped function item is a passive leaf, and active cleanup still
  // leaves the passive route.
  const auto refuses = [&](const auto &forge) {
    auto [mutated, forged] = forgedOp(moved, forge);
    return forged && !lang::CppMirBodyEmitter(mutated, movedMap)
                          .supportsFailureBodyText(moveAddress);
  };
  expect(refuses([](lang::MirClassInstance &instance) {
           instance.declaredFields.front().type.functionId = 0;
         }),
         "a function item without its function identity is not passive");
  expect(refuses([](lang::MirClassInstance &instance) {
           instance.declaredFields.front().requiresActiveCleanup = true;
         }),
         "a function-item field with active cleanup makes the class "
         "non-passive");

  const lang::FrontendResult copied = analyzeWithStandardLibrary(
      "cpp-mir-passive-function-item-class-copy.gti", program("op(op)"));
  expect(copied.canGenerateCode(),
         "the copied function-item class fixture should pass the frontend");
  if (!copied.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *copy = holderConstructor(copied);
  const auto copiesOp = [&](const lang::MirProgram &mir) {
    const auto bindings = lang::cppMirCopyParameterFieldBindings(mir, *copy);
    return bindings &&
           std::any_of(
               bindings->begin(), bindings->end(),
               [](const lang::CppMirCopyParameterFieldBinding &binding) {
                 return binding.initializer == 1;
               });
  };
  expect(copy != nullptr && copy->mayRaiseDefinedFailure &&
             copiesOp(copied.mir),
         "the passive function-item class copy is an exact parameter copy");
  if (copy == nullptr) {
    return;
  }
  std::string copiedText;
  try {
    copiedText = gti_test::emitCpp(copied).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("a function-item class copied after a "
                              "fallible sibling should emit: ") +
                      error.what());
    return;
  }
  const std::string copyBody = failureBody(copiedText);
  expect(count(copyBody, "op(__gti_mir_arg_0)") == 1 &&
             copyBody.find("mir_lifetime_slot<") == std::string::npos,
         "the copied function-item class field publishes through the native "
         "parameter copy");
  auto [shapeless, forged] =
      forgedOp(copied, [](lang::MirClassInstance &instance) {
        instance.declaredFields.front().type.functionId = 0;
      });
  expect(forged && !copiesOp(shapeless),
         "a class holding a function item without its identity is not a "
         "passive copy");
}

void testPassiveClassMoveAndCopyAfterFallibleSibling() {
  // One passive class moves and another is copied after a fallible field.
  // The move stays in the verified body. The copy is not part of the native
  // first-field prefix, but copying an unwritten formal of a passive class
  // runs no source code and cannot fail, so it still publishes natively.
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-passive-class-move-and-copy.gti", R"(
#include <std/utility>

class Ledger {
public:
  int32_t id;
  Ledger(int32_t id, int32_t divisor) : id(id + 60 / divisor) {}
  ~Ledger() {}
};

class Pair {
public:
  int32_t a;
  int32_t b;
  Pair(int32_t a, int32_t b) : a(a), b(b) {}
};

class Shelved {
public:
  Ledger ledger;
  Pair moved;
  Pair copied;
  Shelved(Pair moved, Pair copied, int32_t divisor)
      : ledger(Ledger(31, divisor)), moved(std::move(moved)), copied(copied) {}
};

bool is_odd(int32_t value) { return value % 2 == 1; }

class Gauge {
public:
  bool flagged;
  int32_t tag;
  Pair pair;
  Gauge(int32_t tag, Pair pair)
      : flagged(is_odd(tag)), tag(std::move(tag)), pair(pair) {}
};

int main() {
  Shelved shelved = Shelved(Pair(1, 2), Pair(3, 4), 1);
  Gauge gauge = Gauge(7, Pair(5, 6));
  return shelved.moved.a + shelved.copied.b + gauge.pair.a - 10 +
         (gauge.flagged ? gauge.tag - 7 : 1);
}
)");
  expect(frontend.canGenerateCode(),
         "the passive move-and-copy fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *shelved = nullptr;
  const lang::MirConstructorInstance *gauge = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    const auto owned = std::count_if(
        constructor.initializers.begin(), constructor.initializers.end(),
        [](const lang::MirConstructorInitializer &initializer) {
          return initializer.ownedParameter.has_value();
        });
    if (owned != 1)
      continue;
    if (constructor.parameterTypes.size() == 3)
      shelved = &constructor;
    else if (constructor.parameterTypes.size() == 2 &&
             constructor.parameterTypes.front() == lang::SemanticType::Int32)
      gauge = &constructor;
  }
  expect(shelved != nullptr && gauge != nullptr,
         "both mixed constructors are found");
  if (shelved == nullptr || gauge == nullptr) {
    return;
  }
  const auto copiesField = [&](const lang::MirConstructorInstance &constructor,
                               std::size_t initializer) {
    const auto bindings =
        lang::cppMirCopyParameterFieldBindings(frontend.mir, constructor);
    return bindings &&
           std::any_of(
               bindings->begin(), bindings->end(),
               [&](const lang::CppMirCopyParameterFieldBinding &binding) {
                 return binding.initializer == initializer;
               });
  };
  expect(shelved->mayRaiseDefinedFailure &&
             !shelved->body.failureRecords.empty() &&
             !lang::cppMirOwnedParameterFieldBindings(frontend.mir, *shelved) &&
             copiesField(*shelved, 2),
         "the move after fallible work is body-staged and the later copy is "
         "an exact parameter copy");
  // A class formal's lexical drop keeps Gauge conservatively failure-capable,
  // so its drop-free scalar move, staged after a read of its formal, also
  // takes the failure form beside the native class copy.
  expect(gauge->mayRaiseDefinedFailure && copiesField(*gauge, 2) &&
             !lang::cppMirOwnedParameterFieldBindings(frontend.mir, *gauge),
         "the scalar move after a read is body-staged beside the class copy");
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  for (const auto *constructor : {shelved, gauge}) {
    expect(lang::CppMirBodyEmitter(frontend.mir, map)
               .supportsFailureBodyText({.kind = lang::MirBodyKind::Constructor,
                                         .owner = constructor->id}),
           "the failure form admits the body-staged move beside the native "
           "copy");
  }
  // A copy hoisted out of the prefix must still be MIR's exact adjacent Load,
  // Initialize and boundary: reordering its records fails closed.
  std::optional<lang::CppMirCopyParameterFieldBinding> hoisted;
  if (const auto bindings =
          lang::cppMirCopyParameterFieldBindings(frontend.mir, *shelved)) {
    for (const auto &binding : *bindings) {
      if (binding.initializer == 2)
        hoisted = binding;
    }
  }
  expect(hoisted.has_value() && hoisted->initializeInstruction != 0,
         "the later copy has an exact Initialize to hoist");
  if (hoisted) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                       .owner = shelved->id};
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    bool forged = false;
    for (auto &block : body->blocks) {
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index) {
        if (!forged &&
            block.instructions[index].id == hoisted->initializeInstruction) {
          std::swap(block.instructions[index], block.instructions[index + 1]);
          forged = true;
        }
      }
    }
    expect(forged && !lang::CppMirBodyEmitter(malformed, map)
                          .supportsFailureBodyText(address),
           "a hoisted passive copy with reordered records fails closed");
  }

  std::string text;
  try {
    text = gti_test::emitCpp(frontend).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("a passive move beside a passive copy after a "
                              "fallible sibling should emit: ") +
                      error.what());
    return;
  }
  const std::size_t start = text.find(
      "Shelved::Shelved(::gti_internal::backend::mir_failure_constructor_tag_"
      "v1<");
  const std::size_t end =
      start == std::string::npos ? start : text.find("\n  }\n", start);
  const std::string body = end == std::string::npos
                               ? std::string()
                               : text.substr(start, end - start);
  const std::size_t ledger = body.find("::__gti_program::Ledger(");
  const std::size_t publication = body.find(" = std::move(__gti_mir_v_");
  expect(body.find("moved(::gti_internal::backend::mir_empty_state_v1<std::"
                   "remove_cv_t<decltype(moved)>>())") != std::string::npos &&
             body.find("copied(__gti_mir_arg_1)") != std::string::npos &&
             ledger != std::string::npos && publication != std::string::npos &&
             ledger < publication,
         "the moved field starts empty and is published after the ledger, "
         "while the copied field publishes natively");
}

void testMovedChainReturnConstruction() {
  // A failure-free body returns a class constructed from a moved scalar. The
  // Move and its call input stage into the construction and never
  // materialize, so the returned construction must spell std::move over the
  // formal; spelling the unassigned call-input local returned zero.
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-moved-chain-return.gti", R"(
#include <std/utility>

class Holder {
public:
  int32_t value;
  Holder(int32_t value) : value(value) {}
};

Holder make(int32_t value) { return Holder(std::move(value)); }

expected<int32_t, int32_t> refuse(int32_t code) {
  return unexpected(std::move(code));
}

int main() {
  Holder holder = make(42);
  return holder.value - 42 + (refuse(3) ? 1 : refuse(3).error() - 3);
}
)");
  expect(frontend.canGenerateCode(),
         "the moved-chain return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  for (std::string_view name : {"make", "unexpected"}) {
    const lang::MirFunctionInstance *function = nullptr;
    for (const lang::HirFunctionInstance &candidate :
         frontend.hir.functionInstances())
      if (candidate.source != nullptr &&
          candidate.source->name().lexeme == name)
        function = frontend.mir.findFunctionInstance(candidate.id);
    expect(function != nullptr && !function->mayRaiseDefinedFailure,
           "a construction from a moved scalar should be failure-free");
    if (function == nullptr) {
      continue;
    }
    const std::string text = selectedBodyText(frontend, *function);
    const std::size_t link = text.find("links a moved-place chain");
    const std::size_t returned = text.find("(std::move(__gti_mir_p_1));");
    expect(link != std::string::npos && returned != std::string::npos &&
               text.find("return ", link) < returned,
           "the returned construction should move the formal itself: " + text);
  }
}

void testMovedPassiveFieldInPlainConstructor() {
  // Moving a scalar formal cannot fail, so these constructors are proved
  // failure-free and use the ordinary form. A move after a computed or called
  // sibling, or after a copy of the same formal, cannot be hoisted into the
  // member-initializer list: the ordinary body publishes it in MIR order from
  // the field's empty state. A leading move stays a native publication.
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-moved-passive-plain-field.gti", R"(
#include <std/utility>

bool is_odd(int32_t value) { return (value & 1) == 1; }

class Gauge {
public:
  bool odd;
  bool flagged;
  int32_t tag;
  Gauge(int32_t tag)
      : odd((tag & 1) == 1), flagged(is_odd(tag)), tag(std::move(tag)) {}
};

class Copied {
public:
  int32_t first;
  int32_t second;
  Copied(int32_t tag) : first(tag), second(std::move(tag)) {}
};

class Leading {
public:
  int32_t tag;
  bool odd;
  int32_t mirrored;
  Leading(int32_t tag, int32_t other)
      : tag(std::move(tag)), odd(is_odd(other)), mirrored(other) {}
};

class CopiedChecked {
public:
  int32_t first;
  int32_t second;
  int32_t quotient;
  CopiedChecked(int32_t tag, int32_t divisor)
      : first(tag), second(std::move(tag)), quotient(60 / divisor) {}
};

int main() {
  Gauge gauge = Gauge(7);
  Copied copied = Copied(9);
  Leading leading = Leading(11, 3);
  CopiedChecked checked = CopiedChecked(2, 30);
  return gauge.odd && gauge.flagged && leading.odd
             ? gauge.tag + copied.first + copied.second + leading.tag +
                   leading.mirrored + checked.first + checked.second +
                   checked.quotient - 45
             : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the plain moved-passive-field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto constructorOf =
      [&](std::string_view owner) -> const lang::MirConstructorInstance * {
    const auto found = std::find_if(
        frontend.hir.classInstances().begin(),
        frontend.hir.classInstances().end(), [&](const auto &instance) {
          return instance.source != nullptr &&
                 instance.source->name().lexeme == owner;
        });
    for (const lang::MirConstructorInstance &constructor :
         frontend.mir.constructorInstances())
      if (found != frontend.hir.classInstances().end() &&
          constructor.owner == found->id)
        return &constructor;
    return nullptr;
  };
  const lang::MirConstructorInstance *gauge = constructorOf("Gauge");
  const lang::MirConstructorInstance *copied = constructorOf("Copied");
  const lang::MirConstructorInstance *leading = constructorOf("Leading");
  const lang::MirConstructorInstance *checked = constructorOf("CopiedChecked");
  expect(gauge != nullptr && copied != nullptr && leading != nullptr &&
             checked != nullptr && !gauge->mayRaiseDefinedFailure &&
             !copied->mayRaiseDefinedFailure &&
             !leading->mayRaiseDefinedFailure &&
             checked->mayRaiseDefinedFailure,
         "a scalar move is failure-free; the division keeps its failure");
  if (gauge == nullptr || copied == nullptr || leading == nullptr ||
      checked == nullptr) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const auto address = [](const lang::MirConstructorInstance &constructor) {
    return lang::MirBodyAddress{.kind = lang::MirBodyKind::Constructor,
                                .owner = constructor.id};
  };
  for (const lang::MirConstructorInstance *staged : {gauge, copied}) {
    expect(!lang::cppMirOwnedParameterFieldBindings(frontend.mir, *staged) &&
               lang::cppMirOrdinaryBodyStagedParameterMoves(frontend.mir,
                                                            *staged) &&
               emitter.supportsBodyText(address(*staged)) &&
               !emitter.supportsFailureBodyText(address(*staged)),
           "a move after earlier initializer work publishes in the ordinary "
           "body instead of the member-initializer list");
  }
  const auto leadingMoves =
      lang::cppMirOwnedParameterFieldBindings(frontend.mir, *leading);
  const auto leadingCopies =
      lang::cppMirCopyParameterFieldBindings(frontend.mir, *leading);
  expect(leadingMoves && leadingMoves->size() == 1 &&
             emitter.supportsBodyText(address(*leading)),
         "a leading move keeps its native member-initializer publication");
  expect(leadingCopies && leadingCopies->empty(),
         "a scalar copy after the call stays in the MIR body schedule, as it "
         "did while the moved formal made the constructor failure-capable");
  expect(!lang::cppMirOwnedParameterFieldBindings(frontend.mir, *checked) &&
             emitter.supportsFailureBodyText(address(*checked)),
         "a failure-form copy ahead of its own move publishes in body order");

  std::string text;
  try {
    text = gti_test::emitCpp(frontend).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("plain moved passive fields should emit: ") +
                      error.what());
    return;
  }
  const auto definition = [&](std::string_view head) {
    const std::size_t start = text.find(head);
    const std::size_t end =
        start == std::string::npos ? start : text.find("\n  }\n", start);
    return end == std::string::npos ? std::string()
                                    : text.substr(start, end - start);
  };
  const std::string gaugeText = definition("Gauge::Gauge(std::int32_t");
  const std::string copiedText = definition("Copied::Copied(std::int32_t");
  const std::string checkedText = definition(
      "CopiedChecked::CopiedChecked(::gti_internal::backend::mir_failure_"
      "constructor_tag_v1<");
  const std::size_t call = gaugeText.find("is_odd(");
  const std::size_t move = gaugeText.find(" = std::move(__gti_mir_p_");
  expect(gaugeText.find("tag(::gti_internal::backend::mir_empty_state_v1<") !=
                 std::string::npos &&
             gaugeText.find("std::move(__gti_mir_arg_") == std::string::npos &&
             call != std::string::npos && move != std::string::npos &&
             call < move &&
             text.find("Gauge(::gti_internal::backend::mir_failure_"
                       "constructor_tag_v1<") == std::string::npos,
         "the ordinary constructor moves its formal once, after the call, "
         "and no failure form remains");
  for (const std::string *body : {&copiedText, &checkedText}) {
    const std::size_t shell = body->find(" __gti_mir_p_1 = __gti_mir_arg_0;");
    const std::size_t copy = body->find(" = __gti_mir_p_1;");
    const std::size_t moved = body->find(" = std::move(__gti_mir_p_1);");
    expect(shell != std::string::npos && copy != std::string::npos &&
               moved != std::string::npos && shell < copy && copy < moved &&
               body->find("std::move(__gti_mir_arg_0)") == std::string::npos,
           "a copy ahead of its own move reads the declared formal shell "
           "before the body moves it");
  }

  // Borrowed state and a failure-capable summary each leave this route.
  lang::MirProgram borrowed = frontend.mir;
  lang::MirProgram failing = frontend.mir;
  auto &borrowedConstructors =
      const_cast<std::vector<lang::MirConstructorInstance> &>(
          borrowed.constructorInstances());
  bool forged = false;
  for (lang::MirValue &value : borrowedConstructors[gauge->id - 1].body.values)
    if (value.info.type == lang::SemanticType::Int32 &&
        value.sourceValue == gauge->initializers.back().arguments.front()) {
      value.info.traits.containsBorrowedState = true;
      forged = true;
    }
  const_cast<std::vector<lang::MirConstructorInstance> &>(
      failing.constructorInstances())[gauge->id - 1]
      .mayRaiseDefinedFailure = true;
  expect(forged &&
             !lang::cppMirOrdinaryBodyStagedParameterMoves(
                 borrowed, borrowed.constructorInstances()[gauge->id - 1]) &&
             !lang::cppMirOrdinaryBodyStagedParameterMoves(
                 failing, failing.constructorInstances()[gauge->id - 1]),
         "the ordinary route stages only drop-free moves of failure-free "
         "constructors");
}

void testCopiedFormalAfterEarlierWrite() {
  // The native member-initializer list copies a formal's entry value before
  // the body runs. A copy MIR makes after an earlier initializer wrote the
  // formal therefore stays in body order, from the field's empty state; a
  // copy before the write, or of a formal nothing writes, stays native.
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-copied-formal-after-write.gti", R"(
int32_t bump(mut int32_t& value) {
  value = value + 1;
  return value;
}

struct Point {
  mut int32_t x;
  int32_t y;
  Point(int32_t x, int32_t y) : x(x), y(y) {}
};

int32_t shift(mut Point& point) {
  point.x = point.x + 10;
  return point.x;
}

class Pair {
public:
  int32_t first;
  int32_t untouched;
  int32_t second;
  Pair(mut int32_t seed, int32_t other)
      : first(bump(seed)), untouched(other), second(seed) {}
};

class Early {
public:
  int32_t first;
  int32_t second;
  Early(mut int32_t seed) : first(seed), second(bump(seed)) {}
};

class Line {
public:
  int32_t first;
  Point second;
  Line(mut Point point) : first(shift(point)), second(point) {}
};

int main() {
  Pair pair = Pair(1, 5);
  Early early = Early(1);
  Line line = Line(Point(1, 2));
  return pair.second * 10 + pair.untouched + early.first * 10 + early.second +
         line.second.x - 48;
}
)");
  expect(frontend.canGenerateCode(),
         "the copied-formal-after-write fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *pair = nullptr;
  const lang::MirConstructorInstance *early = nullptr;
  const lang::MirConstructorInstance *line = nullptr;
  std::size_t matches = 0;
  const std::vector<lang::SemanticType> twoIntegers{lang::SemanticType::Int32,
                                                    lang::SemanticType::Int32};
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    const auto &parameters = constructor.parameterTypes;
    if (parameters == twoIntegers && constructor.initializers.size() == 3) {
      pair = &constructor;
      ++matches;
    } else if (parameters.size() == 1 &&
               parameters.front() == lang::SemanticType::Int32 &&
               constructor.initializers.size() == 2) {
      early = &constructor;
      ++matches;
    } else if (parameters.size() == 1 &&
               parameters.front().kind == lang::SemanticType::Class &&
               constructor.initializers.size() == 2) {
      line = &constructor;
      ++matches;
    }
  }
  expect(matches == 3 && pair != nullptr && early != nullptr && line != nullptr,
         "the fixture keeps exactly its Pair, Early and Line constructors");
  if (matches != 3 || pair == nullptr || early == nullptr || line == nullptr) {
    return;
  }
  const auto copiedInitializers =
      [](const lang::MirProgram &program,
         const lang::MirConstructorInstance &constructor) {
        std::vector<std::size_t> initializers;
        if (const auto bindings =
                lang::cppMirCopyParameterFieldBindings(program, constructor)) {
          for (const auto &binding : *bindings) {
            initializers.push_back(binding.initializer);
          }
        }
        std::sort(initializers.begin(), initializers.end());
        return initializers;
      };
  expect(copiedInitializers(frontend.mir, *pair) ==
                 std::vector<std::size_t>{1} &&
             copiedInitializers(frontend.mir, *early) ==
                 std::vector<std::size_t>{0} &&
             copiedInitializers(frontend.mir, *line).empty(),
         "only copies that no earlier write reaches publish natively");

  // The write decides: read-only access to the formal before the copy leaves
  // its entry value in place, so the same copy is native again.
  {
    lang::MirProgram mutated = frontend.mir;
    bool forged = false;
    for (const auto &constructor : mutated.constructorInstances()) {
      if (constructor.id != pair->id) {
        continue;
      }
      auto &changed = const_cast<lang::MirConstructorInstance &>(constructor);
      for (lang::MirBlock &block : changed.body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          for (lang::MirOperand &operand : instruction.operands) {
            if (operand.kind == lang::MirOperandKind::BorrowWrite) {
              operand.kind = lang::MirOperandKind::BorrowRead;
              forged = true;
            }
          }
        }
      }
      expect(forged && copiedInitializers(mutated, constructor) ==
                           std::vector<std::size_t>{1, 2},
             "a copy after read-only access to its formal stays native");
    }
  }

  // The body-ordered class copy keeps the native copy's ownership contract:
  // both forms admit the exact Read, and a drifted Read fails closed.
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::MirBodyAddress lineAddress{.kind = lang::MirBodyKind::Constructor,
                                         .owner = line->id};
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
                 .supportsBodyText(lineAddress) &&
             lang::CppMirBodyEmitter(frontend.mir, map)
                 .supportsFailureBodyText(lineAddress),
         "both constructor forms admit the body-ordered class copy");
  {
    lang::MirProgram mutated = frontend.mir;
    bool forged = false;
    for (const auto &constructor : mutated.constructorInstances()) {
      if (constructor.id != line->id) {
        continue;
      }
      auto &changed = const_cast<lang::MirConstructorInstance &>(constructor);
      for (lang::MirBlock &block : changed.body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Load &&
              instruction.info.type.kind == lang::SemanticType::Class &&
              instruction.ownership) {
            instruction.ownership->reachable = false;
            forged = true;
          }
        }
      }
    }
    const lang::CppMirBodyEmitter emitter(mutated, map);
    expect(forged && !emitter.supportsBodyText(lineAddress) &&
               !emitter.supportsFailureBodyText(lineAddress),
           "a body-ordered class copy fails closed when its Read drifts");
  }

  std::string text;
  try {
    text = gti_test::emitCpp(frontend).contents;
  } catch (const std::exception &error) {
    expect(false, std::string("copies after a formal write should emit: ") +
                      error.what());
    return;
  }
  const auto forms = [&](std::string_view owner) {
    std::vector<std::string> bodies;
    const std::string signature =
        std::string(owner) + "::" + std::string(owner) + "(";
    for (std::size_t start = text.find(signature); start != std::string::npos;
         start = text.find(signature, start + signature.size())) {
      const std::size_t end = text.find("\n  }\n", start);
      if (end == std::string::npos) {
        break;
      }
      bodies.push_back(text.substr(start, end - start));
    }
    return bodies;
  };
  // The statement storing `second` through the field alias the body declares.
  const auto publication = [](const std::string &body) {
    const std::size_t alias = body.find(" = (*this).second;");
    const std::size_t name = alias == std::string::npos
                                 ? std::string::npos
                                 : body.rfind("auto &", alias);
    const std::size_t stored =
        name == std::string::npos
            ? std::string::npos
            : body.find(body.substr(name + 6, alias - name - 6) + " = ", alias);
    return stored == std::string::npos
               ? std::pair<std::size_t, std::string>{stored, {}}
               : std::pair<std::size_t, std::string>{
                     stored,
                     body.substr(stored, body.find('\n', stored) - stored)};
  };
  const std::string emptySecond =
      "second(::gti_internal::backend::mir_empty_state_v1<std::remove_cv_t<"
      "decltype(second)>>())";
  const std::vector<std::string> pairForms = forms("Pair");
  const std::vector<std::string> lineForms = forms("Line");
  expect(!pairForms.empty() && !lineForms.empty(),
         "both constructors should emit at least one form");
  for (const std::string &body : pairForms) {
    const std::size_t call = body.find("bump");
    const auto [stored, statement] = publication(body);
    expect(body.find("untouched(__gti_mir_arg_1)") != std::string::npos &&
               body.find(emptySecond) != std::string::npos &&
               body.find("second(__gti_mir_arg_") == std::string::npos &&
               call != std::string::npos && stored != std::string::npos &&
               call < stored &&
               statement.find("__gti_mir_arg_") == std::string::npos,
           "a scalar copy after the write publishes in body order");
  }
  for (const std::string &body : lineForms) {
    const std::size_t call = body.find("shift");
    const std::size_t copied = body.find("auto __gti_mir_v_");
    const auto [stored, statement] = publication(body);
    expect(body.find(emptySecond) != std::string::npos &&
               body.find("second(__gti_mir_arg_") == std::string::npos &&
               call != std::string::npos && copied != std::string::npos &&
               stored != std::string::npos && call < copied &&
               copied < stored &&
               statement.find("std::move(__gti_mir_v_") != std::string::npos,
           "a class copy after the write copies into a transient and then "
           "into the empty field, in body order");
  }
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

  // value_or() and value() are source members of expected: each observer
  // borrows its receiver through one staged read-borrow CallInput.
  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirInstruction *valueOr = nullptr;
  const lang::MirInstruction *voidExtraction = nullptr;
  const auto member = [&](const lang::MirInstruction &instruction,
                          std::string_view name) {
    const lang::HirFunctionInstance *target =
        instruction.kind == lang::MirInstructionKind::Call &&
                instruction.functionTarget
            ? frontend.hir.findFunctionInstance(*instruction.functionTarget)
            : nullptr;
    return target != nullptr && target->source != nullptr &&
           target->source->name().lexeme == name;
  };
  const auto stagedReceiver = [](const lang::MirBody &body,
                                 const lang::MirInstruction &call) {
    const lang::MirOperand *staged = nullptr;
    for (const lang::MirBlock &block : body.blocks)
      for (const lang::MirInstruction &input : block.instructions)
        if (call.receiver &&
            input.kind == lang::MirInstructionKind::CallInput &&
            input.result == call.receiver->value && input.operands.size() == 1)
          staged = &input.operands.front();
    return staged;
  };
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    entry = &function;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (member(instruction, "value_or")) {
          valueOr = &instruction;
        }
        if (member(instruction, "value") &&
            instruction.info.type == lang::SemanticType::Void) {
          voidExtraction = &instruction;
        }
      }
    }
  }
  const lang::MirOperand *valueOrReceiver =
      entry == nullptr || valueOr == nullptr
          ? nullptr
          : stagedReceiver(entry->body, *valueOr);
  const lang::MirOperand *voidReceiver =
      entry == nullptr || voidExtraction == nullptr
          ? nullptr
          : stagedReceiver(entry->body, *voidExtraction);
  expect(entry != nullptr && valueOr != nullptr && valueOrReceiver &&
             voidExtraction != nullptr && voidReceiver &&
             !voidExtraction->result &&
             valueOrReceiver->kind == lang::MirOperandKind::BorrowRead &&
             valueOrReceiver->place != 0 &&
             voidReceiver->kind == lang::MirOperandKind::BorrowRead &&
             voidReceiver->place != 0,
         "Expected observers should retain their exact read-borrowed receiver "
         "and void value() should remain resultless");
  if (entry == nullptr || valueOr == nullptr || valueOrReceiver == nullptr ||
      voidExtraction == nullptr || voidReceiver == nullptr) {
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
  // Each observer calls its member directly on the live binding; void
  // value() publishes only its failure status.
  const auto line = [&](std::string_view needle) {
    const auto at = text.text.find(needle);
    if (at == std::string::npos)
      return std::string();
    const auto begin = text.text.rfind('\n', at);
    const auto end = text.text.find('\n', at);
    return text.text.substr(begin == std::string::npos ? 0 : begin + 1,
                            end - (begin == std::string::npos ? 0 : begin + 1));
  };
  const std::string valueOrLine = line("_value_or(");
  const std::string voidLine = line("_value__gti_mir_failure(");
  expect(
      valueOrLine.find("__gti_mir_p_" + std::to_string(valueOrReceiver->place) +
                       ".get().") != std::string::npos &&
          voidLine.find("__gti_mir_p_" + std::to_string(voidReceiver->place) +
                        ".get().") != std::string::npos &&
          voidLine.find("__gti_mir_v_") == std::string::npos,
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

// A class row's special-member facts are its application's, not its generic
// declaration's. Both declarations copy and default-construct for their open
// primary; over a move-only payload without a default constructor, each
// application deletes its copies, and box<Tracked> also loses the default
// constructor its requirements exclude. The rows must agree with the emitted
// concrete classes.
void testGenericInstanceSpecialMemberRows() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-generic-instance-rows.gti", R"(
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
  int get() { return 1; }
};

class box<T> {
  mut T item;
public:
  box() requires std::default_initializable<T> : item(T()) {}
  box(T value) : item(std::move(value)) {}
  int get() { return 1; }
};

int main() {
  maybe<Tracked> owned = maybe<Tracked>(Tracked(3));
  maybe<Tracked> vacant = maybe<Tracked>();
  maybe<int> scalar = maybe<int>();
  box<Tracked> boxed = box<Tracked>(Tracked(5));
  box<int> zero = box<int>();
  return owned.get() + vacant.get() + scalar.get() + boxed.get() + zero.get() -
         5;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic special-member row fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  const lang::CppMirBodyEmissionMapRows rows =
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
  const std::string text = gti_test::emitCpp(frontend).contents;
  struct Expectation {
    std::string_view name;
    bool tracked;
    bool defaultConstructible;
    bool copyable;
  };
  for (const Expectation &expected : {Expectation{"maybe", true, true, false},
                                      Expectation{"maybe", false, true, true},
                                      Expectation{"box", true, false, false},
                                      Expectation{"box", false, true, true}}) {
    const std::string label = std::string(expected.name) + "<" +
                              (expected.tracked ? "Tracked" : "int") + ">";
    const auto instance = std::find_if(
        frontend.mir.classInstances().begin(),
        frontend.mir.classInstances().end(),
        [&](const lang::MirClassInstance &candidate) {
          const lang::LoweredClassDeclaration *declaration =
              lowered.findClassDeclaration(candidate.declaration);
          return declaration != nullptr &&
                 declaration->qualifiedName == expected.name &&
                 candidate.type.arguments.size() == 1 &&
                 (expected.tracked ? candidate.type.arguments.front().kind ==
                                         lang::SemanticType::Class
                                   : candidate.type.arguments.front() ==
                                         lang::SemanticType::Int32);
        });
    expect(instance != frontend.mir.classInstances().end(),
           label + " should have one sealed MIR instance");
    if (instance == frontend.mir.classInstances().end()) {
      continue;
    }
    // The declaration's statuses describe the open primary for both
    // applications; only the instance distinguishes them.
    const lang::LoweredClassDeclaration *declaration =
        lowered.findClassDeclaration(instance->declaration);
    expect(declaration != nullptr &&
               declaration->defaultConstructor ==
                   lang::SpecialMemberStatus::Declared &&
               declaration->copyConstructor ==
                   lang::SpecialMemberStatus::Generated &&
               declaration->copyAssignment ==
                   lang::SpecialMemberStatus::Generated,
           label + "'s declaration should keep its open-primary statuses");
    const auto row =
        std::find_if(rows.types.begin(), rows.types.end(),
                     [&](const lang::CppMirTypeRepresentation &candidate) {
                       return candidate.type == instance->type;
                     });
    expect(row != rows.types.end() && row->concreteGenericClass,
           label + " should have a direct concrete class row");
    if (row == rows.types.end()) {
      continue;
    }
    expect(row->boundaryConstructible == expected.defaultConstructible &&
               row->copyConstructible == expected.copyable &&
               row->copyable == expected.copyable,
           label + "'s row should carry its own instance's special members");

    // The emitted concrete class spells the same lifecycle. Every default-
    // constructible application is default-constructed by the program, so
    // its class declares that constructor.
    const std::string name =
        row->spelling.substr(row->spelling.rfind("::") + 2);
    expect(text.find(name + "(const " + name + " &other) = " +
                     (expected.copyable ? "default;" : "delete;")) !=
               std::string::npos,
           label + "'s emitted copy constructor should match its row");
    expect(text.find(" " + name + "()" +
                     (expected.defaultConstructible ? ";" : " = delete;")) !=
               std::string::npos,
           label + "'s emitted default constructor should match its row");
  }
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

// An Expected<Class, E> call result consumed by one by-value parameter is an
// ordinary class result: cleanup-free, scalar-payload and cleanup-owning
// results alike are staged in their prepared-parameter slot, which the
// producer publishes into, the call moves from, and the call retires.
// (Regression: the passive shape raised GTI-B0001 unless its payload was an
// indirectly transported C ABI record.)
void testCleanupFreeExpectedClassArgumentSlot() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-class-argument.gti", R"(
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
  const lang::MirFunctionInstance *entry = namedFunction(frontend, "main");
  expect(entry != nullptr, "the Expected class argument fixture has main");
  if (entry == nullptr) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *entry);
  expect(stagedExpectedArguments(
             frontend, entry->body, text,
             "the producer should publish into the slot on success, the call "
             "should move its wrapper, and the slot should retire "
             "afterwards") == 3,
         "cleanup-free, scalar-payload and cleanup-owning Expected arguments "
         "are each staged once");
}

// The same Expected<Class, E> result may initialize one by-value parameter of
// an exact ordinary constructor. expected is an ordinary class: the failure-
// form construction and a constructor body's plain form both take the staged
// argument out of its prepared-parameter slot, which retires it at once, for
// a cleanup-free payload and a cleanup-owning payload alike.
// (Regression: a constructor consumer raised GTI-B0001.)
void testCleanupFreeExpectedClassConstructorArgumentSlot() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-expected-class-constructor-argument.gti", R"(
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
  // Counts this body's prepared expected arguments that a construction takes
  // out of their staged slot.
  const auto takenArguments = [&](const lang::MirBody &body,
                                  const std::string &text) {
    std::size_t taken = 0;
    for (const lang::MirBlock &block : body.blocks)
      for (const lang::MirInstruction &input : block.instructions)
        if (input.kind == lang::MirInstructionKind::CallInput &&
            input.destination && input.preparedParameterDrop &&
            isExpectedClass(frontend, input.info.type) &&
            text.find("std::move(__gti_mir_p_" +
                      std::to_string(*input.destination) + ".take())") !=
                std::string::npos)
          ++taken;
    return taken;
  };
  const lang::MirFunctionInstance *entry = namedFunction(frontend, "main");
  const lang::MirConstructorInstance *nested = nullptr;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *source = frontend.hir.findConstructorInstance(constructor.id);
    for (const auto &owner : frontend.hir.classInstances())
      if (source != nullptr && owner.id == source->owner &&
          owner.source != nullptr && owner.source->name().lexeme == "outer")
        nested = &constructor;
  }
  expect(entry != nullptr && nested != nullptr,
         "the constructor argument fixture has main and outer's constructor");
  if (entry == nullptr || nested == nullptr) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress nestedAddress{lang::MirBodyKind::Constructor,
                                           nested->id};
  const std::string failureForm =
      emitter.emitFailureBodyText(nestedAddress, "selected", 1).text;
  const std::string plainForm =
      emitter.emitBodyText(nestedAddress, "selected", 1).text;
  expect(takenArguments(entry->body, selectedBodyText(frontend, *entry)) == 2 &&
             takenArguments(nested->body, failureForm) == 1,
         "the entry and the constructor body's failure form should each move "
         "a staged wrapper into the raising constructor and retire it "
         "afterwards");
  expect(takenArguments(nested->body, plainForm) == 1,
         "the constructor body's plain form should construct its binding from "
         "the staged wrapper and retire it afterwards");
}

// A named Expected<Class, E> binding copied into one by-value function or
// constructor parameter constructs MIR's copy in its own prepared-parameter
// slot. The consumer moves the copy, never the binding, and the copy's slot
// retires after the call or construction; expected is an ordinary class, so
// a scalar-payload copy is staged the same way.
// (Regression: the copied binding raised GTI-B0001 in the caller.)
void testCleanupFreeExpectedClassCopiedArgumentSlot() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-expected-class-copied-argument.gti", R"(
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
  const lang::MirFunctionInstance *entry = namedFunction(frontend, "main");
  expect(entry != nullptr, "the copied-argument fixture has main");
  if (entry == nullptr) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *entry);
  std::size_t copies = 0;
  for (const lang::MirBlock &block : entry->body.blocks)
    for (const lang::MirInstruction &input : block.instructions) {
      if (input.kind != lang::MirInstructionKind::CallInput ||
          input.callInputKind != lang::HirCallInputKind::CopyValue ||
          !input.destination || !input.preparedParameterDrop ||
          input.operands.size() != 1 ||
          !isExpectedClass(frontend, input.info.type))
        continue;
      const lang::MirPlace *source =
          entry->body.findPlace(input.operands.front().place);
      const lang::MirPlace *binding = nullptr;
      for (const lang::MirPlace &place : entry->body.places)
        if (source != nullptr && binding == nullptr &&
            place.root == lang::MirPlaceRootKind::Binding &&
            place.binding == source->binding && place.projections.empty())
          binding = &place;
      if (binding == nullptr)
        continue;
      ++copies;
      const std::string slot =
          "__gti_mir_p_" + std::to_string(*input.destination);
      const std::string owner = "__gti_mir_p_" + std::to_string(binding->id);
      const auto copied = text.find(slot + ".construct(" + owner + ".get());");
      const auto moved = copied == std::string::npos
                             ? std::string::npos
                             : text.find("std::move(" + slot + ".", copied);
      expect(moved != std::string::npos &&
                 (text.compare(moved + 10 + slot.size(), 8, ".take())") == 0 ||
                  text.find(slot + ".destroy();", moved) != std::string::npos),
             "the consumer should move the copy staged from the named binding "
             "and retire its slot afterwards");
      expect(text.find("std::move(" + owner + ".get())") == std::string::npos &&
                 text.find("std::move(" + owner + ".take())") ==
                     std::string::npos,
             "a copied binding should never be moved into the parameter");
    }
  expect(copies == 3,
         "the call, the construction and the scalar-payload call each copy "
         "their named binding");
}

// An Expected<Class, E> call result that replaces a whole mutable binding is
// staged in its call result slot. A cleanup-free binding needs no replacement
// cleanup, so the Assign move-assigns the staged value into the binding and
// retires the moved-from slot right after it; expected is an ordinary class,
// so a scalar-payload replacement is staged the same way. (Regression for
// issue #224: the replacing call result raised GTI-B0001 in the caller.)
void testCleanupFreeExpectedClassAssignmentSlot() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-class-assignment.gti", R"(
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
  const lang::MirFunctionInstance *entry = namedFunction(frontend, "main");
  expect(entry != nullptr, "the Expected class assignment fixture has main");
  if (entry == nullptr) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *entry);
  std::size_t replacements = 0;
  for (const lang::MirBlock &block : entry->body.blocks)
    for (const lang::MirInstruction &assign : block.instructions) {
      if (assign.kind != lang::MirInstructionKind::Assign ||
          !assign.destination || assign.operands.size() != 1 ||
          !isExpectedClass(frontend, assign.info.type))
        continue;
      ++replacements;
      const std::string slot =
          "__gti_mir_v_" + std::to_string(assign.operands.front().value);
      const std::string assignment =
          "::gti_internal::backend::assign_representation(__gti_mir_p_" +
          std::to_string(*assign.destination) + ".get(), std::move(" + slot +
          ".get()));";
      const auto published = text.find(slot + ".construction_address()");
      const auto assigned = text.find(assignment);
      const auto next =
          assigned == std::string::npos
              ? std::string::npos
              : text.find_first_not_of(' ', text.find('\n', assigned) + 1);
      expect(text.find(", true> " + slot + ";") != std::string::npos &&
                 published != std::string::npos &&
                 assigned != std::string::npos && published < assigned &&
                 text.compare(next, slot.size() + 11, slot + ".destroy();") ==
                     0,
             "the producer should publish into the slot, the Assign should "
             "move it into the binding, and the slot should retire right "
             "after it");
    }
  expect(replacements == 2,
         "the class-payload and scalar-payload replacements are both staged");
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
  const lang::MirFunctionInstance *entry = namedFunction(frontend, "main");
  const lang::MirInstruction *move = nullptr;
  const lang::MirInstruction *initialize = nullptr;
  if (entry != nullptr)
    for (const lang::MirBlock &block : entry->body.blocks)
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index)
        if (block.instructions[index].kind == lang::MirInstructionKind::Move &&
            isExpectedClass(frontend, block.instructions[index].info.type) &&
            block.instructions[index + 1].kind ==
                lang::MirInstructionKind::Initialize &&
            block.instructions[index + 1].destination) {
          move = &block.instructions[index];
          initialize = &block.instructions[index + 1];
        }
  expect(move != nullptr && initialize != nullptr && move->operands.size() == 1,
         "the moved binding's Initialize should only activate its drop");
  if (move == nullptr || initialize == nullptr || move->operands.size() != 1) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *entry);
  const std::string destination =
      "__gti_mir_p_" + std::to_string(*initialize->destination);
  const std::string marker = "// GTI MIR reparent into p" +
                             std::to_string(*initialize->destination) + "\n";
  const std::size_t initialized = text.find(marker);
  const std::size_t slotEnd = text.find("> " + destination + ";");
  const std::size_t slotStart = slotEnd == std::string::npos
                                    ? slotEnd
                                    : text.rfind("mir_lifetime_slot<", slotEnd);
  const std::size_t constructed =
      initialized == std::string::npos
          ? initialized
          : text.rfind(destination + ".construct(std::move(__gti_mir_p_" +
                           std::to_string(move->operands.front().place) +
                           ".get()));",
                       initialized);
  expect(initialized != std::string::npos && slotStart != std::string::npos &&
             constructed != std::string::npos && slotEnd < constructed &&
             text.find('\n', constructed) == text.rfind('\n', initialized),
         "the Move should construct the new binding's lifetime slot from the "
         "moved source immediately before its Initialize");
  if (slotStart == std::string::npos) {
    return;
  }
  const std::string type =
      text.substr(slotStart + 18, slotEnd - (slotStart + 18));
  expect(text.find(type + " __gti_mir_v_") == std::string::npos,
         "no Expected should value-initialize an ordinary local");
}

// Copying a cleanup-free Expected<Class, E> binding into a new binding
// copy-constructs the new binding's lifetime slot from the source when the
// Load publishes at its consuming Initialize; nothing else initializes the
// binding. The success payload has no default constructor, so no
// value-initialized Expected local may exist. (Regression: the copy was
// outside the C++ text vocabulary and reported GTI-B0001.)
void testCleanupFreeExpectedClassCopySlot() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-class-copy.gti", R"(
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
  const lang::MirFunctionInstance *entry = namedFunction(frontend, "main");
  const lang::MirInstruction *load = nullptr;
  const lang::MirInstruction *initialize = nullptr;
  if (entry != nullptr)
    for (const lang::MirBlock &block : entry->body.blocks)
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index)
        if (block.instructions[index].kind == lang::MirInstructionKind::Load &&
            isExpectedClass(frontend, block.instructions[index].info.type) &&
            block.instructions[index + 1].kind ==
                lang::MirInstructionKind::Initialize &&
            block.instructions[index + 1].destination) {
          load = &block.instructions[index];
          initialize = &block.instructions[index + 1];
        }
  expect(load != nullptr && initialize != nullptr &&
             load->operands.size() == 1 && load->result,
         "the copied binding's Initialize should only activate its drop");
  if (load == nullptr || initialize == nullptr || load->operands.size() != 1 ||
      !load->result) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *entry);
  const std::string destination =
      "__gti_mir_p_" + std::to_string(*initialize->destination);
  const std::string marker = "// class copy load " +
                             std::to_string(*load->result) +
                             " publishes at its consuming initialize\n";
  const std::size_t published = text.find(marker);
  const std::size_t next =
      published == std::string::npos
          ? published
          : text.find_first_not_of(' ', published + marker.size());
  const std::string copy = destination + ".construct(__gti_mir_p_" +
                           std::to_string(load->operands.front().place) +
                           ".get());";
  const std::size_t slotEnd = text.find("> " + destination + ";");
  const std::size_t slotStart = slotEnd == std::string::npos
                                    ? slotEnd
                                    : text.rfind("mir_lifetime_slot<", slotEnd);
  expect(next != std::string::npos && slotStart != std::string::npos &&
             slotEnd < published && text.compare(next, copy.size(), copy) == 0,
         "the Load should copy-construct the new binding's lifetime slot "
         "from the source immediately before its Initialize");
  if (slotStart == std::string::npos) {
    return;
  }
  const std::string type =
      text.substr(slotStart + 18, slotEnd - (slotStart + 18));
  expect(text.find(type + " __gti_mir_v_") == std::string::npos,
         "no Expected should value-initialize an ordinary local");
}

// Every other source replacing a whole Expected<Class, E> binding is an
// ordinary class replacement or a source operator= call, with no
// value-initialized Expected local. A cleanup-free copy and move assign the
// binding directly; an Unexpected and a payload each call expected's
// operator= on the binding with their staged argument and retire it; an
// owning call result moves out of its slot after the active replacement
// cleanup, and that slot retires only the moved-from representation.
// (Regression for issue #225: copies, Unexpected values and payloads raised
// GTI-B0001, a move assigned to the lifetime slot itself, and owning results
// raised GTI-B0001.)
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
          // value()/error() are source accessors of expected.
          const auto *target =
              call.kind == lang::MirInstructionKind::Call && call.functionTarget
                  ? frontend.hir.findFunctionInstance(*call.functionTarget)
                  : nullptr;
          const auto *receiver =
              call.receiver ? function.body.findValue(call.receiver->value)
                            : nullptr;
          const bool extraction =
              name != "getter_owned_field_replacement" && target != nullptr &&
              target->source != nullptr && receiver != nullptr &&
              isExpectedClass(frontend, receiver->info.type) &&
              (target->source->name().lexeme == "value" ||
               target->source->name().lexeme == "error");
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
  const lang::MirFunctionInstance *entry = namedFunction(frontend, "main");
  expect(entry != nullptr, "the replacement fixture has main");
  if (entry == nullptr) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *entry);
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
  // An owning replacement directly follows its active replacement cleanup;
  // a cleanup-free one has none to run.
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
    if (!starts(line, "__gti_mir_p_"))
      continue;
    if (line.find(".get() = __gti_mir_p_") != std::string::npos &&
        ends(line, ".get();") && !afterCleanup(index))
      ++copies;
    const std::size_t moved = line.find(".get() = std::move(__gti_mir_p_");
    if (moved != std::string::npos && ends(line, ".get());")) {
      const std::size_t idStart = moved + 19;
      const std::string slot =
          line.substr(idStart, line.find(".get())", idStart) - idStart);
      if (afterCleanup(index) && index + 1 < lines.size() &&
          lines[index + 1] == slot + ".destroy();")
        ++owningMoves;
      else if (!afterCleanup(index))
        ++moves;
    }
    // expected's source operator= consumes its staged argument.
    const std::string assignment = "_expected::operator=(std::move(";
    const std::size_t assigned = line.find(assignment);
    if (assigned != std::string::npos && ends(line, ".get()));")) {
      const std::size_t idStart = assigned + assignment.size();
      const std::string slot =
          line.substr(idStart, line.find(".get())", idStart) - idStart);
      if (index + 1 >= lines.size() || lines[index + 1] != slot + ".destroy();")
        continue;
      if (stagedByUnexpected(text, slot))
        ++unexpecteds;
      else if (text.find(slot + ".construct(") != std::string::npos)
        ++payloads;
    }
  }
  // Moving a scalar error cannot fail, so unexpected(int32_t) and its
  // unexpected_value constructor have only their plain forms.
  expect(text.find("_unexpected__gti_mir_failure(") == std::string::npos,
         "a scalar error should reach unexpected() through its plain form");
  expect(copies == 1 && moves == 1 && unexpecteds == 1 && payloads == 1,
         "a cleanup-free copy and move should assign the binding directly, "
         "and an Unexpected and a payload should each replace it through "
         "expected's operator= and retire their staged argument");
  expect(owningMoves == 1 &&
             text.find("__gti_mir_unique_cleanup_failure") != std::string::npos,
         "an owning call result should move out of its slot after the active "
         "replacement cleanup and retire the moved-from slot");
  expect(text.find("_expected __gti_mir_v_") == std::string::npos,
         "no replacement source should declare a value-initialized Expected "
         "local");
}

// An owning Expected<Class, E> binding replaced by a move keeps MIR's active
// replacement cleanup and retires the moved binding's staged shell exactly
// once. A payload or an Unexpected replaces a binding through expected's
// source operator=, which takes the staged payload or the unexpected() result
// and retires it. A fresh class error is constructed in its own slot, moved
// into unexpected() and retired there, whether the result initializes or
// replaces a binding. (Regression for issue #227: these raised GTI-B0001, the
// payload conversion while constructing MIR.)
void testShortCircuitExpectedPayloadFieldView() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-short-circuit-expected-payload.gti", R"(
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
  const lang::MirFunctionInstance *checker = namedFunction(frontend, "check");
  expect(checker != nullptr, "the short-circuit fixture has its checker");
  if (checker == nullptr) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *checker);
  // The consuming value() accessor moves the payload out of the expected
  // result's conditional temporary into its own, and the field is read
  // there.
  const std::size_t extracted = text.find("_value__gti_mir_failure(");
  const std::size_t lineStart =
      extracted == std::string::npos ? extracted : text.rfind('\n', extracted);
  const std::size_t lineEnd =
      extracted == std::string::npos ? extracted : text.find('\n', extracted);
  const std::size_t target = extracted == std::string::npos
                                 ? std::string::npos
                                 : text.find(", __gti_mir_p_", extracted);
  const std::size_t nameStart = target == std::string::npos || target > lineEnd
                                    ? std::string::npos
                                    : target + 2;
  const std::string payload =
      nameStart == std::string::npos
          ? std::string()
          : text.substr(nameStart,
                        text.find(".construction_address()", nameStart) -
                            nameStart);
  expect(!payload.empty() &&
             text.substr(lineStart, extracted - lineStart)
                     .find("std::move(__gti_mir_p_") != std::string::npos &&
             text.find(" = " + payload + ".get().number;", extracted) !=
                 std::string::npos,
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
  const lang::MirFunctionInstance *entry = namedFunction(frontend, "main");
  expect(entry != nullptr, "the owning replacement fixture has main");
  if (entry == nullptr) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *entry);
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
  std::size_t transferredPayloads = 0;
  std::size_t unexpectedReplacements = 0;
  for (std::size_t index = 0; index + 1 < lines.size(); ++index) {
    const std::string &line = lines[index];
    const std::size_t moved = line.find(".get() = std::move(__gti_mir_p_");
    if (moved != std::string::npos && contains(line, ".get());")) {
      const std::size_t idStart = moved + 19;
      const std::string slot =
          line.substr(idStart, line.find(".get())", idStart) - idStart);
      if (lines[index + 1] == slot + ".destroy();" &&
          text.find(slot + ".construct(std::move(__gti_mir_p_") !=
              std::string::npos &&
          index > 0 &&
          contains(lines[index - 1], "__gti_mir_replacement_success_"))
        ++movedShells;
    }
    // expected's source operator= takes a staged payload directly.
    if (contains(line, "_expected::operator=(__gti_mir_p_") &&
        contains(line, ".take());"))
      ++transferredPayloads;
    // ... or consumes an unexpected() result staged in its own slot.
    const std::string assignment = "_expected::operator=(std::move(";
    const std::size_t assigned = line.find(assignment);
    if (assigned != std::string::npos) {
      const std::size_t idStart = assigned + assignment.size();
      const std::string slot =
          line.substr(idStart, line.find(".get())", idStart) - idStart);
      if (lines[index + 1] == slot + ".destroy();" &&
          stagedByUnexpected(text, slot))
        ++unexpectedReplacements;
    }
  }
  expect(movedShells == 1,
         "an owning move should stage in its slot, move into the binding and "
         "retire the moved-from shell");
  expect(transferredPayloads == 1,
         "an owning payload should move into the binding and end its "
         "transferred-out shell at the Assign");
  expect(unexpectedReplacements == 3,
         "an owning binding should accept an inline Unexpected replacement");
  // A fresh class error is constructed in its slot, moved into unexpected()
  // and retired right after that call.
  std::size_t stagedErrors = 0;
  for (std::size_t index = 0; index + 1 < lines.size(); ++index) {
    const std::string marker = "_unexpected__gti_mir_failure(std::move(";
    const std::size_t staged = lines[index].find(marker);
    if (staged == std::string::npos)
      continue;
    const std::size_t idStart = staged + marker.size();
    const std::string slot = lines[index].substr(
        idStart, lines[index].find(".get())", idStart) - idStart);
    bool constructed = false;
    for (std::size_t before = index; before > 0 && !constructed; --before)
      constructed = lines[before - 1].compare(0, slot.size() + 11,
                                              slot + ".construct(") == 0;
    bool retired = false;
    for (std::size_t after = index + 1;
         after < lines.size() && !retired &&
         !contains(lines[after], "_unexpected__gti_mir_failure(");
         ++after)
      retired = lines[after] == slot + ".destroy();";
    stagedErrors += constructed && retired;
  }
  expect(stagedErrors == 3,
         "a fresh class error replacing an owning binding should be "
         "constructed in its slot at its MIR position, moved into the "
         "Unexpected at the Assign and retired there");
  expect(text.find("_expected __gti_mir_v_") == std::string::npos,
         "no owning replacement should declare a value-initialized Expected "
         "local");
}

// An owning payload enum replaced through the complete referent of a checked
// mutable reference (a call's reference result, a reference local or a
// reference parameter) drains the old alternative once in its replacement
// cleanup, move-assigns the staged value into that same referent and retires
// the moved-from shell, in both the failure and the plain form. The
// referent's owner keeps the lexical drop, so the borrowed Assign names no
// local Replace target.
void testOwningPayloadEnumReferentReplacement() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-owning-payload-enum-referent-replacement.gti", R"gti(
#include <std/string>

enum class label { text(std::string name), blank };

class box {
  mut label item;
public:
  box(label v) : item(std::move(v)) {}
  mut label& ref() mut { return this.item; }
};

void reset(mut label& target) { target = label::blank; }

// Its only cleanup cannot fail, so clear() has just the plain form.
class quiet {
public:
  int32_t id;
  quiet(int32_t id) : id(id) {}
  ~quiet() {}
};

enum class token { held(quiet q), empty };

void clear(mut token& target) { target = token::empty; }

int main() {
  mut token t = token::held(quiet(1));
  clear(t);
  mut box b = box(label::text(std::string("gti")));
  b.ref() = label::blank;
  mut label local = label::text(std::string("x"));
  {
    mut label& r = local;
    r = label::text(std::string("y"));
  }
  reset(local);
  return 0;
}
)gti");
  expect(frontend.canGenerateCode(),
         "the payload-enum referent replacement fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *entry = namedFunction(frontend, "main");
  const lang::MirFunctionInstance *reset = namedFunction(frontend, "reset");
  const lang::MirFunctionInstance *clear = namedFunction(frontend, "clear");
  expect(entry != nullptr && reset != nullptr && clear != nullptr,
         "the referent replacement fixture has main, reset and clear");
  if (entry == nullptr || reset == nullptr || clear == nullptr) {
    return;
  }
  // Each replacement cleanup drains one dereferenced carrier; the success
  // edge move-assigns the staged slot into that exact spelling and retires
  // the slot's shell. Returns the replaced referent spellings.
  const auto referentReplacements = [](const std::string &text) {
    std::vector<std::string> lines;
    for (std::size_t start = 0; start < text.size();) {
      const std::size_t end = std::min(text.find('\n', start), text.size());
      const std::size_t first = text.find_first_not_of(' ', start);
      lines.push_back(first < end ? text.substr(first, end - first) : "");
      start = end + 1;
    }
    std::vector<std::string> replaced;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const std::string &line = lines[index];
      const std::size_t call = line.rfind("}(");
      if ((line.compare(0, 30, "__gti_mir_replacement_success_") != 0 &&
           line.compare(0, 6, "(void)") != 0) ||
          line.find("retire_cleaned_alternative();") == std::string::npos ||
          call == std::string::npos || line.size() < call + 4)
        continue;
      const std::string referent =
          line.substr(call + 2, line.size() - call - 4);
      if (referent.compare(0, 11, "(*__gti_mir") != 0)
        continue;
      const std::string assignment = referent + " = std::move(__gti_mir_p_";
      for (std::size_t after = index + 1; after < lines.size(); ++after) {
        if (lines[after].compare(0, assignment.size(), assignment) != 0)
          continue;
        const std::size_t slotStart = assignment.size() - 12;
        const std::string slot = lines[after].substr(
            slotStart, lines[after].find(".get());", slotStart) - slotStart);
        bool retired = false;
        for (std::size_t shell = after + 1;
             shell < std::min(after + 3, lines.size()) && !retired; ++shell)
          retired = lines[shell] == slot + ".destroy();";
        if (retired)
          replaced.push_back(referent);
        break;
      }
    }
    return replaced;
  };
  const std::string entryText = selectedBodyText(frontend, *entry);
  const std::vector<std::string> entryReplaced =
      referentReplacements(entryText);
  expect(entryReplaced.size() == 2 &&
             entryReplaced.front().find("__gti_mir_loan_") !=
                 std::string::npos &&
             entryReplaced.back().find("__gti_mir_p_") != std::string::npos,
         "a call's mutable reference result and a reference local should "
         "each drain the old alternative once and move the new value into "
         "the same referent");
  const std::vector<std::string> resetReplaced =
      referentReplacements(selectedBodyText(frontend, *reset));
  expect(resetReplaced.size() == 1,
         "a mutable reference parameter's referent should be replaced the "
         "same way");
  const std::string clearText = selectedBodyText(frontend, *clear);
  expect(!clear->mayRaiseDefinedFailure &&
             referentReplacements(clearText).size() == 1 &&
             clearText.find("(void)[&](auto &__gti_cleanup_value)") !=
                 std::string::npos,
         "a non-failing referent replacement should use the plain form");
  for (const std::string &referent : entryReplaced) {
    std::size_t drains = 0;
    for (std::size_t at = entryText.find("}(" + referent + ");");
         at != std::string::npos;
         at = entryText.find("}(" + referent + ");", at + 1))
      ++drains;
    expect(drains == 1, "each borrowed referent's old alternative should be "
                        "drained by exactly one replacement cleanup");
  }

  // A borrowed Assign that claims a local Replace target is not the
  // referent form and must not reach the shared move-assignment spelling.
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::MirBodyAddress address{lang::MirBodyKind::Function, entry->id};
  const auto supports = [&](const lang::MirProgram &program) {
    const lang::CppMirBodyEmitter emitter(program, map);
    return entry->mayRaiseDefinedFailure
               ? emitter.supportsFailureBodyText(address)
               : emitter.supportsBodyText(address);
  };
  expect(supports(frontend.mir),
         "the unmodified referent replacements should be admitted");
  lang::MirProgram mutated = frontend.mir;
  bool forged = false;
  for (const auto &function : mutated.functionInstances()) {
    if (function.id != entry->id)
      continue;
    auto &body = const_cast<lang::MirFunctionInstance &>(function).body;
    for (lang::MirBlock &block : body.blocks)
      for (lang::MirInstruction &instruction : block.instructions)
        if (!forged && instruction.kind == lang::MirInstructionKind::Assign &&
            instruction.replacementPair != 0 && !instruction.ownership &&
            instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().target == 0 &&
            !body.dropObligations.empty()) {
          instruction.lifecycle.front().target =
              body.dropObligations.front().id;
          forged = true;
        }
  }
  expect(forged && !supports(mutated),
         "a borrowed referent replacement naming a local Replace target "
         "should be refused");
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
    const auto *source = frontend.hir.findFunctionInstance(function.id);
    if (source == nullptr ||
        source->sourceUnit != frontend.sourceGraph.entryUnit() ||
        !isExpectedClass(frontend, function.returnType))
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

// A cleanup-free class result needs no obligation, so both arms of its
// conditional publish into one representation-only join. A member call arm
// keeps the receiver identity ordinary call admission proves (its exact
// borrow, or a moved receiver staged as a prepared input), and a call that
// consumes prepared inputs transfers each one out and records nothing else.
// Removed, duplicated or conditional transfers, a forged result activation
// and a forged consuming capability on a borrowed receiver are refused.
void testCleanupFreeConditionalCallArms() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-cleanup-free-conditional-call-arms.gti", R"(
class Pod {
public:
  int x;
  Pod(int v) : x(v) {}
  int read() { return this.x; }
};
class Maker {
  int base;
public:
  Maker(int base) : base(base) {}
  int get() { return this.base; }
  Pod make() && { return Pod(this.base); }
  Pod peek() { return Pod(this.base + 10); }
};
Pod build(Maker maker) { return Pod(maker.get()); }
int bound(bool left, Maker a, Maker b) {
  Pod kept = left ? std::move(a).make() : build(std::move(b));
  return kept.read();
}
Pod returned(bool left, Maker a, Maker b) {
  return left ? build(std::move(a)) : std::move(b).make();
}
int borrowed(bool left, Maker a, Maker b) {
  Pod kept = left ? a.peek() : b.peek();
  return kept.read() + a.get();
}
int main() {
  return bound(true, Maker(1), Maker(2)) +
         returned(false, Maker(3), Maker(4)).read() +
         borrowed(true, Maker(5), Maker(6)) - 25;
}
)");
  expect(frontend.canGenerateCode(),
         "cleanup-free conditional call arms verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  std::size_t transferring = 0;
  std::size_t borrowing = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    // The two arm calls returning the cleanup-free class: each transfers its
    // one prepared input, or borrows its receiver and records nothing.
    std::vector<lang::MirInstructionId> transfers;
    std::vector<lang::MirInstructionId> borrows;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.result ||
            instruction.info.type.kind != lang::SemanticType::Class)
          continue;
        if (instruction.lifecycle.size() == 1 &&
            instruction.lifecycle[0].kind ==
                lang::MirLifecycleEventKind::TransferOut)
          transfers.push_back(instruction.id);
        else if (instruction.receiver && instruction.lifecycle.empty() &&
                 !instruction.callableInvocation)
          borrows.push_back(instruction.id);
      }
    if (transfers.size() + borrows.size() != 2 ||
        (!transfers.empty() && !borrows.empty()))
      continue;
    transferring += transfers.size() == 2;
    borrowing += borrows.size() == 2;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    // A raising callee selects the caller's failure form.
    expect(emitter.analyze(address).ready() &&
               supportsSelectedBodyText(frontend.mir, map, function.id),
           "a cleanup-free conditional admits its member-call and "
           "input-consuming arms");
    const auto refused = [&](auto &&mutate, bool verifierRejects,
                             const std::string &what) {
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(malformed, address);
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          mutate(instruction);
      if (verifierRejects)
        expect(!lang::verifyMirProgram(malformed).valid(),
               "a forged cleanup-free conditional arm fails MIR "
               "verification: " +
                   what);
      const lang::CppMirBodyEmitter rejected(malformed, map);
      expect(!rejected.analyze(address).ready() ||
                 !supportsSelectedBodyText(malformed, map, function.id),
             "a forged cleanup-free conditional arm cannot enter backend "
             "emission: " +
                 what);
    };
    for (const lang::MirInstructionId call : transfers) {
      for (int mutation = 0; mutation != 4; ++mutation) {
        refused(
            [&](lang::MirInstruction &instruction) {
              if (instruction.id != call)
                return;
              const auto transfer = instruction.lifecycle[0];
              if (mutation == 0)
                instruction.lifecycle.clear();
              else if (mutation == 1)
                instruction.lifecycle.push_back(transfer);
              else if (mutation == 2)
                instruction.lifecycle[0].conditional = true;
              else
                instruction.lifecycle.push_back(
                    {.kind = lang::MirLifecycleEventKind::Initialize,
                     .target = transfer.source});
            },
            true, "transfer " + std::to_string(mutation));
      }
    }
    for (const lang::MirInstructionId call : borrows) {
      refused(
          [&](lang::MirInstruction &instruction) {
            if (instruction.id == call)
              instruction.callableInvocation =
                  lang::CallableInvocationCapability::Once;
          },
          false, "consuming capability on a borrowed receiver");
    }
  }
  expect(transferring == 2 && borrowing == 1,
         "the bound and returned transferring conditionals and the borrowed "
         "one were exercised: " +
             std::to_string(transferring) + ", " + std::to_string(borrowing));
}

// A cleanup-free conditional value borrowed as a member call's receiver gets
// a join temporary with a full-expression obligation of its own, which each
// arm's Initialize activates directly. A call arm, plain or on its Invoke
// success edge, and a copied place publish into that temporary as a
// construction arm does; the call borrows it, read-only or mutably, and its
// Drop ends it. So does an arm moving out of a nested cleanup-free join, which
// also ends the nested join's storage. A removed, conditional, duplicated or
// retargeted activation, an arm result that activates the obligation itself,
// and a nested join's storage with an obligation of its own are refused.
void testCleanupFreeJoinReceiver() {
  const auto frontend =
      analyzeWithStandardLibrary("cpp-mir-cleanup-free-join-receiver.gti", R"(
class Pod {
public:
  mut int x;
  Pod(int v) : x(v) {}
  int read() { return this.x; }
  int swap(int v) mut {
    int old = this.x;
    this.x = v;
    return old;
  }
};
class Maker {
  int base;
public:
  Maker(int base) : base(base) {}
  Pod make() && { return Pod(this.base); }
  Pod peek() { return Pod(this.base); }
};
Pod fresh(int id) { return Pod(id); }
Pod risky(int id, int divisor) { return Pod(id / divisor); }
int read_calls(bool left, int a, int b) {
  return (left ? fresh(a) : fresh(b)).read();
}
int swap_copies(bool left, Pod a, Pod b) { return (left ? a : b).swap(0); }
int read_raising(bool left, int a, int b, int divisor) {
  return (left ? risky(a, divisor) : fresh(b)).read();
}
int swap_members(bool left, Maker a, Maker b) {
  return (left ? std::move(a).make() : b.peek()).swap(0);
}
int read_nested(bool left, bool inner, int a, int b) {
  return (left ? fresh(a) : (inner ? fresh(b) : Pod(a))).read();
}
int main() {
  return read_calls(true, 1, 2) + swap_copies(false, Pod(3), Pod(4)) +
         read_raising(true, 5, 6, 1) +
         swap_members(false, Maker(7), Maker(8)) +
         read_nested(false, true, 9, 10) - 28;
}
)");
  expect(frontend.canGenerateCode(),
         "a cleanup-free conditional receiver verifies");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  std::size_t bodies = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    // Each arm's Initialize activates the join temporary's own obligation;
    // the published value is a call result, a copy or the move out of a
    // nested join, with none of its own.
    std::vector<lang::MirInstructionId> arms;
    const lang::MirPlace *nestedJoin = nullptr;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *slot =
            instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.destination &&
                    instruction.lifecycle.size() == 1 &&
                    instruction.operands.size() == 1
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        const auto *drop = slot == nullptr
                               ? nullptr
                               : function.body.findDropObligation(
                                     instruction.lifecycle.front().target);
        const auto *value =
            slot == nullptr
                ? nullptr
                : function.body.findValue(instruction.operands.front().value);
        const lang::MirInstruction *producer = nullptr;
        for (const auto &candidate : function.body.blocks)
          for (const auto &item : candidate.instructions)
            if (value != nullptr && item.id == value->definition)
              producer = &item;
        if (slot != nullptr &&
            slot->root == lang::MirPlaceRootKind::Temporary &&
            instruction.lifecycle.front().kind ==
                lang::MirLifecycleEventKind::Initialize &&
            drop != nullptr && drop->place == slot->id && producer != nullptr &&
            (producer->kind == lang::MirInstructionKind::Call ||
             producer->kind == lang::MirInstructionKind::Load ||
             producer->kind == lang::MirInstructionKind::Move)) {
          arms.push_back(instruction.id);
          if (producer->kind == lang::MirInstructionKind::Move)
            nestedJoin =
                function.body.findPlace(producer->operands.front().place);
        }
      }
    if (arms.size() != 2)
      continue;
    ++bodies;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyze(address).ready() &&
               supportsSelectedBodyText(frontend.mir, map, function.id),
           "a cleanup-free conditional publishes into the receiver's join "
           "temporary");
    for (const lang::MirInstructionId arm : arms) {
      for (int mutation = 0; mutation != 5; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        lang::MirInstruction *initialize = nullptr;
        for (auto &block : body->blocks)
          for (auto &instruction : block.instructions)
            if (instruction.id == arm)
              initialize = &instruction;
        const auto activation = initialize->lifecycle.front();
        if (mutation == 0) {
          initialize->lifecycle.clear();
        } else if (mutation == 1) {
          initialize->lifecycle.front().conditional = true;
        } else if (mutation == 2) {
          initialize->lifecycle.push_back(activation);
        } else if (mutation == 3) {
          // A drop of another place, or none at all.
          initialize->lifecycle.front().target = 0;
          for (const auto &drop : body->dropObligations)
            if (drop.id != activation.target)
              initialize->lifecycle.front().target = drop.id;
        } else {
          const auto *value =
              body->findValue(initialize->operands.front().value);
          for (auto &block : body->blocks)
            for (auto &instruction : block.instructions)
              if (instruction.id == value->definition)
                instruction.lifecycle.push_back(activation);
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "a forged cleanup-free join receiver activation fails MIR "
               "verification " +
                   std::to_string(mutation));
        const lang::CppMirBodyEmitter rejected(malformed, map);
        expect(!rejected.analyze(address).ready() ||
                   !supportsSelectedBodyText(malformed, map, function.id),
               "a forged cleanup-free join receiver activation cannot enter "
               "backend emission " +
                   std::to_string(mutation));
      }
    }
    if (nestedJoin != nullptr) {
      // The nested join's storage gains an obligation of its own, so the
      // arm's move no longer empties representation-only storage.
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(malformed, address);
      lang::MirDropObligation forged = body->dropObligations.front();
      forged.id = 0;
      for (const auto &drop : body->dropObligations)
        forged.id = std::max(forged.id, drop.id);
      ++forged.id;
      forged.place = nestedJoin->id;
      forged.dropType.type = nestedJoin->type;
      body->dropObligations.push_back(forged);
      expect(!lang::verifyMirProgram(malformed).valid(),
             "a nested join storage obligation that nothing activates fails "
             "MIR verification");
      const lang::CppMirBodyEmitter rejected(malformed, map);
      expect(!rejected.analyze(address).ready() ||
                 !supportsSelectedBodyText(malformed, map, function.id),
             "a nested join whose storage has an obligation cannot enter "
             "backend emission as a cleanup-free arm");
    }
  }
  expect(bodies == 5, "the call, copy, raising, member-call and nested "
                      "receivers were exercised: " +
                          std::to_string(bodies));

  // Both arms publish into the temporary the call borrows; nothing
  // value-initializes the class.
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  // The selected form's definition, not its forward declaration.
  const auto bodyText = [&](const std::string &name) {
    for (const std::string form :
         {"(bool __gti_mir_arg_0", "__gti_mir_failure(bool __gti_mir_arg_0"})
      for (std::size_t start = text.find("_" + name + form);
           start != std::string::npos;
           start = text.find("_" + name + form, start + 1)) {
        const std::size_t line = text.find('\n', start);
        if (line != std::string::npos && text[line - 1] == '{')
          return text.substr(start, text.find("\n  }\n", start) - start);
      }
    return std::string();
  };
  for (const std::string name :
       {"read_calls", "swap_copies", "read_raising", "swap_members"}) {
    const std::string body = bodyText(name);
    const std::string receiver = ".get().::__gti_program::Pod::__gti_fn_";
    const std::size_t call = body.find(receiver);
    const std::size_t slotStart =
        call == std::string::npos ? call : body.rfind("__gti_mir_p_", call);
    const std::string slot = slotStart == std::string::npos
                                 ? std::string()
                                 : body.substr(slotStart, call - slotStart);
    std::size_t publications = 0;
    for (const std::string publication :
         {".construct_from(", ".construct(", ".construction_address()"})
      for (std::size_t at = slot.empty() ? std::string::npos
                                         : body.find(slot + publication);
           at != std::string::npos && at < call;
           at = body.find(slot + publication, at + 1))
        ++publications;
    expect(publications == 2 &&
               body.find(slot + ".destroy();", call) != std::string::npos &&
               body.find("::__gti_program::Pod __gti_mir_v_") ==
                   std::string::npos,
           name + " publishes both arms into the borrowed join temporary, "
                  "which its Drop ends");
  }
  // The nested arm, whose block follows the call's, moves the nested join's
  // object into the borrowed temporary and ends the nested storage on the
  // next line.
  const std::string nested = bodyText("read_nested");
  const std::size_t call =
      nested.find(".get().::__gti_program::Pod::__gti_fn_");
  const std::size_t receiverStart =
      call == std::string::npos ? call : nested.rfind("__gti_mir_p_", call);
  const std::string receiver =
      receiverStart == std::string::npos
          ? std::string()
          : nested.substr(receiverStart, call - receiverStart);
  const std::size_t moved =
      receiver.empty()
          ? std::string::npos
          : nested.find(receiver + ".construct(std::move(__gti_mir_p_");
  const std::size_t innerStart =
      moved == std::string::npos
          ? moved
          : moved + receiver.size() +
                std::string(".construct(std::move(").size();
  const std::size_t innerEnd = innerStart == std::string::npos
                                   ? innerStart
                                   : nested.find('.', innerStart);
  const std::string inner =
      innerEnd == std::string::npos
          ? std::string()
          : nested.substr(innerStart, innerEnd - innerStart);
  const std::size_t lineEnd =
      moved == std::string::npos ? moved : nested.find('\n', moved);
  const std::size_t retire =
      inner.empty() ? std::string::npos
                    : nested.find(inner + ".destroy(); // end moved join "
                                          "representation\n",
                                  lineEnd);
  expect(!inner.empty() && inner != receiver &&
             nested.find(receiver + ".construct_from(") != std::string::npos &&
             nested.find(receiver + ".destroy();", call) != std::string::npos &&
             retire != std::string::npos &&
             nested.find_first_not_of(' ', lineEnd + 1) == retire,
         "read_nested publishes both arms into the borrowed join temporary, "
         "which its Drop ends, and ends the nested join's storage right after "
         "moving out of it");
}

// A cleanup-free arm may move a whole named local out. MIR records only the
// local's ownership transition to Moved: the Move has no lifecycle event and
// the local keeps its binding drop, which ends the moved-from shell after the
// join. The bound, returned and field-read joins publish into
// representation-only storage; the receiver join's arm activates the
// temporary's own full-expression obligation directly. A removed, weakened or
// retargeted ownership event, a forged lifecycle event on the Move, forged
// cleanup on the moved-from local and a conditional activation are refused.
// Without the arm proofs' ownership, passive-drop and activation checks, all
// but the lifecycle forgery would reach text emission.
void testCleanupFreeConditionalMovedLocalArms() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-cleanup-free-conditional-moved-local-arms.gti", R"(
class Pod {
public:
  int x;
  Pod(int v) : x(v) {}
  int read() { return this.x; }
};
int bound(bool left, int one, int two) {
  Pod a{one};
  Pod b{two};
  Pod kept = left ? std::move(a) : std::move(b);
  return kept.read();
}
Pod returned(bool left, int one, int two) {
  Pod a{one};
  Pod b{two};
  return left ? std::move(a) : std::move(b);
}
int field(bool left, int one, int two) {
  Pod a{one};
  Pod b{two};
  return (left ? std::move(a) : std::move(b)).x;
}
int receiver(bool left, int one, int two) {
  Pod a{one};
  Pod b{two};
  return (left ? std::move(a) : std::move(b)).read();
}
int main() {
  return bound(true, 1, 2) + returned(false, 3, 4).read() +
         field(true, 5, 6) + receiver(false, 7, 8) - 18;
}
)");
  expect(frontend.canGenerateCode(),
         "cleanup-free conditional moved-local arms verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  std::size_t representationOnly = 0;
  std::size_t activating = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    // The two arm Moves out of a whole named local, and each arm's
    // Initialize into the join temporary.
    struct Arm {
      lang::MirInstructionId move = 0;
      lang::MirInstructionId initialize = 0;
      lang::MirPlaceId local = 0;
    };
    std::vector<Arm> arms;
    for (const auto &block : function.body.blocks)
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index) {
        const auto &move = block.instructions[index];
        const auto &initialize = block.instructions[index + 1];
        const lang::MirPlace *local =
            move.kind == lang::MirInstructionKind::Move &&
                    move.operands.size() == 1 &&
                    move.operands[0].kind == lang::MirOperandKind::Move
                ? function.body.findPlace(move.operands[0].place)
                : nullptr;
        if (local != nullptr &&
            local->root == lang::MirPlaceRootKind::Binding && move.ownership &&
            initialize.kind == lang::MirInstructionKind::Initialize)
          arms.push_back({move.id, initialize.id, local->id});
      }
    if (arms.size() != 2)
      continue;
    const lang::MirInstruction *firstInitialize = nullptr;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.id == arms[0].initialize)
          firstInitialize = &instruction;
    const bool activates =
        firstInitialize != nullptr && !firstInitialize->lifecycle.empty();
    representationOnly += !activates;
    activating += activates;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyze(address).ready() &&
               supportsSelectedBodyText(frontend.mir, map, function.id),
           "a cleanup-free conditional admits its moved-local arms");
    if (!supportsSelectedBodyText(frontend.mir, map, function.id))
      continue;
    // Each arm constructs the join storage from its local, and each local's
    // binding drop destroys it once, whether or not its arm ran.
    const std::string text =
        emitter.emitBodyText(address, "moved-local-arm-test-v0", 1).text;
    for (const Arm &arm : arms) {
      const std::string local = "__gti_mir_p_" + std::to_string(arm.local);
      std::size_t destroys = 0;
      for (std::size_t at = text.find(local + ".destroy();");
           at != std::string::npos;
           at = text.find(local + ".destroy();", at + 1))
        ++destroys;
      expect(text.find(".construct(std::move(" + local + ".get()));") !=
                     std::string::npos &&
                 destroys == 1,
             "a moved-local arm constructs the join from its local, and the "
             "local's binding drop ends its shell once:\n" +
                 text);
    }
    // The verifier rejects each forgery too; the backend's own arm proof
    // must refuse it without relying on that.
    const auto refused = [&](auto &&mutate, const std::string &what) {
      auto malformed = frontend.mir;
      mutate(*lang::findMirBody(malformed, address));
      expect(!lang::verifyMirProgram(malformed).valid(),
             "a forged moved-local arm fails MIR verification: " + what);
      expect(!supportsSelectedBodyText(malformed, map, function.id),
             "a forged moved-local arm cannot enter backend emission: " + what);
    };
    const auto onInstruction = [](lang::MirBody &body,
                                  lang::MirInstructionId id, auto &&mutate) {
      for (auto &block : body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == id)
            mutate(instruction);
    };
    const lang::MirPlace *other = function.body.findPlace(arms[1].local);
    expect(other != nullptr && other->key.has_value(),
           "the other arm's local has an ownership key");
    if (other == nullptr || !other->key)
      continue;
    const std::string_view forgeries[] = {
        "no ownership event", "maybe-moved ownership", "read ownership",
        "the other local's ownership", "lifecycle event on the move"};
    for (std::size_t mutation = 0; mutation != std::size(forgeries);
         ++mutation) {
      refused(
          [&](lang::MirBody &body) {
            onInstruction(body, arms[0].move, [&](auto &move) {
              if (mutation == 0)
                move.ownership.reset();
              else if (mutation == 1)
                move.ownership->after = lang::OwnershipStateSet::MaybeMoved;
              else if (mutation == 2)
                move.ownership->kind = lang::OwnershipEventKind::Read;
              else if (mutation == 3)
                move.ownership->place = *other->key;
              else
                move.lifecycle.push_back(
                    {.kind = lang::MirLifecycleEventKind::Initialize});
            });
          },
          std::string(forgeries[mutation]));
    }
    // The moved-from local's binding drop must stay passive: it ends the
    // moved-from shell on one path and the live object on the other.
    refused(
        [&](lang::MirBody &body) {
          for (auto &drop : body.dropObligations)
            if (drop.place == arms[0].local)
              drop.dropType.requiresActiveCleanup = true;
        },
        "cleanup on the moved-from local");
    if (activates)
      refused(
          [&](lang::MirBody &body) {
            onInstruction(body, arms[0].initialize, [](auto &initialize) {
              initialize.lifecycle.front().conditional = true;
            });
          },
          "conditional activation");
  }
  expect(representationOnly == 3 && activating == 1,
         "the bound, returned and field-read joins and the receiver join "
         "were exercised: " +
             std::to_string(representationOnly) + ", " +
             std::to_string(activating));
}

// A construction consuming an Expected<Class, E> argument may publish its own
// result through its consuming Return or into a constructor field. The
// success class has no default constructor: the producer publishes the
// argument into its prepared-parameter slot and the construction takes it
// from there, retiring the slot, for a cleanup-owning payload as well.
// (Regression: the former gti-lang-official issue #6 emitted a
// default-constructed ::gti_internal::backend::expected<Item, E> local.)
void testExpectedClassArgumentReturnedConstruction() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-expected-class-returned-construction.gti", R"(
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
  // Counts constructions on a line naming `construction` and `destination`
  // that take one of this body's staged prepared expected arguments.
  const auto takenInto = [&](const lang::MirBody &body, const std::string &text,
                             std::string_view construction,
                             std::string_view destination) {
    std::size_t taken = 0;
    for (const lang::MirBlock &block : body.blocks)
      for (const lang::MirInstruction &input : block.instructions) {
        if (input.kind != lang::MirInstructionKind::CallInput ||
            !input.destination || !input.preparedParameterDrop ||
            !isExpectedClass(frontend, input.info.type))
          continue;
        const auto at =
            text.find("std::move(__gti_mir_p_" +
                      std::to_string(*input.destination) + ".take())");
        if (at == std::string::npos)
          continue;
        const auto lineStart = text.rfind('\n', at) + 1;
        const std::string_view line = std::string_view(text).substr(
            lineStart, text.find('\n', at) - lineStart);
        taken += line.find(construction) != std::string_view::npos &&
                 line.find(destination) != std::string_view::npos;
      }
    return taken;
  };
  const lang::MirFunctionInstance *holdItem =
      namedFunction(frontend, "hold_item");
  const lang::MirFunctionInstance *holdTracked =
      namedFunction(frontend, "hold_tracked");
  const lang::MirConstructorInstance *owner = nullptr;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *source = frontend.hir.findConstructorInstance(constructor.id);
    for (const auto &instance : frontend.hir.classInstances())
      if (source != nullptr && instance.id == source->owner &&
          instance.source != nullptr &&
          instance.source->name().lexeme == "item_owner")
        owner = &constructor;
  }
  expect(holdItem != nullptr && holdTracked != nullptr && owner != nullptr,
         "the returned construction fixture has both returns and the owner "
         "constructor");
  if (holdItem == nullptr || holdTracked == nullptr || owner == nullptr) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress ownerAddress{lang::MirBodyKind::Constructor,
                                          owner->id};
  const std::string itemText = selectedBodyText(frontend, *holdItem);
  const std::string trackedText = selectedBodyText(frontend, *holdTracked);
  const std::string ownerText =
      owner->mayRaiseDefinedFailure
          ? emitter.emitFailureBodyText(ownerAddress, "selected", 1).text
          : emitter.emitBodyText(ownerAddress, "selected", 1).text;
  expect(takenInto(holdItem->body, itemText, "::__gti_program::item_holder(",
                   "(__gti_mir_out_result)) ") == 1,
         "a returned construction should publish into the transformed result "
         "from the staged wrapper and retire it afterwards");
  expect(takenInto(owner->body, ownerText, "::__gti_program::item_holder(",
                   "std::addressof(") == 1,
         "a member-initializer construction should publish into its field "
         "from the staged wrapper and retire it afterwards");
  expect(itemText.find("_expected __gti_mir_v_") == std::string::npos &&
             ownerText.find("_expected __gti_mir_v_") == std::string::npos,
         "no Expected without a default-constructible payload should "
         "value-initialize an ordinary local");
  expect(takenInto(holdTracked->body, trackedText,
                   "::__gti_program::tracked_holder(",
                   "(__gti_mir_out_result)) ") == 1,
         "a returned construction consuming a cleanup-owning Expected should "
         "take it from its staged slot");
}

// An Expected<Class, E> call temporary read once by its state test or a
// checked value() or error() is an ordinary class temporary in MIR's
// value-rooted slot, cleanup-free or owning: the producer constructs the slot,
// the observer reads it there, and MIR's own Drop retires it after that read.
// A class payload moves into its own field-read slot. (Regression: these
// value-initialized ::gti_internal::backend::expected<Item, E> or reported
// GTI-B0001.)
void testExpectedTemporaryObservationSlot() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-expected-temporary-observation.gti", R"(
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
  const lang::MirFunctionInstance *observe = namedFunction(frontend, "observe");
  expect(observe != nullptr, "the observation fixture has its observer");
  if (observe == nullptr) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *observe);
  // Each observed temporary is constructed by its producer in its
  // value-rooted slot, read there, and retired by MIR's Drop after the read.
  std::size_t observed = 0;
  std::size_t owning = 0;
  for (const lang::MirPlace &place : observe->body.places) {
    if (place.root != lang::MirPlaceRootKind::Value ||
        !place.projections.empty() || !isExpectedClass(frontend, place.type))
      continue;
    const std::string slot = "__gti_mir_p_" + std::to_string(place.id);
    const auto produced = text.find(slot + ".construction_address()");
    const auto read = produced == std::string::npos
                          ? std::string::npos
                          : text.find(slot + ".get()", produced);
    if (read == std::string::npos ||
        text.find(slot + ".destroy();", read) == std::string::npos)
      continue;
    if (place.type.arguments.front().kind == lang::SemanticType::Class &&
        frontend.semantics.findClassType(place.type.arguments.front().classId)
                ->declaration->name()
                .lexeme == "tracked")
      ++owning;
    else
      ++observed;
  }
  expect(observed == 3,
         "the state test, the value() extraction and the error() extraction "
         "should each read one constructed slot and retire it: " +
             std::to_string(observed) + " observed");
  bool payloadSlot = false;
  for (const lang::MirBlock &block : observe->body.blocks)
    for (const lang::MirInstruction &call : block.instructions) {
      const auto *target =
          call.kind == lang::MirInstructionKind::Call && call.functionTarget &&
                  call.result
              ? frontend.hir.findFunctionInstance(*call.functionTarget)
              : nullptr;
      if (target == nullptr || target->source == nullptr ||
          target->source->name().lexeme != "value" ||
          call.info.type.kind != lang::SemanticType::Class)
        continue;
      const std::string slot = "__gti_mir_v_" + std::to_string(*call.result);
      payloadSlot =
          text.find("mir_lifetime_slot<::__gti_program::item_value, true> " +
                    slot + ";") != std::string::npos &&
          text.find(slot + ".construction_address()") != std::string::npos &&
          text.find(slot + ".get().number") != std::string::npos;
    }
  expect(payloadSlot,
         "value() should move the payload into its own field-read slot");
  expect(text.find("_expected __gti_mir_v_") == std::string::npos,
         "no observed Expected should value-initialize an ordinary local");
  expect(owning == 1,
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
      analyzeWithStandardLibrary("cpp-mir-owning-temporary-field-view.gti", R"(
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
  const lang::MirFunctionInstance *reader = namedFunction(frontend, "read");
  expect(reader != nullptr, "the field view fixture has its reader");
  if (reader == nullptr) {
    return;
  }
  const std::string text = selectedBodyText(frontend, *reader);
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
    // The consuming value() accessor constructs its payload in this slot.
    const std::size_t accessor = text.find("_value__gti_mir_failure(");
    if (accessor < text.find(slot + read) &&
        text.find(slot + ".construction_address()", accessor) <
            text.find(slot + read))
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
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-raising-construction-assignment.gti", R"(
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
  // The staged expected argument is taken out of its prepared-parameter
  // slot, ending that representation, by the construction into the
  // assignment slot.
  bool takenIntoSlot = false;
  const std::string staged = ".construction_address())) ::__gti_program::"
                             "item_holder(::gti_internal::backend::mir_"
                             "failure_constructor_tag_v1<";
  for (std::size_t at = text.find(staged); at != std::string::npos;
       at = text.find(staged, at + staged.size())) {
    const std::size_t lineStart = text.rfind('\n', at) + 1;
    const std::string_view line = std::string_view(text).substr(
        lineStart, text.find('\n', at) - lineStart);
    takenIntoSlot =
        takenIntoSlot ||
        (line.find("::new (static_cast<void *>(__gti_mir_v_") !=
             std::string_view::npos &&
         line.find("std::move(__gti_mir_p_") != std::string_view::npos &&
         line.find(".take()), ") != std::string_view::npos);
  }
  expect(assignedThroughSlot("item_holder") && takenIntoSlot,
         "a raising construction consuming a staged Expected argument should "
         "use the same assignment slot");
  expect(text.find("mir_lifetime_slot<::__gti_program::plain_holder, true>") ==
                 std::string::npos &&
             text.find("= ::__gti_program::plain_holder(") != std::string::npos,
         "a failure-free construction should keep the fused assignment");
}

// A cleanup-free Expected<Class, E> call result initializing a constructor
// field is staged and moved into the field's storage after its empty
// representation ends. Two such temporaries joined by a conditional both
// publish into the join's one hidden temporary slot, the grouped join reads
// that storage directly, and MIR's Drop retires it after the read. Neither
// value-initializes the Expected. (Regression: both reported GTI-B0001.)
void testExpectedFieldAndJoinPublication() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-field-and-join.gti", R"(
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
  const lang::MirFunctionInstance *observe = namedFunction(frontend, "observe");
  const lang::MirConstructorInstance *box = nullptr;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *source = frontend.hir.findConstructorInstance(constructor.id);
    for (const auto &owner : frontend.hir.classInstances())
      if (source != nullptr && owner.id == source->owner &&
          owner.source != nullptr && owner.source->name().lexeme == "box")
        box = &constructor;
  }
  expect(observe != nullptr && box != nullptr,
         "the field and join fixture has its observer and box constructor");
  if (observe == nullptr || box == nullptr) {
    return;
  }
  const std::string joinText = selectedBodyText(frontend, *observe);
  // The conditional join is one hidden temporary slot both arms publish into.
  bool joined = false;
  for (const lang::MirPlace &place : observe->body.places) {
    if (place.root != lang::MirPlaceRootKind::Temporary ||
        !place.projections.empty() || !isExpectedClass(frontend, place.type))
      continue;
    const std::string slot = "__gti_mir_p_" + std::to_string(place.id);
    const auto first = joinText.find(slot + ".construction_address()");
    const auto second =
        first == std::string::npos
            ? first
            : joinText.find(slot + ".construction_address()", first + 1);
    const auto read = second == std::string::npos
                          ? second
                          : joinText.find(slot + ".get().", second);
    const auto lineEnd =
        read == std::string::npos ? read : joinText.find('\n', read);
    joined =
        joined ||
        (lineEnd != std::string::npos &&
         joinText.substr(read, lineEnd - read).find("_has_value()") !=
             std::string::npos &&
         joinText.find(slot + ".destroy();", lineEnd) != std::string::npos);
  }
  expect(joined,
         "both join arms should publish into the join's storage, and the "
         "grouped join should read that storage");
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress boxAddress{lang::MirBodyKind::Constructor,
                                        box->id};
  const std::string text =
      box->mayRaiseDefinedFailure
          ? emitter.emitFailureBodyText(boxAddress, "selected", 1).text
          : emitter.emitBodyText(boxAddress, "selected", 1).text;
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
  // A checked producer publishes through the slot's construction address; a
  // failure-free one constructs the slot from its returned wrapper.
  expect(!slot.empty() &&
             (text.find(slot + ".construction_address()") < constructed ||
              text.find(slot + ".construct_from([&]() { return ::__gti_"
                               "program::__gti_fn_") < constructed) &&
             text.find(slot + ".destroy();", moved) != std::string::npos,
         "the field initializer should move a staged result into the field");
  expect(text.find("_expected __gti_mir_v_") == std::string::npos &&
             joinText.find("_expected __gti_mir_v_") == std::string::npos,
         "no Expected should value-initialize an ordinary local");
}

void testOwnedArgumentMemberInitializer() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-owned-argument-member-initializer.gti", R"(
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
  // The member's constructor takes the owning result through its prepared
  // parameter, which the result's own value obligation was reparented into;
  // that handoff is not a transfer into `this`.
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
          routed =
              routed ||
              (event.kind == lang::MirLifecycleEventKind::TransferOut &&
               source != nullptr &&
               source->kind == lang::MirDropObligationKind::PreparedParameter &&
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
          : text.find("{}, std::move(__gti_mir_p_", constructed);
  const std::size_t slotEnd = slotStart >= lineEnd
                                  ? std::string::npos
                                  : text.find(".take()), &__gti_mir_construct_"
                                              "success_",
                                              slotStart);
  const std::string slot =
      slotEnd >= lineEnd
          ? std::string()
          : text.substr(slotStart + 14, slotEnd - slotStart - 14);
  // The owned result's slot is its prepared-parameter slot; a checked
  // producer publishes into it through its construction address.
  const std::size_t declared =
      slot.empty() ? std::string::npos
                   : text.rfind("_expected> " + slot + ";", constructed);
  expect(declared != std::string::npos &&
             (text.find(slot + ".construction_address()", declared) <
                  constructed ||
              text.find(slot + ".construct_from([&]() { return ::__gti_"
                               "program::__gti_fn_",
                        declared) < constructed),
         "the member should be constructed in place from the owned result's "
         "slot, taken once after its producer published it");
  expect(text.find("_expected __gti_mir_v_") == std::string::npos,
         "no Expected should value-initialize an ordinary local");
}

void testOwningExpectedFieldPublication() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-owning-expected-field.gti", R"(
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
  const lang::MirConstructorInstance *box = nullptr;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *source = frontend.hir.findConstructorInstance(constructor.id);
    for (const auto &owner : frontend.hir.classInstances())
      if (source != nullptr && owner.id == source->owner &&
          owner.source != nullptr && owner.source->name().lexeme == "box")
        box = &constructor;
  }
  expect(box != nullptr, "the owning Expected field fixture has its box");
  if (box == nullptr) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{lang::MirBodyKind::Constructor, box->id};
  const std::string text =
      box->mayRaiseDefinedFailure
          ? emitter.emitFailureBodyText(address, "selected", 1).text
          : emitter.emitBodyText(address, "selected", 1).text;
  // Each field Initialize reparents its result's value obligation into the
  // field's rollback: the field is move-published from the result's
  // lifetime slot, which then retires without a semantic drop.
  std::size_t publications = 0;
  std::size_t checkedProducers = 0;
  for (const lang::MirBlock &block : box->body.blocks)
    for (const lang::MirInstruction &initialize : block.instructions) {
      if (initialize.kind != lang::MirInstructionKind::Initialize ||
          !initialize.destination || initialize.lifecycle.size() != 1 ||
          initialize.lifecycle.front().kind !=
              lang::MirLifecycleEventKind::Reparent ||
          !isExpectedClass(frontend, initialize.info.type))
        continue;
      const auto *result =
          box->body.findDropObligation(initialize.lifecycle.front().source);
      if (result == nullptr)
        continue;
      const std::string slot = "__gti_mir_p_" + std::to_string(result->place);
      const std::string field =
          "__gti_mir_p_" + std::to_string(*initialize.destination);
      const std::string publish = field + " = std::move(" + slot + ".get());";
      const auto published = text.find(publish);
      const auto next =
          published == std::string::npos
              ? published
              : text.find_first_not_of(' ', text.find('\n', published) + 1);
      if (published == std::string::npos ||
          text.compare(next, slot.size() + 11, slot + ".destroy();") != 0 ||
          text.find("_expected> " + slot + ";") == std::string::npos)
        continue;
      ++publications;
      checkedProducers += text.find(slot + ".mark_constructed();") < published;
    }
  expect(publications == 2 && checkedProducers == 2,
         "each owning Expected field should be published from its result's "
         "lifetime slot, including after a checked producer, and the slot "
         "should retire without a semantic drop");
  expect(text.find("_expected __gti_mir_v_") == std::string::npos,
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

void testClassConditionalJoinBorrowedInputs() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-conditional-borrowed-input.gti", R"gti(
#include <std/string>
struct Packet {
  int value;
  Packet(int value) : value(value) {}
};
Packet first(std::string_view text) { return Packet(text[0] == 'a' ? 1 : 2); }
int choose(bool left) {
  std::string a{"a"};
  std::string b{"b"};
  Packet result = left ? first(a.view()) : first(b.view());
  return result.value;
}
int main() { return choose(true) + choose(false) - 3; }
)gti");
  expect(frontend.canGenerateCode(),
         "conditional class calls with borrowed inputs pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  const auto *function = namedFunction(frontend, "choose");
  expect(function != nullptr,
         "the borrowed-input conditional has its own body");
  if (function == nullptr)
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::MirBodyAddress address{lang::MirBodyKind::Function, function->id};
  const auto supported = [&](const lang::MirProgram &program) {
    return lang::CppMirBodyEmitter(program, rows)
        .supportsFailureBodyText(address);
  };
  expect(supported(frontend.mir),
         "the arm may end its input loan after publishing its class result");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "the complete backend emits conditional borrowed-input calls");
  std::vector<lang::MirBlockId> publications;
  for (const auto &block : function->body.blocks)
    if (block.instructions.size() == 2 &&
        block.instructions.front().kind ==
            lang::MirInstructionKind::Initialize &&
        block.instructions.back().kind == lang::MirInstructionKind::EndBorrow)
      publications.push_back(block.id);
  expect(
      publications.size() == 2,
      "each conditional arm has a publication followed by its input loan end");
  if (publications.size() != 2)
    return;
  for (int mutation = 0; mutation < 2; ++mutation) {
    auto changed = frontend.mir;
    auto *target = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function->id));
    auto *block = const_cast<lang::MirBlock *>(
        target->body.findBlock(publications.front()));
    if (mutation == 0) {
      std::swap(block->instructions.front(), block->instructions.back());
      expect(!supported(changed),
             "loan-only tails do not permit work before the publication");
    } else {
      block->instructions.back().loan =
          target->body.findBlock(publications.back())->instructions.back().loan;
      expect(!lang::verifyMirProgram(changed).valid(),
             "ending the other arm's loan remains invalid MIR");
    }
  }
}

void testNestedCleanupFreeJoinMove() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-nested-cleanup-free-join.gti", R"(
struct plain_value {
  int32_t number;
  plain_value(int32_t number) : number(number) {}
};

plain_value make_plain(int32_t number) { return plain_value(number); }

int32_t bound(bool left, int32_t one) {
  mut int32_t total = 0;
  for (mut int32_t i = 0; i < 3; i += 1) {
    plain_value t =
        left ? make_plain(i) : (i > one ? make_plain(one) : plain_value(i));
    total += t.number;
  }
  return total;
}

int32_t viewed(bool left, int32_t one) {
  mut int32_t total = 0;
  for (mut int32_t i = 0; i < 3; i += 1)
    total += (left ? (i > one ? make_plain(one) : plain_value(i))
                   : make_plain(i)).number;
  return total;
}

int main() { return bound(false, 1) + viewed(true, 1) - 4; }
)");
  expect(frontend.canGenerateCode(),
         "the nested cleanup-free join fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  // Each loop evaluates its conditional again. The outer arm that moves out
  // of the nested join, and the staged move out of the viewed outer join,
  // each end the emptied join's storage on the next line, so no iteration
  // constructs an engaged representation.
  const std::string retire = ".destroy(); // end moved join representation\n";
  std::size_t retirements = 0;
  bool movedFirst = true;
  for (std::size_t at = text.find(retire); at != std::string::npos;
       at = text.find(retire, at + 1)) {
    ++retirements;
    const std::size_t lineStart = text.rfind('\n', at) + 1;
    const std::size_t slotStart = text.find("__gti_mir_p_", lineStart);
    const std::string slot =
        slotStart < at ? text.substr(slotStart, at - slotStart) : std::string();
    const std::size_t previousEnd = lineStart - 1;
    const std::size_t previousStart = text.rfind('\n', previousEnd - 1) + 1;
    const std::string previous =
        text.substr(previousStart, previousEnd - previousStart);
    const std::string moved = ".construct(std::move(" + slot + ".get()));";
    movedFirst =
        movedFirst && !slot.empty() && previous.size() > moved.size() &&
        previous.compare(previous.size() - moved.size(), moved.size(), moved) ==
            0;
  }
  expect(retirements == 3 && movedFirst,
         "both nested joins and the viewed outer join should end their "
         "storage right after the move that empties it");
  // The bound outer join ends its storage at the binding, as before.
  expect(text.find(".destroy(); // end passive conditional representation\n") !=
             std::string::npos,
         "the bound outer join should end its storage at the binding");
}

// A cleanup-free conditional's constructor arm may hand a by-value class
// argument to its constructor: that prepared argument's transfer is the arm's
// only lifecycle event, as it is for a call arm. A class that holds a text
// view is cleanup-free too, since the view is a passive descriptor. Both joins
// publish their arms into representation-only storage, and the joined value
// moves into its field-read slot. Forged transfers and a borrowed-state text
// arm stay out.
void testCleanupFreeJoinTransferAndTextArms() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-cleanup-free-join-transfer-text.gti", R"(
class point_t {
public:
  int32_t x;
  point_t(int32_t x) : x(x) {}
};
class segment_t {
public:
  point_t low;
  segment_t(point_t low) : low(low) {}
};
class word_t {
public:
  std::string_view text;
  int32_t kind;
  word_t(std::string_view text, int32_t kind) : text(text), kind(kind) {}
};
int32_t transferred(bool left) {
  return (left ? segment_t(point_t(1)) : segment_t(point_t(2))).low.x;
}
int32_t texted(bool left) {
  return (left ? word_t("left", 1) : word_t("right", 2)).kind;
}
int main() { return transferred(true) + texted(false) - 3; }
)");
  expect(frontend.canGenerateCode(),
         "the transfer and text join fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const std::string retire = ".destroy(); // end moved join representation\n";
  const auto count = [](const std::string &text, const std::string &needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size()))
      ++occurrences;
    return occurrences;
  };
  std::size_t transferBodies = 0;
  std::size_t textBodies = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    // The two arms are the constructions published into one join storage.
    std::vector<const lang::MirInstruction *> arms;
    bool transfer = false;
    bool text = false;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            !instruction.result)
          continue;
        const auto uses = function.body.usesOf(*instruction.result);
        const lang::MirInstruction *initialize = nullptr;
        for (const auto &candidateBlock : function.body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            if (uses.size() == 1 && candidate.id == uses.front().instruction)
              initialize = &candidate;
        const auto *storage =
            initialize != nullptr &&
                    initialize->kind == lang::MirInstructionKind::Initialize &&
                    initialize->destination
                ? function.body.findPlace(*initialize->destination)
                : nullptr;
        if (storage == nullptr ||
            storage->root != lang::MirPlaceRootKind::Temporary)
          continue;
        arms.push_back(&instruction);
        for (const auto &parameter : instruction.parameterTypes) {
          transfer |= parameter.kind == lang::SemanticType::Class;
          text |= parameter.kind == lang::SemanticType::StringView;
        }
      }
    if (arms.size() != 2 || transfer == text)
      continue;
    transferBodies += transfer;
    textBodies += text;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    const bool supported =
        supportsSelectedBodyText(frontend.mir, map, function.id);
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    const std::string body =
        !supported ? std::string()
        : function.mayRaiseDefinedFailure
            ? emitter.emitFailureBodyText(address, "join-arms", 1).text
            : emitter.emitBodyText(address, "join-arms", 1).text;
    const std::size_t retired = body.find(retire);
    const std::size_t joinStart = retired == std::string::npos
                                      ? std::string::npos
                                      : body.rfind("__gti_mir_p_", retired);
    const std::string join = joinStart == std::string::npos
                                 ? std::string()
                                 : body.substr(joinStart, retired - joinStart);
    expect(supported && !join.empty() && count(body, retire) == 1 &&
               count(body, join + ".mark_constructed()") == 2 &&
               body.rfind(".construct(std::move(" + join + ".get()));",
                          retired) != std::string::npos &&
               count(body, ".take()") == (transfer ? 2u : 0u),
           std::string(transfer ? "by-value argument" : "text view") +
               " arms should publish into one join storage, which the "
               "joined value's move empties and retires");

    for (int mutation = 0; mutation != (transfer ? 3 : 1); ++mutation) {
      auto malformed = frontend.mir;
      auto *mutable_body = lang::findMirBody(malformed, address);
      for (auto &block : mutable_body->blocks)
        for (auto &instruction : block.instructions) {
          if (instruction.id != arms.front()->id)
            continue;
          if (!transfer) {
            // The arm value holds borrowed state the class does not.
            instruction.info.traits.containsBorrowedState = true;
            for (auto &value : mutable_body->values)
              if (value.id == *instruction.result)
                value.info.traits.containsBorrowedState = true;
          } else if (!instruction.lifecycle.empty()) {
            const auto transferOut = instruction.lifecycle.front();
            if (mutation == 0)
              instruction.lifecycle.push_back(transferOut);
            else if (mutation == 1)
              instruction.lifecycle.front().conditional = true;
            else
              instruction.lifecycle.front().kind =
                  lang::MirLifecycleEventKind::Initialize;
          }
        }
      expect(!supportsSelectedBodyText(malformed, map, function.id),
             std::string(transfer ? "a forged arm transfer "
                                  : "a borrowed-state text arm ") +
                 std::to_string(mutation) +
                 " should keep the join out of representation-only storage");
    }
  }
  expect(transferBodies == 1 && textBodies == 1,
         "the fixture should have one by-value argument join and one text "
         "join");
}

// Each arm of a cleanup-free conditional may evaluate its inputs before the
// call producing its result: an overflow-checked argument, a call that can
// fail, or a `?` operand. The branch then targets the start of that region,
// not the block publishing the arm, and the join proves the region exactly.
// A failure edge or an outside edge forged into an arm is refused.
void testCheckedConditionalArmInputs() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-checked-conditional-arm-inputs.gti", R"(
class Pod {
public:
  int x;
  Pod(int v) : x(v) {}
  int read() { return this.x; }
};
enum class input_error { negative };
Pod fresh(int id) { return Pod(id); }
int quotient(int value, int divisor) { return value / divisor; }
expected<int, input_error> checked(int value) {
  if (value < 0) {
    return unexpected(input_error::negative);
  }
  return value;
}
int bound(bool left, int base) {
  Pod kept = left ? fresh(base + 29) : fresh(base + 30);
  return kept.read();
}
Pod returned(bool left, int base, int divisor) {
  return left ? fresh(quotient(base, divisor)) : fresh(base + 1);
}
expected<int, input_error> propagated(bool left, int base) {
  Pod kept = left ? fresh(?checked(base + 31)) : fresh(?checked(base - 32));
  return kept.read();
}
int main() {
  return bound(true, 0) + returned(false, 1, 1).read() +
         propagated(true, 1).value() - 63;
}
)");
  expect(frontend.canGenerateCode(),
         "conditional arms evaluating checked inputs verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  std::size_t bodies = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    // The two arm publications: each Initializes one temporary from a class
    // call result made in the same block.
    std::vector<std::pair<lang::MirPlaceId, lang::MirBlockId>> publications;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *place =
            instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.destination && instruction.operands.size() == 1
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (place == nullptr ||
            place->root != lang::MirPlaceRootKind::Temporary)
          continue;
        for (const auto &producer : block.instructions)
          if (producer.kind == lang::MirInstructionKind::Call &&
              producer.result &&
              *producer.result == instruction.operands.front().value &&
              producer.info.type.kind == lang::SemanticType::Class)
            publications.emplace_back(place->id, block.id);
      }
    std::vector<lang::MirBlockId> entries;
    for (const auto &[place, block] : publications)
      if (std::count_if(publications.begin(), publications.end(),
                        [&](const auto &publication) {
                          return publication.first == place;
                        }) == 2)
        entries.push_back(block);
    // No branch selects the two arm publications directly: each arm first
    // evaluates its inputs.
    const bool regions =
        entries.size() == 2 &&
        std::none_of(
            function.body.blocks.begin(), function.body.blocks.end(),
            [&](const lang::MirBlock &block) {
              return block.terminator.kind == lang::MirTerminatorKind::Branch &&
                     std::is_permutation(entries.begin(), entries.end(),
                                         std::array{block.terminator.target,
                                                    block.terminator.elseTarget}
                                             .begin());
            });
    if (!regions)
      continue;
    ++bodies;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyze(address).ready() &&
               supportsSelectedBodyText(frontend.mir, map, function.id),
           "a cleanup-free conditional admits arms that evaluate checked "
           "inputs first");
    // The first arm's region ends in its predecessor block.
    lang::MirBlockId predecessor = 0;
    lang::MirBlockId other = 0;
    for (const auto &block : function.body.blocks) {
      if (block.terminator.target == entries[0])
        predecessor = block.id;
      if (block.terminator.target == entries[1])
        other = block.id;
    }
    const auto refused = [&](auto &&mutate, const std::string &what) {
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(malformed, address);
      for (auto &block : body->blocks)
        mutate(block);
      expect(!lang::verifyMirProgram(malformed).valid(),
             "a forged conditional arm region fails MIR verification: " + what);
      const lang::CppMirBodyEmitter rejected(malformed, map);
      expect(!rejected.analyze(address).ready() ||
                 !supportsSelectedBodyText(malformed, map, function.id),
             "a forged conditional arm region cannot enter backend "
             "emission: " +
                 what);
    };
    const auto *invoke = function.body.findBlock(predecessor);
    if (invoke != nullptr &&
        invoke->terminator.kind == lang::MirTerminatorKind::Invoke)
      refused(
          [&](lang::MirBlock &block) {
            if (block.id == predecessor)
              block.terminator.elseTarget = entries[0];
          },
          "failure edge into the arm");
    refused(
        [&](lang::MirBlock &block) {
          if (block.id == other)
            block.terminator.target = entries[0];
        },
        "the other arm entering this one");
  }
  expect(bodies == 3, "the bound, returned and propagated conditionals were "
                      "exercised: " +
                          std::to_string(bodies));
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

// A lambda invocation returning a class without a default constructor is the
// same fresh result as a checked source call's: verified MIR binds it to its
// lambda instance and completes it on the Invoke success edge under callable
// propagation. A cleanup-free result read through one field or passed by
// value, and an owning result read through a field view, therefore publish
// into the slot a source call's result uses rather than a default-constructed
// C++ local. An owning result read inside a short-circuit operand constructs
// in that operand's conditional temporary, and a result returned directly
// after a moved parameter's cleanup still constructs in the caller's
// placement storage, with no slot of its own. An invocation without callable
// propagation or without its exact lambda target cannot enter backend
// emission.
void testCallableClassResultSlots() {
  const auto frontend =
      analyzeWithStandardLibrary("cpp-mir-callable-class-result-slots.gti", R"(
#include <std/utility>
class Ticket {
public:
  Ticket(int32_t input) : identity(input) {}
  Ticket(Ticket&& other) = default;
  int32_t get() { return this.identity; }
private:
  int32_t identity;
};
Ticket finish(Ticket input) {
  auto action = [](Ticket value) -> Ticket { return std::move(value); };
  return action(std::move(input));
}
struct Plain {
  int id;
  Plain(int raw) : id(raw) {}
};
struct Res {
  int id;
  Res(int raw) : id(raw) {}
  ~Res() {}
};
int take(Plain p) { return p.id; }
int operand(bool skip) {
  auto owned = [](int raw) -> Res { return Res(raw); };
  return skip || owned(4).id == 4 ? 1 : 0;
}
int main() {
  auto read = [](int raw) -> Plain { return Plain(raw); };
  auto pass = [](int raw) -> Plain { return Plain(raw); };
  auto owned = [](int raw) -> Res { return Res(raw); };
  return read(1).id + take(pass(2)) + owned(3).id + operand(false) +
         finish(Ticket(4)).get() - 11;
}
)");
  expect(frontend.canGenerateCode(), "callable class results verify");
  if (!frontend.canGenerateCode())
    return;
  const auto entry =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  expect(entry != frontend.mir.functionInstances().end(),
         "the callable class result entry is retained");
  if (entry == frontend.mir.functionInstances().end())
    return;
  std::vector<lang::MirInstructionId> invocations;
  for (const auto &block : entry->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.callableInvocation && instruction.result &&
          instruction.info.type.kind == lang::SemanticType::Class)
        invocations.push_back(instruction.id);
  expect(invocations.size() == 3,
         "the three class-returning lambda invocations are exercised: " +
             std::to_string(invocations.size()));
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "class results of lambda invocations enter failure-form emission");
  if (!emitter.supportsFailureBodyText(address))
    return;
  const std::string text =
      emitter.emitFailureBodyText(address, "callable-class-result-v0", 1).text;
  const auto count = [&](std::string_view needle) {
    std::size_t found = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size()))
      ++found;
    return found;
  };
  expect(count("::__gti_program::Plain, true> __gti_mir_v_") == 1 &&
             count(".get().id") >= 2,
         "the field-read result lives in a representation-only SSA slot and "
         "each field read views its slot");
  // The three invocations, plus the transformed call of `finish`.
  const std::size_t addressed =
      count(".construction_address(), __gti_mir_failure_record);");
  expect(addressed == invocations.size() + 1 &&
             count(".mark_constructed();") >= addressed,
         "each invocation constructs its result at its slot's address and "
         "publishes it only on success: " +
             std::to_string(addressed));
  expect(count("::__gti_program::Plain __gti_mir_v_") == 0 &&
             count("::__gti_program::Res __gti_mir_v_") == 0,
         "no class result is a default-constructed C++ local");
  const auto refused = [&](auto &&mutate, const std::string &what) {
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        mutate(instruction);
    const lang::CppMirBodyEmitter rejected(malformed, map);
    expect(!rejected.analyze(address).ready() ||
               !rejected.supportsFailureBodyText(address),
           "a forged lambda class result cannot enter backend emission: " +
               what);
  };
  const auto bodyText = [&](const lang::MirFunctionInstance &function) {
    const lang::MirBodyAddress functionAddress{
        .kind = lang::MirBodyKind::Function, .owner = function.id};
    return function.mayRaiseDefinedFailure
               ? emitter
                     .emitFailureBodyText(functionAddress,
                                          "callable-class-result-v0", 1)
                     .text
               : emitter
                     .emitBodyText(functionAddress, "callable-class-result-v0",
                                   1)
                     .text;
  };
  const auto invokesClassLambda =
      [](const lang::MirFunctionInstance &function) {
        return function.entryKind == lang::ProgramEntryKind::None &&
               std::any_of(
                   function.body.blocks.begin(), function.body.blocks.end(),
                   [](const lang::MirBlock &block) {
                     return std::any_of(
                         block.instructions.begin(), block.instructions.end(),
                         [](const lang::MirInstruction &instruction) {
                           return instruction.kind ==
                                      lang::MirInstructionKind::Call &&
                                  instruction.callableInvocation &&
                                  instruction.info.type.kind ==
                                      lang::SemanticType::Class;
                         });
                   });
      };
  // The directly returned result constructs in the caller's storage.
  const auto finish = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(),
      [&](const lang::MirFunctionInstance &function) {
        return invokesClassLambda(function) &&
               function.returnType.kind == lang::SemanticType::Class;
      });
  expect(finish != frontend.mir.functionInstances().end() &&
             supportsSelectedBodyText(frontend.mir, map, finish->id),
         "a directly returned lambda class result enters emission");
  if (finish != frontend.mir.functionInstances().end() &&
      supportsSelectedBodyText(frontend.mir, map, finish->id)) {
    const std::string finishText = bodyText(*finish);
    expect(finishText.find("__gti_mir_out_result, __gti_mir_failure_record)") !=
                   std::string::npos &&
               finishText.find(", true> __gti_mir_v_") == std::string::npos &&
               finishText.find(".mark_constructed();") == std::string::npos,
           "a directly returned lambda class result publishes into the "
           "caller's placement storage without a slot of its own");
  }
  // The short-circuit operand's result lives in its conditional temporary.
  const auto operand = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(),
      [&](const lang::MirFunctionInstance &function) {
        return invokesClassLambda(function) &&
               function.returnType.kind != lang::SemanticType::Class;
      });
  expect(operand != frontend.mir.functionInstances().end() &&
             supportsSelectedBodyText(frontend.mir, map, operand->id),
         "a lambda result read inside a short-circuit operand enters "
         "emission");
  if (operand != frontend.mir.functionInstances().end() &&
      supportsSelectedBodyText(frontend.mir, map, operand->id)) {
    const std::string operandText = bodyText(*operand);
    expect(operandText.find(".construction_address(), "
                            "__gti_mir_failure_record);") !=
                   std::string::npos &&
               operandText.find(", &__gti_mir_v_") == std::string::npos,
           "the operand's invocation constructs its conditional temporary, "
           "not an undeclared SSA local");
  }
  for (const lang::MirInstructionId call : invocations) {
    refused(
        [&](lang::MirInstruction &instruction) {
          if (instruction.id == call)
            instruction.definedFailure.propagation =
                lang::FailurePropagationKind::DirectCall;
        },
        "non-callable propagation " + std::to_string(call));
    refused(
        [&](lang::MirInstruction &instruction) {
          if (instruction.id == call)
            instruction.lambdaTarget.reset();
        },
        "missing lambda target " + std::to_string(call));
  }
}

// A fused closure literal is spelled only where its chain is consumed: an
// invocation, a deduced callable argument or a comma discard, each of which
// selects the literal's convention. A lambda that is never consumed spells no
// body, so its body needs neither convention, even when no plain form exists
// (a move-only or destructor-bearing temporary passed by value into a class
// constructor). The same lambda invoked once still spells its transformed
// body at that invocation.
void testUnconsumedFusedClosureSpellsNoBody() {
  const auto frontend =
      analyzeWithStandardLibrary("cpp-mir-unconsumed-fused-closure.gti", R"(
struct Token {
  int id;
  Token(int raw) : id(raw) {}
  Token(Token& other) = delete;
  Token(Token&& other) = default;
};
struct Logged {
  int id;
  Logged(int raw) : id(raw) {}
  ~Logged() {}
};
struct Ticket {
  int id;
  Ticket(Token token) : id(token.id) {}
};
struct Receipt {
  int id;
  Receipt(Logged entry) : id(entry.id) {}
};
int unused() {
  auto ticket = [](int raw) -> Ticket { return Ticket(Token(raw)); };
  auto receipt = [](int raw) -> Receipt { return Receipt(Logged(raw)); };
  return 0;
}
int used() {
  auto ticket = [](int raw) -> Ticket { return Ticket(Token(raw)); };
  Ticket t = ticket(8);
  return t.id;
}
int main() { return unused() + used() - 8; }
)");
  expect(frontend.canGenerateCode(), "unconsumed fused closures verify");
  if (!frontend.canGenerateCode())
    return;
  // `unused` creates two closures and invokes neither; `used` invokes its one.
  const lang::MirFunctionInstance *unused = nullptr;
  const lang::MirFunctionInstance *used = nullptr;
  for (const lang::MirFunctionInstance &candidate :
       frontend.mir.functionInstances()) {
    std::size_t closures = 0;
    std::size_t invocations = 0;
    for (const auto &block : candidate.body.blocks)
      for (const auto &instruction : block.instructions) {
        closures += instruction.kind == lang::MirInstructionKind::Compute &&
                    instruction.operation == lang::MirOperation::Closure;
        invocations += instruction.kind == lang::MirInstructionKind::Call &&
                       instruction.callableInvocation.has_value();
      }
    if (closures == 2 && invocations == 0)
      unused = &candidate;
    if (closures == 1 && invocations == 1)
      used = &candidate;
  }
  expect(unused != nullptr && used != nullptr,
         "both closure-owning functions are retained");
  if (unused == nullptr || used == nullptr)
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const auto bodyText = [&](const lang::MirFunctionInstance &instance,
                            std::string_view family) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = instance.id};
    return instance.mayRaiseDefinedFailure
               ? emitter.emitFailureBodyText(address, family, 1).text
               : emitter.emitBodyText(address, family, 1).text;
  };
  expect(supportsSelectedBodyText(frontend.mir, map, unused->id),
         "a function whose lambdas are never consumed enters emission");
  expect(supportsSelectedBodyText(frontend.mir, map, used->id),
         "a function invoking the same lambda enters emission");
  if (!supportsSelectedBodyText(frontend.mir, map, unused->id) ||
      !supportsSelectedBodyText(frontend.mir, map, used->id))
    return;
  const std::string unusedText = bodyText(*unused, "unconsumed-closure-v0");
  const std::string usedText = bodyText(*used, "unconsumed-closure-v0");
  expect(unusedText.find("lambda-instance") == std::string::npos &&
             unusedText.find("spells at its consuming invocation") !=
                 std::string::npos,
         "an unconsumed fused closure spells no lambda body");
  expect(usedText.find("unconsumed-closure-v0 lambda-instance") !=
             std::string::npos,
         "an invoked fused closure spells its body at the invocation");
}

// A temporary argument evaluated only inside a conditional arm or a `&&`/`||`
// operand lives in the hidden slot that carries its conditional drop through
// the CFG merge. Class and payload-enum producers, cleanup-free or owning,
// construct that slot in place. A hidden slot that no recognizer constructs
// stays refused rather than being borrowed unwritten.
void testHiddenSlotTemporaryArguments() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-hidden-slot-temporary-arguments.gti", R"(
class Tracked {
public:
  int id;
  Tracked(int value) : id(value) {}
  ~Tracked() {}
};
enum class state { empty, engaged(int value) };
enum class owned { empty, held(Tracked item) };
Tracked make(int id) { return Tracked(id); }
owned hold(int id) { return owned::held(Tracked(id)); }
int read(Tracked& item) { return item.id; }
int payload(state& item) {
  switch (item) {
  case state::engaged(auto& value):
    return value;
  case state::empty:
    return 0;
  }
}
int held(owned& item) {
  switch (item) {
  case owned::held(auto& tracked):
    return tracked.id;
  case owned::empty:
    return 0;
  }
}
int classes(bool on) {
  return (on && read(make(1)) == 1) || read(Tracked(2)) == 2 ? 1 : 0;
}
int linear_enum(bool on) { return payload(state::engaged(3)); }
int branch_enum(bool on) { return on ? payload(state::engaged(4)) : 0; }
int owning_enum(bool on) {
  return (on && held(hold(5)) == 5) || held(owned::held(Tracked(6))) == 6
             ? 1
             : 0;
}
int main() {
  return classes(true) + linear_enum(true) + branch_enum(true) +
         owning_enum(true) - 9;
}
)");
  expect(frontend.canGenerateCode(),
         "hidden-slot temporary arguments verify in MIR");
  if (!frontend.canGenerateCode())
    return;
  const auto function = [&](std::string_view name) {
    for (const lang::HirFunctionInstance &instance :
         frontend.hir.functionInstances())
      if (instance.source != nullptr && instance.source->name().lexeme == name)
        return frontend.mir.findFunctionInstance(instance.id);
    return static_cast<const lang::MirFunctionInstance *>(nullptr);
  };
  const auto hiddenSlotStages = [](const lang::MirFunctionInstance &body) {
    std::vector<const lang::MirInstruction *> stages;
    for (const lang::MirBlock &block : body.body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions) {
        const lang::MirPlace *place =
            instruction.kind == lang::MirInstructionKind::CallInput &&
                    instruction.callInputKind ==
                        lang::HirCallInputKind::ReadTemporaryBorrow &&
                    instruction.operands.size() == 1
                ? body.body.findPlace(instruction.operands.front().place)
                : nullptr;
        if (place != nullptr &&
            place->root == lang::MirPlaceRootKind::Temporary)
          stages.push_back(&instruction);
      }
    return stages;
  };
  const lang::MirFunctionInstance *classes = function("classes");
  const lang::MirFunctionInstance *linear = function("linear_enum");
  const lang::MirFunctionInstance *branch = function("branch_enum");
  const lang::MirFunctionInstance *owning = function("owning_enum");
  expect(classes != nullptr && linear != nullptr && branch != nullptr &&
             owning != nullptr && hiddenSlotStages(*classes).size() == 2 &&
             hiddenSlotStages(*linear).empty() &&
             hiddenSlotStages(*branch).size() == 1 &&
             hiddenSlotStages(*owning).size() == 2,
         "class and payload-enum arguments in operands borrow hidden slots");
  if (classes == nullptr || linear == nullptr || branch == nullptr ||
      owning == nullptr || hiddenSlotStages(*branch).size() != 1)
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  expect(supportsSelectedBodyText(frontend.mir, map, classes->id),
         "class temporaries in operands are constructed in their hidden slot");
  expect(supportsSelectedBodyText(frontend.mir, map, linear->id),
         "a linear payload-enum argument keeps its value-rooted place");
  expect(supportsSelectedBodyText(frontend.mir, map, branch->id),
         "a cleanup-free payload enum is constructed in its hidden slot");
  expect(supportsSelectedBodyText(frontend.mir, map, owning->id),
         "owning payload enums, called or constructed, are constructed in "
         "their hidden slots");

  // Without its activation no recognizer constructs the payload enum's slot;
  // the borrow must then be refused rather than read unwritten.
  auto malformed = frontend.mir;
  const lang::MirBodyAddress address{lang::MirBodyKind::Function, branch->id};
  lang::MirBody *body = lang::findMirBody(malformed, address);
  const lang::MirPlace *slot =
      body == nullptr
          ? nullptr
          : body->findPlace(
                hiddenSlotStages(*branch).front()->operands.front().place);
  bool forged = false;
  if (body != nullptr && slot != nullptr) {
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::PayloadConstruct &&
            instruction.destination == slot->id) {
          instruction.lifecycle.clear();
          forged = true;
        }
  }
  expect(forged && !supportsSelectedBodyText(malformed, map, branch->id),
         "a payload-enum hidden slot its producer never activates is refused "
         "rather than borrowed unwritten");
}

// A borrowed payload binding names the active payload inside its subject, so
// its MIR value has no native object even when its type is a cleanup-free
// enum. A reference bound to it, as a local or as a reference-returning
// accessor's result, retains that payload's address. A fresh cleanup-free
// enum temporary is still its own SSA value.
void testBorrowedCleanupFreeEnumPayloadReference() {
  const auto frontend =
      analyze("cpp-mir-borrowed-cleanup-free-payload-reference.gti", R"(
enum class shape { circle(int radius), none };
enum class wrap { one(shape inner) };
int radius(shape& value) {
  switch (value) {
  case shape::circle(auto& r): return r;
  case shape::none: return 0;
  }
}
class holder {
  wrap state;

public:
  holder(shape value) : state(wrap::one(value)) {}
  shape& inner() {
    switch (this.state) {
    case wrap::one(auto& item): return item;
    }
  }
};
int bound(wrap& value) {
  switch (value) {
  case wrap::one(auto& item): {
    shape& alias = item;
    return radius(alias);
  }
  }
}
int fresh() { return radius(shape::circle(4)); }
int main() {
  wrap nested = wrap::one(shape::circle(3));
  holder box = holder(shape::circle(3));
  shape copied = box.inner();
  return radius(copied) + bound(nested) + fresh() - 10;
}
)");
  expect(frontend.canGenerateCode(),
         "borrowed cleanup-free payload references verify in MIR");
  if (!frontend.canGenerateCode())
    return;
  const auto function = [&](std::string_view name) {
    for (const lang::HirFunctionInstance &instance :
         frontend.hir.functionInstances())
      if (instance.source != nullptr && instance.source->name().lexeme == name)
        return frontend.mir.findFunctionInstance(instance.id);
    return static_cast<const lang::MirFunctionInstance *>(nullptr);
  };
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const auto bodyText = [&](const lang::MirFunctionInstance &instance) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = instance.id};
    return instance.mayRaiseDefinedFailure
               ? emitter.emitFailureBodyText(address, "borrowed-payload-v0", 1)
                     .text
               : emitter.emitBodyText(address, "borrowed-payload-v0", 1).text;
  };
  // Whether `text` names `__gti_mir_v_<id>` itself, not a longer id.
  const auto namesValue = [](const std::string &text, lang::MirValueId id) {
    const std::string name = "__gti_mir_v_" + std::to_string(id);
    for (std::size_t at = text.find(name); at != std::string::npos;
         at = text.find(name, at + name.size())) {
      const std::size_t end = at + name.size();
      if (end == text.size() || text[end] < '0' || text[end] > '9')
        return true;
    }
    return false;
  };
  const std::string payloadAddress =
      " = &::gti_internal::backend::payload_get<";
  for (const std::string_view name : {"inner", "bound"}) {
    const lang::MirFunctionInstance *instance = function(name);
    std::vector<lang::MirValueId> borrowed;
    if (instance != nullptr)
      for (const lang::MirBlock &block : instance->body.blocks)
        for (const lang::MirInstruction &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::Compute &&
              instruction.operation == lang::MirOperation::PayloadExtract &&
              instruction.result &&
              instruction.info.category == lang::ValueCategory::Place &&
              instruction.info.type.kind == lang::SemanticType::Enum)
            borrowed.push_back(*instruction.result);
    expect(instance != nullptr && borrowed.size() == 1 &&
               supportsSelectedBodyText(frontend.mir, map, instance->id),
           std::string(name) +
               " borrows one cleanup-free enum payload and enters emission");
    if (instance == nullptr || borrowed.size() != 1 ||
        !supportsSelectedBodyText(frontend.mir, map, instance->id))
      continue;
    const std::string text = bodyText(*instance);
    expect(!namesValue(text, borrowed.front()),
           std::string(name) +
               " never reads the borrowed payload as an SSA object");
    expect(text.find(payloadAddress) != std::string::npos,
           std::string(name) +
               " binds the reference to the active payload's address");
  }
  const lang::MirFunctionInstance *temporary = function("fresh");
  lang::MirValueId constructed = 0;
  if (temporary != nullptr)
    for (const lang::MirBlock &block : temporary->body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::PayloadConstruct &&
            instruction.result)
          constructed = *instruction.result;
  expect(temporary != nullptr && constructed != 0 &&
             supportsSelectedBodyText(frontend.mir, map, temporary->id) &&
             namesValue(bodyText(*temporary), constructed),
         "a fresh cleanup-free enum temporary is borrowed as its SSA value");
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
    {"testMovedPassiveFieldAfterFallibleSibling",
     testMovedPassiveFieldAfterFallibleSibling},
    {"testDeferredBoundShellFields", testDeferredBoundShellFields},
    {"testMovedBorrowedPayloadAfterFallibleSibling",
     testMovedBorrowedPayloadAfterFallibleSibling},
    {"testMovedViewAfterFormalRead", testMovedViewAfterFormalRead},
    {"testPassiveTextViewClassAfterFallibleSibling",
     testPassiveTextViewClassAfterFallibleSibling},
    {"testPassiveFunctionItemClassAfterFallibleSibling",
     testPassiveFunctionItemClassAfterFallibleSibling},
    {"testPassiveClassMoveAndCopyAfterFallibleSibling",
     testPassiveClassMoveAndCopyAfterFallibleSibling},
    {"testMovedChainReturnConstruction", testMovedChainReturnConstruction},
    {"testMovedPassiveFieldInPlainConstructor",
     testMovedPassiveFieldInPlainConstructor},
    {"testCopiedFormalAfterEarlierWrite", testCopiedFormalAfterEarlierWrite},
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
    {"testGenericInstanceSpecialMemberRows",
     testGenericInstanceSpecialMemberRows},
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
    {"testCleanupFreeConditionalCallArms", testCleanupFreeConditionalCallArms},
    {"testCleanupFreeConditionalMovedLocalArms",
     testCleanupFreeConditionalMovedLocalArms},
    {"testCleanupFreeJoinReceiver", testCleanupFreeJoinReceiver},
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
    {"testClassConditionalJoinBorrowedInputs",
     testClassConditionalJoinBorrowedInputs},
    {"testCheckedConditionalArmInputs", testCheckedConditionalArmInputs},
    {"testNestedCleanupFreeJoinMove", testNestedCleanupFreeJoinMove},
    {"testCleanupFreeJoinTransferAndTextArms",
     testCleanupFreeJoinTransferAndTextArms},
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
    {"testOwningPayloadEnumReferentReplacement",
     testOwningPayloadEnumReferentReplacement},
    {"testShortCircuitExpectedPayloadFieldView",
     testShortCircuitExpectedPayloadFieldView},
    {"testCallableClassResultSlots", testCallableClassResultSlots},
    {"testUnconsumedFusedClosureSpellsNoBody",
     testUnconsumedFusedClosureSpellsNoBody},
    {"testHiddenSlotTemporaryArguments", testHiddenSlotTemporaryArguments},
    {"testBorrowedCleanupFreeEnumPayloadReference",
     testBorrowedCleanupFreeEnumPayloadReference},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "text");
}
