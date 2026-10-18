#include "../src/compiler/cpp_representation.h"
#include "cpp_mir_body_emitter_test_fixture.h"

namespace lang {
struct LoweredProgramTestAccess {
  static auto &declarations(LoweredProgram &program) {
    return program.declarations_;
  }
};
} // namespace lang

int main() {
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
  const auto text = lang::CppBackend().generate(lowered).contents;
  expect(text.find("__gti_enum_selector_") != std::string::npos &&
             text.find("void state;") == std::string::npos,
         "dependent fields select concrete enums without void placeholders");
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
      bool rejected = false;
      try {
        static_cast<void>(lang::CppBackend().generate(malformed));
      } catch (const std::logic_error &) {
        rejected = true;
      }
      expect(
          rejected,
          "native emission independently rejects malformed selector metadata");
    }
    auto missing = enumeration->type;
    missing.valueArguments.push_back(lang::CompileTimeValue::uint64(12345));
    bool rejected = false;
    try {
      static_cast<void>(lang::cppSemanticTypeSpelling(
          lowered, lang::CppStandard::Cpp23, missing));
    } catch (const std::logic_error &) {
      rejected = true;
    }
    expect(
        rejected,
        "a missing concrete enum cannot acquire a dependent selector fallback");
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
    const auto exactText = gti_test::emitCppText(exact);
    expect(
        exactText.find("__gti_enum_selector_") == std::string::npos,
        "exact-only native aliases never demand a dependent selector catalog");
  }
  return failures ? 1 : 0;
}
