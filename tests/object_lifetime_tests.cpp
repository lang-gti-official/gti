#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view label) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << label << '\n';
  }
}
lang::FrontendResult analyze(const std::string &source, bool trusted = false) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "object-lifetime-test.gti";
  std::vector<std::filesystem::path> preludes{root / "stdlib/prelude.gti"};
  if (trusted)
    preludes.push_back(entry);
  return lang::Frontend().analyze(
      entry, source, preludes,
      {{std::filesystem::weakly_canonical(entry).string(), source}},
      {root / "stdlib"});
}
void diagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
template <typename Mutation>
void forged(const lang::FrontendResult &frontend, Mutation mutation,
            std::string_view label) {
  auto mir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           mir.functionInstances()))
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (!changed &&
            instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy &&
            instruction.elementCleanup) {
          mutation(instruction);
          changed = true;
        }
  expect(changed && !lang::verifyMirProgram(mir).valid(), label);
  bool rejected = false;
  try {
    (void)gti_test::emitCpp(frontend, frontend.mir, mir);
  } catch (const std::exception &) {
    rejected = true;
  }
  expect(changed && rejected, "backend rejects forged lifetime operation");
}
} // namespace

int main() {
  // This is a static/MIR fixture, not an executable program: raw validity is
  // the unsafe caller's obligation, independently of pointer type checking.
  const auto frontend = analyze(R"(
#include <std/memory>
#include <std/panic>
class Sensor {
public:
  Sensor(Sensor& other) = delete;
  Sensor(Sensor&& other) = delete;
  ~Sensor() { std::panic("sensor cleanup failed"); }
};
int main() {
  int* scalar = nullptr;
  Sensor* sensor = nullptr;
  unsafe {
    std::destroy_at(scalar);
    std::destroy_at(sensor);
  }
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    diagnostics(frontend);
    std::cerr << lang::MirPrinter().print(frontend.mir);
  }
  expect(frontend.canGenerateCode(), "raw destruction forms verified MIR");
  if (!frontend.canGenerateCode())
    return 1;
  std::size_t count = 0, cleanups = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy) {
          ++count;
          const auto effects = lang::effects(instruction);
          expect(effects.writesUnknownMemory && effects.dropsValue &&
                     !effects.removableWhenUnused && !effects.allocates,
                 "raw destruction retains memory/cleanup effects without "
                 "allocation");
          expect(
              instruction.unsafeOperation ==
                      lang::UnsafeOperationKind::ObjectLifetime &&
                  instruction.localFailureSites.empty(),
              "raw validity has unsafe authority, not invented runtime checks");
          if (instruction.elementCleanup) {
            ++cleanups;
            expect(instruction.definedFailure.propagation ==
                       lang::FailurePropagationKind::Destructor,
                   "user cleanup retains original failure propagation");
          }
        }
  expect(count == 2 && cleanups == 1,
         "scalar and unrelated immovable cleanup paths");
  for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
    for (auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
      const auto cpp = gti_test::emitCppText(frontend, standard,
                                             lang::TargetInfo::host(), level);
      expect(cpp.find("= ::gti_internal::backend::mir_object_destroy_v1(") !=
                 std::string::npos,
             "verified cleanup survives optimization and uses failure-aware "
             "lowering");
    }
  forged(
      frontend,
      [](auto &i) { i.unsafeOperation = lang::UnsafeOperationKind::None; },
      "missing unsafe lifetime authority rejected");
  forged(
      frontend, [](auto &i) { i.elementCleanup.reset(); },
      "missing exact cleanup rejected");
  forged(
      frontend, [](auto &i) { i.definedFailure = {}; },
      "erased destructor failure rejected");
  forged(
      frontend,
      [](auto &i) {
        i.operands.front().type.pointerAccess = lang::AccessMode::ReadOnly;
        i.parameterTypes.front() = i.operands.front().type;
      },
      "readonly destruction rejected");
  forged(
      frontend, [](auto &i) { i.borrowArgument = 1; },
      "unrelated borrowed authority rejected");
  forged(
      frontend, [](auto &i) { i.storagePreflight = i.id; },
      "container preflight authority rejected");
  forged(
      frontend, [](auto &i) { i.neverReturns = true; },
      "unrelated terminal authority rejected");
  forged(
      frontend, [](auto &i) { i.info.category = lang::ValueCategory::Place; },
      "destruction cannot publish a borrowed result");
  const std::string unsafeSource =
      "#include <std/memory>\nint main() { int* p = nullptr; "
      "std::destroy_at(p); return 0; }";
  const auto missingUnsafe = analyze(unsafeSource);
  const auto close =
      unsafeSource.find(")", unsafeSource.find("std::destroy_at"));
  expect(!missingUnsafe.canGenerateCode() &&
             std::any_of(missingUnsafe.diagnostics.begin(),
                         missingUnsafe.diagnostics.end(),
                         [&](const auto &d) {
                           return d.code == "GTI-S2055" &&
                                  d.phase == lang::DiagnosticPhase::Semantics &&
                                  d.primary.source.ends_with(
                                      "object-lifetime-test.gti") &&
                                  d.primary.start == close &&
                                  d.primary.end == close + 1 &&
                                  d.message.find("unsafe") !=
                                      std::string::npos &&
                                  d.fixes.empty();
                         }),
         "public destruction requires explicit unsafe permission, with no "
         "unsafe fix-it");
  for (const std::string pointer : {"const int*", "void*"}) {
    const auto invalid =
        analyze("#include <std/memory>\nint main() { " + pointer +
                    " p = nullptr; unsafe { gti_internal::object_destroy(p); } "
                    "return 0; }",
                true);
    expect(!invalid.canGenerateCode() &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const auto &d) {
                             return d.code == "GTI-S2019" &&
                                    d.message.find("writable pointer") !=
                                        std::string::npos;
                           }),
           "private destruction rejects readonly and incomplete pointees");
  }
  return failures == 0 ? 0 : 1;
}
