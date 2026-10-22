#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/optimizer.h"

#include "lowered_program_contract_client.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

constexpr std::array generatedKinds{
    std::pair{lang::LoweredGeneratedItemKind::ProgramInitialization,
              "program_initialization"},
    std::pair{lang::LoweredGeneratedItemKind::HostedEntry, "hosted_entry"},
    std::pair{lang::LoweredGeneratedItemKind::StructuralOperatorAdapter,
              "structural_operator_adapter"},
    std::pair{lang::LoweredGeneratedItemKind::CallableAdapter,
              "callable_adapter"},
    std::pair{lang::LoweredGeneratedItemKind::LifecycleCleanup,
              "lifecycle_cleanup"},
    std::pair{lang::LoweredGeneratedItemKind::NativeInteropAdapter,
              "native_interop_adapter"},
    std::pair{lang::LoweredGeneratedItemKind::ThreadTaskAdapter,
              "thread_task_adapter"},
    std::pair{lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter,
              "concrete_instance_adapter"},
};
static_assert([] {
  if (generatedKinds.size() !=
      static_cast<std::size_t>(lang::LoweredGeneratedItemKind::Count))
    return false;
  for (std::size_t index = 0; index < generatedKinds.size(); ++index)
    if (static_cast<std::size_t>(generatedKinds[index].first) != index)
      return false;
  return true;
}());

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
  // Bind the textual column names to the compiled enum identities instead of
  // silently assuming that the Python reader has the same ordinal order.
  std::cout << "generated_kinds";
  for (const auto &[kind, name] : generatedKinds)
    std::cout << '\t' << name;
  std::cout << '\n';
  for (int index = 2; index < argc; ++index) {
    const std::string name =
        std::filesystem::path(argv[index]).filename().string();
    const std::filesystem::path source = root / "examples" / name;
    lang::TargetInfo target = lang::TargetInfo::host();
    if (name == "066-managed-threads.gti" || name == "076-scalar-atomics.gti" ||
        name == "171-thread-local-storage.gti") {
      target.executionProfile = lang::ExecutionProfile::Concurrent;
      target.runtimeCapabilities.threads = true;
    }
    lang::FrontendOptions options;
    options.target = target;
    // The census guards the inventory that artifact builds produce.
    options.lowerReachableOnly = true;
    const lang::FrontendResult frontend =
        lang::Frontend(std::move(options))
            .analyze(source, std::nullopt, {standardLibrary / "prelude.gti"},
                     {}, {standardLibrary});
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
        optimized.mir, optimized.report.transformations, target);
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
    for (const auto &[kind, name] : generatedKinds) {
      std::cout << '\t'
                << inventory.generatedItemKinds[static_cast<std::size_t>(kind)];
    }
    std::cout << '\n';
  }
  return 0;
}
