#include "llvm_backend_test_support.h"
#include "mir_test_fixture.h"

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/private_constructor_placement.gti";
  std::ifstream input(path);
  expect(input.good(), "private constructor fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "same-class private factories retain valid MIR");
  if (!frontend.canGenerateCode())
    return 1;
  std::size_t factories = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source ||
        (hir->source->name().lexeme != "direct" &&
         hir->source->name().lexeme != "named"))
      continue;
    ++factories;
    // The member's own private, failure-capable constructor is invoked from
    // the factory body and published only on its Invoke success edge. The C++
    // lexical-access spelling lives in
    // cpp_mir_private_constructor_placement_tests.cpp.
    std::size_t constructors = 0;
    bool routed = true;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!instruction.constructorTarget)
          continue;
        const auto *constructor = frontend.mir.findConstructorInstance(
            *instruction.constructorTarget);
        const auto &classes = frontend.hir.classInstances();
        const auto owner =
            constructor == nullptr
                ? classes.end()
                : std::find_if(classes.begin(), classes.end(),
                               [&](const auto &candidate) {
                                 return candidate.id == constructor->owner;
                               });
        if (owner == classes.end() || !owner->source ||
            owner->source->name().lexeme != "Secret")
          continue;
        ++constructors;
        routed &= constructor->mayRaiseDefinedFailure &&
                  block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                  block.terminator.invokeInstruction == instruction.id;
      }
    expect(constructors != 0 && routed,
           "failure-capable private construction executes in the member's "
           "own body through its exact Invoke");
  }
  std::string error;
  expect(gti_test::llvmEmits(frontend, lang::OptimizationLevel::O0,
                             lang::TargetInfo::host(), &error),
         "same-class private factories reach native emission");
  if (!error.empty())
    std::cerr << error << '\n';
  expect(factories == 2,
         "fixture covers result storage and named-local storage");
  std::string invalid = source;
  const std::string permitted = "Secret::direct(argc == 2 ? 0 : 1)";
  const auto offset = invalid.find(permitted);
  expect(offset != std::string::npos,
         "fixture retains the external factory call");
  if (offset != std::string::npos) {
    invalid.replace(offset, permitted.size(),
                    "Secret(1, std::string(\"blocked\"))");
    const auto denied =
        analyzeWithStandardLibrary("private-constructor-denied.gti", invalid);
    expect(!denied.canGenerateCode() &&
               std::any_of(denied.diagnostics.begin(), denied.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2007" &&
                                    diagnostic.message.find(
                                        "Constructor of 'Secret' is private") !=
                                        std::string::npos;
                           }),
           "external private construction remains rejected by semantics");
  }
  return failures ? 1 : 0;
}
