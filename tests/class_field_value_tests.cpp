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
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            !instruction.constructorTarget && instruction.operands.empty() &&
            instruction.result) {
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
              expect(!lang::CppMirBodyEmitter(malformed, rows)
                          .supportsFailureBodyText(address),
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
        expect(lang::CppMirBodyEmitter(frontend.mir, rows)
                   .supportsFailureBodyText(address),
               "field assignment bodies admit coherent failure-form text");
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
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(address),
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
    for (const auto &block : constructor.body.blocks) {
      for (const auto &producer : block.instructions) {
        if (producer.kind != lang::MirInstructionKind::Construct ||
            producer.constructorTarget || producer.operands.size() != 0 ||
            producer.info.type.kind != lang::SemanticType::Class)
          continue;
        const auto owner = std::find_if(
            frontend.mir.classInstances().begin(),
            frontend.mir.classInstances().end(), [&](const auto &candidate) {
              return candidate.type == producer.info.type;
            });
        if (owner == frontend.mir.classInstances().end() ||
            owner->declaredFields.size() != 2)
          continue;
        ++nestedDefaults;
        const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                           constructor.id};
        expect(lang::CppMirBodyEmitter(frontend.mir, rows)
                   .supportsFailureBodyText(address),
               "nested generated defaults admit their exact constructor "
               "publication");
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirClassInstance *>(
              malformed.findClassInstance(owner->id));
          bool changedProducer = false;
          for (auto &nestedBlock : changed->fieldInitializers.blocks)
            for (auto &nested : nestedBlock.instructions) {
              if (changedProducer ||
                  (nested.kind != lang::MirInstructionKind::Call &&
                   nested.kind != lang::MirInstructionKind::Construct))
                continue;
              changedProducer = true;
              if (mutation == 0)
                nested.info.type =
                    owner->type; // a recursive default-body cycle
              else
                nested.functionTarget = 999999; // not a generated default
            }
          expect(changedProducer && !lang::CppMirBodyEmitter(malformed, rows)
                                         .supportsFailureBodyText(address),
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
    expect(cpp.find("passive generated default publishes") != std::string::npos,
           "generated field defaults use exact publication rather than SSA "
           "defaulting");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
  if (failures)
    return 1;
  std::cout << "class field value MIR and representation proofs passed\n";
}
