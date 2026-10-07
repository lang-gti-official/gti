#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool okay, const char *message) {
  if (!okay) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}
lang::FrontendResult analyze(const std::string &source, bool trusted = true) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "storage-adoption-test.gti";
  auto preludes =
      std::vector<std::filesystem::path>{root / "stdlib/prelude.gti"};
  if (trusted)
    preludes.push_back(entry);
  return lang::Frontend().analyze(
      entry, source, preludes,
      {{std::filesystem::weakly_canonical(entry).string(), source}},
      {root / "stdlib"});
}
void diagnostics(const lang::FrontendResult &result) {
  for (const auto &item : result.diagnostics)
    std::cerr << item.code << ": " << item.message << '\n';
}
template <typename Mutation>
void forged(const lang::FrontendResult &frontend, Mutation mutation,
            const char *message) {
  auto mir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           mir.functionInstances()))
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (!changed &&
            instruction.intrinsic == lang::IntrinsicKind::StorageTryAdopt) {
          mutation(instruction);
          changed = true;
        }
  expect(changed && !lang::verifyMirProgram(mir).valid(), message);
  bool rejected = false;
  try {
    (void)gti_test::emitCpp(frontend, frontend.mir, mir);
  } catch (const std::exception &) {
    rejected = true;
  }
  expect(changed && rejected, "detached backend rejects malformed adoption");
}
} // namespace

int main() {
  const std::string prefix = "#include <std/memory>\n";
  const std::string source = prefix + R"(
int main() {
  mut gti_internal::storage<int> slots = gti_internal::allocate_storage<int>(uint64_t(0));
  void* address = nullptr;
  unsafe {
    return gti_internal::storage_try_adopt(slots, address, uint64_t(0),
                                          uint64_t(0), uint64_t(64));
  }
})";
  const auto frontend = analyze(source);
  if (!frontend.canGenerateCode()) {
    diagnostics(frontend);
    std::cerr << lang::MirPrinter().print(frontend.mir);
  }
  expect(frontend.canGenerateCode(),
         "trusted unsafe adoption forms verified MIR");
  if (frontend.canGenerateCode()) {
    std::size_t count = 0;
    for (const auto &function : frontend.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.intrinsic == lang::IntrinsicKind::StorageTryAdopt) {
            ++count;
            const auto effects = lang::effects(instruction);
            expect(effects.allocates && effects.writesPlace &&
                       effects.writesUnknownMemory && !effects.mayTrap &&
                       !effects.removableWhenUnused &&
                       instruction.unsafeOperation ==
                           lang::UnsafeOperationKind::StorageAdoption,
                   "adoption keeps unsafe authority and fallible allocation "
                   "effects");
          }
    expect(count == 1, "exactly one adoption call");
    for (auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O3}) {
      const auto cpp = gti_test::emitCppText(frontend, lang::CppStandard::Cpp20,
                                             lang::TargetInfo::host(), level);
      expect(cpp.find("= ::gti_internal::backend::storage_try_adopt(") !=
                 std::string::npos,
             "verified adoption survives optimization and emits from MIR");
    }
    forged(
        frontend,
        [](auto &i) { i.unsafeOperation = lang::UnsafeOperationKind::None; },
        "missing unsafe adoption marker rejected");
    forged(
        frontend, [](auto &i) { i.operands.pop_back(); },
        "missing alignment operand rejected");
    forged(
        frontend,
        [](auto &i) {
          i.operands[1].type.pointerAccess = lang::AccessMode::ReadOnly;
          i.parameterTypes[1] = i.operands[1].type;
        },
        "readonly external region rejected");
    forged(
        frontend, [](auto &i) { i.info.type = lang::SemanticType::Void; },
        "discarded recoverable status contract rejected");
    forged(
        frontend, [](auto &i) { i.borrowArgument = 1; },
        "unrelated borrow metadata rejected");
    forged(
        frontend, [](auto &i) { i.neverReturns = true; },
        "terminal-call authority cannot be attached to adoption");
    forged(
        frontend, [](auto &i) { i.storagePreflight = i.id; },
        "constructor preflight authority cannot be attached to adoption");
    forged(
        frontend, [](auto &i) { i.info.category = lang::ValueCategory::Place; },
        "borrowed adoption status rejected");
  }
  auto withoutUnsafe = source;
  withoutUnsafe.replace(withoutUnsafe.find("unsafe {"), 8, "{");
  const auto missing = analyze(withoutUnsafe);
  expect(!missing.canGenerateCode() &&
             std::any_of(missing.diagnostics.begin(), missing.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-S2055"; }),
         "private adoption still requires an explicit unsafe block");
  const auto untrusted = analyze(source, false);
  expect(!untrusted.canGenerateCode(),
         "applications cannot name private adoption");
  const auto immovable = analyze(prefix + R"(
class Anchor {
  int value;
public:
  Anchor(int value) : value(value) {}
  Anchor(Anchor& other) = delete;
  Anchor(Anchor&& other) = delete;
  int read() { return this.value; }
};
int main() {
  std::allocator allocator{};
  auto attempt = allocator.try_storage<Anchor>(std::size_t(1));
  if (!attempt) { return 1; }
  mut auto slots = std::move(attempt).value();
  slots.emplace(std::size_t(0), 42);
  auto moved = std::move(slots);
  return moved.get(std::size_t(0)).read() - 42;
})",
                                 false);
  if (!immovable.canGenerateCode())
    diagnostics(immovable);
  expect(immovable.canGenerateCode(),
         "owner moves without moving fixed-address elements");
  if (immovable.canGenerateCode())
    expect(!gti_test::emitCppText(immovable).empty(),
           "immovable storage emits through MIR");
  const auto relocation = analyze(prefix + R"(
class Anchor {
public:
  Anchor() {}
  Anchor(Anchor& other) = delete;
  Anchor(Anchor&& other) = delete;
};
int main() {
  std::allocator allocator{};
  mut auto first_result = allocator.try_storage<Anchor>(std::size_t(1));
  mut auto second_result = allocator.try_storage<Anchor>(std::size_t(1));
  if (!first_result || !second_result) { return 1; }
  mut auto first = std::move(first_result).value();
  mut auto second = std::move(second_result).value();
  first.emplace(std::size_t(0));
  first.relocate_to(second, std::size_t(1));
  return 0;
})",
                                  false);
  expect(!relocation.canGenerateCode() &&
             std::any_of(relocation.diagnostics.begin(),
                         relocation.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-S2029"; }),
         "moving an immovable element remains rejected by the relocation "
         "contract");
  const auto borrowed = analyze(prefix + R"(
struct View {
  int& value;
  View(int& source) : value(source) {}
};
int main() {
  std::allocator allocator{};
  [[discard]] allocator.try_storage<View>(std::size_t(1));
  return 0;
})",
                                false);
  expect(!borrowed.canGenerateCode(),
         "fixed-address storage does not admit untracked borrowed elements");
  return failures ? 1 : 0;
}
