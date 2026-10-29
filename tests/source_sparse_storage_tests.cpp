#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "private_stdlib_fixture.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
int failures = 0;
void expect(bool valid, const char *message) {
  if (!valid) {
    std::cerr << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(const std::string &source) {
  return gti_test::analyzeTrustedPrivateMemoryFixture("source_sparse_slots.gti",
                                                      source);
}
} // namespace

int main(int argc, char **argv) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  std::ifstream input(root / "tests/fixtures/source_sparse_slots.gti");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyze(source);
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(), "source sparse policy forms verified MIR");
  if (!frontend.canGenerateCode())
    return 1;
  if (argc == 4) {
    const auto standard = std::string(argv[2]) == "c++20"
                              ? lang::CppStandard::Cpp20
                              : lang::CppStandard::Cpp23;
    const auto level =
        std::string(argv[3]) == "O0"   ? lang::OptimizationLevel::O0
        : std::string(argv[3]) == "O1" ? lang::OptimizationLevel::O1
                                       : lang::OptimizationLevel::O3;
    std::ofstream output(argv[1]);
    output << gti_test::emitCppText(frontend, standard,
                                    lang::TargetInfo::host(), level);
    return output ? 0 : 1;
  }
  for (const auto intrinsic :
       {lang::IntrinsicKind::RequireVacantStorage,
        lang::IntrinsicKind::RequireLiveStorage,
        lang::IntrinsicKind::RequireRelocationCapacity,
        lang::IntrinsicKind::RequireRelocationSource,
        lang::IntrinsicKind::RequireRelocationDestination}) {
    auto forged = frontend.mir;
    bool found = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             forged.functionInstances())) {
      for (auto &block : function.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.intrinsic != intrinsic)
            continue;
          const auto effects = lang::effects(instruction);
          expect(effects.mayTrap && !effects.allocates &&
                     !effects.writesUnknownMemory && !effects.movesValue,
                 "condition checks preserve failure without storage effects");
          instruction.definedFailure.localOrigins.front().outcomes.front() = {
              lang::DefinedFailureCode::KeyNotFound,
              lang::DefinedFailureDetail::KeyLookup};
          found = true;
        }
      }
    }
    expect(found && !lang::verifyMirProgram(forged).valid(),
           "forged storage failure family rejected");
  }
  for (const char *operation : {"owner.destroy(uint64_t(0));", "owner.clear();",
                                "auto moved = std::move(owner);"}) {
    const auto invalid =
        analyze(std::string("import std.memory;\nint main() { "
                            "mut gti_internal::sparse_slots<int> owner{}; "
                            "int& loan = owner.read(uint64_t(0)); ") +
                operation + "return loan; }");
    expect(!invalid.canGenerateCode() &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2017" &&
                                    diagnostic.message.find("borrow") !=
                                        std::string::npos &&
                                    diagnostic.message.find("live") !=
                                        std::string::npos;
                           }),
           "live element loan blocks invalidation");
  }
  return failures == 0 ? 0 : 1;
}
