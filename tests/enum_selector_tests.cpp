#include "gti/lowered_program.h"
#include "llvm_backend_test_support.h"
#include "lowered_program_test_support.h"
#include "mir_test_fixture.h"

using namespace gti_test::mir_fixture;

namespace lang {
struct LoweredProgramTestAccess {
  static auto &declarations(LoweredProgram &program) {
    return program.declarations_;
  }
};
} // namespace lang

int main() {
  // Forged lowered programs below also exercise the construction seal.
  lang::setLoweredProgramSealValidation(true);
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/enum_selector_composition.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "enum selector composition has valid GTI semantics");
  if (!frontend.canGenerateCode())
    return 1;
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "enum selector composition produces a verified lowered program");
  std::string error;
  expect(gti_test::llvmEmits(frontend, lang::OptimizationLevel::O0,
                             lang::TargetInfo::host(), &error),
         "dependent fields select concrete enums natively: " + error);
  std::size_t checked = 0;
  for (const auto &row : lowered.declarations()) {
    const auto *enumeration =
        std::get_if<lang::LoweredEnumDeclaration>(&row.payload);
    if (!enumeration || enumeration->genericParameters.empty())
      continue;
    ++checked;
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto malformed = lowered;
      auto &changed = std::get<lang::LoweredEnumDeclaration>(
          lang::LoweredProgramTestAccess::declarations(malformed)[row.id - 1]
              .payload);
      if (mutation == 0)
        changed.genericParameters.front().id = 0;
      else if (mutation == 1)
        changed.genericParameters.front().value =
            !changed.genericParameters.front().value;
      else
        changed.genericParameters.front().name.clear();
      expect(!lang::verifyLoweredProgram(malformed).empty(),
             "enum selector rejects missing binder, wrong argument kind and "
             "missing name");
    }
  }
  expect(checked >= 8, "selector proof covers distinct families and "
                       "type/value/pack applications");
  const auto exactPath = path.parent_path() / "enum_selector_native_exact.gti";
  std::ifstream exactInput(exactPath);
  const std::string exactSource{std::istreambuf_iterator<char>(exactInput), {}};
  const auto exact =
      analyzeWithStandardLibrary(exactPath.string(), exactSource);
  expect(exact.canGenerateCode(), "exact native aliases remain valid GTI");
  if (exact.canGenerateCode()) {
    expect(lang::verifyMirProgram(exact.mir).valid(),
           "exact native aliases produce verified MIR");
    std::size_t fixtureEnums = 0;
    for (const auto &enumeration : exact.hir.enumDeclarations())
      if (enumeration.sourceUnit == exact.sourceGraph.entryUnit())
        ++fixtureEnums;
    expect(fixtureEnums != 0, "exact alias fixture declares its own enum");
    std::string exactError;
    expect(gti_test::llvmEmits(exact, lang::OptimizationLevel::O0,
                               lang::TargetInfo::host(), &exactError),
           "exact-only native aliases reach native emission: " + exactError);
  }
  return failures ? 1 : 0;
}
