#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "uninitialized-carrier-test.gti";
  return lang::Frontend().analyze(
      entry, source, {root / "stdlib/prelude.gti"},
      {{std::filesystem::weakly_canonical(entry).string(), source}},
      {root / "stdlib"});
}
} // namespace

int main() {
  const std::string prefix = R"(
#include <std/memory>
#include <std/string>
struct Plain { int value = 8; };
class Stationary {
public:
  Stationary(int value) {}
  Stationary(Stationary& other) = delete;
  Stationary(Stationary&& other) = delete;
};
class Holder<std::layout_queryable T> {
  mut std::uninitialized<T> bytes{};
public:
  Holder() {}
};
)";
  const auto valid = analyze(prefix + R"(
int main() {
  Holder<Stationary> holder{};
  std::uninitialized<Stationary> vacant{};
  std::uninitialized<Plain> first{};
  auto second = first;
  auto third = std::move(second);
  return 0;
}
)");
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(valid.canGenerateCode(),
         "carrier admits empty immovable and bitwise copy/move");
  if (valid.canGenerateCode()) {
    try {
      for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
        (void)gti_test::emitCppText(valid, standard);
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      expect(false, "carrier copy/move has complete MIR emission");
    }
    for (int mutation = 0; mutation < 5; ++mutation) {
      auto mir = valid.mir;
      bool changed = false;
      for (auto &owner : const_cast<std::vector<lang::MirClassInstance> &>(
               mir.classInstances())) {
        if (owner.compilerCapability !=
            lang::CompilerCapabilityTypeKind::UninitializedStorage)
          continue;
        switch (mutation) {
        case 0:
          owner.traits.movable = !owner.traits.movable;
          break;
        case 1:
          owner.traits.copyable = !owner.traits.copyable;
          break;
        case 2:
          owner.requiresActiveCleanup = true;
          break;
        case 3:
          owner.traits.transferCapable = !owner.traits.transferCapable;
          break;
        case 4:
          owner.traits.shareCapable = !owner.traits.shareCapable;
          break;
        }
        changed = true;
        break;
      }
      expect(changed && !lang::verifyMirProgram(mir).valid(),
             "forged carrier lifetime/capability facts rejected");
    }
  }
  for (const auto *body :
       {"int main() { Holder<Stationary> a{}; auto b = std::move(a); "
        "return 0; }",
        "int main() { Holder<std::string> a{}; auto b = std::move(a); "
        "return 0; }",
        "int main() { std::uninitialized<Stationary> a{}; auto b = "
        "std::move(a); return 0; }",
        "int main() { std::uninitialized<Stationary> a{}; auto b = a; return "
        "0; }",
        "int main() { std::uninitialized<std::string> a{}; auto b = "
        "std::move(a); return 0; }",
        "int main() { std::uninitialized<int> a{}; int* p = a.data(); return "
        "0; }",
        "int main() { mut std::uninitialized<int> a{}; "
        "std::construct_at(a.data(), 1); return 0; }",
        "int main() { std::uninitialized<void> a{}; return 0; }"}) {
    expect(!analyze(prefix + body).canGenerateCode(),
           "invalid carrier movement, access or construction rejected");
  }
  const auto relocation = analyze(prefix + R"(
int main() {
  Plain* destination = nullptr;
  Plain* source = nullptr;
  unsafe { std::relocate_at(destination, source); }
  return 0;
}
)");
  // Static-only: the two null addresses deliberately do not assert runtime
  // validity. Separate executable fixtures supply actual storage.
  expect(relocation.canGenerateCode(),
         "typed raw relocation forms verified MIR");
  if (relocation.canGenerateCode()) {
    for (int mutation = 0; mutation < 4; ++mutation) {
      auto mir = relocation.mir;
      bool changed = false;
      for (auto &function :
           const_cast<std::vector<lang::MirFunctionInstance> &>(
               mir.functionInstances()))
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            if (changed ||
                instruction.intrinsic != lang::IntrinsicKind::ObjectRelocate)
              continue;
            const auto effects = lang::effects(instruction);
            expect(effects.readsUnknownMemory && effects.writesUnknownMemory &&
                       effects.movesValue && effects.initializesValue &&
                       !effects.mayTrap && !effects.allocates &&
                       !effects.invokesUserCode,
                   "raw relocation retains its no-failure ownership barrier");
            switch (mutation) {
            case 0:
              instruction.unsafeOperation = lang::UnsafeOperationKind::None;
              break;
            case 1:
              instruction.parameterTypes[1].pointerAccess =
                  lang::AccessMode::ReadOnly;
              break;
            case 2:
              instruction.definedFailure.propagation =
                  lang::FailurePropagationKind::Destructor;
              break;
            case 3:
              instruction.storagePreflight = instruction.id;
              break;
            }
            changed = true;
          }
      expect(changed && !lang::verifyMirProgram(mir).valid(),
             "forged relocation rejected");
      bool rejected = false;
      try {
        (void)gti_test::emitCpp(relocation, relocation.mir, mir);
      } catch (const std::exception &) {
        rejected = true;
      }
      expect(rejected, "backend rejects forged raw relocation");
    }
  }
  for (const auto *body :
       {"int main() { Stationary* a = nullptr; Stationary* b = nullptr; unsafe "
        "{ std::relocate_at(a, b); } return 0; }",
        "int main() { int* a = nullptr; const int* b = nullptr; unsafe { "
        "std::relocate_at(a, b); } return 0; }",
        "int main() { int* a = nullptr; int* b = nullptr; std::relocate_at(a, "
        "b); return 0; }"})
    expect(!analyze(prefix + body).canGenerateCode(),
           "invalid raw relocation rejected");
  return failures == 0 ? 0 : 1;
}
