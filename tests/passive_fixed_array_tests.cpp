#include "llvm_backend_test_support.h"
#include "mir_test_fixture.h"

using namespace gti_test::mir_fixture;

namespace {

// The shipped backend accepts the verified program and emits native code.
void expectNativeEmission(const lang::FrontendResult &frontend,
                          std::string_view message) {
  std::string error;
  const bool emitted = gti_test::llvmEmits(
      frontend, lang::OptimizationLevel::O0, lang::TargetInfo::host(), &error);
  expect(emitted, error.empty() ? std::string(message)
                                : std::string(message) + ": " + error);
}

} // namespace

void testPassiveArrayParameterStages() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/passive_fixed_array_parameter.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "ordinary and library array parameter fixtures have verified MIR");
  if (!frontend.canGenerateCode())
    return;
  std::size_t stages = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index) {
        const auto &stage = block.instructions[index];
        const auto &consumer = block.instructions[index + 1];
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            !stage.result ||
            stage.info.type.kind != lang::SemanticType::Array ||
            stage.callInputKind != lang::HirCallInputKind::Value ||
            stage.operands.size() != 1 ||
            stage.operands.front().kind != lang::MirOperandKind::Value ||
            consumer.kind != lang::MirInstructionKind::Construct)
          continue;
        const auto *value =
            function.body.findValue(stage.operands.front().value);
        if (value == nullptr)
          continue;
        const auto drop = std::find_if(
            function.body.dropObligations.begin(),
            function.body.dropObligations.end(), [&](const auto &candidate) {
              const auto *place = function.body.findPlace(candidate.place);
              return place && place->root == lang::MirPlaceRootKind::Value &&
                     place->value == value->id && place->projections.empty();
            });
        expect(
            drop != function.body.dropObligations.end(),
            "copied parameter source retains its full-expression obligation");
        if (drop == function.body.dropObligations.end())
          continue;
        ++stages;
        expect(consumer.parameterTypes.size() > stage.callInputIndex &&
                   consumer.parameterTypes[stage.callInputIndex] ==
                       stage.info.type &&
                   std::none_of(consumer.lifecycle.begin(),
                                consumer.lifecycle.end(),
                                [&](const lang::MirLifecycleEvent &event) {
                                  return event.source == drop->id;
                                }),
               "passive aggregate CallInput copies exact source storage and "
               "never transfers the source's obligation");
      }
    }
  }
  expect(
      stages == 2,
      "both unrelated Table and std::array use passive fixed-array parameters");
  expectNativeEmission(frontend, "passive parameter copies retain source "
                                 "storage rather than taking it");
}

int main() {
  testPassiveArrayParameterStages();
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/passive_fixed_array_values.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "typed passive arrays have verified MIR");
  if (!frontend.canGenerateCode())
    return 1;
  std::size_t passiveUnions = 0;
  for (const auto &owner : frontend.mir.classInstances()) {
    if (!owner.unionLayout)
      continue;
    ++passiveUnions;
    expect(lang::mirTypeMoveIsDefinedFailureFree(frontend.mir, owner.type),
           "verified passive union movement cannot raise defined failure");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(owner.id));
      if (mutation == 0)
        changed->moveConstructor = lang::SpecialMemberStatus::Deleted;
      if (mutation == 1)
        changed->requiresActiveDropState = true;
      if (mutation == 2)
        changed->destructorStatus = lang::SpecialMemberStatus::Declared;
      if (mutation == 3)
        changed->declaredFields.front().type = lang::SemanticType::Unknown;
      expect(!lang::mirTypeMoveIsDefinedFailureFree(malformed, owner.type),
             "passive union movement retains special-member and field proofs");
    }
  }
  expect(passiveUnions == 1, "fixture contains one independent passive union");
  std::size_t unionFieldSchedules = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            instruction.constructorTarget)
          continue;
        const auto found = std::find_if(
            frontend.mir.classInstances().begin(),
            frontend.mir.classInstances().end(), [&](const auto &owner) {
              return owner.unionLayout && owner.type == instruction.info.type;
            });
        if (found == frontend.mir.classInstances().end())
          continue;
        ++unionFieldSchedules;
      }
    }
  }
  expect(unionFieldSchedules == 1,
         "fixture exercises one generated passive-union field schedule");
  std::size_t arrays = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Aggregate ||
            instruction.info.type.kind != lang::SemanticType::Array)
          continue;
        ++arrays;
      }
    }
  }
  expect(arrays == 7,
         "fixture reaches both integer and all passive array shapes");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "typed passive array programs verify");
  expectNativeEmission(frontend, "passive arrays construct final storage "
                                 "without default staging");
  if (failures)
    return 1;
  std::cout << "passive fixed-array MIR proofs passed\n";
}
