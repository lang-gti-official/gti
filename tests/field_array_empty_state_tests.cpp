#include "llvm_backend_test_support.h"
#include "mir_test_fixture.h"

using namespace gti_test::mir_fixture;

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/issue_144_class_array_field.gti";
  std::ifstream stream(path);
  const std::string source{std::istreambuf_iterator<char>(stream), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "class array fields have valid MIR");
  if (!frontend.canGenerateCode())
    return 1;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "class array fields produce verified MIR");
  std::size_t arrays = 0;
  std::size_t interfaces = 0;
  std::size_t fieldElements = 0;
  for (const auto &owner : frontend.mir.classInstances()) {
    if (owner.declaredFields.size() != 6)
      continue;
    for (const auto &field : owner.declaredFields) {
      expect(field.type.kind == lang::SemanticType::Array &&
                 field.type.arrayLength != 0 &&
                 field.type.arguments.size() == 1,
             "each fixed array field has an exact concrete nonzero extent "
             "and element");
      ++arrays;
      if (field.type.arguments.empty())
        continue;
      const auto &elementType = field.type.arguments.front();
      if (elementType.kind == lang::SemanticType::Class)
        fieldElements += field.type.arrayLength;
      const auto element = std::find_if(
          frontend.mir.classInstances().begin(),
          frontend.mir.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == elementType; });
      if (element == frontend.mir.classInstances().end() ||
          !element->polymorphic)
        continue;
      ++interfaces;
      expect(!element->abstract && !element->bases.empty() &&
                 element->bases.front().interface,
             "polymorphic array elements derive from an interface base");
    }
  }
  expect(arrays == 6 && interfaces == 2,
         "fixture exercises scalar and five class arrays including both "
         "interface-only cleanup forms");
  expect(fieldElements != 0, "fixture stores fresh class elements in fields");
  std::size_t publications = 0;
  for (const auto &constructor : frontend.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.info.type.kind != lang::SemanticType::Array ||
            instruction.info.type.arguments.front().kind !=
                lang::SemanticType::Class)
          continue;
        ++publications;
        expect(!instruction.lifecycle.empty() &&
                   instruction.constructorInitializer != 0,
               "field array publication carries its transfer and "
               "initializer identity");
        // MIR verification does not bind a cleanup-free element array's
        // publication to its constructor-initializer stage, so the stale
        // initializer forgery stays in cpp_mir_field_array_empty_state.
        for (const int mutation : {0, 2}) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          for (auto &candidateBlock : changed->body.blocks)
            for (auto &candidate : candidateBlock.instructions) {
              if (candidate.id != instruction.id)
                continue;
              if (mutation == 0)
                candidate.lifecycle.clear();
              else
                candidate.lifecycle.front().target =
                    candidate.lifecycle.front().source;
            }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "field array publication rejects missing transfer and wrong "
                 "rollback identity");
        }
      }
  expect(publications == 5,
         "all five class arrays retain exact MIR publication identities");
  std::string error;
  expect(gti_test::llvmEmits(frontend, lang::OptimizationLevel::O0,
                             lang::TargetInfo::host(), &error),
         "class array aggregates reach native emission: " + error);
  if (failures)
    return 1;
  std::cout << "field array empty-state proof tests passed\n";
}
