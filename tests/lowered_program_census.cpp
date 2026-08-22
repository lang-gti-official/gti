#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/optimizer.h"

#include "lowered_program_contract_client.h"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

void printDiagnostics(const lang::FrontendResult &frontend) {
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: gti_lowered_program_census <repo-root> <example>...\n";
    return 2;
  }

  const std::filesystem::path root = std::filesystem::weakly_canonical(argv[1]);
  const std::filesystem::path standardLibrary = root / "stdlib";
  const lang::TargetInfo target = lang::TargetInfo::host();
  for (int index = 2; index < argc; ++index) {
    const std::string name =
        std::filesystem::path(argv[index]).filename().string();
    const std::filesystem::path source = root / "examples" / name;
    const lang::FrontendResult frontend = lang::Frontend().analyze(
        source, std::nullopt, {standardLibrary / "prelude.gti"}, {},
        {standardLibrary});
    if (!frontend.canGenerateCode()) {
      std::cerr << name << ": frontend rejected corpus source\n";
      printDiagnostics(frontend);
      return 1;
    }

    const lang::OptimizedProgram optimized =
        lang::OptimizationPipeline().run({.hir = frontend.hir,
                                          .mir = frontend.mir,
                                          .level = lang::OptimizationLevel::O1,
                                          .target = target});
    if (!optimized.valid()) {
      std::cerr << name << ": optimizer rejected corpus source\n";
      return 1;
    }
    lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
        optimized.mir, target);
    if (!lowered.valid()) {
      std::cerr << name << ": lowering rejected corpus source\n";
      for (const lang::LoweredProgramIssue &issue : lowered.issues) {
        std::cerr << "lowered: " << issue.detail << '\n';
      }
      return 1;
    }

    const gti_test::LoweredProgramInventory inventory =
        gti_test::inspectLoweredProgram(*lowered.program);
    std::cout << name << '\t' << inventory.declarations << '\t'
              << inventory.bodies << '\t' << inventory.generatedItems;
    for (const std::size_t count : inventory.generatedItemKinds) {
      std::cout << '\t' << count;
    }
    std::cout << '\n';
  }
  return 0;
}
