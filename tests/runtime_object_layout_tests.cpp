#include "cpp_backend_test_support.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}
lang::FrontendResult analyze(const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  return lang::Frontend().analyze(root / "object-layout-test.gti", source,
                                  {root / "stdlib/prelude.gti"}, {},
                                  {root / "stdlib"});
}
void rejected(const std::string &source, std::string_view code,
              std::string_view name) {
  const auto result = analyze(source);
  const bool diagnostic =
      std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                  [&](const auto &item) { return item.code == code; });
  if (result.canGenerateCode() || !diagnostic)
    for (const auto &item : result.diagnostics)
      std::cerr << item.code << ": " << item.message << '\n';
  expect(!result.canGenerateCode() && diagnostic, name);
}
template <typename Mutation>
void forged(const lang::MirProgram &source, Mutation mutation,
            std::string_view name) {
  auto program = source;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           program.functionInstances())) {
    for (auto &block : function.body.blocks) {
      for (auto &instruction : block.instructions) {
        if (!changed &&
            instruction.intrinsic == lang::IntrinsicKind::ObjectSize) {
          mutation(instruction);
          changed = true;
        }
      }
    }
  }
  const auto verification = lang::verifyMirProgram(program);
  expect(changed &&
             std::any_of(verification.errors.begin(), verification.errors.end(),
                         [](const auto &error) {
                           return error.message.find(
                                      "runtime object layout query") !=
                                  std::string::npos;
                         }),
         name);
}
} // namespace

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  std::ifstream input(root / "examples/119-runtime-object-layout.gti");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyze(source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "owning, immovable, nested and generic queries compile");
  if (!frontend.canGenerateCode())
    return 1;
  expect(lang::verifyMirProgram(frontend.mir).valid(), "query MIR verifies");
  std::size_t sizes = 0, alignments = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.objectLayoutType) {
          sizes += instruction.intrinsic == lang::IntrinsicKind::ObjectSize;
          alignments +=
              instruction.intrinsic == lang::IntrinsicKind::ObjectAlignment;
          const auto effects = lang::effects(instruction);
          expect(
              effects.targetDependent && effects.removableWhenUnused &&
                  !effects.allocates && !effects.mayTrap &&
                  !effects.startsLoan && !effects.invokesUserCode &&
                  instruction.operands.empty(),
              "queries are pure target-dependent values without object inputs");
        }
  expect(sizes == 5 && alignments == 5,
         "each concrete query retains two typed operations");
  const auto serialized = lang::MirPrinter().print(frontend.mir);
  expect(serialized.starts_with("mir-v95 ") &&
             serialized.find("object-layout-type=") != std::string::npos,
         "serialization retains query identity");
  for (auto level : {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
                     lang::OptimizationLevel::O3}) {
    const auto cpp = gti_test::emitCppText(frontend, lang::CppStandard::Cpp23,
                                           lang::TargetInfo::host(), level);
    expect(cpp.find("// GTI verified-MIR body:") != std::string::npos &&
               cpp.find("sizeof(") != std::string::npos &&
               cpp.find("alignof(") != std::string::npos,
           "optimized detached MIR emits representation queries");
  }
  const auto formatted = lang::Formatter().format(source);
  expect(lang::Formatter().format(formatted) == formatted &&
             analyze(formatted).canGenerateCode(),
         "formatter preserves ordinary generic API");
  forged(
      frontend.mir, [](auto &i) { i.objectLayoutType.reset(); },
      "missing queried type");
  forged(
      frontend.mir,
      [](auto &i) { i.objectLayoutType = lang::SemanticType::Void; },
      "void queried type");
  forged(
      frontend.mir,
      [](auto &i) {
        i.objectLayoutType = lang::SemanticType::classType(999999);
      },
      "unavailable nominal representation");
  forged(
      frontend.mir,
      [](auto &i) {
        i.objectLayoutType =
            lang::SemanticType::referenceTo(lang::SemanticType::Int32);
      },
      "reference queried type");
  forged(
      frontend.mir,
      [](auto &i) {
        i.objectLayoutType = lang::SemanticType::typeParameter(999);
      },
      "unresolved queried type");
  forged(frontend.mir, [](auto &i) { i.loan = 1; }, "forged loan authority");
  forged(
      frontend.mir,
      [](auto &i) { i.info.traits.drop = lang::DropKind::Lexical; },
      "forged ownership result");
  forged(
      frontend.mir,
      [](auto &i) { i.parameterTypes.push_back(lang::SemanticType::Int32); },
      "forged runtime parameter");
  forged(
      frontend.mir, [](auto &i) { i.intrinsic = lang::IntrinsicKind::None; },
      "metadata cannot become an ordinary call");
  const std::string prefix = "#include <std/memory>\n";
  const auto otherShapes = analyze(prefix + R"gti(
enum class Code : uint16_t { ready, done };
union Bits { uint64_t word; double real; };
class View {
  int& value;
public:
  View(int& value) : value(value) {}
};
int main() {
  auto address = std::layout_of<void*>();
  auto code = std::layout_of<Code>();
  auto bits = std::layout_of<Bits>();
  auto view = std::layout_of<View>();
  return 0;
}
)gti");
  for (const auto &diagnostic : otherShapes.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(otherShapes.canGenerateCode(),
         "pointer, integral enum, union and borrowed-state class query");
  if (otherShapes.canGenerateCode())
    expect(!gti_test::emitCppText(otherShapes).empty(),
           "other representations emit from MIR");
  rejected(prefix + "int main() { auto x = std::layout_of<void>(); return 0; }",
           "GTI-S2029", "void source query");
  rejected(prefix + "int main() { auto x = std::layout_of<int&>(); return 0; }",
           "GTI-S2029", "reference source query");
  rejected(prefix +
               "int main() { auto x = std::layout_of<int[0]>(); return 0; }",
           "GTI-S2029", "zero array source query");
  rejected(prefix +
               "interface Abstract { int read(); };\n"
               "int main() { auto x = std::layout_of<Abstract>(); return 0; }",
           "GTI-S2029", "interface source query");
  rejected(prefix + "int main() { auto x = std::layout_of<expected<int, "
                    "int>>(); return 0; }",
           "GTI-S2029", "bounded direct result-carrier query");
  rejected(prefix + "struct Ordinary { int value; };\n"
                    "constexpr uint64_t bytes = sizeof(Ordinary); int main() { "
                    "return 0; }",
           "GTI-S2063", "runtime API does not widen compile-time sizeof");
  rejected(prefix + "constexpr std::layout descriptor = std::layout_of<int>(); "
                    "int main() { return 0; }",
           "GTI-S2057", "runtime query is not a constant expression");
  // Merely using the public spelling never selects the private capability.
  const auto ordinary =
      analyze("namespace std { int layout_of<T>() { return 17; } }\n"
              "int main() { return std::layout_of<int>() - 17; }");
  expect(ordinary.canGenerateCode(),
         "public spelling has ordinary source meaning");
  for (const auto &function : ordinary.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        expect(!instruction.objectLayoutType &&
                   instruction.intrinsic != lang::IntrinsicKind::ObjectSize &&
                   instruction.intrinsic !=
                       lang::IntrinsicKind::ObjectAlignment,
               "public spelling does not bind an intrinsic");
  rejected("@compiler_constraint(\"object_layout\") concept fake<T>;\n"
           "int main() { return 0; }",
           "GTI-S2049",
           "application cannot forge the compiler-owned eligibility concept");
  std::cout << "runtime object layout: " << (failures ? "FAILED" : "passed")
            << '\n';
  return failures ? 1 : 0;
}
