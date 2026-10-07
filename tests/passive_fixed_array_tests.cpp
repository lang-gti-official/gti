#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/passive_fixed_array_values.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "typed passive arrays have verified MIR");
  if (!frontend.canGenerateCode())
    return 1;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
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
  std::size_t arrays = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Aggregate ||
            instruction.info.type.kind != lang::SemanticType::Array)
          continue;
        ++arrays;
        if (instruction.info.type.arguments.front().kind !=
            lang::SemanticType::Class)
          continue;
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &candidateBlock : changed->body.blocks)
            for (auto &candidate : candidateBlock.instructions) {
              if (candidate.id != instruction.id)
                continue;
              if (mutation == 0)
                candidate.info.type.arrayLength += 1;
              else
                candidate.operands.front().type = lang::SemanticType::Int32;
            }
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "passive aggregate rejects forged element count/type");
        }
      }
    }
  }
  expect(arrays == 7,
         "fixture reaches both integer and all passive array shapes");
  try {
    const auto cpp = gti_test::emitCppText(frontend);
    expect(cpp.find(".construction_address())) std::array<") !=
               std::string::npos,
           "passive arrays construct final storage without default staging");
  } catch (const std::exception &error) {
    expect(false, error.what());
  }
  if (failures)
    return 1;
  std::cout << "passive fixed-array MIR and emitter proofs passed\n";
}
