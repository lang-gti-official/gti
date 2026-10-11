#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/class_field_values.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "class field values have verified MIR");
  if (!frontend.canGenerateCode())
    return 1;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t assignments = 0;
  std::size_t defaultStages = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.constructorTarget && instruction.operands.empty() &&
            instruction.result) {
          const auto *target = frontend.mir.findConstructorInstance(
              *instruction.constructorTarget);
          const auto *owner =
              target ? frontend.mir.findClassInstance(target->owner) : nullptr;
          const auto stage = std::find_if(
              block.instructions.begin(), block.instructions.end(),
              [&](const auto &candidate) {
                return candidate.kind == lang::MirInstructionKind::CallInput &&
                       candidate.operands.size() == 1 &&
                       candidate.operands.front().value ==
                           *instruction.result &&
                       candidate.preparedParameterDrop;
              });
          if (stage != block.instructions.end()) {
            ++defaultStages;
            expect(target && target->generatedDefault && owner &&
                       owner->type == instruction.info.type &&
                       target->parameterTypes.empty() &&
                       stage->operands.front().type == owner->type,
                   "the omitted default stages the exact generated concrete "
                   "zero-argument constructor result");
            for (int mutation = 0; mutation != 2; ++mutation) {
              auto malformed = frontend.mir;
              auto *changed = const_cast<lang::MirFunctionInstance *>(
                  malformed.findFunctionInstance(function.id));
              for (auto &candidateBlock : changed->body.blocks)
                for (auto &candidate : candidateBlock.instructions) {
                  if (mutation == 0 && candidate.id == instruction.id)
                    candidate.constructorKind = lang::ConstructorKind::Copy;
                  if (mutation == 1 && candidate.id == stage->id)
                    candidate.lifecycle.clear();
                }
              expect(!supportsSelectedBodyText(malformed, rows, function.id),
                     "default construction staging rejects wrong construction "
                     "kind or missing activation");
            }
          }
        }
        const auto *destination =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            instruction.info.type.kind != lang::SemanticType::Class ||
            destination == nullptr || destination->projections.empty())
          continue;
        ++assignments;
        expect(supportsSelectedBodyText(frontend.mir, rows, function.id),
               "field assignment bodies admit coherent text in their "
               "selected form");
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto *place = const_cast<lang::MirPlace *>(
              changed->body.findPlace(destination->id));
          if (mutation == 0)
            place->projections.back().field = 999999;
          else
            place->type = lang::SemanticType::Unknown;
          expect(!supportsSelectedBodyText(malformed, rows, function.id),
                 "field transport rejects forged concrete field identity/type");
        }
      }
    }
  }
  expect(assignments >= 9,
         "fixture reaches nested fields, receiver fields and all producers");
  expect(defaultStages == 1,
         "omitted brace default uses exactly one prepared argument stage");
  std::size_t nestedDefaults = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *consumer = frontend.mir.findClassInstance(constructor.owner);
    const auto *consumerInfo =
        consumer ? frontend.semantics.findClassType(consumer->declaration)
                 : nullptr;
    if (!consumerInfo || !consumerInfo->declaration ||
        consumerInfo->declaration->name().lexeme != "NestedDefault")
      continue;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &producer : block.instructions) {
        if (producer.kind != lang::MirInstructionKind::Construct ||
            !producer.constructorTarget || !producer.operands.empty() ||
            producer.info.type.kind != lang::SemanticType::Class)
          continue;
        ++nestedDefaults;
        const auto *selected =
            frontend.mir.findConstructorInstance(*producer.constructorTarget);
        const auto *owner =
            selected ? frontend.mir.findClassInstance(selected->owner)
                     : nullptr;
        const bool exactSchedule =
            selected && selected->generatedDefault &&
            selected->parameterTypes.empty() && owner &&
            owner->type == producer.info.type &&
            owner->declaredFields.size() == 2 &&
            selected->initializers.size() == 2 &&
            std::equal(selected->initializers.begin(),
                       selected->initializers.end(),
                       owner->declaredFields.begin(),
                       [](const auto &initializer, const auto &field) {
                         return initializer.field == field.symbol &&
                                initializer.targetType == field.type &&
                                initializer.arguments.size() == 1;
                       });
        expect(exactSchedule,
               "the nested declaration default selects its exact generated "
               "two-field schedule in declaration order");
        if (!exactSchedule)
          continue;
        const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                           constructor.id};
        expect(lang::CppMirBodyEmitter(frontend.mir, rows)
                   .supportsFailureBodyText(address),
               "nested generated defaults admit their exact constructor "
               "publication");
        const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
            frontend.mir, constructor);
        const auto text =
            lang::CppMirBodyEmitter(frontend.mir, rows)
                .emitFailureBodyText(address, "nested-default", 1);
        expect(bindings.size() == 1 && constructor.initializers.size() == 1 &&
                   bindings.front().field ==
                       constructor.initializers.front().field &&
                   bindings.front().fieldType == owner->type &&
                   bindings.front().constructor == selected->id &&
                   bindings.front().constructInstruction == producer.id &&
                   text.emitted() &&
                   text.text.find("constructed field initializer publishes in "
                                  "the member initializer list") !=
                       std::string::npos,
               "the exact nested field and generated target publish through "
               "the verified constructor binding rather than SSA defaulting");
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(selected->id));
          bool changedProducer = false;
          for (auto &nestedBlock : changed->body.blocks)
            for (auto &nested : nestedBlock.instructions) {
              if (changedProducer ||
                  nested.kind != lang::MirInstructionKind::Construct ||
                  !nested.constructorTarget)
                continue;
              changedProducer = true;
              if (mutation == 0) {
                nested.info.type =
                    owner->type; // a recursive default-body cycle
                nested.constructorTarget = selected->id;
              } else {
                nested.functionTarget = 999999; // not a generated default
              }
            }
          expect(changedProducer &&
                     (!lang::verifyMirProgram(malformed).valid() ||
                      !lang::CppMirBodyEmitter(malformed, rows)
                           .supportsFailureBodyText(address)),
                 "nested default proof rejects cycles and unresolved source "
                 "calls");
        }
      }
    }
  }
  expect(nestedDefaults == 1,
         "fixture proves one recursive passive declaration default");
  try {
    const auto cpp = gti_test::emitCppText(frontend);
    expect(!cpp.empty(),
           "verified generated field schedules retain complete production "
           "emission");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
  if (failures)
    return 1;
  std::cout << "class field value MIR and representation proofs passed\n";
}
